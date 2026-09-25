#!/usr/bin/env bash
# Rebuild Q12 lib deps + EmbedDots + spine in dependency order.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"
LOGOS="${LOGOS_ROOT:-$ROOT/third_party/Logos}"
FSQL="$ROOT/tests/verification/formalsql"
SWITCH="${ROCQ_OPAM_SWITCH:-$LOGOS/.opam-rocq}"
if [[ -d "$SWITCH/_opam" || -L "$SWITCH/_opam" ]]; then _prefix="$SWITCH/_opam"; else _prefix="$(opam var prefix --switch="$SWITCH")"; fi
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
cc() { echo "rocq compile $1"; opam exec --switch="$SWITCH" -- rocq compile "${FLAGS[@]}" "$1"; }
cd "$FSQL"
cc lib/FilterAcceptanceFacade.v
cc lib/TemporalPredAcceptanceFacade.v
cc lib/NumericCallExactFacade.v
cc lib/ProjectExactFacade.v
cc lib/GroupBagExactFacade.v
cc lib/GroupsExactFacade.v
cc lib/AggregateLeafExactFacade.v
cc lib/OrderRowsFacade.v
cc lib/QueryJoin2SpineFacade.v
cd "$FSQL/q12"
cc Q12NormalizeSound.v
[[ -f Q12EmbedDots.v ]] && cc Q12EmbedDots.v
[[ -f Q12WhereAcceptance.v ]] && cc Q12WhereAcceptance.v
[[ -f Q12CaseAcceptance.v ]] && cc Q12CaseAcceptance.v
[[ -f Q12GroupAcceptance.v ]] && cc Q12GroupAcceptance.v
cc Q12JoinSpine.v
echo "CC_ALL OK"
