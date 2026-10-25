"""Token usage, cost and turns of each agent step, from codex's session logs.

codex exec writes a rollout (sessions/**/rollout-*-<session id>.jsonl) with a
`token_count` event after every model response (cumulative for the session)
and one event per tool call.  A step's usage is the difference between the
session's totals at its end and just before it started (a resumed session
carries the earlier steps' totals).

Cost is estimated from per-token prices: `verus.pricing` in the config
({model: {prompt, completion, cache_read}} in USD per million tokens) else
OpenRouter's model list (cached for a day).  Every step is appended to
<artifacts>/usage.jsonl; `python -m accel_pipeline.verus.usage <artifacts dir>`
prints the totals per stage.
"""

from __future__ import annotations

import json
import sys
import time
import urllib.request
from datetime import datetime
from pathlib import Path
from typing import Any

from accel_pipeline.session import rollout_path

_PRICES_URL = "https://openrouter.ai/api/v1/models"
_PRICES_CACHE = Path.home() / ".cache" / "accel_verus" / "openrouter_prices.json"


def _ts(s: str) -> float:
    try:
        return datetime.fromisoformat(s.replace("Z", "+00:00")).timestamp()
    except (ValueError, AttributeError):
        return 0.0


def session_usage(codex_home: Path, session_id: str, since: float) -> dict[str, Any]:
    """Tokens, model calls and tool calls of the part of a session after `since` (epoch seconds)."""
    path = rollout_path(codex_home, session_id)
    if path is None:
        return {}
    keys = ("input_tokens", "cached_input_tokens", "output_tokens", "reasoning_output_tokens")
    before = dict.fromkeys(keys, 0)
    after = dict.fromkeys(keys, 0)
    model_calls = tool_calls = 0
    last_total = None
    with path.open(encoding="utf-8") as f:
        for line in f:
            try:
                j = json.loads(line)
            except json.JSONDecodeError:
                continue
            p = j.get("payload") or {}
            t = p.get("type")
            new = _ts(j.get("timestamp", "")) >= since - 1.0
            if t == "token_count":
                total = (p.get("info") or {}).get("total_token_usage")
                if not total:
                    continue
                vals = {k: int(total.get(k) or 0) for k in keys}
                if new:
                    if vals != last_total:  # one per model response (repeats carry no new usage)
                        model_calls += 1
                    after = vals
                else:
                    before = after = vals
                last_total = vals
            elif new and t in ("function_call", "custom_tool_call", "local_shell_call", "web_search_call"):
                tool_calls += 1
    d = {k: max(after[k] - before[k], 0) for k in keys}
    return {"input_tokens": d["input_tokens"], "cache_read_tokens": d["cached_input_tokens"],
            "output_tokens": d["output_tokens"], "reasoning_tokens": d["reasoning_output_tokens"],
            "total_tokens": d["input_tokens"] + d["output_tokens"],
            "model_calls": model_calls, "tool_calls": tool_calls}


def _openrouter_prices() -> dict[str, dict[str, float]]:
    try:
        if _PRICES_CACHE.is_file() and time.time() - _PRICES_CACHE.stat().st_mtime < 86400:
            return json.loads(_PRICES_CACHE.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        pass
    try:
        with urllib.request.urlopen(_PRICES_URL, timeout=20) as r:
            data = json.load(r).get("data") or []
    except Exception:  # noqa: BLE001 - offline: no cost, never a failed step
        return {}
    prices = {}
    for m in data:
        pr = m.get("pricing") or {}
        try:
            prompt, completion = float(pr.get("prompt") or 0) * 1e6, float(pr.get("completion") or 0) * 1e6
            cache_read = float(pr.get("input_cache_read") or 0) * 1e6 or prompt
        except (TypeError, ValueError):
            continue
        prices[m.get("id", "")] = {"prompt": prompt, "completion": completion, "cache_read": cache_read}
    try:
        _PRICES_CACHE.parent.mkdir(parents=True, exist_ok=True)
        _PRICES_CACHE.write_text(json.dumps(prices), encoding="utf-8")
    except OSError:
        pass
    return prices


def cost_usd(model: str, usage: dict[str, Any], overrides: dict[str, Any] | None = None) -> float | None:
    """Estimated USD for a step (output includes reasoning tokens)."""
    if not usage:
        return None
    name = model.split(":", 1)[-1] if model.count(":") == 1 and "/" not in model.split(":", 1)[0] else model
    price = (overrides or {}).get(name) or (overrides or {}).get(model) or _openrouter_prices().get(name)
    if not price:
        return None
    cached = int(usage.get("cache_read_tokens") or 0)
    fresh = max(int(usage.get("input_tokens") or 0) - cached, 0)
    usd = (fresh * float(price["prompt"]) + cached * float(price.get("cache_read", price["prompt"]))
           + int(usage.get("output_tokens") or 0) * float(price["completion"])) / 1e6
    return round(usd, 4)


def record(art: Path, entry: dict[str, Any]) -> None:
    with (Path(art) / "usage.jsonl").open("a", encoding="utf-8") as f:
        f.write(json.dumps(entry) + "\n")


def summary(art: Path) -> dict[str, Any]:
    """Totals per stage and overall from <art>/usage.jsonl."""
    path = Path(art) / "usage.jsonl"
    per: dict[str, dict[str, Any]] = {}
    fields = ("steps", "seconds", "input_tokens", "cache_read_tokens", "output_tokens", "model_calls",
              "tool_calls", "cost_usd")
    if path.is_file():
        for line in path.read_text(encoding="utf-8").splitlines():
            try:
                e = json.loads(line)
            except json.JSONDecodeError:
                continue
            s = per.setdefault(e.get("stage", "?"), dict.fromkeys(fields, 0) | {"model": e.get("model", "")})
            if s["model"] != e.get("model", ""):
                s["model"] = "(several)"
            s["steps"] += 1
            for k in fields[1:]:
                s[k] = round(s[k] + float(e.get(k) or 0), 4)
    total = dict.fromkeys(fields, 0)
    for s in per.values():
        for k in fields:
            total[k] = round(total[k] + s[k], 4)
    return {"stages": per, "total": total}


def format_summary(sm: dict[str, Any]) -> str:
    rows = [f"{'stage':<26}{'model':<30}{'steps':>6}{'min':>8}{'in Mtok':>9}{'cached':>8}{'out ktok':>9}"
            f"{'turns':>7}{'tools':>7}{'USD':>9}"]
    for name, s in list(sm["stages"].items()) + [("TOTAL", sm["total"] | {"model": ""})]:
        rows.append(f"{name[:25]:<26}{str(s.get('model', ''))[:29]:<30}{int(s['steps']):>6}{s['seconds'] / 60:>8.1f}"
                    f"{s['input_tokens'] / 1e6:>9.2f}{s['cache_read_tokens'] / 1e6:>8.2f}"
                    f"{s['output_tokens'] / 1e3:>9.1f}{int(s['model_calls']):>7}{int(s['tool_calls']):>7}"
                    f"{s['cost_usd']:>9.2f}")
    return "\n".join(rows)


if __name__ == "__main__":
    print(format_summary(summary(Path(sys.argv[1]))))
