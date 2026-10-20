"""The backend API index parsed from api/README.md and rendered into accelerator prompts."""

from pathlib import Path

from accel_pipeline.backend_api import (
    ContractRole,
    parse_contract_roles,
    render_accelerator_engine_profile,
    render_backend_api,
)

PIPELINE_ROOT = Path(__file__).resolve().parents[2]

README = """# toy api

## Contract roles

| role | headers | symbols |
|---|---|---|
| registration | `api/accelerator_api.hpp`, `api/toy_registry.hpp` | `RegisterQuery`, `QueryRegistrar` |
| scan | `api/toy_scan.hpp` | `ScanTable` |
| maintenance | | |

| later | `api/x.hpp` | `Ignored` |
"""


def _write_readme(root: Path, engine: str, text: str = README) -> None:
    api = root / "backends" / engine / "api"
    api.mkdir(parents=True)
    (api / "README.md").write_text(text, encoding="utf-8")


def test_parse_reads_only_the_first_table():
    assert parse_contract_roles(README) == [
        ContractRole("registration", ("api/accelerator_api.hpp", "api/toy_registry.hpp"), ("RegisterQuery", "QueryRegistrar")),
        ContractRole("scan", ("api/toy_scan.hpp",), ("ScanTable",)),
        ContractRole("maintenance", (), ()),
    ]


def test_parse_without_heading_is_empty():
    assert parse_contract_roles("| role | headers | symbols |\n|---|---|---|\n") == []


def test_render_lists_roles_with_engine_paths_and_na(tmp_path):
    _write_readme(tmp_path, "toy")
    out = render_backend_api(tmp_path, "toy")
    assert "- **registration:** `backends/toy/api/accelerator_api.hpp`, `backends/toy/api/toy_registry.hpp`; symbols `RegisterQuery`, `QueryRegistrar`" in out
    assert "- **maintenance:** n/a" in out
    assert "Ignored" not in out


def test_render_differs_only_by_manifest(tmp_path):
    _write_readme(tmp_path, "a")
    _write_readme(tmp_path, "b", README.replace("toy_scan", "b_scan"))
    a, b = render_backend_api(tmp_path, "a"), render_backend_api(tmp_path, "b")
    assert a.replace("backends/a/", "backends/b/") != b
    assert a.replace("backends/a/", "backends/b/").replace("toy_scan", "b_scan") == b


def test_render_without_manifest_says_so(tmp_path):
    assert "absent" in render_backend_api(tmp_path, "none")


def test_profile_rendered_for_shipped_engines_and_empty_for_unknown():
    out = render_accelerator_engine_profile(PIPELINE_ROOT, "postgres")
    assert "### hazards" in out and "### scan" in out
    assert render_accelerator_engine_profile(PIPELINE_ROOT, "nope") == ""
