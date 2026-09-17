set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
cmake_minimum_required(VERSION 3.5...3.30)
project(agentic-blackboard VERSION 0.4.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_definitions(__GLIBC_USE_C2X_STRTOL=0)

# Paths to dependencies
set(L3KVG_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../l3kvg)

# Include L3KVG, which transitively includes L3KV, lite3-cpp, libzmq, and cppzmq
add_subdirectory(${L3KVG_DIR} ${CMAKE_CURRENT_BINARY_DIR}/l3kvg)

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

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
    src/RdfExporter.cpp
)





target_link_libraries(asos_engine PUBLIC
    l3kvg_engine
    lite3-cpp
    lite3client
    libzmq
    cppzmq
    OpenSSL::Crypto
)

target_compile_definitions(asos_engine PRIVATE __GLIBC_USE_C2X_STRTOL=0)

# ASOS / Agentic Blackboard Daemon
add_executable(agentic-blackboardd
    src/main.cpp
)

target_compile_definitions(agentic-blackboardd PRIVATE __GLIBC_USE_C2X_STRTOL=0)
if(UNIX AND NOT APPLE)
    target_link_options(agentic-blackboardd PRIVATE -static-libstdc++ -static-libgcc)
endif()

target_link_libraries(agentic-blackboardd PRIVATE
    asos_engine
    Threads::Threads
    $<$<BOOL:${WIN32}>:ws2_32>
    $<$<BOOL:${WIN32}>:bcrypt>
    $<$<BOOL:${WIN32}>:iphlpapi>
)

add_executable(asos_daemon
    src/main.cpp
)

target_compile_definitions(asos_daemon PRIVATE __GLIBC_USE_C2X_STRTOL=0)
if(UNIX AND NOT APPLE)
    target_link_options(asos_daemon PRIVATE -static-libstdc++ -static-libgcc)
endif()

target_link_libraries(asos_daemon PRIVATE
    asos_engine
    Threads::Threads
    $<$<BOOL:${WIN32}>:ws2_32>
    $<$<BOOL:${WIN32}>:bcrypt>
    $<$<BOOL:${WIN32}>:iphlpapi>
)

# ASOS Verification Tool
add_executable(asos_verify src/main_verify.cpp)
target_link_libraries(asos_verify PRIVATE asos_engine Threads::Threads $<$<BOOL:${WIN32}>:ws2_32> $<$<BOOL:${WIN32}>:bcrypt> $<$<BOOL:${WIN32}>:iphlpapi>)

# Scratch Test
add_executable(test_orphan_rejection scratch/test_orphan_rejection.cpp)
target_link_libraries(test_orphan_rejection PRIVATE asos_engine Threads::Threads $<$<BOOL:${WIN32}>:ws2_32> $<$<BOOL:${WIN32}>:bcrypt> $<$<BOOL:${WIN32}>:iphlpapi>)

add_executable(dump_keys scratch/dump_keys.cpp)
target_link_libraries(dump_keys PRIVATE asos_engine Threads::Threads $<$<BOOL:${WIN32}>:ws2_32> $<$<BOOL:${WIN32}>:bcrypt> $<$<BOOL:${WIN32}>:iphlpapi>)

add_executable(seed_and_wait scratch/seed_and_wait.cpp)
target_link_libraries(seed_and_wait PRIVATE asos_engine Threads::Threads $<$<BOOL:${WIN32}>:ws2_32> $<$<BOOL:${WIN32}>:bcrypt> $<$<BOOL:${WIN32}>:iphlpapi>)

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
    target_compile_options(agentic-blackboardd PRIVATE /bigobj)
endif()

# Installation rules
install(TARGETS agentic-blackboardd
    DESTINATION /usr/bin
)

install(PROGRAMS src/ab-ctl.py
    DESTINATION /usr/bin
    RENAME ab-ctl
    PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE
)

install(FILES packaging/systemd/agentic-blackboard.service
    DESTINATION /usr/lib/systemd/system
    PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ
)

install(FILES packaging/config/blackboard.conf packaging/config/blackboard.conf.default
    DESTINATION /etc/agentic-blackboard
    PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ
)

install(FILES packaging/limits/99-blackboard.conf
    DESTINATION /etc/security/limits.d
    PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ
)

install(DIRECTORY skills/
    DESTINATION /usr/share/agentic-blackboard/skills
    FILE_PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ
    DIRECTORY_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE
)

# CPack Packaging Configuration
set(CPACK_GENERATOR "RPM")
set(CPACK_PACKAGE_NAME "agentic-blackboard")
set(CPACK_PACKAGE_VERSION "0.4.0")
set(CPACK_PACKAGE_VERSION_MAJOR "0")
set(CPACK_PACKAGE_VERSION_MINOR "4")
set(CPACK_PACKAGE_VERSION_PATCH "0")
set(CPACK_PACKAGE_RELEASE "1.el9")
set(CPACK_PACKAGE_VENDOR "ASOS Team")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Agentic Blackboard Server & Ambient Substrate Daemon")
set(CPACK_PACKAGE_LICENSE "Apache-2.0")

set(CPACK_RPM_PACKAGE_NAME "${CPACK_PACKAGE_NAME}")
set(CPACK_RPM_PACKAGE_VERSION "${CPACK_PACKAGE_VERSION}")
set(CPACK_RPM_PACKAGE_RELEASE "1.el9")
set(CPACK_RPM_PACKAGE_LICENSE "Apache-2.0")
set(CPACK_RPM_PACKAGE_GROUP "Applications/System")
set(CPACK_RPM_PACKAGE_ARCHITECTURE "x86_64")
set(CPACK_RPM_FILE_NAME "agentic-blackboard-0.4.0-1.el9.x86_64.rpm")
set(CPACK_RPM_PACKAGE_AUTOREQPROV "no")
set(CPACK_RPM_PACKAGE_REQUIRES "systemd, python3, zeromq, openssl")

set(CPACK_SET_DESTDIR ON)
set(CPACK_PACKAGING_INSTALL_PREFIX "/")

include(CPack)