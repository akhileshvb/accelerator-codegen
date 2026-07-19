# Prompt Template: Initial Construction and Validation of a TPC-H Accelerator

Implement a correctness-first accelerator for TPC-H query **Q<QUERY_NUMBER>** in:

- `/home/akhilvb/accelerator-codegen`
- `/home/akhilvb/accelerator-codegen-duckdb`

This is the initial implementation for this query. Build the accelerator, package/runtime wiring, correctness suite, and benchmark harness from scratch, following the conventions already established by the existing implemented accelerators.

## Query definition

TPC-H query:

```sql
<REFERENCE_SQL>

Parameters:

<QUERY_PARAMETERS>

Expected output schema:

<OUTPUT_SCHEMA>

Before implementation:

Inspect the existing accelerators most relevant to this query for:
multi-table join state;
scan/filter handling;
vector-layout handling;
exact arithmetic;
aggregation and output production;
package registration;
runtime dispatch;
test organization;
benchmark methodology.
Identify the logical query plan and the simplest query-local execution strategy.
Briefly state:
the proposed architecture;
the transient structures that will be built;
the table scan order;
the exact files to add or modify.
Hard project constraints

The implementation must satisfy all of the following:

Single-threaded execution only.
Use DuckDB’s unchanged native storage and compression.
No persistent auxiliary state.
No materialized views.
No custom reordered or denormalized copies of base tables.
No accelerator-specific indexes.
No state cached or reused across query executions.
Every derived structure must be rebuilt within the query execution and discarded afterward.
Query-local transient execution state is allowed.
DuckDB remains responsible for:
MVCC;
transaction-local inserts, updates, and deletes;
rollback;
native scan state;
table filters;
compression and decompression;
vector and buffer lifetimes.
Preserve generic correctness fallbacks for:
unexpected vector layouts;
nulls;
transaction-local vectors;
unsupported encodings;
keys outside an expected dense domain.
Do not introduce multithreading.
Do not introduce a physical operator, custom decoder, scan-consumer path, SIMD implementation, or aggressive specialization in the first correctness milestone.
Do not optimize toward a target speedup in the initial pass.
Do not change the query semantics to make the accelerator easier to implement.

The first milestone is a complete, auditable, trustworthy baseline over native DuckDB storage.

Recommended implementation strategy

Use the simplest exact execution architecture appropriate for the query.

Input scans

For each referenced table:

<TABLE_1>
- Required columns: <COLUMNS>
- Predicates: <PREDICATES>
- Transient state produced: <STATE>

<TABLE_2>
- Required columns: <COLUMNS>
- Predicates: <PREDICATES>
- Transient state produced or consumed: <STATE>

Add more tables as needed.

Use normal DuckDB chunk scans for the baseline.

Use safe generic UnifiedVectorFormat handling initially.

Native table-filter pushdown is allowed where appropriate, but retain residual checks when required for:

transaction-local rows;
updates;
exact boundary behavior;
null semantics;
any predicate not guaranteed by the scan interface.
Query-local state

Suggested baseline state:

<TRANSIENT_STATE_DESCRIPTION>

A straightforward exact implementation using structures such as the following is acceptable:

unordered_map;
unordered_set;
vectors;
fixed-size aggregate arrays;
exact composite-key structs;
DuckDB scalar/value types.

Do not persist or reuse this state between executions.

Do not hardcode SF1 cardinalities or maximum keys. Any capacity assumptions must have:

checked bounds;
an exact fallback;
no out-of-bounds access;
no silently dropped rows.
Join semantics

Implement all joins exactly:

<JOIN_CONDITIONS>

Requirements:

no missing matches;
no duplicated contributions;
correct handling of missing keys;
correct null behavior;
exact duplicate semantics;
exact transaction-local visibility.
Predicate semantics

Implement every predicate exactly:

<PREDICATES>

Be especially careful with:

strict versus inclusive inequalities;
half-open date ranges;
interval arithmetic;
string case sensitivity;
prefix/substring matching;
null semantics;
parameter equality or overlap;
DECIMAL comparisons.
Arithmetic and aggregation

Implement:

<ARITHMETIC_AND_AGGREGATION>

Requirements:

preserve DuckDB’s exact input and output types;
use exact scaled-integer or DuckDB DECIMAL arithmetic where required;
widen before multiplication or accumulation when necessary;
prevent intermediate overflow;
preserve null, empty-input, zero-denominator, and rounding behavior;
preserve ordering and grouping semantics.
Finalization

Produce exactly:

<OUTPUT_DESCRIPTION>

Match DuckDB on:

row count;
column names;
logical types;
nullness;
ordering;
DECIMAL scale;
floating-point behavior where applicable.
Required infrastructure

Add all query-specific infrastructure needed for a complete implementation.

Expected files and wiring include, adapted to the repository’s conventions:

include/accelerator_codegen/tpch_q<QUERY_NUMBER>.hpp

src/generated/tpch/q<QUERY_NUMBER>_package.cpp
src/generated/tpch/q<QUERY_NUMBER>_kernel.cpp

tests/test_support/q<QUERY_NUMBER>_manifest.hpp
tests/test_tpch_q<QUERY_NUMBER>.cpp

benchmarks/tpch/benchmark_q<QUERY_NUMBER>.cpp
benchmarks/tpch/results/q<QUERY_NUMBER>_baseline.csv

Also update as needed:

src/register.cpp
CMakeLists.txt
shared runtime/package registration
validated benchmark integration

Expose a SQL-callable function following the established naming convention, for example:

SELECT *
FROM accel_tpch_q<QUERY_NUMBER>(<PARAMETERS>);

Do not alter existing query behavior unless a shared correctness fix is necessary.

Seeded differential correctness

Test all available generated parameter sets for this query.

For every seed:

Run the reference DuckDB SQL.
Run the accelerator.
Use the same database and connection state.
Compare the complete result exactly.

Compare:

schema;
column names;
logical types;
row count;
nullness;
values;
ordering;
DECIMAL representation;
floating-point behavior, using exact or bit-level comparison where required.

Fail immediately on mismatch.

Use at least:

20 seeded parameter instances

when available.

Synthetic correctness tests

Add focused synthetic tests covering the query’s important semantic boundaries.

Include, as applicable:

no qualifying rows;
exactly one qualifying row;
multiple qualifying rows;
duplicate join matches;
missing join matches;
multiple fact rows referencing one dimension row;
null join keys;
null predicate columns;
null arithmetic inputs;
null grouping keys;
parameter values that are equal;
empty groups;
lower-bound equality;
upper-bound equality;
values just inside and outside each range;
strict inequality equality cases;
string prefix matches;
strings containing but not starting with a prefix;
case sensitivity;
zero aggregate values;
negative or unusual exact numeric values;
empty-input finalization;
zero denominators;
ordering ties;
maximum and minimum valid key values;
keys beyond any former SF1-sized capacity.

Customize these tests to the actual query rather than adding irrelevant cases.

Transaction correctness tests

Add tests covering transaction-visible changes to every participating table.

Include, as applicable:

inserts that enter the result;
inserts that do not enter the result;
updates that move a row into a predicate;
updates that move a row out of a predicate;
join-key updates;
updates that change aggregate classification;
deletes;
transaction-local inserts;
transaction-local updates;
transaction-local deletes;
rollback;
commit visibility on the next execution;
updates crossing lower and upper date bounds;
updates changing string classifications;
updates changing numeric contributions.

Run DuckDB SQL and the accelerator inside the same transaction state and compare exact results.

Do not assume pushed base-table filters automatically handle transaction-local updates correctly. Retain residual checks where necessary.

Schema and registration tests

Test:

SQL function registration;
bind-time parameter validation;
invalid parameter counts;
invalid parameter types;
output schema;
output column names;
repeated invocation;
invocation from separate connections;
empty databases or tables where supported.
Regression testing

After implementing the new accelerator, rerun all existing accelerator tests:

<EXISTING_QUERY_TESTS>

At minimum, run every currently implemented TPC-H accelerator suite and any shared runtime/registration tests.

Any shared runtime change must be justified and must not regress existing queries.

Instrumentation

Add coarse phase instrumentation without per-row timers.

Record phase times appropriate to the query.

Build-side phases

For each transient structure:

native scan/decode time;
predicate or classification time;
state insertion time;
allocation and initialization time;
rows scanned;
rows retained;
state cardinality;
estimated allocated bytes;
overflow/fallback cardinality.
Probe/fact-side phases

Record:

native scan/decode time;
pushed-filter time where available;
residual-predicate time;
vector conversion time;
join-probe time;
arithmetic time;
aggregate-update time;
rows scanned;
survivors after each predicate;
join hits and misses.
Finalization

Record:

sorting;
top-k or grouping finalization;
output construction;
final arithmetic;
emitted row count.

Also report observed vector layouts:

FLAT;
DICTIONARY;
CONSTANT;
dictionary-child layout;
validity behavior;
shared selection-vector frequency;
generic fallback frequency.

Do not distort benchmark timings with verbose instrumentation. Use a benchmark/profile mode or coarse counters.

Fair benchmark methodology

Benchmark the reference SQL and accelerator under identical conditions.

Required setup:

clean Release build;
-O3 -DNDEBUG;
same libduckdb.so;
same database;
same query parameters;
same connection settings;
PRAGMA threads=1, explicitly verified;
inherited accelerator and DuckDB experimental environment variables cleared;
no auxiliary-state path enabled;
no experimental storage decoder enabled unless explicitly part of a separately labeled experiment.

For each seed:

Run differential correctness before timing.
Perform 3 warmup executions.
Perform 10 measured executions.
Alternate execution order between DuckDB and accelerator.
Time full connection.Query().
Fully consume typed results on both paths.
Compute the median latency.
Compute MAD and/or CV.
Compute speedup as:
duckdb_median / accelerator_median

Report:

per-seed DuckDB latency;
per-seed accelerator latency;
per-seed speedup;
arithmetic mean latency;
geometric-mean speedup;
speedup range;
MAD/CV;
phase timing;
state memory.

Run a second independent benchmark session when practical.

Report the accelerator honestly even if it is slower than DuckDB.

Baseline implementation rules

For this initial pass:

favor simple exact code;
retain generic vector handling;
use straightforward query-local maps or arrays;
do not prematurely specialize for observed SF1 layouts;
do not add scan-consumer fusion;
do not add a physical operator;
do not use custom format-specific decode;
do not use persistent state;
do not hide performance behind a benchmark-only path;
do not skip correctness cases because they are absent from standard TPC-H data.

Keep any optional baseline experiment behind a clear default-off compile-time experimental guard.

Production behavior must not depend on ambient shell environment variables.

Required final report

Provide a complete report containing:

Architecture
logical execution plan;
scan order;
transient state;
joins;
predicates;
aggregation;
finalization.
Files
files added;
files modified;
any shared runtime changes.
Correctness
seeded differential results;
synthetic results;
transaction results;
schema/registration results;
cross-query regressions.
Performance
DuckDB median:
Accelerator median:
Speedup:
Per-seed speedup range:
MAD/CV:
Phase profile
phase
time
rows processed
survivors
state cardinality
state bytes
Layout profile
dominant vector layouts;
fast-path or fallback usage;
null/validity behavior;
shared selections.
State
query-local structures;
cardinality;
allocated bytes;
initialization cost;
overflow/fallback usage.
Bottleneck ranking

Rank the initial bottlenecks using measured time.

Recommended next optimizations

Recommend only optimizations supported by the profile, such as:

dense direct-index state;
bitmap membership;
dictionary-over-flat fast paths;
per-child classification caches;
fixed-slot aggregation;
packed exact composite keys;
reordered predicates;
base-vector-only residual skipping;
scan-consumer work;
format-specific decode.

Do not implement those aggressive optimizations in this initial milestone unless required for correctness.

The final result must be a complete, reproducible, correctness-first accelerator validated directly against DuckDB, using native DuckDB storage, one thread, and only transient query-local state rebuilt on every execution.