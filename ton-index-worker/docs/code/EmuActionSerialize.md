---
module: EmuActionSerialize
source: ton-index-worker/ton-mch-engine/src/emu/EmuActionSerialize.cpp
source: ton-index-worker/ton-mch-engine/src/emu/EmuActionSerialize.h
source-commit: 06d4c50
---

# EmuActionSerialize

Serializes classifier action rows into the uncompressed msgpack array stored on the trace hash's actions field, the last step before the scheduler's Redis writeback. Layout is fixed by contract with the Go consumer: twelve unconditional keys plus a conditional ancestor_type on child rows, then only optional fields that carry data, with the map size precomputed by the same passes that emit so the header always matches the body. Integers wider than signed 64 bits are decimal strings on a field-name allowlist; native integers that do not fit are encoded as nil rather than truncating, and composite maps keep their full key set with unfilled members as nil. Stateless and single-threaded; the only mutable state is the caller-supplied stats counters, and the caller must run it while the view's cell anchor is alive so unexpected cell values can still be serialized.

## class mch::ActionField

- decl: `ton-index-worker/ton-mch-engine/src/emu/EmuActionSerialize.cpp:206`

Internal descriptor pairing a serialized key name with its member pointer in the Action struct.

Instances are generated once from the shared MCH_ACTION_VALUE_FIELDS macro, the same single source that declares the Action struct members, so the table cannot drift from the struct. Lives only inside the optional_fields table, which pack_action walks twice: once to count present keys and once to emit them.

## class mch::ActionSerializeStats

- decl: `ton-index-worker/ton-mch-engine/src/emu/EmuActionSerialize.h:15`

Counters for unexpected Value shapes the serializer had to encode with a fallback.

cell_values counts Cell values preserved as base64 BOC, which Action fields should not carry; unrenderable counts null number refs, null cells, BOC serialization failures, and out-of-int64 natives, all encoded as nil. Pure aggregate owned by the caller; serialize_actions takes an optional pointer and absorbs counts into a discarded local when it is null.

## pack_value

`void pack_value(Packer & pk, const Value & v, bool as_decimal_string, ActionSerializeStats & st)`

- decl: `ton-index-worker/ton-mch-engine/src/emu/EmuActionSerialize.cpp:105`
- callers: mch::pack_fields (ton-index-worker/ton-mch-engine/src/emu/EmuActionSerialize.cpp:112); mch::pack_value (ton-index-worker/ton-mch-engine/src/emu/EmuActionSerialize.cpp:190); mch::pack_action (ton-index-worker/ton-mch-engine/src/emu/EmuActionSerialize.cpp:290)
- calls: `mch::pack_fields`, `mch::pack_int`, `mch::pack_text`, `mch::pack_value`, `mch::td_boc_serialize`

Encodes one Value into msgpack according to its type tag.

Null renders nil, Bool and Str render natively, and Account and Asset render their canonical string or nil for the addr_none and plain-TON shapes. Int honors the caller's decimal-string decision, delegating to pack_int for native encoding; Amount always renders a decimal string because it is value-domain by definition. Bytes become base64 text, Cell values become base64 BOCs with every failure path counted and rendered nil, and List elements inherit the key's decimal-string decision. Dict and Obj recurse through pack_fields in insertion order; Block always renders nil and counts unrenderable because arena blocks are nulled before rows leave the classifier actor. BOC serialization errors never propagate; they become nil plus a counter.


## pack_fields

`void pack_fields(Packer & pk, const Value::Fields & fs, ActionSerializeStats & st)`

- decl: `ton-index-worker/ton-mch-engine/src/emu/EmuActionSerialize.cpp:108`
- callers: mch::pack_value (ton-index-worker/ton-mch-engine/src/emu/EmuActionSerialize.cpp:195)
- calls: `mch::decimal_string_key`, `mch::pack_text`, `mch::pack_value`

Writes a composite map in insertion order, recursing per entry.

Emits a msgpack map header with the exact field count, then each key as text followed by its value through pack_value, with the decimal-string decision taken per key from decimal_string_key. Unfilled members are kept and encode as nil, so composite maps retain their full key set.

## pack_key

`void pack_key(Packer & pk, const char * k)`

- decl: `ton-index-worker/ton-mch-engine/src/emu/EmuActionSerialize.cpp:21`
- callers: mch::pack_action (ton-index-worker/ton-mch-engine/src/emu/EmuActionSerialize.cpp:235); mch::pack_action (ton-index-worker/ton-mch-engine/src/emu/EmuActionSerialize.cpp:237); mch::pack_action (ton-index-worker/ton-mch-engine/src/emu/EmuActionSerialize.cpp:239); mch::pack_action (ton-index-worker/ton-mch-engine/src/emu/EmuActionSerialize.cpp:243); mch::pack_action (ton-index-worker/ton-mch-engine/src/emu/EmuActionSerialize.cpp:248); mch::pack_action (ton-index-worker/ton-mch-engine/src/emu/EmuActionSerialize.cpp:253); mch::pack_action (ton-index-worker/ton-mch-engine/src/emu/EmuActionSerialize.cpp:255); mch::pack_action (ton-index-worker/ton-mch-engine/src/emu/EmuActionSerialize.cpp:257); mch::pack_action (ton-index-worker/ton-mch-engine/src/emu/EmuActionSerialize.cpp:259); mch::pack_action (ton-index-worker/ton-mch-engine/src/emu/EmuActionSerialize.cpp:262); mch::pack_action (ton-index-worker/ton-mch-engine/src/emu/EmuActionSerialize.cpp:266); mch::pack_action (ton-index-worker/ton-mch-engine/src/emu/EmuActionSerialize.cpp:270); mch::pack_action (ton-index-worker/ton-mch-engine/src/emu/EmuActionSerialize.cpp:277); mch::pack_action (ton-index-worker/ton-mch-engine/src/emu/EmuActionSerialize.cpp:289)

Writes a literal field name as msgpack text.

Takes the length once from strlen and writes a str header plus body; only compile-time literals are ever passed.

## optional_fields

`const std::vector<ActionField> & optional_fields()`

- decl: `ton-index-worker/ton-mch-engine/src/emu/EmuActionSerialize.cpp:211`
- callers: mch::pack_action (ton-index-worker/ton-mch-engine/src/emu/EmuActionSerialize.cpp:226)

Returns the singleton descriptor table for the optional Action fields.

Function-local static vector built once by expanding MCH_ACTION_VALUE_FIELDS, pairing each field name stringized with its member pointer. pack_action uses it both to count present keys and to emit them, guaranteeing the two passes see the same fields.

## pack_action

`void pack_action(Packer & pk, const Action & a, const std::string & trace_key, std::int64_t finality, ActionSerializeStats & st)`

- decl: `ton-index-worker/ton-mch-engine/src/emu/EmuActionSerialize.cpp:224`
- callers: mch::serialize_actions (ton-index-worker/ton-mch-engine/src/emu/EmuActionSerialize.cpp:307)
- calls: `mch::decimal_string_key`, `mch::omitted`, `mch::optional_fields`, `mch::pack_key`, `mch::pack_text`, `mch::pack_value`

Serializes one action row as a msgpack map of fixed plus present optional keys.

Writes the twelve unconditional keys — action_id, type, success, tx_hashes and accounts as string arrays, the lt and utime bounds, finality taken from the view rather than the row, the trace key as trace_external_hash_norm, and parent_action_id nil on top-level rows — then ancestor_type only on child rows, then every optional field that omitted does not skip. The map header count comes from a first pass over the same optional table plus the ancestor_type condition, so header and body always agree. Fields the Go consumer does not read are never written, and nothing numeric can land in the two string arrays because their Go element decoding bypasses the flexible decoders.


## pack_text

`void pack_text(Packer & pk, const std::string & s)`

- decl: `ton-index-worker/ton-mch-engine/src/emu/EmuActionSerialize.cpp:27`
- callers: mch::pack_fields (ton-index-worker/ton-mch-engine/src/emu/EmuActionSerialize.cpp:111); mch::pack_value (ton-index-worker/ton-mch-engine/src/emu/EmuActionSerialize.cpp:134); mch::pack_value (ton-index-worker/ton-mch-engine/src/emu/EmuActionSerialize.cpp:147); mch::pack_value (ton-index-worker/ton-mch-engine/src/emu/EmuActionSerialize.cpp:150); mch::pack_value (ton-index-worker/ton-mch-engine/src/emu/EmuActionSerialize.cpp:157); mch::pack_value (ton-index-worker/ton-mch-engine/src/emu/EmuActionSerialize.cpp:164); mch::pack_value (ton-index-worker/ton-mch-engine/src/emu/EmuActionSerialize.cpp:168); mch::pack_value (ton-index-worker/ton-mch-engine/src/emu/EmuActionSerialize.cpp:184); mch::pack_action (ton-index-worker/ton-mch-engine/src/emu/EmuActionSerialize.cpp:236); mch::pack_action (ton-index-worker/ton-mch-engine/src/emu/EmuActionSerialize.cpp:238); mch::pack_action (ton-index-worker/ton-mch-engine/src/emu/EmuActionSerialize.cpp:246); mch::pack_action (ton-index-worker/ton-mch-engine/src/emu/EmuActionSerialize.cpp:251); mch::pack_action (ton-index-worker/ton-mch-engine/src/emu/EmuActionSerialize.cpp:267); mch::pack_action (ton-index-worker/ton-mch-engine/src/emu/EmuActionSerialize.cpp:274); mch::pack_action (ton-index-worker/ton-mch-engine/src/emu/EmuActionSerialize.cpp:280)

Writes a std::string as msgpack text.

Thin wrapper over the str header plus body; every string this serializer emits is far below the 32-bit length limit.

## omitted

`bool omitted(const Value & v)`

- decl: `ton-index-worker/ton-mch-engine/src/emu/EmuActionSerialize.cpp:35`
- callers: mch::pack_action (ton-index-worker/ton-mch-engine/src/emu/EmuActionSerialize.cpp:229); mch::pack_action (ton-index-worker/ton-mch-engine/src/emu/EmuActionSerialize.cpp:286)

Reports whether a top-level optional field is unfilled and should be skipped.

True only for Null and for Amount with a null number, the shape produced by make_amount_none; every other type counts as present. This is what keeps composite maps in the output: they are never skipped at top level, and their unfilled members appear as nil inside instead.

## decimal_string_key

`bool decimal_string_key(const std::string & k)`

- decl: `ton-index-worker/ton-mch-engine/src/emu/EmuActionSerialize.cpp:58`
- callers: mch::pack_fields (ton-index-worker/ton-mch-engine/src/emu/EmuActionSerialize.cpp:112); mch::pack_action (ton-index-worker/ton-mch-engine/src/emu/EmuActionSerialize.cpp:290)

Field-name allowlist whose integers must be encoded as decimal strings.

A static set covering amounts, query ids, swap, liquidity, staking, and fee figures that can exceed signed 64 bits on chain, plus names whose Go side is a string slice, where element decoding bypasses the flexible numeric decoders. Keying by name rather than type keeps bounded integers on other keys in native msgpack encoding, and List elements inherit their key's decision.

## pack_int

`void pack_int(Packer & pk, const Value & v, ActionSerializeStats & st)`

- decl: `ton-index-worker/ton-mch-engine/src/emu/EmuActionSerialize.cpp:96`
- callers: mch::pack_value (ton-index-worker/ton-mch-engine/src/emu/EmuActionSerialize.cpp:136)

Packs an integer natively, or nil when it does not fit signed int64.

A null number ref or one wider than 64 bits increments unrenderable and emits nil rather than truncating, protecting the Go decoder from overflow; otherwise packs the value as int64.

## ActionSerializeStats

`void ActionSerializeStats()`

- decl: `ton-index-worker/ton-mch-engine/src/emu/EmuActionSerialize.h:15`
- callers: mch::test_out_of_range_bounded_int (ton-index-worker/ton-mch-engine/harness/EmuActionSerializeTest.cpp:430); mch::test_counters (ton-index-worker/ton-mch-engine/harness/EmuActionSerializeTest.cpp:800); mch::serialize_actions (ton-index-worker/ton-mch-engine/src/emu/EmuActionSerialize.cpp:298); [mch::EmuClassifierActor::classify (ton-index-worker/ton-mch-engine/src/emu/EmuClassifierActor.cpp:200)](EmuClassifierActor.md#classify); [BatchRunner::classify_trace (ton-index-worker/ton-mch-index/src/BatchRunner.cpp:301)](BatchRunner.md#classify_trace)

Default constructor; both counters start at zero.


## serialize_actions

`std::string serialize_actions(const std::vector<Action> & actions, const EmuTraceView & view, ActionSerializeStats * stats)`

- decl: `ton-index-worker/ton-mch-engine/src/emu/EmuActionSerialize.h:23`
- callers: mch::test_evaa_liquidate_surface (ton-index-worker/ton-mch-engine/harness/EmuActionSerializeTest.cpp:1244); mch::test_empty_view_finality (ton-index-worker/ton-mch-engine/harness/EmuActionSerializeTest.cpp:1264); mch::test_empty_actions (ton-index-worker/ton-mch-engine/harness/EmuActionSerializeTest.cpp:167); mch::test_fixed_keys (ton-index-worker/ton-mch-engine/harness/EmuActionSerializeTest.cpp:176); mch::test_child_row_fields (ton-index-worker/ton-mch-engine/harness/EmuActionSerializeTest.cpp:233); mch::test_null_and_stringify (ton-index-worker/ton-mch-engine/harness/EmuActionSerializeTest.cpp:266); mch::test_natural_types (ton-index-worker/ton-mch-engine/harness/EmuActionSerializeTest.cpp:335); mch::test_string_list_fields (ton-index-worker/ton-mch-engine/harness/EmuActionSerializeTest.cpp:394); mch::test_out_of_range_bounded_int (ton-index-worker/ton-mch-engine/harness/EmuActionSerializeTest.cpp:431); mch::test_determinism (ton-index-worker/ton-mch-engine/harness/EmuActionSerializeTest.cpp:454); mch::test_determinism (ton-index-worker/ton-mch-engine/harness/EmuActionSerializeTest.cpp:455); mch::wire_fixture_bytes (ton-index-worker/ton-mch-engine/harness/EmuActionSerializeTest.cpp:669); mch::test_value_table (ton-index-worker/ton-mch-engine/harness/EmuActionSerializeTest.cpp:762); mch::test_counters (ton-index-worker/ton-mch-engine/harness/EmuActionSerializeTest.cpp:801); mch::test_multiple_actions (ton-index-worker/ton-mch-engine/harness/EmuActionSerializeTest.cpp:825); mch::test_unknown_action (ton-index-worker/ton-mch-engine/harness/EmuActionSerializeTest.cpp:871); [mch::build_payload (ton-index-worker/ton-mch-engine/src/emu/EmuClassifierActor.cpp:105)](EmuClassifierActor.md#build_payload); [BatchRunner::classify_trace (ton-index-worker/ton-mch-index/src/BatchRunner.cpp:302)](BatchRunner.md#classify_trace)
- calls: `mch::ActionSerializeStats::ActionSerializeStats`, `mch::pack_action`, `mch::view_finality`

Serializes the action rows into one uncompressed msgpack array string.

Computes the minimum node finality from the view, writes an array header of the row count, and packs each row with the view's trace id as the trace key. An empty action list yields a one-byte empty-array payload; an empty string would mean no payload. Counts land in the caller's stats when provided and a discarded local otherwise, and the caller must invoke it while the view's cell anchor is alive.
