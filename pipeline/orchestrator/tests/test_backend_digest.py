from pathlib import Path

from accel_pipeline.backend_digest import (
    backend_diff,
    backend_digest,
    layer_contracts_digest,
    snapshot_backend,
)

REPO_ROOT = Path(__file__).resolve().parents[3]

README = """# API

## Contract roles

| role | headers | symbols |
|---|---|---|
| registration | `api/register.hpp` | `RegisterThing`, `ThingRegistrar` |
| scan | `api/scan.hpp` | `ScanPartitioned` |
| introspection | | |
"""


def make_backend(root: Path) -> None:
    api = root / "backends" / "toydb" / "api"
    api.mkdir(parents=True)
    (api / "README.md").write_text(README)
    (api / "register.hpp").write_text("// GENERATED BACKEND\n// The registrar every accelerator plugs in through.\nint RegisterThing();\n")
    (api / "scan.hpp").write_text("#pragma once\nint ScanPartitioned();\n")
    tests = root / "backends" / "toydb" / "tests"
    tests.mkdir()
    (tests / "test_toydb_scan.cpp").write_text("int main() {}\n")


def test_digest_lists_files_with_roles_api_symbols_decisions_and_the_next_layers_files(tmp_path):
    make_backend(tmp_path)
    art = tmp_path / "art"
    art.mkdir()
    (art / "experiment_log.md").write_text("- layer `capture` gate FAIL\n- layer `capture` PASS\n")
    (art / "layer_capture.open_findings.md").write_text("blocking: lock held across an engine call\n")
    (art / "backend_analysis.json").write_text('{"layers": [{"id": "epochs", "files": ["backends/toydb/engine/epochs.cpp"]}]}')
    text = backend_digest(tmp_path, "toydb", art, next_layer="epochs")
    assert "`backends/toydb/api/register.hpp`" in text and "The registrar every accelerator plugs in through." in text
    assert "`backends/toydb/tests/test_toydb_scan.cpp`" in text and "layer harness or test" in text
    assert "registration: `RegisterThing`, `ThingRegistrar`" in text and "scan: `ScanPartitioned`" in text
    assert "introspection" not in text  # an n/a role lists nothing
    assert "layer `capture` PASS" in text and "lock held across an engine call" in text
    assert "`backends/toydb/engine/epochs.cpp`" in text
    assert backend_digest(tmp_path, "toydb", art) == backend_digest(tmp_path, "toydb", art)  # deterministic


def test_digest_of_an_empty_backend_says_so(tmp_path):
    assert "(the backend tree is empty)" in backend_digest(tmp_path, "toydb", tmp_path)


def test_diff_against_a_snapshot_shows_only_what_changed(tmp_path):
    make_backend(tmp_path)
    snapshot_backend(tmp_path, "toydb", tmp_path / "before")
    assert backend_diff(tmp_path, "toydb", tmp_path / "before") == ""
    (tmp_path / "backends/toydb/api/scan.hpp").write_text("#pragma once\nint ScanPartitioned(int partitions);\n")
    (tmp_path / "backends/toydb/engine").mkdir()
    (tmp_path / "backends/toydb/engine/new.cpp").write_text("int x;\n")
    (tmp_path / "backends/toydb/tests/test_toydb_scan.cpp").unlink()
    diff = backend_diff(tmp_path, "toydb", tmp_path / "before")
    assert "+int ScanPartitioned(int partitions);" in diff and "+int x;" in diff and "-int main() {}" in diff
    assert "register.hpp" not in diff


def test_contract_digest_keeps_the_layers_section_and_the_requirements_naming_it():
    contracts = (REPO_ROOT / "pipeline/prompts/engine_backend/docs/layer_contracts.md").read_text()
    capture = layer_contracts_digest(contracts, "capture", "layer_contracts.md")
    scan = layer_contracts_digest(contracts, "scan", "layer_contracts.md")
    assert "## `capture` (S1)" in capture and "## `scan` (S5, S8)" not in capture
    assert "**Writes are judged at commit**" in capture and "**Own writes**" not in capture  # names runtime, not capture
    assert "**Lock discipline**" in capture and "**Lock discipline**" in scan  # all layers
    assert len(capture) < len(contracts) / 2 and len(scan) < len(contracts) / 2
    assert "layer_contracts.md" in capture.splitlines()[-1]
