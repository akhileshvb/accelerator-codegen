from __future__ import annotations

import re
from pathlib import Path

from accel_pipeline.paths import pipeline_root


def merge_playbook_proposal(repo_root: Path, query: int) -> bool:
    """Append proposal content to shared playbook if proposal exists.

    TODO: Parse structured playbook_proposal.md and merge/dedupe by technique
    (cap wins/failures per technique; mark stale entries when kernels change).
    """
    art = pipeline_root(repo_root) / "artifacts" / f"q{query}"
    proposal = art / "playbook_proposal.md"
    if not proposal.is_file():
        return False

    playbook = pipeline_root(repo_root) / "artifacts" / "shared" / "optimization_playbook.md"
    text = proposal.read_text(encoding="utf-8").strip()
    if not text:
        return False

    header = f"\n\n## Q{query} update\n"
    existing = playbook.read_text(encoding="utf-8") if playbook.is_file() else ""
    playbook.parent.mkdir(parents=True, exist_ok=True)
    playbook.write_text(existing.rstrip() + header + text + "\n", encoding="utf-8")
    return True


def append_experiment_log(repo_root: Path, query: int, line: str) -> Path:
    path = pipeline_root(repo_root) / "artifacts" / f"q{query}" / "experiment_log.md"
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a", encoding="utf-8") as f:
        f.write(line.rstrip() + "\n")
    return path


def append_patch_log(repo_root: Path, query: int, message: str) -> None:
    path = pipeline_root(repo_root) / "artifacts" / f"q{query}" / "patches.log"
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a", encoding="utf-8") as f:
        f.write(message.rstrip() + "\n")
