/*
 * Copyright (c) 2018 Zilliqa
 * This source code is being disclosed to you solely for the purpose of your
 * participation in testing Zilliqa. You may view, compile and run the code for
 * that purpose and pursuant to the protocols and algorithms that are programmed
 * into, and intended by, the code. You may not do anything else with the code
 * without express permission from Zilliqa Research Pte. Ltd., including
 * modifying or publishing the code (or any part of it), and developing or
 * forming another public or private blockchain network. This source code is
 * provided 'as is' and no warranties are given as to title or non-infringement,
 * merchantability or fitness for purpose and, to the extent permitted by law,
 * all liability for your use of the code is disclaimed. Some programs in this
 * code are governed by the GNU General Public License v3.0 (available at
 * https://www.gnu.org/licenses/gpl-3.0.en.html) ('GPLv3'). The programs that
 * are governed by GPLv3.0 are those programs that are located in the folders
 * src/depends and tests/depends and which include a reference to GPLv3 in their
 * program files.
 */

#include <array>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include <boost/multiprecision/cpp_int.hpp>
#pragma GCC diagnostic pop
#include <chrono>
#include <functional>
#include <thread>

#include "Node.h"
#include "common/Constants.h"
#include "common/Messages.h"
#include "common/Serializable.h"
#include "libCrypto/Sha2.h"
#include "libMediator/Mediator.h"
#include "libMessage/Messenger.h"
#include "libUtils/BitVector.h"
#include "libUtils/DataConversion.h"
#include "libUtils/DetachedFunction.h"
#include "libUtils/Logger.h"
#include "libUtils/TimeLockedFunction.h"
#include "libUtils/TimeUtils.h"
#include "libUtils/TimestampVerifier.h"

using namespace std;

void Node::UpdateDSCommitteeAfterFallback(const uint32_t& shard_id,
                                          const PubKey& leaderPubKey,
                                          const Peer& leaderNetworkInfo,
                                          deque<pair<PubKey, Peer>>& dsComm,
                                          const DequeOfShard& shards) {
  dsComm.clear();
  for (auto const& shardNode : shards[shard_id]) {
    if (std::get<SHARD_NODE_PUBKEY>(shardNode) == leaderPubKey &&
        std::get<SHARD_NODE_PEER>(shardNode) == leaderNetworkInfo) {
      dsComm.push_front({leaderPubKey, leaderNetworkInfo});
    } else {
      dsComm.push_back({std::get<SHARD_NODE_PUBKEY>(shardNode),
                        std::get<SHARD_NODE_PEER>(shardNode)});
    }
  }
}

bool Node::VerifyFallbackBlockCoSignature(const FallbackBlock& fallbackblock) {
  LOG_MARKER();

  unsigned int index = 0;
  unsigned int count = 0;

  uint32_t shard_id = fallbackblock.GetHeader().GetShardId();

  const vector<bool>& B2 = fallbackblock.GetB2();
  if (m_mediator.m_ds->m_shards[shard_id].size() != B2.size()) {
    LOG_GENERAL(WARNING,
                "Mismatch: shard "
                    << fallbackblock.GetHeader().GetShardId()
                    << " size = " << m_mediator.m_ds->m_shards[shard_id].size()
                    << ", co-sig bitmap size = " << B2.size());
    return false;
  }

  // Generate the aggregated key
  vector<PubKey> keys;

  for (auto const& shardNode : m_mediator.m_ds->m_shards[shard_id]) {
    if (B2.at(index)) {
      keys.emplace_back(std::get<SHARD_NODE_PUBKEY>(shardNode));
      count++;
    }
    index++;
  }

  if (count != ConsensusCommon::NumForConsensus(B2.size())) {
    LOG_GENERAL(WARNING, "Cosig was not generated by enough nodes");
    return false;
  }

  shared_ptr<PubKey> aggregatedKey = MultiSig::AggregatePubKeys(keys);
  if (aggregatedKey == nullptr) {
    LOG_GENERAL(WARNING, "Aggregated key generation failed");
    return false;
  }

  // Verify the collective signature
  vector<unsigned char> message;
  if (!fallbackblock.GetHeader().Serialize(message, 0)) {
    LOG_GENERAL(WARNING, "FallbackBlockHeader serialization failed");
    return false;
  }
  fallbackblock.GetCS1().Serialize(message, message.size());
  BitVector::SetBitVector(message, message.size(), fallbackblock.GetB1());
  if (!Schnorr::GetInstance().Verify(message, 0, message.size(),
                                     fallbackblock.GetCS2(), *aggregatedKey)) {
    LOG_GENERAL(WARNING, "Cosig verification failed. Pubkeys");
    for (auto& kv : keys) {
      LOG_GENERAL(WARNING, kv);
    }
    return false;
  }

  return true;
}

bool Node::ProcessFallbackBlock(const vector<unsigned char>& message,
                                unsigned int cur_offset,
                                [[gnu::unused]] const Peer& from) {
  // Message = [Fallback block]
  LOG_MARKER();

  // CheckState
  if (!CheckState(PROCESS_FALLBACKBLOCK)) {
    LOG_GENERAL(INFO,
                "Not in status for ProcessingFallbackBlock, "
                "wait state changing for "
                    << FALLBACK_EXTRA_TIME << " seconds");
    std::unique_lock<std::mutex> cv_lk(m_MutexCVFallbackBlock);
    if (cv_fallbackBlock.wait_for(
            cv_lk, std::chrono::seconds(FALLBACK_EXTRA_TIME),
            [this] { return m_state == WAITING_FALLBACKBLOCK; })) {
      LOG_EPOCH(INFO, to_string(m_mediator.m_currentEpochNum).c_str(),
                "Successfully transit to waiting_fallbackblock or I am in the "
                "correct state.");
    } else {
      return false;
    }
  }

  FallbackBlock fallbackblock;

  if (!Messenger::GetNodeFallbackBlock(message, cur_offset, fallbackblock)) {
    LOG_EPOCH(WARNING, to_string(m_mediator.m_currentEpochNum).c_str(),
              "Messenger::GetNodeFallbackBlock failed.");
    return false;
  }

  if (!m_mediator.CheckWhetherBlockIsLatest(
          fallbackblock.GetHeader().GetFallbackDSEpochNo(),
          fallbackblock.GetHeader().GetFallbackEpochNo())) {
    LOG_GENERAL(WARNING,
                "ProcessFallbackBlock CheckWhetherBlockIsLatest failed");
    return false;
  }

  BlockHash temp_blockHash = fallbackblock.GetHeader().GetMyHash();
  if (temp_blockHash != fallbackblock.GetBlockHash()) {
    LOG_GENERAL(WARNING,
                "Block Hash in Newly received FB Block doesn't match. "
                "Calculated: "
                    << temp_blockHash
                    << " Received: " << fallbackblock.GetBlockHash().hex());
    return false;
  }

  // Check timestamp
  if (!VerifyTimestamp(fallbackblock.GetTimestamp(),
                       CONSENSUS_OBJECT_TIMEOUT + FALLBACK_INTERVAL_WAITING +
                           FALLBACK_CHECK_INTERVAL + FALLBACK_EXTRA_TIME)) {
    return false;
  }

  // Check shard
  uint32_t shard_id = fallbackblock.GetHeader().GetShardId();
  {
    lock_guard<mutex> g(m_mediator.m_ds->m_mutexShards);

    if (shard_id >= m_mediator.m_ds->m_shards.size()) {
      LOG_GENERAL(WARNING,
                  "The shard doesn't exist here for this id " << shard_id);
      return false;
    }

    CommitteeHash committeeHash;
    if (!Messenger::GetShardHash(m_mediator.m_ds->m_shards.at(shard_id),
                                 committeeHash)) {
      LOG_EPOCH(WARNING, to_string(m_mediator.m_currentEpochNum).c_str(),
                "Messenger::GetShardHash failed.");
      return false;
    }
    if (committeeHash != fallbackblock.GetHeader().GetCommitteeHash()) {
      LOG_GENERAL(WARNING, "Fallback committee hash mismatched"
                               << endl
                               << "expected: " << committeeHash << endl
                               << "received: "
                               << fallbackblock.GetHeader().GetCommitteeHash());
      return false;
    }

    // Check consensus leader network info and pubkey
    uint32_t leaderConsensusId =
        fallbackblock.GetHeader().GetLeaderConsensusId();
    if (leaderConsensusId >= m_mediator.m_ds->m_shards[shard_id].size()) {
      LOG_GENERAL(
          WARNING,
          "The consensusLeaderId "
              << leaderConsensusId
              << " is larger than the size of that shard member we have "
              << m_mediator.m_ds->m_shards[shard_id].size());
      return false;
    }

    const PubKey& leaderPubKey = fallbackblock.GetHeader().GetLeaderPubKey();
    const Peer& leaderNetworkInfo =
        fallbackblock.GetHeader().GetLeaderNetworkInfo();

    auto leader = make_tuple(leaderPubKey, leaderNetworkInfo, 0);

    auto found = std::find_if(m_mediator.m_ds->m_shards[shard_id].begin(),
                              m_mediator.m_ds->m_shards[shard_id].end(),
                              [&leader](const auto& item) {
                                return (std::get<SHARD_NODE_PUBKEY>(leader) ==
                                        std::get<SHARD_NODE_PUBKEY>(item)) &&
                                       (std::get<SHARD_NODE_PEER>(leader) ==
                                        std::get<SHARD_NODE_PEER>(item));
                              });
    if (found == m_mediator.m_ds->m_shards[shard_id].end()) {
      LOG_GENERAL(
          WARNING,
          "The expected consensus leader not found in sharding structure"
              << endl
              << "PubKey: " << leaderPubKey << endl
              << "Peer: " << leaderNetworkInfo);
      return false;
    }

    if (AccountStore::GetInstance().GetStateRootHash() !=
        fallbackblock.GetHeader().GetStateRootHash()) {
      LOG_GENERAL(WARNING,
                  "The state root hash mismatched"
                      << endl
                      << "expected: "
                      << AccountStore::GetInstance().GetStateRootHash().hex()
                      << endl
                      << "received: "
                      << fallbackblock.GetHeader().GetStateRootHash().hex());
      return false;
    }

    if (!VerifyFallbackBlockCoSignature(fallbackblock)) {
      LOG_EPOCH(WARNING, to_string(m_mediator.m_currentEpochNum).c_str(),
                "FallbackBlock co-sig verification failed");
      return false;
    }

    uint64_t latestInd = m_mediator.m_blocklinkchain.GetLatestIndex() + 1;
    m_mediator.m_blocklinkchain.AddBlockLink(
        latestInd, fallbackblock.GetHeader().GetFallbackDSEpochNo(),
        BlockType::FB, fallbackblock.GetBlockHash());

    vector<unsigned char> dst;

    FallbackBlockWShardingStructure fbblockwshards(fallbackblock,
                                                   m_mediator.m_ds->m_shards);

    if (!fbblockwshards.Serialize(dst, 0)) {
      LOG_GENERAL(WARNING, "Failed to Serialize");
    } else {
      if (!BlockStorage::GetBlockStorage().PutFallbackBlock(
              fallbackblock.GetBlockHash(), dst)) {
        LOG_GENERAL(WARNING, "Unable to store FallbackBlock");
      }
    }

    FallbackTimerPulse();
    {
      lock_guard<mutex> g(m_mediator.m_mutexDSCommittee);
      UpdateDSCommitteeAfterFallback(shard_id, leaderPubKey, leaderNetworkInfo,
                                     *m_mediator.m_DSCommittee,
                                     m_mediator.m_ds->m_shards);
    }
    StoreState();
  }

  if (!LOOKUP_NODE_MODE) {
    if (BROADCAST_TREEBASED_CLUSTER_MODE) {
      SendFallbackBlockToOtherShardNodes(message);
    }

    BlockStorage::GetBlockStorage().PutMetadata(MetaType::DSINCOMPLETED, {'0'});

    // Clean processedTxn may have been produced during last microblock
    // consensus
    {
      lock_guard<mutex> g(m_mutexProcessedTransactions);
      m_processedTransactions.erase(m_mediator.m_currentEpochNum);
    }

    CleanCreatedTransaction();

    CleanMicroblockConsensusBuffer();

    AccountStore::GetInstance().InitTemp();

    InitiatePoW();
  } else {
    m_mediator.m_consensusID = 0;
    m_consensusLeaderID = 0;
  }

  LOG_EPOCH(INFO, to_string(m_mediator.m_currentEpochNum).c_str(),
            "I am a node and my DS committee is successfully fallback to shard "
                << shard_id);

  return true;
}

void Node::SendFallbackBlockToOtherShardNodes(
    const vector<unsigned char>& fallbackblock_message) {
  LOG_MARKER();
  unsigned int cluster_size = NUM_FORWARDED_BLOCK_RECEIVERS_PER_SHARD;
  if (cluster_size <= NUM_DS_ELECTION) {
    LOG_GENERAL(
        WARNING,
        "Adjusting NUM_FORWARDED_BLOCK_RECEIVERS_PER_SHARD to be greater than "
        "NUM_DS_ELECTION. Why not correct the constant.xml next time.");
    cluster_size = NUM_DS_ELECTION + 1;
  }
  LOG_GENERAL(INFO,
              "Primary CLUSTER SIZE used is "
              "(NUM_FORWARDED_BLOCK_RECEIVERS_PER_SHARD):"
                  << cluster_size);
  SendBlockToOtherShardNodes(fallbackblock_message, cluster_size,
                             NUM_OF_TREEBASED_CHILD_CLUSTERS);
}
