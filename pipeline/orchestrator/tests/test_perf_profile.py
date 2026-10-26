"""The hit-path profile: parsers, per-query counters, and the whole flow against a fake `perf`."""

from __future__ import annotations

import stat
import sys
import textwrap

import pytest

from accel_pipeline import perf_profile as pp

FAKE_PERF = textwrap.dedent(
    """\
    #!{python}
    import os, subprocess, sys
    args = sys.argv[1:]
    verb = args[0]
    def opt(flag):
        return args[args.index(flag) + 1]
    if verb == "stat":
        queries = int(os.environ["ACCEL_BENCH_QUERIES"])
        assert os.environ["ACCEL_PROFILE_CONTROL"].count(",") in (0, 1)
        with open(opt("-o"), "w") as out:
            out.write(f"{{0.5 * queries}},,task-clock:u,1,100.00,,\\n")
            out.write(f"{{5000 * queries}},,cycles:u,1,100.00,,\\n")
            out.write(f"{{12000 * queries}},,instructions:u,1,100.00,,\\n")
            out.write(f"{{800 * queries}},,branches:u,1,100.00,,\\n")
            out.write(f"{{8 * queries}},,branch-misses:u,1,100.00,,\\n")
            out.write("<not supported>,,L1-dcache-load-misses:u,0,0.00,,\\n")
            out.write(f"{{100 * queries}},,cache-references:u,1,100.00,,\\n")
            out.write(f"{{10 * queries}},,cache-misses:u,1,100.00,,\\n")
        sep = args.index("--")
        sys.exit(subprocess.run(args[sep + 1:]).returncode)
    elif verb == "record":
        if os.environ.get("FAKE_NO_WINDOW") != "1":
            open(opt("-o"), "w").write("data")
        sep = args.index("--")
        sys.exit(subprocess.run(args[sep + 1:]).returncode)
    elif verb == "report":
        print("# Overhead  Shared Object  Symbol")
        print("    60.00%  libaccelerator_codegen.so  [.] accelerator_codegen::aux_state::query::ScanState(int)")
        print("    15.00%  libaccelerator_codegen.so  [.] accelerator_codegen::aux_state::core::DecodeBlock()")
        print("    20.00%  libtoydb.so  [.] toydb::Execute()")
        print("     5.00%  libc.so.6  [.] 0x00000000000a73e4")
    elif verb == "annotate":
        print("  0.10 :   401000:   vpaddq (%rdi),%ymm0,%ymm0")
        print("  0.20 :   401005:   vmovdqu %ymm0,(%rsi)")
        print("  0.30 :   40100a:   add    $0x20,%rdi")
    else:
        sys.exit(2)
    """
)


@pytest.fixture
def fake_perf(tmp_path, monkeypatch):
    path = tmp_path / "perf"
    path.write_text(FAKE_PERF.format(python=sys.executable), encoding="utf-8")
    path.chmod(path.stat().st_mode | stat.S_IEXEC)
    monkeypatch.setenv(pp.PERF_BIN_ENV, str(path))
    return path


def test_parse_stat_marks_unsupported_events_none():
    counters = pp.parse_stat("100,,cycles:u,1,100.00,,\n<not supported>,,L1-dcache-load-misses:u,0,0.00,,\n")
    assert counters == {"cycles": 100.0, "L1-dcache-load-misses": None}


def test_per_query_counters_divide_the_window_and_derive_rates():
    window = {"cycles": 10000.0, "instructions": 30000.0, "branches": 1000.0, "branch-misses": 50.0}
    per_query = pp.per_query_counters(window, 10)
    assert per_query["cycles"] == 1000.0
    assert per_query["ipc"] == 3.0
    assert per_query["branch_miss_rate"] == 0.05
    assert per_query["cache-misses"] is None


def test_parse_report_folds_into_buckets_and_functions():
    text = (
        "    40.00%  libaccelerator_codegen.so  [.] ns::query::ScanState(int)\n"
        "    20.00%  libaccelerator_codegen.so  [.] std::sort<ns::query::Entry*>()\n"
        "    15.00%  libaccelerator_codegen.so  [.] ns::core::DecodeBlock()\n"
        "    10.00%  libtoydb.so  [.] toydb::Execute()\n"
        "     5.00%  benchmark_x_aux  [.] ns::query::Compose()\n"
    )
    parsed = pp.parse_report(text)
    assert parsed["self_pct_by_bucket"] == {
        "engine_or_system": 10.0, "own_code": 65.0, "substrate_or_backend": 15.0}
    assert parsed["top"][0]["symbol"] == "ns::query::ScanState(int)"
    assert pp.function_totals(parsed["top"])[0]["bucket"] == "own_code"


def test_vectorization_counts_wide_registers():
    result = pp.vectorization_of("  0.1 :  4010:  vpaddq (%rdi),%ymm0,%ymm0\n  0.2 :  4015:  add $0x20,%rdi\n")
    assert result == {"instructions": 2, "wide_vector_instructions": 1, "uses_wide_vectors": True}


def test_window_query_count_targets_a_short_loop():
    assert pp.window_query_count(None) == 2 * pp.MIN_QUERIES
    assert pp.window_query_count(10_000.0) == 600
    assert pp.window_query_count(1.0) == pp.MAX_QUERIES
    assert pp.window_query_count(1e9) == pp.MIN_QUERIES


def test_server_engine_is_explicitly_not_applicable():
    assert pp.engine_profile_gap({"embedded": False})
    assert pp.engine_profile_gap({"embedded": True}) == ""
    assert pp.engine_profile_gap({"embedded": False, "profile": True}) == ""
    assert pp.engine_profile_gap({"embedded": True, "profile": False})
    assert pp.engine_profile_gap(None)


def test_work_counters_say_what_to_call_when_the_read_reports_nothing():
    silent = pp.work_counter_report({"partitions_read_per_query_median": 0.0}, {})
    assert "RecordPartitionsRead" in silent["note"]
    reported = pp.work_counter_report({"partitions_read_per_query_median": 12.0, "state_bytes_read_median": 4096}, {"x": 1})
    assert reported["state_units_visited_per_query"] == 12.0 and "note" not in reported


def test_profile_hit_path_against_fake_perf(fake_perf):
    profile = pp.profile_hit_path(
        [sys.executable, "-c", "pass"], {}, hit_us=10_000.0, diagnostics={"partitions_read_per_query_median": 3.0}
    )
    assert profile["status"] == "ok"
    assert profile["sampling"]["self_pct_by_bucket"]["own_code"] == 60.0
    assert profile["sampling"]["top_functions"][0]["symbol"].endswith("query::ScanState(int)")
    counters = profile["counters_per_query"]
    assert counters["cycles"] == 5000.0 and counters["ipc"] == 2.4
    assert profile["vectorization_of_hottest_own_function"]["uses_wide_vectors"] is True
    assert profile["work_counters"]["state_units_visited_per_query"] == 3.0
    text = pp.render_for_agent(profile)
    assert "ScanState" in text and "ipc=2.4" in text and "cpu_ms=0.5" in text


def test_bench_that_never_opens_the_window_is_an_error(fake_perf):
    profile = pp.profile_hit_path([sys.executable, "-c", "pass"], {"FAKE_NO_WINDOW": "1"}, hit_us=10_000.0)
    assert profile["status"] == "error"
    assert pp.render_for_agent(profile).startswith("profile FAILED")


def test_profile_hit_path_without_perf_is_an_error_not_a_waiver(monkeypatch):
    monkeypatch.setenv(pp.PERF_BIN_ENV, "/nonexistent/perf")
    profile = pp.profile_hit_path(["true"], {})
    assert profile["status"] == "error" and "perf" in profile["reason"]
    assert "FAILED" in pp.render_for_agent(profile)


def test_a_server_engine_stays_not_applicable():
    assert "not applicable" in pp.render_for_agent(pp.not_applicable(pp.engine_profile_gap({"embedded": False})))


def test_counters_alone_take_no_samples(fake_perf):
    profile = pp.profile_hit_path([sys.executable, "-c", "pass"], {}, hit_us=1000.0, loop_seconds=0.5, record=False)
    assert profile["status"] == "ok" and "sampling" not in profile
    assert profile["counters_per_query"]["instructions"] == 12000.0


def test_the_loop_length_sets_the_window(fake_perf):
    long = pp.profile_hit_path([sys.executable, "-c", "pass"], {}, hit_us=1000.0, loop_seconds=10.0, record=False)
    assert long["counters_per_query"]["cycles"] == 5000.0  # per query, whatever the window held
    assert pp.window_query_count(1000.0, 10.0) == 10_000 and pp.window_query_count(1000.0, 0.5) == pp.MIN_QUERIES * 5


def test_the_build_profile_lists_the_hot_spots(fake_perf):
    profile = pp.profile_build([sys.executable, "-c", "pass"], {})
    assert profile["status"] == "ok"
    assert profile["sampling"]["top_functions"][0]["symbol"].endswith("query::ScanState(int)")
    assert "ScanState" in pp.render_for_agent(profile)


def test_a_build_with_no_samples_is_an_error(fake_perf):
    assert pp.profile_build([sys.executable, "-c", "pass"], {"FAKE_NO_WINDOW": "1"})["status"] == "error"
