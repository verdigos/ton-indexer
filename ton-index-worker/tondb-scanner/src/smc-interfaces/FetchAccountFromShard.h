#pragma once
#include <block/block.h>
#include <td/actor/actor.h>
#include <mc-config.h>

#include "IndexData.h"


using AllShardStates = std::vector<td::Ref<vm::Cell>>;

// The shard-state account read as a plain function. Reading a shard state is a
// dictionary lookup plus a TLB unpack — entirely synchronous — so the actor
// below buys nothing but a scheduler hop, and a caller already on a synchronous
// path (the celldb tier-2 lookup hook) can use this directly. Same static parse
// seam idea as parse_transaction/parse_message in DataParser.
td::Result<schema::AccountState> lookup_account(const AllShardStates& shard_states,
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
