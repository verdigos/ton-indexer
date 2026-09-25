#pragma once
#include <block/block.h>
#include <td/actor/actor.h>
#include <mc-config.h>


using AllShardStates = std::vector<td::Ref<vm::Cell>>;

class TelemintContract: public td::actor::Actor {
public:
    struct Result {
        block::StdAddress address;
        std::string token_name;
        // Auction state
        std::optional<block::StdAddress> bidder_address;
        td::RefInt256 bid;
        uint32_t bid_ts;
        td::RefInt256 min_bid;
        uint32_t end_time;
        // Auction config
        std::optional<block::StdAddress> beneficiary_address;
        std::optional<td::RefInt256> initial_min_bid;
        std::optional<td::RefInt256> max_bid;
        std::optional<td::RefInt256> min_bid_step;
        std::optional<uint32_t> min_extend_time;
        std::optional<uint32_t> duration;
        // Royalty
        int royalty_numerator;
        int royalty_denominator;
        block::StdAddress royalty_destination;
    };

    TelemintContract(block::StdAddress address,
                     td::Ref<vm::Cell> code_cell,
                     td::Ref<vm::Cell> data_cell,
                     AllShardStates shard_states,
                     std::shared_ptr<block::ConfigInfo> config,
                     td::Promise<Result> promise);

    void start_up() override;

    static td::Result<Result> detect(const block::StdAddress& address,
                                     const td::Ref<vm::Cell>& code_cell,
                                     const td::Ref<vm::Cell>& data_cell,
                                     const std::shared_ptr<block::ConfigInfo>& config);

private:
    block::StdAddress address_;
    td::Ref<vm::Cell> code_cell_;
    td::Ref<vm::Cell> data_cell_;
    AllShardStates shard_states_;
    std::shared_ptr<block::ConfigInfo> config_;
    td::Promise<Result> promise_;
};
