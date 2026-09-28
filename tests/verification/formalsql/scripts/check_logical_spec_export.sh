#!/usr/bin/env bash
# LOGICAL_SPEC_EXPORT_OK — FLAT migration scaffolding DATA transcription.
#
# NOT the canonical SpecExportCorrect (A) surface. Canonical RelNode DATA:
#   check_relnode_logical_spec_export.sh (LogicalSpec_Q1_rel → Q1RelNodeData.dfy)
#
# This is NOT a semantic proof. It checks flat expression-IR only:
#   (1) Rocq normalize_q1(catalog) path produces LogicalSpec_Q1 export
#   (2) serialize_LogicalSpec_Q1_json matches committed JSON (regen + empty diff)
#   (3) emitted Dafny LogicalSpecData.dfy matches committed file (regen + empty diff)
#   (4) dump hash pinned in Dafny header
#
# Retained while RelNode↔flat observational proofs still reference flat DATA.
# See resultspec/GENERIC_INTERPRETER_TCB.md.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"
FSQL="$ROOT/tests/verification/formalsql"
QUERY="${1:-q1}"

die() { echo "LOGICAL_SPEC_EXPORT_OK FAIL: $*" >&2; exit 1; }
[[ "$QUERY" == "q1" ]] || die "only q1 supported this pass"

DUMP="$ROOT/tests/verification/specs/$QUERY/logical_spec.dump.json"
DFY="$ROOT/tests/verification/q1/dafny/generated/LogicalSpecData.dfy"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

[[ -f "$DUMP" ]] || die "missing committed dump $DUMP"
[[ -f "$DFY" ]] || die "missing committed $DFY"

# Snapshot committed artifacts BEFORE regenerating (fidelity to tree, not only determinism).
cp "$DUMP" "$WORK/committed.dump.json"
cp "$DFY" "$WORK/committed.LogicalSpecData.dfy"

bash "$FSQL/scripts/dump_logical_spec_json.sh" "$QUERY"
diff -u "$WORK/committed.dump.json" "$DUMP" \
  || die "logical_spec.dump.json differs from committed (update dump after Rocq DATA change)"

python3 "$FSQL/scripts/emit_logical_spec_dafny.py" "$QUERY"
diff -u "$WORK/committed.LogicalSpecData.dfy" "$DFY" \
  || die "LogicalSpecData.dfy differs from committed (update generated DATA after dump change)"

# Pin: Dafny header sha must match hash of template+catalog_params.
PIN="$(python3 - <<PY
import hashlib, json
from pathlib import Path
w = json.loads(Path("$DUMP").read_text())
body = w.get("export") or w.get("logical_spec")
if "template" in body:
    pin_obj = {
        "template": body["template"],
        "catalog_params": body["catalog_params"],
        "catalog_delta": body.get("catalog_delta", 90),
    }
else:
    pin_obj = body
print(hashlib.sha256(json.dumps(pin_obj, separators=(",", ":")).encode()).hexdigest())
PY
)"
grep -q "source_sha256=$PIN" "$DFY" \
  || die "Dafny pin mismatch (expected source_sha256=$PIN)"

grep -q "logical_spec_expr_ir_v2" "$DUMP" || die "dump missing expr-IR v2"
grep -q "EParam\|ParamCutoff" "$DFY" || die "Dafny data missing EParam"
grep -q "EMul\|EColumn\|AggSum" "$DFY" || die "Dafny data missing expression constructors"
if grep -nE '\bPack\b' "$DFY"; then
  die "LogicalSpec DATA must not mention Pack"
fi
# Fail-closed: template must not bake cutoff as EConst in filter.
if grep -nE 'PLe\(EColumn\(ColShipdate\), EConst\(' "$DFY"; then
  die "Q1 filter must use EParam(ParamCutoff), not EConst cutoff"
fi

# Fail-closed: Dafny module must verify (data-only).
DAFNY_BIN="${DAFNY_BIN:-$ROOT/tools/dafny/dafny}"
[[ -x "$DAFNY_BIN" ]] || die "dafny not found at $DAFNY_BIN"
"$DAFNY_BIN" verify "$DFY" || die "dafny verify LogicalSpecData.dfy failed"

echo "LOGICAL_SPEC_EXPORT_OK (Rocq serialize → JSON → Dafny DATA; pin=$PIN)"
