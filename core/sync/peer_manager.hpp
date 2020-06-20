/**
 * Copyright Soramitsu Co., Ltd. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef CPP_FILECOIN_SYNC_PEER_MANAGER_HPP
#define CPP_FILECOIN_SYNC_PEER_MANAGER_HPP

#include <functional>
#include <list>
#include <set>
#include <unordered_map>

#include <libp2p/peer/peer_info.hpp>
#include "node/builder.hpp"
#include "node/config.hpp"
#include "sync/hello.hpp"

namespace fc::sync {

  class PeerManager : public std::enable_shared_from_this<PeerManager> {
   public:
    using PeerId = libp2p::peer::PeerId;

    using PeerStatusUpdateCallback = void(const PeerId &,
                                          bool /*is connected now*/,
                                          bool /*all protocols are supported*/,
                                          bool /*belongs to our network*/);

    struct GetPeerOptions {
      bool must_be_network_node = false;
      bool must_be_connected = false;
      std::set<std::string> must_handle_protocols;
    };

    PeerManager(const node::NodeObjects &o, const node::Config &c);

    ~PeerManager();

    boost::optional<libp2p::peer::PeerInfo> getPeerInfo(
        const PeerId &peer_id) const;

    boost::optional<libp2p::peer::PeerInfo> getPeerInfo(
        const PeerId &peer_id, const GetPeerOptions &options) const;

    void addBootstrapPeer(const std::string &p2p_address);

    const std::vector<libp2p::peer::PeerInfo> &getBootstrapPeers() const;

    std::vector<PeerId> getPeers();

    // connects to bootstrap peers
    outcome::result<void> start();

    // disconnects from peers
    void stop();

    boost::signals2::connection subscribe(
        const std::function<PeerStatusUpdateCallback> &cb);

    /// tell peer manager and others that given peer gone
    void reportOfflinePeer(const PeerId &peer_id);

   private:
    void onIdentifyReceived(const PeerId &peer_id);
    void onHello(const PeerId &peer_id,
                 outcome::result<Hello::Message> hello_message);
    void onHelloLatencyMessage(const PeerId &peer,
                               outcome::result<uint64_t> result);

    void postPeerStatus(const PeerId &peer_id);

    const std::set<std::string> node_protocols_;
    std::shared_ptr<libp2p::Host> host_;
    std::shared_ptr<clock::UTCClock> utc_clock_;
    std::shared_ptr<Hello> hello_;
    std::shared_ptr<libp2p::protocol::Identify> identify_protocol_;
    std::shared_ptr<libp2p::protocol::IdentifyPush> identify_push_protocol_;
    std::shared_ptr<libp2p::protocol::IdentifyDelta> identify_delta_protocol_;
    std::shared_ptr<storage::blockchain::ChainStore> chain_store_;
    std::vector<libp2p::peer::PeerInfo> bootstrap_peers_;
    bool started_ = false;

    boost::signals2::connection on_identify_;
    boost::signals2::signal<PeerStatusUpdateCallback> peer_update_signal_;

    struct PeersRepository {
      using PeersList = std::list<PeerId>;
      using PeersIter = PeersList::iterator;
      using PeersIndex = std::vector<PeersIter>;

      struct PeerInfoAndProtocols {
        PeersIter peer_iter;
        primitives::BigInt current_weight;
        boost::optional<libp2p::multi::Multiaddress> connect_to;
        std::vector<std::string> protocols;
      };

      PeerInfoAndProtocols &getRecord(const PeerId &peer_id);

      PeersList list;
      PeersIndex online;
      PeersIndex all_protocols;
      PeersIndex our_network;
      std::unordered_map<PeerId, PeerInfoAndProtocols> map;

      struct PeerIdComparator {
        bool operator()(const PeersIter &lhs, const PeersIter &rhs);

       private:
        static const std::string &peerToString(const PeerId &peer_id);
        static std::unordered_map<PeerId, std::string> cache;
      };
    } peers_;
  };

}  // namespace fc::sync

#endif  // CPP_FILECOIN_SYNC_PEER_MANAGER_HPP
