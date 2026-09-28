#!/usr/bin/env bash
# SPEC_EXPORT_CHECKED / SpecExportCorrect (Q1) — RelNode-canonical.
#
# Distinguishes four surfaces (do not conflate):
#
#   (A) Canonical RelNode LogicalSpec DATA transcription (NOT a semantic proof):
#         Rocq LogicalSpec_Q1_rel → JSON → Q1RelNodeData.dfy; regen+diff+hash
#         Gate: check_relnode_logical_spec_export.sh
#
#   (B) Canonical RelNode interpreter correspondence TCB:
#         formalsql/logical/*  ↔  tests/verification/logical/dafny/*
#         Manifest: interpreter_correspondence_manifest.txt
#         Vocab:    spec_export_allowed_leaves.txt
#         Pins:     interpreter_source_relnode.pin
#
#   (C) Query-specific adapter correspondence boundary (SEPARATE residual TCB):
#         Rocq Q1Adapt ↔ Dafny Q1AdaptRel + RowOfAux / DbOfAux / ParamCutoff
#         Presence only — NOT semantic equivalence. Being outside (B) does NOT
#         mean adapters are proved; they remain an explicit correspondence gap.
#
#   (D) Flat expression-IR migration / reference scaffolding:
#         Flat DATA: check_logical_spec_export.sh (LogicalSpec_Q1 → LogicalSpecData.dfy)
#         Flat duals/pins/vocab: *_migration_flat.*
#
# IMPORTANT: verifying that dual *names* exist is NOT semantic equivalence.
# Cross-prover agreement of those implementations remains residual TCB.
#
# Explicitly NOT Spec authority:
#   EvalResultSpec / EvalSpec_Q1 / SpecApply / SpecFinalizeOne /
#   FinalizeKeys / SortByKey — migration/reference scaffolding only.
#
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"
FSQL="$ROOT/tests/verification/formalsql"
Q1DFY="$ROOT/tests/verification/q1/dafny"
REL_PIN="$FSQL/resultspec/interpreter_source_relnode.pin"
FLAT_PIN="$FSQL/resultspec/interpreter_source_migration_flat.pin"

die() { echo "SpecExportCorrect FAIL: $*" >&2; exit 1; }

echo "==> (A) Canonical RelNode LogicalSpec DATA transcription"
bash "$FSQL/scripts/check_relnode_logical_spec_export.sh" q1

DFY_REL_DATA="$Q1DFY/generated/Q1RelNodeData.dfy"
DUMP_REL="$ROOT/tests/verification/specs/q1/logical_spec_relnode.dump.json"
[[ -f "$DFY_REL_DATA" ]] || die "missing $DFY_REL_DATA"
[[ -f "$DUMP_REL" ]] || die "missing $DUMP_REL"
grep -q "LogicalSpec_Q1_rel" "$DFY_REL_DATA" \
  || die "Q1RelNodeData.dfy missing LogicalSpec_Q1_rel"
grep -q "EParam" "$DFY_REL_DATA" \
  || die "Q1RelNodeData.dfy missing EParam"
grep -q "logical_spec_relnode_v1" "$DUMP_REL" \
  || die "logical_spec_relnode.dump.json missing version logical_spec_relnode_v1"
grep -q "source_sha256=" "$DFY_REL_DATA" \
  || die "Q1RelNodeData.dfy missing source_sha256 pin"
if grep -nE '\bPack\b' "$DFY_REL_DATA"; then
  die "RelNode LogicalSpec DATA must not mention Pack"
fi
# Spec module must consume generated DATA (no handwritten tree body).
grep -q 'Q1RelNodeData\|Data\.LogicalSpec_Q1_rel' "$Q1DFY/Q1RelNodeSpec.dfy" \
  || die "Q1RelNodeSpec.dfy must consume generated Q1RelNodeData"
if grep -nE 'RelSort\(\s*RelProject\(\s*RelAggregate' "$Q1DFY/Q1RelNodeSpec.dfy"; then
  die "Q1RelNodeSpec.dfy must not hand-duplicate RelNode Q1 tree"
fi

echo "==> (B) Canonical RelNode interpreter correspondence TCB"
ROCQ_SQLTYPES="$FSQL/logical/SqlTypes.v"
ROCQ_SCHEMA="$FSQL/logical/Schema.v"
ROCQ_EXPR="$FSQL/logical/Expr.v"
ROCQ_AGG="$FSQL/logical/Agg.v"
ROCQ_REL="$FSQL/logical/RelNode.v"
ROCQ_TYPING="$FSQL/logical/Typing.v"
ROCQ_EVAL="$FSQL/logical/Eval.v"
DFY_EVAL="$ROOT/tests/verification/logical/dafny/EvalLogicalSpec.dfy"
DFY_LS="$ROOT/tests/verification/logical/dafny/LogicalSpec.dfy"
MANIFEST="$FSQL/resultspec/interpreter_correspondence_manifest.txt"
VOCAB="$FSQL/resultspec/spec_export_allowed_leaves.txt"

for f in "$ROCQ_SQLTYPES" "$ROCQ_SCHEMA" "$ROCQ_EXPR" "$ROCQ_AGG" "$ROCQ_REL" \
         "$ROCQ_TYPING" "$ROCQ_EVAL" "$DFY_EVAL" "$DFY_LS" "$MANIFEST" "$VOCAB" "$REL_PIN"; do
  [[ -f "$f" ]] || die "missing $f"
done

python3 - "$MANIFEST" "$VOCAB" \
  "$ROCQ_SQLTYPES" "$ROCQ_SCHEMA" "$ROCQ_EXPR" "$ROCQ_AGG" "$ROCQ_REL" "$ROCQ_TYPING" "$ROCQ_EVAL" \
  "$DFY_EVAL" "$DFY_LS" <<'PY'
import re, sys
from pathlib import Path

# argv: manifest vocab SqlTypes Schema Expr Agg RelNode Typing Eval dfy_eval dfy_ls
args = [Path(p) for p in sys.argv[1:]]
manifest_path, vocab_path = args[0], args[1]
rocq = "\n".join(p.read_text() for p in args[2:9])
dfy = "\n".join(p.read_text() for p in args[9:11])

FORBIDDEN = {
    "SpecApply", "SpecFinalizeOne", "SortByKey", "FinalizeKeys",
    "EvalResultSpec", "EvalSpec_Q1",
}

def load_nonempty_lines(path: Path):
    out = []
    for raw in path.read_text().splitlines():
        line = raw.split("#", 1)[0].strip()
        if line:
            out.append(line)
    return out

pairs = []
for line in load_nonempty_lines(manifest_path):
    parts = line.split()
    if len(parts) != 2:
        print(f"SpecExportCorrect FAIL: bad RelNode manifest line: {line!r}",
              file=sys.stderr)
        sys.exit(1)
    pairs.append((parts[0], parts[1]))

bad = [f"{r} ↔ {d}" for r, d in pairs if r in FORBIDDEN or d in FORBIDDEN]
if bad:
    print("SpecExportCorrect FAIL: RelNode manifest contains legacy authority:",
          ", ".join(bad), file=sys.stderr)
    sys.exit(1)

def rocq_defined(name: str, text: str) -> bool:
    return bool(re.search(rf"\b(Fixpoint|Definition)\s+{re.escape(name)}\b", text))

def dafny_defined(name: str, text: str) -> bool:
    if re.search(
        rf"\bghost\s+(function|predicate)\s+(\{{[^}}]*\}}\s*)?{re.escape(name)}\b",
        text,
    ):
        return True
    if re.search(rf"\bghost\s+function\s+\{{:opaque\}}\s+{re.escape(name)}\b", text):
        return True
    return False

missing = []
for r, d in pairs:
    if not rocq_defined(r, rocq):
        missing.append(f"Rocq missing def {r}")
    if not dafny_defined(d, dfy):
        missing.append(f"Dafny missing def {d}")
if missing:
    print("SpecExportCorrect FAIL: RelNode interpreter correspondence gaps:",
          file=sys.stderr)
    for m in missing:
        print(" ", m, file=sys.stderr)
    sys.exit(1)

print("RelNode interpreter correspondence pairs present (NOT semantic equivalence):")
for r, d in pairs:
    print(f"  {r}  ↔  {d}")

vocab = load_nonempty_lines(vocab_path)
vbad = [v for v in vocab if v in FORBIDDEN]
if vbad:
    print("SpecExportCorrect FAIL: RelNode vocabulary lists legacy authority:",
          ", ".join(vbad), file=sys.stderr)
    sys.exit(1)
missing_v = [v for v in vocab if v not in rocq or v not in dfy]
if missing_v:
    print("SpecExportCorrect FAIL: RelNode constructor vocabulary missing:",
          ", ".join(missing_v), file=sys.stderr)
    sys.exit(1)
print("RelNode constructor vocabulary present (presence ≠ semantic bound):",
      ", ".join(vocab))
print("NOTE: name presence / dual listing is NOT Rocq↔Dafny semantic equivalence.")
PY

python3 - "$REL_PIN" \
  "$ROCQ_SQLTYPES" "$ROCQ_SCHEMA" "$ROCQ_EXPR" "$ROCQ_AGG" "$ROCQ_REL" \
  "$ROCQ_TYPING" "$ROCQ_EVAL" "$DFY_LS" "$DFY_EVAL" <<'PY'
import hashlib, sys
from pathlib import Path
pin_path = Path(sys.argv[1])
paths = [Path(p) for p in sys.argv[2:]]
expected = {}
for line in pin_path.read_text().splitlines():
    line = line.split("#", 1)[0].strip()
    if not line or "=" not in line:
        continue
    k, v = line.split("=", 1)
    expected[k.strip()] = v.strip()
actual = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}
errs = []
for k, hv in expected.items():
    if k not in actual:
        errs.append(f"pin key {k} not among hashed sources {sorted(actual)}")
    elif actual[k] != hv:
        errs.append(f"{k}: pin={hv} actual={actual[k]} (update pin after TCB review)")
for k in actual:
    if k not in expected:
        errs.append(f"unpinned source {k}={actual[k]} (add to {pin_path.name})")
if errs:
    print("SpecExportCorrect FAIL: RelNode source pin mismatch:", file=sys.stderr)
    for e in errs:
        print(" ", e, file=sys.stderr)
    sys.exit(1)
print("RelNode interpreter source pins OK (PRIMARY):",
      ", ".join(f"{k}={actual[k][:12]}…" for k in sorted(actual)))
PY

echo "==> (C) Query-specific adapter correspondence boundary (separate from A/B)"
# Section (C) is its own residual correspondence surface: Rocq↔Dafny adapter
# duals for row/schema/output bridging. Being outside the generic-interpreter
# TCB does NOT mean these adapters are proved equivalent — presence only.
ROCQ_ADAPT="$FSQL/q1/Q1Adapt.v"
DFY_ADAPT="$Q1DFY/Q1RelNodeSpec.dfy"
[[ -f "$ROCQ_ADAPT" ]] || die "missing $ROCQ_ADAPT"
grep -qE '\b(Fixpoint|Definition)\s+Q1Adapt\b' "$ROCQ_ADAPT" \
  || die "Rocq Q1Adapt missing in Q1Adapt.v"
grep -qE '\bghost\s+function\s+Q1AdaptRel\b' "$DFY_ADAPT" \
  || die "Dafny Q1AdaptRel missing in Q1RelNodeSpec.dfy"
grep -qE '\bghost\s+function\s+RowOfAux\b' "$DFY_ADAPT" \
  || die "Q1RelNodeSpec.dfy missing RowOfAux"
grep -qE '\bghost\s+function\s+DbOfAux\b' "$DFY_ADAPT" \
  || die "Q1RelNodeSpec.dfy missing DbOfAux"
grep -qE '\bghost\s+function\s+ParamCutoff\b' "$DFY_ADAPT" \
  || die "Q1RelNodeSpec.dfy missing ParamCutoff"
echo "Query-specific adapter correspondence present (NOT semantic equivalence):"
echo "  Rocq Q1Adapt ↔ Dafny Q1AdaptRel"
echo "  Dafny row/schema adapters: RowOfAux, DbOfAux, ParamCutoff"
echo "  Boundary: separate from (A) DATA and (B) generic RelNode interpreter TCB."
echo "  Presence here does not prove Rocq↔Dafny adapter agreement."

echo "==> (D) Flat expression-IR migration / reference scaffolding"
bash "$FSQL/scripts/check_logical_spec_export.sh" q1

FLAT_MANIFEST="$FSQL/resultspec/interpreter_correspondence_manifest_migration_flat.txt"
FLAT_VOCAB="$FSQL/resultspec/spec_export_allowed_leaves_migration_flat.txt"
FLAT_ROCQ_EVAL="$FSQL/resultspec/EvalLogicalSpec.v"
FLAT_ROCQ_LS="$FSQL/resultspec/LogicalSpec.v"
FLAT_DFY_EVAL="$Q1DFY/EvalLogicalSpec.dfy"
FLAT_DFY_DATA="$Q1DFY/generated/LogicalSpecData.dfy"

[[ -f "$FLAT_PIN" ]] || die "missing $FLAT_PIN"
[[ -f "$FLAT_MANIFEST" ]] || die "missing $FLAT_MANIFEST"
[[ -f "$FLAT_VOCAB" ]] || die "missing $FLAT_VOCAB"
[[ -f "$FLAT_DFY_DATA" ]] || die "missing $FLAT_DFY_DATA"

grep -q "LogicalSpec_Q1" "$FLAT_DFY_DATA" \
  || die "flat LogicalSpecData.dfy missing LogicalSpec_Q1"
grep -q "EParam" "$FLAT_DFY_DATA" \
  || die "flat LogicalSpecData.dfy missing EParam"
grep -q "ParamCutoff" "$FLAT_DFY_DATA" \
  || die "flat LogicalSpecData.dfy missing ParamCutoff"
if grep -nE '\bPack\b' "$FLAT_DFY_DATA"; then
  die "flat LogicalSpec DATA must not mention Pack"
fi

python3 - "$FLAT_MANIFEST" "$FLAT_VOCAB" "$FLAT_ROCQ_EVAL" "$FLAT_DFY_EVAL" "$FLAT_ROCQ_LS" <<'PY'
import re, sys
from pathlib import Path

manifest_path, vocab_path, rocq_eval, dfy_eval, rocq_ls = map(Path, sys.argv[1:6])
rocq = rocq_eval.read_text() + "\n" + rocq_ls.read_text()
dfy = dfy_eval.read_text()

FORBIDDEN = {
    "SpecApply", "SpecFinalizeOne", "SortByKey", "FinalizeKeys",
    "EvalResultSpec", "EvalSpec_Q1",
}

def load_nonempty_lines(path: Path):
    out = []
    for raw in path.read_text().splitlines():
        line = raw.split("#", 1)[0].strip()
        if line:
            out.append(line)
    return out

pairs = []
for line in load_nonempty_lines(manifest_path):
    parts = line.split()
    if len(parts) != 2:
        print(f"SpecExportCorrect FAIL: bad flat migration manifest line: {line!r}",
              file=sys.stderr)
        sys.exit(1)
    pairs.append((parts[0], parts[1]))

bad = [f"{r} ↔ {d}" for r, d in pairs if r in FORBIDDEN or d in FORBIDDEN]
if bad:
    print("SpecExportCorrect FAIL: flat migration manifest contains legacy authority:",
          ", ".join(bad), file=sys.stderr)
    sys.exit(1)

def rocq_defined(name: str, text: str) -> bool:
    return bool(re.search(rf"\b(Fixpoint|Definition)\s+{re.escape(name)}\b", text))

def dafny_defined(name: str, text: str) -> bool:
    if re.search(
        rf"\bghost\s+(function|predicate)\s+(\{{[^}}]*\}}\s*)?{re.escape(name)}\b",
        text,
    ):
        return True
    if re.search(rf"\bghost\s+function\s+\{{:opaque\}}\s+{re.escape(name)}\b", text):
        return True
    return False

missing = []
for r, d in pairs:
    # Skip query-specific Q1Adapt in flat migration manifest — adapters checked in (C)
    if r == "Q1Adapt" or d == "Q1Adapt":
        continue
    if not rocq_defined(r, rocq):
        missing.append(f"Rocq missing def {r}")
    if not dafny_defined(d, dfy):
        missing.append(f"Dafny missing def {d}")
if missing:
    print("SpecExportCorrect FAIL: flat migration interpreter gaps:", file=sys.stderr)
    for m in missing:
        print(" ", m, file=sys.stderr)
    sys.exit(1)

print("Flat migration interpreter pairs present (scaffolding only):")
for r, d in pairs:
    print(f"  {r}  ↔  {d}")

vocab = load_nonempty_lines(vocab_path)
vbad = [v for v in vocab if v in FORBIDDEN]
if vbad:
    print("SpecExportCorrect FAIL: flat vocab lists legacy authority:",
          ", ".join(vbad), file=sys.stderr)
    sys.exit(1)
missing_v = [v for v in vocab if v not in rocq or v not in dfy]
if missing_v:
    print("SpecExportCorrect FAIL: flat constructor vocabulary missing:",
          ", ".join(missing_v), file=sys.stderr)
    sys.exit(1)
print("Flat migration constructor vocabulary present:", ", ".join(vocab))
PY

python3 - "$FLAT_PIN" "$FLAT_ROCQ_LS" "$FLAT_ROCQ_EVAL" "$FLAT_DFY_EVAL" <<'PY'
import hashlib, sys
from pathlib import Path
pin_path = Path(sys.argv[1])
paths = [Path(p) for p in sys.argv[2:]]
expected = {}
for line in pin_path.read_text().splitlines():
    line = line.split("#", 1)[0].strip()
    if not line or "=" not in line:
        continue
    k, v = line.split("=", 1)
    expected[k.strip()] = v.strip()
actual = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}
errs = []
for k, hv in expected.items():
    if k not in actual:
        errs.append(f"pin key {k} not among hashed sources {sorted(actual)}")
    elif actual[k] != hv:
        errs.append(f"{k}: pin={hv} actual={actual[k]}")
for k in actual:
    if k not in expected:
        errs.append(f"unpinned source {k}={actual[k]}")
if errs:
    print("SpecExportCorrect FAIL: flat migration source pin mismatch:", file=sys.stderr)
    for e in errs:
        print(" ", e, file=sys.stderr)
    sys.exit(1)
print("Flat migration interpreter source pins OK:",
      ", ".join(f"{k}={actual[k][:12]}…" for k in sorted(actual)))
PY

LEGACY_DFY="$Q1DFY/generated/Q1ResultSpec.dfy"
if [[ -f "$LEGACY_DFY" ]]; then
  grep -q "scaffolding\|not Spec authority\|not canonical\|MIGRATION" "$LEGACY_DFY" \
    || die "Q1ResultSpec.dfy must declare migration/reference scaffolding (not Spec authority)"
fi

echo "SPEC_EXPORT_CHECKED OK"
echo "  (A) RelNode DATA pin + (B) RelNode interpreter TCB + (C) adapter correspondence + (D) flat migration"
echo "  Reminder: dual/adapter presence is NOT Rocq↔Dafny semantic equivalence (residual TCB)."
echo "  Primary pin: interpreter_source_relnode.pin"
