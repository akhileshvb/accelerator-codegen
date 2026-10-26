"""The accelerator-facing API contract: manifests, include boundary, the api layer."""

from __future__ import annotations

import importlib.util
import sys
from pathlib import Path

from accel_pipeline.config import load_workload
from accel_pipeline.tools.gate_tools import layer_gate_from_stage

REPO_ROOT = Path(__file__).resolve().parents[3]


def _load(path: str):
    spec = importlib.util.spec_from_file_location(Path(path).stem, REPO_ROOT / path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


manifest = _load("pipeline/gates/check_api_manifest.py")
includes = _load("scripts/check_layer_includes.py")

ROW_ALL = "\n".join(f"| {r} | `api/a.hpp` | `Thing` |" for r in manifest.ROLES)


def _backend(tmp_path: Path, table: str, header: str = "struct Thing {};\n") -> Path:
    api = tmp_path / "backends" / "toy" / "api"
    api.mkdir(parents=True)
    (api / "a.hpp").write_text(header)
    (api / "README.md").write_text("# toy\n\n## Contract roles\n\n| role | headers | symbols |\n|---|---|---|\n" + table + "\n")
    return tmp_path


def test_repo_manifests_match_their_headers():
    assert manifest.main.__module__
    for engine in ("duckdb", "postgres", "mysql"):
        assert manifest.check_engine(REPO_ROOT, engine) == []


def test_manifest_accepts_every_role_provided(tmp_path):
    assert manifest.check_engine(_backend(tmp_path, ROW_ALL), "toy") == []


def test_manifest_rejects_missing_role_header_and_symbol(tmp_path):
    table = ROW_ALL.replace("| scan | `api/a.hpp` | `Thing` |", "| scan | `api/gone.hpp` | `Nope` |")
    table = "\n".join(l for l in table.splitlines() if not l.startswith("| runtime"))
    problems = manifest.check_engine(_backend(tmp_path, table), "toy")
    assert any("role 'runtime' has no row" in p for p in problems)
    assert any("gone.hpp does not exist" in p for p in problems)


def test_manifest_requires_every_header_listed_and_symbols_declared(tmp_path):
    root = _backend(tmp_path, ROW_ALL.replace("`Thing`", "`Missing`"))
    (root / "backends" / "toy" / "api" / "b.hpp").write_text("struct Other {};\n")
    problems = manifest.check_engine(root, "toy")
    assert any("symbol Missing not found" in p for p in problems)
    assert any("api/b.hpp is in no role's headers" in p for p in problems)


def test_introspection_symbols_may_live_in_api_sources(tmp_path):
    table = ROW_ALL.replace("| introspection | `api/a.hpp` | `Thing` |", "| introspection | | `accel_get_execute_counts` |")
    root = _backend(tmp_path, table)
    (root / "backends" / "toy" / "api" / "hooks.cpp").write_text('const char *n = "accel_get_execute_counts";\n')
    assert manifest.check_engine(root, "toy") == []


def test_include_boundary_flags_non_api_headers_only(tmp_path):
    (tmp_path / "backends" / "toy" / "api").mkdir(parents=True)
    (tmp_path / "backends" / "toy" / "engine").mkdir()
    (tmp_path / "backends" / "toy" / "api" / "ok.hpp").write_text("")
    (tmp_path / "backends" / "toy" / "engine" / "inner.hpp").write_text("")
    (tmp_path / "eval").mkdir()
    (tmp_path / "eval" / "t.cpp").write_text(
        '#include "backends/toy/api/ok.hpp"\n#include "backends/toy/engine/inner.hpp"\n')
    (tmp_path / "backends" / "toy" / "api" / "own.cpp").write_text('#include "backends/toy/engine/inner.hpp"\n')
    hits, _ = includes.scan_backend_boundary(tmp_path)
    assert [(h["file"], h["line"]) for h in hits] == [("eval/t.cpp", 2)]


def test_repo_includes_only_api_of_backends():
    hits, _ = includes.scan_backend_boundary(REPO_ROOT)
    assert hits == []


def test_api_is_the_last_layer_with_its_own_gate():
    workload = load_workload("engine_mysql", REPO_ROOT)
    layer = workload["implementation_layers"][-1]
    assert layer["id"] == "api" and layer["gate"] == "engine_api_conformance"
    assert layer["cmake_target"] == "api_conformance_mysql"  # every api/ header compiled alone (cmake/Backend.cmake)
    assert layer_gate_from_stage("impl_api", workload) == "engine_api_conformance"
    assert layer_gate_from_stage("impl_epochs", workload) == "engine_epochs"
    assert "engine_api_conformance" in workload["post_validation_gates"]
