// --actions-msgpack-test: EM2 W1 writer self-test. See EmuActionSerializeTest.h
// and em2-writeback-design.md §2.
//
// Every assertion is made on the DECODED map, never on the bytes: the parity
// bar for EM2 is decoded-map equality, per key (§2.7 / §11) — msgpack maps are
// order-insensitive to ton-index-go's decoder.
#include "EmuActionSerializeTest.h"

#include "ActionBuild.h"
#include "BlockTree.h"
#include "Value.h"
#include "emu/EmuActionSerialize.h"
#include "emu/EmuTypes.h"  // view_finality, actions_write_is_stale (W5)

#include "td/utils/base64.h"

#include "common/refint.h"
#include "vm/cells/CellBuilder.h"

#include <msgpack.hpp>

#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace mch {

namespace {

int g_fail = 0;

void check(const std::string &name, bool ok) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", name.c_str());
  if (!ok) {
    g_fail++;
  }
}

// ---- decoded-payload inspectors -------------------------------------------
// Owns the bytes AND the unpack zone; msgpack::object points into both.
class Decoded {
 public:
  explicit Decoded(std::string bytes) : bytes_(std::move(bytes)) {
    try {
      handle_ = msgpack::unpack(bytes_.data(), bytes_.size());
      ok_ = true;
    } catch (const std::exception &) {
      ok_ = false;
    }
  }
  bool ok() const { return ok_; }
  std::size_t size() const { return bytes_.size(); }
  const msgpack::object &root() const { return handle_.get(); }

 private:
  std::string bytes_;
  msgpack::object_handle handle_;
  bool ok_{false};
};

const msgpack::object *at(const msgpack::object &map, const char *key) {
  if (map.type != msgpack::type::MAP) {
    return nullptr;
  }
  const std::size_t n = std::strlen(key);
  for (std::uint32_t i = 0; i < map.via.map.size; i++) {
    const msgpack::object &k = map.via.map.ptr[i].key;
    if (k.type == msgpack::type::STR && k.via.str.size == n &&
        std::memcmp(k.via.str.ptr, key, n) == 0) {
      return &map.via.map.ptr[i].val;
    }
  }
  return nullptr;
}

bool absent(const msgpack::object &map, const char *key) { return at(map, key) == nullptr; }

const msgpack::object *elem(const msgpack::object *arr, std::uint32_t i) {
  if (arr == nullptr || arr->type != msgpack::type::ARRAY || i >= arr->via.array.size) {
    return nullptr;
  }
  return &arr->via.array.ptr[i];
}

bool is_str(const msgpack::object *o, const std::string &want) {
  return o != nullptr && o->type == msgpack::type::STR && o->via.str.size == want.size() &&
         std::memcmp(o->via.str.ptr, want.data(), want.size()) == 0;
}

// The type test that matters against ton-index-go: an INTEGER where the Go
// field is *uint64/*int64/*int32, a STR where it is *string. A str into a
// numeric field (or the reverse) aborts the whole trace's decode (§2.2).
bool is_uint(const msgpack::object *o, std::uint64_t want) {
  return o != nullptr && o->type == msgpack::type::POSITIVE_INTEGER && o->via.u64 == want;
}

bool is_bool(const msgpack::object *o, bool want) {
  return o != nullptr && o->type == msgpack::type::BOOLEAN && o->via.boolean == want;
}

bool is_nil(const msgpack::object *o) { return o != nullptr && o->type == msgpack::type::NIL; }

bool is_map(const msgpack::object *o, std::uint32_t size) {
  return o != nullptr && o->type == msgpack::type::MAP && o->via.map.size == size;
}

// ---- fixtures --------------------------------------------------------------
const std::string kAddrA = "0:" + std::string(64, 'A');
const std::string kAddrB = "0:" + std::string(64, 'B');
const std::string kTraceKey = "6JLNRxi7yQKUYYSGxSt8SVLZKvC3vHMqBLHqOKTqDGA=";

EmuTraceView make_view(std::vector<EmuFinality> finalities) {
  EmuTraceView view;
  view.trace_id = kTraceKey;
  for (EmuFinality f : finalities) {
    EmuTxRef node;
    node.finality = f;
    view.nodes.push_back(std::move(node));
  }
  return view;
}

Action base_action() {
  Action a;
  a.action_id = "aQpDaKBb2mZmBBHkxTZ9xxJgqhZP4mZBTvUAn1KoLpA=";
  a.type = "ton_transfer";
  a.success = true;
  a.start_lt = 58291746000001;
  a.end_lt = 58291746000003;
  a.start_utime = 1753900000;
  a.end_utime = 1753900002;
  a.mc_seqno_end = 47110815;
  a.tx_hashes = {"txA", "txB"};
  a.accounts = {kAddrA, kAddrB};
  return a;
}

// One serialize + decode + "row 0 is a map" step, shared by most cases.
const msgpack::object *first_row(const Decoded &d) {
  const msgpack::object *row = elem(&d.root(), 0);
  return (row != nullptr && row->type == msgpack::type::MAP) ? row : nullptr;
}

// ---- cases -----------------------------------------------------------------

// "No actions" is the one-byte EMPTY ARRAY, never an empty string:
// EmuActionPayload::actions_blob uses empty to mean "no `actions` field written
// at all" (§7), which is a different state for ton-index-go (Classified=false).
void test_empty_actions() {
  Decoded d(serialize_actions({}, make_view({EmuFinality::finalized})));
  check("empty/one_byte", d.size() == 1);
  check("empty/decodes_to_empty_array", d.ok() && d.root().type == msgpack::type::ARRAY &&
                                            d.root().via.array.size == 0);
}

// The always-written key set, the §2.2 exclusion-set integers, and the §2.3
// deliberate omissions.
void test_fixed_keys() {
  Action a = base_action();
  Decoded d(serialize_actions({a}, make_view({EmuFinality::finalized})));
  const msgpack::object *row = first_row(d);
  check("fixed/one_row", d.ok() && d.root().via.array.size == 1 && row != nullptr);
  if (row == nullptr) {
    return;
  }
  const msgpack::object &m = *row;
  check("fixed/action_id", is_str(at(m, "action_id"), a.action_id));
  check("fixed/type", is_str(at(m, "type"), "ton_transfer"));
  check("fixed/success_bool", is_bool(at(m, "success"), true));
  // Exclusion set (database.py:669): msgpack integers, NOT decimal strings.
  check("fixed/start_lt_int", is_uint(at(m, "start_lt"), 58291746000001ULL));
  check("fixed/end_lt_int", is_uint(at(m, "end_lt"), 58291746000003ULL));
  check("fixed/start_utime_int", is_uint(at(m, "start_utime"), 1753900000));
  check("fixed/end_utime_int", is_uint(at(m, "end_utime"), 1753900002));
  check("fixed/mc_seqno_end_int", is_uint(at(m, "mc_seqno_end"), 47110815));
  check("fixed/finality_int", is_uint(at(m, "finality"), 2));
  // Go only surfaces it when it differs from trace_external_hash (models.go:930).
  check("fixed/trace_external_hash_norm", is_str(at(m, "trace_external_hash_norm"), kTraceKey));

  const msgpack::object *th = at(m, "tx_hashes");
  check("fixed/tx_hashes", th != nullptr && th->type == msgpack::type::ARRAY &&
                               th->via.array.size == 2 && is_str(elem(th, 0), "txA") &&
                               is_str(elem(th, 1), "txB"));
  const msgpack::object *acc = at(m, "accounts");
  check("fixed/accounts", acc != nullptr && acc->type == msgpack::type::ARRAY &&
                              acc->via.array.size == 2 && is_str(elem(acc, 0), kAddrA) &&
                              is_str(elem(acc, 1), kAddrB));

  // §2.3: Go clobbers these right after decode, or has no field at all.
  check("fixed/omits_trace_id", absent(m, "trace_id"));
  check("fixed/omits_trace_external_hash", absent(m, "trace_external_hash"));
  check("fixed/omits_trace_end_lt", absent(m, "trace_end_lt"));
  check("fixed/omits_trace_end_utime", absent(m, "trace_end_utime"));
  check("fixed/omits_trace_mc_seqno_end", absent(m, "trace_mc_seqno_end"));
  check("fixed/omits_trace_start_lt", absent(m, "trace_start_lt"));
  check("fixed/omits_value_extra_currencies", absent(m, "value_extra_currencies"));
  // §2.1: an unfilled field is ABSENT, not present-as-nil.
  check("fixed/omits_unfilled_source", absent(m, "source"));
  check("fixed/omits_unfilled_opcode", absent(m, "opcode"));
  check("fixed/omits_unfilled_composite", absent(m, "ton_transfer_data"));
  check("fixed/exact_key_count", m.via.map.size == 12);
}

// §2.7 presence rule for the four "present but means None" Value shapes, plus
// the address / amount stringification the directional fields rely on.
void test_null_and_stringify() {
  Action a = base_action();
  a.type = "jetton_transfer";
  a.source = Value::make_account_none();  // AccountId(None) -> as_str() is None
  a.destination = Value::make_account_raw(kAddrB);
  a.asset = Value::make_asset_ton();  // TON asset -> None
  a.asset2 = Value::make_asset_jetton(kAddrA);
  a.amount = Value::make_amount_none();  // Amount(None)
  // A jetton amount well above uint64: the 256-bit carrier must survive as a
  // plain base-10 string (§2.2 — no exponent notation).
  a.value = Value::make_amount(
      td::string_to_int256(std::string("340282366920938463463374607431768211456")));
  a.source_secondary = Value::null();
  a.opcode = Value::make_int64(0x0f8a7ea5);

  Decoded d(serialize_actions({a}, make_view({EmuFinality::confirmed})));
  const msgpack::object *row = first_row(d);
  check("null/decodes", d.ok() && row != nullptr);
  if (row == nullptr) {
    return;
  }
  const msgpack::object &m = *row;
  check("null/addr_none_omitted", absent(m, "source"));
  check("null/null_value_omitted", absent(m, "source_secondary"));
  check("null/ton_asset_omitted", absent(m, "asset"));
  check("null/amount_none_omitted", absent(m, "amount"));
  check("null/account_canonical_str", is_str(at(m, "destination"), kAddrB));
  check("null/jetton_asset_str", is_str(at(m, "asset2"), kAddrA));
  check("null/amount_256bit_dec_str",
        is_str(at(m, "value"), "340282366920938463463374607431768211456"));
  // opcode is in the exclusion set: integer, and Python masks it unsigned.
  check("null/opcode_int", is_uint(at(m, "opcode"), 0x0f8a7ea5));
  check("null/finality_min_confirmed", is_uint(at(m, "finality"), 1));
}

// §2.6 / Q1: the four composite fields ton-index-go declares non-string ship as
// msgpack INTEGERS, deliberately diverging from Python (whose top-level-only
// exclusion set stringifies them and drops the whole trace on decode).
void test_composite_int_divergence() {
  Action a = base_action();
  a.type = "jvault_stake";

  Value::Fields nested;
  nested.emplace_back("period", Value::make_int64(11));  // NOT the divergence key
  Value::Fields jv;
  jv.emplace_back("period", Value::make_int64(7776000));
  jv.emplace_back("stake_wallet", Value::make_str(kAddrB));
  jv.emplace_back("nested", Value::make_dict(std::move(nested)));
  a.jvault_stake_data = Value::make_dict(std::move(jv));

  Value::Fields lz;
  lz.emplace_back("src_eid", Value::make_int64(30101));
  lz.emplace_back("dst_eid", Value::make_int64(30343));
  lz.emplace_back("nonce", Value::make_int64(1234567890123LL));
  lz.emplace_back("guid", Value::make_str("0xdeadbeef"));
  a.layerzero_packet_data = Value::make_obj(std::move(lz));

  // Control: a composite with no divergence keys keeps Python's stringify.
  Value::Fields jt;
  jt.emplace_back("query_id", Value::make_int64(99));
  jt.emplace_back("forward_amount", Value::make_amount(td::make_refint(1000000)));
  a.jetton_transfer_data = Value::make_dict(std::move(jt));

  Decoded d(serialize_actions({a}, make_view({EmuFinality::finalized})));
  const msgpack::object *row = first_row(d);
  check("divergence/decodes", d.ok() && row != nullptr);
  if (row == nullptr) {
    return;
  }
  const msgpack::object *jvd = at(*row, "jvault_stake_data");
  check("divergence/jvault_period_int", is_uint(at(*jvd, "period"), 7776000));
  check("divergence/jvault_stake_wallet_str", is_str(at(*jvd, "stake_wallet"), kAddrB));
  // The divergence sets are FLAT: a same-named key one level down is a normal
  // int and stays a decimal string.
  check("divergence/nested_period_str", is_str(at(*at(*jvd, "nested"), "period"), "11"));

  const msgpack::object *lzd = at(*row, "layerzero_packet_data");
  check("divergence/lz_src_eid_int", is_uint(at(*lzd, "src_eid"), 30101));
  check("divergence/lz_dst_eid_int", is_uint(at(*lzd, "dst_eid"), 30343));
  check("divergence/lz_nonce_int", is_uint(at(*lzd, "nonce"), 1234567890123ULL));
  check("divergence/lz_guid_str", is_str(at(*lzd, "guid"), "0xdeadbeef"));

  const msgpack::object *jtd = at(*row, "jetton_transfer_data");
  check("parity/query_id_str", is_str(at(*jtd, "query_id"), "99"));
  check("parity/forward_amount_str", is_str(at(*jtd, "forward_amount"), "1000000"));
}

// The rest of the VType -> msgpack table (§2.7), inside a composite.
void test_value_table() {
  Action a = base_action();
  Value::Fields inner;
  inner.emplace_back("depth", Value::make_int64(2));
  Value::Fields f;
  f.emplace_back("encrypted", Value::make_bool(false));
  f.emplace_back("payload", Value::make_bytes(std::string("\x01\x02\xff", 3)));
  f.emplace_back("content", Value::null());
  f.emplace_back("owner", Value::make_account_none());
  f.emplace_back("items", Value::make_list({Value::make_int64(7), Value::null(),
                                            Value::make_str("tail")}));
  f.emplace_back("nested", Value::make_dict(std::move(inner)));
  a.ton_transfer_data = Value::make_dict(std::move(f));

  Decoded d(serialize_actions({a}, make_view({EmuFinality::emulated})));
  const msgpack::object *row = first_row(d);
  check("table/decodes", d.ok() && row != nullptr);
  if (row == nullptr) {
    return;
  }
  const msgpack::object *ttd = at(*row, "ton_transfer_data");
  check("table/bool_stays_bool", is_bool(at(*ttd, "encrypted"), false));
  check("table/bytes_base64",
        is_str(at(*ttd, "payload"), td::base64_encode(td::Slice("\x01\x02\xff", 3))));
  // The two-level presence rule: a composite is a literal dict on the Python
  // side, so its key set is fixed by the fill and an unfilled field is
  // present-as-NIL. Contrast test_fixed_keys, where an unfilled TOP-LEVEL field
  // is dropped entirely (Python never assigns the attribute).
  check("table/null_key_is_nil", is_nil(at(*ttd, "content")));
  check("table/addr_none_key_is_nil", is_nil(at(*ttd, "owner")));
  const msgpack::object *items = at(*ttd, "items");
  // A List keeps its arity: position is significant, so a null element is nil
  // rather than dropped.
  check("table/list_arity", items != nullptr && items->type == msgpack::type::ARRAY &&
                                items->via.array.size == 3);
  check("table/list_int_str", is_str(elem(items, 0), "7"));
  check("table/list_null_is_nil", is_nil(elem(items, 1)));
  check("table/list_str", is_str(elem(items, 2), "tail"));
  check("table/nested_map", is_map(at(*ttd, "nested"), 1) &&
                                is_str(at(*at(*ttd, "nested"), "depth"), "2"));
  // All six keys survive, nils included — a composite's arity is its fill's.
  check("table/composite_key_count", is_map(ttd, 6));
  check("table/finality_min_emulated", is_uint(at(*row, "finality"), 0));
}

// Float rendering must equal Python's repr(float), not plain shortest
// round-trip: the W2 parity gate found real corpus floats
// (nft_transfer_data.{price,nft_item_index}) where std::to_chars' default
// disagreed — "1037" vs "1037.0" and "1.03e+11" vs "103000000000.0". Every
// expectation below is the literal output of Python 3's repr().
bool python_repr_cases_pass() {
  const std::pair<double, const char *> cases[] = {
      {0.0, "0.0"},           {1037.0, "1037.0"},   {103000000000.0, "103000000000.0"},
      {37970000000.0, "37970000000.0"}, {1.5, "1.5"}, {-2.5, "-2.5"},
      {0.001, "0.001"},       {0.0001, "0.0001"},   {0.00001, "1e-05"},
      {1e15, "1000000000000000.0"},                 {1e16, "1e+16"},
      {1e100, "1e+100"},      {1.5e20, "1.5e+20"},
  };
  bool ok = true;
  for (const auto &[value, want] : cases) {
    Action a = base_action();
    Value::Fields f;
    f.emplace_back("v", Value::make_float(value));
    a.extra = Value::make_dict(std::move(f));
    Decoded d(serialize_actions({a}, make_view({EmuFinality::finalized})));
    const msgpack::object *row = first_row(d);
    if (row == nullptr || !is_str(at(*at(*row, "extra"), "v"), want)) {
      std::printf("     repr mismatch: want %s\n", want);
      ok = false;
    }
  }
  return ok;
}

// §2.7's two "should never reach an Action" shapes: serialized anyway, and
// counted so a future fill that produces one is visible.
void test_counters() {
  Action a = base_action();
  a.amount = Value::make_amount_float(1.5);  // Python Amount(float)
  Value::Fields f;
  f.emplace_back("boc", Value::make_cell(vm::CellBuilder().finalize()));
  f.emplace_back("ratio", Value::make_float(2.5));
  a.extra = Value::make_dict(std::move(f));

  ActionSerializeStats st;
  Decoded d(serialize_actions({a}, make_view({EmuFinality::finalized}), &st));
  const msgpack::object *row = first_row(d);
  check("counters/decodes", d.ok() && row != nullptr);
  if (row == nullptr) {
    return;
  }
  check("counters/floats_counted", st.float_values == 2);  // Amount(float) + Float
  check("counters/cells_counted", st.cell_values == 1);
  check("counters/nothing_unrenderable", st.unrenderable == 0);
  check("counters/amount_float_str", is_str(at(*row, "amount"), "1.5"));
  const msgpack::object *ex = at(*row, "extra");
  check("counters/float_str", is_str(at(*ex, "ratio"), "2.5"));
  check("counters/float_repr_matches_python", python_repr_cases_pass());
  const msgpack::object *boc = at(*ex, "boc");
  // The real BOC bytes, base64'd — NOT the twin-diff root-hash substitution.
  check("counters/cell_is_b64_str", boc != nullptr && boc->type == msgpack::type::STR &&
                                        boc->via.str.size > 0);
}

// Every row of one emission carries the same trace key and finality; the
// payload is a flat array with no trace-level envelope.
void test_multiple_actions() {
  Action a = base_action();
  Action b = base_action();
  b.action_id = "second";
  b.type = "call_contract";
  b.success = false;

  Decoded d(serialize_actions({a, b}, make_view({EmuFinality::confirmed, EmuFinality::emulated,
                                                 EmuFinality::finalized})));
  check("multi/array_of_two",
        d.ok() && d.root().type == msgpack::type::ARRAY && d.root().via.array.size == 2);
  const msgpack::object *r0 = elem(&d.root(), 0);
  const msgpack::object *r1 = elem(&d.root(), 1);
  if (r0 == nullptr || r1 == nullptr) {
    check("multi/rows_present", false);
    return;
  }
  check("multi/row0_type", is_str(at(*r0, "type"), "ton_transfer"));
  check("multi/row1_type", is_str(at(*r1, "type"), "call_contract"));
  check("multi/row1_success_false", is_bool(at(*r1, "success"), false));
  // min over the view's node finalities, matching Python's min(tx.finality).
  check("multi/row0_finality_min", is_uint(at(*r0, "finality"), 0));
  check("multi/row1_finality_min", is_uint(at(*r1, "finality"), 0));
  check("multi/shared_trace_key", is_str(at(*r1, "trace_external_hash_norm"), kTraceKey));
}

// W4: create_unknown_action's row shape, gated hermetically — the corpus twin
// covers it on 50 real fixtures, but that gate needs Python and the fixtures.
void test_unknown_action() {
  Trace trace;
  trace.trace_id = kTraceKey;
  const std::pair<const char *, bool> txs[] = {
      {"hashB", false}, {"hashA", false}, {"hashC", true},  // one aborted
  };
  std::int64_t lt = 400, now = 1753900100, seqno = 47110800;
  for (const auto &[hash, aborted] : txs) {
    auto tx = std::make_unique<Transaction>();
    tx->hash = hash;
    tx->aborted = aborted;
    tx->account = kAddrA;  // deliberately repeated: accounts must dedup
    tx->lt = lt;
    tx->now = now;
    tx->mc_block_seqno = seqno;
    lt -= 100;  // DESCENDING, so min/max cannot come from first/last by luck
    now -= 10;
    seqno -= 5;
    trace.transactions.push_back(std::move(tx));
  }
  trace.transactions[1]->account = kAddrB;
  // The PRODUCTION derivation — SchemaTraceLoader has no trace header and calls
  // exactly this — gated here alongside the row that consumes it.
  fill_trace_aggregates(trace);

  Decoded d(serialize_actions({create_unknown_action(trace)},
                              make_view({EmuFinality::emulated})));
  const msgpack::object *row = first_row(d);
  check("unknown/decodes", d.ok() && row != nullptr);
  if (row == nullptr) {
    return;
  }
  check("unknown/type", is_str(at(*row, "type"), "unknown"));
  // The only action_id in the system not derived from a hash (§10 open item).
  check("unknown/action_id_is_trace_id", is_str(at(*row, "action_id"), kTraceKey));
  // success = not any(tx.aborted) — a whole-trace property, not a block's.
  check("unknown/success_false_when_any_aborted", is_bool(at(*row, "success"), false));
  // fill_trace_aggregates' min/max, carried through onto the row.
  check("unknown/start_lt", is_uint(at(*row, "start_lt"), 200));
  check("unknown/end_lt", is_uint(at(*row, "end_lt"), 400));
  check("unknown/start_utime", is_uint(at(*row, "start_utime"), 1753900080));
  check("unknown/end_utime", is_uint(at(*row, "end_utime"), 1753900100));
  check("unknown/mc_seqno_end", is_uint(at(*row, "mc_seqno_end"), 47110800));
  const msgpack::object *th = at(*row, "tx_hashes");
  check("unknown/tx_hashes_sorted",
        th != nullptr && th->type == msgpack::type::ARRAY && th->via.array.size == 3 &&
            is_str(elem(th, 0), "hashA") && is_str(elem(th, 2), "hashC"));
  const msgpack::object *acc = at(*row, "accounts");
  check("unknown/accounts_deduped",
        acc != nullptr && acc->type == msgpack::type::ARRAY && acc->via.array.size == 2);
  // It sets no directional or composite field at all, so the row is exactly the
  // fixed key set — the same shape Python's create_unknown_action produces.
  check("unknown/no_optional_keys", row->via.map.size == 12);
}

// A view with no nodes degrades to `emulated` — the conservative end, and what
// ton-index-go reads as FinalityStatePending.
void test_bucket_b_fills() {
  BlockArena arena;
  auto row = [&](const char *btype, Value::Fields data) -> Action {
    Block *b = arena.make(btype);
    b->data = Value::make_dict(std::move(data));
    Action a;
    // The whole point of the wave: this used to return false and lose the row.
    check(std::string("bucketB/") + btype + "/builds", build_action(b, a));
    return a;
  };
  auto is_str_v = [](const Value &v, const std::string &want) {
    return v.t == VType::Str && v.str == want;
  };
  auto fld = [](const Value &composite, const char *k) -> Value {
    const Value *v = composite.field(k);
    return v != nullptr ? *v : Value::null();
  };

  {
    Action a = row("renew_dns", {{"source", Value::make_account_raw(kAddrA)},
                                 {"destination", Value::make_account_raw(kAddrB)},
                                 {"collection_address", Value::make_account_raw(kAddrA)}});
    check("bucketB/renew_dns/source", is_str_v(a.source, kAddrA));
    check("bucketB/renew_dns/destination", is_str_v(a.destination, kAddrB));
    check("bucketB/renew_dns/asset", is_str_v(a.asset, kAddrA));
  }
  {
    // No payout yet -> still a REQUEST.
    Action a = row("nominator_pool_withdraw_request",
                   {{"source", Value::make_account_raw(kAddrA)},
                    {"pool", Value::make_account_raw(kAddrB)},
                    {"payout_amount", Value::null()}});
    check("bucketB/nominator/type_request", a.type == "stake_withdrawal_request");
    check("bucketB/nominator/no_amount", a.amount.is_null());
    check("bucketB/nominator/provider", is_str_v(fld(a.staking_data, "provider"), "nominator"));
    // Payout present -> the BTYPE is a request but the ACTION is a withdrawal.
    Action b = row("nominator_pool_withdraw_request",
                   {{"source", Value::make_account_raw(kAddrA)},
                    {"pool", Value::make_account_raw(kAddrB)},
                    {"payout_amount", Value::make_amount(td::make_refint(7000000000LL))}});
    check("bucketB/nominator/type_withdrawal", b.type == "stake_withdrawal");
    check("bucketB/nominator/amount",
          b.amount.t == VType::Int && b.amount.num->to_dec_string() == "7000000000");
  }
  {
    Action a = row("tonstakers_withdraw",
                   {{"stake_holder", Value::make_account_raw(kAddrA)},
                    {"pool", Value::make_account_raw(kAddrB)},
                    {"amount", Value::make_amount(td::make_refint(500))},
                    {"asset", Value::make_account_raw(kAddrB)},
                    {"burnt_nft", Value::make_account_raw(kAddrA)},
                    {"tokens_burnt", Value::make_amount(td::make_refint(499))}});
    check("bucketB/tonstakers_withdraw/type", a.type == "stake_withdrawal");
    check("bucketB/tonstakers_withdraw/source", is_str_v(a.source, kAddrA));
    check("bucketB/tonstakers_withdraw/provider",
          is_str_v(fld(a.staking_data, "provider"), "liquid_staking"));
    check("bucketB/tonstakers_withdraw/ts_nft", is_str_v(fld(a.staking_data, "ts_nft"), kAddrA));
    check("bucketB/tonstakers_withdraw/tokens_burnt",
          fld(a.staking_data, "tokens_burnt").t == VType::Int);
  }
  {
    Action a = row("tonstakers_withdraw_request",
                   {{"source", Value::make_account_raw(kAddrA)},
                    {"tsTON_wallet", Value::make_account_raw(kAddrB)},
                    {"pool", Value::make_account_raw(kAddrB)},
                    {"tokens_burnt", Value::make_amount(td::make_refint(42))},
                    {"asset", Value::make_account_raw(kAddrA)},
                    {"minted_nft", Value::make_account_raw(kAddrB)}});
    check("bucketB/tonstakers_request/type", a.type == "stake_withdrawal_request");
    check("bucketB/tonstakers_request/source_secondary", is_str_v(a.source_secondary, kAddrB));
    check("bucketB/tonstakers_request/ts_nft", is_str_v(fld(a.staking_data, "ts_nft"), kAddrB));
  }
  {
    // Note the field-name shift the bare stonfi-v2 producer uses: amount_1 /
    // sender_wallet_1 going in, amount1 / user_jetton_wallet_1 coming out.
    Action a = row("dex_deposit_liquidity",
                   {{"sender", Value::make_account_raw(kAddrA)},
                    {"pool", Value::make_account_raw(kAddrB)},
                    {"dex", Value::make_str("stonfi_v2")},
                    {"amount_1", Value::make_amount(td::make_refint(10))},
                    {"amount_2", Value::make_amount_none()},
                    {"asset_1", Value::make_asset_jetton(kAddrA)},
                    {"asset_2", Value::make_asset_ton()},
                    {"sender_wallet_1", Value::make_account_raw(kAddrB)},
                    {"sender_wallet_2", Value::make_account_none()},
                    {"lp_tokens_minted", Value::make_amount(td::make_refint(3))}});
    const Value &c = a.dex_deposit_liquidity_data;
    check("bucketB/dex_deposit/dex", is_str_v(fld(c, "dex"), "stonfi_v2"));
    check("bucketB/dex_deposit/amount1", fld(c, "amount1").t == VType::Int);
    check("bucketB/dex_deposit/amount2_null", fld(c, "amount2").is_null());
    check("bucketB/dex_deposit/asset1", is_str_v(fld(c, "asset1"), kAddrA));
    check("bucketB/dex_deposit/asset2_ton_null", fld(c, "asset2").is_null());
    check("bucketB/dex_deposit/wallet1", is_str_v(fld(c, "user_jetton_wallet_1"), kAddrB));
    check("bucketB/dex_deposit/lp_minted", fld(c, "lp_tokens_minted").t == VType::Int);
    check("bucketB/dex_deposit/source", is_str_v(a.source, kAddrA));
  }
  {
    Action a = row("dex_withdraw_liquidity",
                   {{"sender", Value::make_account_raw(kAddrA)},
                    {"sender_wallet", Value::make_account_raw(kAddrB)},
                    {"pool", Value::make_account_raw(kAddrB)},
                    {"asset", Value::make_asset_jetton(kAddrA)},
                    {"dex", Value::make_str("dedust")},
                    {"amount1_out", Value::make_amount(td::make_refint(11))},
                    {"amount2_out", Value::make_amount(td::make_refint(12))},
                    {"asset1_out", Value::make_asset_jetton(kAddrA)},
                    {"asset2_out", Value::make_asset_ton()},
                    {"wallet1", Value::make_account_raw(kAddrA)},
                    {"wallet2", Value::make_account_none()},
                    {"dex_jetton_wallet_1", Value::make_account_raw(kAddrB)},
                    {"dex_wallet_1", Value::make_account_raw(kAddrA)},
                    {"dex_wallet_2", Value::make_account_none()},
                    {"dex_jetton_wallet_2", Value::make_account_none()},
                    {"is_refund", Value::make_bool(true)},
                    {"lp_tokens_burnt", Value::make_amount(td::make_refint(9))}});
    const Value &c = a.dex_withdraw_liquidity_data;
    check("bucketB/dex_withdraw/dex", is_str_v(fld(c, "dex"), "dedust"));
    check("bucketB/dex_withdraw/amount1", fld(c, "amount1").t == VType::Int);
    check("bucketB/dex_withdraw/asset2_out_ton_null", fld(c, "asset2_out").is_null());
    check("bucketB/dex_withdraw/dex_jetton_wallet_1",
          is_str_v(fld(c, "dex_jetton_wallet_1"), kAddrB));
    // is_refund stays a BOOL on the wire, never a stringified int (§2.2).
    check("bucketB/dex_withdraw/is_refund_bool",
          fld(c, "is_refund").t == VType::Bool && fld(c, "is_refund").boolean);
    check("bucketB/dex_withdraw/lp_burnt", fld(c, "lp_tokens_burnt").t == VType::Int);
    check("bucketB/dex_withdraw/source_secondary", is_str_v(a.source_secondary, kAddrB));
  }
  {
    // The former named skip, reversed in W6: leaving it unported would not have
    // avoided a phase-1 diff, it would have manufactured one on live GetGems
    // traffic that the fixture corpus happens not to sample.
    Action a = row("nft_purchase",
                   {{"prev_owner", Value::make_account_raw(kAddrA)},
                    {"new_owner", Value::make_account_raw(kAddrB)},
                    {"nft_address", Value::make_account_raw(kAddrB)},
                    {"collection_address", Value::make_account_raw(kAddrA)},
                    {"query_id", Value::make_int64(77)},
                    {"price", Value::make_amount(td::make_refint(1500000000LL))},
                    {"nft_index", Value::make_int64(1037)},
                    {"forward_amount", Value::make_amount_none()},
                    {"custom_payload", Value::null()},
                    {"forward_payload", Value::null()},
                    {"response_destination", Value::make_account_none()},
                    {"marketplace", Value::make_str("getgems")},
                    {"marketplace_address", Value::make_account_raw(kAddrB)},
                    {"real_prev_owner", Value::make_account_raw(kAddrA)},
                    {"payout_amount", Value::make_amount(td::make_refint(1425000000LL))},
                    {"payout_comment_encrypted", Value::make_bool(false)},
                    {"payout_comment_encoded", Value::make_bool(false)},
                    {"payout_comment", Value::null()}});
    check("bucketB/nft_purchase/source", is_str_v(a.source, kAddrA));
    check("bucketB/nft_purchase/destination", is_str_v(a.destination, kAddrB));
    check("bucketB/nft_purchase/asset_is_collection", is_str_v(a.asset, kAddrA));
    check("bucketB/nft_purchase/asset_secondary_is_item", is_str_v(a.asset_secondary, kAddrB));
    const Value &c = a.nft_transfer_data;
    // The constant that separates a purchase from a plain transfer.
    check("bucketB/nft_purchase/is_purchase",
          fld(c, "is_purchase").t == VType::Bool && fld(c, "is_purchase").boolean);
    check("bucketB/nft_purchase/price", fld(c, "price").t == VType::Int);
    check("bucketB/nft_purchase/nft_item_index", fld(c, "nft_item_index").t == VType::Int);
    check("bucketB/nft_purchase/marketplace", is_str_v(fld(c, "marketplace"), "getgems"));
    check("bucketB/nft_purchase/payout_amount", fld(c, "payout_amount").t == VType::Int);
    // Unlike fill_nft_transfer, this fill writes NO listing data.
    check("bucketB/nft_purchase/no_listing_data", a.nft_listing_data.is_null());
  }
  {
    // change_dns, one branch per DNS record schema. The key set is PYTHON's,
    // not the branch's: value_schema/flags/address/key always, `value` only for
    // a recognised schema.
    auto rec = [](std::vector<std::pair<std::string, Value>> fields) {
      Value::Fields f;
      for (auto &[k, v] : fields) f.emplace_back(k, std::move(v));
      return Value::make_dict(std::move(f));
    };
    // A raw 32-byte key, to prove the hex is LOWERCASE (Python bytes.hex()) —
    // td::buffer_to_hex would render it uppercase and Go would never notice.
    const std::string key_bytes(4, '\xAB');

    Action a = row("change_dns",
                   {{"source", Value::make_account_raw(kAddrA)},
                    {"destination", Value::make_account_raw(kAddrB)},
                    {"collection_address", Value::make_account_raw(kAddrA)},
                    {"key", Value::make_bytes(key_bytes)},
                    {"value", rec({{"schema", Value::make_str("DNSSmcAddress")},
                                   {"address", Value::make_account_raw(kAddrB)},
                                   {"flags", Value::make_int64(3)}})}});
    check("bucketB/change_dns/source", is_str_v(a.source, kAddrA));
    check("bucketB/change_dns/asset", is_str_v(a.asset, kAddrA));
    const Value &c = a.change_dns_record_data;
    check("bucketB/change_dns/key_hex_lowercase", is_str_v(fld(c, "key"), "abababab"));
    check("bucketB/change_dns/value_schema", is_str_v(fld(c, "value_schema"), "DNSSmcAddress"));
    check("bucketB/change_dns/smc_value_is_address", is_str_v(fld(c, "value"), kAddrB));
    // The second, standalone `if` in Python — an elif would have missed it.
    check("bucketB/change_dns/smc_flags", fld(c, "flags").t == VType::Int);
    // Dead on both sides, kept so the decoded map matches Python key for key.
    check("bucketB/change_dns/address_key_present_and_null",
          c.field("address") != nullptr && fld(c, "address").is_null());

    Action t = row("change_dns",
                   {{"source", Value::make_account_raw(kAddrA)},
                    {"destination", Value::make_account_raw(kAddrB)},
                    {"key", Value::make_bytes(key_bytes)},
                    {"value", rec({{"schema", Value::make_str("DNSText")},
                                   {"dns_text", Value::make_str("hello")}})}});
    check("bucketB/change_dns/text_value", is_str_v(fld(t.change_dns_record_data, "value"),
                                                    "hello"));
    check("bucketB/change_dns/text_no_flags",
          fld(t.change_dns_record_data, "flags").is_null());

    Action adnl = row("change_dns",
                      {{"source", Value::make_account_raw(kAddrA)},
                       {"destination", Value::make_account_raw(kAddrB)},
                       {"key", Value::make_bytes(key_bytes)},
                       {"value", rec({{"schema", Value::make_str("DNSAdnlAddress")},
                                      {"address", Value::make_bytes(key_bytes)},
                                      {"flags", Value::make_int64(7)}})}});
    check("bucketB/change_dns/adnl_value_is_hex",
          is_str_v(fld(adnl.change_dns_record_data, "value"), "abababab"));

    // "Unknown" schema: Python never creates the `value` key at all.
    Action u = row("change_dns",
                   {{"source", Value::make_account_raw(kAddrA)},
                    {"destination", Value::make_account_raw(kAddrB)},
                    {"key", Value::make_bytes(key_bytes)},
                    {"value", rec({{"schema", Value::make_str("Unknown")}})}});
    check("bucketB/change_dns/unknown_omits_value",
          u.change_dns_record_data.field("value") == nullptr);
  }
  {
    // delete_dns: the same column, everything but the key nulled.
    Action a = row("delete_dns", {{"source", Value::make_account_raw(kAddrA)},
                                  {"destination", Value::make_account_raw(kAddrB)},
                                  {"collection_address", Value::make_account_raw(kAddrA)},
                                  {"key", Value::make_bytes(std::string(2, '\x0F'))}});
    const Value &c = a.change_dns_record_data;
    check("bucketB/delete_dns/destination", is_str_v(a.destination, kAddrB));
    check("bucketB/delete_dns/key_hex", is_str_v(fld(c, "key"), "0f0f"));
    check("bucketB/delete_dns/value_schema_null", fld(c, "value_schema").is_null());
    check("bucketB/delete_dns/no_value_key", c.field("value") == nullptr);
  }
}

void test_empty_view_finality() {
  Decoded d(serialize_actions({base_action()}, make_view({})));
  const msgpack::object *row = first_row(d);
  check("view/empty_nodes_decodes", d.ok() && row != nullptr);
  if (row != nullptr) {
    check("view/empty_nodes_finality_pending", is_uint(at(*row, "finality"), 0));
  }
}

// --- W5: the two decisions the write-back's INSERT side turns on -------------
//
// Neither is reachable from a fixture, and both are easy to get backwards in a
// way no A/B would catch: view_finality feeds both the `finality` wire key and
// the §4.3a guard, and actions_write_is_stale decides whether a trace's actions
// may be overwritten at all. The insert path itself needs Redis; these two do
// not, so they are gated here rather than left to code review.

EmuTraceView view_of(std::initializer_list<EmuFinality> finalities) {
  EmuTraceView v;
  for (EmuFinality f : finalities) {
    EmuTxRef node;
    node.finality = f;
    v.nodes.push_back(node);
  }
  return v;
}

void test_view_finality() {
  check("view_finality/empty_is_emulated", view_finality(view_of({})) == EmuFinality::emulated);
  check("view_finality/single_finalized",
        view_finality(view_of({EmuFinality::finalized})) == EmuFinality::finalized);
  // The MIN, not the root's and not the max: one emulated node makes the whole
  // emission emulated, which is what stops a partly-emulated re-emission from
  // outranking the finalized emission it would be superseding.
  check("view_finality/min_over_mixed",
        view_finality(view_of({EmuFinality::finalized, EmuFinality::emulated,
                               EmuFinality::confirmed})) == EmuFinality::emulated);
  check("view_finality/min_confirmed",
        view_finality(view_of({EmuFinality::finalized, EmuFinality::confirmed})) ==
            EmuFinality::confirmed);
}

void test_actions_write_guard() {
  const auto emulated = static_cast<std::uint8_t>(EmuFinality::emulated);
  const auto confirmed = static_cast<std::uint8_t>(EmuFinality::confirmed);
  const auto finalized = static_cast<std::uint8_t>(EmuFinality::finalized);

  // Nothing stored: the first emission always writes.
  check("guard/absent_writes", !actions_write_is_stale(std::nullopt, emulated));
  // THE case the guard exists for — a stale emulated emission landing after a
  // finalized one, which the node loop already refuses and `actions` did not.
  check("guard/emulated_over_finalized_skips",
        actions_write_is_stale(std::string("2"), emulated));
  check("guard/emulated_over_confirmed_skips",
        actions_write_is_stale(std::string("1"), emulated));
  // Equal is NOT stale: a re-emission at the same finality must still overwrite,
  // or a trace whose tx set grew keeps the older, shorter action list.
  check("guard/equal_writes", !actions_write_is_stale(std::string("2"), finalized));
  check("guard/advancing_writes", !actions_write_is_stale(std::string("0"), confirmed));
  // Fails OPEN on a corrupt value: failing closed would wedge the actions field
  // permanently on the first bad write.
  check("guard/unparseable_writes", !actions_write_is_stale(std::string("banana"), emulated));
  check("guard/empty_writes", !actions_write_is_stale(std::string(""), emulated));
}

}  // namespace

int run_action_msgpack_test() {
  g_fail = 0;
  test_empty_actions();
  test_fixed_keys();
  test_null_and_stringify();
  test_composite_int_divergence();
  test_value_table();
  test_counters();
  test_multiple_actions();
  test_unknown_action();
  test_bucket_b_fills();
  test_empty_view_finality();
  test_view_finality();
  test_actions_write_guard();
  std::printf("ACTIONS-MSGPACK-TEST %s\n", g_fail == 0 ? "ALL PASS" : "FAILURES");
  return g_fail == 0 ? 0 : 1;
}

}  // namespace mch
