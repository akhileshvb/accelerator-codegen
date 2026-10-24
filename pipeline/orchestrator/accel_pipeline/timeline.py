"""Where a run's wall time and money went -> <run dir>/TIMELINE.md.

    python -m accel_pipeline.timeline <run dir>      (run dir: tree/, pipeline.log)

Stages come from pipeline_events.jsonl; model and tool time from the codex rollouts (model time: from a turn
start or a tool result to the model's next item; tool time per tool: call to output); gate durations from the
pipeline log, attributed to the stage running when they were logged; cost from each session's token usage at
the model's price (accel_pipeline.verus.usage).
"""

from __future__ import annotations

import collections
import datetime as dt
import glob
import json
import re
import sys
from dataclasses import dataclass
from pathlib import Path

from accel_pipeline.verus.usage import cost_usd, session_usage


@dataclass
class Stage:
    name: str
    model: str
    start: dt.datetime
    end: dt.datetime | None = None


def _ts(text: str) -> dt.datetime:
    return dt.datetime.fromisoformat(text.replace("Z", "+00:00"))


def _minutes(seconds: float) -> str:
    return f"{seconds / 60:.1f}"


def read_stages(events_path: str) -> list[Stage]:
    stages: list[Stage] = []
    open_stage: dict[str, Stage] = {}
    for line in open(events_path):
        event = json.loads(line)
        if event.get("event") == "stage_start":
            open_stage[event["stage"]] = Stage(event["stage"], event.get("model", ""), _ts(event["ts"]))
            stages.append(open_stage[event["stage"]])
        elif event.get("event") == "stage_complete" and event.get("stage") in open_stage:
            open_stage.pop(event["stage"]).end = _ts(event["ts"])
    return stages


def read_rollouts(sessions: list[str]) -> tuple[list[tuple[dt.datetime, dt.datetime, str, str]], dict[str, str]]:
    """((start, end, "model" | "tool", tool name) intervals, {rollout file: model})."""
    intervals, models = [], {}
    for path in sessions:
        pending: dict[str, tuple[dt.datetime, str]] = {}
        ready = None
        for line in open(path):
            event = json.loads(line)
            when, payload = _ts(event["timestamp"]), event.get("payload") or {}
            if event.get("type") == "turn_context":
                models[path] = str(payload.get("model", ""))
            kind = payload.get("type") if event.get("type") in ("response_item", "event_msg") else None
            if kind == "task_started":
                ready = when
            elif kind in ("reasoning", "message", "function_call") and ready is not None:
                intervals.append((ready, when, "model", ""))
                ready = None
            if kind == "function_call":
                pending[payload.get("call_id")] = (when, payload.get("name", "?"))
            elif kind == "function_call_output" and payload.get("call_id") in pending:
                started, name = pending.pop(payload["call_id"])
                intervals.append((started, when, "tool", name))
                ready = when
            elif kind == "task_complete":
                ready = None
    return intervals, models


def read_gates(log_path: Path) -> dict[str, list[tuple[str, str, float]]]:
    gates: dict[str, list[tuple[str, str, float]]] = collections.defaultdict(list)
    stage = "before the stages"
    for line in log_path.read_text(errors="replace").splitlines():
        if m := re.search(r"stage start: (\w+)", line):
            stage = m.group(1)
        if m := re.search(r"gate complete: (?:\[\d+/\d+\] )?(\w+) .*status=(\w+) duration=(\d+)ms", line):
            gates[stage].append((m.group(1), m.group(2), int(m.group(3)) / 1000))
    return gates


def read_costs(sessions: list[str], models: dict[str, str]) -> list[tuple[str, str, int, float | None]]:
    """(model, session id, total tokens, USD or None when the model has no price) per session."""
    rows = []
    for path in sessions:
        session_id = Path(path).stem[-36:]
        usage = session_usage(Path(path).parents[4], session_id, 0.0)
        rows.append((models.get(path, "?"), session_id, usage.get("total_tokens", 0), cost_usd(models.get(path, ""), usage)))
    return rows


def render(run: Path) -> str:
    tree = run / "tree"
    events = glob.glob(f"{tree}/pipeline/artifacts/*/q*/*/pipeline_events.jsonl")
    sessions = sorted(glob.glob(f"{tree}/pipeline/.codex/sessions/*/*/*/*.jsonl"))
    now = dt.datetime.now(dt.timezone.utc)
    stages = read_stages(events[0]) if events else []
    intervals, models = read_rollouts(sessions)
    for stage, following in zip(stages, stages[1:] + [None]):
        # a stage killed by an operator never logs completion: it ends at its last agent activity
        if stage.end is None and following is not None:
            stage.end = max((end for _, end, _, _ in intervals if stage.start <= end <= following.start), default=stage.start)
    running = {id(s) for s in stages if s.end is None}
    for stage in stages:
        stage.end = stage.end or now

    out = [f"# Timeline: {run.name} (generated {now:%H:%M}Z)", "",
           "| # | stage | model | start | wall min | model min | tool min | top tools (min, calls) |", "|---|---|---|---|---|---|---|---|"]
    wall_by_stage: collections.Counter = collections.Counter()
    for i, stage in enumerate(stages, 1):
        mine = [iv for iv in intervals if stage.start <= iv[0] < stage.end]
        wall = (stage.end - stage.start).total_seconds()
        model_s = sum((e - b).total_seconds() for b, e, kind, _ in mine if kind == "model")
        tool_s, tool_calls = collections.Counter(), collections.Counter()
        for b, e, kind, name in mine:
            if kind == "tool":
                tool_s[name] += (e - b).total_seconds()
                tool_calls[name] += 1
        top = ", ".join(f"{n} {_minutes(s)} ({tool_calls[n]})" for n, s in tool_s.most_common(4))
        out.append(f"| {i} | {stage.name}{' (running)' if id(stage) in running else ''} | {stage.model.split('/')[-1]} | "
                   f"{stage.start:%H:%M} | {_minutes(wall)} | {_minutes(model_s)} | {_minutes(sum(tool_s.values()))} | {top} |")
        wall_by_stage[stage.name] += wall
    between = sum(max(0.0, (b.start - a.end).total_seconds()) for a, b in zip(stages, stages[1:]))
    span = (stages[-1].end - stages[0].start).total_seconds() if stages else 0
    out += ["", f"Span {_minutes(span)} min; in stages {_minutes(sum(wall_by_stage.values()))}; between stages (orchestrator, "
            f"gates outside agents, operator pauses) {_minutes(between)}.", "",
            "By stage (wall min): " + ", ".join(f"{k} {_minutes(v)}" for k, v in wall_by_stage.most_common()), "", "## Gates (orchestrator-run)", ""]
    log = next((p for p in (run / "pipeline.log", run / "logs" / "pipeline.log") if p.is_file()), None)
    for stage, rows in (read_gates(log) if log else {}).items():
        out += [f"- {stage}: {name} {status} {_minutes(seconds)} min" for name, status, seconds in rows]
    costs = read_costs(sessions, models)
    priced = [usd for *_, usd in costs if usd is not None]
    out += ["", "## Cost", "", f"Total ${sum(priced):.2f}" + ("" if len(priced) == len(costs) else f" (+{len(costs) - len(priced)} unpriced sessions)"), ""]
    out += [f"- {model} {session[:8]}: {tokens:,} tokens, " + (f"${usd:.2f}" if usd is not None else "unpriced") for model, session, tokens, usd in costs]
    return "\n".join(out) + "\n"


def main(argv: list[str] | None = None) -> int:
    run = Path((argv or sys.argv[1:])[0]).resolve()
    text = render(run)
    (run / "TIMELINE.md").write_text(text)
    print(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
