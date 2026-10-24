"""A gate script that hangs is killed with its whole process group and reads as a failure."""

from __future__ import annotations

import json
import os
import subprocess
import threading
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


def test_a_cancelled_gate_is_killed_and_reads_as_cancelled(tmp_path):
    script = tmp_path / "hang.sh"
    script.write_text("sleep 300\n")
    cancel = threading.Event()
    threading.Timer(0.5, cancel.set).start()

    started = time.monotonic()
    proc = gates._run_gate_script(script, 0, tmp_path, dict(os.environ), cancel=cancel)

    assert time.monotonic() - started < 30
    assert json.loads(proc.stdout.strip().splitlines()[-1])["status"] == "cancelled"


def test_the_timeout_can_be_given_per_call(tmp_path):
    script = tmp_path / "hang.sh"
    script.write_text("sleep 300\n")

    proc = gates._run_gate_script(script, 0, tmp_path, dict(os.environ), timeout_s=0.5)

    assert json.loads(proc.stdout.strip().splitlines()[-1])["status"] == "fail"


def test_a_tool_call_limit_kills_the_gate_group_before_the_client_gives_up(tmp_path, monkeypatch):
    marker = tmp_path / "child.pid"
    script = tmp_path / "hang.sh"
    script.write_text(f"sleep 300 & echo $! > {marker}; wait\n")
    monkeypatch.setattr(gates, "TOOL_CALL_MARGIN_S", 0.0)

    started = time.monotonic()
    with gates.tool_call_limit(1.0):
        proc = gates._run_gate_script(script, 0, tmp_path, dict(os.environ))  # no gate timeout of its own (2 h)

    assert time.monotonic() - started < 30
    verdict = json.loads(proc.stdout.strip().splitlines()[-1])
    assert verdict["status"] == "fail" and "tool-call limit" in verdict["reason"] and "background=True" in verdict["reason"]
    child = int(marker.read_text())
    time.sleep(0.2)
    assert not os.path.exists(f"/proc/{child}") or "Z" in open(f"/proc/{child}/stat").read().split()[2]


def test_outside_a_tool_call_the_gate_timeout_alone_applies(tmp_path):
    script = tmp_path / "quick.sh"
    script.write_text("echo done\n")
    with gates.tool_call_limit(1.0):
        pass
    assert gates._tool_deadline.get() is None
    assert gates._run_gate_script(script, 0, tmp_path, dict(os.environ)).stdout.strip() == "done"


def test_a_gate_still_running_when_the_server_exits_is_killed(tmp_path):
    script = tmp_path / "hang.sh"
    script.write_text("sleep 300\n")
    proc = subprocess.Popen(["bash", str(script)], start_new_session=True)
    gates._live_groups.add(proc.pid)
    gates._kill_live_groups()
    assert proc.wait(timeout=10) == -9
    gates._live_groups.discard(proc.pid)
