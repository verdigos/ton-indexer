#include "TraceInserter.h"
#include "Serializer.hpp"
#include "Statistics.h"
#include <cstdint>
#include <optional>
#include <queue>

namespace {

constexpr bool kPipelineTransactionCommands = false;
constexpr bool kCreateDedicatedTransactionConnection = false;
constexpr const char* kNewTraceChannel = "new_trace"; // for classsifier and others
constexpr const char* kNewPendingTraceChannel = "new_pending_trace"; // for streaming only
// The write-back's own key names. Inlined, not configurable: they were only
// ever parameterised so a phase-2 A/B run could rename them, and there is no
// A/B run.
constexpr const char* kActionsField = "actions";
constexpr const char* kActionsFinalityField = "actions_finality";
constexpr const char* kActionsStateField = "mch_classify_state";
constexpr const char* kAaiPrefix = "_aai:";
constexpr const char* kReferencedAccountsChannel = "referenced_accounts";

std::string finality_name(FinalityState finality) {
    switch (finality) {
        case FinalityState::Emulated:
            return "pending";
        case FinalityState::Confirmed:
            return "confirmed";
        case FinalityState::Finalized:
            return "finalized";
    }
    return "pending";
}

std::string trace_emulator_operation(FinalityState finality) {
    return finality == FinalityState::Emulated ? "emulate" : "read_finalized";
}

} // namespace

class TraceInserter: public td::actor::Actor {
private:
    static constexpr size_t kMaxExistingTraceFields = 1000; // to avoid reading and inserting into unexpectedly large trace
    sw::redis::Transaction transaction_;
    Trace trace_;
    mch::EmuActionPayload actions_;
    td::Promise<td::Unit> promise_;
    std::string trace_key_;
    MeasurementPtr measurement_;
public:
    TraceInserter(sw::redis::Transaction&& transaction, Trace trace, mch::EmuActionPayload actions,
                  td::Promise<td::Unit> promise, const MeasurementPtr& measurement) :
        transaction_(std::move(transaction)), trace_(std::move(trace)), actions_(std::move(actions)),
        promise_(std::move(promise)), measurement_(measurement) {
    }

    void start_up() override {
        td::Timer timer;
        try {
            std::queue<TraceNode*> queue;
        
            std::vector<std::string> tx_keys_to_delete;
            std::vector<std::pair<std::string, std::string>> addr_keys_to_delete;
            std::vector<std::pair<std::string, RedisTraceNode>> flattened_trace;

            trace_key_ = td::base64_encode(trace_.ext_in_msg_hash_norm.as_slice());
            auto existing_fields_count = transaction_.redis().hlen(trace_key_);
            bool has_existing_trace = existing_fields_count > 0;
            measurement_->set_otel_attribute("ton.redis.existing_fields_count", static_cast<std::int64_t>(existing_fields_count));
            if (existing_fields_count > static_cast<long long>(kMaxExistingTraceFields)) {
                LOG(WARNING) << "Skipping trace insert for " << trace_key_
                             << ": existing trace hash is too large (" << existing_fields_count << " fields)";
                promise_.set_value(td::Unit());
                stop();
                return;
            }

            if (trace_.root) {
                queue.push(trace_.root.get());
            }

            while (!queue.empty()) {
                TraceNode* current = queue.front();
                queue.pop();
                if (!current) {
                    continue;
                }

                auto redis_node_r = parse_trace_node(*current);
                if (redis_node_r.is_error()) {
                    promise_.set_error(redis_node_r.move_as_error_prefix("Failed to parse trace node: "));
                    stop();
                    return;
                }
                auto redis_node = redis_node_r.move_as_ok();
                auto msg_hash_key = td::base64_encode(redis_node.transaction.in_msg.value().hash.as_slice());

                // if existing node is more advanced (e.g. it's finalized, but we are inserting emulated), skip subtree
                std::optional<RedisTraceNode> existing_node;
                if (has_existing_trace) {
                    existing_node = load_existing_node(trace_key_, msg_hash_key);
                }
                if (existing_node &&
                    static_cast<int>(existing_node->finality) > static_cast<int>(redis_node.finality)) {
                    continue;
                }

                for (auto& child : current->children) {
                    if (child) {
                        queue.push(child.get());
                    }
                }

                if (has_existing_trace && redis_node.finality != FinalityState::Emulated) {
                    delete_db_subtree(msg_hash_key, tx_keys_to_delete, addr_keys_to_delete,
                                      existing_node ? &*existing_node : nullptr);
                }

                flattened_trace.emplace_back(std::move(msg_hash_key), std::move(redis_node));
            }

            if (flattened_trace.empty()) {
                measurement_->set_otel_attribute("ton.redis.flattened_nodes_count", static_cast<std::int64_t>(0));
                promise_.set_value(td::Unit());
                stop();
                return;
            }
            measurement_->set_otel_attribute("ton.redis.flattened_nodes_count", static_cast<std::int64_t>(flattened_trace.size()));
            measurement_->set_otel_attribute("ton.redis.deleted_nodes_count", static_cast<std::int64_t>(tx_keys_to_delete.size()));

            // delete previously emulated trace
            if (tx_keys_to_delete.size() > 0) {
                transaction_.hdel(trace_key_, tx_keys_to_delete.begin(), tx_keys_to_delete.end());
            }

            for (const auto& [addr, by_addr_key] : addr_keys_to_delete) {
                transaction_.zrem(addr, by_addr_key);
            }

            std::string commited_txs_hashes = trace_key_ + ":";
            bool has_commited_txs = false;
            bool has_pending_txs = false;
            std::vector<std::pair<std::string, std::string>> trace_fields_to_set;
            trace_fields_to_set.reserve(flattened_trace.size() + trace_.interfaces.size() + 4);

            // insert new trace
            for (const auto& entry : flattened_trace) {
                const auto& node_key = entry.first;
                const auto& node = entry.second;
                std::stringstream buffer;
                msgpack::pack(buffer, node);

                trace_fields_to_set.emplace_back(node_key, buffer.str());

                auto addr_raw = std::to_string(node.transaction.account.workchain) + ":" + node.transaction.account.addr.to_hex();
                auto by_addr_key = trace_key_ + ":" + td::base64_encode(node.transaction.in_msg.value().hash.as_slice());
                transaction_.zadd(addr_raw, by_addr_key, node.transaction.lt);

                if (node.finality == FinalityState::Emulated) {
                    has_pending_txs = true;
                } else {
                    if (has_commited_txs) {
                        commited_txs_hashes += ",";
                    }
                    commited_txs_hashes += td::base64_encode(node.transaction.hash.as_slice());
                    has_commited_txs = true;
                }
            }

            // insert interfaces
            for (const auto& [addr, interfaces] : trace_.interfaces) {
                auto interfaces_redis = parse_interfaces(interfaces);
                std::stringstream buffer;
                msgpack::pack(buffer, interfaces_redis);
                auto addr_raw = std::to_string(addr.workchain) + ":" + addr.addr.to_hex();
                trace_fields_to_set.emplace_back(addr_raw, buffer.str());
            }
            
            for (const auto& [addr, account] : trace_.committed_accounts) {
                auto account_redis_r = parse_account(account);
                if (account_redis_r.is_error()) {
                    promise_.set_error(account_redis_r.move_as_error_prefix("Failed to parse account: "));
                    stop();
                    return;
                }
                auto account_redis = account_redis_r.move_as_ok();
                std::stringstream state_buffer;
                msgpack::pack(state_buffer, account_redis);
                auto addr_raw = std::to_string(addr.workchain) + ":" + addr.addr.to_hex();

                std::stringstream if_buffer;
                auto interfaces = trace_.committed_interfaces.find(addr);
                if (interfaces != trace_.committed_interfaces.end()) {
                    auto interfaces_redis = parse_interfaces(interfaces->second);
                    msgpack::pack(if_buffer, interfaces_redis);
                }
                
                const std::string script = R"(
                    local cur = redis.call('HGET', KEYS[1], 'lt')
                    local cur_num = tonumber(cur)
                    local new_num = tonumber(ARGV[1])
                    if (not cur_num) or (new_num > cur_num) then
                        redis.call('HSET', KEYS[1], 'lt', ARGV[1], 'state', ARGV[2], 'interfaces', ARGV[3])
                        redis.call('PUBLISH', 'new_account_state', KEYS[1])
                    end
                    redis.call('EXPIRE', KEYS[1], 60)
                    return 1
                )";
                std::string key_prefix;
                switch (trace_.root->finality_state) {
                    case FinalityState::Finalized:
                        key_prefix = "account_finalized:";
                        break;
                    case FinalityState::Confirmed:
                        key_prefix = "account_confirmed:";
                        break;
                    default:
                        UNREACHABLE();
                }
                auto key = key_prefix + addr_raw;
                transaction_.eval(script, {key}, {std::to_string(account.last_trans_lt_), state_buffer.str(), if_buffer.str()});
            }

            if (has_commited_txs) {
                if (trace_.root->finality_state == FinalityState::Finalized) {
                    transaction_.publish("new_finalized_txs", commited_txs_hashes);
                } else {
                    transaction_.publish("new_confirmed_txs", commited_txs_hashes);
                }
            }

            if (has_commited_txs) {
                transaction_.publish("new_commited_txs", commited_txs_hashes);
            }

            if (trace_.root) {
                auto it = trace_.emulated_accounts.find(trace_.root->address);
                if (it != trace_.emulated_accounts.end()) {
                    const auto& [address, account] = *it;
                    if (account.code.not_null()) {
                        trace_fields_to_set.emplace_back(
                            "root_account_code_hash",
                            td::base64_encode(account.code->get_hash().as_slice()));
                    }
                }
            }
            if (trace_.root) {
                measurement_->set_finality(finality_name(trace_.root->finality_state));
                measurement_->set_operation(trace_emulator_operation(trace_.root->finality_state));
            }
            measurement_->set_out_channel(kNewTraceChannel);
            trace_fields_to_set.emplace_back("root_node", td::base64_encode(trace_.ext_in_msg_hash.as_slice()));
            trace_fields_to_set.emplace_back("depth_limit_exceeded", trace_.tx_limit_exceeded ? "1" : "0");

            // The classifier's rows ride the SAME transaction as the trace they
            // describe, so a pending trace is never visible without them. This
            // sits AFTER both early returns (the >1000-field guard and the
            // empty-flatten short-circuit), which is the correct coupling: a
            // skipped trace skips its actions too.
            append_actions(trace_fields_to_set);

            auto otel_fields = measurement_->otel_propagation_fields();
            trace_fields_to_set.reserve(trace_fields_to_set.size() + otel_fields.size());
            for (const auto& [field, value] : otel_fields) {
                trace_fields_to_set.emplace_back(field, value);
            }
            if (!trace_fields_to_set.empty()) {
                transaction_.hset(trace_key_, trace_fields_to_set.begin(), trace_fields_to_set.end());
            }
            
            transaction_.set("tr_in_msg:" + td::base64_encode(trace_.ext_in_msg_hash.as_slice()), trace_key_);
            transaction_.expire("tr_in_msg:" + td::base64_encode(trace_.ext_in_msg_hash.as_slice()), 600);

            if (has_pending_txs) {
                transaction_.publish(kNewPendingTraceChannel, trace_key_);
            }
            transaction_.publish(kNewTraceChannel, trace_key_);
            
            transaction_.exec();

            promise_.set_value(td::Unit());
        } catch (const vm::VmError &e) {
            promise_.set_error(td::Status::Error("Got VmError while inserting trace: " + std::string(e.get_msg())));
        } catch (const std::exception &e) {
            promise_.set_error(td::Status::Error("Got exception while inserting trace: " + std::string(e.what())));
        }
        g_statistics.record_time(INSERT_TRACE, timer.elapsed() * 1e3);
        stop();
    }
    void delete_db_subtree(std::string key,
                           std::vector<std::string>& tx_keys,
                           std::vector<std::pair<std::string, std::string>>& addr_keys,
                           const RedisTraceNode* cached_node = nullptr) {
        struct Frame {
            std::string key;
            std::optional<RedisTraceNode> node;
            bool expanded{false};
        };

        std::vector<Frame> stack;
        stack.push_back(Frame{std::move(key), std::nullopt, false});
        if (cached_node) {
            stack.back().node = *cached_node;
        }

        while (!stack.empty()) {
            auto& frame = stack.back();

            if (!frame.node.has_value()) {
                auto node_raw = transaction_.redis().hget(trace_key_, frame.key);
                if (!node_raw) {
                    stack.pop_back();
                    continue;
                }
                msgpack::unpacked result;
                msgpack::unpack(result, node_raw->data(), node_raw->size());
                frame.node.emplace();
                result.get().convert(*frame.node);
            }

            if (!frame.expanded) {
                frame.expanded = true;
                std::vector<std::string> child_keys;
                child_keys.reserve(frame.node->transaction.out_msgs.size());
                for (const auto& out_msg : frame.node->transaction.out_msgs) {
                    child_keys.push_back(td::base64_encode(out_msg.hash.as_slice()));
                }
                // Reverse order keeps child processing equivalent to recursive DFS.
                for (auto it = child_keys.rbegin(); it != child_keys.rend(); ++it) {
                    stack.push_back(Frame{std::move(*it), std::nullopt, false});
                }
                continue;
            }

            tx_keys.push_back(frame.key);
            auto addr_raw = std::to_string(frame.node->transaction.account.workchain) + ":" + frame.node->transaction.account.addr.to_hex();
            auto by_addr_key = trace_key_ + ":" + td::base64_encode(frame.node->transaction.in_msg.value().hash.as_slice());
            addr_keys.push_back(std::make_pair(addr_raw, by_addr_key));

            stack.pop_back();
        }
    }

    // Appends the write-back's hash fields and queues its ZADDs and PUBLISHes
    // into the same MULTI as the trace.
    //
    // THE GUARD is the load-bearing part. Emissions of one trace can reach the
    // MULTI out of finality order, and the node blobs survive that only because
    // every node is individually guarded above (:87-95 skips the subtree when
    // the stored node is more advanced). `actions` had no equivalent: a stale
    // emulated emission landing after a finalized one would overwrite finalized
    // actions with emulated ones, silently, while the node blobs in the very
    // same MULTI correctly refused the downgrade — leaving the trace and its
    // actions describing different emissions, which is exactly the invariant
    // the write-back exists to establish.
    //
    // Two independent races produce that, and only one is closable upstream.
    // The classifier-side race (a slower earlier emission answering after a
    // faster later one) is closed by per-trace ordering. The second is not:
    // each emission gets its OWN TraceInserter actor, and those contend for a
    // shared connection pool whose waiters are woken by a bare condition_variable
    // (redis-plus-plus connection_pool.cpp:220-234) — no FIFO, no fairness — so
    // two inserters for one trace can exec() in an order unrelated to their
    // creation order no matter what the classifier did.
    //
    // Scope: the guard covers the whole actions AREA, marker included. Writing
    // mch_classify_state from a stale emission while refusing its actions would
    // leave the hash claiming e.g. "shed_deadline" over a finalized emission's
    // rows — the same disagreement, one field over.
    void append_actions(std::vector<std::pair<std::string, std::string>>& trace_fields_to_set) {
        if (actions_.state == nullptr) {
            // Feature off. Return BEFORE the otel attribute and before the
            // hget, so a build with no --mch-artifact issues exactly the
            // commands it issued before the write-back and pays no extra round
            // trip.
            return;
        }
        measurement_->set_otel_attribute("ton.actions.count",
                                         static_cast<std::int64_t>(actions_.action_count));
        // One more hget on a path that already performs several. The decision
        // itself is mch::actions_write_is_stale, gated hermetically by
        // --actions-msgpack-test.
        auto stored = transaction_.redis().hget(trace_key_, kActionsFinalityField);
        if (mch::actions_write_is_stale(stored, actions_.finality)) {
            LOG(DEBUG) << "skipping stale actions write for " << trace_key_ << ": stored finality "
                       << *stored << " > emission finality " << static_cast<int>(actions_.finality);
            return;
        }

        trace_fields_to_set.emplace_back(kActionsStateField, actions_.state);
        if (actions_.actions_blob.empty()) {
            // Empty-degraded: the marker says why, and the `actions` field stays
            // absent so Go reports Classified:false — which is exactly what a
            // pending trace looks like before it is classified.
            return;
        }
        trace_fields_to_set.emplace_back(kActionsField, actions_.actions_blob);
        trace_fields_to_set.emplace_back(kActionsFinalityField,
                                         std::to_string(static_cast<int>(actions_.finality)));
        for (const auto& [account, member] : actions_.aai) {
            transaction_.zadd(kAaiPrefix + account, member,
                              static_cast<double>(actions_.aai_score));
        }
        // referenced_accounts is load-bearing, not decoration: ton-trace-ttl-tracker
        // subscribes it and uses the accounts it learns to ZREM the trace's members
        // out of _aai:<addr> at expiry, so an unpublished account leaks its index
        // entry forever. The payload format is ALSO a remote-kill surface — the
        // subscriber splits on ';' and indexes [1] with no length check, and there
        // is no recover() in that module, so a payload without a ';' takes the
        // whole ttl-tracker process down. Both halves here are BUILT, never
        // interpolated from anything that could contain a separator: the account is
        // "wc:HEX" and trace_key_ is base64.
        for (const auto& account : actions_.referenced_accounts) {
            transaction_.publish(kReferencedAccountsChannel, account + ";" + trace_key_);
        }
    }

    std::optional<RedisTraceNode> load_existing_node(const std::string& trace_key, const std::string& node_key) {
        auto existing = transaction_.redis().hget(trace_key, node_key);
        if (!existing) {
            return std::nullopt;
        }
        try {
            RedisTraceNode node;
            msgpack::unpacked result;
            msgpack::unpack(result, existing->data(), existing->size());
            result.get().convert(node);
            return node;
        } catch (const std::exception& e) {
            LOG(ERROR) << "Failed to parse existing trace node for " << node_key << ": " << e.what();
            return std::nullopt;
        }
    }
};

void RedisInsertManager::insert(Trace trace, mch::EmuActionPayload actions, td::Promise<td::Unit> promise,
                                MeasurementPtr measurement) {
    td::actor::create_actor<TraceInserter>(
        "TraceInserter",
        redis_.transaction(kPipelineTransactionCommands, kCreateDedicatedTransactionConnection),
        std::move(trace),
        std::move(actions),
        std::move(promise),
        measurement).release();
}
