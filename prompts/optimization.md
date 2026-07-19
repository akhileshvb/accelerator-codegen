# Prompt Template: Profile-Guided, Scale-Aware Optimization of a TPC-H Accelerator

Optimize the existing TPC-H **Q<QUERY_NUMBER>** accelerator in:

- `/home/akhilvb/accelerator-codegen`
- `/home/akhilvb/accelerator-codegen-duckdb`

The correctness-first baseline is already implemented and validated against DuckDB. This task is to improve its single-threaded performance while preserving correctness, benchmark fairness, native DuckDB storage, and scalability beyond SF1.

## Query and baseline context

TPC-H query:

```sql
<REFERENCE_SQL>

Current accelerator architecture:

<CURRENT_ARCHITECTURE>

Current validated performance:

Scale factor: SF<BASELINE_SCALE>
DuckDB median: <DUCKDB_MEDIAN>
Accelerator median: <ACCELERATOR_MEDIAN>
Speedup: <SPEEDUP>
Per-seed range: <RANGE>

Current phase profile:

<PHASE_PROFILE>

Current transient state:

<STATE_STRUCTURES_AND_MEMORY>

Observed vector layouts:

<LAYOUT_PROFILE>

Current correctness status:

<SEEDED_SYNTHETIC_TRANSACTION_AND_REGRESSION_STATUS>

Before changing code:

Inspect the current Q<QUERY_NUMBER> kernel, package, headers, tests, benchmark, and profiling instrumentation.
Inspect existing optimized accelerators for reusable techniques.
Reproduce the current baseline using the validated benchmark methodology.
Confirm the current bottleneck ranking with measured phase timings.
Audit the implementation for SF1-specific cardinality, capacity, key-range, reserve, and integer-width assumptions.
State the first optimization hypothesis and why the profile supports it.

Do not begin by implementing a large rewrite.

Hard constraints

The optimized implementation must continue to satisfy all of the following:

Single-threaded execution only.
Use DuckDB’s unchanged native storage and compression.
No persistent auxiliary state.
No materialized views.
No custom reordered, denormalized, or accelerator-specific copies of base tables.
No accelerator-specific indexes.
No state cached, precomputed, incrementally maintained, or reused across query executions.
Every derived structure must be created during the query execution and discarded afterward.
Query-local transient execution state is allowed.
DuckDB remains responsible for:
MVCC;
transaction-local inserts, updates, and deletes;
rollback;
native scans and scan-state advancement;
table-filter semantics;
compression and decompression;
vector and buffer lifetimes.
Preserve exact generic fallbacks for:
unexpected vector layouts;
nulls and nullable dictionary children;
transaction-local vectors;
unsupported encodings;
sparse, negative, or out-of-range keys.
Do not introduce multithreading.
Do not alter query semantics.
Do not modify benchmark methodology to favor the accelerator.
Do not hide work outside the timed query execution.
Do not optimize only for SF1.
Production behavior must not depend on inherited shell environment variables.

The final implementation must be correct and practical at SF1 and SF10, and must be width-safe and memory-audited for SF100 where feasible.

Optimization process

Use a strictly profile-guided, one-change-at-a-time process.

For each experiment:

Write down the hypothesis.
Identify the measured phase or hardware cost it targets.
Implement one bounded change.
Place the experiment behind an independent compile-time experimental guard or explicit benchmark option.
Run query-specific seeded, synthetic, schema, and transaction tests.
Run all existing cross-query regression tests.
Benchmark at SF1.
Benchmark at SF10 when the change affects state sizing, key lookup, initialization, memory layout, scan behavior, or cache locality.
Record per-seed latency, speedup, phase timings, state memory, and variance.
Retain the change only when the evidence supports it.
Re-profile after every retained change before selecting the next optimization.

Do not stack several unmeasured changes and attribute the combined result to all of them.

Retention criteria

A change may be folded into the production path only if:

all correctness tests pass;
the result is reproducible;
it improves the current production path across all or nearly all seeds;
any seed regressions are negligible and explicitly reported;
it does not introduce a meaningful SF1 regression;
it improves or preserves SF10 performance;
transient memory remains reasonable;
it does not create a pathological SF100 projection;
it does not make normal scaled keys use a slow overflow fallback;
it retains exact generic and transaction-safe fallback behavior.

A scale-safety fix may be retained without an immediate speedup when it prevents incorrectness, out-of-bounds access, overflow, or pathological fallback behavior at larger scale factors.

Phase 1: refine the profile

Use coarse phase timers and hardware profiling. Do not add per-row timers.

Break down the current implementation into query-appropriate phases.

Scan and decode

For each table and projected column, measure:

native scan time;
table-filter time where observable;
decompression/decode time;
DataChunk construction or handoff;
vector-layout conversion;
validity processing;
residual predicate evaluation.
Build-side processing

For each transient structure, measure:

allocation;
zero initialization;
reserve;
classification;
insertion;
hash computation;
rehashing;
duplicate handling;
populated cardinality;
logical domain size;
allocated capacity;
overflow/fallback cardinality.
Probe-side processing

Measure:

early predicate rejection;
lookup/probe time;
probe hits and misses;
arithmetic;
aggregate update;
memory access and cache behavior.
Finalization

Measure:

grouping;
sorting;
top-k;
state scan;
final arithmetic;
result construction and consumption.

Record:

FLAT, DICTIONARY, CONSTANT, and other observed layouts;
dictionary child layouts;
selected-child validity;
shared selection-vector frequency;
fast-path and fallback chunk counts;
rows surviving each predicate;
state cardinality;
allocated bytes;
peak RSS;
page faults;
cycles;
instructions;
cache misses;
branch misses;
allocation counts.

Rank bottlenecks by end-to-end impact.

Phase 2: scale-safety audit

Search the Q<QUERY_NUMBER> implementation for:

SF1 table-size constants;
fixed maximum keys;
hardcoded capacities;
hardcoded reserve values;
direct indexing based on SF1 domains;
arrays that do not grow;
EnsureCapacity functions that are not called on every relevant insertion path;
32-bit index or cardinality assumptions;
unchecked multiplication when calculating allocation bytes;
packed-key truncation or collisions;
full-domain clearing or final scans;
overflow maps that become the normal SF10 path;
nominal scale-factor assumptions instead of actual metadata or observed domains;
aggregate widths safe at SF1 but unsafe at SF10/SF100.

Produce an audit table:

file / function
assumption
SF1 behavior
SF10 risk
SF100 risk
correctness impact
performance impact
planned fix
Scale-aware state requirements

Any dense array, bitmap, or direct-index map must:

derive its size from actual metadata, observed key domains, or safe geometric growth;
use checked allocation arithmetic;
avoid out-of-bounds access;
have an exact sparse fallback for exceptional keys;
grow for normal SF10/SF100 keys rather than spilling them unnecessarily;
report logical size, allocated capacity, bytes, and fallback cardinality;
avoid silently ignoring negative or oversized keys.

Geometric growth is acceptable, but report capacity overshoot and initialization/copy cost.

Consider exact pre-sizing from trustworthy table metadata when it avoids expensive repeated growth and excessive over-allocation.

Do not allocate based solely on the nominal TPC-H scale-factor argument.

Phase 3: establish scale baselines

Before aggressive optimization, validate and benchmark the current production path at:

SF1;
SF10.

Use the same parameter seeds where TPC-H parameters are scale-independent.

For each scale factor, report:

DuckDB median
accelerator median
speedup
speedup range
MAD/CV
phase timings
state cardinality
state allocated capacity
state bytes
peak RSS
overflow/fallback cardinality

Do not combine SF1 and SF10 speedups into one geometric mean. Report each scale separately.

Project SF100 state memory and integer limits before claiming the design scales.

Phase 4: optimization handbook

Select only techniques supported by the measured profile.

A. Dense direct-index state

Use when:

keys are integer-valued;
the domain is bounded or moderately sparse;
hash build/probe dominates;
scaled memory is acceptable.

Possible representations:

uint8_t, uint16_t, int32_t, or int64_t arrays;
dense key-to-slot arrays;
presence plus payload arrays;
paged or segmented dense state;
exact overflow hash for exceptional keys.

Requirements:

no SF1-only fixed capacity;
checked growth;
scale-aware reserve;
absent sentinel cannot collide with valid payload;
no full-domain finalization scan unless measured and justified;
memory report at SF1, SF10, and projected SF100.

Prior successes:

Q3 dense customer and order state;
Q9 dense order-year and supplier state;
Q12 dense order-priority state;
Q14 dense promo classification.
B. Bitmaps and packed classification

Use when the state is set membership or a tiny classification.

Possible forms:

one-bit membership bitmap;
two-bit class array;
presence bitmap plus one-bit classification;
inline word updates.

Requirements:

correct word-count arithmetic;
correct grow path;
no normal scaled keys sent to a hash spill;
no duplicate-related semantic errors;
initialization cost measured;
segmented or lazy pages considered only when clearing becomes material.

Prior success:

Q4 order-key membership;
Q9 selected-part membership.

Do not assume the most compact representation is fastest.

C. Compact slot maps

Use when only a selective subset requires payload state.

Possible pattern:

dense key → compact slot index
compact slot arrays → hot payload
cold fields → separate arrays

Investigate:

hot/cold separation;
narrower slot fields;
structure-of-arrays layout;
dynamic reserve based on metadata or observed selectivity;
chunked allocation;
paged dense key-to-slot maps.

At larger scale factors, report:

slot cardinality;
key-map bytes;
slot bytes;
reserve waste;
reallocations;
cache behavior.

Prior success:

Q3 selective orders representation.

Do not revert to unordered_map without scaled evidence.

D. Dictionary-over-flat fast paths

Use when the layout profile shows DICTIONARY→FLAT or equivalent nested layouts.

Implement:

direct dictionary child access;
direct nested indexing;
per-child classification caches;
selected-entry validity checks;
no composed selection vector when avoidable;
generic fallback for unsupported nesting.

Possible cached classifications:

string predicate result;
group slot;
enum/category;
null classification;
small integer code.

Prior successes:

Q1;
Q3;
Q4;
Q12;
Q14.

Do not require globally all-valid dictionary children when only selected entries need validation.

E. Shared-selection fast paths

Use only when columns actually share selection vectors.

Measure:

pointer or identity equality;
chunk hit count;
rows covered;
end-to-end benefit.

Do not retain dead paths for layouts never observed unless they provide a clear generic benefit.

Prior result:

useful in some Q1/Q3 paths;
absent for Q12/Q14 SF1 layouts.
F. Flat/all-valid hot paths

Use when vectors are consistently FLAT and valid.

Implement a tight loop with:

direct typed pointers;
inlined predicate logic;
direct state probes;
local aggregates;
generic fallback.

Prior success:

Q9 lineitem.

Do not assume SF10 has identical layouts without measuring.

G. Predicate ordering and early rejection

Order predicates by measured cost and selectivity.

Goals:

reject rows before expensive probes;
avoid decoding or loading unnecessary columns when practical;
avoid arithmetic for rejected rows;
retain bulk access where sparse access would be more expensive.

Record survivor counts after every predicate.

Prior successes:

Q9 early selected-part rejection;
Q12 early shipmode classification.

Do not assume sparse late materialization helps. It repeatedly regressed in Q6 and other experiments.

H. Exact packed composite keys

Use only when the encoding is collision-free for all supported domains.

Requirements:

derive required bit widths;
check every operand before packing;
no truncation;
no signed-shift undefined behavior;
exact fallback for keys outside packable bounds;
document maximum supported domains;
validate boundary and collision tests at SF10/SF100 projections.

Prior success:

Q9 partsupp key.

A proper pair struct and equality comparator is preferable to unsafe packing.

I. Fixed-domain aggregation

Use when the group domain is tiny or statically bounded.

Possible forms:

fixed arrays;
direct enum-to-slot mapping;
local scalar accumulators;
compact dense nation/year tables;
fixed top-k heap.

Requirements:

generic fallback where the domain is not guaranteed;
exact output ordering;
exact null semantics;
safe aggregate widths.

Prior successes:

Q1;
Q4;
Q9;
Q12;
Q14.
J. Exact arithmetic optimization

Profile arithmetic only after lookup, layout, and decode overhead are reduced.

Possible techniques:

scaled-integer arithmetic;
widening before multiplication;
narrow chunk-local partials with proven bounds;
exact wide merge;
eliminate redundant scale conversion;
reuse loaded values;
direct date integer comparisons.

Requirements:

exact DuckDB semantics;
no aggregate overflow at SF100;
bit-exact or value-exact final comparison as appropriate;
explicit range proof or checked fallback.

Prior successes:

exact integer revenue/profit paths;
Q1 narrow partials with wide merge.

Do not weaken DECIMAL semantics because the output is later converted to DOUBLE.

K. Base-vector-only residual skipping

Use only when the scan/runtime interface can prove that a base-storage vector has passed the pushed predicate.

Requirements:

residual checks retained for transaction-local and uncertain vectors;
counters for skipped and executed checks;
boundary-crossing transaction tests;
default off until all correctness tests and scaled benchmarks pass.

Prior success:

Q3.

Prior neutral result:

Q12 and Q14 residual skips were correct but not consistently faster.
L. Initialization and growth optimization

At SF10/SF100, allocation and clearing may become material.

Measure separately:

allocation;
zero fill;
geometric realloc/copy;
reserve;
page faults.

Possible techniques:

metadata-based pre-sizing;
geometric growth;
narrower state;
segmented allocation;
lazy pages;
epoch tags;
reuse within local query phases only when semantics and lifetime permit.

Do not add complexity unless initialization is a measured bottleneck.

Be careful: query-local state may not persist across query executions.

M. Alternate join orders and multi-pass plans

Consider only after modeling:

additional scans;
selectivity;
state reduction;
decode cost;
transaction handling.

Possible cases:

fact-side filtering before building a large dimension-side state;
collecting relevant keys;
deferred aggregation.

Do not retain because it processes fewer logical rows. Extra scans and sparse structures often negate the benefit.

Prior failures or non-starters:

Q4/Q6 multi-pass and sparse approaches;
Q9/Q12/Q14 alternate joins not justified after dense state became cheap.
N. Scan-consumer or aligned projected scans

Attempt only if profiling shows measurable cost from:

DataChunk construction;
slicing;
vector conversion;
handoff between scan and kernel;
inability to align related projected columns.

A useful scan-consumer must enable a concrete fused operation, such as:

native filtering
→ aligned projected decode
→ direct predicate
→ direct state probe
→ aggregate update

Requirements:

DuckDB keeps ownership of MVCC, scan state, filtering, and lifetimes;
bulk vector callback, not per-row callback;
transaction-safe fallback;
independently toggled;
end-to-end benchmark win at SF1 and SF10.

Prior results:

strong Q1 win;
Q4 consumer was neutral/slower.

Do not add one merely to eliminate a DataChunk.

O. Format-specific decode

Attempt only after ordinary vector paths are efficient and decode is the dominant floor.

Requirements:

use DuckDB’s existing storage formats;
do not change persisted encoding;
preserve MVCC and update fallbacks;
support only measured dominant formats;
retain generic fallback;
prove end-to-end benefit at SF1 and SF10.

Prior results:

small Q6 bitpacking opportunity;
Q4 paired decode did not improve full-query time.
P. Physical operator

Do not start here.

A physical operator is justified only if the existing interface blocks a measured optimization such as:

scan/probe/aggregate pipeline fusion;
aligned decode;
removal of material intermediate state;
pipeline-local state unavailable through the package API.

It is not justified when:

function dispatch is negligible;
finalization is negligible;
decode or memory bandwidth dominates;
a scan-consumer or vector fast path can express the same operation.
Techniques that frequently failed or regressed

Treat the following as negative prior evidence:

generic unordered_map/unordered_set for dense bounded domains;
SF1-hardcoded capacity;
normal scaled keys using an overflow hash;
generic UnifiedVectorFormat in the dominant hot path;
composed selection vectors;
full-domain finalization scans;
row-ID late materialization;
sparse selected callbacks;
candidate-aware sparse filtering;
duplicate-skipping branches;
run-at-a-time tracking;
bitmap word batching;
two-pass predicate/update loops;
manual unrolling without evidence;
branchless variants with extra memory touches;
extra aggregate banks;
global trust in pushed filters;
scan-consumer fusion without a measured handoff bottleneck;
unsafe packed keys;
format-specific decoding without an end-to-end win;
premature physical operators;
per-row timing instrumentation;
ambient runtime flags controlling the production path.

These are not absolute prohibitions. Retry only when Q<QUERY_NUMBER> has a materially different measured profile.

Correctness validation

After every meaningful change, run:

Seeded differential tests

At least:

20 seeds at SF1;
5 or more seeds at SF10 during iteration;
expand SF10 coverage for the final retained path.

Compare exactly:

schema;
column names;
logical types;
row count;
ordering;
nullness;
DECIMAL values;
floating-point behavior;
duplicate semantics.
Synthetic tests

Include query-specific semantic boundaries and scale boundaries:

keys beyond former SF1 capacities;
keys forcing geometric growth;
keys using the sparse fallback;
negative or invalid keys where supported;
packed-key boundary values;
null dictionary children;
unexpected vector layouts;
lower/upper predicate boundaries;
empty results;
duplicate joins;
missing joins;
aggregate overflow stress;
large date and numeric values.
Transaction tests

Cover:

transaction-local insert;
update;
delete;
join-key change;
predicate entry/exit;
rollback;
commit visibility;
base-vector versus transaction-local residual handling;
keys beyond initial dense capacity.
Regressions

Run every implemented accelerator suite and shared runtime/registration test:

<ALL_EXISTING_QUERY_TESTS>
Benchmark methodology

Use the validated apples-to-apples harness.

Required:

clean Release build;
-O3 -DNDEBUG;
same libduckdb.so;
same database and parameters;
PRAGMA threads=1 verified;
correctness gate before timing;
full connection.Query() and typed result consumption;
alternating DuckDB/accelerator order;
inherited ACCEL_* and DuckDB experimental variables cleared;
no persistent auxiliary state;
no benchmark-only cached state.
SF1

Use:

20 seeds when available;
3 warmups;
10 measured iterations;
two independent sessions for the final result.
SF10

During development, use:

at least 5 representative seeds;
3 warmups;
5 measured iterations.

For the final retained path, increase seeds and iterations where runtime permits and run a second session.

Report separately for SF1 and SF10:

query
seed
DuckDB median
accelerator median
speedup
MAD
CV
state bytes
peak RSS
overflow/fallback cardinality

Also report:

per-query geometric-mean speedup;
arithmetic mean latency;
speedup range;
overall geomean across implemented queries, if using the shared suite;
session-to-session drift.

Do not average SF1 and SF10 into one headline.

SF100 readiness

After SF1 and SF10 validation:

Calculate projected state sizes using actual capacity formulas.
Verify index, key, allocation, and aggregate widths.
Identify geometric-capacity overshoot.
Estimate initialization and clearing costs.
Check packed-key safety.
Determine feasibility on the current machine.
Run SF100 when resources permit, or run a representative stress subset.

Distinguish clearly between:

code is width-safe;
projected memory is feasible;
correctness was tested;
performance was measured.

Do not claim SF100 performance from projection alone.

Experimental control and production cleanup

During optimization:

keep each experiment independently controllable;
prefer compile-time ACCEL_EXPERIMENTAL_Q<QUERY_NUMBER>_OPTS;
avoid ambient runtime behavior in production;
record the exact configuration in result artifacts.

After selecting the best path:

Fold retained optimizations into one clear production implementation.
Remove duplicated obsolete hot loops.
Remove misleading no-op flags and labels.
Preserve generic correctness and transaction fallbacks.
Keep useful ablations only under explicit experimental compile-time guards.
Ensure benchmark behavior is deterministic without environment variables.
Rerun SF1 and SF10 after cleanup.
Confirm cleanup does not alter correctness or performance materially.
Required final report
1. Starting point
baseline architecture
baseline SF1 timing
baseline SF10 timing
baseline state
baseline bottlenecks
2. Scale-safety audit

For each issue:

file/function
former assumption
risk
fix
tests
SF1 effect
SF10 effect
SF100 implication
3. Experiment log

For every attempted optimization:

hypothesis
toggle/guard
targeted phase
SF1 result
SF10 result
correctness result
memory result
retained or reverted
reason
4. Cumulative performance
step
DuckDB median
accelerator median
speedup
phase change
state bytes

Report separately for SF1 and SF10.

5. Final phase profile
phase
SF1 time
SF10 time
percentage of accelerator time
rows
survivors
fast-path hits
fallback hits
6. State and memory
structure
representation
logical cardinality
allocated capacity
SF1 bytes
SF10 bytes
SF100 projection
initialization time
build time
probe time
fallback cardinality

Include process peak RSS and page-fault behavior.

7. Correctness

Report:

SF1 seeded tests;
SF10 seeded tests;
synthetic boundary tests;
transaction tests;
cross-query regressions;
failures found and fixed.
8. Final validated result
SF1 DuckDB median:
SF1 accelerator median:
SF1 speedup:

SF10 DuckDB median:
SF10 accelerator median:
SF10 speedup:

SF1 per-seed range:
SF10 per-seed range:

session reproducibility:
9. Remaining bottleneck

Rank the remaining costs and state whether any of the following is now justified:

different transient representation;
scan-consumer;
format-specific decode;
alternate join order;
physical operator;
multithreading as future work.
10. Production path

State:

final active algorithm;
retained optimizations;
generic fallbacks;
experimental guards retained;
files modified;
whether cleanup changed performance.

The final result must be an auditable, profile-guided accelerator that remains single-threaded, uses native DuckDB storage, has no persistent auxiliary state, preserves exact correctness, and performs well at both SF1 and SF10 without relying on SF1-specific capacities or assumptions.