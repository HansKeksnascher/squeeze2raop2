#!/usr/bin/env bash
# Report the runtime surface of a built binary: dynamic dependencies and the
# maximum glibc/libstdc++ symbol versions it requires. CI attaches this to the
# dynamically linked release artifact so distros can check their baseline.
#
#   tools/runtime-deps.sh path/to/squeeze2raop2
set -euo pipefail

bin="${1:?usage: runtime-deps.sh <binary>}"
if [ ! -f "${bin}" ]; then
    echo "no such file: ${bin}" >&2
    exit 1
fi

echo "# runtime dependencies: $(basename "${bin}")"
echo
file "${bin}"
echo
if file "${bin}" | grep -q 'statically linked'; then
    echo "dynamic dependencies: none (statically linked)"
    echo "required runtime libraries: none (Linux kernel only)"
    exit 0
fi
echo "## ldd"
ldd "${bin}" 2>&1 || true
echo
echo "## required symbol versions (maximum per namespace)"
for ns in GLIBC GLIBCXX CXXABI; do
    v="$(objdump -T "${bin}" 2>/dev/null | grep -oE "${ns}_[0-9.]+" | sort -uV | tail -1 || true)"
    if [ -n "${v}" ]; then
        echo "${ns}: ${v}"
    fi
done