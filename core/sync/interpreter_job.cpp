/**
 * Copyright Soramitsu Co., Ltd. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "interpreter_job.hpp"

#include "common/logger.hpp"
#include "vm/interpreter/impl/interpreter_impl.hpp"

namespace fc::sync {

  namespace {
    auto log() {
      static common::Logger logger = common::createLogger("interpreter");
      return logger.get();
    }
  }  // namespace

  InterpreterJob::InterpreterJob(
      std::shared_ptr<storage::PersistentBufferMap> kv_store,
      libp2p::protocol::Scheduler &scheduler,
      ChainDb &chain_db,
      IpfsStoragePtr ipld,
      Callback callback)
      : kv_store_(kv_store),
        interpreter_(std::make_shared<vm::interpreter::CachedInterpreter>(
            std::make_shared<vm::interpreter::InterpreterImpl>(),
            std::move(kv_store))),
        scheduler_(scheduler),
        chain_db_(chain_db),
        ipld_(std::move(ipld)),
        callback_(std::move(callback)),
        result_{nullptr, vm::interpreter::InterpreterError::kTipsetMarkedBad} {
    assert(callback_);
  }

  outcome::result<void> InterpreterJob::start(const TipsetKey &head) {
    if (active_) {
      log()->warn("current job ({} -> {}) is still active, cancelling it",
                  status_.current_height,
                  status_.target_height);
      std::ignore = cancel();
    }

    OUTCOME_TRYA(result_.head, chain_db_.getTipsetByKey(head));
    status_.target_height = result_.head->height();

    const auto &hash = result_.head->key.hash();

    // maybe already interpreted
    OUTCOME_TRY(maybe_result,
                vm::interpreter::getSavedResult(*kv_store_, result_.head));
    if (maybe_result) {
      result_.result = std::move(maybe_result.value());
      status_.current_height = status_.target_height;
      scheduleResult();
      return outcome::success();
    }

    // set current head to enable moving forward
    OUTCOME_TRY(chain_db_.setCurrentHead(result_.head->key.hash()));

    std::error_code e;

    // find the highest interpreted tipset in chain
    OUTCOME_TRY(chain_db_.walkBackward(hash, 0, [this, &e](TipsetCPtr tipset) {
      if (e) {
        return false;
      }
      auto res = vm::interpreter::getSavedResult(*kv_store_, tipset);
      if (!res) {
        e = res.error();
      } else {
        if (res.value()) {
          status_.current_height = tipset->height();
          return false;
        }
      }
      return (!e);
    }));

    if (e) {
      return e;
    }

    log()->info(
        "starting {} -> {}", status_.current_height, status_.target_height);
    active_ = true;

    scheduleStep();
    return outcome::success();
  }

  const InterpreterJob::Status &InterpreterJob::cancel() {
    active_ = false;
    cb_handle_.cancel();
    return status_;
  }

  const InterpreterJob::Status &InterpreterJob::getStatus() const {
    return status_;
  }

  void InterpreterJob::scheduleResult() {
    active_ = false;
    next_steps_.clear();
    step_cursor_ = 0;
    cb_handle_ = scheduler_.schedule([wptr{weak_from_this()}] {
      auto self = wptr.lock();
      if (self) {
        self->callback_(self->result_);
      }
    });
  }

  void InterpreterJob::scheduleStep() {
    if (!active_) {
      return;
    }
    cb_handle_ = scheduler_.schedule([wptr{weak_from_this()}] {
      auto self = wptr.lock();
      if (self) {
        self->nextStep();
      }
    });
  }

  void InterpreterJob::nextStep() {
    if (!active_) {
      return;
    }
    fillNextSteps();
    if (next_steps_.empty()) {
      scheduleResult();
      return;
    }

    assert(step_cursor_ < next_steps_.size());
    const auto &tipset = next_steps_[step_cursor_];
    ++step_cursor_;

    status_.current_height = tipset->height();
    log()->info("syncing {}/{}", status_.current_height, status_.target_height);

    result_.result = interpreter_->interpret(ipld_, tipset);
    if (!result_.result) {
      log()->error("syncing stopped at height {}: {}",
                   status_.current_height,
                   result_.result.error().message());
      active_ = false;
      scheduleResult();
      return;
    }

    scheduleStep();
  }

  void InterpreterJob::fillNextSteps() {
    if (step_cursor_ < next_steps_.size()) {
      return;
    }
    next_steps_.clear();
    step_cursor_ = 0;

    assert(active_);
    assert(status_.target_height >= status_.current_height);

    size_t diff = status_.target_height - status_.current_height;
    if (diff == 0) {
      return;
    }

    static constexpr size_t kQueryLimit = 100;
    if (diff > kQueryLimit) {
      diff = kQueryLimit;
    }

    auto res =
        chain_db_.walkForward(status_.current_height + 1,
                              status_.current_height + diff - 1,
                              [this](TipsetCPtr tipset) {
                                if (tipset->height() <= status_.target_height) {
                                  next_steps_.push_back(tipset);
                                }
                                return true;
                              });
    if (!res) {
      log()->error("failed to load {} tipsets starting from height {}",
                   diff,
                   status_.current_height + 1);
      result_.result = res.error();
      next_steps_.clear();
    } else {
      log()->debug("scheduled {} tipsets starting from height {}",
                   next_steps_.size(),
                   status_.current_height + 1);
    }
  }

}  // namespace fc::sync
