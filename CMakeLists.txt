cmake_minimum_required(VERSION 3.16)

# App version: derived from git so a release cannot drift from its tag. A
# reachable vX.Y.Z tag wins; otherwise fall back to 1.0.0+g<sha>. Tarball builds
# without .git can force it with -DSQUEEZE2RAOP2_VERSION=....
set(SQUEEZE2RAOP2_VERSION "" CACHE STRING "full version string (default: derived from git)")
set(SQUEEZE2RAOP2_VERSION_CORE "1.0.0")
set(SQUEEZE2RAOP2_GIT_COMMIT "unknown")
find_package(Git QUIET)
if(GIT_FOUND)
  execute_process(
    COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE SQUEEZE2RAOP2_GIT_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  execute_process(
    COMMAND ${GIT_EXECUTABLE} describe --tags --long --dirty --match "v[0-9]*"
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE SQUEEZE2RAOP2_GIT_DESCRIBE
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE SQUEEZE2RAOP2_GIT_DESCRIBE_RC ERROR_QUIET)
  if(SQUEEZE2RAOP2_GIT_DESCRIBE_RC EQUAL 0)
    # vX.Y.Z-<commits>-g<sha>[-dirty]
    string(REGEX MATCH
      "^v([0-9]+\\.[0-9]+\\.[0-9]+)(-([0-9]+)-g([0-9a-f]+))?(-dirty)?$"
      _sq2_desc_match "${SQUEEZE2RAOP2_GIT_DESCRIBE}")
    if(_sq2_desc_match)
      set(SQUEEZE2RAOP2_VERSION_CORE "${CMAKE_MATCH_1}")
      if(CMAKE_MATCH_3 AND NOT CMAKE_MATCH_3 EQUAL 0)
        set(_sq2_version "${CMAKE_MATCH_1}-${CMAKE_MATCH_3}-g${CMAKE_MATCH_4}")
      else()
        set(_sq2_version "${CMAKE_MATCH_1}")
      endif()
      if(SQUEEZE2RAOP2_GIT_DESCRIBE MATCHES "-dirty$")
        set(_sq2_version "${_sq2_version}-dirty")
      endif()
      if(NOT SQUEEZE2RAOP2_VERSION)
        set(SQUEEZE2RAOP2_VERSION "${_sq2_version}")
      endif()
    endif()
  endif()
endif()
if(NOT SQUEEZE2RAOP2_VERSION)
  set(SQUEEZE2RAOP2_VERSION
      "${SQUEEZE2RAOP2_VERSION_CORE}+g${SQUEEZE2RAOP2_GIT_COMMIT}")
endif()

project(squeeze2raop2 VERSION ${SQUEEZE2RAOP2_VERSION_CORE} LANGUAGES C CXX)

message(STATUS "squeeze2raop2 version: ${SQUEEZE2RAOP2_VERSION} (${SQUEEZE2RAOP2_GIT_COMMIT})")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(SQUEEZE2RAOP2_BUILD_TESTS "build squeeze2raop2 unit tests" ON)
option(SQUEEZE2RAOP2_WITH_SENDER "integrate the vendored raop_sender library" ON)
option(SQUEEZE2RAOP2_WITH_AAC "integrate the vendored libxaac AAC decoder" ON)
option(SQUEEZE2RAOP2_DIST "name the release binary <name>-<os>-<arch>" OFF)
option(SQUEEZE2RAOP2_STATIC "link the executable fully static" OFF)

# Release artifact naming: squeeze2raop2-<os>-<arch>, e.g. linux-x86_64. The
# same mapping is reused verbatim for cross builds (aarch64/armv7).
if(SQUEEZE2RAOP2_DIST)
  string(TOLOWER "${CMAKE_SYSTEM_NAME}" SQUEEZE2RAOP2_OS)
  string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" SQUEEZE2RAOP2_ARCH)
  if(SQUEEZE2RAOP2_ARCH MATCHES "^(amd64|x86_64)$")
    set(SQUEEZE2RAOP2_ARCH x86_64)
  elseif(SQUEEZE2RAOP2_ARCH MATCHES "^(arm64|aarch64)$")
    set(SQUEEZE2RAOP2_ARCH aarch64)
  elseif(SQUEEZE2RAOP2_ARCH MATCHES "^armv7")
    set(SQUEEZE2RAOP2_ARCH armv7)
  endif()
  set(SQUEEZE2RAOP2_BIN_NAME "squeeze2raop2-${SQUEEZE2RAOP2_OS}-${SQUEEZE2RAOP2_ARCH}")
else()
  set(SQUEEZE2RAOP2_BIN_NAME "squeeze2raop2")
endif()
# The packaged static build carries a -static suffix so it cannot be confused
# with the dynamically linked release binary.
if(SQUEEZE2RAOP2_DIST AND SQUEEZE2RAOP2_STATIC)
  set(SQUEEZE2RAOP2_BIN_NAME "${SQUEEZE2RAOP2_BIN_NAME}-static")
endif()

# Generated version header; core and the executable both include app/version.h.
configure_file(src/app/version.h.in
               ${CMAKE_BINARY_DIR}/generated/app/version.h @ONLY)

find_package(Threads REQUIRED)

find_program(CLANG_FORMAT_EXE NAMES clang-format)

# First-party warning set (GCC 13+); third_party targets are excluded.
set(SQUEEZE2RAOP2_CXX_WARNINGS
  -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
  -Wold-style-cast -Wcast-align -Wuseless-cast -Wnon-virtual-dtor
  -Woverloaded-virtual -Wnull-dereference -Wdouble-promotion -Wformat=2)

# SQUEEZE2RAOP2_SANITIZERS="address,undefined" (ASan+UBSan) or "thread" (TSan).
# Build in a separate directory; sanitizer builds must not mix.
set(SQUEEZE2RAOP2_SANITIZERS "" CACHE STRING "runtime sanitizers for first-party targets")

if(SQUEEZE2RAOP2_STATIC AND SQUEEZE2RAOP2_SANITIZERS)
  message(FATAL_ERROR
    "SQUEEZE2RAOP2_STATIC cannot be combined with SQUEEZE2RAOP2_SANITIZERS")
endif()
# A fully-static artifact also needs static test binaries: a musl-linked test
# executable will not run on a glibc CI host otherwise. Apply to every
# first-party executable via the global linker flags.
if(SQUEEZE2RAOP2_STATIC)
  set(CMAKE_POSITION_INDEPENDENT_CODE OFF)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static -no-pie")
  else()
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static")
  endif()
endif()

function(squeeze2raop2_target_settings tgt)
  target_compile_options(${tgt} PRIVATE ${SQUEEZE2RAOP2_CXX_WARNINGS})
  if(SQUEEZE2RAOP2_SANITIZERS)
    set(SQUEEZE2RAOP2_SANITIZE_FLAGS -fsanitize=${SQUEEZE2RAOP2_SANITIZERS} -fno-omit-frame-pointer)
    target_compile_options(${tgt} PRIVATE ${SQUEEZE2RAOP2_SANITIZE_FLAGS})
    target_link_options(${tgt} PRIVATE ${SQUEEZE2RAOP2_SANITIZE_FLAGS})
  endif()
endfunction()

if(SQUEEZE2RAOP2_WITH_SENDER)
  set(AIRPLAY_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
  set(AIRPLAY_BUILD_TESTS ON CACHE BOOL "" FORCE)
  set(AIRPLAY_BUILD_QT_HOST OFF CACHE BOOL "" FORCE)
  # mbedtls is vendored as a submodule; hand it to the sender's FetchContent so
  # configure never downloads anything. The sender's declaration still applies
  # when it is built standalone.
  set(FETCHCONTENT_SOURCE_DIR_MBEDTLS
      ${CMAKE_SOURCE_DIR}/third_party/mbedtls CACHE PATH "" FORCE)
  add_subdirectory(third_party/airplay2-sender-cpp EXCLUDE_FROM_ALL)
endif()

# libxaac decoder: built through cmake/libxaac so its directory-scoped global
# options stay isolated. Only the decoder is built (no encoder/testbench).
if(SQUEEZE2RAOP2_WITH_AAC)
  if(NOT EXISTS "${CMAKE_SOURCE_DIR}/third_party/libxaac/CMakeLists.txt")
    message(FATAL_ERROR
      "SQUEEZE2RAOP2_WITH_AAC is ON but third_party/libxaac is missing. "
      "Run: git submodule update --init --recursive")
  endif()
  add_subdirectory(cmake/libxaac EXCLUDE_FROM_ALL)
endif()

add_library(squeeze2raop2_apple_mdns STATIC
  third_party/mDNSResponder/mDNSPosix/mDNSPosix.c
  third_party/mDNSResponder/mDNSPosix/mDNSUNP.c
  third_party/mDNSResponder/mDNSCore/mDNS.c
  third_party/mDNSResponder/mDNSCore/DNSDigest.c
  third_party/mDNSResponder/mDNSCore/uDNS.c
  third_party/mDNSResponder/mDNSCore/DNSCommon.c
  third_party/mDNSResponder/mDNSShared/PlatformCommon.c
  third_party/mDNSResponder/mDNSShared/GenLinkedList.c
  third_party/mDNSResponder/mDNSShared/DebugServices.c
  third_party/mDNSResponder/mDNSShared/mDNSDebug.c
  third_party/mDNSResponder/mDNSShared/utilities/mdns_addr_tailq.c
  third_party/mDNSResponder/mDNSShared/utilities/misc_utilities.c
)
target_compile_options(squeeze2raop2_apple_mdns PRIVATE -w -std=gnu17)
target_compile_definitions(squeeze2raop2_apple_mdns PRIVATE
  _GNU_SOURCE HAVE_IPV6 NOT_HAVE_SA_LEN USES_NETLINK HAVE_LINUX TARGET_OS_LINUX
  POSIX_BUILD MDNS_NO_STRICT=1 NOT_HAVE_DSO
)
target_include_directories(squeeze2raop2_apple_mdns PUBLIC
  third_party/mDNSResponder/mDNSCore
  third_party/mDNSResponder/mDNSPosix
  third_party/mDNSResponder/mDNSShared
  third_party/mDNSResponder/mDNSShared/utilities
)
target_link_libraries(squeeze2raop2_apple_mdns PRIVATE Threads::Threads)

# AAC decoder sources, added to the core only when libxaac is enabled.
set(SQUEEZE2RAOP2_AAC_SOURCES "")
if(SQUEEZE2RAOP2_WITH_AAC)
  set(SQUEEZE2RAOP2_AAC_SOURCES
    src/playback/decoder/aac_decoder.cpp
    src/playback/decoder/mp4_aac_demux.cpp)
endif()

add_library(squeeze2raop2_core STATIC
  # common
  src/common/util.cpp
  src/common/log.cpp
  src/common/net_util.cpp
  # app
  src/app/config.cpp
  src/app/persistence.cpp
  # discovery
  src/discovery/device_registry.cpp
  src/discovery/mdns_embedded.cpp
  # lms
  src/lms/lms_stream.cpp
  src/lms/slimproto.cpp
  src/lms/slimproto_packets.cpp
  src/lms/icy_meta.cpp
  # airplay
  src/airplay/raop_player.cpp
  src/airplay/airplay_output.cpp
  # playback
  src/playback/volume_map.cpp
  src/playback/wav_sink.cpp
  src/playback/ring_telemetry.cpp
  src/playback/stream_counters.cpp
  src/playback/playback_stream.cpp
  src/playback/player_session.cpp
  src/playback/session_manager.cpp
  # decoder
  src/playback/decoder/decoder.cpp
  src/playback/decoder/mp3_decoder.cpp
  src/playback/decoder/pcm_decoder.cpp
  ${SQUEEZE2RAOP2_AAC_SOURCES}
)
target_include_directories(squeeze2raop2_core PUBLIC
  ${CMAKE_BINARY_DIR}/generated src third_party/minimp3)
target_link_libraries(squeeze2raop2_core PUBLIC Threads::Threads squeeze2raop2_apple_mdns)
squeeze2raop2_target_settings(squeeze2raop2_core)
if(SQUEEZE2RAOP2_WITH_SENDER)
  target_link_libraries(squeeze2raop2_core PUBLIC raop_sender)
  target_compile_definitions(squeeze2raop2_core PUBLIC SQUEEZE2RAOP2_WITH_SENDER=1)
endif()
if(SQUEEZE2RAOP2_WITH_AAC)
  target_link_libraries(squeeze2raop2_core PUBLIC libxaacdec)
  target_compile_definitions(squeeze2raop2_core PUBLIC SQUEEZE2RAOP2_WITH_AAC=1)
endif()

add_executable(squeeze2raop2 src/app/main.cpp src/app/bridge.cpp)
set_target_properties(squeeze2raop2 PROPERTIES OUTPUT_NAME "${SQUEEZE2RAOP2_BIN_NAME}")
target_link_libraries(squeeze2raop2 PRIVATE squeeze2raop2_core)
squeeze2raop2_target_settings(squeeze2raop2)

if(SQUEEZE2RAOP2_BUILD_TESTS)
  enable_testing()
  add_subdirectory(test)
endif()

if(CLANG_FORMAT_EXE)
  file(GLOB_RECURSE SQUEEZE2RAOP2_FORMAT_SOURCES
    ${CMAKE_SOURCE_DIR}/src/*.cpp ${CMAKE_SOURCE_DIR}/src/*.h
    ${CMAKE_SOURCE_DIR}/test/*.cpp ${CMAKE_SOURCE_DIR}/test/*.h)
  add_custom_target(format
    COMMAND ${CLANG_FORMAT_EXE} -i ${SQUEEZE2RAOP2_FORMAT_SOURCES}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "clang-format over first-party sources"
    VERBATIM)
endif()

install(TARGETS squeeze2raop2 RUNTIME DESTINATION bin)
