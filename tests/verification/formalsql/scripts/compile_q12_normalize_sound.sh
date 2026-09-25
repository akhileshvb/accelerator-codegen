#!/usr/bin/env bash
# NORMALIZE_SOUND_Q12 — Eval_FS(qhat) = Embed(EvalSpec(S_Q12)) on ResultSpec path.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"
LOGOS="${LOGOS_ROOT:-$ROOT/third_party/Logos}"
FSQL="$ROOT/tests/verification/formalsql"
SWITCH="${ROCQ_OPAM_SWITCH:-$LOGOS/.opam-rocq}"

die() { echo "compile_q12_normalize_sound FAIL: $*" >&2; exit 1; }

command -v opam >/dev/null || die "opam not found"
[[ -f "$FSQL/q12/generated/Queries.v" ]] || die "missing q12 Queries.v"

if [[ -d "$SWITCH/_opam" || -L "$SWITCH/_opam" ]]; then
  _prefix="$SWITCH/_opam"
else
  _prefix="$(opam var prefix --switch="$SWITCH")"
fi
export ROCQLIB="${ROCQLIB:-$_prefix/lib/coq}"
export COQLIB="${COQLIB:-$ROCQLIB}"
export OCAMLFIND_CONF="${OCAMLFIND_CONF:-$_prefix/lib/findlib.conf}"

FLAGS=(
  -Q "$LOGOS/vendor/FormalSQL/src" SQLFS
  -Q "$LOGOS/theories" Logos
  -Q "$FSQL/lib" FormalSQLLib
  -Q "$FSQL/resultspec" FormalSQLResultSpec
  -Q "$FSQL/q12/generated" LogosGenerated
)

compile_one() {
  local f="$1"
  echo "rocq compile $f"
  opam exec --switch="$SWITCH" -- rocq compile "${FLAGS[@]}" "$f"
}

# Ensure normalize gate artifacts exist
"$FSQL/scripts/compile_q12_resultspec.sh"

cd "$FSQL"
compile_one resultspec/Q12EvalSpec.v
compile_one resultspec/Q12SemanticBridge.v
# Leaf facades used by the Q12 join-spine leaf discharge.
compile_one lib/FilterAcceptanceFacade.v
compile_one lib/TemporalPredAcceptanceFacade.v
compile_one lib/NumericCallExactFacade.v
compile_one lib/ProjectExactFacade.v
compile_one lib/CaseExactFacade.v
compile_one lib/GroupBagExactFacade.v
compile_one lib/GroupsExactFacade.v
compile_one lib/AggregateLeafExactFacade.v
compile_one lib/OrderRowsFacade.v
compile_one lib/QueryJoin2SpineFacade.v
compile_one lib/KeyedSortFacade.v
compile_one lib/SortedGroupResultFacade.v
cd "$FSQL/q12"
compile_one Q12NormalizeSound.v
compile_one Q12EmbedDots.v
compile_one Q12WhereAcceptance.v
compile_one Q12CaseAcceptance.v
compile_one Q12GroupAcceptance.v
compile_one Q12JoinSpine.v
compile_one Q12IdentityProject.v
[[ -f Q12GroupsCorrespond.v ]] && compile_one Q12GroupsCorrespond.v

# Q12_SQL_SPEC_PROVED: bag-sort + FormalSQL_exec of_holds Qed (no GroupRowsPerm residual)
if rg -n 'Q12_GroupBagsPerm orders lines ->' Q12NormalizeSound.v Q12JoinSpine.v Q12GroupsCorrespond.v 2>/dev/null; then
  echo "Q12_SQL_SPEC_PROVED pending (GroupBagsPerm still a hyp)"
elif rg -n 'Q12_GroupRowsPerm_CollectModes orders lines ->' Q12NormalizeSound.v Q12JoinSpine.v Q12GroupsCorrespond.v 2>/dev/null; then
  echo "Q12_GroupRowsPerm_CollectModes residual remains"
  echo "Q12_SQL_SPEC_PROVED pending"
else
  echo "Q12_SQL_SPEC_PROVED OK"
fi

echo "NORMALIZE_SOUND_Q12 OK"
