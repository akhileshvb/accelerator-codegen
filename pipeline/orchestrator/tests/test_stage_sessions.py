"""Which codex session each stage runs in: analysis, implementation, optimization and repairs share
the main session; the test author and the audit are independent."""

from __future__ import annotations

import re
from pathlib import Path

STAGES = Path(__file__).resolve().parents[1] / "accel_pipeline" / "stages"


def _role(stage_file: str) -> str:
    text = (STAGES / stage_file).read_text(encoding="utf-8")
    roles = set(re.findall(r"session_role=([A-Z_]+)", text))
    assert len(roles) == 1, roles
    return roles.pop()


def test_implementation_continues_the_analysis_conversation():
    assert _role("analysis.py") == _role("implementation.py") == "MAIN"


def test_the_test_author_and_the_audit_keep_their_own_sessions():
    assert _role("test_suite.py") == "TESTER"
    assert _role("audit.py") == "READONLY"
