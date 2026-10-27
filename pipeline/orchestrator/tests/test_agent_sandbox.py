"""The agents' OS sandbox (shared builder, C++ mounts, Verus prefix) and the post-stage ownership check."""

from __future__ import annotations

import shutil
import subprocess
from pathlib import Path
from types import SimpleNamespace

import pytest

from accel_pipeline import layout, sandbox
from accel_pipeline.layout import StageWriteCheck, violation_message

PRIM_A, PRIM_B = f"{layout.PRIMS}/toy_a.h", f"{layout.PRIMS}/toy_b.h"
PROOF = f"{layout.PROOFLIB}/toy_lib.rs"
NEEDS: dict = {}  # engine name -> the backend `info` the sandbox would read
needs_bwrap = pytest.mark.skipif(not shutil.which("bwrap"), reason="bubblewrap not installed")


def _cfg(root: Path, engine: str = "toydb", **kw) -> SimpleNamespace:
    base = dict(repo_root=root, build_dir=root / "build-x", codex_home=None, engine_name=engine, datasets={},
                workload_kind="aux_state", benchmark_name="toybench", sandbox_extra_ro=[], sandbox_extra_rw=[],
                write_paths=["accelerators/*/q*/**", "pipeline/artifacts/**"], shared_paths=[])
    base.update(kw)
    return SimpleNamespace(**base)


def _tree(root: Path) -> Path:
    for d in ("substrate", "workloads/toybench", "accelerators/toybench/q6", "accelerators/toybench/q3", "backends/toydb",
              "backends/dockerdb", "pipeline"):
        (root / d).mkdir(parents=True, exist_ok=True)
    (root / "backends/toydb/host.sh").write_text("#!/usr/bin/env bash\n# embedded\n")
    (root / "backends/dockerdb/host.sh").write_text("#!/usr/bin/env bash\ndocker exec x run\n")
    for h in ("toydb", "dockerdb"):
        (root / f"backends/{h}/host.sh").chmod(0o755)
    return root


@pytest.fixture
def isolated(monkeypatch: pytest.MonkeyPatch, tmp_path: Path):
    sock = tmp_path / "docker.sock"
    sock.touch()
    monkeypatch.setattr(sandbox, "DOCKER_SOCKET", sock)
    for k in ("ACCEL_PIPELINE_PRIMARY_REPO", "ACCEL_ENGINE_STATE_DIR", "ACCEL_DATASET_DIR",
              "ACCEL_PIPELINE_PACK_QUERIES", "CCACHE_DIR"):
        monkeypatch.delenv(k, raising=False)
    NEEDS.clear()
    monkeypatch.setattr(sandbox, "backend_needs", lambda cfg: NEEDS.get(cfg.engine_name))
    return sock


def _dsts(binds, mode=None):
    return {dst for m, _, dst in binds if mode in (None, m)}


def test_cpp_mounts_tree_layers_build_data(tmp_path: Path, isolated: Path) -> None:
    root = _tree(tmp_path / "tree").resolve()
    data = tmp_path / "data"
    data.mkdir()
    (data / "toybench.toydb").touch()
    extra = tmp_path / "extra"
    extra.mkdir()
    cfg = _cfg(root, datasets={"toybench_sf1": data / "toybench.toydb"}, build_dir=tmp_path / "scratch" / "build",
               sandbox_extra_ro=[str(extra)])
    binds = sandbox.cpp_mounts(cfg)
    order = [(m, d) for m, _, d in binds]
    assert order.index(("rw", root)) < order.index(("ro", root / "substrate")) < len(order)
    assert ("ro", root / "workloads") in order
    assert ("rw", (tmp_path / "scratch" / "build").resolve()) in order  # build dir outside the tree
    assert ("ro", data.resolve()) in order and ("ro", extra.resolve()) in order
    assert not any(m == "rw" and d != root and not d.is_relative_to(tmp_path) for m, d in order
                   if d not in {Path.home() / ".cache" / "ccache"})  # nothing else writable
    # a build dir inside the tree adds no bind
    inside = sandbox.cpp_mounts(_cfg(root, build_dir=root / "build-y"))
    assert ("rw", root / "build-y") not in [(m, d) for m, _, d in inside]


def test_worktree_mode_mounts_primary_and_hides_other_workers(tmp_path: Path, isolated: Path,
                                                              monkeypatch: pytest.MonkeyPatch) -> None:
    primary = _tree(tmp_path / "primary").resolve()
    wt = primary / "pipeline" / "worktrees" / "w1"
    _tree(wt)
    monkeypatch.setenv("ACCEL_PIPELINE_PRIMARY_REPO", str(primary))
    cfg = _cfg(wt, build_dir=primary / "build-release-w1", codex_home=primary / "pipeline/.codex/workers/w1")
    order = [(m, d) for m, _, d in sandbox.cpp_mounts(cfg)]
    wts = primary / "pipeline" / "worktrees"
    assert order.index(("rw", primary)) < order.index(("tmpfs", wts)) < order.index(("rw", wt.resolve()))
    for tree in (primary, wt.resolve()):
        assert ("ro", tree / "substrate") in order and ("ro", tree / "workloads") in order
        assert order.index(("rw", tree)) < order.index(("ro", tree / "substrate"))
    # worker build dir and codex home live in the primary: already covered, no separate binds
    assert ("rw", primary / "build-release-w1") not in order


@needs_bwrap
def test_bwrap_cpp_sandbox_blocks_substrate_and_allows_the_query_dir(tmp_path: Path, isolated: Path,
                                                                     monkeypatch: pytest.MonkeyPatch) -> None:
    root = _tree(tmp_path / "tree").resolve()
    cfg = _cfg(root, benchmark_name="toybench")
    monkeypatch.setenv("ACCEL_PIPELINE_QUERY", "6")
    prefix = sandbox.cpp_prefix(cfg)
    script = ("touch substrate/x 2>/dev/null && echo SUB_WRITABLE; touch workloads/toybench/x 2>/dev/null && echo WL_WRITABLE; "
              "touch accelerators/toybench/q6/ok && echo QUERY_OK; touch build-x/ok && echo BUILD_OK; ls /home | wc -l; "
              "test -d /sys/devices/system/cpu && echo SYS_VISIBLE; touch /sys/x 2>/dev/null && echo SYS_WRITABLE")
    proc = subprocess.run(prefix + ["bash", "-c", script], capture_output=True, text=True, timeout=60)
    out = proc.stdout.split()
    assert "QUERY_OK" in out and "BUILD_OK" in out, proc.stderr
    assert "SUB_WRITABLE" not in out and "WL_WRITABLE" not in out
    assert "SYS_VISIBLE" in out and "SYS_WRITABLE" not in out  # without /sys, perf finds no PMU and every profile reads "not applicable"
    assert (root / "accelerators/toybench/q6/ok").exists() and not (root / "substrate/x").exists()
    check = sandbox.cpp_selfcheck(cfg, 6)
    assert check["problems"] in ([], ["MISSING codex"]), check
    assert check["notes"] and "no usable host.sh" in check["notes"][0]  # the fake engine has no info


# ---- the post-stage ownership check ----

def _git_tree(root: Path) -> Path:
    _tree(root)
    for f in ("substrate/s.h", "accelerators/toybench/q6/k.cpp", "accelerators/toybench/q3/k.cpp", "backends/toydb/e.cpp",
              PRIM_A, PROOF):
        (root / f).parent.mkdir(parents=True, exist_ok=True)
        (root / f).write_text("x\n")
    subprocess.run(["git", "init", "-q", str(root)], check=True)
    subprocess.run(["git", "-C", str(root), "add", "-A"], check=True)
    subprocess.run(["git", "-C", str(root), "-c", "user.email=a@b", "-c", "user.name=t", "commit", "-qm", "init"],
                   check=True)
    return root


def _violations(root: Path, write, **kw) -> dict[str, str]:
    check = StageWriteCheck(root, query=6, stage=kw.pop("stage", "implementation"),
                            pipeline_config=kw.pop("cfg", _cfg(root)))
    write(root)
    return dict(check.violations())


def test_ownership_allows_own_query_and_artifacts(tmp_path: Path) -> None:
    root = _git_tree(tmp_path / "t")

    def write(r: Path) -> None:
        (r / "accelerators/toybench/q6/k.cpp").write_text("changed\n")
        (r / "accelerators/toybench/q6/new.cpp").write_text("new\n")
        (r / "pipeline/artifacts/toybench/q6").mkdir(parents=True)
        (r / "pipeline/artifacts/toybench/q6/a.json").write_text("{}")

    assert _violations(root, write) == {}


def test_ownership_flags_foreign_layers(tmp_path: Path) -> None:
    root = _git_tree(tmp_path / "t")

    def write(r: Path) -> None:
        (r / "accelerators/toybench/q3/k.cpp").write_text("peer edit\n")
        (r / "backends/toydb/e.cpp").write_text("backend edit\n")
        (r / PRIM_A).write_text("prim edit\n")
        (r / PROOF).write_text("proof edit\n")
        (r / "substrate/s.h").write_text("substrate edit\n")
        (r / "accelerators/toybench/q6/k.cpp").write_text("mine\n")

    bad = _violations(root, write)
    assert set(bad) == {"accelerators/toybench/q3/k.cpp", "backends/toydb/e.cpp", PRIM_A,
                        PROOF, "substrate/s.h"}
    assert "cross-query" in bad["accelerators/toybench/q3/k.cpp"] and "prooflib_publish" in bad[PROOF]
    assert "never writable" in bad["substrate/s.h"] or "SUBSTRATE" in bad["substrate/s.h"].upper()
    msg = violation_message("implementation", list(bad.items()))
    assert "undo" in msg and "backends/toydb/e.cpp" in msg


def test_ownership_ignores_what_was_dirty_before_the_stage(tmp_path: Path) -> None:
    root = _git_tree(tmp_path / "t")
    (root / "accelerators/toybench/q3/k.cpp").write_text("dirty before\n")
    assert _violations(root, lambda r: None) == {}


def test_ownership_exempts_publish_tool_writes(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    import json
    import time

    root = _git_tree(tmp_path / "t")

    def write(r: Path) -> None:
        (r / PRIM_A).write_text("published\n")
        (r / PRIM_B).write_text("not published\n")
        (r / "pipeline/prims_provenance.jsonl").write_text(
            json.dumps({"ts": time.time() + 1, "files": [PRIM_A]}) + "\n")

    assert set(_violations(root, write)) == {PRIM_B}


def test_ownership_engine_backend_run_owns_its_backend(tmp_path: Path) -> None:
    root = _git_tree(tmp_path / "t")
    cfg = _cfg(root, workload_kind="engine_backend")

    def write(r: Path) -> None:
        (r / "backends/toydb/e.cpp").write_text("mine\n")
        (r / "backends/dockerdb/x.cpp").write_text("other engine\n")

    assert set(_violations(root, write, cfg=cfg, stage="impl_runtime")) == {"backends/dockerdb/x.cpp"}


def test_ownership_shared_tree_pack_skips_other_queries(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    root = _git_tree(tmp_path / "t").resolve()
    monkeypatch.setenv("ACCEL_PIPELINE_PACK_QUERIES", "3,6")
    monkeypatch.setenv("ACCEL_PIPELINE_PRIMARY_REPO", str(root))

    def write(r: Path) -> None:
        (r / "accelerators/toybench/q3/k.cpp").write_text("another worker's edit\n")
        (r / "backends/toydb/e.cpp").write_text("backend edit\n")

    assert set(_violations(root, write)) == {"backends/toydb/e.cpp"}


def _backend(root: Path, policy: str, monkeypatch: pytest.MonkeyPatch):
    from accel_pipeline.backends.base import StageRunRequest, StageRunResult
    from accel_pipeline.backends.codex_exec import CodexExecBackend

    cfg = _cfg(root, write_policy=policy, repo_root=root)
    backend = CodexExecBackend(pipeline_config=cfg, model_config=SimpleNamespace(), query=6, stage="implementation")

    def fake(self, request):
        (root / "accelerators/toybench/q3/k.cpp").write_text("peer edit\n")
        return StageRunResult(backend="codex_exec", stage=request.stage, status="finished")

    monkeypatch.setattr(CodexExecBackend, "_run_codex", fake)
    monkeypatch.setattr(CodexExecBackend, "_log_violations", lambda self, policy, bad: None)
    return backend, StageRunRequest(stage="implementation", instructions="", user_message="", agent_name="a",
                                    model_id="m")


@pytest.mark.parametrize("policy,status", [("enforce", "error"), ("warn", "finished"), ("off", "finished")])
def test_write_policy_enforce_warn_off(tmp_path: Path, monkeypatch: pytest.MonkeyPatch, policy: str, status: str) -> None:
    root = _git_tree(tmp_path / "t")
    backend, request = _backend(root, policy, monkeypatch)
    result = backend.run_stage(request)
    assert result.status == status
    if policy == "enforce":
        assert "accelerators/toybench/q3/k.cpp" in result.error and "cross-query" in result.error
        assert "put back" in result.error
    assert bool(result.metadata.get("write_violations")) == (policy != "off")
    # enforce reverts the out-of-bounds edit; warn and off leave it
    assert ((root / "accelerators/toybench/q3/k.cpp").read_text() == "x\n") == (policy == "enforce")


def test_a_revert_restores_each_path_as_the_stage_found_it(tmp_path: Path) -> None:
    root = _git_tree(tmp_path / "t")
    (root / "backends/toydb/e.cpp").write_text("dirty before the stage\n")  # an earlier stage's change stays
    check = StageWriteCheck(root, query=6, stage="implementation", pipeline_config=_cfg(root))
    (root / "backends/toydb/e.cpp").write_text("this stage's edit\n")
    (root / "substrate/s.h").write_text("this stage's edit\n")
    (root / "eval").mkdir()
    (root / "eval/moved.cpp").write_text("relocated code\n")
    (root / "accelerators/toybench/q6/k.cpp").write_text("mine\n")
    bad = check.violations()
    assert {rel for rel, _ in bad} == {"backends/toydb/e.cpp", "substrate/s.h", "eval/moved.cpp"}
    check.revert(bad)
    assert (root / "backends/toydb/e.cpp").read_text() == "dirty before the stage\n"
    assert (root / "substrate/s.h").read_text() == "x\n" and not (root / "eval/moved.cpp").exists()
    assert (root / "accelerators/toybench/q6/k.cpp").read_text() == "mine\n" and check.violations() == []


def test_no_check_runs_while_the_session_has_out_of_bounds_edits(tmp_path: Path) -> None:
    import json

    from accel_pipeline.tools.gate_tools import ToolContext, admission_refusal_payload

    root = _git_tree(tmp_path / "t")
    cfg = _cfg(root, quick_tier=SimpleNamespace(enabled=False))
    ctx = ToolContext(cfg, 6, workload={}, stage="host_bootstrap",
                      writes=StageWriteCheck(root, query=6, stage="host_bootstrap", pipeline_config=cfg))
    assert admission_refusal_payload(ctx, "quick_check") == ""
    (root / "eval").mkdir()
    (root / "eval/sweep_session.cpp").write_text("moved out of the scanned harness dir\n")
    refusal = json.loads(admission_refusal_payload(ctx, "host_bootstrap"))
    assert refusal["status"] == "refused" and "eval/sweep_session.cpp" in refusal["reason"]


def test_engine_state_and_build_dirs_in_the_tree_are_not_agent_writes(tmp_path: Path, monkeypatch) -> None:
    root = _git_tree(tmp_path / "t")
    monkeypatch.setenv("ACCEL_ENGINE_STATE_DIR", str(root / ".engine_state"))

    def write(r: Path) -> None:
        (r / ".engine_state/build_stamps").mkdir(parents=True)
        (r / ".engine_state/build_stamps/abc").write_text("stamp")

    assert _violations(root, write) == {}


def test_dataset_inside_a_writable_bind_is_rebound_read_only(tmp_path: Path, isolated: Path) -> None:
    root = _tree(tmp_path / "tree").resolve()
    (root / "data").mkdir()
    dataset = root / "data" / "toybench.toydb"
    dataset.touch()
    binds = sandbox.cpp_mounts(_cfg(root, datasets={"toybench_sf1": dataset}))
    order = [(m, d) for m, _, d in binds]
    assert ("ro", dataset) in order and order.index(("rw", root)) < order.index(("ro", dataset))
