# Aux unit-test contract (mandatory)

`test_*_q*_aux.cpp` smoke tests are **synthetic fixtures**, but they are still a **semantic contract** with the workload SQL. Wrong fixtures that “agree” with a buggy kernel are a failure.

Read the workload `reference_sql_path` (tar placeholders / materialized seeds) **before** writing asserts. Do not invent filter polarity.

## Hard rules

1. **Predicate polarity must match SQL**
   - `col NOT LIKE p` → fixture values that should survive must **fail** the LIKE against `p`.
   - `col <> x` / `!=` → surviving rows must differ from `x`; add a case that equals `x` and is excluded.
   - Anti-joins / `NOT IN` / `LIKE '%Customer%Complaints%'` → excluded keys contribute **0**; included keys count.

2. **Bind args from real SQL when available**
   - Prefer seed_1 / catalog bindings (brand, type pattern, sizes, dates) over made-up patterns.
   - If you shorten patterns for a toy table, keep the **same operator polarity** (e.g. use `SMALL%` against `MED BOX`, never `MED%`).

3. **Required cases (minimum)**
   - Happy path: ≥1 expected output row with correct measures.
   - Each selective predicate: at least one case proving exclusion.
   - Anti-join / eligibility: included vs excluded entity.
   - One mutation path that changes results (update watched column → republish → re-execute).
   - Hot path and fallback must agree when both are reachable (same args / same expected rows).

4. **Comment each case with the SQL clause it exercises**
   - Example: `// p_type NOT LIKE 'SMALL%': MED BOX survives`.

5. **Never “fix” by weakening asserts**
   - Do not delete cases, invert expected counts, or redefine eligibility to match a broken bitmap.
   - If smoke passes but SF1 differential fails, the kernel (or catalog wiring) is wrong — fix the kernel.

## What `aux_correctness` checks

1. **Smoke** — in-process `*_aux_smoke_test` (this contract).
2. **SF1 differential** — DuckDB reference SQL vs aux execute for the catalog seed binding (`test_tpch_shared_differential`).

Both must pass. Smoke-only success is not enough.
