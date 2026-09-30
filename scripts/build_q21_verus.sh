#!/usr/bin/env bash
# Verify the shared Verus library (src/verus/common) and the Q21 kernel
# (src/verus/q21), and compile the kernel to build-verus/libq21_verus.a.
# Needs tools/verus (a Verus release) and its pinned rust toolchain.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERUS="${VERUS_BIN:-$ROOT/tools/verus/verus-x86-linux/verus}"
OUT="$ROOT/build-verus"
RUSTFLAGS_V=(-C opt-level=3 -C panic=abort -C codegen-units=1)
mkdir -p "$OUT"

# 1. shared library: verify, export its specs/proofs, compile to an rlib
"$VERUS" "$ROOT/src/verus/common/src/lib.rs" --crate-type=lib --crate-name=verus_common \
  --export "$OUT/verus_common.vir" --compile "${RUSTFLAGS_V[@]}" \
  -o "$OUT/libverus_common.rlib"

# 2. Q21 kernel against the verified library
"$VERUS" "$ROOT/src/verus/q21/src/lib.rs" --crate-type=staticlib --crate-name=q21_verus \
  --import "verus_common=$OUT/verus_common.vir" \
  --extern "verus_common=$OUT/libverus_common.rlib" -L "dependency=$OUT" \
  --compile "${RUSTFLAGS_V[@]}" -o "$OUT/libq21_verus.a"
echo "built $OUT/libq21_verus.a"
