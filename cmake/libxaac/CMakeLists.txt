# Build only libxaac's decoder, in an isolated CMake directory scope.
#
# libxaac's own CMakeLists is not add_subdirectory'd directly: it also builds
# the encoder, testbench and fuzzers, and its fragments call directory-scoped
# commands (add_compile_options / add_definitions / include_directories /
# CMAKE_STATIC_LIBRARY_PREFIX). Running them in this dedicated subdirectory
# keeps those side effects off squeeze2raop2's own targets while still producing
# the `libxaacdec` static library.
#
# The vendored source is pinned as a git submodule at third_party/libxaac.

set(XAAC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/libxaac")
set(XAAC_CONFIG_DIR "${CMAKE_CURRENT_BINARY_DIR}")
set(CMAKE_STATIC_LIBRARY_PREFIX "")

include("${XAAC_ROOT}/cmake/utils.cmake")
libxaac_add_compile_options()
libxaac_add_definitions()

include("${XAAC_ROOT}/common/common.cmake")
include("${XAAC_ROOT}/decoder/libxaacdec.cmake")

# libxaac is third-party: silence its warnings and expose the decoder headers to
# the first-party wrapper that links it.
target_compile_options(libxaacdec PRIVATE -w)
target_include_directories(libxaacdec PUBLIC
  "${XAAC_ROOT}/decoder"
  "${XAAC_ROOT}/decoder/drc_src"
  "${XAAC_ROOT}/common")
