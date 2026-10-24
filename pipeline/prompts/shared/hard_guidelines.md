# Hard guidelines (all agent stages)

Non-negotiable in every stage.

You run unattended: nobody answers a question, and a turn that ends asking for files, scaffolds or permission changes nothing. A file this task names that does not exist yet is yours to create (the tree may start with none of the query's files). A backend API that looks unfamiliar is the one to use, through its `api/README.md`. "Blocked" is not a result: ship the work and make the gate pass.

## 1. Native base-table storage

Read **base tables** through **this engine's** (`{{engine_name}}`) native storage and scan APIs. Consume engine-local batches, slots or typed rebuild callbacks, never a portable chunk IR and never SQL-over-client scans on the hot path. An engine's native scan helpers are private to its backend (the **scan** role of the Backend API index; read-only to query runs).

Stateful modules may keep custom storage (typed in-process images, engine-local tables, shared-memory blobs). **Forbidden:** caching or materializing the final query result (the answer rows for a binding) and serving it back. Partial aggregates, indexes, eligibility bitmaps, intern tables and membership layouts are intermediates and allowed.

## 2. Fair benchmarks

- Time the full production query path and consume all results.
- No hidden warmup billed as latency, no production `getenv` shortcuts, no hardcoded answers.
- The fairness gate is authoritative. Do not game the harness.

## 3. Scale: SF3000

Iteration runs on SF1; the design target is SF3000 (~2e10 fact rows, ids beyond 2^32, sums beyond 2^63, all state in memory). A win at SF1 that blows up memory, scans a full domain or hardcodes an iteration cardinality is a failure. Size everything from bind-time estimates and table statistics (`scale_factor_contract.md`).

## 4. Generality and exactness

Fast is worthless if the answer is wrong. Spec-value parameters, spec-range data and SF1 sizes are test inputs, not assumptions.

- **Parameters.** Every template parameter is answered exactly for any value of its SQL type, including NULL (SQL semantics: usually an empty result, never a throw). Never enumerate or hardcode a parameter domain. Precompute for likely values as an optimization only; any other value takes a fast exact path (a bounded residual over compact state), never a per-call full rebuild.
  - Dates and intervals: the backend API's date helpers (role "value semantics"): any day, Feb 29, month-end plus interval; no month alignment, no assumed year range.
  - LIKE and strings: the backend API's LIKE/collation helpers: partial and mixed-case words, `_`, escapes, empty pattern.
  - Enumerated and numeric parameters: unknown value, two parameters equal, value absent from the data, out of the documented range, fractional.
  - Thresholds derived from other parameters compare in exact arithmetic, not doubles.
- **Data.** Out-of-range dates, unseen categories, off-range numbers, NULLs, and negative or huge keys give the SQL result (and writes of such rows maintain state correctly).
- **Numerics.** Decimals and money use integer-scaled or exact-decimal accumulation (128-bit accumulators; numeric text and division from the backend API's "value semantics" helpers), never `double`, never a 128-bit accumulator truncated to 64 bits. Match the engine's rounding and scale rules for division and AVG.
- **Key width.** Every key, id, offset, count and sum is sized for SF3000: 64-bit keys, > 4 GB heaps, 128-bit sums. Forbidden: 32-bit or bit-packed keys with a fixed field width, fixed dense caps, 32-bit blob offsets, hashing a key into a partition id that can collide. Keys may be negative or sparse. Failure classes and their checks: `scale_factor_contract.md`.
- **Acceptance evidence.** Beyond the seed binding, run the host's parameter/data differential when it exists (e.g. `eval/tests/test_<engine>_<bench>_params.cpp`): plain, `--data-perturb`, `--wide-keys`, `--negative-keys`. Where none exists, write equivalent cases (off-spec parameters incl. NULL, off-spec rows, shifted and negated keys) in the query's tests. Run the verification set of `scale_factor_contract.md`. A mismatch is a correctness failure, not a tolerance issue.

## Edit freedom

Edit the allowlisted implementation files freely; coordinated refactors are fine. Hardcoding answers, semantic shortcuts and gaming the benchmark are forbidden.
