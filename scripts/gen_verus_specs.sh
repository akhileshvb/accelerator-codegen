#!/usr/bin/env bash
# Regenerate the trusted Verus specs (src/verus/<q>/src/gen_spec.rs) from the
# SQL in src/verus/sql2verus/queries with sql2verus (DataFusion pinned), and,
# for a query with declared kernel inputs (queries/<q>.inputs), its kernel
# obligations (src/verus/<q>/src/gen_obligations.rs) and the glue derived from
# them: the store adapter (gen_world.rs), the C ABI (gen_ffi.rs) and its header
# (include/accelerator_codegen/verus/<q>_verus.h, C names <q>v_* / <q>vs_*).
#   --check   regenerate into a temp dir and fail if any committed file differs
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
T="$ROOT/src/verus/sql2verus"
cargo build --release --quiet --manifest-path "$T/Cargo.toml"
BIN="${CARGO_TARGET_DIR:-$T/target}/release/sql2verus"
CHECK=0; [ "${1:-}" = "--check" ] && CHECK=1
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
status=0
for q in q1 q21; do
  src="$ROOT/src/verus/$q/src"
  inc="$ROOT/include/accelerator_codegen/verus"
  dst="$src"; dinc="$inc"; [ $CHECK = 1 ] && { dst="$TMP/$q"; dinc="$dst"; mkdir -p "$dst"; }
  files=("$src/gen_spec.rs:$dst/gen_spec.rs")
  ob=()
  if [ -f "$T/queries/$q.inputs" ]; then
    ob=(--obligations "$dst/gen_obligations.rs" --inputs "$T/queries/$q.inputs"
        --world "$dst/gen_world.rs" --ffi "$dst/gen_ffi.rs" --ffi-header "$dinc/${q}_verus.h" --ffi-prefix "${q}v")
    for f in gen_obligations.rs gen_world.rs gen_ffi.rs; do files+=("$src/$f:$dst/$f"); done
    files+=("$inc/${q}_verus.h:$dinc/${q}_verus.h")
  fi
  "$BIN" --schema "$T/queries/tpch_schema.sql" --query "$T/queries/$q.sql" --out "$dst/gen_spec.rs" "${ob[@]}"
  for pair in "${files[@]}"; do
    committed="${pair%%:*}"; fresh="${pair#*:}"
    if [ $CHECK = 1 ]; then
      if diff -u "$committed" "$fresh"; then echo "${committed#$ROOT/}: up to date"; else echo "${committed#$ROOT/} is stale" >&2; status=1; fi
    else
      echo "wrote ${committed#$ROOT/}"
    fi
  done
done
exit $status
