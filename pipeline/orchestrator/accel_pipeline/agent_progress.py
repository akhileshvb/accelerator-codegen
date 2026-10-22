from __future__ import annotations

import sys


def begin_agent_run(stage: str, *, max_turns: int, prompt_chars: int) -> None:
    print(
        f"[pipeline] agent run start: stage={stage} max_turns={max_turns} prompt_chars={prompt_chars}",
        file=sys.stderr,
        flush=True,
    )
