# W12 — mch ↔ ton-abi bridge design (T12.1)

Authoring wave: W12. Branch `ton-abi`. Bridge lives in `ton-mch-engine` (links
`ton-abi`; dep direction mch → ton-abi only — ton-abi stays mch-free forever,
plan decision 5). This doc RECONCILES plan §0.16 (written in the interpreter era)
against the W11.5 amendment (interpreter RETIRED, end state = pure codegen), then
specifies the trampoline, the AbiValue→mch::Value adapter, the duplicate-key
policy, the error taxonomy, the test plan, and the gate list.

Read first: docs/ton-abi-plan.md §0.16 + W11/W11.5 wave log; ton-abi/W11-SHAPE-SPEC.md;
ton-abi/testdata/ERROR_MAPPING.md (W11.5 re-home); ton-abi/src/AbiValue.h;
ton-mch-engine/src/{MsgParse.h,MsgParse.cpp,Value.h}; ton-mch-engine/src/AbiGenSupport
-equivalent ton-abi/src/AbiGenSupport.h; docs/mch-cpp-plan.md §1 (gate set).

---

## 1. Reconciliation table — §0.16 (interpreter era) vs W11.5 (pure codegen)

§0.16 assumed runtime ABI loading (parse an ABI JSON at startup, walk it with an
interpreter, parsers close over `(AbiModel*, ty_idx)` → needed `std::function`, an
`abi_bridge_init(path)` API, a committed ABI-artifact dir, CMake install changes).
W11.5 deleted the interpreter. Parsing is now **committed generated structs**
(ton-abi-gen output) compiled into the binary; all dispatch is baked at generation
time. That collapses most of §0.16.

| §0.16 item | Disposition | Why |
|---|---|---|
| `MsgParserFn` → `std::function` migration | **DROP** | Trampoline `abi_parse_body<T>` is a free function template; `&abi_parse_body<T>` is an ordinary function pointer whose type is EXACTLY `td::Result<mch::Value>(*)(const td::Ref<vm::Cell>&)` = the current `MsgParserFn`. No closure state exists (dispatch is baked into `T::from_slice`), so nothing needs `std::function`. `MsgParserFn` UNCHANGED; both existing tables (hand map literal, `generated_message_parsers()` vector) structurally UNCHANGED. |
| Bridge owns loaded `AbiModel`s (static, immortal) | **DROP** | No `AbiModel` at runtime. Generated code never links AbiKernel/AbiLoader (W11-SHAPE-SPEC §1); the kernel ran at generation time only. Nothing to own. |
| ABI-artifact dir passed via config/CLI (like the IR artifact) | **DROP** | No runtime artifact. Parsers are compile-time symbols. |
| `abi_bridge_init(path) -> td::Status`, called before workers; init failure aborts startup | **DROP** | No init phase exists. Registration is compile-time static rows merged into the Meyer's-static map on first use, exactly like `generated_message_parsers()`. |
| Access-before-init = hard error AT the accessor (pre-init callers: BuildDriver validate, --surface, ParseDump) | **DROP the hazard** | With no init phase the Meyer's-static map is safe to read at any time. Pre-init callers are a non-issue. `validate_registries()` (below) is an OPTIONAL startup consistency check, NOT a gate on access. |
| CMake install/packaging update (ship ABI dir, not just exe) | **DROP** | No runtime files to ship. |
| Registry identity = **NAME**, never opcode | **KEEP** | mch selects parsers by registry name from artifact parse stmts; shared prefixes across standard ABIs are legitimate, so opcode dispatch is wrong. |
| Key = `<contract_name>::<decl name \| mangled monomorph>`; inline unions need manifest name | **KEEP** | Same identity rule as W11-SHAPE-SPEC §2 mangling. T12.1 fixtures use plain decl names; manifest-name-for-inline-unions is a T12.2 concern (no inline-union protocol row yet). |
| Duplicate registry keys = startup error | **KEEP, reshaped** | No "startup init" to fail. Realized as (a) `validate_registries() -> td::Status` called from the CLI + the mch-classify embedding entry paths, non-empty dup set = error; (b) defensively, the merge records collisions and `parse_message_body` returns an error for any ambiguous name — because `message_parsers()` returns `const map&` and cannot itself surface an error, and the no-abort rule forbids failing inside it. |
| NO prefix-uniqueness enforcement (prefix check lives inside each parser) | **KEEP** | Each `from_slice` checks its own prefix; the bridge never dispatches by opcode. |
| ton-abi stays mch-free; dep direction mch → ton-abi | **KEEP** | ton-abi links nothing from mch. mch-classify links ton-abi. |
| `to_abi_value()` returns `td::Result` | **KEEP** | Trampoline propagates it (custom hooks can fail). |
| Top-level exotic-cell reject | **KEEP, re-homed to bridge** | W11.5 re-home (ERROR_MAPPING.md): the interpreter's top-level exotic reject moved to the bridge. The trampoline rejects exotic root cells at open, abort-safe, returning an error result (never an abort). |

Net: §0.16's runtime-loading machinery is entirely dropped; only the NAME identity,
dup-key-is-error, no-prefix-uniqueness, dep-direction, exotic-reject, and
Result-propagation invariants survive — and they get simpler.

---

## 2. Trampoline

`src/AbiBridge.h` (mch-classify lib), header — template so instantiation happens in
the including TU against a concrete generated struct:

```cpp
namespace mch {

// AbiValue -> mch::Value adapter (impl in AbiBridge.cpp). Total: never errors.
Value abi_value_to_mch(const ton_abi::AbiValue &v);

// Parse a message body as the generated ABI type T, return an mch::Value.
// T is a ton-abi-gen struct (ton_abi::gen::<contract>::<Decl>) exposing:
//   static td::Result<T> from_slice(vm::CellSlice&);   // checks its own prefix
//   td::Result<ton_abi::AbiValue> to_abi_value() const;
// &abi_parse_body<T> has type MsgParserFn exactly -> registrable as a raw row.
template <class T>
td::Result<Value> abi_parse_body(const td::Ref<vm::Cell> &body) {
  // (1) open root cell, abort-safe, REJECT exotic (top-level exotic reject is
  //     the bridge's per W11.5 re-home). Exact abort-safe open helper verified
  //     in phase 2 (candidate: load_cell_slice_special / Cell::load_cell +
  //     special_type() check); on special -> td::Status::Error("exotic cell").
  //     on null body -> error.
  // (2) TRY_RESULT(v, T::from_slice(cs));            // parse failure propagates
  // (3) TRY_RESULT(av, v.to_abi_value());            // custom-hook failure props
  // (4) return abi_value_to_mch(av);                 // total, no error
}

}  // namespace mch
```

No consumed-fully check (matches the reference and existing MsgParse parsers;
extra bits after the body are the caller's concern). No opcode SKIP (unlike the
pytoniq hand parsers which skip 32 unchecked bits): ABI `from_slice` CHECKS the
struct prefix — this is the tolk-faithful universe, intentional per plan.

`&abi_parse_body<T>` is a plain function pointer → `MsgParserFn` unchanged; the
instantiation is COMDAT-folded across TUs (one definition). Confirmed against
the current signature `using MsgParserFn = td::Result<Value>(*)(const td::Ref<vm::Cell>&);`.

---

## 3. AbiValue → mch::Value adapter (`abi_value_to_mch`)

TOLK-FAITHFUL conventions (@ton/core / AbiValue semantics), NOT pytoniq. ABI-parsed
bodies form a NEW value universe (plan-locked universe separation): the adapter does
NOT imitate PSlice's `extern:<len>:<val>` strings or the pytoniq None-address→null
convention. It mirrors the documented AbiValue dump shape (AbiValue.h) so the mch
value tree is a faithful in-memory image of `to_abi_value()`'s JSON. Total — every
kind maps, no error path (all trampoline errors come from open/from_slice/to_abi_value).

`AbiValue` is move-only; the adapter reads it by `const&` and recurses by `const&`
(no copies of AbiValue; it builds fresh `mch::Value`s).

| AbiValueKind | payload | mch::Value | Notes |
|---|---|---|---|
| Int | RefInt256 | `make_int(int_v)` | also carries enum values (RefInt256) |
| Bool | bool | `make_bool(bool_v)` | |
| Address (Std) | AbiAddress | `make_account_raw(canon)` | canon = `normalize_raw_address(wc":"+hash.to_hex())`; uppercase raw form, matches AccountId.as_str(). Fallback to the unnormalized string only if normalize returns nullopt (should not happen for a valid Std addr). |
| Address (None) | AbiAddress | `make_account_none()` | present-but-empty address (addr_none from any_address). NOT null — faithful to AccountId(None) vs a null Value. |
| Address (Extern) | AbiAddress | `make_obj({{"bits",make_int(ext_bits)},{"value",make_cell(bits→cell)}})` | No native mch external-address kind and NO new VType (Value.h VType is append-only + needs a clean engine rebuild — avoid). Structured Obj mirrors AbiValue's `{"extern":{bits,value}}` without the pytoniq string. ALTERNATIVE the lead may prefer: REJECT (error result) since no fixture/consumer carries an extern in a body — flagged, Obj chosen as the total default. |
| Cell | Ref<Cell> | `make_cell(cell_v)` | |
| CellOf | inner (unique_ptr) | `make_obj({{"ref", abi_value_to_mch(*inner)}})` | mirrors AbiValue dump `{"ref":inner}`. |
| Bits | Ref<CellSlice> | `make_cell(slice→cell)` | build a cell from the bit-slice (`CellBuilder.append_cellslice(*bits_v).finalize()`) → lossless (bits + refs), root-hash comparable. Avoids an Obj{bits,refs} shape and a new kind. |
| String | std::string | `make_str(string_v)` | RAW bytes into Str (byte-preserving). The lossy-UTF8 decode is a DUMP-side concern (AbiValue::to_json); the adapter does not re-impose it — no consumer yet, raw is lossless. Flagged: expose AbiValue's decoder later if a consumer needs decoded text. |
| List | vector<AbiValue> | `make_list([abi_value_to_mch(x)...])` | arrayOf / lispListOf / tensor / shapedTuple |
| Struct | name + ordered fields | `make_obj([{"$",make_str(name)}] + [{field, abi_value_to_mch(v)}...])` | Obj = attribute access (same shape existing MsgParse parsers return for bodies). `"$"` = struct_name FIRST, then fields in decl order → a consumer can switch on `.$`. |
| Union (has_value_field) | label + inner | `make_obj({{"$",make_str(union_label)},{"value",abi_value_to_mch(*inner)}})` | mirrors AbiValue `{"$":label,"value":...}`. |
| Union (struct-labeled) | — | (never reaches Union kind) | struct-labeled variants ARE their inner Struct (carry own `"$"`, AbiValue.h note) → hit the Struct branch. |
| Void | — | `make_obj({{"$",make_str("void")}})` | mirrors `{"$":"void"}`. |
| Null | — | `Value::null()` | nullLiteral AND addressOpt-absent AND nullable-absent all land here. |
| Map | vector<pair<AbiValue,AbiValue>> | `make_list([ make_list([adapt(k),adapt(v)]) ... ])` | mirrors dump `[[k,v],...]` in wire order. mch Dict keys are std::string only → cannot carry intN/address keys, so List-of-pairs preserves key types losslessly. ALTERNATIVE: Dict with stringified keys if a lookup consumer appears (flagged). |

Rationale for reusing existing VType kinds (no new VType): Value.h VType enum is
append-only and adding a member forces a clean engine-obj rebuild (mch_cpp_value_abi_rebuild
memory) — every kind above maps onto an existing VType (Int/Bool/Account/Cell/Str/
List/Obj/Null). The only non-obvious calls are Extern (Obj), Bits (Cell), Map
(List-of-pairs), all justified above and flagged for the lead.

---

## 4. Registry merge + duplicate-key policy

`generated_message_parsers()` (schema-gen rows) is unchanged. NEW:

```cpp
// AbiBridge.cpp — the ABI rows. EMPTY for T12.1 (no production protocol yet;
// T12.2 adds the first real row). Rows: {"<contract>::<Decl>", &abi_parse_body<T>}.
const std::vector<std::pair<std::string, MsgParserFn>> &abi_message_parsers();
```

`message_parsers()` (MsgParse.cpp) merges THREE sources — hand map, generated rows,
abi rows — into the Meyer's-static map, while RECORDING any key that appears in more
than one source into a static duplicate set:

```cpp
const std::map<std::string, MsgParserFn> &message_parsers() {
  static const auto built = build_message_registry();   // {map, dup_keys}
  return built.map;
}
```

`build_message_registry()` (file-local helper, also unit-exercised) merges rows and
collects collisions. `emplace` is first-source-wins (current behavior); collisions are
NOT silently swallowed — each collided key is pushed to `dup_keys`.

Surfacing the error under the `const map&` + no-abort constraints:
- `td::Status validate_registries();` → OK if `dup_keys` empty, else an error listing
  the ambiguous names. Called from the CLI `main` startup and any mch-classify
  embedding entry point BEFORE processing (advisory startup check; NOT on the access
  path). Chosen over "check at merge" because the merge runs inside the Meyer's-static
  and cannot return status.
- `parse_message_body(name, body)` additionally checks the dup set: an ambiguous name
  returns `"message type <name> is ambiguously registered"` (belt-and-suspenders — a
  process that skipped `validate_registries()` still fails closed rather than dispatching
  to an arbitrary first-source parser).

Both existing source tables stay byte-identical; only `message_parsers()`'s body gains
the third merge + dup tracking. `--surface` / `cpp_surface.json` is UNTOUCHED at T12.1
(abi rows empty → registered set unchanged; surface updates are a T12.2 concern per plan).

---

## 5. Error taxonomy + soft-parse interplay

The engine's `_soft_parse` contract (MsgParse.cpp comment): a parse FAILURE = a clean
`td::Status` error result, on which the engine tries the next alternative / yields null.
Every bridge error is a Result error (never an abort), preserving that contract:

| Condition | Result | Soft-parse effect |
|---|---|---|
| Unknown name | `Error("message type <n> is not registered")` (existing) | mismatch → next alt |
| Ambiguous name (dup across sources) | `Error("... ambiguously registered")` (new, §4) | mismatch → next alt |
| Exotic root cell | `Error("exotic cell ...")` from trampoline open (new, §1) | mismatch → next alt |
| Parse failure (prefix mismatch, truncation, bad address, union no-match) | `td::Status` from `T::from_slice` leaf/dispatch, propagated | mismatch → next alt |
| `to_abi_value` failure (missing custom hook) | `td::Status` propagated | mismatch → next alt |
| Adapter | never fails (total) | — |

`parse_message_body` already wraps the call in try/catch for `vm::VmError` /
`std::exception` → status; the trampoline is fail-closed `td::Result` and should not
throw, but that guard stays as defense (unchanged). No abort path anywhere (engine
no-abort rule).

---

## 6. File / build layout

- `src/AbiBridge.h` (mch-classify) — trampoline template + adapter decl. Includes
  ton-abi `AbiValue.h`, mch `Value.h`, vm cell headers, td Result. NOT AbiKernel/
  AbiLoader (baked dispatch; generated code needs only AbiGenSupport + leaf headers).
- `src/AbiBridge.cpp` (mch-classify) — `abi_value_to_mch` impl; `abi_message_parsers()`
  (empty vector for T12.1).
- `src/MsgParse.cpp` — `build_message_registry()` (merge + dup tracking),
  `validate_registries()`, dup-aware `parse_message_body`. `MsgParse.h` grows
  `validate_registries()` decl (+ a test-visible `build_message_registry` or a
  `registry_duplicate_keys()` accessor for the unit).
- CMake: `mch-classify` links `ton-abi` (`target_link_libraries(mch-classify PUBLIC …
  ton-abi)`) + adds `AbiBridge.cpp`. This is the ONLY prod build change; the ABI rows
  are empty so no generated fixture pair enters the prod lib.

Fixture generated pairs (test-only): the `--abi-bridge-test` mode's TU
(`src/AbiBridgeTest.cpp`, part of the `ton-mch-engine` exe, NOT mch-classify) `#include`s
the chosen `ton-abi/generated/<fixture>_gen.h` and the exe compiles the matching
`ton-abi/generated/<fixture>_gen.cpp` as a test source, with `ton-abi/generated` added to
that exe's include dirs. This keeps fixture parsers OUT of the production registry (they
are UNDER TEST, not shipped) while still exercising the real trampoline+adapter against
real generated structs.

---

## 7. Test plan

mch's test harness is NOT doctest/gtest — the ctest gates are self-contained CLI modes
on the `ton-mch-engine` exe (`--shape-test`, `--msg-vectors`, …) registered via
`add_test`. The bridge follows that pattern:

- NEW mode `--abi-bridge-test` (`src/AbiBridgeTest.cpp`), registered as
  `add_test(NAME mch_abi_bridge COMMAND ton-mch-engine --abi-bridge-test)`. Self-contained:
  builds input cells in C++ (vm::CellBuilder + ton-abi leaf stores, or `T::create(...).store`),
  runs the trampoline + adapter, asserts the resulting `mch::Value` tree via
  `structural_equal` / field walks. No fixture corpus, no Python.
- Fixture pairs registered UNDER TEST (chosen to maximize adapter-kind coverage; exact
  set confirmed in phase 2 against the committed `ton-abi/generated/` pairs):
  - **tolk_counter / lots-of-messages** `IncreaseBy` — simple prefixed struct → Obj{"$",Int,Int}
    (covers prefix check, Struct→Obj, Int).
  - a **lots-of-wrappers** subset — richest fixture: exercises Address(Std)→Account,
    optional-absent→Null, List (arrayOf/tensor), Union→Obj{"$","value"} and struct-labeled
    union→inner-Struct, Bool, nested Struct. Picks the specific decls that cover the most
    adapter branches.
  These are the plan's "tolk_counter + jetton fixtures" intent — the reference fixture set
  has no jetton .tolk, so lots-of-wrappers stands in as the rich-coverage fixture. NOT
  production rows (T12.2 is the first real protocol, a separate task).
- Adapter branch coverage asserted: Int, Bool, Address Std→Account (uppercase raw),
  Null (absent optional), List, Struct→Obj (with `"$"`), Union both forms, Void, Cell.
  Extern/Bits/Map covered by hand-built AbiValue → adapter unit asserts if no fixture
  decl reaches them (adapter is exercised directly, not only via from_slice).
- Registry-policy units (in the same mode, plain asserts):
  - dup key: feed `build_message_registry`-style merge two rows with the same key →
    `validate_registries()` (or the dup accessor) reports it; `parse_message_body` on that
    name returns the ambiguous error.
  - unknown name: `parse_message_body("Nope", cell)` → not-registered error (existing).
  - exotic cell: trampoline on a special/exotic root → error result (not abort).

---

## 8. Gate list (phase-2 acceptance)

All must hold; STOP-and-report on any red rather than tweak-toward-green.

- (a) mch gate set (docs/mch-cpp-plan.md §1) stays green — built via
  `ton-mch-engine\twins\build_mch.bat` (PowerShell, never Bash), gate twins run from
  `indexer/` with venv `S:\Dev\H-dump\tonscan-indexator\venv0` python. Load-bearing:
  classify diff 0 (196 actions), actions diff 0 (196 rows), match diff 0, parse-dump
  diff 0, schema/prod diff 0, over 147 fixtures. Bridge is additive (empty prod rows) →
  these MUST be unchanged.
- (b) ton-abi ctest 2/2 stays green (build dir `cmake-build-debug-visual-studio/ton-abi`):
  `ton_abi_smoke` (doctest 162 cases) + `ton_abi_gen_check` (--check byte-repro). Bridge
  touches nothing in ton-abi → untouched.
- (c) new `mch_abi_bridge` ctest mode green (§7).
- (d) existing mch ctest modes green (`ctest -R mch`, 5/5 → 6/6 with the new mode);
  `mch_codegen_selftest` + `--check` byte-repro untouched.

---

## 9. Open items to confirm in phase 2 (not blockers)

1. Exact abort-safe exotic-open helper + special-type check API (candidates:
   `vm::load_cell_slice_special`, `Cell::load_cell().is_special`).
2. Final fixture-pair selection from committed `ton-abi/generated/` (coverage-driven).
3. Whether `build_message_registry` is exposed via MsgParse.h for the unit or the unit
   drives a smaller merge helper (keep the exposed surface minimal).

## 10. Deltas vs plan §0.16 (summary for the ack)

DROPPED (W11.5 pure-codegen consequence, refuted with evidence in §1): std::function
migration, AbiModel runtime ownership, ABI-artifact dir, `abi_bridge_init`, access-before-init
accessor guard, CMake install change. KEPT: NAME identity + key format, dup-key-is-error
(reshaped to validate_registries + parse_message_body dup-check under the const-map&/no-abort
constraints), no-prefix-uniqueness, dep direction, top-level exotic reject (re-homed to
trampoline), Result propagation. `MsgParserFn` and both existing parser tables stay
structurally UNCHANGED.
