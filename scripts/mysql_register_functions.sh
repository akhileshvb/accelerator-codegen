#!/usr/bin/env bash
# Print the full MySQL UDF registration SQL: the backend's query-independent
# base (scripts/mysql_register_base.sql) followed by every per-query fragment
# src/generated/<bench>/q<N>/mysql/plugin/functions.sql (sorted by path).
# Adding a query adds a fragment in its own directory; nothing shared changes.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cat "${ROOT}/scripts/mysql_register_base.sql"
shopt -s nullglob
frags=("${ROOT}"/src/generated/*/q*/mysql/plugin/functions.sql)
if ((${#frags[@]})); then
  printf '%s\n' "${frags[@]}" | sort -V | while IFS= read -r f; do
    echo
    echo "-- ${f#"${ROOT}"/}"
    cat "$f"
  done
fi
