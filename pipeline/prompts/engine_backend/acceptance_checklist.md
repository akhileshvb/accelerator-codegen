# Engine backend acceptance checklist (shared bar)

**Audience:** analysis, implementation, and architecture_audit.
**Scope:** engine-agnostic. Do **not** invent engine-specific APIs here — map
each item using this engine's profile / analysis only.

This is the **single closed set of hard rules**. Implementation must satisfy
every item before finishing. Architecture audit may **only** fail on these
`rule` ids (below). Do **not** fail for style, extra concurrency ideas, or
requirements that appear only in `backend_analysis.json` but are not listed
here. Each rule is owned by one implementation layer (or is global).

Gates (`engine_build` plus each layer gate in `layers.md`) are necessary but
**not sufficient**: a green harness can still fail audit if a checklist item
is missing in source. The slot gates (`slot_*`, validation stage, including `slot_tsan`) judge the
behaviour of the finished backend under concurrency.

---

## Rules (fixed `rule` ids)

### `slot_capture` (layer `capture`)

- Capture delivers **one** `ColumnarBatch` per statement and table through the
  host-bound delivery function (`RuntimeHost::OnCapture`), for **every**
  connection, with exactly the columns of `RuntimeHost::CaptureColumns(table)`.
- Update / delete batches carry old row values; types are typed columns, no
  per-row heap allocation; rolled-back (sub)transactions never reach a commit;
  a non-row change (TRUNCATE) invalidates.
- Harness asserts values (table id, kind, rows, old/new cells incl. NULL, a wide
  decimal) from two connections. Hand-built batches alone are not this rule.

### `slot_epochs` (layer `epochs`)

- Commit epochs increase per accelerator in **visibility order**; the reader
  epoch is exact (`epoch(c) <= E` iff visible); the version floor never exceeds
  an open reader's epoch; `PendingEpochLowerBound` covers the window between
  pre-commit and visibility.
- Wired to the engine's real commit / abort lifecycle (`PreCommit` / `Commit` /
  `Abort`); a harness that calls those C++ methods directly is not this rule.
- No wall-clock epochs; an unmappable snapshot is reported, not guessed.
- Harness: concurrent writers, two accelerators, snapshots inside the pending
  window (sync barrier, not `sleep`), floor vs open readers, abort after
  pre-commit.

### `slot_scan` (layer `scan`)

- The build snapshot's epoch `b` is exact (commit in snapshot iff epoch <= `b`)
  and writers are not blocked while it is open.
- `WaitForPreRegistrationWriters` waits for earlier writers and times out
  (false). `RunBuild` calls `builder.Finish()`.
- Scans are partitioned, typed, chunked and parallel on the thread budget;
  64-bit counts and offsets. The module-facing `ScanPartitioned` surface and
  its guarantees (`native_scan_contract.md`) exist with the specified names:
  fixed partitions in table order, one thread per partition, all at epoch `b`,
  at most the build cap of threads, `partitions == 1` serial on the caller.

### `native_scan` (layer `scan`)

- The scan uses **in-process / storage-AM / table-AM** (or the engine's native
  connection-per-partition range-scan) APIs and emits **engine-local** typed
  chunks. **Forbidden:** a portable `DataChunk` IR; a single client SQL
  round-trip that parses rows on the hot path; a silent no-op stub.
- The harness value-checks it: >=3 seeded rows incl. NULL, a proper-subset
  projection (order preserved), a filter dropping >=1 row, exact cells, >=2
  chunk callbacks whose concatenation equals the golden set. `Build` reaches
  the scan through `RunBuild`; a UDF that re-implements the scan inline does
  not count.

### `slot_runtime` (layer `runtime`)

- The host owns one `RuntimeHost` with the engine's thread setting
  (`ResolveThreadBudget`); `EngineWorkOnHostThread()` matches the placement.
- Seed -> background build -> read; DML -> rematch; an invalidated state
  rebuilds **while writes continue** and becomes synced without a quiet
  window; an old reader gets its own epoch's answer or `Fallback`, never a
  newer one; the three phase caps come from the engine's settings or the
  `ACCEL_AUX_*_THREADS` overrides and are observed by `WorkerCount()`;
  `TableRows` feeds `DataScaleRows`; `CostModel` is resolved by engine and scale; shipped placement: gap -> rebuild, worker restart -> reads fall
  back exact then hit; peak threads <= the budget.

### `slot_entry` (layer `host`; skip if the workload does not require an in-process host)

- The SQL entry decodes arguments (incl. NULL), calls
  `AcceleratorRuntime::Read` at the S3 epoch, emits rows, answers `Fallback`
  through `ExecuteFallback`. Read parallelism is inside `Execute`, not an
  engine parallel source.

### `slots_only` (global)

- The backend contains **no runtime flow**: no commit ordering, gap handling,
  catch-up, build scheduling, version management, reader/build lock or thread
  pool, and no edits to `substrate/aux/**`. The only threads are those the
  placement requires (shipped worker: commit thread, read threads, main
  thread) and the pool of `ThreadBudget`.
- No `std::thread::hardware_concurrency`, fixed thread counts or caps
  (`pipeline/gates/run_thread_budget_lint.sh`).

### `scale_safe` (global)

- No 32-bit counts or offsets on rows, batches, images, partitions; 64-bit
  partition keys; 128-bit decimal cells in captured batches and no narrowing of
  engine numerics on the way to a module; capture without per-row heap allocation, once per statement
  and table; scans and builds parallel; no build, rebuild or catch-up inside a
  writer's commit.

### `fail_closed` (global)

- If the SDK / server / embedded host cannot start or be reached, the harness
  exits non-zero (`Fail`). No SKIP-then-PASS / "PASS (structural)".

### `host_glue` (layer `host`; skip if the workload does not require an in-process host)

- Real in-process registration exists (not a placeholder TU). Server-only
  symbols (registration, txn callbacks, table-AM / storage-AM, triggers, module
  `_init` entrypoints) belong only in the MODULE / extension — never in a host
  shared library that standalone harnesses link.
- Do **not** recompile the same stateful TUs into both the host lib and the
  MODULE while also linking the host lib (ODR / dual-singleton).
- If false: N/A — the auditor must not invent a host requirement.

### `query_registries` (global)

- The backend names no query: `scripts/check_backend_query_free.py --engine {engine}`
  passes (no `TpchQ<N>` / `tpch_q<N>` / `tpch/q<N>/` outside comments).
- Catalog, SQL entry points and runtime bindings are registries that queries
  fill from their own directories with static registrars; the SQL that creates
  functions is assembled from per-query fragments.
- The backend builds and loads with zero registered queries.

### `independence` (global)

- No reading/copying another engine's `backends/{other}/` tree.
- No dependence on generated query accelerators (`tpch` / `sec_edgar` / etc.)
  in layer harnesses.
- No reintroduction of a shared portable chunk IR.

---

## Implementation self-check (before finishing)

For each applicable rule record (in the agent transcript or a short comment
block at the top of the layer harness) **path + symbol** evidence. If you
cannot point to a call site that exercises the slot, the rule is not done.

## Architecture audit discipline

- Use **only** the `rule` ids listed above in `violations[].rule`.
- On fail: actionable `feedback_for_implementation` that maps 1:1 to those rules.
- On pass: empty feedback; no "nice to have" follow-ups as failures.
- Do **not** raise a new bar between rounds.
- Prefer "slot + harness call the real paths with value asserts" over demanding
  a redesign of already-correct internals.
