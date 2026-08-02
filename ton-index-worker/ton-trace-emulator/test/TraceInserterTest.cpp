#include "../src/TraceInserter.cpp"

#include "td/utils/tests.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::string actions_blob(std::uint8_t finality) {
    msgpack::sbuffer buffer;
    msgpack::packer<msgpack::sbuffer> packer(&buffer);
    packer.pack_array(2);

    packer.pack_map(4);
    packer.pack(std::string("action_id"));
    packer.pack(std::string("first"));
    packer.pack(std::string("finality"));
    packer.pack(finality);
    packer.pack(std::string("success"));
    packer.pack(true);
    packer.pack(std::string("amount"));
    packer.pack(std::uint64_t{17});

    packer.pack_map(4);
    packer.pack(std::string("action_id"));
    packer.pack(std::string("second"));
    packer.pack(std::string("finality"));
    packer.pack(finality);
    packer.pack(std::string("success"));
    packer.pack(false);
    packer.pack(std::string("amount"));
    packer.pack(std::uint64_t{29});

    return {buffer.data(), buffer.size()};
}

const msgpack::object& map_field(
    const msgpack::object& row,
    std::string_view name) {
    for (std::uint32_t index = 0; index < row.via.map.size; ++index) {
        const auto& field = row.via.map.ptr[index];
        if (field.key.type == msgpack::type::STR &&
            std::string_view(field.key.via.str.ptr,
                             field.key.via.str.size) == name) {
            return field.val;
        }
    }
    UNREACHABLE();
}

std::optional<std::string> redis_field(
    const RedisWritePlan& plan,
    std::string_view name) {
    for (const auto& [field, value] : plan.fields_to_set) {
        if (field == name) {
            return value;
        }
    }
    return std::nullopt;
}

ActiveTrace trace_with_actions(
    FinalityState trace_finality,
    std::optional<std::uint8_t> action_finality) {
    ActiveTrace trace;
    trace.finality = trace_finality;
    trace.metadata.emplace("root_node", "root");
    if (action_finality) {
        trace.actions.blob = actions_blob(*action_finality);
        trace.actions.blob_finality = *action_finality;
        trace.actions.classify_state = "ok";
    }
    return trace;
}

PreparedTraceUpdate promote(ActiveTrace current, bool materialize_full_state = false) {
    auto result = prepare_confirmed_promotion(
        current, CachedConfirmedTrace{}, "trace", 100, materialize_full_state);
    ASSERT_TRUE(result.is_ok());
    return result.move_as_ok();
}

struct RecordedRedisCommand {
    std::string name;
    std::string key;
    std::string member;
};

class PipelineFailureMock {
public:
    std::map<std::string, std::map<std::string, std::string>> hashes;
    std::map<std::string, std::set<std::string>> indexes;
    std::set<std::string> mappings;
    std::vector<std::pair<std::string, std::string>> publications;
    std::vector<RecordedRedisCommand> commands;

    void fail_next(std::string command) {
        failure_ = std::move(command);
    }

    void execute_data(const RedisWriteBatch& batch) {
        bool failed = false;
        for (const auto& plan : batch.plans) {
            auto index_writes = group_redis_index_writes(
                plan.indexes_to_remove, plan.indexes_to_add);
            if (plan.erase_trace) {
                for (const auto& index : index_writes) {
                    if (index.members_to_remove.empty()) {
                        continue;
                    }
                    commands.push_back({"ZREM", index.index_key, {}});
                    if (reject("ZREM")) {
                        failed = true;
                    } else {
                        for (const auto& member : index.members_to_remove) {
                            indexes[index.index_key].erase(member);
                        }
                    }
                }
                commands.push_back({"UNLINK", plan.trace_key, {}});
                if (reject("UNLINK")) {
                    failed = true;
                } else {
                    hashes.erase(plan.trace_key);
                }
                if (!plan.raw_external_message_hash.empty()) {
                    const auto mapping =
                        "tr_in_msg:" + plan.raw_external_message_hash;
                    commands.push_back({"DEL", mapping, {}});
                    if (reject("DEL")) {
                        failed = true;
                    } else {
                        mappings.erase(mapping);
                    }
                }
                continue;
            }

            for (const auto& field : plan.node_fields_to_delete) {
                commands.push_back({"HDEL", plan.trace_key, field});
                if (reject("HDEL")) {
                    failed = true;
                } else {
                    hashes[plan.trace_key].erase(field);
                }
            }
            for (const auto& index : index_writes) {
                if (!index.members_to_remove.empty()) {
                    commands.push_back({"ZREM", index.index_key, {}});
                    if (reject("ZREM")) {
                        failed = true;
                    } else {
                        for (const auto& member : index.members_to_remove) {
                            indexes[index.index_key].erase(member);
                        }
                    }
                }
                if (!index.members_to_add.empty()) {
                    commands.push_back({"ZADD", index.index_key, {}});
                    if (reject("ZADD")) {
                        failed = true;
                    } else {
                        for (const auto& [member, _] : index.members_to_add) {
                            indexes[index.index_key].insert(member);
                        }
                    }
                }
            }
            if (!plan.fields_to_set.empty()) {
                commands.push_back({"HSET", plan.trace_key, {}});
                if (reject("HSET")) {
                    failed = true;
                } else {
                    for (const auto& [field, value] : plan.fields_to_set) {
                        hashes[plan.trace_key].insert_or_assign(field, value);
                    }
                }
            }
        }
        if (failed) {
            throw std::runtime_error("injected data pipeline reply error");
        }
    }

    void execute_publications(const RedisWriteBatch& batch) {
        bool failed = false;
        for (const auto& plan : batch.plans) {
            for (const auto& [channel, message] : plan.publications) {
                commands.push_back({"PUBLISH", channel, message});
                if (reject("PUBLISH")) {
                    failed = true;
                } else {
                    publications.emplace_back(channel, message);
                }
            }
        }
        if (failed) {
            throw std::runtime_error(
                "injected publication pipeline reply error");
        }
    }

    bool has_member(const TraceStateIndexRef& ref) const {
        auto index = indexes.find(ref.index_key);
        return index != indexes.end() &&
               index->second.count(ref.member) != 0;
    }

    std::size_t count_commands(std::string_view name) const {
        return std::count_if(
            commands.begin(), commands.end(), [name](const auto& command) {
                return command.name == name;
            });
    }

private:
    std::optional<std::string> failure_;

    bool reject(std::string_view command) {
        if (!failure_ || *failure_ != command) {
            return false;
        }
        failure_.reset();
        return true;
    }
};

ActiveTrace trace_for_cleanup_retry(
    const TraceStateIndexRef& aai_ref,
    TraceStateFinality node_finality,
    std::uint8_t action_finality,
    std::uint64_t update_seq) {
    auto trace = trace_with_actions(
        static_cast<FinalityState>(
            static_cast<std::uint8_t>(node_finality)),
        action_finality);
    trace.update_seq = update_seq;
    trace.actions.aai_refs = {aai_ref};
    auto node_change = trace.nodes.upsert_nodes({TraceStateNode{
        .key = "root",
        .finality = node_finality,
        .serialized = std::make_shared<const std::string>("root-node"),
    }});
    trace.nodes.apply(std::move(node_change));
    return trace;
}

PromoteConfirmedRequest promotion_request(
    std::shared_ptr<const ActiveTrace> fallback,
    bool& completed) {
    return PromoteConfirmedRequest{
        .fallback_state = std::move(fallback),
        .mc_seqno = 100,
        .promise = td::PromiseCreator::lambda(
            [&completed](td::Result<td::Unit> result) {
                completed = result.is_ok();
            }),
    };
}

InsertRequest regular_insert_request(bool& completed) {
    InsertCompletion completion;
    completion.regular_promise = td::PromiseCreator::lambda(
        [&completed](td::Result<td::Unit> result) {
            completed = result.is_ok();
        });
    return InsertRequest{
        .completion = std::move(completion),
        .timer = td::Timer(),
    };
}

RedisWriteBatch take_ready_cleanup_retry(TraceSlot& slot,
                                         TraceCleanupMode& mode) {
    ASSERT_TRUE(slot.cleanup_retry.has_value());
    ASSERT_TRUE(slot.cleanup_retry->ready);
    mode = slot.cleanup_retry->mode;
    auto batch = std::move(slot.cleanup_retry->batch);
    slot.cleanup_retry.reset();
    return batch;
}

PreparedTraceUpdate prepare_surviving_full_promotion(TraceSlot& slot) {
    ASSERT_EQ(1u, slot.queued.size());
    auto request = std::move(
        std::get<PromoteConfirmedRequest>(slot.queued.front()));
    slot.queued.pop_front();
    ASSERT_TRUE(slot.current->nodes.nodes().empty());
    slot.current = std::move(request.fallback_state);
    auto prepared = prepare_confirmed_promotion(
        *slot.current, request.trace, "trace", request.mc_seqno, true);
    ASSERT_TRUE(prepared.is_ok());
    request.promise.set_value(td::Unit());
    return prepared.move_as_ok();
}

}  // namespace

TEST(TraceInserter, promotion_rewrites_all_action_finalities_and_keeps_content) {
    auto prepared = promote(trace_with_actions(FinalityState::Finalized, 1));

    auto rewritten = redis_field(prepared.redis, kActionsField);
    ASSERT_TRUE(rewritten.has_value());
    ASSERT_EQ(std::string("2"),
              *redis_field(prepared.redis, kActionsFinalityField));
    ASSERT_EQ(std::uint8_t{2},
              *prepared.next_trace.actions.blob_finality);
    ASSERT_EQ(*rewritten, *prepared.next_trace.actions.blob);

    auto unpacked = msgpack::unpack(rewritten->data(), rewritten->size());
    const auto& rows = unpacked.get();
    ASSERT_EQ(2u, rows.via.array.size);
    ASSERT_EQ(std::string("first"),
              map_field(rows.via.array.ptr[0], "action_id").as<std::string>());
    ASSERT_TRUE(map_field(rows.via.array.ptr[0], "success").as<bool>());
    ASSERT_EQ(std::uint64_t{17},
              map_field(rows.via.array.ptr[0], "amount").as<std::uint64_t>());
    ASSERT_EQ(std::uint64_t{2},
              map_field(rows.via.array.ptr[0], "finality").as<std::uint64_t>());
    ASSERT_EQ(std::string("second"),
              map_field(rows.via.array.ptr[1], "action_id").as<std::string>());
    ASSERT_TRUE(!map_field(rows.via.array.ptr[1], "success").as<bool>());
    ASSERT_EQ(std::uint64_t{29},
              map_field(rows.via.array.ptr[1], "amount").as<std::uint64_t>());
    ASSERT_EQ(std::uint64_t{2},
              map_field(rows.via.array.ptr[1], "finality").as<std::uint64_t>());
}

TEST(TraceInserter, promotion_without_actions_writes_no_action_fields) {
    auto prepared = promote(trace_with_actions(FinalityState::Finalized, std::nullopt));

    ASSERT_TRUE(!redis_field(prepared.redis, kActionsField).has_value());
    ASSERT_TRUE(!redis_field(prepared.redis, kActionsFinalityField).has_value());
    ASSERT_TRUE(!redis_field(prepared.redis, kActionsStateField).has_value());
}

TEST(TraceInserter, promotion_at_equal_finality_still_rewrites_actions) {
    auto prepared = promote(trace_with_actions(FinalityState::Confirmed, 1));

    ASSERT_TRUE(redis_field(prepared.redis, kActionsField).has_value());
    ASSERT_EQ(std::string("1"),
              *redis_field(prepared.redis, kActionsFinalityField));
    ASSERT_EQ(std::uint8_t{1},
              *prepared.next_trace.actions.blob_finality);
}

TEST(TraceInserter, promotion_cannot_demote_actions) {
    auto current = trace_with_actions(FinalityState::Emulated, 1);
    auto original_blob = *current.actions.blob;
    auto prepared = promote(std::move(current));

    ASSERT_TRUE(!redis_field(prepared.redis, kActionsField).has_value());
    ASSERT_TRUE(!redis_field(prepared.redis, kActionsFinalityField).has_value());
    ASSERT_EQ(std::uint8_t{1},
              *prepared.next_trace.actions.blob_finality);
    ASSERT_EQ(original_blob, *prepared.next_trace.actions.blob);
}

TEST(TraceInserter, full_materialization_restores_action_marker_and_indexes) {
    auto current = trace_with_actions(FinalityState::Finalized, 1);
    current.actions.aai_refs.push_back(TraceStateIndexRef{
        .index_key = "_aai:account",
        .member = "trace:first",
        .score = 17,
    });
    auto prepared = promote(std::move(current), true);

    ASSERT_EQ(std::string("ok"),
              *redis_field(prepared.redis, kActionsStateField));
    ASSERT_TRUE(redis_field(prepared.redis, kActionsField).has_value());
    ASSERT_EQ(1u, prepared.redis.indexes_to_add.size());
    ASSERT_EQ(std::string("_aai:account"),
              prepared.redis.indexes_to_add[0].index_key);
    ASSERT_EQ(std::string("trace:first"),
              prepared.redis.indexes_to_add[0].member);
    ASSERT_EQ(std::uint64_t{17}, prepared.redis.indexes_to_add[0].score);
}

TEST(TraceInserter, cleanup_removes_aai_refs) {
    const TraceStateIndexRef node_ref{
        .index_key = "account:node",
        .member = "trace:node",
        .score = 11,
    };
    const TraceStateIndexRef aai_ref{
        .index_key = "_aai:account",
        .member = "trace:action",
        .score = 17,
    };
    ActiveTrace current;
    auto node_change = current.nodes.upsert_nodes({TraceStateNode{
        .key = "node",
        .index_refs = {node_ref},
    }});
    current.nodes.apply(std::move(node_change));
    current.actions.aai_refs = {aai_ref};
    TraceSlot slot;
    slot.current = std::make_shared<const ActiveTrace>(std::move(current));

    const auto refs = collect_cleanup_index_refs(slot);

    ASSERT_EQ(2u, refs.size());
    ASSERT_TRUE(std::find(refs.begin(), refs.end(), node_ref) != refs.end());
    ASSERT_TRUE(std::find(refs.begin(), refs.end(), aai_ref) != refs.end());
}

TEST(TraceInserter, cleanup_of_actionless_trace_unchanged) {
    const TraceStateIndexRef node_ref{
        .index_key = "account:node",
        .member = "trace:node",
        .score = 11,
    };
    ActiveTrace current;
    auto node_change = current.nodes.upsert_nodes({TraceStateNode{
        .key = "node",
        .index_refs = {node_ref},
    }});
    current.nodes.apply(std::move(node_change));
    TraceSlot slot;
    slot.current = std::make_shared<const ActiveTrace>(std::move(current));

    const auto refs = collect_cleanup_index_refs(slot);

    ASSERT_EQ(1u, refs.size());
    ASSERT_EQ(node_ref, refs[0]);
}

TEST(TraceInserter, invalidation_data_failure_retries_exact_cleanup_before_promotion) {
    const TraceStateIndexRef aai_ref{
        .index_key = "_aai:account",
        .member = "trace:action",
        .score = 17,
    };
    auto live = trace_for_cleanup_retry(
        aai_ref, TraceStateFinality::Emulated, 1, 10);
    auto fallback = std::make_shared<const ActiveTrace>(
        trace_for_cleanup_retry(
            aai_ref, TraceStateFinality::Confirmed, 1, 5));
    auto stale_non_full = prepare_confirmed_promotion(
        live, CachedConfirmedTrace{}, "trace", 100, false);
    ASSERT_TRUE(stale_non_full.is_ok());
    auto stale_non_full_prepared = stale_non_full.move_as_ok();
    ASSERT_TRUE(stale_non_full_prepared.redis.fields_to_set.empty());
    ASSERT_TRUE(stale_non_full_prepared.redis.indexes_to_add.empty());

    TraceSlot slot;
    slot.current = std::make_shared<const ActiveTrace>(std::move(live));
    slot.cleanup_mode = TraceCleanupMode::Invalidation;
    auto cleanup = build_cleanup_batch(
        "trace", slot, TraceCleanupMode::Invalidation);
    slot.in_flight.emplace(InFlightWork{
        .kind = InFlightKind::Cleanup,
        .cleanup_mode = TraceCleanupMode::Invalidation,
    });
    slot.cleanup_mode = TraceCleanupMode::Retention;

    bool promotion_completed = false;
    bool regular_completed = false;
    slot.queued.emplace_back(
        promotion_request(std::move(fallback), promotion_completed));
    slot.queued.emplace_back(regular_insert_request(regular_completed));
    std::size_t queued_updates = 1;

    ASSERT_TRUE(terminal_cleanup_pending(slot));
    ASSERT_TRUE(slot.in_flight->cleanup_mode ==
                TraceCleanupMode::Invalidation);
    ASSERT_TRUE(slot.cleanup_mode == TraceCleanupMode::Retention);

    PipelineFailureMock redis;
    redis.hashes["trace"].emplace(kActionsField, "actions");
    redis.indexes[aai_ref.index_key].insert(aai_ref.member);
    redis.fail_next("ZREM");
    auto status = execute_redis_write(
        cleanup,
        [&] { redis.execute_data(cleanup); },
        [&] { redis.execute_publications(cleanup); });
    ASSERT_TRUE(status.is_error());
    ASSERT_TRUE(redis.has_member(aai_ref));
    ASSERT_TRUE(redis.hashes.count("trace") == 0);

    auto in_flight = std::move(*slot.in_flight);
    slot.in_flight.reset();
    remember_failed_cleanup(
        slot, std::move(cleanup), in_flight.cleanup_mode);
    ASSERT_TRUE(slot.cleanup_retry.has_value());
    ASSERT_TRUE(slot.cleanup_retry->mode ==
                TraceCleanupMode::Invalidation);
    ASSERT_TRUE(!slot.cleanup_retry->batch.data_phase_completed);
    ASSERT_EQ(1u, slot.cleanup_retry->batch.plans.size());
    ASSERT_TRUE(slot.cleanup_retry->batch.plans[0].indexes_to_remove ==
                std::vector<TraceStateIndexRef>{aai_ref});
    ASSERT_TRUE(slot.cleanup_retry->batch.plans[0].erase_trace);
    ASSERT_EQ(std::string("root"),
              slot.cleanup_retry->batch.plans[0].raw_external_message_hash);
    ASSERT_EQ(1u,
              slot.cleanup_retry->batch.plans[0].publications.size());
    ASSERT_TRUE(slot.deadline);
    ASSERT_TRUE(!trace_work_is_ready(slot));
    ASSERT_TRUE(!promotion_completed);
    ASSERT_TRUE(!regular_completed);

    slot.cleanup_retry->ready = true;
    TraceCleanupMode retry_mode;
    auto retry = take_ready_cleanup_retry(slot, retry_mode);
    ASSERT_TRUE(retry_mode == TraceCleanupMode::Invalidation);
    status = execute_redis_write(
        retry,
        [&] { redis.execute_data(retry); },
        [&] { redis.execute_publications(retry); });
    ASSERT_TRUE(status.is_ok());
    ASSERT_TRUE(!redis.has_member(aai_ref));
    ASSERT_EQ(2u, redis.count_commands("ZREM"));
    ASSERT_EQ(2u, redis.count_commands("UNLINK"));

    slot.queued = resolve_completed_cleanup_queue(
        slot, queued_updates, retry_mode);
    ASSERT_EQ(0u, queued_updates);
    ASSERT_TRUE(regular_completed);
    ASSERT_TRUE(!promotion_completed);
    ASSERT_EQ(1u, slot.queued.size());
    ASSERT_TRUE(std::holds_alternative<PromoteConfirmedRequest>(
        slot.queued.front()));

    auto surviving = std::move(slot.queued);
    TraceSlot replacement;
    replacement.queued = std::move(surviving);
    slot = std::move(replacement);
    auto prepared = prepare_surviving_full_promotion(slot);
    ASSERT_TRUE(promotion_completed);
    ASSERT_TRUE(std::find(
                    prepared.redis.indexes_to_add.begin(),
                    prepared.redis.indexes_to_add.end(),
                    aai_ref) != prepared.redis.indexes_to_add.end());
    ASSERT_TRUE(redis_field(prepared.redis, kActionsField).has_value());

    RedisWriteBatch promotion_batch{
        .plans = {std::move(prepared.redis)},
    };
    status = execute_redis_write(
        promotion_batch,
        [&] { redis.execute_data(promotion_batch); },
        [&] { redis.execute_publications(promotion_batch); });
    ASSERT_TRUE(status.is_ok());
    ASSERT_TRUE(redis.hashes.count("trace") != 0);
    ASSERT_TRUE(redis.hashes["trace"].count(kActionsField) != 0);
    ASSERT_TRUE(redis.has_member(aai_ref));
}

TEST(TraceInserter, invalidation_publish_failure_retries_only_publication_before_promotion) {
    const TraceStateIndexRef aai_ref{
        .index_key = "_aai:account",
        .member = "trace:action",
        .score = 17,
    };
    auto live = trace_for_cleanup_retry(
        aai_ref, TraceStateFinality::Finalized, 1, 10);
    auto fallback = std::make_shared<const ActiveTrace>(
        trace_for_cleanup_retry(
            aai_ref, TraceStateFinality::Finalized, 1, 5));

    auto non_full = prepare_confirmed_promotion(
        live, CachedConfirmedTrace{}, "trace", 100, false);
    ASSERT_TRUE(non_full.is_ok());
    auto non_full_prepared = non_full.move_as_ok();
    ASSERT_TRUE(redis_field(
                    non_full_prepared.redis, kActionsField).has_value());
    ASSERT_TRUE(std::find(
                    non_full_prepared.redis.indexes_to_add.begin(),
                    non_full_prepared.redis.indexes_to_add.end(),
                    aai_ref) == non_full_prepared.redis.indexes_to_add.end());

    TraceSlot slot;
    slot.current = std::make_shared<const ActiveTrace>(std::move(live));
    auto cleanup = build_cleanup_batch(
        "trace", slot, TraceCleanupMode::Invalidation);
    slot.in_flight.emplace(InFlightWork{
        .kind = InFlightKind::Cleanup,
        .cleanup_mode = TraceCleanupMode::Invalidation,
    });
    slot.cleanup_mode = TraceCleanupMode::Retention;
    bool promotion_completed = false;
    slot.queued.emplace_back(
        promotion_request(std::move(fallback), promotion_completed));

    PipelineFailureMock redis;
    redis.hashes["trace"].emplace(kActionsField, "actions");
    redis.indexes[aai_ref.index_key].insert(aai_ref.member);
    redis.fail_next("PUBLISH");
    auto status = execute_redis_write(
        cleanup,
        [&] { redis.execute_data(cleanup); },
        [&] { redis.execute_publications(cleanup); });
    ASSERT_TRUE(status.is_error());
    ASSERT_TRUE(cleanup.data_phase_completed);
    ASSERT_TRUE(!redis.has_member(aai_ref));
    ASSERT_TRUE(redis.hashes.count("trace") == 0);
    ASSERT_EQ(1u, redis.count_commands("ZREM"));
    ASSERT_EQ(1u, redis.count_commands("UNLINK"));

    auto in_flight = std::move(*slot.in_flight);
    slot.in_flight.reset();
    remember_failed_cleanup(
        slot, std::move(cleanup), in_flight.cleanup_mode);
    ASSERT_TRUE(!trace_work_is_ready(slot));
    ASSERT_TRUE(!promotion_completed);
    ASSERT_TRUE(slot.cleanup_retry->mode ==
                TraceCleanupMode::Invalidation);
    ASSERT_TRUE(slot.cleanup_retry->batch.data_phase_completed);
    ASSERT_TRUE(slot.cleanup_retry->batch.plans[0].indexes_to_remove ==
                std::vector<TraceStateIndexRef>{aai_ref});

    slot.cleanup_retry->ready = true;
    TraceCleanupMode retry_mode;
    auto retry = take_ready_cleanup_retry(slot, retry_mode);
    status = execute_redis_write(
        retry,
        [&] { redis.execute_data(retry); },
        [&] { redis.execute_publications(retry); });
    ASSERT_TRUE(status.is_ok());
    ASSERT_EQ(1u, redis.count_commands("ZREM"));
    ASSERT_EQ(1u, redis.count_commands("UNLINK"));
    ASSERT_EQ(2u, redis.count_commands("PUBLISH"));

    std::size_t queued_updates = 0;
    slot.queued = resolve_completed_cleanup_queue(
        slot, queued_updates, retry_mode);
    auto surviving = std::move(slot.queued);
    TraceSlot replacement;
    replacement.queued = std::move(surviving);
    slot = std::move(replacement);
    auto prepared = prepare_surviving_full_promotion(slot);
    ASSERT_TRUE(promotion_completed);
    ASSERT_TRUE(std::find(
                    prepared.redis.indexes_to_add.begin(),
                    prepared.redis.indexes_to_add.end(),
                    aai_ref) != prepared.redis.indexes_to_add.end());
    ASSERT_TRUE(redis_field(prepared.redis, kActionsField).has_value());

    RedisWriteBatch promotion_batch{
        .plans = {std::move(prepared.redis)},
    };
    status = execute_redis_write(
        promotion_batch,
        [&] { redis.execute_data(promotion_batch); },
        [&] { redis.execute_publications(promotion_batch); });
    ASSERT_TRUE(status.is_ok());
    ASSERT_TRUE(redis.hashes.count("trace") != 0);
    ASSERT_TRUE(redis.hashes["trace"].count(kActionsField) != 0);
    ASSERT_TRUE(redis.has_member(aai_ref));
}
