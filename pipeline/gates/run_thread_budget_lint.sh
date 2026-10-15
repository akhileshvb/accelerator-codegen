#!/usr/bin/env bash
# Thread-budget lint (contract C12): generated code must size every parallel
# step from the backend's one thread budget (services.WorkerCount() /
# ThreadBudget), never from the machine or from a fixed number.
#
# Usage: run_thread_budget_lint.sh [--strict] [--json <out.json>] [root ...]
#   default      report-only: prints offenders, exits 0
#   --strict     exits 1 when any offender is found
#   root ...     directories to scan (default: <repo>/accelerators <repo>/backends)
#
# Scanned: *.cpp *.cc *.hpp *.h under the roots. Skipped: substrate/, eval/,
# tests/, bench/, build dirs (the budget implementation and harnesses may read
# the hardware).
#
# Rules (pattern based, per statement; statements are joined across lines):
#   hardware_concurrency  any use of hardware_concurrency, sysconf(_SC_NPROCESSORS*),
#                         omp_get_num_procs / omp_get_max_threads
#   worker_cap            WorkerCount()/thread budget capped by a numeric
#                         literal >= 2 in std::min (e.g. min(WorkerCount(), 8))
#   fixed_thread_count    a thread/worker count variable assigned a numeric literal
#                         (threads = 8, kThreads = 4, num_workers{16}; 0 and 1 are
#                         allowed: serial/auto), or
#                         std::vector<std::thread> v(<literal>)
#
# Limits: regexes cannot follow data flow. A count computed through a helper, a
# macro, or a differently named variable is missed (false negatives); a literal
# that is not a thread count but is named like one is flagged (false positives).
# Suppress a reviewed line with a trailing comment `// thread-budget-ok: <why>`.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
STRICT=0
JSON_OUT=""
ROOTS=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --strict) STRICT=1 ;;
    --json) JSON_OUT="$2"; shift ;;
    -h|--help) sed -n 2,28p "${BASH_SOURCE[0]}"; exit 0 ;;
    *) ROOTS+=("$1") ;;
  esac
  shift
done
if [[ ${#ROOTS[@]} -eq 0 ]]; then
  ROOTS=("${ROOT}/accelerators" "${ROOT}/backends")
fi

FINDINGS="$(mktemp)"
trap 'rm -f "${FINDINGS}"' EXIT

for r in "${ROOTS[@]}"; do
  [[ -d "$r" ]] || continue
  find "$r" -type f \( -name '*.cpp' -o -name '*.cc' -o -name '*.hpp' -o -name '*.h' \) \
    -not -path '*/substrate/*' -not -path '*/eval/*' -not -path '*/tests/*' \
    -not -path '*/bench/*' -not -path '*/build*/*' -print0 |
    xargs -0 -r perl -e '
      use strict; use warnings;
      for my $f (@ARGV) {
        open(my $fh, "<", $f) or next;
        local $/; my $s = <$fh>; close $fh;
        my @lines = split /\n/, $s, -1;
        my $report = sub {
          my ($rule, $pos) = @_;
          my $n = 1 + (substr($s, 0, $pos) =~ tr/\n//);
          my $l = $lines[$n-1] // "";
          return if $l =~ m{thread-budget-ok};
          (my $t = $l) =~ s/^\s+//;
          print "$rule\t$f:$n\t$t\n";
        };
        while ($s =~ /hardware_concurrency|_SC_NPROCESSORS|omp_get_num_procs|omp_get_max_threads/g) { $report->("hardware_concurrency", $-[0]); }
        while ($s =~ /std::(?:min|max)\s*(?:<[^>]*>)?\s*\(\s*(?:[^;]*?\b(?:WorkerCount|worker_count|thread_budget)\b[^;]*?\b\d+u?\b|[^;]*?\b\d+u?\b[^;]*?\b(?:WorkerCount|worker_count|thread_budget)\b)[^;]*\)/g) {
          my $m = $&; my $p0 = $-[0]; next unless $m =~ /std::min.*[,({]\s*(?:[2-9]|\d\d+)u?\s*[,)}]/s; $report->("worker_cap", $p0); }
        while ($s =~ /\b(?:k?[Nn]um)?_?(?:[Tt]hreads?|[Ww]orkers?|[Tt]hread_?[Cc]ount|[Nn]_?threads)\w*\s*(?:=|\{)\s*(?:[2-9]|\d\d+)u?\s*[;}]/g) { $report->("fixed_thread_count", $-[0]); }
        while ($s =~ /std::vector<\s*std::thread\s*>\s*\w+\s*[({]\s*\d+u?\s*[)}]/g) { $report->("fixed_thread_count", $-[0]); }
      }' >> "${FINDINGS}" || true
done

sort -u "${FINDINGS}" -o "${FINDINGS}"
COUNT="$(wc -l < "${FINDINGS}" | tr -d ' ')"
MODE="report"; [[ ${STRICT} -eq 1 ]] && MODE="strict"

if [[ ${COUNT} -gt 0 ]]; then
  echo "thread_budget_lint: ${COUNT} offender(s) (mode=${MODE})"
  awk -F'\t' '{printf "  [%s] %s\n      %s\n", $1, $2, $3}' "${FINDINGS}"
else
  echo "thread_budget_lint: clean"
fi

if [[ -n "${JSON_OUT}" ]]; then
  mkdir -p "$(dirname "${JSON_OUT}")"
  STATUS="pass"; [[ ${COUNT} -gt 0 && ${STRICT} -eq 1 ]] && STATUS="fail"
  python3 - "${FINDINGS}" "${JSON_OUT}" "${MODE}" "${STATUS}" <<'PY'
import json, sys
rows = [l.rstrip("\n").split("\t", 2) for l in open(sys.argv[1]) if l.strip()]
json.dump({"gate": "thread_budget_lint", "mode": sys.argv[3], "status": sys.argv[4],
           "count": len(rows),
           "offenders": [{"rule": r[0], "where": r[1], "line": r[2]} for r in rows]},
          open(sys.argv[2], "w"), indent=2)
PY
fi

if [[ ${STRICT} -eq 1 && ${COUNT} -gt 0 ]]; then exit 1; fi
exit 0
