from __future__ import annotations


def squeeze_truncate(text: str, max_chars: int) -> str:
    compact = " ".join(str(text).split())
    if len(compact) <= max_chars:
        return compact
    return compact[: max_chars - 3] + "..."


def tail_marked(text: str, n: int = 6000) -> str:
    return text if len(text) <= n else "... [truncated]\n" + text[-n:]
