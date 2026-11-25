cmake_minimum_required(VERSION 3.16)

enable_testing()

add_library(ton-dex-prices STATIC
    src/PoolMath.cpp
)

target_include_directories(ton-dex-prices
    PUBLIC
        src
)

target_compile_features(ton-dex-prices PRIVATE cxx_std_20)

target_link_libraries(ton-dex-prices
    PUBLIC
        ton_crypto_core
        tdutils
)

# Embed the schema so deployments need no separate SQL file.
set(DEX_PRICES_SCHEMA ${CMAKE_CURRENT_SOURCE_DIR}/sql/schema.sql)
file(READ ${DEX_PRICES_SCHEMA} DEX_PRICES_SCHEMA_SQL)
configure_file(src/SnapshotWriterSchema.cpp.in ${CMAKE_CURRENT_BINARY_DIR}/SnapshotWriterSchema.cpp @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${DEX_PRICES_SCHEMA})

add_library(ton-dex-prices-writer STATIC
    src/PoolCache.cpp
    src/SnapshotWriter.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/SnapshotWriterSchema.cpp
)
target_compile_features(ton-dex-prices-writer PRIVATE cxx_std_20)
target_link_libraries(ton-dex-prices-writer PUBLIC ton-dex-prices tondb-scanner clickhouse-cpp-lib)

add_executable(ton-dex-prices-backfill backfill/main.cpp)
target_compile_features(ton-dex-prices-backfill PRIVATE cxx_std_20)
target_link_libraries(ton-dex-prices-backfill PRIVATE ton-dex-prices-writer tondb-scanner)
install(TARGETS ton-dex-prices-backfill RUNTIME DESTINATION bin)

add_executable(ton-dex-prices-test tests/dex-prices-test.cpp)
target_link_libraries(ton-dex-prices-test PRIVATE ton-dex-prices-writer tdutils tdactor tondb-scanner smc-envelope ton_block ton_crypto)
add_test(NAME dex_prices COMMAND ton-dex-prices-test)
