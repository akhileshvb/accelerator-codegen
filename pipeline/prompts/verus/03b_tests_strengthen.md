# Verus pipeline: strengthen the tests ({{benchmark}} q{{query}})

The pipeline injected deliberate bugs into a working accelerator and ran
your tests. These bugs went unnoticed:

{{missed}}

Add tests (new `txns/` files, new `params.sql` lines, `setup.sql`
additions) that make each one change an answer. Existing files must stay as
they are (the tests only grow). Typical causes: writes that never touch rows
in any answer, parameter tuples whose answers are empty, no transaction that
needs a rescan. Use `verus_sql` to find rows that matter, and
`verus_validate_tests` to check the files run. New writes must satisfy the
schema (`query/schema.sql`; its NOT NULL constraints are enforced) and run in
sequence after the existing ones; prefer realistic writes that hit answers.

{{include:shared/rules.md}}
