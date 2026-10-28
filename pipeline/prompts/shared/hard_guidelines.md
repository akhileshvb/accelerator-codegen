# Hard guidelines (all agent stages)

Non-negotiable. You run unattended: nobody answers questions, and a turn that ends asking for files, scaffolds or permissions changes nothing. A file this task names that does not exist yet is yours to create (the tree may start without the query's files). An unfamiliar backend API is the one to use, through its `api/README.md`. "Blocked" is not a result: ship the work and make the gate pass.

## 1. Native base-table storage

Read base tables through this engine's (`{{engine_name}}`) native storage and scan APIs: engine-local batches, slots or typed rebuild callbacks; never a portable chunk IR, never SQL-over-client scans on the hot path. Native scan helpers are private to the backend (the **scan** role; read-only to query runs).

Stateful modules may keep custom storage (typed in-process images, engine-local tables, shared-memory blobs). Forbidden: caching or materializing the final query result (the answer rows for a binding) and serving it back. Partial aggregates, indexes, eligibility bitmaps, intern tables and membership layouts are allowed intermediates.

## 2. Fair benchmarks

Time the full production query path and consume all results. No hidden warmup billed as latency, no production `getenv` shortcuts, no hardcoded answers. The fairness gate is authoritative; do not game the harness.

## 3. Scale: SF3000

Iteration runs on SF1; the target is SF3000 (~2e10 fact rows, ids beyond 2^32, sums beyond 2^63, all state in memory). A win at SF1 that blows up memory, scans a full domain or hardcodes an iteration cardinality is a failure. Size from bind-time estimates and table statistics (`scale_factor_contract.md`).

## 4. Generality and exactness

Fast is worthless if the answer is wrong. Spec-value parameters, spec-range data and SF1 sizes are test inputs, not assumptions.

- **Parameters.** Every template parameter is answered exactly for any value of its SQL type, including NULL (SQL semantics: usually an empty result, never a throw). Never enumerate or hardcode a parameter domain. Precomputing likely values is an optimization only; any other value takes a fast exact path (a bounded residual over compact state), never a per-call full rebuild.
  - Dates and intervals: the backend API's date helpers (role "value semantics"): any day, Feb 29, month-end plus interval; no month alignment, no assumed year range.
  - LIKE and strings: the backend API's LIKE/collation helpers: partial and mixed-case words, `_`, escapes, empty pattern.
  - Enumerated and numeric parameters: unknown value, two parameters equal, value absent from the data, out of documented range, fractional.
  - Thresholds derived from other parameters compare in exact arithmetic, not doubles.
- **Data.** Out-of-range dates, unseen categories, off-range numbers, NULLs, negative or huge keys give the SQL result; writes of such rows maintain state correctly.
- **Numerics.** Every aggregate and expression gives the engine's result for every value its column type admits, not only the values in the data; the result type, rounding and overflow rules come from the engine (the "value semantics" helpers).
  - Exact types (integers, decimals, money): exact integer-scaled or decimal accumulation, never `double`. Accumulators are wide enough that no SF3000 sum overflows (128-bit, never truncated to 64); division and AVG follow the engine's scale and rounding.
  - Floating types (`DOUBLE`, `FLOAT`, `REAL`): SQL leaves the summation order open, so the differential compares floating results within a relative 1e-9 and accepts reordering among floating ties. Maintain SUM/AVG by delta (add inserted values, subtract deleted ones) in a compensated (double-double) accumulator so cancellation stays within that tolerance; never keep raw values only to recompute them. While a group's sum of absolute values stays below half the type's maximum no order overflows; keep that bound per group. NaN and ±inf inputs are counted per group and finalized by IEEE rules (NaN if any NaN or both infinities, else the infinity). Floating keys group and compare as the engine does (±0, NaN).
  - NULLs: aggregates skip NULL inputs, an aggregate over no non-NULL input is NULL (COUNT is 0), and NULL keys form one group; keep per-group non-NULL counts rather than inferring them from a sum.
  - A value the state cannot reproduce exactly (an overflow bound crossed, a type the helpers lack) is a local fallback for the bindings it can affect, detected from maintained state (a flag or count, not a scan), never a reason to decline the query.
- **Key width.** Every key, id, offset, count and sum is sized for SF3000: 64-bit keys, > 4 GB heaps, 128-bit sums. Forbidden: 32-bit or bit-packed keys with a fixed field width, fixed dense caps, 32-bit blob offsets, hashing a key into a partition id that can collide. Keys may be negative or sparse. Failure classes and checks: `scale_factor_contract.md`.
- **Acceptance evidence.** Beyond the seed binding, run the host's parameter/data differential when it exists (e.g. `eval/tests/test_<engine>_<bench>_params.cpp`): plain, `--data-perturb`, `--wide-keys`, `--negative-keys`. Where none exists, write equivalent cases (off-spec parameters incl. NULL, off-spec rows, shifted and negated keys) in the query's tests. Run the verification set of `scale_factor_contract.md`. A mismatch is a correctness failure, not a tolerance issue.

## Edit freedom

Edit the allowlisted implementation files freely; coordinated refactors are fine. Hardcoding answers, semantic shortcuts and gaming the benchmark are forbidden.
