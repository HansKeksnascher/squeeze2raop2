cmake_minimum_required(VERSION 3.16)
project(squeeze2raop2 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(SQUEEZE2RAOP2_BUILD_TESTS "build squeeze2raop2 unit tests" ON)
option(SQUEEZE2RAOP2_WITH_SENDER "integrate the vendored raop_sender library" ON)

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
  add_subdirectory(third_party/airplay2-sender-cpp EXCLUDE_FROM_ALL)
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
)
target_include_directories(squeeze2raop2_core PUBLIC src third_party/minimp3)
target_link_libraries(squeeze2raop2_core PUBLIC Threads::Threads squeeze2raop2_apple_mdns)
squeeze2raop2_target_settings(squeeze2raop2_core)
if(SQUEEZE2RAOP2_WITH_SENDER)
  target_link_libraries(squeeze2raop2_core PUBLIC raop_sender)
  target_compile_definitions(squeeze2raop2_core PUBLIC SQUEEZE2RAOP2_WITH_SENDER=1)
endif()

add_executable(squeeze2raop2 src/app/main.cpp src/app/bridge.cpp)
target_link_libraries(squeeze2raop2 PRIVATE squeeze2raop2_core)
squeeze2raop2_target_settings(squeeze2raop2)

if(SQUEEZE2RAOP2_BUILD_TESTS)
  enable_testing()
  add_executable(squeeze2raop2_tests test/test_util.cpp)
  target_link_libraries(squeeze2raop2_tests PRIVATE squeeze2raop2_core)
  squeeze2raop2_target_settings(squeeze2raop2_tests)
  add_test(NAME util_test COMMAND squeeze2raop2_tests)

  add_executable(squeeze2raop2_config_tests test/test_config.cpp)
  target_link_libraries(squeeze2raop2_config_tests PRIVATE squeeze2raop2_core)
  squeeze2raop2_target_settings(squeeze2raop2_config_tests)
  add_test(NAME config_test COMMAND squeeze2raop2_config_tests)

  add_executable(squeeze2raop2_persistence_tests test/test_persistence.cpp)
  target_link_libraries(squeeze2raop2_persistence_tests PRIVATE squeeze2raop2_core)
  squeeze2raop2_target_settings(squeeze2raop2_persistence_tests)
  add_test(NAME persistence_test COMMAND squeeze2raop2_persistence_tests)

  add_executable(squeeze2raop2_wav_tests test/test_wav_sink.cpp)
  target_link_libraries(squeeze2raop2_wav_tests PRIVATE squeeze2raop2_core)
  squeeze2raop2_target_settings(squeeze2raop2_wav_tests)
  add_test(NAME wav_sink_test COMMAND squeeze2raop2_wav_tests)

  add_executable(squeeze2raop2_wire_tests test/test_slimproto_wire.cpp)
  target_link_libraries(squeeze2raop2_wire_tests PRIVATE squeeze2raop2_core)
  squeeze2raop2_target_settings(squeeze2raop2_wire_tests)
  add_test(NAME slimproto_wire_test COMMAND squeeze2raop2_wire_tests)

  add_executable(squeeze2raop2_transport_tests test/test_transport.cpp)
  target_link_libraries(squeeze2raop2_transport_tests PRIVATE squeeze2raop2_core)
  squeeze2raop2_target_settings(squeeze2raop2_transport_tests)
  add_test(NAME transport_test COMMAND squeeze2raop2_transport_tests)

  add_executable(squeeze2raop2_volume_tests test/test_volume_map.cpp)
  target_link_libraries(squeeze2raop2_volume_tests PRIVATE squeeze2raop2_core)
  squeeze2raop2_target_settings(squeeze2raop2_volume_tests)
  add_test(NAME volume_map_test COMMAND squeeze2raop2_volume_tests)

  add_executable(squeeze2raop2_registry_tests test/test_device_registry.cpp)
  target_link_libraries(squeeze2raop2_registry_tests PRIVATE squeeze2raop2_core)
  squeeze2raop2_target_settings(squeeze2raop2_registry_tests)
  add_test(NAME device_registry_test COMMAND squeeze2raop2_registry_tests)

  add_executable(squeeze2raop2_byte_order_tests test/test_byte_order.cpp)
  target_link_libraries(squeeze2raop2_byte_order_tests PRIVATE squeeze2raop2_core)
  squeeze2raop2_target_settings(squeeze2raop2_byte_order_tests)
  add_test(NAME byte_order_test COMMAND squeeze2raop2_byte_order_tests)

  add_executable(squeeze2raop2_pcm_tests test/test_pcm_decoder.cpp)
  target_link_libraries(squeeze2raop2_pcm_tests PRIVATE squeeze2raop2_core)
  squeeze2raop2_target_settings(squeeze2raop2_pcm_tests)
  add_test(NAME pcm_decoder_test COMMAND squeeze2raop2_pcm_tests)

  add_executable(squeeze2raop2_decoder_tests test/test_decoder.cpp)
  target_link_libraries(squeeze2raop2_decoder_tests PRIVATE squeeze2raop2_core)
  squeeze2raop2_target_settings(squeeze2raop2_decoder_tests)
  add_test(NAME decoder_test COMMAND squeeze2raop2_decoder_tests)

  add_executable(squeeze2raop2_ring_telemetry_tests test/test_ring_telemetry.cpp)
  target_link_libraries(squeeze2raop2_ring_telemetry_tests PRIVATE squeeze2raop2_core)
  squeeze2raop2_target_settings(squeeze2raop2_ring_telemetry_tests)
  add_test(NAME ring_telemetry_test COMMAND squeeze2raop2_ring_telemetry_tests)

  add_executable(squeeze2raop2_icy_tests test/test_icy_meta.cpp)
  target_link_libraries(squeeze2raop2_icy_tests PRIVATE squeeze2raop2_core)
  squeeze2raop2_target_settings(squeeze2raop2_icy_tests)
  add_test(NAME icy_meta_test COMMAND squeeze2raop2_icy_tests)

  add_executable(squeeze2raop2_counters_tests test/test_stream_counters.cpp)
  target_link_libraries(squeeze2raop2_counters_tests PRIVATE squeeze2raop2_core)
  squeeze2raop2_target_settings(squeeze2raop2_counters_tests)
  add_test(NAME stream_counters_test COMMAND squeeze2raop2_counters_tests)

  add_executable(squeeze2raop2_exit_tests test/test_session_exit.cpp)
  target_link_libraries(squeeze2raop2_exit_tests PRIVATE squeeze2raop2_core)
  squeeze2raop2_target_settings(squeeze2raop2_exit_tests)
  add_test(NAME session_exit_test COMMAND squeeze2raop2_exit_tests)

  add_executable(mp3_smoke test/mp3_smoke.cpp)
  target_link_libraries(mp3_smoke PRIVATE squeeze2raop2_core)
  squeeze2raop2_target_settings(mp3_smoke)
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
