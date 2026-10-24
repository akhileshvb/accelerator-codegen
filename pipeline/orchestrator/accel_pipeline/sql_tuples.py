"""Parameter tuples as SQL literal lists: how many a query takes, how to split one, a fresh seed."""

from __future__ import annotations

import os
import re


def fresh_seed() -> int:
    """A new seed per call: the OS entropy pool (never the clock alone)."""
    return int.from_bytes(os.urandom(8), "little") % (2**31 - 1) + 1


def n_params(query_sql: str) -> int:
    """How many $n parameters the query takes (the highest n)."""
    nums = [int(m) for m in re.findall(r"\$(\d+)", query_sql)]
    return max(nums) if nums else 0


def split_tuple(t: str) -> list[str]:
    """The top-level comma-separated literals of a tuple (quotes and parentheses respected)."""
    out, depth, cur, i = [], 0, "", 0
    while i < len(t):
        c = t[i]
        if c == "'":
            j = i + 1
            while j < len(t):
                if t[j] == "'" and t[j + 1:j + 2] == "'":
                    j += 2
                    continue
                if t[j] == "'":
                    break
                j += 1
            cur += t[i:j + 1]
            i = j + 1
            continue
        if c in "([":
            depth += 1
        elif c in ")]":
            depth -= 1
        if c == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += c
        i += 1
    if cur.strip():
        out.append(cur.strip())
    return out
