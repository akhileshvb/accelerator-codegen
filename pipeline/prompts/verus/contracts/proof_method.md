# How to prove a kernel (any query)

Every obligation you prove has one of four generated shapes, and each has a
standard proof. Plan with them before you write proof code.

## 1. Work top-down

1. Write the state's invariant in words first: for each executable structure,
   which sum over the database it equals (e.g. "for every key k, `map[k]` is
   the number of rows of T with key k that satisfy P, times their weight").
   Put it in the type invariant as spec functions of `db`, one quantifier per
   structure, triggered on the structure's lookup term.
2. State the three top-level facts as lemmas, before proving anything else:
   build gives the invariant for `db`; apply gives it for `db2`; execute
   returns `query_result(db, params, out)`. Find which supporting facts each
   needs, and prove only those.
3. Prove the top-level facts early, even with large supporting lemmas still
   to do; spend proof effort on infrastructure (index structures, helpers)
   only as far as a top-level fact needs it.
4. If a data structure's own proof grows large (rotations, rebalancing,
   custom iterators), replace the structure with one whose specs already
   exist (`vstd` `Vec`, `HashMap`, sequences) rather than proving it.

## 2. The four obligation shapes and their proofs

Notation: `sum(s, f)` and the lemmas below are in `ref/verus_common`
(`sums.rs`, `bag.rs`, `order.rs`, `rel.rs`, `sort.rs`, `bytes.rs`).

**Scan contracts** (`scan_<n>_ok(db, rows)`):
`forall f. sum(rows, f) == sum(db.table, |r| if <WHERE> { f(<projection>) } else { 0 })`.
They hold for every weight `f`: instantiate them with the weight your
invariant needs, e.g. `f = |x| ind(key(x) == k) * w(x)` for "the contribution
to key k", or `eqv(v)` to get row multiplicities (`lemma_count_as_sum`).

**Rescan contracts** (`rescan_<n>_ok(db2, keys, rows)`): the same, restricted
to rows whose key is in `keys`, on the post-transaction database. For a key
outside `keys` they say nothing: apply must check that every key it relies on
was rescanned, and use only those.

**Capture** (`capture_ok(db, db2, deltas...)`): per table,
`bag_step(before, after, d)`:
`forall f. sum(after, f) == sum(before, f) + sum(d, |(row, sign)| sign * f(row))`.
So any per-table sum after the transaction is the sum before plus the
signed sum over the deltas, for the same weight `f`. Use it with exactly the
weight your invariant sums (`lemma_bag_step_empty`, `lemma_apply_bag_step`
for the multiset form).

**Query result** (`query_result(db, params, out)`): usually
"exists `full`: `full_result(db, params, full)` and `out == limit_rows(full, n)`",
where `full_result` says every row is correct, every qualifying group is
present, and the order is the ORDER BY. Prove it by exhibiting `full` (a
ghost sequence), proving it sorted (`order.rs`: `sorted_by`, `sort_by`,
`lemma_sort_by`, `lemma_sorted_unique`; byte order: `bytes.rs`), and that
your output is its prefix of length n.

## 3. Standard proof patterns

* **Loops over input rows** (build, apply): carry a ghost index and an
  invariant "accumulated so far == sum over the rows processed",
  i.e. `acc(k) == sum(rows.take(i), f_k)` for every key k (quantified, triggered
  on the accumulator lookup). Step with `lemma_sum_push` / `lemma_sum_take`,
  finish with `lemma_sum_take_full`, then transfer to the database with the
  scan contract instantiated at `f_k`.
* **Per-key maps**: the invariant `forall k. map[k] == spec_count(db, k)`;
  absent keys mean 0. Changing one key: `lemma_sum_update`,
  `lemma_sum_single_match`; other keys are untouched (say so in the loop
  invariant).
* **Group-by and joins**: split a sum by a key with `lemma_sum_group`
  (`distinct` keys), swap nested sums with `lemma_sum_swap`, linearity with
  `lemma_sum_add` / `lemma_sum_sub` / `lemma_sum_scale`, pointwise-equal
  weights with `lemma_sum_ext` / `lemma_sum_ext_all`, filters with
  `lemma_sum_filter`, permuted inputs with `lemma_sum_perm`, nested
  sequences with `rel.rs` (`lemma_sum_flat`, `lemma_sum_flat_map`).
* **Predicates that depend on other rows** (EXISTS / NOT EXISTS, a row's
  neighbors): a captured row's effect is not a function of that row alone.
  Recompute the affected neighborhoods from the rescans (post-transaction),
  get their pre-transaction values from the capture equation (before = after
  minus the signed deltas, for the same weight), and prove that every other
  neighborhood is unchanged (its rows are not in the deltas). Keep the
  per-neighborhood contribution as one spec function used in both the
  invariant and the update.
* **Arithmetic**: checked operations in exec code (decline on overflow);
  in proofs, nonlinear facts need `by (nonlinear_arith)` or
  `vstd::arithmetic` lemmas.
* **Quantifiers**: one trigger per quantifier, on a term containing every
  bound variable (usually the map lookup or the spec function applied to the
  key); prefer named lemmas with explicit `requires`/`ensures` over large
  `assert forall ... by` blocks.

## 4. When verification fails

Read the first error; find which top-level fact it belongs to. A failing
postcondition usually means a missing loop invariant (what the loop
maintains) or a missing lemma call (the weight `f` you need was never
instantiated). A resource limit or slow check means too much unfolding:
make a spec function `closed` or `opaque` and reveal it only where needed,
and split the proof into lemmas. `verus_verify` takes seconds: check often.
