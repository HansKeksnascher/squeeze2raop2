cmake_minimum_required(VERSION 3.16)
project(sqraop2 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(SQRAOP_BUILD_TESTS "build sqraop2 unit tests" ON)
option(SQRAOP_WITH_SENDER "integrate the vendored raop_sender library" ON)

find_package(Threads REQUIRED)

if(SQRAOP_WITH_SENDER)
  set(AIRPLAY_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
  set(AIRPLAY_BUILD_TESTS ON CACHE BOOL "" FORCE)
  set(AIRPLAY_BUILD_QT_HOST OFF CACHE BOOL "" FORCE)
  add_subdirectory(third_party/airplay2-sender-cpp EXCLUDE_FROM_ALL)
endif()

add_library(sqraop_apple_mdns STATIC
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
target_compile_options(sqraop_apple_mdns PRIVATE -w -std=gnu17)
target_compile_definitions(sqraop_apple_mdns PRIVATE
  _GNU_SOURCE HAVE_IPV6 NOT_HAVE_SA_LEN USES_NETLINK HAVE_LINUX TARGET_OS_LINUX
  POSIX_BUILD MDNS_NO_STRICT=1 NOT_HAVE_DSO
)
target_include_directories(sqraop_apple_mdns PUBLIC
  third_party/mDNSResponder/mDNSCore
  third_party/mDNSResponder/mDNSPosix
  third_party/mDNSResponder/mDNSShared
  third_party/mDNSResponder/mDNSShared/utilities
)
target_link_libraries(sqraop_apple_mdns PRIVATE Threads::Threads)

add_library(sqraop_core STATIC
  src/util.cpp
  src/log.cpp
  src/config.cpp
  src/state_store.cpp
  src/device_registry.cpp
  src/mdns_embedded.cpp
  src/raop_player.cpp
  src/wav_sink.cpp
  src/lms_stream.cpp
  src/slimproto.cpp
  src/mp3_decoder.cpp
)
target_include_directories(sqraop_core PUBLIC src third_party/minimp3)
target_link_libraries(sqraop_core PUBLIC Threads::Threads sqraop_apple_mdns)
if(SQRAOP_WITH_SENDER)
  target_link_libraries(sqraop_core PUBLIC raop_sender)
  target_compile_definitions(sqraop_core PUBLIC SQRAOP_WITH_SENDER=1)
endif()

add_executable(sqraop2 src/main.cpp src/bridge.cpp)
target_link_libraries(sqraop2 PRIVATE sqraop_core)

if(SQRAOP_BUILD_TESTS)
  enable_testing()
  add_executable(sqraop_tests test/test_util.cpp)
  target_link_libraries(sqraop_tests PRIVATE sqraop_core)
  add_test(NAME util_test COMMAND sqraop_tests)

  add_executable(mp3_smoke test/mp3_smoke.cpp)
  target_link_libraries(mp3_smoke PRIVATE sqraop_core)
endif()

install(TARGETS sqraop2 RUNTIME DESTINATION bin)
