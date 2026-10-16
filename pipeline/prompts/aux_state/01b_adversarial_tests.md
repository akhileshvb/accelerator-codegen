# Aux-state adversarial write tests (post-analysis)

Design **rigorous invalidating-write scenarios** for **{{display_name}}** **Q{{query_number}}**. Do **not** implement the accelerator.

{{hard_guidelines}}

{{codex_tooling}}

## Goal

Produce a small frozen contract that later correctness gates enforce via the **shared** SQL-vs-aux harness (catalog probes + TPC-H RF chunks). Cover:

1. **Point writes** — several distinct rows that must invalidate aux (multiple probes, not one)
2. **Bulk writes** — RF-style multi-row / multi-chunk churn when the query depends on `orders`/`lineitem`
3. **Sequences** — more than one invalidating write applied before restores (incremental, not only RO)
4. **Large-scale shapes** — the failure classes of `scale_factor_contract.md` that `correctness_facts.scale_plan` says apply: writes of rows with keys beyond 2^32 and with value-inflated money columns; RF-sized bulk writes spread over the key space, repeated insert/delete cycles (state must not grow), and a bulk write compared at 1 vs N maintenance threads with `ACCEL_AUX_TEST_GRAIN=1` (forces the split; also raced by the `aux_tsan` gate)

## Inputs

- Analysis: `{{full_analysis_path}}`
- Compact facts: `{{analysis_json}}`
- Catalog contract (`backends/{engine}/api/catalog/` / `probe_select` + `build_probe`) — read-only; do not invent private C++ APIs
- Shared harness: `RunAdversarialWriteDifferentialCheck` uses catalog probes + RF; your JSON only **configures** how hard that check is

## Deliverable (required)

Write exactly:

`{{artifacts_path}}/adversarial_write_scenarios.json`

Schema:

```json
{
  "version": 1,
  "query": {{query_number}},
  "point_writes": {
    "min_count": 3,
    "max_probes": 8,
    "tables": ["orders"],
    "notes": "short: how probes invalidate"
  },
  "bulk_writes": {
    "required": true,
    "min_chunks": 2,
    "notes": "RF1/RF2 on orders+lineitem"
  },
  "scale_scenarios": ["short, one per applicable failure class: the write or database that exercises it"],
  "rationale": "2-5 sentences tying scenarios to analysis"
}
```

Rules:

- `point_writes.min_count` ≥ 3; `max_probes` ≥ `min_count` and ≤ 16
- Set `bulk_writes.required=true` and `min_chunks` ≥ 1 when the query depends on `orders` and/or `lineitem`
- Set `bulk_writes.required=false` only when RF on orders/lineitem cannot invalidate this query’s result (explain in notes)
- `scale_scenarios` (optional for the gate, required here): one entry per `scale_plan.failure_classes` item that is not `n/a`, naming the write shape or magnitude database (`scripts/make_tpch_magnitude_dbs.sh`: `tpch_shift`, `tpch_big`) that would expose it; implementation and audit run them
- Prefer tables that appear in analysis join/filter paths and that catalog probes can hit
- Implementation must expand catalog `probe_select` until at least `min_count` distinct invalidating probes exist (gate enforces this)
- Do **not** invent synthetic C++ fixtures or module-private state APIs
- Do **not** edit accelerator sources, catalog, or smoke tests in this stage

## Shared docs (read on demand)

{{shared_docs_reference}}

## Done when

1. `adversarial_write_scenarios.json` exists and validates against the schema above
2. Rationale references concrete tables/predicates from analysis
3. Point + bulk coverage is enough that a wrong invalidate or stale maintenance would fail the shared gate
