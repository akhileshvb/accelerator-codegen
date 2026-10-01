#!/usr/bin/env bash
# Verify the stateless TPC-H Q1 kernel (src/verus/q1) against the shared
# Verus library and compile it to build-verus/libq1_verus.a.
# Needs tools/verus and build-verus/verus_common.{vir,rlib}
# (scripts/build_q21_verus.sh builds those).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERUS="${VERUS_BIN:-$ROOT/tools/verus/verus-x86-linux/verus}"
OUT="$ROOT/build-verus"
RUSTFLAGS_V=(-C opt-level=3 -C panic=abort -C codegen-units=1)
if [ ! -f "$OUT/verus_common.vir" ] || [ -n "$(find "$ROOT/src/verus/common/src" -newer "$OUT/verus_common.vir")" ]; then
  "$VERUS" "$ROOT/src/verus/common/src/lib.rs" --crate-type=lib --crate-name=verus_common \
    --export "$OUT/verus_common.vir" --compile "${RUSTFLAGS_V[@]}" \
    -o "$OUT/libverus_common.rlib"
fi
"$VERUS" "$ROOT/src/verus/q1/src/lib.rs" --crate-type=staticlib --crate-name=q1_verus \
  --import "verus_common=$OUT/verus_common.vir" \
  --extern "verus_common=$OUT/libverus_common.rlib" -L "dependency=$OUT" \
  --compile "${RUSTFLAGS_V[@]}" -o "$OUT/libq1_verus.a"
echo "built $OUT/libq1_verus.a"
