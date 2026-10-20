#!/usr/bin/env bash
# Fixture host.sh of the synthetic toydb engine (host_contract.md).
case "${1:-}" in
  info) echo '{"engine": "toydb", "embedded": true, "verbs": ["info", "up", "down", "load", "build", "sql", "run", "slot"]}' ;;
  up | down) exit 77 ;;
  *) exit 2 ;;
esac
