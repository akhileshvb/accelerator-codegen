#!/usr/bin/env bash
# Put a (re)built Verus kit into its benchmark's world: the one verified store
# for every accelerator of the database (ACCEL_AUX_STORE=verus_global).  CMake
# links a benchmark's kits only through the world archive, so a kit change
# reaches the engines only once the world is rebuilt.
#   scripts/update_verus_world.sh <bench> <kit_name> [<crate_dir>] [--no-verify]
# With <crate_dir>: first compile that kit as a library (--rlib; --no-verify:
# code first, its proofs not checked).  A kit the world does not name yet is
# added (scripts/gen_verus_world.py, kits in query order); then the world is
# verified against every kit and compiled (scripts/build_verus_world.sh).
# Serialized under $OUT/.world.lock (parallel pipeline runs).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${ACCEL_VERUS_LIB_DIR:-$ROOT/build-verus}"
BENCH="$1"; KIT="$2"; CRATE_DIR="${3:-}"; MODE="${4:-}"
WORLD_RS="$ROOT/src/generated/$BENCH/verus_world/${BENCH}_world/crate/src/world.rs"
mkdir -p "$OUT"

if [ -n "$CRATE_DIR" ]; then
  if [ "$MODE" = "--no-verify" ]; then
    bash "$ROOT/scripts/build_verus_crate.sh" "$CRATE_DIR" "$KIT" --rlib-no-verify
  else
    bash "$ROOT/scripts/build_verus_crate.sh" "$CRATE_DIR" "$KIT" --rlib
  fi
fi

(
  flock 9
  kits=()
  if [ -f "$WORLD_RS" ]; then
    mapfile -t kits < <(grep -oE '^pub type S[0-9]+ = [a-z0-9_]+_verus::' "$WORLD_RS" | sed -E 's/.* = ([a-z0-9_]+)_verus::/\1/')
  fi
  if ! printf '%s\n' "${kits[@]}" | grep -qx "$KIT"; then
    # every kit in query order (tpch_q2_vgen before tpch_q10_vgen)
    mapfile -t kits < <(printf '%s\n' "${kits[@]}" "$KIT" | grep . | sort -t q -k2,2n -u)
    echo "world ${BENCH}_world: adding $KIT (${#kits[@]} kits)"
    python3 "$ROOT/scripts/gen_verus_world.py" "$BENCH" "${kits[@]}"
  fi
  bash "$ROOT/scripts/build_verus_world.sh" "$BENCH"
) 9>"$OUT/.world.lock"
