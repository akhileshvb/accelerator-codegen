#!/bin/sh
# A stand-in for `codex exec`: logs its arguments and prompt, prints a session banner, then behaves
# as FAKE_CODEX_MODE says (ok | sleep | chatty | fail | edit | blocker | blocker_exit | gate_pass | gate_fail). FAKE_CODEX_DIR holds its log files;
# it appends to FAKE_CODEX_TOUCH (an agent edit) when set.
echo "$@" >> "$FAKE_CODEX_DIR/args.log"
for last; do :; done
[ -f "$last" ] && cat "$last" >> "$FAKE_CODEX_DIR/prompts.log" && echo "-----" >> "$FAKE_CODEX_DIR/prompts.log"
[ -n "$FAKE_CODEX_TOUCH" ] && echo edit >> "$FAKE_CODEX_TOUCH"
sid="${FAKE_CODEX_SESSION:-11111111-2222-3333-4444-555555555555}"
mkdir -p "$CODEX_HOME/sessions" && touch "$CODEX_HOME/sessions/rollout-fake-$sid.jsonl"
echo "session id: $sid" >&2
case "$FAKE_CODEX_MODE" in
  sleep) sleep 30 ;;
  chatty) for i in 1 2 3 4 5 6 7 8 9 10 11 12; do echo tick; sleep 0.25; done ;;
  fail) echo boom >&2; exit 1 ;;
  edit) for i in $(seq 1 30); do echo x >> "$FAKE_CODEX_EDIT"; sleep 0.2; done ;;
  blocker) printf 'summary\n' > "$FAKE_CODEX_ART/blocker.md"; sleep 30 ;;
  blocker_exit) printf 'summary\n' > "$FAKE_CODEX_ART/blocker.md" ;;
  gate_pass) mkdir -p "$FAKE_CODEX_ART/gates"; sleep 0.3; printf '{"status": "pass"}' > "$FAKE_CODEX_ART/gates/fake_gate.json"; sleep 30 ;;
  gate_fail) mkdir -p "$FAKE_CODEX_ART/gates"; sleep 0.3; printf '{"status": "fail"}' > "$FAKE_CODEX_ART/gates/fake_gate.json"; sleep 1.5 ;;
esac
echo done
