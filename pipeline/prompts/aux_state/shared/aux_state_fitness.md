# Aux-state fitness

Judge fit per query (`{{aux_function_name}}`) by signals, none mandatory. Partition keys may come from bind parameters, grouping attributes, join keys or other attributes. Design for this query only; note incidental reuse in `fitness_assessment.incidental_reuse`.

- For: stable partition grain with a clear mutation-to-partition map; reusable compact partial aggregate; bounded invalidation; plausible incremental maintenance (deltas or bounded rebuilds); read-heavy or mixed workload; a fallback that is correct while state is missing, stale or txn-blocked.
- Against: predicate-dominated filtering with little reuse; unbounded or global state needing raw data; high-cardinality partition space; frequent whole-state invalidation; no stable mutation-to-partition grain; semantics needing full re-execution (volatile UDFs, non-determinism); full-result cache; negligible speedup.
- Concurrent correctness is still required: classify irrelevant mutations as NoOp and bound reads to the query window (`concurrency_contract.md`).

Output `fitness_assessment`: `recommended`, `confidence`, `signals_for`, `signals_against`, `partition_key_source`, `rationale`. `recommended` is true: the fitness signals shape the design, they do not decline the query (`01_analysis.md`).
