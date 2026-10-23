"""The agents' OS sandbox (shared builder, C++ mounts, Verus prefix) and the post-stage ownership check."""

from __future__ import annotations

import os
import shutil
import subprocess
from pathlib import Path
from types import SimpleNamespace

import pytest

from accel_pipeline import layout, sandbox
from accel_pipeline.config import PipelineConfig
from accel_pipeline.write_ownership import StageWriteCheck, violation_message

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


def test_backend_info_drives_docker_socket_and_mounts(tmp_path: Path, isolated: Path) -> None:
    root = _tree(tmp_path / "tree").resolve()
    sdk = tmp_path / "sdk"
    cache = tmp_path / "cache"
    sdk.mkdir()
    cache.mkdir()
    NEEDS["toydb"] = {"mounts": {"ro": [str(sdk)], "rw": [str(cache)]}, "docker": False}
    NEEDS["dockerdb"] = {"mounts": {"ro": [], "rw": []}, "docker": True}
    plain = sandbox.cpp_mounts(_cfg(root, "toydb"))
    assert isolated.resolve() not in _dsts(plain)
    assert ("ro", sdk.resolve()) in [(m, d) for m, _, d in plain] and ("rw", cache.resolve()) in [(m, d) for m, _, d in plain]
    assert isolated.resolve() in _dsts(sandbox.cpp_mounts(_cfg(root, "dockerdb")), "rw")
    assert isolated.resolve() not in _dsts(sandbox.cpp_mounts(_cfg(root, "")))  # no info: nothing extra


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


def test_pipeline_config_carries_sandbox_keys() -> None:
    from accel_pipeline.config import load_pipeline_config

    repo = Path(__file__).resolve().parents[3]
    cfg = load_pipeline_config(repo_root=repo)
    assert (cfg.sandbox, cfg.write_policy) == ("bwrap", "warn")
    assert PipelineConfig.__dataclass_fields__["sandbox_extra_rw"].default_factory() == []


def test_verus_prefix_unchanged_by_the_shared_builder(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    """The Verus bwrap command line after the system mounts: the same binds, in the same order, as before
    the builder was shared."""
    from accel_pipeline.verus import actions, sandbox as vsb
    from test_verus_pipeline import _ctx

    home = tmp_path / "home"
    for d in (".cargo", ".rustup"):
        (home / d).mkdir(parents=True)
    (tmp_path / "bin").mkdir()
    codex = tmp_path / "pkg" / "lib" / "codex.js"
    codex.parent.mkdir(parents=True)
    codex.touch()
    codex.chmod(0o755)
    (tmp_path / "bin" / "codex").symlink_to(codex)
    verus = tmp_path / "verus"
    verus.mkdir()
    monkeypatch.setenv("HOME", str(home))
    monkeypatch.setenv("PATH", f"{tmp_path / 'bin'}:{os.environ['PATH']}")
    for k in [k for k in os.environ if k.startswith("ACCEL_") and k.endswith("_ROOT")]:  # the sibling checkout is not part of this fixture
        monkeypatch.delenv(k)
    monkeypatch.setenv("VERUS_BIN", str(verus / "verus"))
    monkeypatch.delenv("CARGO_TARGET_DIR", raising=False)
    for mod in (sandbox, vsb):
        monkeypatch.setattr(mod, "HOME", home)
    ctx = _ctx(tmp_path)
    ctx.root.mkdir(parents=True, exist_ok=True)
    root = ctx.root.resolve()
    args = vsb.prefix(ctx, ctx.ws)
    tail = args[args.index("--dir") + 2:]
    target = actions.cargo_target_dir(ctx)
    pkg = codex.parent.parent
    expected = ["--bind", str(root), str(root),
                "--ro-bind", str(verus), str(verus), "--ro-bind", str(home / ".rustup"), str(home / ".rustup"),
                "--bind", str(home / ".cargo"), str(home / ".cargo"), "--bind", str(target), str(target),
                "--symlink", str(codex), str(tmp_path / "bin" / "codex"), "--ro-bind", str(pkg), str(pkg),
                "--chdir", str(ctx.ws)]
    assert tail == expected
    assert args[1:4] == ["--unshare-all", "--share-net", "--die-with-parent"]


@needs_bwrap
def test_bwrap_cpp_sandbox_blocks_substrate_and_allows_the_query_dir(tmp_path: Path, isolated: Path,
                                                                     monkeypatch: pytest.MonkeyPatch) -> None:
    root = _tree(tmp_path / "tree").resolve()
    cfg = _cfg(root, benchmark_name="toybench")
    monkeypatch.setenv("ACCEL_PIPELINE_QUERY", "6")
    prefix = sandbox.cpp_prefix(cfg)
    script = ("touch substrate/x 2>/dev/null && echo SUB_WRITABLE; touch workloads/toybench/x 2>/dev/null && echo WL_WRITABLE; "
              "touch accelerators/toybench/q6/ok && echo QUERY_OK; touch build-x/ok && echo BUILD_OK; ls /home | wc -l")
    proc = subprocess.run(prefix + ["bash", "-c", script], capture_output=True, text=True, timeout=60)
    out = proc.stdout.split()
    assert "QUERY_OK" in out and "BUILD_OK" in out, proc.stderr
    assert "SUB_WRITABLE" not in out and "WL_WRITABLE" not in out
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
    assert bool(result.metadata.get("write_violations")) == (policy != "off")
