# Cross toolchain file for a fully-static x86_64 Linux/musl build.
#
# Point MUSL_TOOLCHAIN_ROOT at an unpacked Bootlin x86-64--musl--stable-* tree
# (or any musl cross toolchain whose bin/ triple matches _musl_triple below):
#
#   cmake -B build-musl -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-musl-x86_64.cmake \
#         -DSQUEEZE2RAOP2_DIST=ON -DSQUEEZE2RAOP2_STATIC=ON \
#         -DSQUEEZE2RAOP2_BUILD_TESTS=OFF \
#         -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-musl -j
#
# tools/fetch-musl-toolchain.sh downloads the pinned Bootlin toolchain.
# The target OS equals the host, so CMake does not treat this as a cross build
# on x86_64 Linux and the produced binaries can be run/tested natively.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

if(DEFINED ENV{MUSL_TOOLCHAIN_ROOT})
  set(MUSL_TOOLCHAIN_ROOT "$ENV{MUSL_TOOLCHAIN_ROOT}")
else()
  set(MUSL_TOOLCHAIN_ROOT "/opt/x86-64--musl--stable-2026.08-1")
endif()

set(_musl_triple x86_64-buildroot-linux-musl)
set(CMAKE_C_COMPILER   "${MUSL_TOOLCHAIN_ROOT}/bin/${_musl_triple}-gcc")
set(CMAKE_CXX_COMPILER "${MUSL_TOOLCHAIN_ROOT}/bin/${_musl_triple}-g++")
set(CMAKE_FIND_ROOT_PATH "${MUSL_TOOLCHAIN_ROOT}/${_musl_triple}/sysroot")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)