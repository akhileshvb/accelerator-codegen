"""Code stamps: one digest of the source state a build or a gate result depends on.

The stamp covers the checked-out commit and the content of every uncommitted file, except what
cannot change this query's build: pipeline code, scripts, docs and the other queries' directories.
Gate scripts use it to skip a build that already succeeded (build_stamp CLI below); the repair and
gate-plan code uses it to reuse a gate result while the stamp still matches.

    python3 build_stamp.py <repo_root> [--query-dir DIR] [EXTRA ...]   # prints the stamp
"""

from __future__ import annotations

import fcntl
import hashlib
import re
import subprocess
import sys
from contextlib import contextmanager
from pathlib import Path
from typing import Iterator, Sequence

IRRELEVANT_TOP_DIRS = frozenset({"pipeline", "scripts", "docs"})
_BUILD_OR_VCS = re.compile(r"^(\.git|build([-._/]|$))")  # build dirs and their lock files, at the repo root
_QUERY_DIR = re.compile(r"^accelerators/[A-Za-z0-9_]+/q\d+/")


def git(repo_root: Path, *args: str) -> bytes:
    return subprocess.run(["git", *args], cwd=repo_root, capture_output=True, check=True).stdout


def uncommitted_files(repo_root: Path) -> list[str]:
    """Paths git reports as modified, added, deleted or untracked (renames count both ends)."""
    entries = git(repo_root, "status", "--porcelain=v1", "-z", "-uall").decode("utf-8", "replace").split("\0")
    paths: list[str] = []
    entries_iter = iter(entry for entry in entries if entry)
    for entry in entries_iter:
        paths.append(entry[3:])
        if entry[0] in "RC":
            paths.append(next(entries_iter, ""))
    return sorted(set(path for path in paths if path))


def is_relevant(path: str, query_dir: str) -> bool:
    """Whether `path` can change this query's build or behaviour: everything but pipeline code, scripts,
    docs, build output and the other queries' directories (accelerators, prims, backends, substrate,
    cmake and the shared accelerators/<bench>/common are all relevant)."""
    if path.split("/", 1)[0] in IRRELEVANT_TOP_DIRS or _BUILD_OR_VCS.match(path):
        return False
    return not (query_dir and _QUERY_DIR.match(path) and not path.startswith(query_dir.rstrip("/") + "/"))


def code_stamp(repo_root: Path, *, query_dir: str = "", extra: Sequence[str] = ()) -> str:
    """Digest of HEAD plus the relevant uncommitted files for `query_dir` (repo-relative), mixed with `extra`.
    "" when the tree is not a git checkout: no stamp matches anything."""
    try:
        digest = hashlib.sha256(git(repo_root, "rev-parse", "HEAD"))
        changed = uncommitted_files(repo_root)
    except (OSError, subprocess.CalledProcessError):
        return ""
    for path in changed:
        if not is_relevant(path, query_dir):
            continue
        file = repo_root / path
        content = hashlib.sha256(file.read_bytes()).hexdigest() if file.is_file() else "deleted"
        digest.update(f"\0{path}\0{content}".encode())
    for item in extra:
        digest.update(f"\0{item}".encode())
    return digest.hexdigest()


@contextmanager
def build_lock(build_dir: Path) -> Iterator[None]:
    """Exclusive lock on a build directory: concurrent gates and tools serialize their builds on it.
    The lock file is the one the gate scripts' flock uses (aux_gate_common.sh engine_steps)."""
    lock_path = Path(f"{build_dir}.build.lock")
    lock_path.parent.mkdir(parents=True, exist_ok=True)
    with lock_path.open("a") as handle:
        fcntl.flock(handle, fcntl.LOCK_EX)
        try:
            yield
        finally:
            fcntl.flock(handle, fcntl.LOCK_UN)


def main(argv: list[str]) -> int:
    query_dir = ""
    args = list(argv)
    if "--query-dir" in args:
        at = args.index("--query-dir")
        query_dir = args[at + 1]
        del args[at : at + 2]
    if not args:
        print(__doc__, file=sys.stderr)
        return 2
    print(code_stamp(Path(args[0]), query_dir=query_dir, extra=args[1:]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
