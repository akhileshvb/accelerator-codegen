"""Static review of kernel sources for common hot-path performance smells."""

from __future__ import annotations

import re
from pathlib import Path
from typing import Any

# Functions that usually sit on the per-row consume / aggregate path.
_HOT_PATH_FUNC_RE = re.compile(
    r"\b(void|bool)\s+("
    r"ProcessNum\w*|Accumulate\w*|ConsumeNum\w*|Aggregate\w*|Insert\w*Row|"
    r"Process\w*Chunk\w*|RunNum\w*Pass"
    r")\s*\(",
    re.MULTILINE,
)

_STRING_KEYED_AGG_MAP_RE = re.compile(
    r"unordered_map\s*<\s*[^>]*(?:std::string|NameCikKey|GroupKey)[^>]*,\s*"
    r"(?:double|float|int(?:32|64)_t|size_t|duckdb::idx_t)",
    re.MULTILINE,
)

_STRING_KEYED_INTERN_MAP_RE = re.compile(
    r"unordered_map\s*<\s*[^>]*(?:std::string|NameCikKey|GroupKey)[^>]*,\s*uint32_t",
    re.MULTILINE,
)

_STRING_HASH_RE = re.compile(r"std::hash\s*<\s*std::string\s*>")

_TO_STORED_STRING_RE = re.compile(r"\bToStoredString\s*\(")

_BUILD_PHASE_HINTS = re.compile(
    r"\b(InsertSubRow|InternGroup|Build\w*|RunSubScan|ConsumeSubChunk)\b",
    re.MULTILINE,
)


def _function_spans(text: str) -> list[tuple[str, int, int]]:
    """Rough function body spans: (name, start, end) by brace matching."""
    spans: list[tuple[str, int, int]] = []
    for match in _HOT_PATH_FUNC_RE.finditer(text):
        name = match.group(2)
        brace = text.find("{", match.end())
        if brace < 0:
            continue
        depth = 0
        end = brace
        for idx in range(brace, len(text)):
            ch = text[idx]
            if ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
                if depth == 0:
                    end = idx + 1
                    break
        spans.append((name, match.start(), end))
    return spans


def _line_number(text: str, index: int) -> int:
    return text.count("\n", 0, index) + 1


def analyze_kernel_source(kernel_path: Path) -> dict[str, Any]:
    """Return structured hot-path smells for a kernel translation unit."""
    if not kernel_path.is_file():
        return {"kernel_path": str(kernel_path), "smells": [], "status": "missing"}

    text = kernel_path.read_text(encoding="utf-8", errors="replace")
    smells: list[dict[str, Any]] = []

    hot_spans = _function_spans(text)
    hot_text = "\n".join(text[start:end] for _, start, end in hot_spans) if hot_spans else ""

    for match in _STRING_KEYED_AGG_MAP_RE.finditer(text):
        line = _line_number(text, match.start())
        smells.append(
            {
                "id": "string_keyed_hash_map",
                "severity": "high",
                "line": line,
                "message": (
                    "Kernel declares a hash map with string-composite keys mapped to numeric "
                    "aggregates — typical per-row GROUP BY update pattern."
                ),
                "suggestion": (
                    "Intern GROUP BY / dimension keys during build or bind (sub scan, "
                    "dimension table load). Store `uint32_t group_id` on lookup rows; "
                    "update `std::vector<double> group_sums` on the consume path."
                ),
            }
        )

    if _STRING_KEYED_INTERN_MAP_RE.search(text) and hot_text:
        if re.search(r"name_cik_to_group\.(?:find|emplace|try_emplace|\[)", hot_text):
            smells.append(
                {
                    "id": "intern_map_on_hot_path",
                    "severity": "medium",
                    "line": None,
                    "message": (
                        "Build-time intern map may be probed from a consume function — "
                        "resolve group_id during sub/build only."
                    ),
                    "suggestion": (
                        "Store `group_id` on lookup rows (e.g. FilingInfo) during sub scan; "
                        "never touch the intern map inside ProcessNum*."
                    ),
                }
            )

    if _STRING_HASH_RE.search(text) and (
        _STRING_KEYED_AGG_MAP_RE.search(text) or _STRING_HASH_RE.search(hot_text)
    ):
        smells.append(
            {
                "id": "std_string_hash",
                "severity": "medium",
                "line": None,
                "message": "Uses `std::hash<std::string>` — expensive on a hot aggregation path.",
                "suggestion": (
                    "Replace string hashing with build-time interning to dense integer group ids "
                    "(see q16 `GetOrCreateGroupId`, SEC-EDGAR Q1 `AdshInternTable`)."
                ),
            }
        )

    if hot_spans:
        if _TO_STORED_STRING_RE.search(hot_text):
            smells.append(
                {
                    "id": "heap_string_in_hot_path",
                    "severity": "high",
                    "line": None,
                    "message": (
                        "`ToStoredString` / heap string allocation appears inside a likely "
                        "consume/aggregate function."
                    ),
                    "suggestion": (
                        "Move string materialization to build phase only; pass `string_t` or "
                        "interned ids through the num scan loop."
                    ),
                }
            )
        if re.search(r"\.find\s*\(", hot_text) and _STRING_KEYED_AGG_MAP_RE.search(text):
            smells.append(
                {
                    "id": "map_find_in_hot_path",
                    "severity": "high",
                    "line": None,
                    "message": (
                        "Hash map `.find()` in a consume/aggregate function while string-keyed "
                        "maps are present — likely per-row probe cost."
                    ),
                    "suggestion": (
                        "Replace per-row map lookup with O(1) indexing via interned `group_id` "
                        "or integer slot id resolved once at build time."
                    ),
                }
            )
    elif _TO_STORED_STRING_RE.search(text) and not _BUILD_PHASE_HINTS.search(text):
        smells.append(
            {
                "id": "heap_string_allocation",
                "severity": "medium",
                "line": None,
                "message": "Kernel allocates stored strings; verify this is build-phase only.",
                "suggestion": "Confine `ToStoredString` to sub/build paths, not num aggregation loops.",
            }
        )

    # De-dupe by id (keep first).
    seen: set[str] = set()
    unique: list[dict[str, Any]] = []
    for smell in smells:
        key = smell["id"]
        if key in seen:
            continue
        seen.add(key)
        unique.append(smell)

    return {
        "kernel_path": str(kernel_path),
        "status": "ok",
        "smells": unique,
    }


def format_kernel_review_for_prompt(review: dict[str, Any]) -> list[str]:
    lines: list[str] = []
    smells = review.get("smells") or []
    if not smells:
        lines.append("- No high-confidence hot-path structure smells detected in kernel source.")
        return lines
    lines.append(
        "- **Static kernel review** (hints only — confirm with profile + a targeted read before acting):"
    )
    for smell in smells[:6]:
        sev = smell.get("severity", "info")
        line = smell.get("line")
        loc = f" (line {line})" if line else ""
        lines.append(f"  - [{sev}]{loc} {smell.get('message', '')}")
        suggestion = smell.get("suggestion")
        if suggestion:
            lines.append(f"    → {suggestion}")
    return lines
