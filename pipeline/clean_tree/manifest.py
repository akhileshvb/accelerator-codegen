#!/usr/bin/env python3
"""The clean-tree manifest (manifest.yaml): classify repo paths, prune a tree to its KEEP set.

    manifest.py classify <repo> [--rev REV]   print KEEP/GENERATED/DROP per tracked file
    manifest.py prune <tree>                  delete GENERATED and DROP files from an exported tree,
                                              then overlay the skeleton
"""
from __future__ import annotations

import re
import shutil
import subprocess
import sys
from pathlib import Path

import yaml

HERE = Path(__file__).resolve().parent
KEEP, GENERATED, DROP = "KEEP", "GENERATED", "DROP"


def _glob_regex(pattern: str) -> re.Pattern[str]:
    if pattern.endswith("/"):
        pattern += "**"
    out, i = [], 0
    while i < len(pattern):
        if pattern.startswith("**/", i):
            out.append("(?:.*/)?")
            i += 3
        elif pattern.startswith("**", i):
            out.append(".*")
            i += 2
        elif pattern[i] == "*":
            out.append("[^/]*")
            i += 1
        else:
            out.append(re.escape(pattern[i]))
            i += 1
    return re.compile("".join(out) + r"\Z")


class Manifest:
    def __init__(self, data: dict):
        self.skeleton: str = data["skeleton"]
        self._rules = {
            cls: [_glob_regex(p) for p in data.get(cls.lower()) or []] for cls in (DROP, GENERATED, KEEP)
        }

    @classmethod
    def load(cls, path: Path = HERE / "manifest.yaml") -> "Manifest":
        return cls(yaml.safe_load(path.read_text(encoding="utf-8")))

    def classify(self, path: str) -> str | None:
        """KEEP, GENERATED or DROP (strongest match wins), None when no glob matches."""
        return next((c for c, rules in self._rules.items() if any(r.match(path) for r in rules)), None)


def tracked_files(repo: Path, rev: str | None = None) -> list[str]:
    cmd = ["git", "ls-tree", "-r", "--name-only", "-z", rev] if rev else ["git", "ls-files", "-z"]
    out = subprocess.run(cmd, cwd=repo, capture_output=True, check=True).stdout.decode()
    return [p for p in out.split("\0") if p]


def prune(tree: Path, manifest: Manifest) -> list[str]:
    """Remove every non-KEEP file under `tree`, then overlay the skeleton; returns the removed paths."""
    removed = []
    for path in sorted(p for p in tree.rglob("*") if (p.is_file() or p.is_symlink()) and ".git" not in p.parts):
        rel = path.relative_to(tree).as_posix()
        if manifest.classify(rel) != KEEP:
            path.unlink()
            removed.append(rel)
    for directory in sorted((p for p in tree.rglob("*") if p.is_dir()), reverse=True):
        if ".git" not in directory.parts and not any(directory.iterdir()):
            directory.rmdir()
    skeleton = tree / manifest.skeleton
    if skeleton.is_dir():
        shutil.copytree(skeleton, tree, dirs_exist_ok=True)
        shutil.rmtree(skeleton)
    return removed


def main(argv: list[str]) -> int:
    if len(argv) >= 3 and argv[1] == "classify":
        repo, rev = Path(argv[2]), (argv[4] if len(argv) > 4 and argv[3] == "--rev" else None)
        manifest = Manifest.load()
        for rel in tracked_files(repo, rev):
            print(f"{manifest.classify(rel) or 'UNCLASSIFIED':12s}{rel}")
        return 0
    if len(argv) == 3 and argv[1] == "prune":
        tree = Path(argv[2])
        removed = prune(tree, Manifest.load(tree / "pipeline/clean_tree/manifest.yaml"))
        print(f"pruned {len(removed)} files")
        return 0
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
