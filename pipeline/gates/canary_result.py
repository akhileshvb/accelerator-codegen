#!/usr/bin/env python3
"""The result of one canary gate run: the checks' JSON lines and the sanitizer reports in the run's
log, as the gate's JSON (status, reason, the owning layers of the failures, the log tail).

usage: canary_result.py <out.json> <gate> <rc> <run_reason> <report_dir> <engine> <repo_root>
"""
from __future__ import annotations

import json
import re
import sys
from pathlib import Path

ASAN_REPORT = re.compile(r"ERROR: AddressSanitizer|ERROR: LeakSanitizer|runtime error:")
EXCERPT_LINES = 70


def check_results(log: str) -> list[dict]:
    results = []
    for line in log.splitlines():
        if line.startswith('{"check"'):
            try:
                results.append(json.loads(line))
            except json.JSONDecodeError:
                continue
    return results


def report_excerpt(log: str) -> str:
    lines = log.splitlines()
    for i, line in enumerate(lines):
        if ASAN_REPORT.search(line):
            return "\n".join(lines[max(0, i - 2) : i + EXCERPT_LINES])
    return ""


_LIBRARY_FRAME = re.compile(r"libtsan|/usr/include/|libstdc\+\+|libc\.so|<null> <null>")


def _is_ours(report: str, ours_pattern: re.Pattern[str]) -> bool:
    """A race is ours when, in one of its access stacks, the innermost frame outside the sanitizer and
    the standard library is in our code: the call that raced. A stack that only passes through our
    code on its way into the engine (the canary calling a connection) is the engine's."""
    stack: list[str] = []
    stacks = []
    for line in report.splitlines():
        if line.startswith("    #"):
            stack.append(line)
            continue
        if stack:
            stacks.append(stack)
            stack = []
        if line.startswith(("  Location", "  Mutex", "  Thread", "  Heap block")):
            break
    if stack:
        stacks.append(stack)
    for frames in stacks:
        inner = next((f for f in frames if not _LIBRARY_FRAME.search(f)), "")
        if ours_pattern.search(inner):
            return True
    return False


def tsan_reports(report_dir: Path, engine: str, root: str) -> tuple[int, int, str]:
    """(races in our code, reports inside the engine only, the text of the first of ours else the first)."""
    ours_pattern = re.compile(rf"{re.escape(root)}/(substrate|backends/{re.escape(engine)}|pipeline/canary/canary_mirror)")
    ours = engine_only = 0
    first_ours = first = ""
    for path in sorted(report_dir.glob("tsan.*")):
        for report in re.split(r"(?=^WARNING: ThreadSanitizer)", path.read_text(errors="replace"), flags=re.MULTILINE):
            if not report.startswith("WARNING: ThreadSanitizer"):
                continue
            text = "\n".join(report.splitlines()[:EXCERPT_LINES])
            first = first or text
            if _is_ours(report, ours_pattern):
                ours += 1
                first_ours = first_ours or text
            else:
                engine_only += 1
    return ours, engine_only, first_ours or first


def main() -> int:
    out, gate, rc, run_reason, report_dir, engine, root = sys.argv[1:8]
    rc_code = int(rc)
    log = (Path(report_dir) / "run.log").read_text(errors="replace")
    results = check_results(log)
    failing = [r for r in results if r.get("status") != "pass"]
    asan = report_excerpt(log)
    ours, engine_only, tsan = tsan_reports(Path(report_dir), engine, root)
    reasons = []
    if run_reason:
        reasons.append(run_reason)
    for r in failing[:3]:
        reasons.append(f"canary check `{r['check']}` ({r.get('defect_class', '')}): {r.get('detail', '')}")
    if len(failing) > 3:
        reasons.append(f"... and {len(failing) - 3} more failing checks")
    if asan:
        reasons.append("sanitizer report: " + next(ln for ln in asan.splitlines() if ASAN_REPORT.search(ln)).strip())
    if ours:
        reasons.append(f"{ours} ThreadSanitizer report(s) in the backend or canary code")
    if not results and not reasons:
        reasons.append(f"no canary check ran (exit {rc_code})")
    if rc_code not in (0, 1) and not reasons:
        reasons.append(f"the canary binary exited {rc_code}")
    status = "fail" if reasons else "pass"
    tsan = tsan if ours else ""
    tail = "\n".join(x for x in (asan, tsan, "\n".join(json.dumps(r) for r in failing or results), log[-3000:]) if x)
    payload = {
        "gate": gate, "status": status, "reason": "; ".join(reasons) if reasons else f"{len(results)} canary checks passed"
        + (f" ({engine_only} ThreadSanitizer report(s) inside the uninstrumented engine only, not judged)" if engine_only else ""),
        "engine_only_reports": engine_only,
        "log_tail": tail[-6000:], "engine_name": engine, "returncode": rc_code,
        "checks": results, "owner_layers": sorted({r["owner_layer"] for r in failing}),
    }
    if failing:
        payload["owner_layer"] = failing[0]["owner_layer"]
    path = Path(out)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2))
    print(json.dumps({k: payload[k] for k in ("gate", "status", "reason")}, indent=2))
    return 0 if status == "pass" else 1


if __name__ == "__main__":
    sys.exit(main())
