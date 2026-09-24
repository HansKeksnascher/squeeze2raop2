# Build the Ogg Opus third-party stack in an isolated CMake directory scope.
#
# - libogg is two C files (framing + bitwise); compile them directly and
#   silence their warnings like the other vendored sources.
# - libopus has its own CMake, but it also builds tests/programs/install rules
#   and touches directory-scoped state. Running it from this dedicated
#   subdirectory keeps those side effects off squeeze2raop2's own targets
#   while still producing the static `opus` library.
#
# Both are pinned as git submodules at third_party/ogg and third_party/opus.

set(OGG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/ogg")
set(OPUS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/opus")

if(NOT EXISTS "${OGG_ROOT}/src/framing.c" OR NOT EXISTS "${OPUS_ROOT}/CMakeLists.txt")
  message(FATAL_ERROR
    "SQUEEZE2RAOP2_WITH_OPUS is ON but third_party/ogg or third_party/opus is "
    "missing. Run: git submodule update --init --recursive")
endif()

# libogg: framing + bitwise only, warnings silenced.
add_library(squeeze2raop2_ogg STATIC
  "${OGG_ROOT}/src/framing.c"
  "${OGG_ROOT}/src/bitwise.c")
target_compile_options(squeeze2raop2_ogg PRIVATE -w)
target_include_directories(squeeze2raop2_ogg PUBLIC "${OGG_ROOT}/include")

# libopus reads its version from a git tag or a package_version file, neither
# of which is present in a submodule checkout; seed it from the pinned tag so
# the build stays warning-free.
find_package(Git QUIET)
if(GIT_FOUND)
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${OPUS_ROOT}" describe --tags
    OUTPUT_VARIABLE _opus_desc OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
endif()
if(_opus_desc MATCHES "^v?([0-9]+\\.[0-9]+\\.[0-9]+)")
  set(OPUS_PACKAGE_VERSION "${CMAKE_MATCH_1}")
else()
  set(OPUS_PACKAGE_VERSION "0.0.0")
endif()

# Only the static library: no shared lib, tests, programs, pkg-config or CMake
# package install rules.
set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
add_subdirectory("${OPUS_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/opus" EXCLUDE_FROM_ALL)