# The per-query test suite: format

One suite per query, in the query's `tests/workload/` directory (the format the Verus pipeline's tests use too). Everything is declarative: no code, portable SQL only, and every file is read by the engine's harness through the host contract (`ACCEL_SUITE_*`).

| File | Content |
|---|---|
| `params.sql` | Fixed parameter tuples, one per line, SQL literals in the query's `$n` order (`'open', DATE '2024-03-15'`). Comment lines start with `--`. |
| `domain.yaml` | The parameter domain; every run draws fresh random tuples from it. |
| `txns/NNN.sql` | Write transactions, applied **in order, committed**: each runs on the state the earlier ones left, so choose rows by subquery, never by hard-coded id. A first line `-- rollback` makes it applied then aborted (the accelerator must see nothing). |
| `write_mix.yaml` | How benchmarks mix writes in: the write-frequency range and the write kinds' weights. |
| `suite.yaml` | The manifest: what each transaction is, which tuples cover which edge class, scale scenarios, bulk and point minimums. |
| `setup.sql` | Optional. Shapes the data before the accelerator is built (schema-valid rows only). |

`domain.yaml`: one entry per query parameter, in `$n` order (`name` = the workload's parameter name; a trailing parameter the accelerator derives, such as an interval's end date, is `type: derived`). A derived parameter is not independent: the accelerator receives only its source, so in every fixed tuple it must equal its derivation (validation checks it); put edge cases (NULL, extremes, empty ranges) on the source parameter. Reversed or equal bounds are edge cases only when both bounds are parameters:

```yaml
version: 1
params:
  - {name: status,      type: choice,  values: ["'open'", "'closed'", "'pending'"]}   # SQL literals
  - {name: cutoff_date, type: date,    min: 2020-01-02, max: 2024-12-01}
  - {name: count,       type: int,     min: 1, max: 50}
  - {name: rate,        type: decimal, min: 0.0, max: 0.10, scale: 2}
  - {name: tag,         type: pattern, template: "%{}%", words: [green, red, blue]}
  - {name: end_date,       type: derived, from: cutoff_date, add_months: 3}
```

`write_mix.yaml`:

```yaml
version: 1
frequency_pct: {min: 1, max: 50, samples: 3, distribution: log}   # % of operations that are writes; 0% is always added
kinds: {point_update: 5, point_insert: 2, point_delete: 2, bulk_insert: 1}   # relative weights; names are `kind`s of suite.yaml txns
```

`suite.yaml` (the sections marked *stateful* only for queries with writes):

```yaml
version: 1
query: 1
rationale: "2-5 sentences tying the suite to the analysis: tables, predicates, why these tuples and writes"
edge_coverage:            # class -> 1-based tuple numbers of params.sql (comments not counted), or "n/a: why"
  in_spec: [1, 2, 3]            # values the benchmark's own generator draws
  off_spec: [10, 11]            # valid values outside it (unknown enum, wrong case, partial LIKE pattern, far dates)
  null_param: "n/a: the SQL's = / < comparisons make a NULL parameter an empty answer, covered by tuple 12"   # NULL where the SQL allows it
  empty_result: [12]            # tuples whose answer is empty (checked: they return no rows)
  limit_ties: [4]               # ties at the LIMIT cut
  extreme_values: [13, 14]      # extreme dates / amounts / sizes
  large_keys: "n/a: no key parameter"   # keys beyond 2^32, where a type allows
txns:                     # stateful: every file of txns/, with its kind
  "001.sql": {kind: point_update}   # point_insert|point_update|point_delete|bulk_insert|bulk_update|bulk_delete|sequence|rollback|key_move|other
not_applicable: {"accounts:DELETE": "account rows reach the answer only through ... which a delete cannot change"}   # stateful: waivers, each with its reason
point_writes: {min_count: 3, max_probes: 8, tables: [events]}   # stateful
bulk_writes: {required: true, min_chunks: 2}                    # stateful: refresh-shaped chunks (many rows, related tables together) when they matter
scale_scenarios: ["one per applicable failure class of scale_factor_contract.md: the write or database that exercises it"]   # stateful
```

## Worked example: a complete minimal suite

Toy query `SELECT kind, count(*) FROM events WHERE ts >= $1 AND ts < $2 AND status = $3 GROUP BY 1 ORDER BY 1` (columns `event_id`, `account_id`, `kind`, `status`, `ts`; dependent table `events`, parameters in `$n` order). The directory is `accelerators/<bench>/q<N>/tests/workload/` and holds exactly:

`params.sql` (one tuple per line, SQL literals; `NULL` is a literal; tuple numbers in `suite.yaml` count the non-comment lines from 1):

```sql
-- in-spec
DATE '2024-01-01', DATE '2024-04-01', 'open'
DATE '2023-07-01', DATE '2023-10-01', 'closed'
-- off-spec: unknown status, reversed bounds, equal bounds
DATE '2024-01-01', DATE '2024-04-01', 'x'
DATE '2024-04-01', DATE '2024-01-01', 'open'
DATE '2024-01-01', DATE '2024-01-01', 'open'
-- NULL, far dates
NULL, DATE '2024-04-01', 'open'
DATE '1900-01-01', DATE '2100-01-01', 'pending'
```

`txns/001.sql` ... (names sort in application order; one file = one transaction of `;`-separated statements, committed; later files see earlier effects; target rows by subquery, never by id; each statement is plain INSERT / UPDATE / DELETE):

```sql
-- 001.sql  point_update: moves one counted event to another kind
UPDATE events SET kind = 'alert' WHERE event_id = (SELECT min(event_id) FROM events WHERE ts >= DATE '2024-01-01' AND ts < DATE '2024-04-01' AND status = 'open' AND kind <> 'alert');
```

```sql
-- rollback
-- 002.sql: the first line is exactly the marker above; the transaction is applied, then aborted
DELETE FROM events WHERE event_id = (SELECT min(event_id) FROM events WHERE status = 'open');
```

An INSERT copies a visible row by subquery (`INSERT INTO events SELECT event_id + 1000000000, account_id, ... FROM events WHERE ... LIMIT 1`); a bulk chunk is one file with many rows (`INSERT INTO t SELECT ... FROM t WHERE <slice> `); a duplicate key is an INSERT of an existing key, and its undo a DELETE of the later copy.

`domain.yaml` for the toy (both bounds are parameters, so reversed and equal bounds are valid tuples): `start_ts` and `end_ts` as `type: date`, `status` as `type: choice`. `write_mix.yaml`: as above. `suite.yaml` for the toy:

```yaml
version: 1
query: 1
rationale: "events is the only dependent table; the tuples cover in-spec, off-spec status, reversed/equal bounds, NULL and far dates; the writes move counted rows across groups, remove and re-add them, and duplicate a key."
edge_coverage: {in_spec: [1, 2], off_spec: [3, 4, 5], null_param: [6], empty_result: [3, 4, 5], limit_ties: "n/a: the query has no LIMIT", extreme_values: [7], large_keys: "n/a: no key parameter"}
txns: {"001.sql": {kind: point_update}, "002.sql": {kind: rollback}, "003.sql": {kind: point_insert}, "004.sql": {kind: point_delete}, "005.sql": {kind: key_move}, "006.sql": {kind: bulk_insert}}
not_applicable: {}
point_writes: {min_count: 3, max_probes: 8, tables: [events]}
bulk_writes: {required: true, min_chunks: 2}
scale_scenarios: ["one entry per applicable failure class of scale_factor_contract.md: the write or database that exercises it"]
```

## How to validate (do not build or run the harness yourself)

Write the whole suite first (every file above), then call `request_gate` with `test_suite_validate`: about a minute. It first checks the files (every problem of the static checks is listed at once, in the wording below), then runs, with no accelerator, every tuple and random draw on the reference SQL and every transaction applied, rolled back and re-applied on a writable copy of the **test database** (a small subset of the benchmark database: choose targets by subquery so they exist there; `query_data` explores the full database). Fix everything the result lists, and call it again; a draft that fails is cheaper than reading how the checker works. The gate needs no registration, build or host process from you: do not start engine processes (the host script, the engine's CLI on its database files) and never read the orchestrator, gates or harness sources.

## What the stage's validation checks

- every `params.sql` tuple and every random draw from `domain.yaml` has the query's parameter count and **runs the reference SQL** on the test database without error;
- every transaction applies, **rolls back to the state before it**, and applies again in sequence;
- for each dependent table of the analysis: an INSERT, an UPDATE and a DELETE on it (or a `not_applicable` waiver with the reason), a `rollback` and a `key_move` transaction (or waivers), and **at least one transaction on it that changes the reference answer of some tuple**: a suite that cannot make a stale maintenance visible catches nothing;
- every edge class is covered or waived with a reason; `empty_result` tuples are empty, `null_param` tuples contain NULL;
- `write_mix.yaml` is well-formed and weights only kinds that exist.

After the stage the suite is **frozen**: its content hash is recorded, the gates refuse to run when it differs, and writes to it are denied. A test that is genuinely wrong is raised as a change request (`test_suite_change_request.md` in the artifacts: the test, the evidence, what is right), which re-runs the tester.
