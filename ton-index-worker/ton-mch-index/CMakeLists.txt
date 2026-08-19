cmake_minimum_required(VERSION 3.16)

# Batch CLI: read a masterchain seqno range from a node RocksDB the way the
# indexer does, assemble traces, classify with the mch engine, write actions
# to a JSONL file. No postgres, no scheduler.
add_executable(ton-mch-index
    src/main.cpp
)

target_compile_features(ton-mch-index PRIVATE cxx_std_20)
target_link_libraries(ton-mch-index
    tondb-scanner
    mch-classify
    mch-emu
)
target_link_options(ton-mch-index PUBLIC -rdynamic)

install(TARGETS ton-mch-index RUNTIME DESTINATION bin)
