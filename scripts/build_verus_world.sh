#!/usr/bin/env bash
# Verify a benchmark's Verus world crate (scripts/gen_verus_world.py) against
# its kits and compile it, with every kit, to $OUT/lib<bench>_world_verus.a:
# one archive holding each kit's C ABI and the one store for all of them
# (CMake links it instead of the kits' own archives).
#   scripts/build_verus_world.sh <bench> [--verify-only]
# Each kit must be built first:  scripts/build_verus_crate.sh <crate_dir> <name> --rlib
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERUS="${VERUS_BIN:-$ROOT/tools/verus/verus-x86-linux/verus}"
OUT="${ACCEL_VERUS_LIB_DIR:-$ROOT/build-verus}"
BENCH="$1"; MODE="${2:-}"
WORLD="${BENCH}_world"
DIR="$ROOT/src/generated/$BENCH/verus_world/$WORLD"
RF=(-C opt-level=3 -C panic=abort -C codegen-units=1)
args=("$DIR/crate/src/lib.rs" --crate-type=staticlib --crate-name="${WORLD}_verus"
      --import "verus_common=$OUT/verus_common.vir" --extern "verus_common=$OUT/libverus_common.rlib"
      --import "verus_substrate=$OUT/verus_substrate.vir" --extern "verus_substrate=$OUT/libverus_substrate.rlib"
      --import "verus_prooflib=$OUT/verus_prooflib.vir" --extern "verus_prooflib=$OUT/libverus_prooflib.rlib"
      -L "dependency=$OUT")
# every kit the world names
for kit in $(grep -oE '^pub type S[0-9]+ = [a-z0-9_]+_verus::' "$DIR/crate/src/world.rs" | sed -E 's/.* = ([a-z0-9_]+)_verus::/\1/'); do
  [ -f "$OUT/lib${kit}_verus.rlib" ] || { echo "missing $OUT/lib${kit}_verus.rlib (build_verus_crate.sh ... --rlib)" >&2; exit 1; }
  args+=(--import "${kit}_verus=$OUT/${kit}_verus.vir" --extern "${kit}_verus=$OUT/lib${kit}_verus.rlib")
done
if [ "$MODE" = "--verify-only" ]; then
  "$VERUS" "${args[@]}"
else
  part="$OUT/tmp.$$.lib${WORLD}_verus.a"
  trap 'rm -f "$part"' EXIT
  "$VERUS" "${args[@]}" --compile "${RF[@]}" -o "$part"
  mv -f "$part" "$OUT/lib${WORLD}_verus.a"
  echo "built $OUT/lib${WORLD}_verus.a"
fi
