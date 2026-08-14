"""Load repo-root .env into os.environ (does not override existing variables)."""

from __future__ import annotations

import os
from pathlib import Path


def _strip_quotes(value: str) -> str:
    value = value.strip()
    if len(value) >= 2 and value[0] == value[-1] and value[0] in {"'", '"'}:
        return value[1:-1]
    return value


def load_repo_dotenv(repo_root: Path) -> Path | None:
    """Load `.env` then `.env.local` from repo_root. Returns path loaded, if any."""
    loaded: Path | None = None
    for name in (".env", ".env.local"):
        path = repo_root / name
        if not path.is_file():
            continue
        for raw_line in path.read_text(encoding="utf-8").splitlines():
            line = raw_line.strip()
            if not line or line.startswith("#"):
                continue
            if line.startswith("export "):
                line = line[len("export ") :].strip()
            if "=" not in line:
                continue
            key, _, value = line.partition("=")
            key = key.strip()
            if not key:
                continue
            os.environ.setdefault(key, _strip_quotes(value))
        loaded = path
    return loaded
