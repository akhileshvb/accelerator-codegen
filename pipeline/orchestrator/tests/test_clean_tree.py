"""The clean-tree manifest, the builder and the leakage gate (pipeline/clean_tree/)."""
from __future__ import annotations

import importlib.util
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[3]
CLEAN_TREE = REPO / "pipeline" / "clean_tree"


def _load(name: str):
    sys.path.insert(0, str(CLEAN_TREE))
    spec = importlib.util.spec_from_file_location(name, CLEAN_TREE / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


manifest_py = _load("manifest")
check_leakage = _load("check_leakage")

pytestmark = pytest.mark.skipif(not (REPO / ".git").exists(), reason="needs a git checkout")


def _git(cwd: Path, *args: str) -> str:
    env = {"GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@t", "GIT_COMMITTER_NAME": "t", "GIT_COMMITTER_EMAIL": "t@t",
           "PATH": "/usr/bin:/bin", "HOME": str(cwd)}
    return subprocess.run(["git", *args], cwd=cwd, env=env, check=True, capture_output=True, text=True).stdout


def _toy_repo(tmp_path: Path) -> Path:
    """A repo with the real manifest and checker, one file per class, and a skeleton."""
    repo = tmp_path / "src"
    shutil.copytree(CLEAN_TREE, repo / "pipeline/clean_tree", ignore=shutil.ignore_patterns("__pycache__"))
    files = {
        "substrate/aux/core/store.hpp": "struct Store {};\n",
        "backends/acme/api/host.hpp": "namespace acme {\nclass AcmeHostAdapter {};\nvoid run_acme_scan_pass();\n}\n",
        "accelerators/prims/old_map.hpp": "struct OldMap {};\n",
        "eval/compare/results/run1.csv": "a,b\n",
        "pipeline/prompts/keep_me.md": "Read `backends/acme/api/host.hpp`; use AcmeHostAdapter and run_acme_scan_pass.\n",
        "pipeline/prompts/clean.md": "Read the backend's api/README.md manifest.\n",
        "scripts/driver.sh": '#include "prims/old_map.hpp"\n',
    }
    for rel, text in files.items():
        (repo / rel).parent.mkdir(parents=True, exist_ok=True)
        (repo / rel).write_text(text, encoding="utf-8")
    _git(tmp_path / "src", "init", "-q")
    _git(repo, "add", "-A")
    _git(repo, "commit", "-q", "-m", "toy")
    return repo


def test_every_tracked_file_is_classified():
    manifest = manifest_py.Manifest.load()
    unclassified = [f for f in manifest_py.tracked_files(REPO) if manifest.classify(f) is None]
    assert not unclassified, unclassified[:20]


def test_substrate_is_kept_and_backends_are_generated():
    manifest = manifest_py.Manifest.load()
    assert manifest.classify("substrate/aux/core/aux_cost_model.hpp") == "KEEP"
    assert manifest.classify("backends/acme/api/anything.hpp") == "GENERATED"
    assert manifest.classify("accelerators/benchx/q1/acme/aux/module.cpp") == "GENERATED"
    assert manifest.classify("eval/compare/results/x/y.csv") == "DROP"
    assert manifest.classify("pipeline/artifacts/shared/optimization_playbook.md") == "DROP"


def test_skeleton_replaces_generated_prims_and_prooflib():
    manifest = manifest_py.Manifest.load()
    skeleton = CLEAN_TREE / "skeleton"
    for path in skeleton.rglob("*"):
        if path.is_file():
            assert manifest.classify(path.relative_to(skeleton).as_posix()) == "GENERATED"


def test_builder_exports_a_standalone_single_commit_tree(tmp_path):
    repo = _toy_repo(tmp_path)
    dest = tmp_path / "clean"
    subprocess.run([str(CLEAN_TREE / "make_clean_tree.sh"), str(repo), str(dest)], check=True, capture_output=True,
                   env={"PATH": "/usr/bin:/bin", "HOME": str(tmp_path), "GIT_CONFIG_GLOBAL": "/dev/null"})
    kept = set(_git(dest, "ls-files").split())
    assert "substrate/aux/core/store.hpp" in kept and "pipeline/prompts/keep_me.md" in kept
    assert not kept & {"backends/acme/api/host.hpp", "accelerators/prims/old_map.hpp", "eval/compare/results/run1.csv"}
    assert {"accelerators/prims/README.md", "accelerators/prims/tests/test_prims.cpp"} <= kept
    assert not (dest / "pipeline/clean_tree/skeleton").exists()
    assert (dest / ".git").is_dir()
    assert _git(dest, "rev-list", "--count", "HEAD").strip() == "1"
    assert _git(dest, "remote").strip() == "" and not (dest / ".git/objects/info/alternates").exists()
    assert _git(dest, "status", "--porcelain").strip() == ""


def test_leakage_gate_finds_each_kind_of_reference(tmp_path):
    repo = _toy_repo(tmp_path)
    findings, _, _ = check_leakage.run(repo)
    kinds = {(f.file, f.kind, f.token) for f in findings}
    assert ("pipeline/prompts/keep_me.md", "path", "backends/acme/api/host.hpp") in kinds
    assert ("pipeline/prompts/keep_me.md", "symbol", "AcmeHostAdapter") in kinds
    assert ("pipeline/prompts/keep_me.md", "symbol", "run_acme_scan_pass") in kinds
    assert ("scripts/driver.sh", "include", "prims/old_map.hpp") in kinds
    assert not [f for f in findings if f.file == "pipeline/prompts/clean.md"]


def test_leakage_never_grows_beyond_the_baseline():
    if not any(f.startswith("backends/") for f in manifest_py.tracked_files(REPO)):
        pytest.skip("the generated material is absent (clean tree): nothing to index")
    findings, _, _ = check_leakage.run(REPO)
    baseline = __import__("json").loads((CLEAN_TREE / "leakage_baseline.json").read_text(encoding="utf-8"))
    assert not check_leakage.regressions(findings, baseline)
