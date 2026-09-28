#!/usr/bin/env bash
# Q1 structural inductive POC: dafny verify + Dafny-extract ≡ C++ on grid D.
# SQL meaning is FormalSQL (Logos); no MathSem / Q1Sql path.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

DFY_DIR="$ROOT/tests/verification/q1/dafny"

find_dafny() {
  if command -v dafny >/dev/null 2>&1; then
    command -v dafny
    return
  fi
  if [[ -x "$ROOT/tools/dafny/dafny" ]]; then
    echo "$ROOT/tools/dafny/dafny"
    return
  fi
  return 1
}

if ! DAFNY_BIN="$(find_dafny)"; then
  cat <<'EOF' >&2
dafny not found on PATH or at tools/dafny/dafny.

Install options:
  - Download: https://github.com/dafny-lang/dafny/releases
    (e.g. dafny-*-x64-ubuntu-*.zip → extract to tools/dafny/)
  - Or: dotnet tool install --global dafny
EOF
  exit 1
fi

echo "==> dafny verify Q1Finalize"
"$DAFNY_BIN" verify "$DFY_DIR/Q1Finalize.dfy"

echo "==> dafny verify Q1ModuleTwin"
"$DAFNY_BIN" verify "$DFY_DIR/Q1ModuleTwin.dfy"

echo "==> dafny verify Q1Aux (derived Fold/Inv lemmas)"
"$DAFNY_BIN" verify "$DFY_DIR/Q1Aux.dfy"

echo "==> dafny verify Q1Maintenance (Inv under all maint actions)"
"$DAFNY_BIN" verify "$DFY_DIR/Q1Maintenance.dfy"

echo "==> dafny verify Q1MaintenanceEdges (delete-any, Install/CAS, policy)"
"$DAFNY_BIN" verify "$DFY_DIR/Q1MaintenanceEdges.dfy"

echo "==> Dafny extract ≡ generated C++ (grid D)"
"$ROOT/scripts/run_dafny_q1_extract_diff.sh"

echo "PASS q1 inductive POC (Finalize + ModuleTwin + Aux + Maintenance + Edges + extract-diff)"
