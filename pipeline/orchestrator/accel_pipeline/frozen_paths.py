"""The pipeline's own files and the frozen substrate are not the agents' to change.

An agent stage that edits a gate, prompt, contract, the orchestrator or the substrate gets
them back exactly as they were before it ran; what it touched is reported so the run log
shows it.
"""

from __future__ import annotations

from pathlib import Path

FROZEN = (
    "substrate",
    "cmake",
    "CMakeLists.txt",
    "pipeline/codex",
    "pipeline/config",
    "pipeline/gates",
    "pipeline/orchestrator",
    "pipeline/prompts",
    "pipeline/schemas",
    "pipeline/templates",
)
_SKIP_PARTS = {"__pycache__", ".venv", ".pytest_cache"}


def _files(repo_root: Path) -> list[Path]:
    out: list[Path] = []
    for rel in FROZEN:
        p = repo_root / rel
        if p.is_file():
            out.append(p)
        elif p.is_dir():
            out.extend(
                f for f in p.rglob("*")
                if f.is_file() and not _SKIP_PARTS.intersection(f.relative_to(repo_root).parts)
            )
    return out


def snapshot(repo_root: Path) -> dict[Path, bytes]:
    return {f: f.read_bytes() for f in _files(repo_root)}


def restore(repo_root: Path, before: dict[Path, bytes]) -> list[str]:
    """Undo every change to the frozen paths since `before`; return the paths that changed."""
    changed: list[str] = []
    now = set(_files(repo_root))
    for f in sorted(now - before.keys()):
        f.unlink()
        changed.append(f"added {f.relative_to(repo_root)}")
    for f, data in before.items():
        if f in now and f.read_bytes() == data:
            continue
        f.parent.mkdir(parents=True, exist_ok=True)
        f.write_bytes(data)
        changed.append(f"{'edited' if f in now else 'deleted'} {f.relative_to(repo_root)}")
    return changed
