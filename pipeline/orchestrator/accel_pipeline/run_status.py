"""Where a run is: its stage and state, the last events and the cost so far."""

from __future__ import annotations

import json
import time
from pathlib import Path

from accel_pipeline.state import PipelineState
from accel_pipeline.timeline import read_costs, read_rollouts, run_files, run_log

LAST_EVENTS = 6


def _phase(state: PipelineState, events: list[dict]) -> str:
    if state.status:
        return state.status
    finished = any(e.get("event") == "stage_complete" and e.get("stage") == "audit" for e in events)
    return "finished" if finished else f"in {state.stage}"


def _read_events(paths: list[str]) -> list[dict]:
    return [json.loads(line) for path in paths for line in Path(path).read_text().splitlines() if line.strip()]


def render(run: Path) -> str:
    tree, event_paths, sessions = run_files(run)
    states = sorted(tree.glob("pipeline/artifacts/**/pipeline_state.json"), key=lambda p: p.stat().st_mtime)
    if not states:
        return f"{run}: no pipeline state yet (the run has not reached its first stage)\n"
    state = PipelineState.load(states[-1])
    events = _read_events(event_paths)
    lines = [f"run:    {run}", f"query:  {state.workload} q{state.query} on {state.engine or '-'}", f"stage:  {_phase(state, events)}"]
    if log := run_log(run):
        lines.append(f"log:    {log} (last written {(time.time() - log.stat().st_mtime) / 60:.0f} min ago)")
    _, models = read_rollouts(sessions)
    costs = read_costs(sessions, models)
    priced = [usd for *_, usd in costs if usd is not None]
    unpriced = f", {len(costs) - len(priced)} unpriced" if len(priced) < len(costs) else ""
    lines += [f"cost:   ${sum(priced):.2f} ({len(costs)} sessions{unpriced})", "last events:"]
    lines += [f"  {e.get('ts', '')[:19]}  {e.get('event', '?')}  {e.get('stage', '')}" for e in events[-LAST_EVENTS:]]
    return "\n".join(lines) + "\n"
