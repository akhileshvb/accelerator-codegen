# Fixed-operation shared-interface sweep

`shared_fixed_operation_sweep.csv` uses a deterministic virtual-time trace. Each
mode executes the same reads and writes for a given query and interval; query
latency does not change the number of operations. The DuckDB accelerated sink
materializes every output row, matching the base result-consumption work.

The Q10 comparison uses 2 reads at 2 virtual reads/second. The Q24 comparison
uses 8 reads at 2 virtual reads/second. Build and differential-check time are
outside the timed workload. Every measured run passed its engine-local
differential check; PostgreSQL Q24 compares the unordered tie-equivalent result
set because its SQL ordering does not specify a tie breaker.

The PostgreSQL base mode disables auxiliary triggers with
`session_replication_role = replica`; this isolates ordinary query/write cost
from auxiliary maintenance. The results are therefore apples-to-apples for
base versus shared within each engine, but not a claim about cross-engine
absolute latency.
