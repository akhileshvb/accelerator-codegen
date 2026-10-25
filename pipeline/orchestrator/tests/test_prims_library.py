"""primitive_publish / refresh_prims / write policy for the shared primitives library."""

from __future__ import annotations

import json
import shutil
import subprocess
from pathlib import Path

import pytest

from accel_pipeline import prims_library as pl
from accel_pipeline.write_ownership import write_allowed

REAL_ROOT = Path(__file__).resolve().parents[3]
P = pl.PRIMS_REL

EXISTING = """\
#pragma once
//
// Existing<T>
// PURPOSE    A stand-in primitive.
// KEY SPACE  none.
// COMPLEXITY O(1).
// MEMORY     8 B.
// COW/THREADS Not COW.
// USE WHEN   never.
//
namespace accelerator_codegen::aux_state::core { struct Existing {}; }
"""

README = (
    "# Primitives\n\n" + pl.CATALOGUE_HEADING + "\n\n"
    "| " + " | ".join(pl.CATALOGUE_COLUMNS) + " |\n"
    "|" + " --- |" * len(pl.CATALOGUE_COLUMNS) + "\n"
    "| `existing.hpp` | A stand-in primitive. | none. | 8 B. | Not COW. | never. |\n"
    "\n### Which one to use\n\nNothing.\n"
)
ROW_RING = ("| `ring_buffer.hpp` | Fixed-capacity ring buffer. | dense positions [0, capacity). "
            "| sizeof(T) per slot. | Not COW; single owner. | a bounded FIFO is needed. |")

TESTS = """\
#include "prims/existing.hpp"
static void TestExisting() {}
int main() {
  TestExisting();
  return 0;
}
"""


def new_header(name: str = "ring_buffer", extra_comment: str = "") -> str:
    return f"""\
#pragma once
//
// RingBuffer<T>
// PURPOSE    Fixed-capacity ring buffer.{extra_comment}
// KEY SPACE  dense positions [0, capacity).
// COMPLEXITY O(1) push and pop.
// MEMORY     sizeof(T) per slot.
// COW/THREADS Not COW; single owner.
// USE WHEN   a bounded FIFO is needed.
//
namespace accelerator_codegen::aux_state::core {{ struct {name} {{}}; }}
"""


NEW_README = README.replace("\n### Which one", ROW_RING + "\n\n### Which one", 1).replace("|\n\n" + ROW_RING, "|\n" + ROW_RING)

NEW_TESTS = (
    '#include "prims/existing.hpp"\n'
    '#include "prims/ring_buffer.hpp"\n'
    "static void TestExisting() {}\n"
    "static void TestRingBuffer() {\n"
    "  std::mt19937 rng(1);\n"
    + "".join(f"  CHECK(rng() >= 0); // reference model step {i}\n" for i in range(20))
    + "}\n"
    "int main() {\n  TestExisting();\n  TestRingBuffer();\n  return 0;\n}\n"
)


class FakeRun:
    """Stands in for pl._run: real git, scripted everything else."""

    def __init__(self) -> None:
        self.calls: list[list[str]] = []
        self.fail_on: str | None = None

    def __call__(self, cmd, *, cwd, env=None, timeout=1800):
        if cmd[0] == "git":
            p = subprocess.run(cmd, cwd=str(cwd), capture_output=True, text=True)
            return p.returncode, (p.stdout or "") + (p.stderr or "")
        self.calls.append([str(c) for c in cmd])
        if self.fail_on and any(self.fail_on in str(c) for c in cmd):
            return 1, f"error: simulated failure of {self.fail_on}\n" * 3
        return 0, "ok"


@pytest.fixture()
def fake(monkeypatch):
    fr = FakeRun()
    monkeypatch.setattr(pl, "_run", fr)
    return fr


@pytest.fixture()
def primary(tmp_path):
    root = tmp_path / "primary"
    (root / P).mkdir(parents=True)
    (root / P / "existing.hpp").write_text(EXISTING)
    (root / P / "README.md").write_text(README)
    (root / pl.TESTS_REL).parent.mkdir(parents=True)
    (root / pl.TESTS_REL).write_text(TESTS)
    (root / "scripts").mkdir()
    shutil.copy(REAL_ROOT / "scripts/check_backend_query_free.py", root / "scripts")
    for rel in ("scripts/postgres_tpch_schema.sql", "scripts/mysql_tpch_schema.sql",
                "workloads/tpch/schema.sql", "workloads/sec_edgar/schema.sql"):
        (root / rel).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy(REAL_ROOT / rel, root / rel)
    for cmd in (["init", "-q"], ["add", "-A"], ["-c", "user.name=t", "-c", "user.email=t@t", "commit", "-qm", "init"]):
        subprocess.run(["git", *cmd], cwd=root, check=True, capture_output=True)
    return root


def stage(root: Path, rel: str, text: str) -> None:
    p = root / pl.STAGING_REL / rel
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(text)


def stage_new_primitive(root: Path, *, header=None, tests=NEW_TESTS):
    stage(root, f"{P}/ring_buffer.hpp", header if header is not None else new_header())
    if tests is not None:
        stage(root, pl.TESTS_REL, tests)
    return [f"{P}/ring_buffer.hpp"] + ([pl.TESTS_REL] if tests is not None else [])


RATIONALE = "bounded FIFO; nothing existing is a ring"


def publish(root, files, **kw):
    return pl.primitive_publish(root, files, kw.pop("rationale", RATIONALE), kw.pop("query", 7), **kw)


def test_accepts_new_header_with_readme_and_tests(primary, fake):
    files = stage_new_primitive(primary)
    res = publish(primary, files)
    assert res["ok"], res
    assert res["new_headers"] == ["ring_buffer.hpp"]
    assert (primary / P / "ring_buffer.hpp").read_text() == new_header()
    assert (primary / pl.README_REL).read_text() == NEW_README
    assert (primary / pl.TESTS_REL).read_text() == NEW_TESTS
    cmds = [" ".join(c) for c in fake.calls]
    assert any("--target test_prims" in c for c in cmds)
    assert any(c.endswith("build-prims/test_prims") for c in cmds)
    # staged files are consumed
    assert not (primary / pl.STAGING_REL / P / "ring_buffer.hpp").exists()


def test_provenance_appended(primary, fake):
    res = publish(primary, stage_new_primitive(primary), query=21)
    assert res["ok"], res
    rec = json.loads((primary / pl.PROVENANCE_REL).read_text().splitlines()[-1])
    assert rec["query"] == 21
    assert rec["rationale"] == RATIONALE
    assert rec["new_headers"] == ["ring_buffer.hpp"]
    assert len(rec["git_head"]) == 40 and rec["ts"] > 0
    assert f"{P}/ring_buffer.hpp" in rec["files"]


def test_rejects_changed_existing_line(primary, fake):
    stage(primary, f"{P}/existing.hpp", EXISTING.replace("// MEMORY     8 B.", "// MEMORY     16 B."))
    res = publish(primary, [f"{P}/existing.hpp"])
    assert not res["ok"] and "only grows" in res["error"]
    assert any("MEMORY     8 B." in d for d in res["diff"])
    assert (primary / P / "existing.hpp").read_text() == EXISTING
    assert fake.calls == []


def test_existing_header_may_gain_lines(primary, fake):
    stage(primary, f"{P}/existing.hpp", EXISTING + "namespace accelerator_codegen::aux_state::core { inline int Extra() { return 1; } }\n")
    res = publish(primary, [f"{P}/existing.hpp"])
    assert res["ok"], res
    assert res["changed_headers"] == ["existing.hpp"]
    assert "Extra" in (primary / P / "existing.hpp").read_text()


def test_changed_existing_header_rebuilds_dependents(primary, fake):
    src = primary / "accelerators/some/q1/duckdb/dep.cpp"
    src.parent.mkdir(parents=True)
    src.write_text('#include "prims/existing.hpp"\nint x;\n')
    build = primary / "build-release"
    build.mkdir()
    (build / "compile_commands.json").write_text(json.dumps([
        {"directory": str(build), "file": str(src), "output": "CMakeFiles/dep_target.dir/src/dep.cpp.o", "command": "c++"}]))
    stage(primary, f"{P}/existing.hpp", EXISTING + "// more\n")
    res = publish(primary, [f"{P}/existing.hpp"])
    assert res["ok"], res
    assert any("--target" in c and "dep_target" in c for c in (" ".join(x) for x in fake.calls))


def test_dependent_build_failure_restores(primary, fake):
    src = primary / "accelerators/some/q1/duckdb/dep.cpp"
    src.parent.mkdir(parents=True)
    src.write_text('#include "prims/existing.hpp"\n')
    build = primary / "build-release"
    build.mkdir()
    (build / "compile_commands.json").write_text(json.dumps([
        {"directory": str(build), "file": str(src), "output": "CMakeFiles/dep_target.dir/src/dep.cpp.o"}]))
    fake.fail_on = "dep_target"
    stage(primary, f"{P}/existing.hpp", EXISTING + "// more\n")
    res = publish(primary, [f"{P}/existing.hpp"])
    assert not res["ok"] and "dependent" in res["error"]
    assert (primary / P / "existing.hpp").read_text() == EXISTING
    assert not (primary / pl.PROVENANCE_REL).exists()


def test_rejects_missing_header_comment(primary, fake):
    bad = new_header().replace("// USE WHEN   a bounded FIFO is needed.\n", "")
    res = publish(primary, stage_new_primitive(primary, header=bad))
    assert not res["ok"]
    assert any("USE WHEN" in p for p in res["problems"])
    assert not (primary / P / "ring_buffer.hpp").exists()


def test_catalogue_row_is_appended_by_publish(primary, fake):
    res = publish(primary, stage_new_primitive(primary))
    assert res["ok"], res
    lines = (primary / pl.README_REL).read_text().splitlines()
    assert lines.count(ROW_RING) == 1
    assert lines.index(ROW_RING) == lines.index(next(l for l in lines if "`existing.hpp`" in l)) + 1


def test_readme_cannot_be_staged(primary, fake):
    stage(primary, pl.README_REL, NEW_README)
    res = publish(primary, [f"{P}/ring_buffer.hpp", pl.README_REL])
    assert not res["ok"] and "not publishable" in res["error"]


def test_publish_into_empty_skeleton(primary, fake):
    for h in (primary / P).glob("*.hpp"):
        h.unlink()
    (primary / pl.README_REL).write_text("# Primitives\n\n" + pl.CATALOGUE_HEADING + "\n\n| "
                                         + " | ".join(pl.CATALOGUE_COLUMNS) + " |\n|" + " --- |" * 6 + "\n")
    (primary / pl.TESTS_REL).write_text("int main() {\n  return 0;\n}\n")
    tests = NEW_TESTS.replace('#include "prims/existing.hpp"\n', "").replace("  TestExisting();\n", "") \
        .replace("static void TestExisting() {}\n", "")
    res = publish(primary, stage_new_primitive(primary, tests=tests))
    assert res["ok"], res
    readme = (primary / pl.README_REL).read_text()
    assert readme.splitlines()[-1] == ROW_RING and readme.count("| Header |") == 1


@pytest.mark.parametrize("name,purpose", [
    ("ring_buffer2", "Something else entirely"),
    ("fifo", "Fixed-capacity ring buffer."),
    ("fifo", "A ring buffer with fixed capacity."),
])
def test_rejects_duplicate_of_published_primitive(primary, fake, name, purpose):
    assert publish(primary, stage_new_primitive(primary))["ok"]
    header = new_header().replace("Fixed-capacity ring buffer.", purpose)
    stage(primary, f"{P}/{name}.hpp", header.replace("RingBuffer", "Other"))
    stage(primary, pl.TESTS_REL, NEW_TESTS + f'// uses "prims/{name}.hpp"\n' + "".join(
        f"// mt19937 line {i}\n" for i in range(20)))
    res = publish(primary, [f"{P}/{name}.hpp", pl.TESTS_REL])
    if name == "ring_buffer2":
        assert res["ok"], res  # distinct name and purpose are accepted
        return
    assert not res["ok"] and any("duplicates the published ring_buffer.hpp" in p for p in res["problems"])
    assert not (primary / P / f"{name}.hpp").exists()


def test_rejects_missing_key_space_or_purpose(primary, fake):
    bad = new_header().replace("// KEY SPACE  dense positions [0, capacity).\n", "")
    res = publish(primary, stage_new_primitive(primary, header=bad))
    assert not res["ok"] and any("KEY SPACE" in p for p in res["problems"])


def test_rejects_missing_tests(primary, fake):
    res = publish(primary, stage_new_primitive(primary, tests=None))
    assert not res["ok"]
    assert any("test_prims.cpp" in p for p in res["problems"])


def test_rejects_trivial_or_non_random_tests(primary, fake):
    thin = TESTS.replace('existing.hpp"', 'existing.hpp"\n#include "prims/ring_buffer.hpp"')
    res = publish(primary, stage_new_primitive(primary, tests=thin))
    assert not res["ok"] and any("add real tests" in p for p in res["problems"])
    nonrandom = NEW_TESTS.replace("std::mt19937 rng(1);", "int seed = 1;").replace("rng() >= 0", "seed >= 0") \
        .replace("random", "x")
    res = publish(primary, stage_new_primitive(primary, tests=nonrandom))
    assert not res["ok"] and any("randomized" in p for p in res["problems"])


@pytest.mark.parametrize("word", ["lineitem", "l_orderkey", "TPCH", "sec_edgar", "adsh"])
def test_rejects_workload_vocabulary(primary, fake, word):
    header = new_header(extra_comment=f" Used by {word}.")
    res = publish(primary, stage_new_primitive(primary, header=header))
    assert not res["ok"] and "workload vocabulary" in res["error"]
    assert res["hits"] and not (primary / P / "ring_buffer.hpp").exists()


def test_generic_schema_words_are_allowed(primary, fake):
    header = new_header(extra_comment=" Keyed by name, date and value; count and size are O(1).")
    res = publish(primary, stage_new_primitive(primary, header=header))
    assert res["ok"], res


def test_build_failure_restores_backup(primary, fake):
    fake.fail_on = "--target"
    res = publish(primary, stage_new_primitive(primary))
    assert not res["ok"] and "does not build" in res["error"]
    assert "simulated failure" in res["log_tail"]
    assert not (primary / P / "ring_buffer.hpp").exists()
    assert (primary / pl.README_REL).read_text() == README
    assert (primary / pl.TESTS_REL).read_text() == TESTS
    assert not (primary / pl.PROVENANCE_REL).exists()


def test_test_failure_restores_backup(primary, fake):
    fake.fail_on = "test_prims"
    # configure/build commands contain test_prims too; only the run command is the bare binary
    fake.fail_on = "build-prims/test_prims"
    res = publish(primary, stage_new_primitive(primary))
    assert not res["ok"] and "fails" in res["error"]
    assert not (primary / P / "ring_buffer.hpp").exists()
    assert (primary / pl.README_REL).read_text() == README


def test_rejects_unpublishable_paths(primary, fake):
    res = publish(primary, ["accelerators/tpch/q1/duckdb/aux/module.cpp"])
    assert not res["ok"] and "not publishable" in res["error"]
    res = publish(primary, [f"{P}/sub/dir.hpp"])
    assert not res["ok"]


def test_requires_rationale_and_staged_file(primary, fake):
    assert not publish(primary, [f"{P}/ring_buffer.hpp"], rationale="")["ok"]
    res = publish(primary, [f"{P}/ring_buffer.hpp"])
    assert not res["ok"] and "staged file missing" in res["error"]


def test_concurrent_publish_is_rebased(primary, fake, tmp_path, monkeypatch):
    """A worker whose copy is behind (another primitive landed) still publishes cleanly."""
    worker = tmp_path / "worker"
    shutil.copytree(primary, worker, ignore=shutil.ignore_patterns(".git"))
    files = stage_new_primitive(worker)  # staged against the OLD tests file
    (primary / P / "other.hpp").write_text(new_header().replace("RingBuffer", "Other")
                                           .replace("Fixed-capacity ring buffer.", "Unrelated skip list."))
    (primary / pl.TESTS_REL).write_text(TESTS + "// other tests\n")
    monkeypatch.setenv("ACCEL_PIPELINE_PRIMARY_REPO", str(primary))
    res = pl.primitive_publish(worker, files, RATIONALE, 3)
    assert res["ok"], res
    assert ROW_RING in (primary / pl.README_REL).read_text()
    assert "// other tests" in (primary / pl.TESTS_REL).read_text()
    assert (worker / P / "other.hpp").is_file() and (worker / P / "ring_buffer.hpp").is_file()


def test_concurrent_publishers_serialise_on_readme_and_tests(primary, fake, tmp_path, monkeypatch):
    import threading
    monkeypatch.setenv("ACCEL_PIPELINE_PRIMARY_REPO", str(primary))
    names = ["alpha_set", "beta_tree", "gamma_heap", "delta_ring"]
    purposes = ["Ordered integer set.", "Balanced search tree.", "Binary min heap.", "Circular byte queue."]
    workers = []
    for name, purpose in zip(names, purposes):
        wt = tmp_path / f"w_{name}"
        shutil.copytree(primary, wt, ignore=shutil.ignore_patterns(".git"))
        header = new_header().replace("Fixed-capacity ring buffer.", purpose).replace("RingBuffer", name)
        tests = NEW_TESTS.replace("ring_buffer", name).replace("step", name)
        stage(wt, f"{P}/{name}.hpp", header)
        stage(wt, pl.TESTS_REL, tests)
        workers.append((wt, [f"{P}/{name}.hpp", pl.TESTS_REL]))
    results = [None] * len(workers)

    def run(i):
        results[i] = pl.primitive_publish(workers[i][0], workers[i][1], RATIONALE, i)

    threads = [threading.Thread(target=run, args=(i,)) for i in range(len(workers))]
    [t.start() for t in threads]
    [t.join() for t in threads]
    assert all(r and r["ok"] for r in results), results
    readme = (primary / pl.README_REL).read_text()
    tests = (primary / pl.TESTS_REL).read_text()
    for name in names:
        assert readme.count(f"| `{name}.hpp` |") == 1
        assert f'prims/{name}.hpp' in tests
    assert len((primary / pl.PROVENANCE_REL).read_text().splitlines()) == len(names)


def test_refresh_prims_mirrors_primary(primary, tmp_path):
    wt = tmp_path / "wt"
    (wt / P).mkdir(parents=True)
    (wt / P / "stale.hpp").write_text("// stale\n")
    (wt / P / "existing.hpp").write_text("// edited directly\n")
    changed = pl.refresh_prims(primary, wt)
    assert (wt / P / "existing.hpp").read_text() == EXISTING
    assert not (wt / P / "stale.hpp").exists()
    assert (wt / pl.TESTS_REL).read_text() == TESTS
    assert (wt / pl.README_REL).read_text() == README
    assert changed
    assert pl.refresh_prims(primary, wt) == []  # idempotent, mtimes untouched
    assert pl.refresh_prims(primary, primary) == []


def test_sync_owned_never_copies_prims(primary, tmp_path):
    from accel_pipeline.pack.worker_worktree import sync_owned_to_primary

    wt = tmp_path / "wt"
    (wt / P).mkdir(parents=True)
    (wt / P / "evil.hpp").write_text("// evil\n")
    (wt / "accelerators/tpch/q5/duckdb/aux").mkdir(parents=True)
    (wt / "accelerators/tpch/q5/duckdb/aux/module.cpp").write_text("// q5\n")
    copied = sync_owned_to_primary(primary=primary, worktree=wt, query=5)
    assert "accelerators/tpch/q5/duckdb/aux/module.cpp" in copied
    assert not any(c.startswith(P) for c in copied)
    assert not (primary / P / "evil.hpp").exists()


@pytest.mark.parametrize("rel", [f"{P}/new.hpp", f"{P}/README.md", f"{P}/existing.hpp", pl.TESTS_REL])
@pytest.mark.parametrize("stage_name", ["implementation", "optimization_worker", "optimization_unified_patch", ""])
def test_write_policy_denies_direct_prims_writes(primary, rel, stage_name):
    ok, reason = write_allowed(primary, primary / rel, query=5, stage=stage_name,
                               write_paths=[f"{P}/**", "accelerators/**", "**"])
    assert not ok
    assert "primitive_publish" in reason


def test_write_policy_allows_staging(primary):
    ok, _ = write_allowed(primary, primary / pl.STAGING_REL / P / "new.hpp", query=5, stage="implementation",
                          write_paths=[])
    assert ok


def test_prims_are_shared_lease_paths(primary):
    from accel_pipeline.config import DEFAULT_SHARED_PATH_GLOBS
    from accel_pipeline.write_ownership import is_shared_path

    assert is_shared_path(primary, primary / P / "existing.hpp")
    assert is_shared_path(primary, primary / pl.TESTS_REL)
    assert f"{P}/**" in DEFAULT_SHARED_PATH_GLOBS


def test_real_prims_are_workload_agnostic():
    r = subprocess.run(["python3", str(REAL_ROOT / "scripts/check_backend_query_free.py"), "--prims-only"],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stdout


def test_rebuilding_dependents_keeps_going_with_a_numeric_k(tmp_path, monkeypatch):
    from accel_pipeline import prims_library as lib

    (tmp_path / "build").mkdir()
    (tmp_path / "build" / "compile_commands.json").write_text("[]")
    calls = []
    monkeypatch.setattr(lib, "_headers_including", lambda primary, changed: (set(), {primary / "a.cpp"}))
    monkeypatch.setattr(lib, "_targets_for_sources", lambda build_dir, sources: {"dep"})
    monkeypatch.setattr(lib, "_run", lambda cmd, **kw: (calls.append(cmd), (1, "boom"))[1])
    result = lib._rebuild_dependents(tmp_path, {"x.hpp"}, tmp_path / "build", {})
    assert result and calls[0][-3:] == ["--", "-k", "0"]
