#!/usr/bin/env bash
# Q21 SpecExportCorrect (A-RelNode): RelNode LogicalSpec DATA transcription.
# NOT a semantic proof.  Checks, for the committed artifacts:
#   (1) Rocq Compute of serialize_LogicalSpec_Q21_rel_export_json (from
#       LogicalSpec_Q21_rel) == specs/q21/logical_spec_relnode.dump.json export;
#   (2) the dump tree, rendered to Dafny LogicalSpec constructors, equals the
#       constant in q21/dafny/generated/Q21RelNodeData.dfy;
#   (3) the Dafny header pin == sha256(sorted compact export JSON).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
LOGOS="${LOGOS_ROOT:-$ROOT/third_party/Logos}"
FSQL="$ROOT/tests/verification/formalsql"
SWITCH="${ROCQ_OPAM_SWITCH:-$LOGOS/.opam-rocq}"
DUMP="$ROOT/tests/verification/specs/q21/logical_spec_relnode.dump.json"
DFY="$ROOT/tests/verification/q21/dafny/generated/Q21RelNodeData.dfy"
die() { echo "Q21_RELNODE_EXPORT FAIL: $*" >&2; exit 1; }
[[ -f "$DUMP" && -f "$DFY" ]] || die "missing committed dump or Dafny data"
WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT

if [[ -d "$SWITCH/_opam" || -L "$SWITCH/_opam" ]]; then _prefix="$SWITCH/_opam"; else _prefix="$(opam var prefix --switch="$SWITCH")"; fi
export ROCQLIB="$_prefix/lib/coq" COQLIB="$_prefix/lib/coq" OCAMLFIND_CONF="$_prefix/lib/findlib.conf"
FLAGS=( -Q "$FSQL/q21" "" -Q "$LOGOS/vendor/FormalSQL/src" SQLFS -Q "$LOGOS/theories" Logos
  -Q "$FSQL/lib" FormalSQLLib -Q "$FSQL/logical" FormalSQLLogical -Q "$FSQL/resultspec" FormalSQLResultSpec
  -Q "$FSQL/q21/generated" LogosGenerated -Q "$FSQL/generated" JetStreamFS )
cat > "$WORK/dump.v" <<'V'
Require Import Q21LogicalSpecSerialize.
From Stdlib Require Import String.
Eval vm_compute in serialize_LogicalSpec_Q21_rel_export_json.
V
(cd "$FSQL/q21" && opam exec --switch="$SWITCH" -- rocq repl -q "${FLAGS[@]}" < "$WORK/dump.v") > "$WORK/rocq.out" 2>&1 \
  || die "rocq serialization failed"

python3 - "$WORK/rocq.out" "$DUMP" "$DFY" <<'PY' || exit 1
import hashlib, json, re, sys
out, dump_path, dfy_path = sys.argv[1:4]
txt = open(out).read()
m = re.search(r'=\s*"(.*)"\s*%string', txt, re.S)
if not m:
    sys.exit("Q21_RELNODE_EXPORT FAIL: could not parse Rocq output")
rocq_json = json.loads(m.group(1).replace('""', '"'))
dump = json.load(open(dump_path))
if rocq_json != dump["export"]:
    sys.exit("Q21_RELNODE_EXPORT FAIL: Rocq serialization differs from committed dump")

BIN = {"eq": "BEq", "ne": "BNe", "lt": "BLt", "le": "BLe", "gt": "BGt", "ge": "BGe",
       "and": "BAnd", "or": "BOr", "add": "BAdd", "sub": "BSub", "mul": "BMul", "div": "BDiv"}
def ty(t): return {"text": "TText", "int": "TInt", "bool": "TBool", "date": "TDate"}[t["kind"]]
def val(v):
    if v["kind"] == "text": return 'VText("%s")' % v["value"]
    if v["kind"] == "int": return "VInt(%d)" % v["value"]
    raise SystemExit("unsupported value %r" % v)
def ex(e):
    k = e["kind"]
    if k == "input_ref": return "EInputRef(%d)" % e["slot"]
    if k == "group_ref": return "EGroupRef(%d)" % e["i"]
    if k == "agg_ref": return "EAggRef(%d)" % e["i"]
    if k == "const": return "EConst(%s, %s)" % (val(e["value"]), ty(e["ty"]))
    if k == "param": return 'EParam("%s", %s)' % (e["id"], ty(e["ty"]))
    if k == "binary": return "EBinary(%s, %s, %s)" % (BIN[e["op"]], ex(e["left"]), ex(e["right"]))
    raise SystemExit("unsupported expr %r" % k)
def agg(a):
    if a["kind"] == "count_star": return "AggSpec(AggCountStar, NoExpr)"
    raise SystemExit("unsupported agg")
def ok(o):
    return "OrderKey(%s, %s, %s)" % (ex(o["expr"]), {"asc": "SortAsc", "desc": "SortDesc"}[o["dir"]],
                                     {"first": "NullsFirst", "last": "NullsLast"}[o["nulls"]])
def rel(n):
    k = n["kind"]
    if k == "scan": return 'RelScan("%s")' % n["table"]
    if k == "filter": return "RelFilter(%s, %s)" % (rel(n["input"]), ex(n["pred"]))
    if k == "project": return "RelProject(%s, [%s])" % (rel(n["input"]), ", ".join(ex(e) for e in n["exprs"]))
    if k == "join": return "RelJoin(%s, %s, %s, %s)" % ({"inner": "JoinInner"}[n["join_kind"]], rel(n["left"]), rel(n["right"]), ex(n["cond"]))
    if k == "semi_join": return "RelSemiJoin(%s, %s, %s)" % (rel(n["left"]), rel(n["right"]), ex(n["cond"]))
    if k == "anti_join": return "RelAntiJoin(%s, %s, %s)" % (rel(n["left"]), rel(n["right"]), ex(n["cond"]))
    if k == "aggregate": return "RelAggregate(%s, [%s], [%s])" % (rel(n["input"]), ", ".join(ex(e) for e in n["group_exprs"]), ", ".join(agg(a) for a in n["aggs"]))
    if k == "sort": return "RelSort(%s, [%s])" % (rel(n["input"]), ", ".join(ok(o) for o in n["order_keys"]))
    raise SystemExit("unsupported node %r" % k)
rendered = rel(dump["export"]["tree"])
dfy = open(dfy_path).read()
mm = re.search(r"const LogicalSpec_Q21_rel: LogicalSpec :=\s*(.*?)\n\}", dfy, re.S)
if not mm or re.sub(r"\s+", "", mm.group(1)) != re.sub(r"\s+", "", rendered):
    sys.exit("Q21_RELNODE_EXPORT FAIL: Dafny constant differs from dump tree")
pin = hashlib.sha256(json.dumps(dump["export"], separators=(",", ":"), sort_keys=True).encode()).hexdigest()
if ("source_sha256=" + pin) not in dfy:
    sys.exit("Q21_RELNODE_EXPORT FAIL: Dafny pin mismatch (expected %s)" % pin)
print("Q21_RELNODE_EXPORT_OK (Rocq == dump == Dafny DATA; pin=%s)" % pin)
PY
