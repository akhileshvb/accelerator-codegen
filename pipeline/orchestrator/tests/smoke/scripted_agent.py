#!/usr/bin/env python3
"""A scripted stand-in for `codex exec`, for the pipeline smoke test (pipeline_smoke.py).

The orchestrator launches it exactly as it launches codex (same arguments, sandbox, environment, session
banner); per stage it writes known-good artifacts instead of calling a model:

    analysis      the fixture analysis
    test_suite    the fixture suite
    implementation, with the reference accelerator's files (never the frozen suite), after one ask_advisor call
                  (a fork of its session answers with a fixed line)
    optimization  declares saturation
    audit         a passing audit
    critique      a passing critique

With ACCEL_SMOKE_CASE set it misbehaves in one named way (the adversarial smoke cases, see fixtures/cases.json and
the BEHAVIOURS table below); the pipeline's guardrails have to catch it.

A stage without a script fails loudly: in a smoke run an agent that is asked for work nobody planned for
means the pipeline's own plumbing needed repairing. Every launch is appended to $ACCEL_SMOKE_CALLS.

Environment (set by the driver): ACCEL_SMOKE_KIT (fixtures), ACCEL_SMOKE_REFERENCE (known-good tree),
ACCEL_SMOKE_CALLS (launch log), ACCEL_SMOKE_TOOLS (what the tools answered, for the case assertions),
ACCEL_SMOKE_CASE (the misbehaviour). The orchestrator sets ACCEL_PIPELINE_{STAGE,WORKLOAD,QUERY,ENGINE,REPO_ROOT}.
"""

from __future__ import annotations

import asyncio
import json
import os
import re
import shutil
import sys
import time
import uuid
from pathlib import Path

ENV = os.environ
PYTHON = ENV.get("ACCEL_PIPELINE_PYTHON")
if PYTHON and Path(PYTHON).absolute() != Path(sys.executable).absolute():
    os.execv(PYTHON, [PYTHON, *sys.argv])  # the orchestrator's interpreter: it has the MCP client library
STAGE, WORKLOAD, ENGINE = ENV["ACCEL_PIPELINE_STAGE"], ENV["ACCEL_PIPELINE_WORKLOAD"], ENV["ACCEL_PIPELINE_ENGINE"]
QUERY = ENV["ACCEL_PIPELINE_QUERY"]
ROOT = Path(ENV["ACCEL_PIPELINE_REPO_ROOT"])
KIT = Path(ENV["ACCEL_SMOKE_KIT"]) / "fixtures" / WORKLOAD
REFERENCE = Path(ENV["ACCEL_SMOKE_REFERENCE"])
PROMPT, RESUMED = "", False  # of this launch (set by main)
SHARED_QUERY_DIRS = {"config", "bench", "tests"}  # besides the engine's own directory and loose files


def benchmark() -> str:
    return json.loads((KIT / "fixture.json").read_text())["benchmark"]


def artifacts_dir() -> Path:
    return Path(ENV.get("ACCEL_PIPELINE_ART_DIR") or ROOT / "pipeline" / "artifacts" / benchmark() / f"q{QUERY}" / ENGINE)


def query_dir() -> Path:
    return ROOT / "accelerators" / benchmark() / f"q{QUERY}"


def copy_tree(src: Path, dst: Path, *, skip: tuple[str, ...] = ()) -> None:
    for path in sorted(src.rglob("*")):
        rel = path.relative_to(src)
        if path.is_file() and not any(rel.as_posix().startswith(s) for s in skip):
            (dst / rel).parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(path, dst / rel)


def analysis(mutate=None) -> str:
    copy_tree(KIT / "artifacts", artifacts_dir())
    if mutate:
        path = artifacts_dir() / "analysis.json"
        design = json.loads(path.read_text())
        mutate(design)
        path.write_text(json.dumps(design, indent=2))
    return "analysis written"


def test_suite() -> str:
    copy_tree(KIT / "suite", query_dir() / "tests" / "workload")
    return "suite written"


def leaves(exc: BaseException) -> list[BaseException]:
    return [leaf for sub in exc.exceptions for leaf in leaves(sub)] if isinstance(exc, BaseExceptionGroup) else [exc]


def note(text: str) -> None:
    """What the agent saw, for the adversarial cases to assert on."""
    if path := ENV.get("ACCEL_SMOKE_TOOLS"):
        with open(path, "a", encoding="utf-8") as log:
            log.write(text + "\n")


def call_tool(name: str, arguments: dict) -> dict:
    """One call of a tool of the pipeline's MCP server, started the way codex starts it."""
    from mcp import ClientSession, StdioServerParameters
    from mcp.client.stdio import stdio_client

    config = (Path(ENV["CODEX_HOME"]) / "config.toml").read_text()
    launcher = re.search(r'^command\s*=\s*"([^"]+)"', config, re.M).group(1)

    async def call() -> dict:
        async with stdio_client(StdioServerParameters(command=launcher, env=dict(ENV))) as (read, write):
            async with ClientSession(read, write, read_timeout_seconds=1800.0) as session:
                await session.initialize()
                result = await session.call_tool(name, arguments)
                text = result.content[0].text
                note(f"[tool {name}] {text}")
                try:
                    return json.loads(text)
                except ValueError:
                    return {"ok": not getattr(result, "isError", getattr(result, "is_error", False)), "text": text}

    return asyncio.run(call())


SCRIPTED_ADVICE = "scripted advice: the copied implementation is the plan"
HEADER_FIELDS = ("PURPOSE", "KEY SPACE", "COMPLEXITY", "MEMORY", "COW/THREADS", "USE WHEN")


def needed_primitives(query_dir_: Path) -> list[str]:
    """The reference primitives the copied sources include, transitively."""
    library = REFERENCE / "accelerators" / "prims"
    pending, found = [p for p in query_dir_.rglob("*") if p.suffix in (".hpp", ".cpp")], []
    while pending:
        for name in re.findall(r'#include "prims/([^"/]+)"', pending.pop().read_text(errors="replace")):
            if name not in found:
                found.append(name)
                pending.append(library / name)
    return sorted(found)


def conforming(name: str, text: str) -> str:
    """The header with every comment field the library's catalogue asks for (a reference header may predate them)."""
    present = text[:3000]
    missing = [f for f in HEADER_FIELDS if not re.search(rf"^//\s*{re.escape(f)}\b", present, re.M)]
    stem = Path(name).stem.replace("_", " ")
    return "".join(f"// {f}  {stem} (scripted smoke publication)\n" for f in missing) + text


def smoke_tests(names: list[str]) -> str:
    includes = "".join(f'#include "prims/{n}"\n' for n in names)
    return f"""// Unit tests for the generic aux-state core primitives. No DBMS required.
#include <cstdio>
#include <cstdint>
#include <random>
#include <vector>
{includes}
// randomized reference test: a running sum over a seeded random stream matches a std:: model
static bool random_stream_matches_model() {{
  std::mt19937 rng(20240601);
  std::vector<uint32_t> values(4096);
  uint64_t model = 0;
  for (auto &v : values) {{
    v = rng() & 0xFFFF;
    model += v;
  }}
  uint64_t again = 0;
  for (uint32_t v : values) again += v;
  return again == model;
}}

int main() {{
  if (!random_stream_matches_model()) {{
    std::puts("test_prims: FAIL");
    return 1;
  }}
  std::puts("test_prims: PASS");
  return 0;
}}
"""


def publish_primitives(query_dir_: Path) -> None:
    """The primitives the sources need, published the way an agent must: staged, then primitive_publish."""
    library = REFERENCE / "accelerators" / "prims"
    names = needed_primitives(query_dir_)
    staged = {f"accelerators/prims/{n}": conforming(n, (library / n).read_text()) for n in names}
    staged["accelerators/prims/tests/test_prims.cpp"] = smoke_tests(names)
    for rel, text in staged.items():
        target = ROOT / "pipeline" / "prims_staging" / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text)
    result = call_tool("primitive_publish", {"files": list(staged), "rationale": "scripted agent: primitives the reference needs"})
    if not result.get("ok"):
        raise SystemExit(f"primitive_publish failed: {json.dumps(result)[:4000]}")


def copy_benchmark_common(query_dir_: Path) -> None:
    """The benchmark-level headers the copied sources include (accelerators/<bench>/common)."""
    common = REFERENCE / "accelerators" / benchmark() / "common"
    pending, seen = [p for p in query_dir_.rglob("*") if p.suffix in (".hpp", ".cpp")], set()
    while pending:
        for name in re.findall(rf'#include "{benchmark()}/common/([^"]+)"', pending.pop().read_text(errors="replace")):
            if name not in seen:
                seen.add(name)
                (ROOT / "accelerators" / benchmark() / "common" / name).parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(common / name, ROOT / "accelerators" / benchmark() / "common" / name)
                pending.append(common / name)


def implementation(edit=None, then=None) -> str:
    """The reference accelerator, `edit`ed (given module.cpp) before the primitives are published; `then` runs last."""
    reference = REFERENCE / "accelerators" / benchmark() / f"q{QUERY}"
    for entry in sorted(reference.iterdir()):
        if entry.is_dir() and entry.name != ENGINE and entry.name not in SHARED_QUERY_DIRS:
            continue
        if entry.is_dir():
            copy_tree(entry, query_dir() / entry.name, skip=("workload",) if entry.name == "tests" else ())
        else:
            shutil.copyfile(entry, query_dir() / entry.name)
    copy_benchmark_common(query_dir())
    if edit:
        edit(query_dir() / ENGINE / "aux" / "module.cpp")
    call_tool("discover_host_sdk", {})
    advice = call_tool("ask_advisor", {"question": "scripted agent: a second opinion on the copied implementation?"})
    if SCRIPTED_ADVICE not in advice.get("text", ""):
        raise SystemExit(f"ask_advisor gave no advice: {json.dumps(advice)[:2000]}")
    publish_primitives(query_dir())
    if then:
        then()
    return "implementation copied from the reference"


# ---------------------------------------------------------------- misbehaviours (ACCEL_SMOKE_CASE)

def replace_once(path: Path, old: str, new: str) -> None:
    text = path.read_text()
    if text.count(old) != 1:
        raise SystemExit(f"scripted agent: {path.name} no longer contains exactly one {old!r} (the reference kernel changed)")
    path.write_text(text.replace(old, new))


def rebuild_every_write(module: Path) -> None:
    """Every write with changes rebuilds the whole state (the module's own rebuild path), though the analysis
    maintains lineitem writes incrementally."""
    replace_once(module, "  if (deltas.empty() && events.empty())\n    return outcome;\n",
                 "  if (deltas.empty() && events.empty())\n    return outcome;\n"
                 "  core::FinishRebuildAll(outcome, services.ChooseMaintenance(core::MaintenanceRequest::RebuildOnly(\n"
                 "                                      services.ExpectedQueriesBeforeNextWrite())));\n  return outcome;\n")


def one_slice_fold(module: Path) -> None:
    """A parallel section run as one slice: serial work where the plan is parallel."""
    replace_once(module, "services, deltas.size(), kDeltaGrain, pass,", "services, deltas.size(), deltas.size() + 1, pass,")


def racy_maintenance(module: Path) -> None:
    """Two threads bump a shared counter without synchronisation in the maintenance path (in a function of the
    module, not an inlined lambda: the sanitizer check attributes a race to the query by the frames it finds)."""
    replace_once(module, "core::StagedMaintenanceOutcome outcome;\n  if (buffer.RequiresRebuild()) {",
                 "core::StagedMaintenanceOutcome outcome;\n  {\n    static int racy = 0;\n"
                 "    std::thread a([] { RacyBump(racy); });\n"
                 "    std::thread b([] { RacyBump(racy); });\n    a.join();\n    b.join();\n  }\n"
                 "  if (buffer.RequiresRebuild()) {")
    replace_once(module, '#include "substrate/aux/core/parallel_slices.hpp"',
                 '#include <thread>\n#include "substrate/aux/core/parallel_slices.hpp"\n'
                 "__attribute__((noinline)) static void RacyBump(int &n) { for (int i = 0; i < 1000; ++i) ++n; }")


def dismiss_failed_check() -> None:
    call_tool("quick_check", {})
    note("[agent] the check failed, but it only concerns the budget: done")


def stall() -> str:
    """No edits and no tool calls (thinking), one read-only call that shows the nudge, then more of the same: the
    watchdog nudges, then ends the turn."""
    time.sleep(10)
    call_tool("discover_host_sdk", {})
    time.sleep(150)
    return implementation()


def extend_primitive() -> None:
    """Adds an API to a published primitive (the library only grows), which rebuilds the targets that include it."""
    name = needed_primitives(query_dir())[0]
    staged = ROOT / "pipeline" / "prims_staging" / "accelerators" / "prims" / name
    staged.write_text((ROOT / "accelerators" / "prims" / name).read_text() + "\ninline int ScriptedAdditiveApi() { return 1; }\n")
    result = call_tool("primitive_publish", {"files": [f"accelerators/prims/{name}"], "rationale": "scripted agent: an additive API"})
    if not result.get("ok"):
        raise SystemExit(f"primitive_publish of the extension failed: {json.dumps(result)[:4000]}")


def report_blocker_on_failing_code() -> None:
    """Calls its failing code a blocker; the repair turn it resumes in has the code fixed."""
    call_tool("quick_check", {})
    call_tool("report_blocker", {"summary": "quick_check fails and I cannot see why", "evidence": "differential failed"})


def blocker_then_linger() -> None:
    """Reports a slow gate as a blocker, but does not stop as told."""
    call_tool("report_blocker", {"summary": "aux_correctness has been running for minutes, it must be hung",
                                 "evidence": "gate_status: running"})
    time.sleep(60)


def drop_units_probes(design: dict) -> None:
    for candidate in design["state_candidate_search"]["candidates"]:
        candidate.pop("units_probe", None)


def one_primitive_for_everything(design: dict) -> None:
    storage = design["correctness_facts"]["state_design"]["state_storage"]
    storage.append(dict(storage[0]))
    for entry in storage:
        entry["primitive"] = storage[0]["primitive"]


def simulate_every_candidate(design: dict) -> None:
    """Each candidate cites its own simulated read (queries the data log holds)."""
    for i, candidate in enumerate(design["state_candidate_search"]["candidates"]):
        candidate["units_probe"], candidate["measured_units"] = f"SELECT {i + 1}", "1"


def broken_analysis(mutate) -> str:
    """The first launch writes a design the validation refuses; the repair turn it resumes writes a valid one.
    With a log of data queries the validation demands a logged simulation behind every candidate's units_probe."""
    artifacts_dir().mkdir(parents=True, exist_ok=True)
    queries = (json.dumps({"tool": "query_data", "args": {"sql": f"SELECT {i}"}, "error": None}) for i in range(1, 9))
    (artifacts_dir() / "data_queries.jsonl").write_text("\n".join(queries) + "\n")
    if RESUMED:
        note(f"[prompt resumed {STAGE}] {PROMPT[-2500:]}")
        return analysis(simulate_every_candidate)
    return analysis(lambda design: (simulate_every_candidate(design), mutate(design)))


BEHAVIOURS = {
    "rebuild_on_write": {"implementation": lambda: implementation(rebuild_every_write)},
    "serial_fold": {"implementation": lambda: implementation(one_slice_fold)},
    "data_race": {"implementation": lambda: implementation(
        racy_maintenance, lambda: call_tool("quick_check", {"sanitizer": "thread", "threads": 4}))},
    "heavy_gate_first": {"implementation": lambda: implementation(
        then=lambda: call_tool("request_gate", {"gate_name": "aux_correctness"}))},
    "dismissed_check": {"implementation": lambda: implementation(rebuild_every_write, dismiss_failed_check)},
    "false_blocker": {"implementation": lambda: implementation(then=blocker_then_linger)},
    "real_blocker": {"implementation": lambda: implementation(
        None if RESUMED else rebuild_every_write, None if RESUMED else report_blocker_on_failing_code)},
    "primitive_extension": {"implementation": lambda: implementation(then=extend_primitive)},
    "stalled": {"implementation": stall},
    "no_units_probe": {"analysis": lambda: broken_analysis(drop_units_probes)},
    "one_primitive": {"analysis": lambda: broken_analysis(one_primitive_for_everything)},
}


def optimization() -> str:
    return 'OPTIMIZATION_SATURATED\n```json\n{"reason": "scripted agent: the reference kernel is final"}\n```'


def audit() -> str:
    art = artifacts_dir()
    result = {"correctness_ok": True, "scale_review": {"generality_failures": [], "cow_findings": [], "large_scale": []},
              "fairness_review": {"cheating_suspected": False, "issues": []}, "summary": "scripted audit"}
    (art / "audit_result.json").write_text(json.dumps(result, indent=2) + "\n")
    (art / "final_audit.md").write_text("# Final audit\n\nScripted: the reference implementation is correct.\n")
    return "audit written"


def analysis_critic() -> str:
    review = {"estimates": {}, "checks": [], "verdict": "accept", "alternative": {"better": False}}
    (artifacts_dir() / "analysis_critique.json").write_text(json.dumps(review, indent=2) + "\n")
    return "critique written"


def critique() -> str:
    art = ROOT / "pipeline" / "artifacts" / "engines" / ENGINE / "host_bootstrap"
    art.mkdir(parents=True, exist_ok=True)
    review = {"status": "pass", "engine_name": ENGINE, "summary": "scripted critique", "violations": [],
              "feedback_for_implementation": ""}
    (art / "host_bootstrap_critique.json").write_text(json.dumps(review, indent=2) + "\n")
    (art / "host_bootstrap_critique.md").write_text("# Critique\n\nScripted: pass.\n")
    return "critique written"


SCRIPTS = {"analysis": analysis, "analysis_critic": analysis_critic, "test_suite": test_suite, "implementation": implementation, "audit": audit,
           "critique_host_bootstrap": critique}
PREFIX_SCRIPTS = {"optimization": optimization}


def script_for(stage: str):
    return BEHAVIOURS.get(ENV.get("ACCEL_SMOKE_CASE", ""), {}).get(stage) or SCRIPTS.get(stage) or next((fn for prefix, fn in PREFIX_SCRIPTS.items() if stage.startswith(prefix)), None)


def main() -> int:
    args = sys.argv[1:]
    resumed = "resume" in args[:2]
    sid = next((a for a in reversed(args) if len(a) == 36 and a.count("-") == 4), str(uuid.uuid4()))
    global PROMPT, RESUMED
    PROMPT, RESUMED = sys.stdin.read(), resumed
    home = Path(ENV.get("CODEX_HOME", ""))
    if home.name:
        (home / "sessions").mkdir(parents=True, exist_ok=True)
        (home / "sessions" / f"rollout-scripted-{sid}.jsonl").touch()
    print(f"session id: {sid}", file=sys.stderr)
    if args[:2] == ["exec", "fork"]:  # the advisor forking the agent's session
        with open(ENV["ACCEL_SMOKE_CALLS"], "a", encoding="utf-8") as log:
            log.write(json.dumps({"stage": "advisor", "forked": True}) + "\n")
        print(SCRIPTED_ADVICE)
        return 0
    with open(ENV["ACCEL_SMOKE_CALLS"], "a", encoding="utf-8") as log:
        log.write(json.dumps({"stage": STAGE, "resumed": resumed}) + "\n")
    fn = script_for(STAGE)
    if fn is None:
        print(f"scripted agent: no script for stage {STAGE!r} (the pipeline asked an agent for unplanned work)", file=sys.stderr)
        return 1
    try:
        print(fn())
    except BaseException as exc:  # noqa: BLE001 - the launch log keeps what the orchestrator may truncate
        with open(ENV["ACCEL_SMOKE_CALLS"], "a", encoding="utf-8") as log:
            log.write(json.dumps({"stage": STAGE, "error": "; ".join(f"{type(e).__name__}: {e}" for e in leaves(exc))[-6000:]}) + "\n")
        raise
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
