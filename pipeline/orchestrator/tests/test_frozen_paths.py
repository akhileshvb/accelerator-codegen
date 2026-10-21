"""An agent stage cannot change the pipeline's own files or the substrate."""

from __future__ import annotations

from accel_pipeline import frozen_paths


def test_edits_to_frozen_paths_are_reverted(tmp_path):
    gate = tmp_path / "pipeline/gates/run_x.sh"
    gate.parent.mkdir(parents=True)
    gate.write_text("original\n")
    backend = tmp_path / "backends/duckdb/engine.cpp"
    backend.parent.mkdir(parents=True)
    backend.write_text("v1\n")
    before = frozen_paths.snapshot(tmp_path)

    gate.write_text("weakened\n")
    (tmp_path / "substrate").mkdir()
    (tmp_path / "substrate/new.hpp").write_text("x\n")
    backend.write_text("v2\n")

    changed = frozen_paths.restore(tmp_path, before)
    assert gate.read_text() == "original\n"
    assert not (tmp_path / "substrate/new.hpp").exists()
    assert backend.read_text() == "v2\n"
    assert sorted(changed) == ["added substrate/new.hpp", "edited pipeline/gates/run_x.sh"]
