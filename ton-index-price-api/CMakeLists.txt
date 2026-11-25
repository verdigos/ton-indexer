add_subdirectory(cmd/dex-prices-aggregator)

set(SOURCE_FILE main.go)
add_go_executable(ton-index-price-api ${SOURCE_FILE})
generate_swagger(ton-index-price-api)
add_dependencies(ton-index-price-api ton-marker)
