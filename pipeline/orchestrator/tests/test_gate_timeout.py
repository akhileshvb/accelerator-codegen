"""A gate script that hangs is killed with its whole process group and reads as a failure."""

from __future__ import annotations

import json
import os
import time

from accel_pipeline import gates


def test_hung_gate_is_killed_with_its_children(tmp_path, monkeypatch):
    marker = tmp_path / "child.pid"
    script = tmp_path / "hang.sh"
    script.write_text(f"sleep 300 & echo $! > {marker}; wait\n")
    monkeypatch.setattr(gates, "GATE_TIMEOUT_S", 1.0)

    started = time.monotonic()
    proc = gates._run_gate_script(script, 0, tmp_path, dict(os.environ))

    assert time.monotonic() - started < 30
    assert json.loads(proc.stdout.strip().splitlines()[-1])["status"] == "fail"
    child = int(marker.read_text())
    time.sleep(0.2)
    assert not os.path.exists(f"/proc/{child}") or "Z" in open(f"/proc/{child}/stat").read().split()[2]
