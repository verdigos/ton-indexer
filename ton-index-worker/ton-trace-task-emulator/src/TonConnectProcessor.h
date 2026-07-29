#pragma once
#include <td/actor/actor.h>
#include <validator/impl/external-message.hpp>
#include <emulator/transaction-emulator.h>
#include <msgpack.hpp>
#include "IndexData.h"

// INBOUND WIRE FORMAT: MAPS, keyed by field name. Stated explicitly on each
// struct below rather than left to plain MSGPACK_DEFINE, whose meaning depends on
// whether MSGPACK_USE_DEFINE_MAP happens to be defined for whatever target
// compiles this header — array without it, map with it. These payloads are a
// contract with an external producer, so the format belongs in the source, not in
// a build flag.
//
// The producer is ton-emulate-go: it LPushes onto `emulatorqueue`
// (ton-emulate-go/main.go:154, :245, :364) with an encoder configured
// `UseArrayEncodedStructs(false)` and commented "map-encoding" (:60, :66, :232,
// :351). That setting applies to the whole `enc.Encode(env)` call, so the NESTED
// task payloads decoded here are map-encoded too, with keys from the Go
// `msgpack:"..."` tags — which match these member names one for one.
//
// This is not a change of format: the flag was already set PRIVATE on this
// executable (git HEAD, CMakeLists.txt:17) long before it became PUBLIC on
// ton-trace-emulator-core, so these structs have always been maps. Pinning them
// is a no-op today (MSGPACK_DEFINE expands to MSGPACK_DEFINE_MAP under that flag,
// define_decl.hpp:191-197) and immunity tomorrow. Do NOT "restore" these to
// MSGPACK_DEFINE_ARRAY — that would break the producer contract.
struct TonConnectMessage {
    std::string address;
    std::string amount;
    std::optional<std::string> payload;
    std::optional<std::string> state_init;
    // std::optional<std::map<std::string, std::string>> extra_currency;

    MSGPACK_DEFINE_MAP(address, amount, payload, state_init/*, extra_currency*/);
};

struct TonConnectTraceTask {
  std::string id;
  std::string from;
  std::optional<uint64_t> valid_until;
  std::vector<TonConnectMessage> messages;
  bool detect_interfaces;
  bool include_code_data;
  std::optional<uint32_t> mc_block_seqno;

  MSGPACK_DEFINE_MAP(id, from, valid_until, messages, detect_interfaces, include_code_data, mc_block_seqno);
};

struct TraceTask {
  std::string id;
  std::string boc;
  bool ignore_chksig;
  bool detect_interfaces;
  bool include_code_data;
  std::optional<uint32_t> mc_block_seqno;

  MSGPACK_DEFINE_MAP(id, boc, ignore_chksig, detect_interfaces, include_code_data, mc_block_seqno);
};

struct TaskEnvelope {
    std::string     type; // "trace" | "tonconnect"
    msgpack::object task; // hold as generic object; convert after switch
    MSGPACK_DEFINE_MAP(type, task);
};

enum WalletType {
  WALLET_V1R1 = 0,
  WALLET_V1R2 = 1,
  WALLET_V1R3 = 2,
  WALLET_V2R1 = 3,
  WALLET_V2R2 = 4,
  WALLET_V3R1 = 5,
  WALLET_V3R2 = 6,
  WALLET_V4R1 = 7,
  WALLET_V4R2 = 8,
  WALLET_V5R1 = 9,
  WALLET_UNKNOWN = -1
};

class TonConnectProcessor: public td::actor::Actor {
public:
  TonConnectProcessor(TonConnectTraceTask tonconnect_task, const schema::MasterchainBlockDataState& mc_data_state, td::Promise<TraceTask> promise)
    : tonconnect_task_(std::move(tonconnect_task)), mc_data_state_(mc_data_state), promise_(std::move(promise)) {}

  // Convert TonConnect task to regular TraceTask
  void process_tonconnect_task(const TonConnectTraceTask& tonconnect_task, 
                            const schema::MasterchainBlockDataState& mc_data_state);

  void start_up() override;
private:
  TonConnectTraceTask tonconnect_task_;
  schema::MasterchainBlockDataState mc_data_state_;
  td::Promise<TraceTask> promise_;

  void error(td::Status error);
  void got_account_state(schema::AccountState account_state);

  td::Result<td::Ref<vm::Cell>> message_to_cell(const TonConnectMessage& message);

  // Wallet detection and external message composition methods
  WalletType detect_wallet_type(const td::Bits256& code_hash);
  td::Result<td::Ref<vm::Cell>> compose_message_body_v3(const schema::AccountState& account_state);
  td::Result<td::Ref<vm::Cell>> compose_message_body_v4(const schema::AccountState& account_state);
  td::Result<td::Ref<vm::Cell>> compose_message_body_v5(const schema::AccountState& account_state);
  td::Result<td::Ref<vm::Cell>> compose_external_message(td::Ref<vm::Cell> body);

  // Helper method to serialize cell to BOC
  td::Result<std::string> serialize_cell_to_boc(td::Ref<vm::Cell> cell);
};
