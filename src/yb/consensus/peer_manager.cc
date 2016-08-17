// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "yb/consensus/peer_manager.h"

#include <mutex>

#include "yb/consensus/consensus_peers.h"
#include "yb/consensus/log.h"
#include "yb/gutil/map-util.h"
#include "yb/gutil/stl_util.h"
#include "yb/gutil/strings/substitute.h"
#include "yb/util/threadpool.h"

namespace yb {
namespace consensus {

using log::Log;
using strings::Substitute;

PeerManager::PeerManager(const std::string tablet_id,
                         const std::string local_uuid,
                         PeerProxyFactory* peer_proxy_factory,
                         PeerMessageQueue* queue,
                         ThreadPool* request_thread_pool,
                         const scoped_refptr<log::Log>& log)
    : tablet_id_(tablet_id),
      local_uuid_(local_uuid),
      peer_proxy_factory_(peer_proxy_factory),
      queue_(queue),
      thread_pool_(request_thread_pool),
      log_(log) {
}

PeerManager::~PeerManager() {
  Close();
}

Status PeerManager::UpdateRaftConfig(const RaftConfigPB& config) {
  unordered_set<string> new_peers;

  VLOG(1) << "Updating peers from new config: " << config.ShortDebugString();

  std::lock_guard<simple_spinlock> lock(lock_);
  // Create new peers
  for (const RaftPeerPB& peer_pb : config.peers()) {
    new_peers.insert(peer_pb.permanent_uuid());
    Peer* peer = FindPtrOrNull(peers_, peer_pb.permanent_uuid());
    if (peer != nullptr) {
      continue;
    }
    if (peer_pb.permanent_uuid() == local_uuid_) {
      continue;
    }

    VLOG(1) << GetLogPrefix() << "Adding remote peer. Peer: " << peer_pb.ShortDebugString();
    gscoped_ptr<PeerProxy> peer_proxy;
    RETURN_NOT_OK_PREPEND(peer_proxy_factory_->NewProxy(peer_pb, &peer_proxy),
                          "Could not obtain a remote proxy to the peer.");

    gscoped_ptr<Peer> remote_peer;
    RETURN_NOT_OK(Peer::NewRemotePeer(peer_pb,
                                      tablet_id_,
                                      local_uuid_,
                                      queue_,
                                      thread_pool_,
                                      peer_proxy.Pass(),
                                      &remote_peer));
    InsertOrDie(&peers_, peer_pb.permanent_uuid(), remote_peer.release());
  }

  return Status::OK();
}

void PeerManager::SignalRequest(bool force_if_queue_empty) {
  std::lock_guard<simple_spinlock> lock(lock_);
  auto iter = peers_.begin();
  for (; iter != peers_.end(); iter++) {
    Status s = (*iter).second->SignalRequest(force_if_queue_empty);
    if (PREDICT_FALSE(!s.ok())) {
      LOG(WARNING) << GetLogPrefix()
                   << "Peer was closed, removing from peers. Peer: "
                   << (*iter).second->peer_pb().ShortDebugString();
      peers_.erase(iter);
    }
  }
}

void PeerManager::Close() {
  {
    std::lock_guard<simple_spinlock> lock(lock_);
    for (const PeersMap::value_type& entry : peers_) {
      entry.second->Close();
    }
    STLDeleteValues(&peers_);
  }
}

std::string PeerManager::GetLogPrefix() const {
  return Substitute("T $0 P $1: ", tablet_id_, local_uuid_);
}

} // namespace consensus
} // namespace yb
