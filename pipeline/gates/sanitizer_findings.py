#!/usr/bin/env python3
"""Sorts sanitizer reports into the query's own and infrastructure, compactly.

usage: sanitizer_findings.py <log dir> <query dir fragment>

A report is the query's own when, in the stack of any of its accesses, the first frame that is not a system
header or runtime library is a source path under the query directory (the fragment, e.g.
accelerators/<bench>/q<N>/): the racing access is in the query's code, however deep a container or atomic
beneath it, or the location the race names (a global, or a heap block allocated by a frame) is under the query
directory (a global defined there, or a heap block whose allocating frame, the first that is not a system one, is
there). A race inside the (uninstrumented) engine or the substrate that merely sits under a call from the query is
not the query's. Every other report is summarized as infrastructure and never fails the check. Prints JSON:
{"yours": [{"summary", "frames"}], "infra": {"count", "summaries": [...]}, "owner"}, where owner is "query" when
any report is the query's, "backend" when every report is inside the engine or the substrate, "" without reports.
"""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path

START = re.compile(r"^(WARNING: ThreadSanitizer|ERROR: AddressSanitizer|ERROR: LeakSanitizer)|runtime error:")
SUMMARY = re.compile(r"^SUMMARY: (Thread|Address|UndefinedBehavior|Leak)Sanitizer")
FRAME = re.compile(r"^\s+#\d+ ")
ACCESS = re.compile(r"^\s+(Atomic |Previous |Write|Read|Mutex).* by (main )?thread")
SYSTEM = re.compile(r"libtsan|libasan|libubsan|libstdc\+\+|libc\.so|\s/usr/(include|lib)/")
MAX_FRAMES = 8
MAX_YOURS = 5
MAX_INFRA = 5


def reports(text: str) -> list[list[str]]:
    out, cur = [], None
    for line in text.splitlines():
        if START.search(line):
            if cur and not any(SUMMARY.match(x) for x in cur):
                out.append(cur)  # UBSan prints no SUMMARY block per report
            cur = [line]
        elif cur is not None:
            cur.append(line)
            if SUMMARY.match(line):
                out.append(cur)
                cur = None
    if cur:
        out.append(cur)
    return out


def access_stacks(rep: list[str]) -> list[list[str]]:
    """The frames of each racing access (TSan), or of the report's stacks as one (ASan, UBSan)."""
    if not rep[0].startswith("WARNING: ThreadSanitizer"):
        return [[l.strip() for l in rep if FRAME.match(l)] or [l.strip() for l in rep[:1]]]
    stacks: list[list[str]] = []
    active = False  # inside an access (not a "Location is ..." or "Thread T created by ..." section)
    for line in rep:
        if ACCESS.match(line):
            stacks.append([])
            active = True
        elif FRAME.match(line):
            if active:
                stacks[-1].append(line.strip())
        elif line.strip():
            active = False
    return stacks


def location_lines(rep: list[str]) -> list[str]:
    """The "Location is ..." section of a TSan report: what the racing address belongs to."""
    out, inside = [], False
    for line in rep:
        if line.strip().startswith("Location is"):
            inside = True
        elif not line.strip():
            inside = False
        if inside:
            out.append(line)
    return out


def is_yours(rep: list[str], fragment: str) -> bool:
    location = location_lines(rep)
    allocation = [l.strip() for l in location if FRAME.match(l)]
    # the location's own line (a global's file), else the frame that allocated the heap block
    stacks = [*access_stacks(rep), allocation] if allocation else access_stacks(rep)
    if location and not allocation and fragment in location[0]:
        return True
    for stack in stacks:
        first = next((f for f in stack if not SYSTEM.search(" " + f)), None)
        if first and fragment in first:
            return True
    return False


def classify(log_dir: Path, fragment: str) -> dict:
    yours, infra = [], []
    for path in sorted(log_dir.glob("*")):
        if not path.is_file() or not re.match(r"(tsan|asan|ubsan)", path.name):
            continue
        for rep in reports(path.read_text(errors="replace")):
            summary = next((l for l in rep if SUMMARY.match(l)), rep[0]).strip()
            frames = [l.strip() for l in rep if FRAME.match(l) and "libtsan" not in l and "libasan" not in l]
            mine = [f for f in frames if fragment in f]
            if is_yours(rep, fragment):
                yours.append({"summary": summary, "frames": (mine + [f for f in frames if f not in mine])[:MAX_FRAMES]})
            else:
                infra.append(summary)
    return {
        "yours": yours[:MAX_YOURS], "yours_count": len(yours),
        "infra": {"count": len(infra), "summaries": sorted(set(infra))[:MAX_INFRA]},
        "owner": "query" if yours else "backend" if infra else "",
    }


if __name__ == "__main__":
    print(json.dumps(classify(Path(sys.argv[1]), sys.argv[2])))
