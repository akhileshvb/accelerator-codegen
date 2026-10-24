# Stage 4: Aux-state audit

Audit **Q{{query_number}}** (`{{aux_function_name}}`) against the stateful goals: correct, fast reads and cheap maintenance from COW primitive state (`cow_primitives.md`), hardware-aware inner loops (`low_level_execution.md`), at SF3000.

{{hard_guidelines}}

## Audit contract

```json
{{audit_context_json}}
```

Full analysis: `{{full_analysis_path}}`

## What you are given

The user message carries a deterministic **query digest** (file map with roles, public entry points, the chosen design, the experiment log, open findings) and the **cumulative diff since implementation began**. Review from those; open a file only where they leave a question open, rather than re-reading the whole directory.

Your findings are acted on: anything you list under `scale_review.generality_failures`, `scale_review.cow_findings`, a failed `scale_review.large_scale` class, `fairness_review.issues` with `cheating_suspected: true`, or `correctness_ok: false` is sent to the implementer as one batched repair round, and you are then asked to re-check only those findings. List a problem there only if it is a real blocker; put the rest under `risks`.

## Task

Hard gates after this stage verify **correctness** and run a **fair SF1 bench sweep**. Your job is analysis + a short written audit — not brittle micro-gates.

1. **Correctness / contract** — implementation matches the selected design (partitioning, mutations, payload, `cross_accelerator_concurrency`). Confirm `mutation_coverage` behaviors are tested, and that the frozen suite (`tests/workload/`: `suite.yaml`, `params.sql`, `domain.yaml`, `txns/`) covers every parameter of the query and every write kind that can change its answer; list gaps as findings.
2. **Sweep evidence** — read `{{artifacts_path}}/aux_bench_summary.json` and/or `gates/aux_bench_sweep.json` (or `request_gate` `aux_bench_sweep` if missing). Record 0% / write-mix speedups and hit latency.
3. **Scale review** (write this up; do not invent SF10 numbers if unmeasured):
   - Do structures size from bind-time / `row_estimate` (or equivalent), or are there SF1-hardcoded capacities / loops?
   - Per-query and per-write cost: O(touched work) vs full rebuild / full-table scan on the hot path?
   - Any absolute SF1 cardinalities (`150000` / `600000` / `800000`) used as live capacities in **aux** code?
   - **Generality checklist** (list each as pass/fail with file:line evidence; any fail is `correctness_ok: false` or a scale risk):
     1. Every SQL parameter exact for any value of its type incl. NULL (no crash, no drop, no throw); no enumerated/hardcoded parameter domains.
     2. Dates: non-month-aligned, Feb 29, month-end + interval, dates outside the data's usual years use the backend API's date semantics.
     3. LIKE: partial/mixed-case words, `_`, escapes, empty pattern via the backend API's LIKE/collation semantics.
     4. Enum/numeric params: unknown value, params equal to each other, out-of-range, fractional.
     5. Data: out-of-range dates, unseen categories, off-range numbers, NULLs, negative/huge keys handled in build **and** delta paths.
     6. No key/id/offset narrower than 64-bit, no bit-packed fields, no fixed dense cap, no hashed ids used as exact partition ids, blob offsets 64-bit/chunked.
     7. Memory: bytes/row estimate stated; no node-based map or per-row `std::string`; no 2-5x transient build copies.
     8. Writes: per-write cost O(touched); no whole-state copy per batch; no `RebuildAll` for routine insert/delete/update.
     9. Parameters not precomputed take a fast exact path, not a per-call full rebuild.
     10. Numerics: no `double` for decimals/thresholds, no int128 -> int64 truncation, engine rounding rules matched.
     11. Copy-on-write hygiene: report as findings (file:line) any hand-rolled sharded/chunked COW container in the module, any whole-state copy per commit (e.g. `make_shared<State>(*old)` over O(rows) members, copying a map/vector of all rows to apply a delta), and any container duplicating what a `prims/` primitive already covers (name the primitive). Workload-specific structures built from primitives and still COW are fine. List them in `scale_review.cow_findings`.
     12. Evidence: host `test_<engine>_<bench>_params` (plain, `--data-perturb`, `--wide-keys`, `--negative-keys`) run or equivalent tests present; state results.
4. **Large-scale and parallel review** (`scale_factor_contract.md` failure-class table; `engine_interfaces.md` thread rules). Put each in `scale_review.large_scale` as `{class, pass|fail|n/a, file:line evidence}`; a fail is `correctness_ok: false` (overflow, truncation, skipped keys) or a scale risk (the rest):
   1. Every item of `correctness_facts.scale_plan.failure_classes` is implemented as stated; classes it omitted are assessed anyway.
   2. No max-key-sized or fixed-count containers; no size guard that rebuilds; overlays fold and deletes remove; loop indices and counters over rows are 64-bit (key, offset and sum width: generality items 6 and 10).
   3. `Build` is `PartitionedBuild` with a cheap, associatively merged `Partial` (float partials on `kFloatBuildPartitions`); `Execute` and maintenance split data-proportional work with the substrate helpers; no `hardware_concurrency`, fixed counts, private pools or engine threads; slices call no engine API, `StateStore()` or `CurrentVisibility()`; no shared mutable process state.
   4. `ChooseMaintenance` fed honest `rows_touched` / `affected_partition_count` / `*_supported`; cost model has `calibrated_scale_rows`; calibration writes include refresh-shaped writes.
   5. Evidence: the magnitude-database differential (`tpch_shift`, `tpch_big`), the 1-vs-N thread differential (with `ACCEL_AUX_TEST_GRAIN=1`) and the decision-log / `ReadAuxExecuteCounts` check were run; the `aux_tsan` gate (ThreadSanitizer, forced tiny grains) passed for any parallel read/build/maintenance code; state results, or mark the class unverified.
5. **Harness / benchmark fairness** — confirm the timed path is the real production query (consume results; no result-cache-as-aux; no skipping work the base path does). Flag any harness gaming.
6. Write `{{artifacts_path}}/audit_result.json` and `{{artifacts_path}}/final_audit.md` with clear verdicts:

```json
{
  "design_success": true,
  "correctness_ok": true,
  "bench_sweep": {
    "read_only_query_speedup": 0.0,
    "write_mix_workload_speedup": 0.0,
    "write_mix_pct": 10,
    "accel_hit_median_us": 0.0,
    "notes": ""
  },
  "scale_review": {
    "likely_scales": true,
    "bytes_per_row_estimate": 0.0,
    "generality_failures": [],
    "cow_findings": [],
    "large_scale": [],
    "risks": [],
    "notes": ""
  },
  "fairness_review": {
    "cheating_suspected": false,
    "issues": [],
    "notes": ""
  }
}
```

Optional: `request_gate` `fairness` is a **heuristic** static scan (informational). Do not treat substring false-positives as blockers; explain them in the audit.

Post-stage hard gates: `aux_correctness`, `aux_unit`, `prims_tests`, `aux_tsan`, `module_tsan`, `aux_bench_sweep`.

{{shared_docs_reference}}

{{constraints}}
