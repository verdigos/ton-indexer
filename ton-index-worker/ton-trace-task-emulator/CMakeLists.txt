cmake_minimum_required(VERSION 3.16)

add_executable(ton-trace-task-emulator
    src/main.cpp
    src/TraceTaskScheduler.cpp
    src/TaskResultInserter.cpp
    src/RedisListener.cpp
    src/TonConnectProcessor.cpp
)
target_include_directories(ton-trace-task-emulator
    PUBLIC ${CMAKE_CURRENT_BINARY_DIR}/../external/redis-plus-plus/src
)

target_compile_features(ton-trace-task-emulator PRIVATE cxx_std_20)
target_link_libraries(ton-trace-task-emulator ton-trace-emulator-core redis++_static msgpack-cxx)
target_link_options(ton-trace-task-emulator PUBLIC -rdynamic)
# MSGPACK_USE_DEFINE_MAP is INHERITED from ton-trace-emulator-core (PUBLIC), which
# is also where TaskResultInserter.cpp gets Serializer.hpp from. Do not re-add it.
# NOTE: TonConnectProcessor.h has MSGPACK_DEFINE structs of its own, and they were
# already compiled in map mode under the flag this line replaces — inheriting it
# keeps them byte-identical.
install(TARGETS ton-trace-task-emulator RUNTIME DESTINATION bin)
