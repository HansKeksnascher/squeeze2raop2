#!/usr/bin/env bash
# Download and unpack the pinned Bootlin x86_64 musl toolchain used for the
# fully-static release binary. Prints the toolchain root on stdout.
#
#   tools/fetch-musl-toolchain.sh [destination-dir]   (default: /opt)
#
# The toolchain and its version live here so CI and local builds agree.
set -euo pipefail

version="2026.08-1"
name="x86-64--musl--stable-${version}"
url="https://toolchains.bootlin.com/downloads/releases/toolchains/x86-64/tarballs/${name}.tar.xz"
dest="${1:-/opt}"

root="${dest}/${name}"
if [ -d "${root}" ]; then
    echo "${root}"
    exit 0
fi

if ! mkdir -p "${dest}" 2>/dev/null; then
    echo "error: cannot create ${dest}; pass a writable destination" >&2
    exit 1
fi
if [ ! -w "${dest}" ]; then
    echo "error: ${dest} is not writable; pass a writable destination" >&2
    exit 1
fi

tmp="$(mktemp -d)"
trap 'rm -rf "${tmp}"' EXIT

echo "downloading ${url}" >&2
curl -fsSL -o "${tmp}/toolchain.tar.xz" "${url}"
tar -xJf "${tmp}/toolchain.tar.xz" -C "${tmp}"
mv "${tmp}/${name}" "${root}"

echo "${root}"