"""An agent stage cannot change the pipeline's own files or the substrate."""

from __future__ import annotations

from accel_pipeline import layout


def test_edits_to_frozen_paths_are_reverted(tmp_path):
    gate = tmp_path / "pipeline/gates/run_x.sh"
    gate.parent.mkdir(parents=True)
    gate.write_text("original\n")
    backend = tmp_path / "backends/anyengine/engine.cpp"
    backend.parent.mkdir(parents=True)
    backend.write_text("v1\n")
    before = layout.frozen_snapshot(tmp_path)

    gate.write_text("weakened\n")
    (tmp_path / "substrate").mkdir()
    (tmp_path / "substrate/new.hpp").write_text("x\n")
    backend.write_text("v2\n")

    changed = layout.frozen_restore(tmp_path, before)
    assert gate.read_text() == "original\n"
    assert not (tmp_path / "substrate/new.hpp").exists()
    assert backend.read_text() == "v2\n"
    assert sorted(changed) == ["added substrate/new.hpp", "edited pipeline/gates/run_x.sh"]


def test_a_committed_change_is_kept(tmp_path):
    import subprocess

    gate = tmp_path / "pipeline/gates/run_x.sh"
    gate.parent.mkdir(parents=True)
    gate.write_text("v1\n")
    git = ["git", "-C", str(tmp_path), "-c", "user.name=t", "-c", "user.email=t@t"]
    subprocess.run([*git, "init", "-q"], check=True)
    subprocess.run([*git, "add", "-A"], check=True)
    subprocess.run([*git, "commit", "-qm", "v1"], check=True)
    before = layout.frozen_snapshot(tmp_path)

    gate.write_text("v2 fix\n")
    subprocess.run([*git, "commit", "-qam", "fix"], check=True)

    assert layout.frozen_restore(tmp_path, before) == []
    assert gate.read_text() == "v2 fix\n"
