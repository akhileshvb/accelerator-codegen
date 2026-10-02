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
  `Bool` / `Text` read cells exactly (they throw `Declined` on NULL or a value
  outside the type); `TextColumn` builds a text column; `QueryKeys` runs a
  keyed rescan in chunks; `Arg*` read query arguments; `Out*` make result
  cells; `Glue` describes the query; `Registrar` registers it.
* `frozen/<crate>_verus.h`: the kernel's C ABI (prefix `{{ffi_prefix}}_` for
  states owned by the host, `{{ffi_prefix}}s_` for the Verus store). Its
  conventions: a rows argument `x` is `size_t x__n` then either one array `x`
  (a row type without text: the header declares the struct) or column arrays
  in field order: `x__<field>` (nested `x__<field>__<field>`), a text column
  as the concatenated bytes `x__<f>` plus n + 1 offsets `x__<f>__off`; a
  `(row, sign)` delta is the row under `x__row` plus `int64_t *x__sign`. Text
  parameters are `(const uint8_t *, size_t)`. Results are read with
  `_result_len` and `_result_c<k>` (0: NULL).

## The Glue to define and register

```cpp
const kit::Glue kGlue = {
    "{{accel_id}}", "{{benchmark}}", {{query}}, kQuerySql /* query/query.sql, verbatim */,
    { /* tables: one per delta in inputs/q.inputs, any order */
      {"<table>", {"<captured column>", ...}}, ... },
    { /* parameter kinds, $1 first: kit::Kind::Text / Int / Dec / Date / Bool */ },
    Build, FreeState, Apply, Execute, PrepareApply, PrepareBuild,
    kit::StoreOps{ /* every {{ffi_prefix}}s_* function, wrapped */ },
};
const kit::Registrar kRegistrar(&kGlue);
```

* `tables`: the engine captures, for each listed table, every inserted /
  deleted / updated row, projected on the listed columns **in that order**
  (`Change::row` holds them; `Change::sign` is +1 / -1; an update is both).
  List exactly the columns the table's delta declaration reads.
  `Changes` (in Apply) is indexed in this table order.
* `Build(Host &h)`: run every declared `scan`'s SQL **verbatim** through
  `h.Query`, fill the C ABI's arrays (cells in the SELECT's column order), and
  call `{{ffi_prefix}}_build`. `PrepareBuild(store, h)`: the same into
  `{{ffi_prefix}}s_prepare_build`.
* `Apply(state, changes, h, err)`: marshal each table's changes into its
  delta argument (lossy fields computed exactly as the delta's SELECT says),
  call `{{ffi_prefix}}_rescan_<n>_keys` for each rescan (it writes up to `cap`
  keys and returns how many: call again with more room if needed), fetch
  those rows with `kit::QueryKeys(h, <declared rescan SQL>, <its KEYS output
  column>, keys, ...)`, then call `{{ffi_prefix}}_apply`. `PrepareApply(store,
  base, changes, h, err)`: the same with the base's state
  (`{{ffi_prefix}}s_base_state(base)`) into `{{ffi_prefix}}s_prepare_apply`.
* `Execute(state, args, out)`: read the parameters (`kit::Arg*`; false: return
  false, the host answers with SQL), call `{{ffi_prefix}}_execute`, and turn
  every result row into an `AuxRow` (`kit::Out*`; columns in the query's
  order; a decimal with its scale; NULLs as `kit::OutNull`). Free the result.
* `FreeState`: `{{ffi_prefix}}_free`. `StoreOps`: wrap each `{{ffi_prefix}}s_*`
  function (new, free, newest_base, base_state, base_free, prepare_invalid,
  prepared_free, begin, abort, install, read, state_release, invalidate_all,
  version_count) with `void *` casts, in that field order.

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
