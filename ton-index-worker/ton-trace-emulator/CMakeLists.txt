cmake_minimum_required(VERSION 3.16)

add_library(ton-trace-emulator-core STATIC
    src/TraceEmulator.cpp
    src/ExternalMessageAdmission.cpp
    src/OverlayListener.cpp
    src/BlockEmulator.cpp
    src/TraceInterfaceDetector.cpp
    src/Measurement.cpp
)
target_include_directories(ton-trace-emulator-core
    PUBLIC ../external/ton
    PUBLIC src/
    PUBLIC tondb-scanner/src
)
target_link_directories(ton-trace-emulator-core
    PUBLIC ../external/ton
)

target_link_libraries(
    ton-trace-emulator-core
    PUBLIC
        tondb-scanner
        ton-observability
        overlay
        emulator_static
)
target_compile_features(ton-trace-emulator-core PRIVATE cxx_std_20)

# THE REDIS WIRE FORMAT LIVES HERE. With this defined, MSGPACK_DEFINE emits and
# consumes MAPS keyed by field name; without it, POSITIONAL ARRAYS. Every struct
# Serializer.hpp stores in Redis depends on it, and nothing in the code says so —
# a TU that loses it still compiles, still links, and silently reads and writes a
# different format.
#
# PUBLIC, and on THIS target, so the flag cannot be separated from the header it
# governs: Serializer.hpp is reachable only through this library's PUBLIC `src/`
# include path, so any target able to include it necessarily inherits the
# definition too. It was previously PRIVATE on the ton-trace-emulator EXECUTABLE,
# where it held only by accident of file placement — TraceInserter.cpp is the sole
# includer and happens to sit under add_executable rather than in this library.
# Moving that one file here (the natural refactor, since it is a core insert path)
# would have flipped the format of every blob with no compile error and no gate
# failure. Found while verifying the EM2 write-back; keep it on the library.
target_compile_definitions(ton-trace-emulator-core PUBLIC -DMSGPACK_USE_DEFINE_MAP)

add_executable(ton-trace-emulator
    src/main.cpp
    src/TraceScheduler.cpp
    src/InvalidatedTraceTracker.cpp
    src/TraceInserter.cpp
    src/RedisListener.cpp
    src/Measurement.h
)
target_include_directories(ton-trace-emulator
    PUBLIC ${CMAKE_CURRENT_BINARY_DIR}/../external/redis-plus-plus/src
)

target_compile_features(ton-trace-emulator PRIVATE cxx_std_20)
# MSGPACK_USE_DEFINE_MAP is INHERITED from ton-trace-emulator-core (PUBLIC, see
# above). Do not re-add it here: one authoritative place is the whole point.
target_link_libraries(ton-trace-emulator ton-trace-emulator-core mch-emu-bridge redis++_static msgpack-cxx)
target_link_options(ton-trace-emulator PUBLIC -rdynamic)
install(TARGETS ton-trace-emulator RUNTIME DESTINATION bin)
