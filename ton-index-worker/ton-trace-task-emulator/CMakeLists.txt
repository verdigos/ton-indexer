cmake_minimum_required(VERSION 3.16)

add_executable(ton-trace-task-emulator
    src/main.cpp
    src/TraceTaskScheduler.cpp
    src/TaskResultInserter.cpp
    src/RedisListener.cpp
    src/TonConnectProcessor.cpp
    src/EmuTraceViewBuilder.cpp
)
target_include_directories(ton-trace-task-emulator
    PUBLIC ${CMAKE_CURRENT_BINARY_DIR}/../external/redis-plus-plus/src
)

target_compile_features(ton-trace-task-emulator PRIVATE cxx_std_20)
target_link_libraries(ton-trace-task-emulator ton-trace-emulator-core redis++_static msgpack-cxx
    mch-classification-runtime mch-emu-bridge)
target_link_options(ton-trace-task-emulator PUBLIC -rdynamic)
target_compile_definitions(ton-trace-task-emulator PRIVATE -DMSGPACK_USE_DEFINE_MAP)
install(TARGETS ton-trace-task-emulator RUNTIME DESTINATION bin)

add_executable(task-emulator-with-actions-test
    test/WithActionsTest.cpp
    src/EmuTraceViewBuilder.cpp
)
target_include_directories(task-emulator-with-actions-test PRIVATE src)
target_compile_features(task-emulator-with-actions-test PRIVATE cxx_std_20)
target_link_libraries(task-emulator-with-actions-test ton-trace-emulator-core msgpack-cxx
    mch-classification-runtime mch-emu-bridge)
target_compile_definitions(task-emulator-with-actions-test PRIVATE -DMSGPACK_USE_DEFINE_MAP)

enable_testing()
add_test(NAME task_emulator_with_actions COMMAND task-emulator-with-actions-test)
