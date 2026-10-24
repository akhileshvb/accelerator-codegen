"""What a smoke run proved: its log and artifacts judged against 'nothing needed repairing'."""

from __future__ import annotations

import json
import re
from pathlib import Path

STAGES = ("analysis", "test_suite", "implementation", "optimization", "audit")
GATE_DONE = re.compile(r"gate complete: (?:\[\d+/\d+\] )?(\w+) \((\w+)\) status=(\w+)")
GATE_PLAN = re.compile(r"═══ gates \((\w+), Q\d+\): \d+ to run ═══")
GATE_NAME = re.compile(r"^\[pipeline\]\s+\d+\. (\w+)")
GATE_REUSED = re.compile(r"gate (\w+): reused")
STAGE_MARK = re.compile(r"^=== stage: (\w+) ===", re.M)
# a line that says something was broken or repaired (a clean run prints none of them)
TROUBLE = re.compile(r"gate FAILED|^\[repair\]|stage \w+ raised|retrying stage|write_policy_violation|stopped:", re.M)
SKIPPABLE = {"skip"}  # a gate may skip itself when it does not apply (gate_plan.GATES_ALLOW_SKIP)


def planned_gates(log: str) -> list[tuple[str, str]]:
    """(stage, gate) of every gate list the pipeline announced."""
    planned, stage = [], ""
    for line in log.splitlines():
        if m := GATE_PLAN.search(line):
            stage = m.group(1)
        elif stage and (m := GATE_NAME.match(line)):
            planned.append((stage, m.group(1)))
        elif stage and not line.startswith("[pipeline]  "):
            stage = ""
    return planned


def problems_of(log: str, tree: Path, calls: list[dict], rc: int | None) -> list[str]:
    found: list[str] = []
    if rc != 0:
        found.append("the run timed out" if rc is None else f"run_pipeline exited {rc}")
    if "pipeline complete" not in log:
        found.append("the run never printed 'pipeline complete'")
    seen = STAGE_MARK.findall(log)
    found += [f"stage {s} never started" for s in STAGES if s not in seen]
    for line in TROUBLE.findall(log):
        found.append(f"trouble in the log: {line}")
    done = {(g, s): st for g, s, st in GATE_DONE.findall(log)}
    reused = set(GATE_REUSED.findall(log))
    for stage, gate in planned_gates(log):
        status = next((st for (g, s), st in done.items() if g == gate), "reused" if gate in reused else "")
        if status not in ("pass", "reused", *SKIPPABLE):
            found.append(f"gate {gate} (planned in {stage}) {'did not run' if not status else 'ended ' + status}")
    found += [f"stop report: {p.relative_to(tree)}" for p in tree.rglob("stop_report.md")]
    stages_called = [c["stage"] for c in calls if "error" not in c]
    found += [f"agent call {c}" for c in calls if c.get("resumed") or "error" in c]
    found += [f"agent stage {s} ran {stages_called.count(s)} times" for s in set(stages_called) if stages_called.count(s) > 1]
    return found


def timing_table(marks: list[tuple[float, str]], timings: list[tuple[str, float]]) -> str:
    """Rows: preparation steps, then the pipeline's stages and gates at the times their lines appeared."""
    rows = [(name, seconds) for name, seconds in timings if name != "pipeline"]
    if marks:
        start = marks[0][0]
        stage_starts = [(t, STAGE_MARK.match(l).group(1)) for t, l in marks if STAGE_MARK.match(l)]
        end = marks[-1][0]
        previous = ("before the first stage (host_bootstrap)", start)
        for t, name in stage_starts:
            rows.append((previous[0], t - previous[1]))
            previous = (f"stage {name}", t)
        rows.append((previous[0], end - previous[1]))
    gates = [(f"  gate {m.group(1)}", float(m.group(4)) / 1000) for _, l in marks
             if (m := re.search(r"gate complete: (?:\[\d+/\d+\] )?(\w+) \((\w+)\) status=(\w+) duration=(\d+)ms", l))]
    rows += gates
    total = sum(s for name, s in timings if name in ("pipeline",))
    width = max(len(n) for n, _ in rows + [("total", 0)])
    lines = [f"{name:<{width}}  {seconds:7.0f}s" for name, seconds in rows]
    return "\n".join([*lines, f"{'pipeline total':<{width}}  {total:7.0f}s"])


def verify(tree: Path, work: Path, rc: int | None, timings: list[tuple[str, float]],
           marks: list[tuple[float, str]]) -> int:
    log = (work / "pipeline.log").read_text(errors="replace")
    calls_file = work / "state" / "agent_calls.jsonl"
    calls = [json.loads(l) for l in calls_file.read_text().splitlines()] if calls_file.is_file() else []
    problems = problems_of(log, tree, calls, rc)
    print("\n" + timing_table(marks, timings))
    if problems:
        print("\nSMOKE TEST FAILED:", *[f"  - {p}" for p in problems], sep="\n")
        print(f"\nlog tail ({work / 'pipeline.log'}):\n" + "\n".join(log.splitlines()[-25:]))
        return 1
    print("\nSMOKE TEST PASSED: every stage ran, every announced gate passed, nothing was repaired")
    return 0
