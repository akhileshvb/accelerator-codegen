#!/usr/bin/env bash
# Regenerate the trusted Verus specs (src/verus/<q>/src/gen_spec.rs) from the
# SQL in src/verus/sql2verus/queries with sql2verus (DataFusion pinned).
#   --check   regenerate into a temp dir and fail if any committed spec differs
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
T="$ROOT/src/verus/sql2verus"
cargo build --release --quiet --manifest-path "$T/Cargo.toml"
BIN="${CARGO_TARGET_DIR:-$T/target}/release/sql2verus"
CHECK=0; [ "${1:-}" = "--check" ] && CHECK=1
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
status=0
for q in q1 q21; do
  out="$ROOT/src/verus/$q/src/gen_spec.rs"
  dst="$out"; [ $CHECK = 1 ] && dst="$TMP/$q.rs"
  "$BIN" --schema "$T/queries/tpch_schema.sql" --query "$T/queries/$q.sql" --out "$dst"
  if [ $CHECK = 1 ]; then
    if diff -u "$out" "$dst"; then echo "$q: up to date"; else echo "$q: gen_spec.rs is stale" >&2; status=1; fi
  else
    echo "wrote $out"
  fi
done
exit $status
