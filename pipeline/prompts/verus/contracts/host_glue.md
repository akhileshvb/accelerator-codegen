# The host glue (glue/glue.cpp): what it must do

The glue connects the engine to the kernel's C ABI. It is **trusted** (not
proved): the kernel's proof assumes the host delivers exactly what
`inputs/q.inputs` declares, and the glue is where that happens. It is checked
by building it, by a contract check (in record mode the C ABI logs every row
the glue passes; those are compared with the declared SQL's results on the
engine), and by an audit; then it is frozen.

You write one C++ file against two APIs:

* `ref/kit/verus_kit.hpp` (namespace `accelerator_codegen::verus_kit`): the
  engine-neutral kit. Read it fully. `Host` runs SQL; `I64` / `Ts` / `Date` /
  `Bool` / `Text` / `F64` (a DOUBLE's IEEE-754 bits) read cells exactly (they
  throw `Declined` on NULL or a value outside the type); `IsNull` tests a
  cell first, for a NULL-able field; `TextColumn` builds a text column; `QueryKeys` runs a
  keyed rescan in chunks; `Arg*` read query arguments; `Out*` make result
  cells; `Glue` describes the query; `Registrar` registers it.
* `frozen/<crate>_verus.h`: the kernel's C ABI (prefix `{{ffi_prefix}}_` for
  kernel states, `{{ffi_prefix}}s_` for the Verus store; both are used, see
  "The Verus store" below). Its
  conventions: a rows argument `x` is `size_t x__n` then either one array `x`
  (a row type without text: the header declares the struct) or column arrays
  in field order: `x__<field>` (nested `x__<field>__<field>`), a text column
  as the concatenated bytes `x__<f>` plus n + 1 offsets `x__<f>__off`; a
  `(row, sign)` delta is the row under `x__row` plus `int64_t *x__sign`. Text
  parameters are `(const uint8_t *, size_t)`. Results are read with
  `_result_len` and `_result_c<k>` (0: NULL). A NULL-able field `f` comes
  with a flag `f__null` (nonzero: NULL): set it from `kit::IsNull` and leave
  the value slot zero (`kit::kNoText` for text); never read the value of a
  NULL cell. A NULL-able **parameter** (the header's `execute` declares it: a
  text parameter is a null pointer, any other type has a `p<n>__null` flag
  argument after its value, zero when NULL) is tested with `kit::ArgIsNull`
  before the `kit::Arg*` conversion; other parameters never are NULL (the
  `Arg*` functions return false on NULL: the host answers with SQL).

## The Glue to define and register

```cpp
const kit::Glue kGlue = {
    "{{accel_id}}", "{{benchmark}}", {{query}}, kQuerySql /* query/query.sql, verbatim */,
    { /* tables: one per delta in inputs/q.inputs, any order */
      {"<table>", {"<captured column>", ...}}, ... },
    { /* parameter kinds, $1 first: kit::Kind::Text / Int / Dec / Date / Bool */ },
    Build, FreeState, Apply, Execute, Answer, PrepareApply, PrepareBuild,
    kit::StoreOps{ /* see "The Verus store" */ },
};
const kit::Registrar kRegistrar(&kGlue);
```

* `tables`: the engine captures, for each listed table, every inserted /
  deleted / updated row, projected on the listed columns **in that order**
  (`Change::row` holds them; `Change::sign` is +1 / -1; an update is both).
  List exactly the table's own columns that the delta declaration's SELECT
  reads (its base columns, never an output alias): for
  `SELECT id, note LIKE '%x%' AS flagged FROM t`, capture `id, note` and
  compute `flagged` in Apply.
  `Changes` (in Apply) is indexed in this table order.
* `Build(Host &h)`: run every declared `scan`'s SQL **verbatim** through
  `h.Query`, fill the C ABI's arrays (cells in the SELECT's column order), and
  call `{{ffi_prefix}}_build`.
* `Apply(state, changes, h, err)`: marshal each table's changes into its
  delta argument (lossy fields computed exactly as the delta's SELECT says),
  call `{{ffi_prefix}}_rescan_<n>_keys` for each rescan (it writes up to `cap`
  keys and returns how many: call again with more room if needed; text keys
  come as bytes `out` plus offsets `off`, written only when both fit, with the
  bytes needed in `*out_len`), fetch those rows with `kit::QueryKeys(h,
  <declared rescan SQL>, <its KEYS output column>, keys, ...)` (`int64_t`
  keys, or `std::string` keys for text), pass the same keys to
  `{{ffi_prefix}}_apply` (text keys as a text column), and call it.
* `Execute(state, args, out)`: read the parameters (`kit::Arg*`; false: return
  false, the host answers with SQL), call `{{ffi_prefix}}_execute`, and turn
  every result row into an `AuxRow` (`kit::Out*`; columns in the query's
  order; a decimal with its scale; NULLs as `kit::OutNull`). Free the result.
* `FreeState`: `{{ffi_prefix}}_free`.

## The Verus store (same file)

The database's Verus store keeps the kernel's states as versions; the glue
hands it the kernel's calls. Three more functions and the store wiring, all
from the same marshalling as above (write each argument list once and share
it between the pairs below):

* `Answer(reading, args, out)`: `Execute`'s body, calling
  `{{ffi_prefix}}s_answer(reading, <parameters>)` (the verified answer on a
  store reading) instead of `{{ffi_prefix}}_execute(state, ...)`.
* `PrepareApply(base, changes, h, err)`: `Apply`'s body, but the base is a
  `{{ffi_prefix}}s_base` (the newest version's base, not a state): take the
  state for the rescan keys from `{{ffi_prefix}}s_base_state(base)` and finish
  with `{{ffi_prefix}}s_stage_apply(base, <apply's arguments>, err)`, which
  returns the next version's slot (null with `*err`: the host rebuilds).
* `PrepareBuild(head_of, h)`: `Build`'s body; the last step is
  `{{ffi_prefix}}s_stage_build(head_of(), <build's arguments>)`, where
  `head_of()` (a `{{ffi_prefix}}s_head`) is called once, after the scans, and
  the staged slot is returned (never null).
* The `kit::StoreOps` aggregate, in the order of `verus_kit.hpp`, each entry a
  cast-and-forward lambda to the same-named `{{ffi_prefix}}s_*` function
  (`new`, `free`, `newest_base`, `base_state`, `base_free`, `newest_head`,
  `head_free`, `stage_invalid`, `staged_free`, `begin`, `abort`, `install`,
  `read`, `reading_free`, `invalidate_all`, `version_count`). Pass `Answer`,
  `PrepareApply`, `PrepareBuild` and the aggregate after `Execute` in `kGlue`.

## Rules

* Run declared SQL **verbatim** (scans, rescans, the query). Never add,
  drop or reorder filters, and never compute in C++ what SQL should return.
* Convert exactly: integers and decimals with `kit::I64(row, i, <scale of the
  field's type>)`, dates `kit::Date`, timestamps `kit::Ts`, booleans
  `kit::Bool`, text `kit::Text`. Let `Declined` propagate (the module rebuilds
  or answers with SQL); do not catch it to substitute values.
* A captured value the C ABI cannot carry (e.g. a NULL in a column the schema
  declares NOT NULL, which the database may still accept) must **decline**: let
  the converter throw. Never substitute a value (0, a sentinel), drop the row,
  or special-case the transaction. `verus_glue_check` accepts such a decline
  (it reports it under "declined"); it rejects every workaround.
* Deliver every captured change unconditionally: what Apply passes must never
  depend on what the kernel returned (e.g. whether it asked for rescans).
* Fill arrays in the header's argument order; one row per result row.
* No state of your own, no caching, no threads, no I/O besides the Host.
