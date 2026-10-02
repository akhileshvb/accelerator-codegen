#!/usr/bin/env bash
# Verify a Verus kernel crate against the verified libraries (src/verus/common,
# src/verus/substrate, src/verus/prooflib) and, unless --verify-only, compile
# it to $OUT/lib<name>_verus.a for the engines (CMake links each
# src/generated/<bench>/q<N>/verus_kit/<name>/ whose library exists).
#   scripts/build_verus_crate.sh <crate_dir> <name> [--verify-only]
# OUT: $ACCEL_VERUS_LIB_DIR (default build-verus).  The libraries are rebuilt
# only when their sources change, under a lock (parallel pipeline runs).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERUS="${VERUS_BIN:-$ROOT/tools/verus/verus-x86-linux/verus}"
OUT="${ACCEL_VERUS_LIB_DIR:-$ROOT/build-verus}"
CRATE_DIR="$1"; NAME="$2"; MODE="${3:-}"
RF=(-C opt-level=3 -C panic=abort -C codegen-units=1)
mkdir -p "$OUT"

# 1. the libraries (once per source change)
(
  flock 9
  for lib in common substrate prooflib; do
    src="$ROOT/src/verus/$lib/src"
    stamp="$OUT/verus_$lib.stamp"
    hash="$(cat "$src"/*.rs | sha256sum | cut -d' ' -f1)"
    [ -f "$stamp" ] && [ "$(cat "$stamp")" = "$hash" ] && [ -f "$OUT/libverus_$lib.rlib" ] && continue
    deps=()
    [ "$lib" = prooflib ] && deps=(--import "verus_common=$OUT/verus_common.vir" --extern "verus_common=$OUT/libverus_common.rlib" -L "dependency=$OUT")
    "$VERUS" "$src/lib.rs" --crate-type=lib --crate-name="verus_$lib" "${deps[@]}" \
      --export "$OUT/verus_$lib.vir" --compile "${RF[@]}" -o "$OUT/libverus_$lib.rlib" >/dev/null
    echo "$hash" > "$stamp"
  done
) 9>"$OUT/.libs.lock"

# 2. the crate
args=("$CRATE_DIR/src/lib.rs" --crate-type=staticlib --crate-name="${NAME}_verus"
      --import "verus_common=$OUT/verus_common.vir" --extern "verus_common=$OUT/libverus_common.rlib"
      --import "verus_substrate=$OUT/verus_substrate.vir" --extern "verus_substrate=$OUT/libverus_substrate.rlib"
      --import "verus_prooflib=$OUT/verus_prooflib.vir" --extern "verus_prooflib=$OUT/libverus_prooflib.rlib"
      -L "dependency=$OUT")
if [ "$MODE" = "--verify-only" ]; then
  "$VERUS" "${args[@]}"
else
  "$VERUS" "${args[@]}" --compile "${RF[@]}" -o "$OUT/lib${NAME}_verus.a"
  echo "built $OUT/lib${NAME}_verus.a"
fi
