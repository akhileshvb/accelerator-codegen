"""canary --resume: the run's tree takes the pipeline of another revision without losing what the run wrote."""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent / "smoke"))
import canary  # noqa: E402


def git(cwd: Path, *args: str) -> None:
    subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@t", "-C", str(cwd), *args], check=True, capture_output=True)


def repo(path: Path, files: dict[str, str]) -> Path:
    path.mkdir()
    git(path, "init", "-q")
    for name, text in files.items():
        (path / name).parent.mkdir(parents=True, exist_ok=True)
        (path / name).write_text(text)
    git(path, "add", "-A")
    git(path, "commit", "-qm", "base")
    return path


def test_resume_takes_the_pipelines_files_from_the_base_and_keeps_the_runs_own(tmp_path):
    base = repo(tmp_path / "base", {"pipeline/a.py": "new a", "pipeline/log.jsonl": "", "eval/t.cpp": "new test",
                                    "accelerators/prims/p.hpp": "base prim"})
    tree = repo(tmp_path / "tree", {"pipeline/a.py": "old a", "pipeline/gone.py": "", "pipeline/log.jsonl": "",
                                    "accelerators/prims/p.hpp": "published prim", "accelerators/q/m.cpp": "kernel"})
    (tree / "pipeline/log.jsonl").write_text("the run's record")
    (tree / "pipeline/artifacts").mkdir()
    (tree / "pipeline/artifacts/state.json").write_text("{}")

    canary.replace_tracked(tree, base)

    assert (tree / "pipeline/a.py").read_text() == "new a" and not (tree / "pipeline/gone.py").exists()
    assert (tree / "eval/t.cpp").read_text() == "new test"
    assert (tree / "pipeline/log.jsonl").read_text() == "the run's record"
    assert (tree / "pipeline/artifacts/state.json").exists()
    assert (tree / "accelerators/prims/p.hpp").read_text() == "published prim" and (tree / "accelerators/q/m.cpp").exists()
