from pathlib import Path

from accel_pipeline.base_bench import (
    build_optimization_diagnosis,
    format_optimization_feedback,
    parse_benchmark_log,
    write_baseline_summary,
    write_optimization_feedback,
)
from accel_pipeline.crash_debug import extract_crash_highlights, format_gate_failure_diagnosis
from accel_pipeline.gates import GateResult
from accel_pipeline.kernel_inspiration import format_kernel_inspiration_section, load_kernel_inspiration_catalog


def test_parse_benchmark_log_sec_edgar_summary():
    log = """label,reference_mean_us,reference_median_us,accelerator_mean_us,accelerator_median_us,speedup_mean,speedup_median
q1,437152.70,436185.00,2734453.20,2737151.50,0.16,0.16
summary_avg_median,436185.00,436185.00,2737151.50,2737151.50,0.16,0.16"""
    parsed = parse_benchmark_log(log)
    assert parsed["speedup_median"] == 0.16
    assert parsed["reference_median_us"] == 436185.0


def test_parse_benchmark_log_profile_row():
    log = """label,reference_ms,accelerator_ms,speedup,speedup_median,pre_scan_us,pre_next_us,pre_consume_us,finalize_us,qualifying_rows,output_groups,pre_rows,generic_rows,flat_chunks,dict_chunks,generic_chunks
q1_profile,0,0,0,0,100.0,200.0,3000.0,50.0,10,5,1000000,500000,10,20,30"""
    parsed = parse_benchmark_log(log)
    assert parsed["pre_consume_us"] == 3000.0
    assert parsed["generic_rows"] == 500000


def test_build_optimization_diagnosis_regression_vs_best():
    baseline = {"speedup_median": 0.84, "reference_median_ms": 436.0, "accelerator_median_ms": 522.0}
    profile = {"phases_us": {"pre_consume_us": 800.0}, "counters": {"pre_rows": 100, "generic_rows": 99}}
    diagnosis = build_optimization_diagnosis(baseline, profile, best_speedup_median=1.09)
    assert diagnosis["regressed_vs_best"] is True
    assert any("REGRESSION" in item for item in diagnosis["lacking"])
    assert diagnosis.get("suggested_bundle")


def test_build_optimization_diagnosis_slow_with_generic_rows():
    baseline = {"speedup_median": 0.59, "reference_median_ms": 432.0, "accelerator_median_ms": 728.0}
    profile = {
        "phases_us": {"pre_scan_us": 100.0, "pre_next_us": 200.0, "pre_consume_us": 3000.0, "finalize_us": 50.0},
        "counters": {
            "pre_rows": 9_600_799,
            "generic_rows": 9_586_688,
            "flat_chunks": 1,
            "dict_chunks": 2,
            "generic_chunks": 3,
        },
    }
    diagnosis = build_optimization_diagnosis(baseline, profile)
    assert diagnosis["beats_duckdb"] is False
    assert any("beat DuckDB" in s.lower() or "slower than duckdb" in s.lower() for s in diagnosis["lacking"])
    assert diagnosis.get("suggested_focus")
    assert any("generic_fallback_rows" in f for f in diagnosis["findings"])


def test_write_baseline_summary(tmp_path: Path):
    gates = tmp_path / "gates"
    gates.mkdir()
    (gates / "baseline.json").write_text(
        '{"log_tail": "label,reference_median_us,accelerator_median_us,speedup_median\\n'
        'summary_avg_median,100.0,200.0,0.5"}',
        encoding="utf-8",
    )
    summary = write_baseline_summary(tmp_path)
    assert summary["speedup_median"] == 0.5
    assert (tmp_path / "baseline_summary.json").is_file()


def test_write_optimization_feedback(tmp_path: Path):
    baseline = {"speedup_median": 0.8, "reference_median_ms": 100.0, "accelerator_median_ms": 125.0}
    profile = {"phases_us": {"finalize_us": 500.0}, "counters": {}}
    diagnosis = write_optimization_feedback(tmp_path, baseline, profile, iteration=1)
    assert diagnosis["after_iteration"] == 1
    assert (tmp_path / "optimization_feedback.md").is_file()
    assert "beat DuckDB" in (tmp_path / "optimization_feedback.md").read_text(encoding="utf-8")


def test_kernel_inspiration_catalog_loads():
    from accel_pipeline.paths import find_repo_root

    categories = load_kernel_inspiration_catalog(find_repo_root())
    assert categories
    assert any("Fused scan" in str(c.get("name", "")) for c in categories)


def test_format_kernel_inspiration_section_lists_paths():
    from accel_pipeline.paths import find_repo_root

    section = format_kernel_inspiration_section(find_repo_root(), stage="implementation")
    assert "q12_kernel.cpp" in section
    assert "read_file" in section


def test_crash_highlights_extract_sigsegv():
    log = "line1\nSegmentation fault (core dumped)\nctest failed"
    highlights = extract_crash_highlights(log)
    assert any("Segmentation" in line for line in highlights)


def test_format_gate_failure_diagnosis():
    result = GateResult(
        name="correctness",
        status="fail",
        data={"exit_code": 8, "log_tail": "error: boom\nSegmentation fault"},
    )
    text = format_gate_failure_diagnosis(result, change_kind="optimization", iteration=2)
    assert "correctness" in text
    assert "Segmentation fault" in text
