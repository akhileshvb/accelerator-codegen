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
  * `rescan_<name>_keys(&State, deltas...) -> Vec<key>` ensures
    `r@ == rescan_<name>_wanted(*s, delta views...)`, a spec fn you define in
    `kernel.rs` (the stub declares it): the keys you ask for. The host passes
    back exactly those keys, so apply's `requires` says `<name>_keys@ ==
    rescan_<name>_wanted(..)`; choose `wanted` so the rows it covers are all
    apply needs, and apply need not check coverage.
* `gen_world.rs`, `gen_ffi.rs`: the adapter that puts your `State` into the
  database's one Verus store, and the C ABI (no kernel work). The store is
  global: one verified store, one reader-writer lock, every accelerator of the
  database a slot in it; each commit is one new version for all of them.

Yours: `kernel.rs` (it starts as sql2verus's stub: replace every
`#[verifier::external_body]` body with a verified implementation) and any
modules you add (declare them in `lib.rs`; keep the five `pub mod gen_* /
kernel` lines). `State` must have `pub closed spec fn db(self) -> Db`, the
database it answers for, tied to its executable fields by a type invariant.

Declining (`None` / `Err`) is part of the obligations: the kernel may decline
only outside the `envelope` (a table above `max_rows()` rows, or a value
outside its column's SQL type), and `execute` also when `answer_fits` fails (a
correct answer does not fit the result's runtime types). Inside the envelope,
`build`, `apply` and `execute` must answer for every parameter value. So size
accumulators for the envelope, not for the data you expect (a table may
hold up to 2^63 rows, so a sum of its DECIMAL(15,2) products needs more than
128 bits), and prove
"impossible" maintenance branches unreachable from the invariant instead of
returning `Err`. To decline on overflow, exhibit the correct answer (in `int`)
that does not fit.

* Exact integers: tables are bags with no unique keys, so a join group (and
  its sums) is unbounded inside the envelope. Use the prooflib's exact
  integer (`wide`: an `i128` fast path, unbounded limbs) if it exists;
  otherwise write one (verified add, sub, mul, neg, compare, narrowing to
  `i128`) and publish it.
* History: define `pub open spec fn history(s: State) -> nat` in kernel.rs
  (`0` unless the state keeps something that grows with past writes, such as
  ids it never frees). Build yields 0 and each apply adds at most its changed
  rows; apply may decline only past `max_history()` (2^62) rows since the build.
* Tracked parameter values: a design may precompute answers for chosen
  values (e.g. the most frequent ones) as a fast path, but every other value of the
  parameter's type needs a total general path, compact and maintained
  incrementally (no full-table copies, no rebuild per read). A fast-path
  decline falls back to the general path without degrading the state.

Libraries you can use (`ref/`): `verus_common` (the spec vocabulary: bags and
sums over tables, byte order, rationals, SQL functions, sorting) and
`verus_prooflib` (lemmas earlier runs proved; `prooflib/src/lib.rs`). Add a
general lemma there when it would serve other queries, then call
`prooflib_publish`: lemmas are general (no query names), verify without
`assume`, say when to use them in a doc comment. The library only grows.

How to prove it: `contracts/proof_method.md` (the obligation shapes, their
standard proofs, the lemmas, the patterns). Do not write a new tree or index
when a verified one exists (its own proofs are large): reuse the shared proof
library's or another kernel's (lifted into the library). When none exists,
write it generic (no query-specific types) and publish it with
`prooflib_publish`, so later kernels reuse it. The state must still be
persistent (`contracts/scale_principles.md`).

Verification: `verus_verify` runs Verus on the crate (and the trusted-base
check). Keep functions small and proofs local; prefer lemmas with explicit
triggers over large `assert forall` blocks; watch verification time (it is
recorded and can count against a change in optimization).

**Never stub out the kernel to make verification pass.** Prove the real code; if a part is too hard, simplify that part's design instead.
