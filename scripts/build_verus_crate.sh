#!/usr/bin/env bash
# Verify a Verus kernel crate against the verified libraries (substrate/verus/common,
# substrate/verus/store, accelerators/prooflib) and, unless --verify-only, compile
# it (with the counting allocator of substrate/verus/heap) to $OUT/lib<name>_verus.a for the engines (CMake links each
# accelerators/<bench>/q<N>/verus_kit/<name>/ whose library exists).
#   scripts/build_verus_crate.sh <crate_dir> <name> [--verify-only | --no-verify | --rlib | --rlib-no-verify]
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
  for lib in heap common substrate prooflib; do
    case $lib in heap) src="$ROOT/substrate/verus/heap/src" ;; common) src="$ROOT/substrate/verus/common/src" ;; substrate) src="$ROOT/substrate/verus/store/src" ;; prooflib) src="$ROOT/accelerators/prooflib/src" ;; esac
    stamp="$OUT/verus_$lib.stamp"
    srcs=("$src"/*.rs)
    [ "$lib" = prooflib ] && srcs+=("$ROOT"/substrate/verus/common/src/*.rs)   # built against common
    hash="$(cat "${srcs[@]}" | sha256sum | cut -d' ' -f1)"
    [ -f "$stamp" ] && [ "$(cat "$stamp")" = "$hash" ] && [ -f "$OUT/libverus_$lib.rlib" ] && continue
    deps=()
    [ "$lib" = prooflib ] && deps=(--import "verus_common=$OUT/verus_common.vir" --extern "verus_common=$OUT/libverus_common.rlib" -L "dependency=$OUT")
    # written aside, then renamed: a crate build reading them never sees a partial file
    tmp="$OUT/tmp.$$"; mkdir -p "$tmp"
    "$VERUS" "$src/lib.rs" --crate-type=lib --crate-name="verus_$lib" "${deps[@]}" \
      --export "$tmp/verus_$lib.vir" --compile "${RF[@]}" -o "$tmp/libverus_$lib.rlib" >/dev/null
    mv -f "$tmp/verus_$lib.vir" "$OUT/verus_$lib.vir"; mv -f "$tmp/libverus_$lib.rlib" "$OUT/libverus_$lib.rlib"
    rmdir "$tmp"
    echo "$hash" > "$stamp"
  done
) 9>"$OUT/.libs.lock"

# 2. the crate
args=("$CRATE_DIR/src/lib.rs" --crate-type=staticlib --crate-name="${NAME}_verus"
      --import "verus_common=$OUT/verus_common.vir" --extern "verus_common=$OUT/libverus_common.rlib"
      --import "verus_substrate=$OUT/verus_substrate.vir" --extern "verus_substrate=$OUT/libverus_substrate.rlib"
      --import "verus_prooflib=$OUT/verus_prooflib.vir" --extern "verus_prooflib=$OUT/libverus_prooflib.rlib"
      --extern "verus_heap=$OUT/libverus_heap.rlib"
      -L "dependency=$OUT")
if [ "$MODE" = "--verify-only" ]; then
  "$VERUS" "${args[@]}"
elif [ "$MODE" = "--rlib" ] || [ "$MODE" = "--rlib-no-verify" ]; then
  # a library for a world crate (one store for every accelerator of a
  # benchmark, scripts/gen_verus_world.py): verified (--rlib-no-verify: code
  # first, proofs not checked), its specs exported
  args[1]=--crate-type=lib
  [ "$MODE" = "--rlib-no-verify" ] && args+=(--no-verify)
  part="$OUT/tmp.$$.lib${NAME}_verus.rlib"
  trap 'rm -f "$part" "$OUT/tmp.$$.${NAME}_verus.vir"' EXIT
  "$VERUS" "${args[@]}" --export "$OUT/tmp.$$.${NAME}_verus.vir" --compile "${RF[@]}" -o "$part"
  mv -f "$part" "$OUT/lib${NAME}_verus.rlib"
  mv -f "$OUT/tmp.$$.${NAME}_verus.vir" "$OUT/${NAME}_verus.vir"
  echo "built$([ "$MODE" = "--rlib-no-verify" ] && echo " (unverified)") $OUT/lib${NAME}_verus.rlib"
else
  # written aside, then renamed: another pipeline's link never reads a partial archive
  part="$OUT/tmp.$$.lib${NAME}_verus.a"
  trap 'rm -f "$part"' EXIT
  if [ "$MODE" = "--no-verify" ]; then
    # code first: compile the kernel without checking its proofs (tests can run on it)
    "$VERUS" "${args[@]}" --no-verify --compile "${RF[@]}" -o "$part"
  else
    "$VERUS" "${args[@]}" --compile "${RF[@]}" -o "$part"
  fi
  mv -f "$part" "$OUT/lib${NAME}_verus.a"
  echo "built$([ "$MODE" = "--no-verify" ] && echo " (unverified)") $OUT/lib${NAME}_verus.a"
fi
