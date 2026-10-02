# The kernel crate (crate/src)

Generated and frozen (read them; never edit):

* `gen_spec.rs`: the query's meaning (`Db`, `Params`, `OutRow`,
  `query_result(db, params, out)`). The spec of record.
* `gen_obligations.rs`: the runtime row types, the scan / rescan / capture
  contracts, and the `ob_*` functions the host calls. Each `ob_*` calls
  `crate::kernel::<name>` with exactly its contract:
  * `build(scans..., Ghost(db)) -> Option<State>` ensures `s.db() == db`;
  * `execute(&State, params...) -> Option<Vec<row>>` ensures
    `query_result(s.db(), params(..), out_view(out@))`;
  * `apply(&State, deltas..., [rescan keys, rows]..., Ghost(..), Ghost(db2)) -> Result<State, u32>`
    ensures `s2.db() == db2`;
  * `rescan_<name>_keys(&State, deltas...) -> Vec<key>` (no contract: apply
    must check every key it relies on was rescanned).
* `gen_world.rs`, `gen_ffi.rs`: the store adapter and the C ABI (no kernel
  work).

Yours: `kernel.rs` (it starts as sql2verus's stub: replace every
`#[verifier::external_body]` body with a verified implementation) and any
modules you add (declare them in `lib.rs`; keep the five `pub mod gen_* /
kernel` lines). `State` must have `pub closed spec fn db(self) -> Db`, the
database it answers for, tied to its executable fields by a type invariant.

`None` / `Err` are always sound: the host then answers with SQL or
rebuilds. But a kernel that declines often fails the integration gate
(fallback rate), so decline only on genuine limits (arithmetic overflow,
inconsistent input).

Libraries you can use (`ref/`): `verus_common` (the spec vocabulary: bags and
sums over tables, byte order, rationals, SQL functions, sorting) and
`verus_prooflib` (lemmas earlier runs proved; `prooflib/src/lib.rs`). Add a
general lemma there when it would serve other queries, then call
`prooflib_publish`: lemmas are general (no query names), verify without
`assume`, say when to use them in a doc comment. The library only grows.

Verification: `verus_verify` runs Verus on the crate (and the trusted-base
check). Keep functions small and proofs local; prefer lemmas with explicit
triggers over large `assert forall` blocks; watch verification time (it is
recorded and can count against a change in optimization).
