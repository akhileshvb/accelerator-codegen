#!/usr/bin/env bash
# Test sql2verus on the query corpora in src/verus/sql2verus/tests/<suite>/:
# every query must be translated or refused as expected.txt says, and every
# generated spec (definitions and facts) must pass Verus.
#   scripts/test_sql2verus.sh [-j N] [suite ...]      (default: all suites)
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
T="$ROOT/src/verus/sql2verus"
VERUS="${VERUS_BIN:-$ROOT/tools/verus/verus-x86-linux/verus}"
JOBS=4
if [ "${1:-}" = "-j" ]; then JOBS=$2; shift 2; fi
SUITES=("$@"); [ ${#SUITES[@]} -eq 0 ] && SUITES=($(ls "$T/tests"))
cargo build --release --quiet --manifest-path "$T/Cargo.toml"
BIN="${CARGO_TARGET_DIR:-$T/target}/release/sql2verus"
WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT
"$VERUS" "$ROOT/src/verus/common/src/lib.rs" --crate-type=lib --crate-name=verus_common \
  --export "$WORK/verus_common.vir" --compile -o "$WORK/libverus_common.rlib" >/dev/null
one() { # suite query expectation...
  local suite=$1 q=$2; shift 2; local want="$*"
  local d="$WORK/$suite/$q"; mkdir -p "$d"
  local out
  if ! out=$("$BIN" --schema "$T/tests/$suite/schema.sql" --query "$T/tests/$suite/$q.sql" --out "$d/gen_spec.rs" 2>&1); then
    if [[ "$want" == refused* ]] && [[ "$out" == *"${want#refused }"* ]]; then echo "PASS $suite/$q (refused)"; else echo "FAIL $suite/$q: $out"; fi
    return
  fi
  if [[ "$want" != ok ]]; then echo "FAIL $suite/$q: translated, expected $want"; return; fi
  printf '#![allow(unused_imports)]\npub mod gen_spec;\n' > "$d/lib.rs"
  local res
  res=$("$VERUS" "$d/lib.rs" --crate-type=lib --crate-name="t_$q" --import "verus_common=$WORK/verus_common.vir" \
        --extern "verus_common=$WORK/libverus_common.rlib" -L "dependency=$WORK" 2>&1 || true)
  if echo "$res" | grep -q "verification results:: [0-9]* verified, 0 errors" && ! echo "$res" | grep -q "^error"; then
    echo "PASS $suite/$q ($(echo "$res" | grep -o '[0-9]* verified' | tail -1))"
  else
    echo "FAIL $suite/$q: Verus"; echo "$res" | grep -E "^error" -A4 | head -20
  fi
}
export -f one; export BIN T VERUS WORK
for s in "${SUITES[@]}"; do
  grep -v '^#' "$T/tests/$s/expected.txt" | sed "s|^|$s |"
done | xargs -P "$JOBS" -L 1 bash -c 'one "$@"' _ | sort | tee "$WORK/results"
fails=$(grep -c '^FAIL' "$WORK/results" || true)
echo "$(grep -c '^PASS' "$WORK/results") passed, $fails failed"
[ "$fails" -eq 0 ]
