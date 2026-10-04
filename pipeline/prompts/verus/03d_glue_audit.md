# Verus pipeline: audit the host glue ({{benchmark}} q{{query}})

`glue/glue.cpp` (written by another agent) connects the engine to the
kernel's C ABI and is trusted: the kernel's proof assumes it delivers exactly
what `inputs/q.inputs` declares. It already builds and passed a contract
check on the test data. Your job is what the test data cannot show. The
glue contract, the declared inputs, the C ABI header, the kit API and the
glue are inlined below.

Go through these checks, in order. For each, decide pass or fail from the
glue text (and, where a check says so, one `verus_sql` call). A check fails
only with a concrete defect you can point to: the line, and the input it
mishandles.

* **G1 SQL verbatim**: every declared scan / rescan SQL and the query appear
  exactly as declared; each rescan filters on its KEYS output column.
* **G2 capture list**: `tables` lists, per delta, the table's own base columns
  the delta's SELECT reads, in Apply's order (not output aliases).
* **G3 marshalling**: every C ABI argument in the header's order, with the
  right arrays, counts and offsets; signs pass through unchanged.
* **G4 lossy fields**: each computed delta field is exactly the declared
  expression (types, NULLs, byte comparison of text).
* **G5 conversions**: decimal scales match the field types; dates /
  timestamps / booleans exact; text unchanged unless the spec's host contract
  says otherwise; `Declined` never swallowed or replaced by a value.
* **G6 parameters and results**: each parameter and result column maps to the
  right kind and position; results are freed.
* **G7 nothing extra**: no state, caching, threads or I/O; nothing that could
  deliver rows the declared SQL would not (other data, other parameters,
  larger rescans).

Write `analysis/glue_audit.json` and stop:

`{"checks": [{"id": "G1", "ok": true}, ..., {"id": "G5", "ok": false, "where": "glue.cpp:84", "problem": "...", "fix": "..."}], "verdict": "ok" | "issues"}`

`verdict` is `issues` if any check fails. Keep it short: no preamble, no
restating the code; a failing check is one line each for `where`, `problem`
and `fix`. A check you cannot fail with a concrete defect passes.

## Material

You have these files in full below: do not re-read them. Work from this text and answer in a few steps; use tools only to check something against data (`verus_sql`).

{{file:contracts/host_glue.md}}

{{file:inputs/q.inputs}}

{{file:frozen/{{crate}}_verus.h}}

{{file:ref/kit/verus_kit.hpp}}

{{file:glue/*.cpp}}

{{include:shared/rules.md}}
