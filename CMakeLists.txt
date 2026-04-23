set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
cmake_minimum_required(VERSION 3.5...3.30)
project(ASOS VERSION 0.2 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Paths to dependencies
set(L3KVG_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../L3KVG)

# Include L3KVG, which transitively includes L3KV, lite3-cpp, libzmq, and cppzmq
add_subdirectory(${L3KVG_DIR} ${CMAKE_CURRENT_BINARY_DIR}/l3kvg)

find_package(Threads REQUIRED)

# Include directories
include_directories(
    include
    ${L3KVG_DIR}/include
    ${L3KVG_DIR}/../L3KV/src
)


# Core ASOS Library
add_library(asos_engine STATIC
    src/Blackboard.cpp
    src/Librarian.cpp
    src/Orchestrator.cpp
    src/Validator.cpp
    src/DeltaEngine.cpp
    src/Monitor.cpp
    src/ApiServer.cpp
)





target_link_libraries(asos_engine PUBLIC
    l3kvg_engine
    lite3-cpp
    lite3client
    libzmq
    cppzmq
)

# ASOS Daemon
add_executable(asos_daemon
    src/main.cpp
)

target_link_libraries(asos_daemon PRIVATE
    asos_engine
    Threads::Threads
    ws2_32
    bcrypt
    iphlpapi
)

# ASOS Verification Tool
add_executable(asos_verify src/main_verify.cpp)
target_link_libraries(asos_verify PRIVATE asos_engine Threads::Threads ws2_32 bcrypt iphlpapi)

# Scratch Test
add_executable(test_orphan_rejection scratch/test_orphan_rejection.cpp)
target_link_libraries(test_orphan_rejection PRIVATE asos_engine Threads::Threads ws2_32 bcrypt iphlpapi)

add_executable(dump_keys scratch/dump_keys.cpp)
target_link_libraries(dump_keys PRIVATE asos_engine Threads::Threads ws2_32 bcrypt iphlpapi)

add_executable(seed_and_wait scratch/seed_and_wait.cpp)
target_link_libraries(seed_and_wait PRIVATE asos_engine Threads::Threads ws2_32 bcrypt iphlpapi)

if(MSVC)
    target_compile_options(asos_verify PRIVATE /bigobj)
    target_compile_options(test_orphan_rejection PRIVATE /bigobj)
    target_compile_options(dump_keys PRIVATE /bigobj)
    target_compile_options(seed_and_wait PRIVATE /bigobj)
endif()


if(MSVC)
    add_compile_definitions(_WIN32_WINNT=0x0A00 NOMINMAX)
    target_compile_options(asos_engine PRIVATE /bigobj)
    target_compile_options(asos_daemon PRIVATE /bigobj)
endif()
