#!/usr/bin/env bash
# Dump LogicalSpec JSON via Rocq Compute of serialize_LogicalSpec_Q1_export_json.
# Fail-closed: normalize_q1(catalog) must be Some LogicalSpec_Q1 (parameterized).
# Usage: dump_logical_spec_json.sh q1
# Writes tests/verification/specs/<q>/logical_spec.dump.json
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"
LOGOS="${LOGOS_ROOT:-$ROOT/third_party/Logos}"
FSQL="$ROOT/tests/verification/formalsql"
SWITCH="${ROCQ_OPAM_SWITCH:-$LOGOS/.opam-rocq}"
QUERY="${1:?usage: $0 q1}"

die() { echo "dump_logical_spec_json FAIL: $*" >&2; exit 1; }
command -v opam >/dev/null || die "opam not found"
command -v python3 >/dev/null || die "python3 not found"

OUT_DIR="$ROOT/tests/verification/specs/$QUERY"
mkdir -p "$OUT_DIR"
OUT="$OUT_DIR/logical_spec.dump.json"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

case "$QUERY" in
  q1)
    cd "$FSQL/q1"
    FLAGS=(
      -Q "$LOGOS/vendor/FormalSQL/src" SQLFS
      -Q "$LOGOS/theories" Logos
      -Q "$FSQL/lib" FormalSQLLib
      -Q "$FSQL/logical" FormalSQLLogical
      -Q "$FSQL/resultspec" FormalSQLResultSpec
      -Q "$FSQL/q1/generated" LogosGenerated
      -Q "$FSQL/generated" JetStreamFS
    )
    cat >"$WORK/dump.v" <<'EOF'
From FormalSQLResultSpec Require Import
  NormalizeCompositional LogicalSpec LogicalSpecSerialize QueryResultSpec.
From LogosGenerated Require Import Queries.
Require Import Q1FormalSQLProof.
Require Import Q1LogicalNormalize.
From Stdlib Require Import ZArith String.
Open Scope Z_scope.
Open Scope string_scope.

(* Fail closed: normalize must return the parameterized Q1 template. *)
Goal normalize_q1 Queries.source_query_expr = Some LogicalSpec_Q1.
Proof. exact normalize_q1_catalog. Qed.

Compute (serialize_LogicalSpec_Q1_export_json 90).
EOF
    ;;
  *) die "unsupported query $QUERY (q1 only this pass)" ;;
esac

opam exec --switch="$SWITCH" -- rocq compile "${FLAGS[@]}" "$WORK/dump.v" \
  >"$WORK/out.txt" 2>"$WORK/err.txt" || {
  cat "$WORK/err.txt" >&2
  die "rocq compile dump failed"
}

python3 - "$OUT" "$WORK/out.txt" "$WORK/err.txt" <<'PY'
import json, re, sys
out_path, log_path, err_path = sys.argv[1], sys.argv[2], sys.argv[3]
text = open(log_path).read() + "\n" + open(err_path).read()
m = re.search(r'=\s*"(.*?)"\s*:\s*string', text, re.S)
if not m:
    m = re.search(r'=\s*"(.*?)"%string', text, re.S)
if not m:
    sys.stderr.write(text[-4000:])
    sys.exit("dump_logical_spec_json FAIL: could not parse Compute JSON string")
raw = m.group(1)
raw = raw.replace('""', '"')
try:
    body = json.loads(raw)
except json.JSONDecodeError as e:
    sys.stderr.write(raw[:500] + "\n")
    sys.exit(f"dump_logical_spec_json FAIL: invalid JSON from Rocq: {e}")
if body.get("version") != "logical_spec_expr_ir_v2":
    sys.exit("dump_logical_spec_json FAIL: unexpected version")
tmpl = body.get("template")
if not isinstance(tmpl, dict):
    sys.exit("dump_logical_spec_json FAIL: missing template")
if tmpl.get("well_scaled") is not True:
    sys.exit("dump_logical_spec_json FAIL: well_scaled is not true")
# Template filter must use param, not baked const cutoff.
filt = json.dumps(tmpl.get("filters", []))
if "param" not in filt:
    sys.exit("dump_logical_spec_json FAIL: template filter missing param")
if '"kind": "const"' in filt or '"kind":"const"' in filt:
    sys.exit("dump_logical_spec_json FAIL: template filter must not bake const cutoff")
cp = body.get("catalog_params") or {}
if cp.get("cutoff") != 10471:
    sys.exit("dump_logical_spec_json FAIL: expected catalog cutoff 10471")
if body.get("catalog_delta") != 90:
    sys.exit("dump_logical_spec_json FAIL: expected catalog_delta 90")
wrapped = {
    "authority": "rocq_serialize_LogicalSpec_Q1_json",
    "query": "q1",
    "export": body,
}
open(out_path, "w").write(json.dumps(wrapped, indent=2, sort_keys=False) + "\n")
print(f"dump_logical_spec_json OK -> {out_path}")
PY
