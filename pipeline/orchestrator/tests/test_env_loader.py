from __future__ import annotations

import os
from pathlib import Path

from accel_pipeline.env_loader import load_repo_dotenv


def test_load_repo_dotenv_sets_missing_keys(tmp_path: Path):
    env_file = tmp_path / ".env"
    env_file.write_text(
        "OPENROUTER_API_KEY=from-file\n"
        "# comment\n"
        'QUOTED="hello"\n',
        encoding="utf-8",
    )
    os.environ.pop("OPENROUTER_API_KEY", None)
    os.environ.pop("QUOTED", None)
    loaded = load_repo_dotenv(tmp_path)
    assert loaded == env_file
    assert os.environ["OPENROUTER_API_KEY"] == "from-file"
    assert os.environ["QUOTED"] == "hello"


def test_load_repo_dotenv_does_not_override_existing(tmp_path: Path):
    env_file = tmp_path / ".env"
    env_file.write_text("OPENROUTER_API_KEY=from-file\n", encoding="utf-8")
    os.environ["OPENROUTER_API_KEY"] = "from-shell"
    try:
        load_repo_dotenv(tmp_path)
        assert os.environ["OPENROUTER_API_KEY"] == "from-shell"
    finally:
        os.environ.pop("OPENROUTER_API_KEY", None)
