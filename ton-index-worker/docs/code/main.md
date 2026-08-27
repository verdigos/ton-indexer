---
module: main
source: ton-index-worker/ton-mch-index/src/main.cpp
source: ton-index-worker/ton-trace-emulator/src/main.cpp
source-commit: 06d4c50
---

# main

main covers the two executable entry points of the lane. ton-mch-index is a one-shot batch tool: it prepares the classify engine fatally early via make_engine_prep, parses db/workdir/out and a masterchain seqno range, then drives DbScanner, TraceAssembler, ParseManager and BatchRunner on a td::actor scheduler over that range and exits with the runner's code. ton-trace-emulator is the long-running production counterpart: it parses its own option set, prepares the engine only when in-process classification is enabled, flushes the pending Redis database before any actor can write, and runs DbScanner, TraceProcessor and TraceEmulatorScheduler on a scheduler until shutdown. Both validate options by hand after parsing and exit non-zero on missing or contradictory flags, and both perform no classification work on the main thread itself.

## operator()

`(lambda at /home/verdigos/dev/ton-indexer-private/ton-index-worker/ton-mch-index/src/main.cpp:45:12) operator()(std::uint32_t & dst, const char * name) const`

- decl: `ton-index-worker/ton-mch-index/src/main.cpp:44`
- callers: main (ton-index-worker/ton-mch-index/src/main.cpp:61); main (ton-index-worker/ton-mch-index/src/main.cpp:62); main (ton-index-worker/ton-mch-index/src/main.cpp:64)

Lambda factory whose returned closure parses an option value into a 32-bit unsigned destination.

The closure calls std::stoul on the value and truncates the result to 32 bits, writing it through the captured destination pointer; any throw becomes a bad value error naming the captured option. It backs the from, to and prewarm checked options of ton-mch-index, so non-numeric input fails option parsing rather than falling back to a default. Values that parse but exceed 32 bits are silently truncated — there is no range check beyond what stoul itself enforces.

## parse_positive_seconds

`td::Status parse_positive_seconds(td::Slice value, const char * option, double & destination)`

- decl: `ton-index-worker/ton-trace-emulator/src/main.cpp:21`
- callers: main (ton-index-worker/ton-trace-emulator/src/main.cpp:105); main (ton-index-worker/ton-trace-emulator/src/main.cpp:115); main (ton-index-worker/ton-trace-emulator/src/main.cpp:124); main (ton-index-worker/ton-trace-emulator/src/main.cpp:130)

Parses a positive finite seconds value for a duration option into a double destination.

Delegates to std::stod, turning any throw into an error naming the option as not a number, then rejects non-finite or non-positive results with a must be positive error carrying the same option name. On success the parsed double is stored and OK returned. The four trace-retention TTL options of ton-trace-emulator route through it as checked options, so a bad value aborts option parsing before any actor starts.
