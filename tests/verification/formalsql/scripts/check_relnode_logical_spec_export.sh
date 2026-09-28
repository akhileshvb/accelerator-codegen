#!/usr/bin/env bash
# RELNODE_LOGICAL_SPEC_EXPORT_OK — SpecExportCorrect (A-RelNode):
# RelNode LogicalSpec DATA transcription vs *committed* artifacts.
#
# NOT a semantic proof. Checks:
#   (1) copy committed JSON/DFY, regenerate, diff committed vs regenerated
#       (proves fidelity to the tree, not merely regen determinism)
#   (2) dump hash pinned in Dafny header
#
# Flat LogicalSpec DATA export remains migration scaffolding
# (check_logical_spec_export.sh).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"
FSQL="$ROOT/tests/verification/formalsql"
QUERY="${1:-q1}"

die() { echo "RELNODE_LOGICAL_SPEC_EXPORT_OK FAIL: $*" >&2; exit 1; }
[[ "$QUERY" == "q1" ]] || die "only q1 supported this pass"

DUMP="$ROOT/tests/verification/specs/$QUERY/logical_spec_relnode.dump.json"
DFY="$ROOT/tests/verification/q1/dafny/generated/Q1RelNodeData.dfy"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

[[ -f "$DUMP" ]] || die "missing committed dump $DUMP"
[[ -f "$DFY" ]] || die "missing committed $DFY"

# Snapshot committed artifacts BEFORE regenerating.
cp "$DUMP" "$WORK/committed.dump.json"
cp "$DFY" "$WORK/committed.Q1RelNodeData.dfy"

chmod +x "$FSQL/scripts/dump_relnode_logical_spec_json.sh"
bash "$FSQL/scripts/dump_relnode_logical_spec_json.sh" "$QUERY"
diff -u "$WORK/committed.dump.json" "$DUMP" \
  || die "logical_spec_relnode.dump.json differs from committed (update dump after Rocq DATA change)"

python3 "$FSQL/scripts/emit_relnode_logical_spec_dafny.py" "$QUERY"
diff -u "$WORK/committed.Q1RelNodeData.dfy" "$DFY" \
  || die "Q1RelNodeData.dfy differs from committed (update generated DATA after dump change)"

PIN="$(python3 - <<PY
import hashlib, json
from pathlib import Path
w = json.loads(Path("$DUMP").read_text())
body = w["export"]
pin_obj = {
    "version": body["version"],
    "table_lineitem": body["table_lineitem"],
    "pred_shipdate": body["pred_shipdate"],
    "group_exprs": body["group_exprs"],
    "aggs": body["aggs"],
    "project_exprs": body["project_exprs"],
    "order_keys": body["order_keys"],
    "tree": body["tree"],
    "catalog_params": body["catalog_params"],
    "catalog_delta": body["catalog_delta"],
}
print(hashlib.sha256(json.dumps(pin_obj, separators=(",", ":")).encode()).hexdigest())
PY
)"
grep -q "source_sha256=$PIN" "$DFY" \
  || die "Dafny pin mismatch (expected source_sha256=$PIN)"

grep -q "logical_spec_relnode_v1" "$DUMP" || die "dump missing relnode v1"
grep -q "EParam" "$DFY" || die "Dafny data missing EParam"
grep -q "cutoff" "$DFY" || die "Dafny data missing cutoff"
grep -q "RelAggregate" "$DFY" || die "Dafny data missing RelAggregate"
grep -q "RelSort" "$DFY" || die "Dafny data missing RelSort"
grep -q "RelFilter" "$DFY" || die "Dafny data missing RelFilter"
if grep -nE '\bPack\b' "$DFY"; then
  die "RelNode LogicalSpec DATA must not mention Pack"
fi

DAFNY_BIN="${DAFNY_BIN:-$ROOT/tools/dafny/dafny}"
[[ -x "$DAFNY_BIN" ]] || die "dafny not found at $DAFNY_BIN"
"$DAFNY_BIN" verify "$DFY" || die "dafny verify Q1RelNodeData.dfy failed"

echo "RELNODE_LOGICAL_SPEC_EXPORT_OK (committed↔regen RelNode DATA; pin=$PIN)"
