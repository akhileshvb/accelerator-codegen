#!/bin/sh
# A stand-in for `codex exec`: logs its arguments and prompt, prints a session banner, then behaves
# as FAKE_CODEX_MODE says (ok | sleep | chatty | fail). FAKE_CODEX_DIR holds its log files.
echo "$@" >> "$FAKE_CODEX_DIR/args.log"
for last; do :; done
[ -f "$last" ] && cat "$last" >> "$FAKE_CODEX_DIR/prompts.log" && echo "-----" >> "$FAKE_CODEX_DIR/prompts.log"
echo "session id: ${FAKE_CODEX_SESSION:-11111111-2222-3333-4444-555555555555}" >&2
case "$FAKE_CODEX_MODE" in
  sleep) sleep 30 ;;
  chatty) for i in 1 2 3 4 5 6 7 8 9 10 11 12; do echo tick; sleep 0.25; done ;;
  fail) echo boom >&2; exit 1 ;;
esac
echo done
