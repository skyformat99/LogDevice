/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "NodesConfig.h"
#include <folly/hash/SpookyHashV2.h>

namespace facebook { namespace logdevice { namespace configuration {

void NodesConfig::calculateHash() {
  static_assert(sizeof(node_index_t) == 2,
                "node_index_t size has changed, "
                "this will cause a recalculation of "
                "nodes config hashes");
  static_assert(sizeof(shard_size_t) == 2,
                "shard_size_t size has changed, "
                "this will cause a recalculation of "
                "nodes config hashes");

  // Generating a list of sorted node IDs, so the order is consistent regardless
  // of std::unordered_map internals
  std::vector<node_index_t> sorted_node_ids(nodes_.size());
  std::transform(
      nodes_.begin(),
      nodes_.end(),
      sorted_node_ids.begin(),
      [](const std::pair<node_index_t, Node>& src) { return src.first; });
  std::sort(sorted_node_ids.begin(), sorted_node_ids.end());

  std::string hashable_string;

  // for each node, writing out the attributes being hashed
  auto append = [&](const void* ptr, size_t num_bytes) {
    size_t old_size = hashable_string.size();
    size_t new_size = old_size + num_bytes;
    if (new_size > hashable_string.capacity()) {
      size_t new_capacity = std::max(256ul, hashable_string.capacity() * 2);
      hashable_string.reserve(new_capacity);
    }
    hashable_string.resize(new_size);
    memcpy(&hashable_string[old_size], ptr, num_bytes);
  };

  for (auto node_id : sorted_node_ids) {
    auto it = nodes_.find(node_id);
    ld_check(it != nodes_.end());
    const auto& node = it->second;
    std::string location_str = node.locationStr();

    append(&node_id, sizeof(node_id));
    double storage_capacity = node.storage_capacity.value_or(0);
    append(&storage_capacity, sizeof(storage_capacity));
    append(&node.storage_state, sizeof(node.storage_state));
    append(&node.exclude_from_nodesets, sizeof(node.exclude_from_nodesets));
    append(&node.num_shards, sizeof(node.num_shards));
    // appending location str and the terminating null-byte
    append(location_str.c_str(), location_str.size() + 1);
  }
  const uint64_t SEED = 0x9a6bf3f8ebcd8cdfL; // random
  hash_ = folly::hash::SpookyHashV2::Hash64(
      hashable_string.data(), hashable_string.size(), SEED);
}

// TODO(T15517759): remove when Flexible Log Sharding is fully implemented.
void NodesConfig::calculateNumShards() {
  num_shards_ = 0;
  for (auto& it : nodes_) {
    if (!it.second.isReadableStorageNode()) {
      continue;
    }
    ld_check(it.second.num_shards > 0);
    num_shards_ = it.second.num_shards;
    break; // The other storage nodes have the same amount of shards.
  }
}

}}} // namespace facebook::logdevice::configuration
