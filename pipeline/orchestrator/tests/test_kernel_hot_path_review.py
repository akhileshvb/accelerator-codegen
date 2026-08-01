from pathlib import Path

from accel_pipeline.base_bench import build_optimization_diagnosis
from accel_pipeline.kernel_hot_path_review import analyze_kernel_source, format_kernel_review_for_prompt


def test_analyze_kernel_detects_string_keyed_map(tmp_path: Path):
    kernel = tmp_path / "kernel.cpp"
    kernel.write_text(
        """
        std::unordered_map<NameCikKey, double, NameCikKeyHash> name_cik_totals;
        void AccumulateValue(int32_t cik, const std::string &name, double value) {
            const NameCikKey key {name, cik};
            const auto it = name_cik_totals.find(key);
            if (it == name_cik_totals.end()) {
                name_cik_totals.emplace(key, value);
            }
        }
        """,
        encoding="utf-8",
    )
    review = analyze_kernel_source(kernel)
    ids = {s["id"] for s in review["smells"]}
    assert "string_keyed_hash_map" in ids
    assert "map_find_in_hot_path" in ids
    lines = format_kernel_review_for_prompt(review)
    assert any("Intern GROUP BY" in line for line in lines)


def test_analyze_kernel_clean_after_interning(tmp_path: Path):
    kernel = tmp_path / "kernel.cpp"
    kernel.write_text(
        """
        std::unordered_map<NameCikKey, uint32_t, NameCikKeyHash> name_cik_to_group;
        std::vector<double> group_sums;
        void InsertSubRow(const duckdb::string_t &name, int32_t cik) {
            const auto group_id = InternGroup(name, cik);
            group_sums[group_id] = 0;
        }
        void ProcessNumRow(uint32_t group_id, double value) {
            group_sums[group_id] += value;
        }
        """,
        encoding="utf-8",
    )
    review = analyze_kernel_source(kernel)
    high = [s for s in review["smells"] if s.get("severity") == "high"]
    assert not high


def test_build_optimization_diagnosis_scan_amplification():
    baseline = {"speedup_median": 1.02, "reference_median_ms": 2900.0, "accelerator_median_ms": 2850.0}
    profile = {
        "phases_us": {"pre_scan_us": 2_800_000.0, "num_scan_us": 2_790_000.0},
        "counters": {
            "pre_rows": 33_000_000,
            "generic_rows": 0,
            "dict_chunks": 20_000,
            "qualifying_rows": 5_000_000,
            "distinct_inserts": 8_000,
        },
    }
    diagnosis = build_optimization_diagnosis(baseline, profile)
    assert any("scan_amplification" in f for f in diagnosis["findings"])
    assert diagnosis["investigation_questions"]
    assert diagnosis.get("suggested_bundle")
    assert any("early_filter_pushdown" in s for s in diagnosis["strategy_palette"])


def test_build_optimization_diagnosis_kernel_review_merged():
    baseline = {"speedup_median": 1.01}
    profile = {"phases_us": {"pre_scan_us": 100.0}, "counters": {"pre_rows": 1000, "generic_rows": 0}}
    kernel_review = {
        "smells": [
            {
                "id": "string_keyed_hash_map",
                "severity": "high",
                "message": "string map",
                "suggestion": "Intern GROUP BY keys at build time.",
            }
        ]
    }
    diagnosis = build_optimization_diagnosis(baseline, profile, kernel_review=kernel_review)
    assert diagnosis["kernel_review"]["smells"]
    assert diagnosis["investigation_questions"]
    assert any("static_kernel_review" in f for f in diagnosis["findings"])
