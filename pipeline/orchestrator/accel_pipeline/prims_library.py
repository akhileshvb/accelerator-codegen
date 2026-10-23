"""Growable primitives library: accelerators/prims/.

Accelerator modules build their versioned state from the generic primitives in
that directory.  When none fits, an aux_state agent adds a new generic one.  The
library is shared by every worker, so it only changes through `primitive_publish`
(the C++ analogue of the Verus `prooflib_publish`):

* the agent stages candidate files under ``pipeline/prims_staging/<repo-relative
  path>`` (direct writes anywhere under accelerators/prims/ -- the headers, README.md
  and tests/test_prims.cpp -- are denied by ``write_ownership``);
* publish takes an exclusive lock, checks the candidates are additions only,
  structurally complete (header comment, tests), not a duplicate of a published
  primitive, and workload-agnostic; appends the catalogue row to README.md itself
  (generated from the header comment, so the README always matches the headers),
  writes them into the PRIMARY tree with a backup, builds and runs ``test_prims``
  (and every target that includes a changed existing header), restores the backup
  on any failure, and appends a provenance record on success;
* ``refresh_prims`` mirrors the primary's library into a worker worktree (stage
  start, after publish) so concurrent workers see published primitives.  Nothing
  else ever copies prims/ back to the primary (``sync_owned_to_primary`` skips it).
"""

from __future__ import annotations

import difflib
import fcntl
import importlib.util
import json
import os
import re
import shutil
import subprocess
import tempfile
import time
from contextlib import contextmanager
from pathlib import Path
from typing import Any, Iterator

from accel_pipeline.textutil import tail_marked

PRIMS_REL = "accelerators/prims"
README_REL = f"{PRIMS_REL}/README.md"
TESTS_REL = f"{PRIMS_REL}/tests/test_prims.cpp"
# Include form of a primitive: "prims/<name>" (accelerators/ is an include root).
INCLUDE_PREFIX = "prims"
STAGING_REL = "pipeline/prims_staging"
PROVENANCE_REL = "pipeline/prims_provenance.jsonl"
LOCK_REL = "pipeline/.locks/prims.lock"
CMAKE_SRC_REL = "pipeline/gates/prims_test"
BUILD_DIR_NAME = "build-prims"

HEADER_FIELDS = ("PURPOSE", "KEY SPACE", "COMPLEXITY", "MEMORY", "COW/THREADS", "USE WHEN")
CATALOGUE_HEADING = "### Catalogue"
CATALOGUE_COLUMNS = ("Header", "Purpose", "Key space", "Memory per element", "Thread-safety", "Use when")
CATALOGUE_FIELDS = ("PURPOSE", "KEY SPACE", "MEMORY", "COW/THREADS", "USE WHEN")
DUPLICATE_PURPOSE_SIMILARITY = 0.6
_HEADER_SUFFIXES = (".hpp", ".h")
_SOURCE_SUFFIXES = (".cpp", ".cc", ".hpp", ".h")
_RANDOM_RE = re.compile(r"mt19937|minstd|std::rand|\brng\b|random", re.I)
MIN_TEST_LINES = 15

DENY_MESSAGE = (
    "the primitives library is shared by every worker and is changed only through the "
    "primitive_publish tool: stage the candidate file(s) under "
    f"{STAGING_REL}/<repo-relative path> (new header, full updated README.md, full updated "
    "test_prims.cpp) and call primitive_publish(files, rationale). Additions only, generic only."
)


# ---------------------------------------------------------------- paths

def is_prims_path(rel: str) -> bool:
    """True for the files only primitive_publish may change."""
    rel = rel.replace("\\", "/").lstrip("./")
    return rel.startswith(PRIMS_REL + "/") or rel == PRIMS_REL or rel == TESTS_REL


def is_staging_path(rel: str) -> bool:
    return rel.replace("\\", "/").lstrip("./").startswith(STAGING_REL + "/")


def primary_root(repo_root: Path) -> Path:
    env = os.environ.get("ACCEL_PIPELINE_PRIMARY_REPO")
    return Path(env).resolve() if env else Path(repo_root).resolve()


def _norm_rel(path: str, repo_root: Path | None = None) -> str:
    rel = str(path).replace("\\", "/")
    if repo_root is not None and rel.startswith("/"):
        try:
            rel = str(Path(rel).resolve().relative_to(Path(repo_root).resolve()))
        except ValueError:
            return rel
    while rel.startswith("./"):
        rel = rel[2:]
    if rel.startswith(STAGING_REL + "/"):
        rel = rel[len(STAGING_REL) + 1:]
    return rel


# ---------------------------------------------------------------- process / lock

def _run(cmd: list[str], *, cwd: Path, env: dict[str, str] | None = None, timeout: int = 1800) -> tuple[int, str]:
    """Run a command; (returncode, combined output).  Tests monkeypatch this."""
    try:
        proc = subprocess.run(cmd, cwd=str(cwd), env=env, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired as exc:
        out = exc.stdout if isinstance(exc.stdout, str) else ""
        return 124, out + f"\ntimed out after {timeout}s"
    except FileNotFoundError as exc:
        return 127, str(exc)
    return proc.returncode, (proc.stdout or "") + (proc.stderr or "")


@contextmanager
def file_lock(path: Path) -> Iterator[None]:
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "a+") as fh:
        fcntl.flock(fh, fcntl.LOCK_EX)
        try:
            yield
        finally:
            fcntl.flock(fh, fcntl.LOCK_UN)


def _git_head(root: Path) -> str:
    rc, out = _run(["git", "rev-parse", "HEAD"], cwd=root, timeout=30)
    return out.strip() if rc == 0 else ""


# ---------------------------------------------------------------- refresh

def _same(a: Path, b: Path) -> bool:
    try:
        return a.read_bytes() == b.read_bytes()
    except OSError:
        return False


def refresh_prims(primary: Path, worktree: Path) -> list[str]:
    """Mirror the primary's prims/ (+ README) and test_prims.cpp into `worktree`.

    The worktree copy is read-only for agents, so this also reverts any direct edit
    made outside the publish path.  Files are only rewritten when they differ (keeps
    mtimes stable for incremental builds).  Returns the changed relative paths.
    """
    primary, worktree = Path(primary).resolve(), Path(worktree).resolve()
    if primary == worktree:
        return []
    changed: list[str] = []
    src_dir, dst_dir = primary / PRIMS_REL, worktree / PRIMS_REL
    src_files = {p.relative_to(src_dir) for p in src_dir.rglob("*") if p.is_file()} if src_dir.is_dir() else set()
    for rel in sorted(src_files):
        dst = dst_dir / rel
        if not _same(src_dir / rel, dst):
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy(src_dir / rel, dst)
            changed.append(f"{PRIMS_REL}/{rel}")
    if dst_dir.is_dir():
        for p in sorted(dst_dir.rglob("*")):
            if p.is_file() and p.relative_to(dst_dir) not in src_files:
                p.unlink()
                changed.append(f"{PRIMS_REL}/{p.relative_to(dst_dir)} (removed)")
    src_t, dst_t = primary / TESTS_REL, worktree / TESTS_REL
    if src_t.is_file() and not _same(src_t, dst_t):
        dst_t.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy(src_t, dst_t)
        changed.append(TESTS_REL)
    return changed


def refresh_for_stage(repo_root: Path) -> list[str]:
    """Stage-start hook: refresh this worker's copy from the primary (no-op in a shared tree)."""
    try:
        return refresh_prims(primary_root(repo_root), Path(repo_root))
    except OSError:
        return []


# ---------------------------------------------------------------- vocabulary check

def _load_vocab_module(primary: Path):
    path = primary / "scripts" / "check_backend_query_free.py"
    if not path.is_file():
        return None
    spec = importlib.util.spec_from_file_location("_check_backend_query_free", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)  # type: ignore[union-attr]
    return mod


def vocabulary_hits(primary: Path, texts: dict[str, str]) -> list[dict[str, Any]]:
    mod = _load_vocab_module(primary)
    if mod is None:
        return []
    vocab = mod.workload_vocabulary(primary)
    hits = []
    for label, text in texts.items():
        for no, match, line in mod.scan_vocab_text(text, vocab):
            hits.append({"file": label, "line": no, "word": match, "text": line})
    return hits


# ---------------------------------------------------------------- checks

def _is_subsequence(old: list[str], new: list[str]) -> bool:
    it = iter(new)
    return all(any(line == cand for cand in it) for line in old)


def removed_lines_diff(old: str, new: str, limit: int = 40) -> list[str]:
    diff = difflib.unified_diff(old.splitlines(), new.splitlines(), "published", "candidate", lineterm="", n=0)
    return [d for d in diff][:limit]


def _added_lines(old: str, new: str) -> list[str]:
    sm = difflib.SequenceMatcher(None, old.splitlines(), new.splitlines(), autojunk=False)
    nl = new.splitlines()
    out: list[str] = []
    for tag, _i1, _i2, j1, j2 in sm.get_opcodes():
        if tag in ("insert", "replace"):
            out += nl[j1:j2]
    return out


def additions_only(old: str, new: str) -> bool:
    return _is_subsequence(old.splitlines(), new.splitlines())


def header_fields(text: str) -> dict[str, str]:
    """`// FIELD  value` lines of a header comment (continuation lines joined)."""
    fields: dict[str, str] = {}
    current = ""
    for line in text.splitlines()[:40]:
        if not line.lstrip().startswith("//"):
            continue
        body = line.lstrip()[2:].strip()
        name = next((f for f in HEADER_FIELDS if re.match(rf"{re.escape(f)}\b", body)), "")
        if name:
            current, body = name, body[len(name):].strip()
            fields[current] = body
        elif current and body:
            fields[current] += " " + body
    return fields


def check_header_comment(text: str) -> list[str]:
    fields = header_fields(text)
    return [f for f in HEADER_FIELDS if not fields.get(f)]


def _words(text: str) -> set[str]:
    return {w for w in re.findall(r"[a-z0-9]+", text.lower()) if len(w) > 2}


def _stem(header_name: str) -> str:
    return re.sub(r"[^a-z0-9]", "", Path(header_name).stem.lower())


def duplicate_of(primary: Path, name: str, purpose: str) -> str | None:
    """Name of a published header with the same name or a near-identical declared purpose."""
    mine = _words(purpose)
    for path in sorted((primary / PRIMS_REL).glob("*.hpp")) + sorted((primary / PRIMS_REL).glob("*.h")):
        if path.name == name:
            continue
        theirs = _words(header_fields(_read(path) or "").get("PURPOSE", ""))
        same_purpose = bool(mine and theirs) and \
            len(mine & theirs) / len(mine | theirs) >= DUPLICATE_PURPOSE_SIMILARITY
        if _stem(path.name) == _stem(name) or same_purpose:
            return path.name
    return None


def catalogue_row(name: str, header_text: str) -> str:
    fields = header_fields(header_text)
    cells = [f"`{name}`", *(fields[f].replace("|", "/") for f in CATALOGUE_FIELDS)]
    return "| " + " | ".join(cells) + " |"


def with_catalogue_rows(readme: str, rows: list[str]) -> str:
    """Append `rows` to the README's catalogue table (creating the table if absent)."""
    lines = readme.rstrip("\n").splitlines() if readme.strip() else []
    if CATALOGUE_HEADING not in lines:
        lines += ["", CATALOGUE_HEADING, "", "| " + " | ".join(CATALOGUE_COLUMNS) + " |",
                  "|" + " --- |" * len(CATALOGUE_COLUMNS)]
    at = lines.index(CATALOGUE_HEADING) + 1
    while at < len(lines) and not lines[at].startswith("|"):
        at += 1
    while at < len(lines) and lines[at].startswith("|"):
        at += 1
    lines[at:at] = rows
    return "\n".join(lines) + "\n"


def _merge3(candidate: str, base: str | None, primary: str) -> tuple[str | None, str]:
    """Rebase `candidate` (edited from `base`) onto a newer `primary` (git merge-file)."""
    if base is None or base == primary or candidate == primary:
        return candidate, ""
    with tempfile.TemporaryDirectory() as td:
        paths = []
        for name, text in (("cand", candidate), ("base", base), ("prim", primary)):
            p = Path(td) / name
            p.write_text(text, encoding="utf-8")
            paths.append(str(p))
        proc = subprocess.run(["git", "merge-file", "-p", "--union", *paths], capture_output=True, text=True)
    if proc.returncode != 0:
        return None, "conflicts with a primitive published since this worker's copy was refreshed"
    return proc.stdout, ""


def _read(path: Path) -> str | None:
    try:
        return path.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError):
        return None


def _fail(error: str, **extra: Any) -> dict[str, Any]:
    return {"ok": False, "error": error, **extra}


# ---------------------------------------------------------------- dependents

def _headers_including(primary: Path, changed: set[str]) -> tuple[set[Path], set[Path]]:
    """Source files (and headers, transitively) that include any of the changed prims headers."""
    inc = re.compile(r'#\s*include\s*[<"]([^>"]+)[>"]')
    files: list[Path] = []
    for sub in ("accelerators", "eval", "backends", "substrate"):
        base = primary / sub
        if base.is_dir():
            files += [p for p in base.rglob("*") if p.suffix in _SOURCE_SUFFIXES and p.is_file()]
    includes: dict[Path, list[str]] = {}
    for p in files:
        text = _read(p) or ""
        includes[p] = [m.group(1) for m in inc.finditer(text)]
    targets = {f"{INCLUDE_PREFIX}/{name}" for name in changed}  # prims/x.hpp
    hit_headers: set[Path] = set()
    hit_sources: set[Path] = set()
    frontier = set(targets)
    seen: set[str] = set()
    while frontier:
        cur = frontier.pop()
        seen.add(cur)
        for p, incs in includes.items():
            if any(i == cur or i.endswith("/" + cur) or cur.endswith("/" + i) for i in incs):
                if p.suffix in _HEADER_SUFFIXES:
                    if p not in hit_headers:
                        hit_headers.add(p)
                        try:
                            rel = str(p.relative_to(primary))  # suffix-matched against include forms
                        except ValueError:
                            rel = p.name
                        if rel not in seen:
                            frontier.add(rel)
                else:
                    hit_sources.add(p)
    return hit_headers, hit_sources


def _targets_for_sources(build_dir: Path, sources: set[Path]) -> set[str]:
    cc = build_dir / "compile_commands.json"
    data = json.loads(cc.read_text(encoding="utf-8"))
    wanted = {str(p.resolve()) for p in sources}
    out: set[str] = set()
    for entry in data:
        f = Path(entry.get("directory", "."), entry["file"]).resolve()
        if str(f) in wanted:
            m = re.search(r"CMakeFiles/([^/]+)\.dir/", entry.get("output", "") or "")
            if m:
                out.add(m.group(1))
    return out


def _rebuild_dependents(primary: Path, changed_existing: set[str], build_dir: Path, env: dict[str, str]) -> dict[str, Any] | None:
    """Build every target that includes a changed existing header and run its tests.

    Returns a failure dict, or None when everything built and passed.
    """
    _, sources = _headers_including(primary, changed_existing)
    sources.discard(primary / TESTS_REL)  # already built and run (standalone test_prims)
    if not sources:
        return None
    if not (build_dir / "compile_commands.json").is_file():
        rc, out = _run(["cmake", "-S", ".", "-B", str(build_dir), "-DCMAKE_BUILD_TYPE=Release",
                        "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"], cwd=primary, env=env, timeout=1800)
        if rc != 0 or not (build_dir / "compile_commands.json").is_file():
            return _fail("cannot configure the main build tree to rebuild dependents of a changed header; "
                         "nothing published", log_tail=tail_marked(out))
    targets = sorted(_targets_for_sources(build_dir, sources))
    if not targets:
        return None
    jobs = str(os.cpu_count() or 4)
    rc, out = _run(["cmake", "--build", str(build_dir), "--target", *targets, "-j", jobs, "--", "-k"],
                   cwd=primary, env=env, timeout=3600)
    if rc != 0:
        return _fail("a dependent no longer builds; nothing published", targets=targets, log_tail=tail_marked(out))
    rc, out = _run(["ctest", "--test-dir", str(build_dir), "--show-only=json-v1"], cwd=primary, env=env, timeout=300)
    names: list[str] = []
    if rc == 0:
        try:
            for t in json.loads(out[out.index("{"):]).get("tests", []):
                cmd = t.get("command") or []
                if cmd and Path(cmd[0]).name in targets:
                    names.append(t["name"])
        except (ValueError, KeyError):
            names = []
    if names:
        regex = "^(" + "|".join(re.escape(n) for n in names) + ")$"
        rc, out = _run(["ctest", "--test-dir", str(build_dir), "-R", regex, "--output-on-failure", "-j", "1"],
                       cwd=primary, env=env, timeout=3600)
        if rc != 0:
            return _fail("a dependent's unit test fails; nothing published", targets=targets, tests=names,
                         log_tail=tail_marked(out))
    return None


# ---------------------------------------------------------------- publish

def primitive_publish(
    repo_root: Path,
    files: list[str],
    rationale: str,
    query: int | str = "",
    *,
    worker: str = "",
    dependent_build_dir: Path | None = None,
) -> dict[str, Any]:
    """Publish staged primitives to the shared library (additions only, built and tested first)."""
    repo_root = Path(repo_root).resolve()
    primary = primary_root(repo_root)
    if not rationale or len(rationale.strip()) < 10:
        return _fail("rationale required: one line saying what the primitive is and why none existing fits")
    if not files:
        return _fail("no files given")
    rels: list[str] = []
    for f in files:
        rel = _norm_rel(f, repo_root)
        ok_header = rel.startswith(PRIMS_REL + "/") and rel.endswith(_HEADER_SUFFIXES) and "/" not in rel[len(PRIMS_REL) + 1:]
        if not (ok_header or rel == TESTS_REL):
            return _fail(f"not publishable: {f} (allowed: a new/extended header directly under {PRIMS_REL}/ and "
                         f"{TESTS_REL}; the {README_REL} catalogue row is added by primitive_publish)")
        if rel not in rels:
            rels.append(rel)

    lock = primary / LOCK_REL
    with file_lock(lock):
        # -- gather candidates (rebased onto what was published since this worker's refresh)
        final: dict[str, str] = {}
        old: dict[str, str | None] = {}
        for rel in rels:
            cand = _read(repo_root / STAGING_REL / rel)
            if cand is None:
                return _fail(f"staged file missing: {STAGING_REL}/{rel}")
            cur = _read(primary / rel)
            base = _read(repo_root / rel) if repo_root != primary else cur
            merged, err = _merge3(cand, base, cur) if cur is not None else (cand, "")
            if merged is None:
                return _fail(f"{rel}: {err}; re-copy the current file from {PRIMS_REL}/ into staging and re-apply your additions")
            old[rel] = cur
            final[rel] = merged
            if cur is not None and not additions_only(cur, merged):
                return _fail(f"{rel}: the library only grows: these lines were removed or changed "
                             "(add a new primitive or new member instead of editing existing lines)",
                             diff=removed_lines_diff(cur, merged))
        new_headers = [r for r in rels if r.endswith(_HEADER_SUFFIXES) and old[r] is None]
        changed_existing = {Path(r).name for r in rels if r.endswith(_HEADER_SUFFIXES) and old[r] is not None
                            and final[r] != old[r]}
        changed = [r for r in rels if final[r] != old[r]]
        if not changed:
            return {"ok": True, "note": "no change"}

        # -- structure: header comment, tests, no duplicate of a published primitive
        problems: list[str] = []
        tests = final.get(TESTS_REL, _read(primary / TESTS_REL) or "")
        for rel in new_headers:
            name = Path(rel).name
            missing = check_header_comment(final[rel])
            if missing:
                problems.append(f"{name}: header comment lacks {', '.join(missing)} (see any existing prims header)")
            twin = None if missing else duplicate_of(primary, name, header_fields(final[rel])["PURPOSE"])
            if twin:
                problems.append(f"{name}: duplicates the published {twin} (same name or purpose); reuse it, or "
                                "extend it by staging a version of it with added members")
            if TESTS_REL not in final or f"prims/{name}" not in tests:
                problems.append(f"{name}: {TESTS_REL} must include it and test it")
        if new_headers and TESTS_REL in final:
            added = _added_lines(old[TESTS_REL] or "", final[TESTS_REL])
            if len(added) < MIN_TEST_LINES:
                problems.append(f"{TESTS_REL}: add real tests (>= {MIN_TEST_LINES} new lines), incl. randomized "
                                "reference tests against a std:: model with copy/mutate/old-version checks")
            elif not _RANDOM_RE.search("\n".join(added)):
                problems.append(f"{TESTS_REL}: new tests must include a randomized reference test (std::mt19937)")
        if problems:
            return _fail("incomplete primitive", problems=problems)

        # -- workload-agnostic
        hits = vocabulary_hits(primary, {r: "\n".join(_added_lines(old[r] or "", final[r])) for r in changed})
        if hits:
            return _fail("workload vocabulary in the primitives library (names must say what a structure is, "
                         "not who uses it); remove these words, including from comments", hits=hits[:30])

        if new_headers:
            readme_old = _read(primary / README_REL)
            old[README_REL] = readme_old
            final[README_REL] = with_catalogue_rows(
                readme_old or "", [catalogue_row(Path(r).name, final[r]) for r in new_headers])
            changed.append(README_REL)

        # -- write with backup, build, test
        backup: dict[str, str | None] = {r: old[r] for r in changed}
        for r in changed:
            dst = primary / r
            dst.parent.mkdir(parents=True, exist_ok=True)
            dst.write_text(final[r], encoding="utf-8")

        def restore() -> None:
            for r, text in backup.items():
                dst = primary / r
                if text is None:
                    dst.unlink(missing_ok=True)
                else:
                    dst.write_text(text, encoding="utf-8")

        try:
            env = dict(os.environ)
            bdir = primary / BUILD_DIR_NAME
            rc, out = _run(["cmake", "-S", str(primary / CMAKE_SRC_REL), "-B", str(bdir),
                            f"-DPRIMS_REPO_ROOT={primary}", "-DCMAKE_BUILD_TYPE=Release"], cwd=primary, env=env)
            if rc == 0:
                rc, out = _run(["cmake", "--build", str(bdir), "--target", "test_prims", "-j",
                                str(os.cpu_count() or 4)], cwd=primary, env=env)
            if rc != 0:
                restore()
                return _fail("test_prims does not build; nothing published", log_tail=tail_marked(out))
            rc, out = _run([str(bdir / "test_prims")], cwd=primary, env=env, timeout=1800)
            if rc != 0:
                restore()
                return _fail("test_prims fails; nothing published", log_tail=tail_marked(out))
            if changed_existing:
                fail = _rebuild_dependents(primary, changed_existing,
                                           Path(dependent_build_dir) if dependent_build_dir else primary / "build-release", env)
                if fail:
                    restore()
                    return fail
        except BaseException:
            restore()
            raise

        # -- provenance, refresh, clear staging
        record = {"ts": time.time(), "query": query, "worker": worker, "files": changed,
                  "new_headers": [Path(r).name for r in new_headers], "changed_headers": sorted(changed_existing),
                  "rationale": rationale.strip(), "git_head": _git_head(primary)}
        log = primary / PROVENANCE_REL
        log.parent.mkdir(parents=True, exist_ok=True)
        with log.open("a", encoding="utf-8") as fh:
            fh.write(json.dumps(record) + "\n")
        for r in rels:
            (repo_root / STAGING_REL / r).unlink(missing_ok=True)
        refresh_prims(primary, repo_root)
    return {"ok": True, "published": changed, "new_headers": record["new_headers"],
            "changed_headers": record["changed_headers"],
            "note": "published; this worker's prims/ copy is refreshed. Include the header as "
                    "\"prims/<name>\""}


def publish_tool(pipeline_config: Any, query: int | str, files: list[str], rationale: str) -> str:
    """Tool entry point (pipeline tool policy and MCP server): JSON result string."""
    repo_root = Path(pipeline_config.repo_root)
    worker = str(getattr(pipeline_config, "worker_id", "") or "")
    primary = primary_root(repo_root)
    bdir = Path(getattr(pipeline_config, "build_dir", "") or "build-release")
    if not bdir.is_absolute():
        bdir = primary / bdir
    result = primitive_publish(repo_root, list(files or []), rationale, query, worker=worker,
                               dependent_build_dir=bdir)
    return json.dumps(result, indent=2)
