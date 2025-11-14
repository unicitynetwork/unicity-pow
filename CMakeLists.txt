cmake_minimum_required(VERSION 3.20)
project(Unicity VERSION 1.0.0 LANGUAGES CXX C)

# =============================================================================
# Build Configuration
# =============================================================================

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Default to Release build
if(NOT CMAKE_BUILD_TYPE OR CMAKE_BUILD_TYPE STREQUAL "")
    set(CMAKE_BUILD_TYPE "Release")
endif()

# =============================================================================
# Sanitizer Support
# =============================================================================
# Usage:
#   cmake -DSANITIZER=thread ..    # ThreadSanitizer (data races)
#   cmake -DSANITIZER=address ..   # AddressSanitizer (memory errors)
#   cmake -DSANITIZER=undefined .. # UndefinedBehaviorSanitizer
#
set(SANITIZER "" CACHE STRING "Enable sanitizer (thread, address, undefined)")

if(SANITIZER)
    message(STATUS "Building with ${SANITIZER} sanitizer")

    if(SANITIZER STREQUAL "thread")
        set(SANITIZER_FLAGS "-fsanitize=thread -g -O1")
        set(SANITIZER_NAME "ThreadSanitizer")
    elseif(SANITIZER STREQUAL "address")
        set(SANITIZER_FLAGS "-fsanitize=address -fsanitize-address-use-after-scope -fno-omit-frame-pointer -g -O1")
        set(SANITIZER_NAME "AddressSanitizer")
    elseif(SANITIZER STREQUAL "undefined")
        set(SANITIZER_FLAGS "-fsanitize=undefined -fno-omit-frame-pointer -g -O1")
        set(SANITIZER_NAME "UndefinedBehaviorSanitizer")
    else()
        message(FATAL_ERROR "Unknown sanitizer: ${SANITIZER}")
    endif()

    message(STATUS "${SANITIZER_NAME} flags: ${SANITIZER_FLAGS}")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${SANITIZER_FLAGS}")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${SANITIZER_FLAGS}")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${SANITIZER_FLAGS}")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} ${SANITIZER_FLAGS}")
endif()

# =============================================================================
# Find System Dependencies
# =============================================================================

find_package(Threads REQUIRED)
find_package(Boost REQUIRED)

# Create interface target for header-only Boost.System if needed
if(NOT TARGET Boost::system)
    add_library(Boost::system INTERFACE IMPORTED)
    set_target_properties(Boost::system PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${Boost_INCLUDE_DIRS}"
    )
endif()

# =============================================================================
# Fetch External Dependencies
# =============================================================================

include(FetchContent)

# RandomX - ASIC-resistant proof-of-work
FetchContent_Declare(
    randomx
    GIT_REPOSITORY https://github.com/unicitynetwork/RandomX.git
    GIT_TAG        origin/master
    GIT_SHALLOW    TRUE
)
set(RANDOMX_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(RANDOMX_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(randomx)

# fmt - Fast formatting library
FetchContent_Declare(
    fmt
    GIT_REPOSITORY https://github.com/fmtlib/fmt.git
    GIT_TAG        10.2.1
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(fmt)

# spdlog - Logging library (uses external fmt)
set(SPDLOG_USE_STD_FORMAT OFF CACHE BOOL "" FORCE)
set(SPDLOG_FMT_EXTERNAL ON CACHE BOOL "" FORCE)
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG        v1.12.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(spdlog)

# nlohmann/json - JSON for Modern C++
FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(json)

# miniupnpc - UPnP NAT traversal
FetchContent_Declare(
    miniupnpc
    GIT_REPOSITORY https://github.com/miniupnp/miniupnp.git
    GIT_TAG        miniupnpc_2_2_5
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  miniupnpc
)
set(UPNPC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(UPNPC_BUILD_SAMPLE OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(miniupnpc)

# =============================================================================
# Core Libraries
# =============================================================================

# Util Library - General utilities (crypto, logging, threading, time, network address validation, string parsing)
add_library(util STATIC
    src/util/files.cpp
    src/util/fs_lock.cpp
    src/util/logging.cpp
    src/util/threadpool.cpp
    src/util/time.cpp
    src/util/sha256.cpp
    src/util/uint.cpp
    src/util/arith_uint256.cpp
    src/util/netaddress.cpp
    src/util/string_parsing.cpp
)

target_include_directories(util PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${Boost_INCLUDE_DIRS}
)

target_compile_definitions(util PRIVATE DISABLE_OPTIMIZED_SHA256)
target_link_libraries(util PUBLIC spdlog::spdlog Boost::system)

# Chain Library - Blockchain logic (consensus, validation, mining)
add_library(chain STATIC
    src/chain/randomx_pow.cpp
    src/chain/block.cpp
    src/chain/pow.cpp
    src/chain/notifications.cpp
    src/chain/block_index.cpp
    src/chain/chain.cpp
    src/chain/block_manager.cpp
    src/chain/chainparams.cpp
    src/chain/validation.cpp
    src/chain/chainstate_manager.cpp
    src/chain/chain_selector.cpp
    src/chain/timedata.cpp
    src/chain/miner.cpp
)

target_include_directories(chain PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

FetchContent_GetProperties(randomx)
if(randomx_POPULATED)
    target_include_directories(chain PUBLIC ${randomx_SOURCE_DIR}/src)
endif()

target_link_libraries(chain PUBLIC
    util
    randomx
    nlohmann_json::nlohmann_json
)

# Network Library - P2P networking and RPC
add_library(network STATIC
    src/network/protocol.cpp
    src/network/connection_types.cpp
    src/network/message.cpp
    src/network/peer.cpp
    src/network/addr_manager.cpp
    src/network/ban_manager.cpp
    src/network/misbehavior_manager.cpp
    src/network/peer_lifecycle_manager.cpp
    src/network/anchor_manager.cpp
    src/network/header_sync_manager.cpp
    src/network/block_relay_manager.cpp
    src/network/blockchain_sync_manager.cpp
    src/network/message_dispatcher.cpp
    src/network/peer_discovery_manager.cpp
    src/network/network_manager.cpp
    src/network/real_transport.cpp
    src/network/nat_manager.cpp
    src/network/notifications.cpp
    src/network/rpc_server.cpp
    src/network/rpc_client.cpp
)

target_include_directories(network PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${Boost_INCLUDE_DIRS}
)

target_link_libraries(network PUBLIC
    chain
    Boost::system
    nlohmann_json::nlohmann_json
    libminiupnpc-static
)

# =============================================================================
# Executables
# =============================================================================

# Main node executable
add_executable(unicityd
    src/main.cpp
    src/application.cpp
)

target_include_directories(unicityd PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${Boost_INCLUDE_DIRS}
)

target_link_libraries(unicityd PRIVATE
    Threads::Threads
    Boost::system
    network
)

set_target_properties(unicityd PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# CLI tool
add_executable(unicity-cli src/cli.cpp)

target_include_directories(unicity-cli PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(unicity-cli PRIVATE network)

set_target_properties(unicity-cli PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# =============================================================================
# Subdirectories
# =============================================================================

add_subdirectory(test)
add_subdirectory(tools/genesis_miner)
