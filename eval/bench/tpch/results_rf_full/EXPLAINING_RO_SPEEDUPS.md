# Explaining extreme read-only speedups (TPC-H SF=20)

This note explains what the **JetStream (stateful)** bars in
`sf20_rf_workload_speedup_readonly.{png,pdf}` measure, why the comparison is
fair, and why some queries reach \(10^4\)–\(10^5\times\) over DuckDB / Postgres.

---

## 1. What the speedup measures

For each TPC-H query \(q\) at SF=20, the **JetStream (stateful) vs DuckDB**
number is:

\[
\text{speedup}_q
=
\frac{\text{median DuckDB base-SQL latency}}{\text{median JetStream aux-SQL latency}}
\]

Both sides use DuckDB’s JSON profiler root `"latency"` (same timer):

| Side | SQL | Timer |
|------|-----|--------|
| Baseline | Unmodified TPC-H `base_sql` on DuckDB ST | `PRAGMA enable_profiling='json'` |
| JetStream (stateful) | Generated `aux_sql` table function over warm aux state | same JSON profiler |

So this is **not** “end-to-end system wall time including build,” and **not**
a comparison of different clocks. It is query execution latency of a full
OLAP plan vs a hit on prebuilt auxiliary state.

**JetStream (stateful) vs Postgres** is the analogous ratio for Postgres:
p50 of base Postgres TPC-H vs p50 of the shared/aux path on the same engine.

**JetStream (stateless)** is a different bar: compiled kernels *without* aux
state (≈2× geomean over DuckDB ST). GenDB (MT) / Bespoke OLAP (ST) are
external baselines on the same axes.

Cold-start metrics that **do** add the one-time `Build()` cost exist in the
CSVs (`cold_start_workload_speedup`) but are **not** what the RO chart shows.

---

## 2. Why this is a fair comparison

1. **Same measurement instrument.** DuckDB RO chart uses JSON-profile latency
   for both base SQL and aux SQL (avoids mixing profiler latency with outer
   wall-clock around `connection.Query`).
2. **Steady-state, warm hits.** Aux state is fully built once before the RO
   window. Warmups run both paths. Measured RO points have hit rate 1.0
   (no fallback / rebuild inside the timed queries).
3. **Build is intentionally out of the RO ratio.** Aux `Build()` is a
   one-time capital cost (tens–hundreds of seconds at SF20, logged as
   `setup_full_build_ms`). Steady-state RO asks: *given valid state, how
   fast is a query?* Write-interval / RF workloads elsewhere amortize
   maintenance; cold-start columns exist when you want build included.
4. **Single-threaded, results consumed.** Both engines use `threads=1`;
   both materialize and touch result cells (`ExecuteAndConsume`), so we do
   not win by skipping output.
5. **Within-engine baselines.** DuckDB× and Postgres× are each relative to
   that engine’s own base plan—not “JetStream vs a weak Postgres while
   claiming DuckDB.” Absolute DuckDB vs Postgres ms are not treated as
   interchangeable.

What we are **not** claiming: that a cold empty process answering one query
is \(10^5\times\) faster, or that aux is free to maintain under heavy writes.
The RO plot is the **read-dominated / warm-aux** regime.

---

## 3. Why speedups can be \(10^4\)–\(10^5\times\) (mechanism)

### General story

TPC-H at SF=20 forces the base engine to do **large scans, joins, and
aggregates over hundreds of millions of fact rows** (primarily `lineitem`,
plus `orders` / dimension tables). That work is seconds on DuckDB ST and
often tens of seconds on Postgres.

JetStream’s **stateful** path does not re-run that plan. At build time it
materializes **query-owned, partitioned sufficient statistics** (typed
in-memory images)—compact intermediates keyed by the dimensions the query
actually needs (day, month, nation pair, `(brand, container)`, etc.).

A **hit** is then:

1. Validity check (state still covers the query’s parameter window).
2. `ReadPartitions` for only the keys that window touches.
3. A tiny compose / finalize (ratios, top-\(k\) emit, tree range sum, …).

So asymptotically:

| Path | Dominant cost at SF=20 |
|------|-------------------------|
| Base DuckDB / Postgres | \(O(|\text{lineitem}| + \text{joins})\) → **seconds** |
| JetStream aux hit | \(O(\#\text{touched partitions} + \text{finalize})\) → **~0.2–2 ms** |

Ratio \(\approx 10^3\)–\(10^5\) when the answer’s sufficient statistics fit in
a small structure and the base plan still touches the full fact table.

Aux is **not** (in the extreme cases) a prohibited “cache the full result
relation for every parameter.” It stores **statistics that make the
parameterized query \(O(1)\) or \(O(\log N_{\text{days}})\)** after lookup—
e.g. one month cell, one attribute bucket, a segment-tree range—not a
replay of the OLAP join tree.

### Why Postgres ratios are often larger than DuckDB

Postgres’s base TPC-H plans at SF=20 are typically **much slower** than
DuckDB’s for the same SQL (full scans / nested loops / less aggressive
vectorized execution). JetStream’s hit latency is in the same ballpark on
both engines (pointer/bytes → compose). So

\[
\frac{T_{\text{Postgres base}}}{T_{\text{hit}}}
\;>\;
\frac{T_{\text{DuckDB base}}}{T_{\text{hit}}}
\]

for many queries—even when the *absolute* hit time is similar. That is why
the chart’s Postgres bars sit above DuckDB for Q14/Q15/Q17/Q6/… while a few
queries (e.g. Q19) can invert when Postgres’s base plan is relatively less
terrible or the aux residual work is larger.

### When speedups are *not* extreme

Queries whose aux still does **heavy residual work** or holds **huge
state** stay modest vs DuckDB:

| Query | ≈ vs DuckDB | Why not \(10^4\times\) |
|-------|-------------|------------------------|
| Q18 | ~1.3× | Hit still does large compose / near base cost |
| Q2, Q13 | ~5× | Large aux footprints; hit work not a micro-lookup |
| Q9, Q10 | ~10–35× | Substantial state + non-trivial hit scan/compose |
| Q20 | ~57× | Large state; hit not constant-time |

Extreme bars are concentrated where **base = multi-second fact scan** and
**hit ≈ load one (or few) tiny partition(s) + scalar finalize**.

---

## 4. Concrete query anecdotes (high bars)

Numbers below are SF=20 RO from `results_rf_full` (DuckDB JSON-profile
medians) and the Postgres RO series on the plot.

### Q17 — small-quantity order revenue (~38k× DuckDB, ~286k× Postgres)

- **Base:** Classic TPC-H killer—per-part quantity thresholds, then filter
  lineitems and aggregate. DuckDB ≈ **15.5 s**; hit ≈ **0.41 ms**.
- **Aux:** Precompute contributions grouped by `(brand, container)` (not
  one slot per `partkey`, to keep validity scans tractable).
- **Hit:** Resolve attributes → one partition → `SumAttrContributions` /
  7. Essentially a dictionary lookup + sum of a small contribution list.
- **Why \(10^5\times\) vs Postgres:** Postgres pays the full threshold +
  scan plan; JetStream does not touch `lineitem` on the hit.

### Q15 — top supplier for a 3-month window (~22k× DuckDB, ~195k× Postgres)

- **Base:** Aggregate lineitem revenue by supplier over
  \([\text{start},\text{start}+3\text{mo})\), take the max, join supplier
  attributes. DuckDB ≈ **7.2 s**; hit ≈ **0.33 ms**.
- **Aux:** Per month-start window: `revenue_by_supplier` plus **cached
  winners**.
- **Hit:** Read that window partition and emit the winners (tiny result).
- Extreme because the **answer is a handful of rows** while the base plan
  still aggregates a multi-month slice of SF20 `lineitem`.

### Q14 — promo revenue share (~15k× DuckDB, ~144k× Postgres)

- **Base:** Join `lineitem`⋈`part` for a shipdate month; classify promo
  via `p_type`; aggregate. DuckDB ≈ **4.4 s**; hit ≈ **0.30 ms**.
- **Aux:** Part promo bitvector + **per-month**
  `{total_revenue, promo_revenue}`.
- **Hit:** Map the parameter date → month key → load one cell → finalize
  the promo revenue ratio (one scalar).
- Textbook “sufficient statistic replaces a scan”: the month aggregate
  **is** the query.

### Q6 — selective revenue (~5.5k× DuckDB, ~45k× Postgres)

- **Base:** Predicate-heavy but still a full `lineitem` pass at SF20
  (DuckDB ≈ **1.8 s**).
- **Aux:** Per-month (and day) **discount×quantity revenue matrices**.
- **Hit:** Load ~12 month matrices for a year window; `SumRange` over the
  discount/qty bounds—**no row scan**.
- Illustrates that even “selective” SQL can be seconds when selectivity
  is applied after (or during) a full fact scan, while a dense matrix
  range sum is microseconds.

### Q4 / Q5 / Q7 — windowed OLAP (~12k–16k× DuckDB; up to ~74k× Postgres)

- **Q4:** Eligibility + **segment tree** over orderdate × priority
  counts → 3-month range query (DuckDB base ≈ 4.1 s → hit ≈ 0.33 ms).
- **Q5:** Multi-join nation revenue rolled into a **date segment tree**
  by region/nation (≈ 5.6 s → 0.34 ms).
- **Q7:** Nation-pair × year revenue vectors; hit indexes two nations
  and emits a few cells (≈ 5.4 s → 0.34 ms). Guidance: *materialize
  nation-pair aggregates; avoid re-joining full lineitem/orders on every
  read.*
- Shared theme: **joins and date windows are compiled into trees /
  dense arrays at build**; the hit is logarithmic or \(O(1)\) indexing.

### Q1 — prefix aggregates (~4k× DuckDB, ~28k× Postgres)

- **Base:** Full `lineitem` group-by (DuckDB ≈ **8.3 s**).
- **Aux:** Per-`shipdate` day partitions of `(returnflag, linestatus)`
  stats.
- **Hit:** Load all days ≤ cutoff and **compose** groups (~2 ms)—still
  far cheaper than rescanning SF20 `lineitem`, but more residual work
  than Q14’s single cell, so the ratio is large but not \(10^5\).

---

## 5. How to say this in the paper (short form)

> Read-only speedups compare **warm auxiliary hits** to **full base SQL**
> on the same engine under the same profiler. JetStream’s stateful path
> answers parameterized TPC-H queries from **prebuilt partitioned
> sufficient statistics**, so hit latency is typically **0.2–2 ms** while
> DuckDB/Postgres still **scan and join SF=20 facts for seconds**. The
> largest ratios (Q14, Q15, Q17, and similar windowed aggregates) are
> exactly the queries whose answers are determined by a **small number of
> aggregate cells**, while the base plan remains **linear in the fact
> table**. Postgres ratios are often higher because its base plans are
> slower, not because the hit is slower. These figures exclude one-time
> build cost; maintenance under updates is evaluated separately in the
> write-interval experiments.

---

## 6. Pointers

| Item | Location |
|------|----------|
| RO plot + Postgres × table | `eval/bench/tpch/plot_sf20_rf_workload_speedup.py` |
| Timers / build exclusion | `eval/bench/tpch/single-threaded/aux_benchmark_sweep_session.cpp` |
| Per-query RO CSVs | `eval/bench/tpch/results_rf_full/csv/q*_results.csv` |
| Aux design (sufficient stats, not full-result cache) | `pipeline/prompts/aux_state/shared/aux_state_design.md` |
| Hit / fallback API | `pipeline/prompts/aux_state/shared/read_path_scaffold.md` |
| Query modules | `accelerators/tpch/q{1,4,5,6,7,14,15,17}/duckdb/aux/` (and `postgres/aux/`, `mysql/aux/`) |
