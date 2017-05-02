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
#ifndef YB_TSERVER_REMOTE_BOOTSTRAP_CLIENT_H
#define YB_TSERVER_REMOTE_BOOTSTRAP_CLIENT_H

#include <string>
#include <memory>
#include <vector>

#include <gtest/gtest_prod.h>

#include "yb/consensus/consensus.h"
#include "yb/gutil/gscoped_ptr.h"
#include "yb/gutil/macros.h"
#include "yb/gutil/ref_counted.h"
#include "yb/util/status.h"

namespace yb {

class BlockId;
class BlockIdPB;
class FsManager;
class HostPort;

namespace consensus {
class ConsensusMetadata;
class ConsensusStatePB;
class RaftConfigPB;
class RaftPeerPB;
} // namespace consensus

namespace rpc {
class ErrorStatusPB;
class Messenger;
class RpcController;
} // namespace rpc

namespace tablet {
class TabletMetadata;
class TabletPeer;
class TabletStatusListener;
class TabletSuperBlockPB;
} // namespace tablet

namespace tserver {
class DataIdPB;
class DataChunkPB;
class RemoteBootstrapServiceProxy;
class TSTabletManager;

// Client class for using remote bootstrap to copy a tablet from another host.
// This class is not thread-safe.
//
// TODO:
// * Parallelize download of blocks and WAL segments.
//
class RemoteBootstrapClient {
 public:

  // Construct the remote bootstrap client.
  // 'fs_manager' and 'messenger' must remain valid until this object is destroyed.
  // 'client_permanent_uuid' is the permanent UUID of the caller server.
  RemoteBootstrapClient(std::string tablet_id, FsManager* fs_manager,
                        std::shared_ptr<rpc::Messenger> messenger,
                        std::string client_permanent_uuid);

  // Attempt to clean up resources on the remote end by sending an
  // EndRemoteBootstrapSession() RPC
  ~RemoteBootstrapClient();

  // Pass in the existing metadata for a tombstoned tablet, which will be
  // replaced if validation checks pass in Start().
  // 'meta' is the metadata for the tombstoned tablet and 'caller_term' is the
  // term provided by the caller (assumed to be the current leader of the
  // consensus config) for validation purposes.
  // If the consensus metadata exists on disk for this tablet, and if
  // 'caller_term' is lower than the current term stored in that consensus
  // metadata, then this method will fail with a Status::InvalidArgument error.
  CHECKED_STATUS SetTabletToReplace(const scoped_refptr<tablet::TabletMetadata>& meta,
                            int64_t caller_term);

  // Start up a remote bootstrap session to bootstrap from the specified
  // bootstrap peer. Place a new superblock indicating that remote bootstrap is
  // in progress. If the 'metadata' pointer is passed as NULL, it is ignored,
  // otherwise the TabletMetadata object resulting from the initial remote
  // bootstrap response is returned.
  // ts_manager pointer allows the bootstrap function to assign non-random
  // data and wal directories for the bootstrapped tablets.
  // TODO: Rename these parameters to bootstrap_source_*.
  CHECKED_STATUS Start(const std::string& bootstrap_peer_uuid,
               const HostPort& bootstrap_peer_addr,
               scoped_refptr<tablet::TabletMetadata>* metadata,
               TSTabletManager* ts_manager = nullptr);

  // Runs a "full" remote bootstrap, copying the physical layout of a tablet
  // from the leader of the specified consensus configuration.
  CHECKED_STATUS FetchAll(tablet::TabletStatusListener* status_listener);

  // After downloading all files successfully, write out the completed
  // replacement superblock.
  CHECKED_STATUS Finish();

  // Verify that the remote bootstrap was completed successfully by verifying that the ChangeConfig
  // request was propagated.
  CHECKED_STATUS VerifyRemoteBootstrapSucceeded(
      const scoped_refptr<consensus::Consensus>& shared_consensus);

 private:
  FRIEND_TEST(RemoteBootstrapKuduClientTest, TestBeginEndSession);
  FRIEND_TEST(RemoteBootstrapKuduClientTest, TestDownloadBlock);
  FRIEND_TEST(RemoteBootstrapKuduClientTest, TestVerifyData);
  FRIEND_TEST(RemoteBootstrapKuduClientTest, TestDownloadWalSegment);
  FRIEND_TEST(RemoteBootstrapKuduClientTest, TestDownloadAllBlocks);

  FRIEND_TEST(RemoteBootstrapRocksDBClientTest, TestBeginEndSession);
  FRIEND_TEST(RemoteBootstrapRocksDBClientTest, TestDownloadRocksDBFiles);

  // Extract the embedded Status message from the given ErrorStatusPB.
  // The given ErrorStatusPB must extend RemoteBootstrapErrorPB.
  static CHECKED_STATUS ExtractRemoteError(const rpc::ErrorStatusPB& remote_error);

  static CHECKED_STATUS UnwindRemoteError(const Status& status,
                                          const rpc::RpcController& controller);

  // Update the bootstrap StatusListener with a message.
  // The string "RemoteBootstrap: " will be prepended to each message.
  void UpdateStatusMessage(const std::string& message);

  // End the remote bootstrap session.
  CHECKED_STATUS EndRemoteSession();

  // Download all WAL files sequentially.
  CHECKED_STATUS DownloadWALs();

  // Download a single WAL file.
  // Assumes the WAL directories have already been created.
  // WAL file is opened with options so that it will fsync() on close.
  CHECKED_STATUS DownloadWAL(uint64_t wal_segment_seqno);

  // Write out the Consensus Metadata file based on the ConsensusStatePB
  // downloaded as part of initiating the remote bootstrap session.
  CHECKED_STATUS WriteConsensusMetadata();

  // Download all blocks belonging to a tablet sequentially.
  //
  // Blocks are given new IDs upon creation. On success, 'new_superblock_'
  // is populated to reflect the new block IDs and should be used in lieu
  // of 'superblock_' henceforth.
  CHECKED_STATUS DownloadBlocks();

  // Download the block specified by 'block_id'.
  //
  // On success:
  // - 'block_id' is set to the new ID of the downloaded block.
  // - 'block_count' is incremented.
  CHECKED_STATUS DownloadAndRewriteBlock(BlockIdPB* block_id, int* block_count, int num_blocks);

  // Download a single block.
  // Data block is opened with options so that it will fsync() on close.
  //
  // On success, 'new_block_id' is set to the new ID of the downloaded block.
  CHECKED_STATUS DownloadBlock(const BlockId& old_block_id, BlockId* new_block_id);

  // Download a single remote file. The block and WAL implementations delegate
  // to this method when downloading files.
  //
  // An Appendable is typically a WritableBlock (block) or WritableFile (WAL).
  //
  // Only used in one compilation unit, otherwise the implementation would
  // need to be in the header.
  template<class Appendable>
  CHECKED_STATUS DownloadFile(const DataIdPB& data_id, Appendable* appendable);

  CHECKED_STATUS DownloadRocksDBFiles();

  CHECKED_STATUS VerifyData(uint64_t offset, const DataChunkPB& resp);

  // Return standard log prefix.
  std::string LogPrefix();

  // Set-once members.
  const std::string tablet_id_;
  FsManager* const fs_manager_;
  const std::shared_ptr<rpc::Messenger> messenger_;
  const std::string permanent_uuid_;

  // State flags that enforce the progress of remote bootstrap.
  bool started_;            // Session started.
  bool downloaded_wal_;     // WAL segments downloaded.
  bool downloaded_blocks_;  // Data blocks downloaded.
  bool downloaded_rocksdb_files_;

  // Session-specific data items.
  bool replace_tombstoned_tablet_;

  // Local tablet metadata file.
  scoped_refptr<tablet::TabletMetadata> meta_;

  // Local Consensus metadata file. This may initially be NULL if this is
  // bootstrapping a new replica (rather than replacing an old one).
  gscoped_ptr<consensus::ConsensusMetadata> cmeta_;

  tablet::TabletStatusListener* status_listener_;
  std::shared_ptr<RemoteBootstrapServiceProxy> proxy_;
  std::string session_id_;
  uint64_t session_idle_timeout_millis_;
  gscoped_ptr<tablet::TabletSuperBlockPB> superblock_;
  gscoped_ptr<tablet::TabletSuperBlockPB> new_superblock_;
  gscoped_ptr<consensus::ConsensusStatePB> remote_committed_cstate_;
  std::vector<uint64_t> wal_seqnos_;

  int64_t start_time_micros_;

  // We track whether this session succeeded and send this information as part of the
  // EndRemoteBootstrapSessionRequestPB request.
  bool succeeded_;

  DISALLOW_COPY_AND_ASSIGN(RemoteBootstrapClient);
};

} // namespace tserver
} // namespace yb
#endif /* YB_TSERVER_REMOTE_BOOTSTRAP_CLIENT_H */
