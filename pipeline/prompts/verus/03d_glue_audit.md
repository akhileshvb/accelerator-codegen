# Verus pipeline: audit the host glue ({{benchmark}} q{{query}})

`glue/glue.cpp` (written by another agent) connects the engine to the
kernel's C ABI and is trusted: the kernel's proof assumes it delivers exactly
what `inputs/q.inputs` declares. It already builds and passed a contract
check on the test data. Your job is what the test data cannot show. Read
`contracts/host_glue.md`, `inputs/q.inputs`, `frozen/{{crate}}_verus.h`,
`ref/kit/verus_kit.hpp` and the glue.

Check:

* every declared scan / rescan SQL and the query are used verbatim; the
  rescan filters on the KEYS output column; `tables` lists exactly each
  delta's columns, and Apply reads them in that order;
* every argument is passed in the header's order, with the right arrays,
  counts and offsets; signs pass through; lossy fields compute exactly the
  declared expression (types, NULL handling, byte comparison of text);
* conversions are exact (decimal scales match the fields' types; dates,
  timestamps, booleans), and `Declined` is never swallowed to substitute a
  value; parameters and results map to the right kinds and columns; results
  are freed;
* no state, caching, threads or I/O; nothing that could deliver rows other
  than the declared SQL's (e.g. on other data, other parameter values, larger
  rescans).

Write `analysis/glue_audit.json`:
`{"verdict": "ok" | "issues", "issues": [{"where": "...", "problem": "...", "fix": "..."}]}`.
Report only real defects (an "issues" verdict sends the glue back).

{{include:shared/rules.md}}
