---
module: HostCocoon
source: ton-index-worker/ton-mch-engine/src/host/HostCocoon.cpp
source-commit: 06d4c50
---

# HostCocoon

Binds Cocoon bridge address expectation to the engine through the single arity-1 host fn cocoon_expected_address, the expectedMyAddress string renderer the registry notes is shared by three matchers. It renders the address a message is expected to target for worker payout, proxy charge, and grant refund flows: an account yields its canonical workchain-prefixed hex string, addr_extern yields a length-then-minimal-hex rendering taken from either MsgParse's extern string sentinel or the ABI bridge's bits-and-cell object, addr_none stays null, and any other value is null. The result is always ok, so an unrecognized shape is a null the matcher compares, never a fault.

## extern_address_string

`Value extern_address_string(const std::string & s)`

- decl: `ton-index-worker/ton-mch-engine/src/host/HostCocoon.cpp:14`
- callers: mch::cocoon_expected_address (ton-index-worker/ton-mch-engine/src/host/HostCocoon.cpp:72)
- calls: `mch::Value::make_str`, `mch::Value::null`

Rewrites MsgParse's extern colon-sentinel string into the length-semicolon-hex form.

A string not starting with extern: or missing a second colon yields null. Otherwise the output is everything after the prefix with the first remaining colon replaced by a semicolon, so both the length and the hex digits pass through verbatim.

## extern_address_object

`Value extern_address_object(const Value & v)`

- decl: `ton-index-worker/ton-mch-engine/src/host/HostCocoon.cpp:32`
- callers: mch::cocoon_expected_address (ton-index-worker/ton-mch-engine/src/host/HostCocoon.cpp:75)
- calls: `mch::Value::field`, `mch::Value::make_str`, `mch::Value::null`

Renders the ABI bridge's bits-and-cell extern object as length-semicolon-hex.

Requires an object whose bits field is a non-negative integer of at most 511 and whose value field is a cell whose slice holds exactly that many bits and no refs; anything else yields null. Nibbles render lowercase, leading zeros are stripped with zero collapsing to a bare 0, and reading straight from the cell keeps widths above the 256-bit big-integer limit exact, which no integer-typed rendering could guarantee.

## cocoon_expected_address

`EvalResult cocoon_expected_address(BuildEnv &, const std::vector<Value> & args)`

- decl: `ton-index-worker/ton-mch-engine/src/host/HostCocoon.cpp:66`
- callers: none found in parsed TU set
- calls: `mch::Value::make_str`, `mch::Value::null`, `mch::extern_address_object`, `mch::extern_address_string`, `mch::rt_ok`

Renders the expected address for a Cocoon matcher from an account, an extern string, or an extern object.

An Account returns its canonical string, or null when addr_none; a Str goes through extern_address_string, an Obj through extern_address_object, and every other type is null. The result is always ok, matching the reject-free contract of a pure renderer.
