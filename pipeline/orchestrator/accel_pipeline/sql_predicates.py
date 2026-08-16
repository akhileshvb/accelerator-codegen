"""Extract predicate polarity checklist from reference SQL for implementation prompts."""

from __future__ import annotations

import re
from pathlib import Path


def _norm(sql: str) -> str:
    return re.sub(r"\s+", " ", sql.strip())


def extract_predicate_checklist(sql: str) -> list[str]:
    """Return human checklist lines derived from SQL operator polarity."""
    if not sql or not sql.strip():
        return []
    text = sql
    upper = sql.upper()
    checks: list[str] = []

    if re.search(r"\bNOT\s+LIKE\b", upper):
        patterns = re.findall(r"NOT\s+LIKE\s+('[^']+'|\$\d+)", text, flags=re.IGNORECASE)
        detail = ", ".join(patterns) if patterns else "bound pattern"
        checks.append(
            f"NOT LIKE ({detail}): surviving fixture values must NOT match the pattern; "
            "never bind a pattern that matches the rows you expect to keep."
        )
    elif re.search(r"\bLIKE\b", upper):
        checks.append(
            "LIKE filter present: surviving fixture values must match the pattern; "
            "add a non-matching row that is excluded."
        )

    if re.search(r"<>|!=", text):
        checks.append(
            "Inequality (<> / !=): include one row that differs (kept) and one that equals "
            "the excluded key (dropped)."
        )

    if re.search(r"\bNOT\s+IN\b", upper) or "CUSTOMER%COMPLAINTS" in upper.replace(" ", ""):
        checks.append(
            "Anti-join / NOT IN / complaint exclusion: excluded keys contribute 0 to counts; "
            "non-excluded keys must be counted. Do not store eligibility as the inverse bit."
        )

    if re.search(r"\bCOUNT\s*\(\s*DISTINCT\b", upper):
        checks.append(
            "COUNT(DISTINCT …): duplicate fact links to the same key must not inflate the measure."
        )

    if re.search(r"\bIN\s*\(", upper) and "SIZE" in upper:
        checks.append(
            "Size / IN-list window: only requested keys contribute; absent partitions emit nothing."
        )

    if not checks:
        checks.append(
            "Derive every Execute filter from this SQL; dual-path (hot Execute vs fallback) must agree."
        )
    else:
        checks.append(
            "Hot-path Execute must apply the same predicates as fallback / SQL (never filter only in fallback)."
        )
    return checks


def load_reference_sql(path: str | Path | None, *, max_chars: int = 2500) -> str:
    if not path:
        return ""
    p = Path(path)
    if not p.is_file():
        return ""
    text = p.read_text(encoding="utf-8")
    text = text.strip()
    if len(text) > max_chars:
        return text[: max_chars - 20] + "\n-- … truncated …\n"
    return text


def format_predicate_bootstrap_section(sql: str, sql_path: str = "") -> str:
    checklist = extract_predicate_checklist(sql)
    lines = [
        "### Reference SQL predicate contract (authoritative)",
        "",
        "Synthetic unit fixtures **must** obey this SQL. See `pipeline/prompts/aux_state/shared/unit_test_contract.md`.",
        "",
    ]
    if sql_path:
        lines.append(f"Source: `{sql_path}`")
        lines.append("")
    if sql.strip():
        lines.extend(["```sql", _norm(sql) if len(sql) < 400 else sql.strip(), "```", ""])
    lines.append("Checklist:")
    for item in checklist:
        lines.append(f"- {item}")
    lines.append("")
    return "\n".join(lines)
