# Critique the aux-state analysis ({{display_name}} Q{{query_number}})

Another agent designed the aux state for this query; its `analysis.json` is below. Decide: **is this the
best design for fast reads at any binding and cheap maintenance under heavy writes, at SF3000?** A design
you accept is implemented and optimized over hours; one that is slow at scale, loose where it could be
tight, or beaten by a design the analyst did not consider wastes all of it. Reject it now.

You have the same data tools as the analyst (`query_data`, `column_stats`, `join_fanout`,
`rows_qualifying`, `column_spread`, `query_shape`). Do not edit any file except your report, and do not
read pipeline or accelerator sources: everything you need is below.

## 1. Re-measure the selected design

Re-run the selected candidate's `units_probe`. Check that the statement models the design as described:
the state it keeps, the bound or partition it prunes with, the stopping rule. A probe that counts
something easier than what the read does (a global window where the design needs per-entry eligibility,
a bound the design cannot maintain, the typical binding only) fails. Run it yourself at the worst binding
and under a write that loosens the bound or widens the range, if the analyst did not.

## 2. Try to beat it

Name the cost that dominates the selected design's measured read, then find a candidate that removes it:
a per-entry bound instead of a global one, a bound that depends on the parameter, a different sort or
partition key, a finer or coarser grain, two families combined, or a family the analysis skipped.
Simulate your best alternative with one `query_data` statement over the base tables, the same way. If it
visits materially fewer entries (or the same with cheaper maintenance), the verdict is `revise`.

## 3. Checks

A check fails when you can name a concrete failing case (a write, a parameter value, a data size, and
what goes wrong) or when the analysis leaves the cost unbounded and your estimate breaks it.

* **R1 reads follow the output.** At SF3000, a read's work follows the answer or the qualifying data,
  not the state or a table. For ORDER BY ... LIMIT, MIN/MAX and thresholds, stored bounds skip what cannot
  contribute and the walk stops early; the bound tightens with the parameter. Fails if the measured count
  grows with SF while the answer does not, or if the analysis's units are guesses.
* **R2 any binding.** Every value of each parameter's type is answered exactly (absent values, NULL,
  domain edges, widened data). Fails if the state is specialized to the values in the data or the seed.
* **W1 cost of a write.** A commit's work follows changed rows times their real fan-out: no rescan or
  re-sort of unchanged data, no whole-state or per-partition copy for a small change, dimension writes
  reach only actual dependents. Check the heaviest write mix and a refresh-sized batch.
* **W2 bounds stay exact under writes.** Every bound, extremum and partition the read relies on is
  maintained incrementally, including deletes that remove the current extremum.
* **S1 storage.** Every component in `state_storage` is held in a named COW primitive whose `cow_unit` is
  small (no write copies a whole map, partition or run array), and the read and write access listed for it
  is what the design's read and maintenance actually do.
* **S2 size and build.** State at SF3000 in bytes (your own entries times real bytes per entry, not the
  analysis's), transient build peak, one streaming pass with O(1) work per scanned row. Speed comes first:
  fail on size only for a component that stores data no read or write needs, duplicates another, or
  could be materially smaller at the same read and write speed, and name the smaller layout.
* **E1 exactness.** Multiplicity (duplicate join keys), NULLs, numeric widths at SF3000 (keys beyond
  2^31, sums beyond 64 bits), the engine's comparison semantics.
* **C1 no answer cache.** The state is not a cache of final answers per binding.

## Report

Write `{{critique_path}}` and stop:

```json
{"estimates": {"read": "...", "write": "...", "size": "...", "build": "..."},
 "remeasured": {"units_probe_ok": true, "notes": "..."},
 "alternative": {"description": "...", "units_probe": "SELECT ...", "measured_units": "...", "better": false},
 "checks": [{"id": "R1", "ok": true}, {"id": "W1", "ok": false, "case": "...", "suggestion": "..."}],
 "verdict": "accept"}
```

`verdict` is `revise` if any check fails or `alternative.better` is true. Each estimate is one line with
numbers. A failing check gives one or two sentences each for `case` and `suggestion`, and the suggestion
names the structure or key change that fixes it. Style, naming and nice-to-haves are not failures.

## Material

{{workload_reference_section}}

### analysis.json

```json
{{analysis_json}}
```
