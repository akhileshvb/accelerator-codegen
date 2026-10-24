#!/usr/bin/env python3
"""The result of one canary gate run: the checks' JSON lines and the sanitizer reports in the run's
log, as the gate's JSON (status, reason, the owning layers of the failures, the log tail).

usage: canary_result.py <out.json> <gate> <rc> <run_reason> <report_dir> <engine> <repo_root>
"""
from __future__ import annotations

import json
import re
import subprocess
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


def tsan_reports(report_dir: Path, engine: str, root: str) -> tuple[int, int, str]:
    """(reports in our code, reports in the engine only, the first report's text)."""
    ours = engine_only = 0
    first = ""
    pattern = f"{root}/(substrate|backends/{engine}|pipeline/canary)/"
    awk = Path(__file__).with_name("tsan_classify.awk")
    for path in sorted(report_dir.glob("tsan.*")):
        counts = subprocess.run(["awk", "-v", f"pat={pattern}", "-f", str(awk), str(path)],
                                capture_output=True, text=True).stdout.split()
        if len(counts) == 2:
            ours += int(counts[0])
            engine_only += int(counts[1])
        if not first:
            first = "\n".join(path.read_text(errors="replace").splitlines()[:EXCERPT_LINES])
    return ours, engine_only, first


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
    if ours or engine_only:
        reasons.append(f"{ours} ThreadSanitizer report(s) in the backend or canary code, {engine_only} in the engine only")
    if not results and not reasons:
        reasons.append(f"no canary check ran (exit {rc_code})")
    if rc_code not in (0, 1) and not reasons:
        reasons.append(f"the canary binary exited {rc_code}")
    status = "fail" if reasons else "pass"
    tail = "\n".join(x for x in (asan, tsan, "\n".join(json.dumps(r) for r in failing or results), log[-3000:]) if x)
    payload = {
        "gate": gate, "status": status, "reason": "; ".join(reasons) if reasons else f"{len(results)} canary checks passed",
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
