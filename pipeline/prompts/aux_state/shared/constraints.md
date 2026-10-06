# Aux-state project constraints

See also `pipeline/prompts/shared/hard_guidelines.md`.

Multi-connection production path with **single-threaded per connection** where the harness requires it.

- Base tables: engine **native** storage/scan APIs only.
- **Persistent partitioned aux state is allowed**; custom aux storage is preferred.
- **Do not** store precomputed **final query results** as aux state. Intermediate structures (indexes, partial aggregates, eligibility, membership, encodings) are allowed; redesign freely when a read-path ceiling appears.
- State is **per-query** (`{{aux_function_name}}`); no cross-query state sharing required, but accelerators must remain correct when other queries' writes run concurrently (see `concurrency_contract.md`).
- **Fallback required**: when aux state is missing, stale, or txn-blocked, the base kernel (`{{base_function_name}}`) must produce correct results.
- Txn-safe invalidation and staged publication are mandatory.
- Irrelevant mutations must be **NoOp** — no epoch bump, no stale marks, no maintenance side effects.
- Read paths must be **bounded to the query window** — do not scan all partition slots / all groups globally when the query only needs a subset.
- Rebuild actions selected by the cost model must **resync/rebuild** state for later hits (not invalidate-only).
- **Maintenance must be fast**: optimize bulk refresh/`Rebuild*` and point/synthetic write apply; prefer delta-local work over full rebuilds when safe.
- **Exact for any parameter value / data / key width, compact, O(touched) writes** — see `hard_guidelines.md` §4.
- Do not alter query semantics for implementation convenience.
- Benchmark fairness: time the full production query path, consume all results.
- TPC-H aux: iteration on SF1; **read + maintenance must keep strong speedups at SF10/SF20+** (no SF1-only designs).

See `profiling_conventions.md` and `scale_factor_contract.md`.
