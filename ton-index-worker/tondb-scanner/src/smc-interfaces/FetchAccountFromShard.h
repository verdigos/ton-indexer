#pragma once
#include <block/block.h>
#include <td/actor/actor.h>
#include <mc-config.h>
#include <optional>

#include "IndexData.h"


using AllShardStates = std::vector<td::Ref<vm::Cell>>;

// Found = engaged; absent from the covering dictionary or account_none = nullopt.
// Missing shard coverage, unpack failure or an unknown account tag = error.
td::Result<std::optional<schema::AccountState>> lookup_account(const AllShardStates& shard_states,
                                                const block::StdAddress& address);

class FetchAccountFromShardV2: public td::actor::Actor {
private:
  AllShardStates shard_states_;
  block::StdAddress address_;
  td::Promise<schema::AccountState> promise_;
public:
  FetchAccountFromShardV2(AllShardStates shard_states, block::StdAddress address, td::Promise<schema::AccountState> promise)
    : shard_states_(shard_states), address_(address), promise_(std::move(promise)) {
  }

  void start_up() override;
};
