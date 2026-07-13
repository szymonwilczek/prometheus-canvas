#!/usr/bin/env bash
# Compile freestanding C engine to public/wasm/prometheus.wasm
#
# Requirements: clang with the wasm32 backend (any modern clang)
# and the wasm-ld linker from LLVM lld:
#   Fedora:        sudo dnf install lld
set -euo pipefail
cd "$(dirname "$0")/.."

OUT=public/wasm/prometheus.wasm

command -v clang >/dev/null 2>&1 || {
    echo "error: clang not found" >&2
    exit 1
}
command -v wasm-ld >/dev/null 2>&1 || {
    echo "error: wasm-ld not found — install the 'lld' package" >&2
    exit 1
}

mkdir -p "$(dirname "$OUT")"

clang --target=wasm32 -O3 -ffast-math -nostdlib -ffreestanding -fno-builtin \
    -Wall -Wextra -Werror \
    -Wl,--no-entry \
    -Wl,--export-all \
    -Wl,-z,stack-size=1048576 \
    -o "$OUT" engine/*.c

echo "built $OUT ($(stat -c%s "$OUT") bytes)"
