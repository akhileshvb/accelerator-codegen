"""Token usage per codex session, the basis of TIMELINE.md's cost."""

from __future__ import annotations

import json

from accel_pipeline.verus.usage import session_usage


def rollout(home, session_id, lines):
    path = home / "sessions/2026/10/25" / f"rollout-2026-10-25T00-00-00-{session_id}.jsonl"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("".join(json.dumps(line) + "\n" for line in lines))


def tokens(total_in, total_out, last_in, last_out):
    usage = lambda i, o: {"input_tokens": i, "cached_input_tokens": 0, "output_tokens": o, "reasoning_output_tokens": 0}
    return {"timestamp": "2026-10-25T00:00:01Z", "type": "event_msg",
            "payload": {"type": "token_count", "info": {"total_token_usage": usage(total_in, total_out),
                                                        "last_token_usage": usage(last_in, last_out)}}}


def test_a_fork_counts_only_its_own_responses_not_the_parents_running_total(tmp_path):
    meta = {"timestamp": "2026-10-25T00:00:00Z", "type": "session_meta", "payload": {"id": "fork", "forked_from_id": "main"}}
    rollout(tmp_path, "fork", [meta, tokens(8_000_100, 1_010, 100, 10), tokens(8_000_300, 1_030, 200, 20)])
    usage = session_usage(tmp_path, "fork", 0.0)
    assert (usage["input_tokens"], usage["output_tokens"], usage["model_calls"]) == (300, 30, 2)


def test_a_session_of_its_own_counts_from_zero(tmp_path):
    meta = {"timestamp": "2026-10-25T00:00:00Z", "type": "session_meta", "payload": {"id": "main"}}
    rollout(tmp_path, "main", [meta, tokens(100, 10, 100, 10), tokens(300, 30, 200, 20)])
    assert session_usage(tmp_path, "main", 0.0)["input_tokens"] == 300
