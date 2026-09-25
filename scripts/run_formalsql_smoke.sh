#!/usr/bin/env bash
# Fail-closed FormalSQL / Logos smoke for JetStream verification.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LOGOS="${LOGOS_ROOT:-$ROOT/third_party/Logos}"
PIN="$ROOT/tests/verification/formalsql/PIN.md"
SMOKE_V="$ROOT/tests/verification/formalsql/scripts/Smoke.v"

die() { echo "formalsql smoke FAIL: $*" >&2; exit 1; }

[[ -f "$PIN" ]] || die "missing PIN.md"
[[ -d "$LOGOS" ]] || die "missing Logos at $LOGOS (clone pin into third_party/Logos)"
[[ -d "$LOGOS/vendor/FormalSQL" ]] || die "missing FormalSQL submodule under Logos"

EXPECTED_LOGOS="333d5275ac871f5b36ce3a7dc5f6fef5f7879069"
EXPECTED_FSQL="2238975a4a989c844ade5b75d9d0414295fbdfd1"

ACTUAL_LOGOS="$(git -C "$LOGOS" rev-parse HEAD)"
ACTUAL_FSQL="$(git -C "$LOGOS/vendor/FormalSQL" rev-parse HEAD)"

[[ "$ACTUAL_LOGOS" == "$EXPECTED_LOGOS" ]] || \
  die "Logos SHA mismatch: got $ACTUAL_LOGOS want $EXPECTED_LOGOS"
[[ "$ACTUAL_FSQL" == "$EXPECTED_FSQL" ]] || \
  die "FormalSQL SHA mismatch: got $ACTUAL_FSQL want $EXPECTED_FSQL"

if [[ -z "${ROCQ_OPAM_SWITCH:-}" ]]; then
  # Prefer Logos local switch (default in Logos Makefile / .env.example).
  if [[ -d "$LOGOS/.opam-rocq/_opam" || -L "$LOGOS/.opam-rocq/_opam" ]]; then
    export ROCQ_OPAM_SWITCH="$LOGOS/.opam-rocq"
  elif command -v opam >/dev/null 2>&1 && opam switch list --short 2>/dev/null | grep -qx logos-rocq; then
    export ROCQ_OPAM_SWITCH=logos-rocq
  fi
fi

if [[ -z "${ROCQ_OPAM_SWITCH:-}" ]] || ! command -v opam >/dev/null 2>&1; then
  die "Rocq opam switch not configured (set ROCQ_OPAM_SWITCH and install opam). Pin OK but smoke cannot compile."
fi

FORMALSQL_SRC="$LOGOS/vendor/FormalSQL/src"
[[ -f "$SMOKE_V" ]] || die "missing $SMOKE_V"

# Local switches use <switch>/_opam; named global switches use ~/.opam/<name>.
if [[ -d "$ROCQ_OPAM_SWITCH/_opam" || -L "$ROCQ_OPAM_SWITCH/_opam" ]]; then
  _rocq_prefix="$ROCQ_OPAM_SWITCH/_opam"
else
  _rocq_prefix="$(opam var prefix --switch="$ROCQ_OPAM_SWITCH")"
fi
export ROCQLIB="${ROCQLIB:-$_rocq_prefix/lib/coq}"
export COQLIB="${COQLIB:-$ROCQLIB}"
export OCAMLFIND_CONF="${OCAMLFIND_CONF:-$_rocq_prefix/lib/findlib.conf}"

opam exec --switch="$ROCQ_OPAM_SWITCH" -- rocq compile \
  -Q "$FORMALSQL_SRC" SQLFS \
  "$SMOKE_V"

echo "formalsql smoke OK (Logos=$ACTUAL_LOGOS FormalSQL=$ACTUAL_FSQL)"
