#!/usr/bin/env bash
# Test sql2verus on the query corpora in substrate/verus/sql2verus/tests/<suite>/:
# every query must be translated or refused as expected.txt says, and every
# generated spec (definitions and facts), with its default kernel obligations
# (gen_obligations.rs) and kernel stub, must pass Verus.  With obligations,
# the derived glue must work too: the store adapter (gen_world.rs, verified
# against the verified store, substrate/verus/store) and the C ABI (gen_ffi.rs)
# compile into a static library; its C header compiles as C and C++; and a C
# program that references every function links and drives the store.
#   expected.txt: <query> ok | <query> ok-spec <text the obligations refusal
#   must contain> | <query> refused <text the refusal must contain>
#   A <query>.inputs next to a query declares its kernel inputs (default:
#   every table whole); a <query>.nullable-params its NULL-able parameters
#   (numbers, comma separated).  Suite `queries`: the production queries with declared
#   inputs (workloads/tpch/<q>.inputs), each expected ok.
#   scripts/test_sql2verus.sh [-j N] [suite ...]      (default: all suites)
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
T="$ROOT/substrate/verus/sql2verus"
VERUS="${VERUS_BIN:-$ROOT/tools/verus/verus-x86-linux/verus}"
JOBS=4
if [ "${1:-}" = "-j" ]; then JOBS=$2; shift 2; fi
SUITES=("$@"); [ ${#SUITES[@]} -eq 0 ] && SUITES=($(ls "$T/tests") queries)
cargo build --release --quiet --manifest-path "$T/Cargo.toml"
BIN="${CARGO_TARGET_DIR:-$T/target}/release/sql2verus"
WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT
"$VERUS" "$ROOT/substrate/verus/common/src/lib.rs" --crate-type=lib --crate-name=verus_common \
  --export "$WORK/verus_common.vir" --compile -o "$WORK/libverus_common.rlib" >/dev/null
"$VERUS" "$ROOT/substrate/verus/store/src/lib.rs" --crate-type=lib --crate-name=verus_substrate \
  --export "$WORK/verus_substrate.vir" --compile -o "$WORK/libverus_substrate.rlib" >/dev/null
one() { # suite query expectation...
  local suite=$1 q=$2; shift 2; local want="$*"
  local d="$WORK/$suite/$q"; mkdir -p "$d"
  local dir="$T/tests/$suite" schema
  if [ "$suite" = queries ]; then dir="$ROOT/workloads/tpch"; schema="$dir/schema.sql"; else schema="$dir/schema.sql"; fi
  local inp=(); [ -f "$dir/$q.inputs" ] && inp=(--inputs "$dir/$q.inputs")
  [ -f "$dir/$q.nullable-params" ] && inp+=(--nullable-params "$(cat "$dir/$q.nullable-params")")
  local out
  if ! out=$("$BIN" --schema "$schema" --query "$dir/$q.sql" --out "$d/gen_spec.rs" "${inp[@]}" \
               --obligations "$d/gen_obligations.rs" --kernel-stub "$d/kernel.rs" --world "$d/gen_world.rs" \
               --ffi "$d/gen_ffi.rs" --ffi-header "$d/q.h" --ffi-prefix tq \
               --host-glue "$d/gen_glue.cpp" --accel-id "t_$q" --workload tpch --query-id 1 2>&1); then
    if [[ "$out" == *"refused: obligations: "* ]]; then
      if [[ "$want" == ok-spec* ]] && [[ "$out" == *"${want#ok-spec }"* ]]; then
        rm -f "$d/gen_obligations.rs" "$d/kernel.rs" "$d/gen_world.rs" "$d/gen_ffi.rs" "$d/q.h" "$d/gen_glue.cpp"
      else echo "FAIL $suite/$q: $out"; return; fi
    elif [[ "$want" == refused* ]] && [[ "$out" == *"${want#refused }"* ]]; then echo "PASS $suite/$q (refused)"; return
    else echo "FAIL $suite/$q: $out"; return; fi
  elif [[ "$want" != ok ]]; then echo "FAIL $suite/$q: translated with obligations, expected $want"; return
  fi
  local glue=0 res
  if [ -f "$d/gen_obligations.rs" ]; then
    glue=1
    printf '#![allow(unused_imports)]\npub mod gen_spec;\npub mod gen_obligations;\npub mod kernel;\npub mod gen_world;\npub mod gen_ffi;\n' > "$d/lib.rs"
    res=$("$VERUS" "$d/lib.rs" --crate-type=staticlib --crate-name="t_$q" --import "verus_common=$WORK/verus_common.vir" \
          --extern "verus_common=$WORK/libverus_common.rlib" --import "verus_substrate=$WORK/verus_substrate.vir" \
          --extern "verus_substrate=$WORK/libverus_substrate.rlib" -L "dependency=$WORK" \
          --compile -C opt-level=0 -o "$d/libq.a" 2>&1 || true)
  else
    printf '#![allow(unused_imports)]\npub mod gen_spec;\n' > "$d/lib.rs"
    res=$("$VERUS" "$d/lib.rs" --crate-type=lib --crate-name="t_$q" --import "verus_common=$WORK/verus_common.vir" \
          --extern "verus_common=$WORK/libverus_common.rlib" -L "dependency=$WORK" 2>&1 || true)
  fi
  if ! echo "$res" | grep -q "verification results:: [0-9]* verified, 0 errors" || echo "$res" | grep -q "^error"; then
    echo "FAIL $suite/$q: Verus"; echo "$res" | grep -E "^error" -A4 | head -20; return
  fi
  if [ $glue = 1 ]; then
    local cres
    if ! cres=$(glue_check "$d" 2>&1); then echo "FAIL $suite/$q: glue"; echo "$cres" | head -20; return; fi
  fi
  echo "PASS $suite/$q ($(echo "$res" | grep -o '[0-9]* verified' | tail -1)$([ $glue = 1 ] && echo ', glue' || echo ', spec only'))"
}
# The C side of the generated glue: the header compiles as C and C++, the
# host glue (gen_glue.cpp) compiles against the kit, and a program
# referencing every declared function links against the crate and drives
# the store through the ABI (no kernel call: the kernel is a stub).
glue_check() {
  local d=$1
  python3 - "$d" <<'PY'
import re, sys
d = sys.argv[1]
names = re.findall(r'\b(tq\w*)\s*\(', open(d + '/q.h').read())
open(d + '/link.c', 'w').write('''#include "q.h"
#include <stdio.h>
int main(void) {
  void *fs[] = {%s};
  tqs_store *st = tqs_new();
  tqs_head *h = tqs_newest_head(st);
  tqs_staged *p = tqs_stage_invalid(h);
  tqs_head_free(h);
  int32_t status = -1;
  if (tqs_install(st, p, 5, ~(uint64_t)0, 0) != 0) return 1;
  if (tqs_read(st, 0, 0, &status) != NULL || status != 1) return 2;
  if (tqs_read(st, 4, 0, &status) != NULL || status != 3) return 3;
  if (tqs_begin(st, 6) != 1 || tqs_begin(st, 6) != 0) return 4;
  tqs_abort(st);
  if (tqs_version_count(st) != 1) return 5;
  tqs_free(st);
  printf("%%zu functions\\n", sizeof fs / sizeof fs[0]);
  return 0;
}
''' % ', '.join('(void *)&' + n for n in names))
open(d + '/hdr.cc', 'w').write('#include "q.h"\nint main() { return 0; }\n')
PY
  gcc -std=c99 -Wall -Wextra -Werror -I"$d" "$d/link.c" "$d/libq.a" -lpthread -ldl -lm -o "$d/link" &&
    g++ -std=c++17 -Wall -Wextra -Werror -I"$d" -fsyntax-only "$d/hdr.cc" &&
    g++ -std=c++17 -Wall -Wextra -Werror -Wno-unused-parameter -I"$d" -I"$ROOT" -I"$ROOT/accelerators" -fsyntax-only "$d/gen_glue.cpp" &&
    "$d/link"
}
export -f one glue_check; export BIN T VERUS WORK ROOT
for s in "${SUITES[@]}"; do
  if [ "$s" = queries ]; then
    for f in "$ROOT"/workloads/tpch/*.inputs; do echo "queries $(basename "$f" .inputs) ok"; done
  else
    grep -v '^#' "$T/tests/$s/expected.txt" | sed "s|^|$s |"
  fi
done | xargs -P "$JOBS" -L 1 bash -c 'one "$@"' _ | sort | tee "$WORK/results"
fails=$(grep -c '^FAIL' "$WORK/results" || true)
echo "$(grep -c '^PASS' "$WORK/results") passed, $fails failed"
[ "$fails" -eq 0 ]
