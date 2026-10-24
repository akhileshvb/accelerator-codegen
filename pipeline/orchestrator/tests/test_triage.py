import subprocess

import pytest

from accel_pipeline import triage

WORKLOAD = {"benchmark_name": "tpch", "query_number": 3}

TSAN = """WARNING: ThreadSanitizer: data race (pid=1)
  Write of size 8 at 0x1 by thread T1:
    #0 do_work /engine/src/pool.cc:88 (libengine.so+0x1)
    #1 run /engine/src/pool.cc:90 (libengine.so+0x2)
SUMMARY: ThreadSanitizer: data race /engine/src/pool.cc:88 in do_work
"""
TSAN_OURS = TSAN.replace("/engine/src/pool.cc:88 (libengine", "/repo/accelerators/tpch/q3/aux.cpp:88 (libengine", 1)


@pytest.fixture
def repo(tmp_path):
    subprocess.run(["git", "init", "-q"], cwd=tmp_path, check=True)
    subprocess.run(["git", "-c", "user.email=a@b", "-c", "user.name=n", "commit", "-q", "--allow-empty", "-m", "i"],
                   cwd=tmp_path, check=True)
    return tmp_path


def classify(text, repo, **kw):
    return triage.classify("aux_correctness", {"log_tail": text, **kw}, WORKLOAD, repo)


def test_engine_internal_sanitizer_is_infra(repo):
    assert classify(TSAN, repo) == ("infra", "engine_internal_sanitizer")


def test_sanitizer_frame_in_the_query_dir_is_code(repo):
    assert classify(TSAN_OURS, repo) == ("code", None)


def test_sanitizer_frame_in_a_file_changed_this_stage_is_code(repo):
    (repo / "shared.cc").write_text("x")
    text = TSAN.replace("/engine/src/pool.cc:88 (libengine", "/repo/shared.cc:88 (libengine", 1)
    assert classify(text, repo) == ("code", None)


def test_missing_producer_artifact(repo):
    assert classify("error: analysis.json not found for query 3", repo) == ("infra", "missing_producer")
    assert classify("target missing from the gate context", repo) == ("infra", "missing_producer")


@pytest.mark.parametrize("text", [
    "run.sh: line 4: ACCEL_X: unbound variable",
    "Traceback (most recent call last):\n  File \"/r/pipeline/gates/read_gate_context.py\", line 3, in <module>\nKeyError: 'a'",
    "backends/e/host.sh up exited with status 2",
])
def test_harness_errors(text, repo):
    assert classify(text, repo) == ("infra", "harness_error")


def test_python_traceback_outside_the_harness_is_code(repo):
    text = "Traceback (most recent call last):\n  File \"/r/accelerators/tpch/q3/check.py\", line 3\nValueError"
    assert classify(text, repo) == ("code", None)


def test_timeout_is_infra_only_under_load(repo, monkeypatch):
    text = "gate did not finish within 7200s"
    monkeypatch.setattr("os.getloadavg", lambda: (0.1, 0.1, 0.1))
    assert classify(text, repo) == ("code", None)
    monkeypatch.setattr("os.getloadavg", lambda: (1e6, 1e6, 1e6))
    assert classify(text, repo) == ("infra", "timeout_under_load")
    assert classify("", repo, returncode=137) == ("infra", "timeout_under_load")


def test_shared_tree_failures_are_infra(repo):
    assert classify("unsupported shared query binding", repo) == ("infra", "unsupported_shared_query")


def test_plain_test_failure_is_code(repo):
    assert classify("Segmentation fault in accelerators/tpch/q3/aux.cpp\n1 test failed", repo) == ("code", None)


def test_transient_provider_failure():
    assert triage.is_transient_provider_failure("stream disconnected before completion")
    assert not triage.is_transient_provider_failure("assertion failed in kernel")
    assert not triage.is_transient_provider_failure(None)
