#!/usr/bin/env bash
# Print Assumptions on the final Rocq correctness theorems and diff against
# tests/verification/formalsql/<query>/allowed_assumptions.txt.
# Usage: check_print_assumptions.sh q1
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"
LOGOS="${LOGOS_ROOT:-$ROOT/third_party/Logos}"
FSQL="$ROOT/tests/verification/formalsql"
SWITCH="${ROCQ_OPAM_SWITCH:-$LOGOS/.opam-rocq}"
QUERY="${1:?usage: $0 q1|q21}"

die() { echo "PRINT_ASSUMPTIONS FAIL ($QUERY): $*" >&2; exit 1; }

case "$QUERY" in
  q1)
    cd "$FSQL/q1"
    REQUIRE="Require Import Q1NormalizeSound Q1AllOutcomes."
    THEOREMS=(NormalizeSound_Q1_all_outcomes NormalizeSound_Q1_catalog_all_outcomes
      Q1_qhat_outcomes_unique FormalSQLSpecCorrect_Q1 NormalizeSound_generic
      NormalizeSound_Q1)
    FLAGS=(
      -Q "$LOGOS/vendor/FormalSQL/src" SQLFS
      -Q "$LOGOS/theories" Logos
      -Q "$FSQL/lib" FormalSQLLib
      -Q "$FSQL/logical" FormalSQLLogical
      -Q "$FSQL/resultspec" FormalSQLResultSpec
      -Q "$FSQL/q1/generated" LogosGenerated
      -Q "$FSQL/generated" JetStreamFS
    )
    ;;
  q21)
    cd "$FSQL/q21"
    REQUIRE="Require Import Q21OuterOrdinarySpine Q21ExistsL2 Q21NotExistsL3 Q21WhereFusion Q21GroupCount Q21NormalizeSoundFinal Q21AllOutcomes."
    THEOREMS=(NormalizeSound_Q21_all_outcomes NormalizeSound_Q21_catalog_all_outcomes
      Q21_qhat_outcomes_unique NormalizeSound_Q21 NormalizeSound_Q21_catalog
      Q21_catalog_where_fusion Q21_GroupCountStageCorrespond
      Q21_NotExistsL3StageCorrespond Q21_ExistsL2StageCorrespond
      Q21_OuterOrdinarySpineCorrespond Q21_text_c_leb_string_compare)
    FLAGS=(
      -Q "$FSQL/q21" ""
      -Q "$LOGOS/vendor/FormalSQL/src" SQLFS
      -Q "$LOGOS/theories" Logos
      -Q "$FSQL/lib" FormalSQLLib
      -Q "$FSQL/logical" FormalSQLLogical
      -Q "$FSQL/resultspec" FormalSQLResultSpec
      -Q "$FSQL/q21/generated" LogosGenerated
      -Q "$FSQL/generated" JetStreamFS
    )
    ;;
  *)
    die "unsupported query $QUERY (q1 | q21)"
    ;;
esac

command -v opam >/dev/null || die "opam not found"
ALLOW="$FSQL/$QUERY/allowed_assumptions.txt"
[[ -f "$ALLOW" ]] || die "missing $ALLOW"

if [[ -d "$SWITCH/_opam" || -L "$SWITCH/_opam" ]]; then
  _prefix="$SWITCH/_opam"
else
  _prefix="$(opam var prefix --switch="$SWITCH")"
fi
export ROCQLIB="${ROCQLIB:-$_prefix/lib/coq}"
export COQLIB="${COQLIB:-$ROCQLIB}"
export OCAMLFIND_CONF="${OCAMLFIND_CONF:-$_prefix/lib/findlib.conf}"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
SCRIPT="$WORK/print_assumptions.v"
{
  echo "$REQUIRE"
  for th in "${THEOREMS[@]}"; do
    echo "Redirect \"${WORK}/${th}.txt\" Print Assumptions ${th}."
  done
} >"$SCRIPT"

opam exec --switch="$SWITCH" -- rocq repl -q -batch "${FLAGS[@]}" -l "$SCRIPT" \
  >"$WORK/rocq_top.out" 2>"$WORK/rocq_top.err" \
  || die "rocq top failed (see $WORK — rerun with the .vo files built)"

python3 - "$ALLOW" "$WORK" "${THEOREMS[@]}" <<'PY'
import sys
from pathlib import Path

allow_path = Path(sys.argv[1])
work = Path(sys.argv[2])
theorems = sys.argv[3:]

def allowlist(path: Path) -> set[str]:
    names = set()
    for line in path.read_text().splitlines():
        line = line.split("#", 1)[0].strip()
        if line:
            names.add(line)
    return names

def printed(text: str) -> set[str] | None:
    if "Closed under the global context" in text and "Axioms:" not in text:
        return set()
    names: set[str] = set()
    in_section = False
    saw = False
    for raw in text.splitlines():
        line = raw.strip()
        if line.endswith(":") and line[:-1] in {
            "Axioms", "Variables", "Section Variables", "Parameters", "Constants",
        }:
            in_section = True
            saw = True
            continue
        if not line:
            in_section = False
            continue
        if in_section:
            # Only axiom *names* look like `Ident : type` at column 0 of the
            # section; type continuations (e.g. `forall`) must be ignored.
            if ":" in line:
                tok = line.split(":", 1)[0].strip()
                if tok and " " not in tok and tok[0].isalpha():
                    names.add(tok)
    if not saw and "Closed under the global context" not in text:
        return None
    return names

allowed = allowlist(allow_path)
extra: list[str] = []
for th in theorems:
    path = work / f"{th}.txt"
    if not path.is_file():
        # Rocq Redirect may append .out to the target path.
        alt = work / f"{th}.txt.out"
        path = alt if alt.is_file() else path
    if not path.is_file():
        print(f"PRINT_ASSUMPTIONS FAIL: missing Print Assumptions output for {th}", file=sys.stderr)
        sys.exit(1)
    found = printed(path.read_text())
    if found is None:
        print(f"PRINT_ASSUMPTIONS FAIL: unparsed output for {th}", file=sys.stderr)
        print(path.read_text(), file=sys.stderr)
        sys.exit(1)
    unexpected = sorted(found - allowed)
    if unexpected:
        extra.append(f"{th}: {', '.join(unexpected)}")
    else:
        print(f"PRINT_ASSUMPTIONS {th}: {sorted(found) or ['closed']}")

if extra:
    print("PRINT_ASSUMPTIONS FAIL: assumptions outside allowlist", file=sys.stderr)
    for line in extra:
        print(line, file=sys.stderr)
    sys.exit(1)
print("PRINT_ASSUMPTIONS OK")
PY
