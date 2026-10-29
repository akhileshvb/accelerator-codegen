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
# a clean tree has no workload harnesses: host_bootstrap's gate fails once, and the stage that writes them follows
EXPECTED_BOOTSTRAP_FAILURE = re.compile(r"^\[pipeline\] gate FAILED: host_bootstrap\n(?=\[pipeline\] stage start: host_bootstrap )", re.M)
SINGLE_RUN_STAGES = ("analysis", "test_suite", "implementation", "audit")
REPAIR_STAGES = ("crash_debug", "optimization_fix")
SCALING_MIN_THREADS, MIN_BUILD_SPEEDUP = 4, 1.5  # a build that does not speed up at 4+ threads is serial
SKIPPABLE = {"skip"}  # a gate may skip itself when it does not apply (stages.base.GATES_ALLOW_SKIP)


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
    for line in TROUBLE.findall(EXPECTED_BOOTSTRAP_FAILURE.sub("", log)):
        found.append(f"trouble in the log: {line}")
    done = {(g, s): st for g, s, st in GATE_DONE.findall(log)}
    reused = set(GATE_REUSED.findall(log))
    for stage, gate in planned_gates(log):
        status = next((st for (g, s), st in done.items() if g == gate), "reused" if gate in reused else "")
        if status not in ("pass", "reused", *SKIPPABLE):
            found.append(f"gate {gate} (planned in {stage}) {'did not run' if not status else 'ended ' + status}")
    found += serial_builds(tree)
    found += [f"stop report: {p.relative_to(tree)}" for p in tree.rglob("stop_report.md")]
    found += [f"agent call failed: {c}" for c in calls if "error" in c]
    called = [c["stage"] for c in calls if "error" not in c]
    # a stage the pipeline asks an agent for once; the optimization worker's session legitimately resumes per candidate
    found += [f"agent stage {s} ran {called.count(s)} times" for s in SINGLE_RUN_STAGES if called.count(s) > 1]
    found += [f"agent asked for {s}, which the pipeline only asks on a failure" for s in set(called) if s in REPAIR_STAGES]
    return found


def serial_builds(tree: Path) -> list[str]:
    """The module's build must scale with the threads the bench sweep gave it (a missing link of the parallel scan made it serial)."""
    problems = []
    for path in sorted(tree.rglob("gates/aux_bench_sweep.json")):
        scaling = json.loads(path.read_text()).get("thread_scaling") or {}
        speedup = scaling.get("build_speedup_at_N_vs_1")
        if scaling.get("threads", 0) >= SCALING_MIN_THREADS and speedup is not None and speedup < MIN_BUILD_SPEEDUP:
            problems.append(f"the build does not scale: {speedup}x at {scaling['threads']} threads ({path.relative_to(tree)})")
    return problems


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


def coverage_notes(tree: Path) -> list[str]:
    """What the thread checks of the gates did in this run (reported, not asserted: the host's cores decide)."""
    notes = []
    for gate, key in (("aux_correctness", "threads_checked"), ("aux_bench_sweep", "thread_scaling")):
        for path in sorted(tree.rglob(f"gates/{gate}.json")):
            value = json.loads(path.read_text()).get(key)
            notes.append(f"{gate}.{key}: {value if value else 'none (the host gave this run one thread)'}")
    return notes


def verify(tree: Path, work: Path, rc: int | None, timings: list[tuple[str, float]],
           marks: list[tuple[float, str]]) -> int:
    log = (work / "pipeline.log").read_text(errors="replace")
    calls_file = work / "state" / "agent_calls.jsonl"
    calls = [json.loads(l) for l in calls_file.read_text().splitlines()] if calls_file.is_file() else []
    problems = problems_of(log, tree, calls, rc)
    print("\n" + timing_table(marks, timings))
    print("\n" + "\n".join(coverage_notes(tree)))
    if problems:
        print("\nSMOKE TEST FAILED:", *[f"  - {p}" for p in problems], sep="\n")
        print(f"\nlog tail ({work / 'pipeline.log'}):\n" + "\n".join(log.splitlines()[-25:]))
        return 1
    print("\nSMOKE TEST PASSED: every stage ran, every announced gate passed, nothing was repaired")
    return 0


# ---------------------------------------------------------------- adversarial cases

def evidence(work: Path, tree: Path) -> str:
    """Everything a case asserts on: the pipeline log, what the tools answered the agent, the pipeline's events."""
    parts = [work / "pipeline.log", work / "state" / "agent_tools.log", *tree.glob("pipeline/artifacts/*/q*/*/pipeline_events.jsonl")]
    return "\n".join(p.read_text(errors="replace") for p in parts if p.is_file())


def _seen(pattern: str, text: str) -> bool:
    return re.search(pattern, text, re.S) is not None


def case_decided(case: dict, text: str) -> bool:
    """The guardrail's message appeared, or the misbehaviour got through."""
    return all(_seen(p, text) for p in case["expect"]) or bool(case.get("escaped") and _seen(case["escaped"], text))


def verify_case(name: str, case: dict, work: Path, tree: Path, timings: list[tuple[str, float]]) -> int:
    text = evidence(work, tree)
    missing = [p for p in case["expect"] if not _seen(p, text)]
    got_through = bool(case.get("escaped") and _seen(case["escaped"], text))
    caught = not missing and not got_through
    seconds = dict(timings).get("pipeline", 0)
    if case.get("xfail"):
        if caught:
            print(f"case {name}: caught now, remove its xfail ({case['xfail']})")
            return 1
        print(f"case {name}: XFAIL, not caught yet ({seconds:.0f}s): {case['xfail']}")
        return 0
    if caught:
        print(f"case {name}: CAUGHT ({seconds:.0f}s): {case['about']}")
        return 0
    print(f"case {name}: NOT CAUGHT: {'the misbehaviour got through' if got_through else 'missing ' + ' | '.join(missing)}")
    print(f"evidence tail ({work}):\n" + "\n".join(text.splitlines()[-25:]))
    return 1
