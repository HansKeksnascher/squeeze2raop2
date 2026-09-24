# Build the Ogg Opus third-party stack in an isolated CMake directory scope.
#
# - libogg is built through its own CMake: its os_types.h includes the
#   generated ogg/config_types.h, which only its build step produces. Building
#   framing.c/bitwise.c by hand would silently rely on a system libogg header.
# - libopus also has its own CMake, but it builds tests/programs/install rules
#   and touches directory-scoped state. Running it from this dedicated
#   subdirectory keeps those side effects off squeeze2raop2's own targets.
#
# Both are pinned as git submodules at third_party/ogg and third_party/opus.

set(OGG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/ogg")
set(OPUS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/opus")

if(NOT EXISTS "${OGG_ROOT}/src/framing.c" OR NOT EXISTS "${OPUS_ROOT}/CMakeLists.txt")
  message(FATAL_ERROR
    "SQUEEZE2RAOP2_WITH_OPUS is ON but third_party/ogg or third_party/opus is "
    "missing. Run: git submodule update --init --recursive")
endif()

# libogg: static library only, no docs/pkg-config/CMake package install rules.
set(INSTALL_DOCS OFF CACHE BOOL "" FORCE)
set(INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(INSTALL_CMAKE_PACKAGE_MODULE OFF CACHE BOOL "" FORCE)
add_subdirectory("${OGG_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/ogg" EXCLUDE_FROM_ALL)
# Descriptive alias so the top-level links a name that says what it is.
add_library(squeeze2raop2_ogg ALIAS ogg)

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