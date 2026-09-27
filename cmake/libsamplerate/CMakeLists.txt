# Build only libsamplerate's static library, in an isolated CMake scope.
#
# libsamplerate's own CMakeLists includes CTest and, with BUILD_TESTING on,
# builds its test programs; it also adds install/pkg-config/CMake-package rules
# when it is the root project, and calls directory-scoped add_definitions /
# include_directories / link_libraries. Running it from this dedicated
# subdirectory keeps those side effects off squeeze2raop2's own targets while
# still producing the `samplerate` static library (also aliased as
# SampleRate::samplerate by its own CMake).
#
# The vendored source is pinned as a git submodule at third_party/libsamplerate.

set(SAMPLERATE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/libsamplerate")

if(NOT EXISTS "${SAMPLERATE_ROOT}/CMakeLists.txt")
  message(FATAL_ERROR
    "libsamplerate is required for the AirPlay output clock but "
    "third_party/libsamplerate is missing. "
    "Run: git submodule update --init --recursive")
endif()

# Static library only: no examples, tests, docs or install rules. BUILD_TESTING
# is a global CTest cache option; forcing it off here only stops libsamplerate
# from adding its tests subdirectory. squeeze2raop2 registers its own tests
# through enable_testing()/add_test(), which do not depend on it.
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(LIBSAMPLERATE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LIBSAMPLERATE_INSTALL OFF CACHE BOOL "" FORCE)

add_subdirectory("${SAMPLERATE_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/libsamplerate"
                 EXCLUDE_FROM_ALL)

# Descriptive alias so the top-level links a name that says what it is.
add_library(squeeze2raop2_samplerate ALIAS samplerate)
