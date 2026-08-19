cmake_minimum_required(VERSION 3.16)

# Batch CLI for the mch read, trace assembly, and classification pipeline.
add_executable(ton-mch-index
    src/main.cpp
    src/BatchRunner.cpp
)

target_compile_features(ton-mch-index PRIVATE cxx_std_20)
target_link_libraries(ton-mch-index
    tondb-scanner
    mch-classify
    mch-emu
)
target_link_options(ton-mch-index PUBLIC -rdynamic)

install(TARGETS ton-mch-index RUNTIME DESTINATION bin)
