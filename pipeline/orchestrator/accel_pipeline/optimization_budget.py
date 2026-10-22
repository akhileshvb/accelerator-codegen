"""Per-attempt state file for optimization sub-stages."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any


def save_attempt_state(art: Path, state: dict[str, Any]) -> None:
    (art / "attempt_state.json").write_text(json.dumps(state, indent=2), encoding="utf-8")

