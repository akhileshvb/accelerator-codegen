// Incremental maintenance: from the state for db and a transaction's captured
// deltas, produce the state for db2 = db + deltas.
//
// Orders whose 'F' multiplicity rises from 0 have no tracked lineitems; for
// those the host supplies a targeted scan of db2's lineitems (scan_contract).
use vstd::prelude::*;
use std::collections::HashMap;
use std::sync::Arc;
use verus_common::sums::*;
use crate::spec::*;
use crate::bridge::*;
use crate::orderspec::*;
use crate::ents::*;
use crate::state::*;
use crate::build::*;
use verus_common::bag::*;

verus! {

broadcast use vstd::std_specs::hash::group_hash_axioms;

/// SELECT l_orderkey, l_suppkey, l_commitdate, l_receiptdate FROM lineitem
///  WHERE l_orderkey = ANY(keys)          -- evaluated on db2 (trusted)
pub open spec fn scan_contract(db2: Db, keys: Seq<i64>, rows: Seq<LiRow>) -> bool {
    forall|f: spec_fn(Lineitem) -> int| #[trigger]
        sum(lis_view(rows), f) == sum(
            db2.lineitem,
            |l: Lineitem| if ikeys(keys).contains(l.orderkey) { f(l) } else { 0 },
        )
}

// Effect of one order change on the count of 'F' rows for order ok: +1/-1 if it is an 'F' order with that key, else 0.
// Summing over the batch gives how many 'F' rows the batch adds or removes for ok.
pub open spec fn od_w(ok: int) -> spec_fn((Order, int)) -> int {
    |d: (Order, int)| d.1 * ind(d.0.orderkey == ok && d.0.status_is_f)
}

// Effect of one lineitem change on the count of supplier sk's lines in order ok: its +1/-1 sign if both keys match, else 0.
// Summing over the batch gives the batch's net change in that line count.
pub open spec fn ld_t(ok: int, sk: int) -> spec_fn((Lineitem, int)) -> int {
    |d: (Lineitem, int)| d.1 * ind(d.0.orderkey == ok && d.0.suppkey == sk)
}

// Same as the all-lines version but only for late lines (received after commit date), so the sum is the net change in late-line count.
// Together the two counts decide whether a supplier is the only late one in an order.
pub open spec fn ld_l(ok: int, sk: int) -> spec_fn((Lineitem, int)) -> int {
    |d: (Lineitem, int)| d.1 * ind(d.0.orderkey == ok && d.0.suppkey == sk && late(d.0))
}

// How much order ok's share of supplier sk's waiting-order total moves when the database goes from db to db2.
// Summed over the affected orders it gives the change in sk's total, which is what the incremental update needs.
pub open spec fn dpo(db: Db, db2: Db, sk: int) -> spec_fn(int) -> int {
    |ok: int| per_order(db2, ok, sk) - per_order(db, ok, sk)
}

// After a transaction, each count (F rows, supplier lines, late supplier lines) for an order equals its old value
// plus the net weight of the batch's changes that touch it, so counts can be updated by deltas instead of recounted.
proof fn lemma_step_counts(
    db: Db,
    db2: Db,
    od: Seq<(Order, int)>,
    ld: Seq<(Lineitem, int)>,
    ok: int,
    sk: int,
)
    requires
        db_step(db, db2, od, ld),
    ensures
        fm(db2, ok) == fm(db, ok) + sum(od, od_w(ok)),
        cnt(db2, ok, sk) == cnt(db, ok, sk) + sum(ld, ld_t(ok, sk)),
        lcnt(db2, ok, sk) == lcnt(db, ok, sk) + sum(ld, ld_l(ok, sk)),
{
    lemma_counts_step(db, db2, od, ld, ok, sk);
    lemma_sum_ext_all(
        od,
        |d: (Order, int)| d.1 * ind(d.0.orderkey == ok && d.0.status_is_f),
        od_w(ok),
    );
    lemma_sum_ext_all(
        ld,
        |d: (Lineitem, int)| d.1 * ind(d.0.orderkey == ok && d.0.suppkey == sk),
        ld_t(ok, sk),
    );
    lemma_sum_ext_all(
        ld,
        |d: (Lineitem, int)| d.1 * ind(d.0.orderkey == ok && d.0.suppkey == sk && late(d.0)),
        ld_l(ok, sk),
    );
}

// An order not touched by any change in the batch has the same F count and supplier line counts afterward,
// so its stored per-order record stays valid and need not be recomputed.
proof fn lemma_unchanged_order(
    db: Db,
    db2: Db,
    od: Seq<(Order, int)>,
    ld: Seq<(Lineitem, int)>,
    a: Seq<int>,
    ok: int,
)
    requires
        db_step(db, db2, od, ld),
        deltas_within(a, od, ld),
        !a.contains(ok),
    ensures
        fm(db2, ok) == fm(db, ok),
        forall|sk: int| #[trigger] cnt(db2, ok, sk) == cnt(db, ok, sk),
        forall|sk: int| #[trigger] lcnt(db2, ok, sk) == lcnt(db, ok, sk),
{
    lemma_fm_unchanged(db, db2, od, ld, a, ok);
    assert forall|sk: int| #[trigger] cnt(db2, ok, sk) == cnt(db, ok, sk) by {
        lemma_step_counts(db, db2, od, ld, ok, sk);
        assert forall|i: int| 0 <= i < ld.len() implies ld_t(ok, sk)(#[trigger] ld[i]) == 0 by {
            assert(a.contains(ld[i].0.orderkey));
        }
        lemma_sum_zero(ld, ld_t(ok, sk));
    }
    assert forall|sk: int| #[trigger] lcnt(db2, ok, sk) == lcnt(db, ok, sk) by {
        lemma_step_counts(db, db2, od, ld, ok, sk);
        assert forall|i: int| 0 <= i < ld.len() implies ld_l(ok, sk)(#[trigger] ld[i]) == 0 by {
            assert(a.contains(ld[i].0.orderkey));
        }
        lemma_sum_zero(ld, ld_l(ok, sk));
    }
}

// ── affected orderkeys ─────────────────────────────────────────────────────
// Well-formedness of the list of affected orders and its lookup table: no duplicates, table keys are exactly the list,
// and each key maps to its position. This lets us update per-order slots by key without double counting.
pub open spec fn keys_ok(a: Seq<i64>, pos: Map<i64, usize>) -> bool {
    &&& forall|i: int, j: int| 0 <= i < a.len() && 0 <= j < a.len() && i != j ==> a[i] != a[j]
    &&& forall|k: i64| #[trigger] pos.contains_key(k) <==> a.contains(k)
    &&& forall|k: i64| #[trigger] pos.contains_key(k) ==> (pos[k] as int) < a.len() && a[pos[k] as int] == k
}

// Add an order key to the affected-orders list unless already present, keeping it duplicate-free and the lookup table
// consistent, so every key seen so far stays findable.
fn add_key(a: &mut Vec<i64>, pos: &mut HashMap<i64, usize>, k: i64)
    requires
        keys_ok(old(a)@, old(pos)@),
    ensures
        keys_ok(final(a)@, final(pos)@),
        final(a)@.contains(k),
        forall|x: i64| old(a)@.contains(x) ==> final(a)@.contains(x),
        final(a)@.len() <= old(a)@.len() + 1,
{
    assert(vstd::std_specs::hash::obeys_key_model::<i64>());
    if !pos.contains_key(&k) {
        let ghost a0 = a@;
        let n = a.len();
        pos.insert(k, n);
        a.push(k);
        proof {
            assert(a@[n as int] == k);
            assert forall|i: int, j: int| 0 <= i < a@.len() && 0 <= j < a@.len() && i != j implies a@[i] != a@[j] by {
                if i == n as int {
                    assert(!a0.contains(k));
                    assert(a@[j] == a0[j]);
                } else if j == n as int {
                    assert(!a0.contains(k));
                    assert(a@[i] == a0[i]);
                } else {
                    assert(a@[i] == a0[i] && a@[j] == a0[j]);
                }
            }
            assert forall|x: i64| #[trigger] pos@.contains_key(x) <==> a@.contains(x) by {
                if x != k {
                    if a@.contains(x) {
                        let i = choose|i: int| 0 <= i < a@.len() && a@[i] == x;
                        assert(a0[i] == x);
                    }
                    if a0.contains(x) {
                        let i = choose|i: int| 0 <= i < a0.len() && a0[i] == x;
                        assert(a@[i] == x);
                    }
                }
            }
            assert forall|x: i64| old(a)@.contains(x) implies a@.contains(x) by {
                let i = choose|i: int| 0 <= i < a0.len() && a0[i] == x;
                assert(a@[i] == x);
            }
        }
    } else {
        assert(a@.contains(k));
    }
}

// Gather the distinct orders that the batch could affect: orders with an 'F' status change and orders with lineitem changes.
// Only these orders need their per-order records refreshed; all others are provably unaffected.
fn collect_keys(od: &[OrdDelta], ld: &[LiDelta]) -> (r: (Vec<i64>, HashMap<i64, usize>))
    ensures
        keys_ok(r.0@, r.1@),
        forall|i: int| 0 <= i < od@.len() && (#[trigger] od@[i]).status_is_f != 0 ==> r.0@.contains(od@[i].orderkey),
        forall|i: int| 0 <= i < ld@.len() ==> r.0@.contains((#[trigger] ld@[i]).row.orderkey),
{
    let mut a: Vec<i64> = Vec::new();
    let mut pos: HashMap<i64, usize> = HashMap::new();
    assert(vstd::std_specs::hash::obeys_key_model::<i64>());
    let mut t: usize = 0;
    while t < od.len()
        invariant
            0 <= t <= od@.len(),
            keys_ok(a@, pos@),
            forall|i: int| 0 <= i < t && (#[trigger] od@[i]).status_is_f != 0 ==> a@.contains(od@[i].orderkey),
        decreases od@.len() - t,
    {
        if od[t].status_is_f != 0 {
            add_key(&mut a, &mut pos, od[t].orderkey);
        }
        t += 1;
    }
    let mut t: usize = 0;
    while t < ld.len()
        invariant
            0 <= t <= ld@.len(),
            keys_ok(a@, pos@),
            forall|i: int| 0 <= i < od@.len() && (#[trigger] od@[i]).status_is_f != 0 ==> a@.contains(od@[i].orderkey),
            forall|i: int| 0 <= i < t ==> a@.contains((#[trigger] ld@[i]).row.orderkey),
        decreases ld@.len() - t,
    {
        add_key(&mut a, &mut pos, ld[t].row.orderkey);
        t += 1;
    }
    (a, pos)
}

// Shows the gathered key list really covers every order mentioned in the batch, which is the premise for treating all
// other orders as unchanged.
proof fn lemma_within(a: Seq<i64>, od: Seq<OrdDelta>, ld: Seq<LiDelta>)
    requires
        forall|i: int| 0 <= i < od.len() && (#[trigger] od[i]).status_is_f != 0 ==> a.contains(od[i].orderkey),
        forall|i: int| 0 <= i < ld.len() ==> a.contains((#[trigger] ld[i]).row.orderkey),
    ensures
        deltas_within(ikeys(a), od_view(od), ld_view(ld)),
{
    assert forall|i: int| 0 <= i < od_view(od).len() && (#[trigger] od_view(od)[i]).0.status_is_f implies ikeys(a).contains(od_view(od)[i].0.orderkey) by {
        assert(od[i].status_is_f != 0);
        let j = choose|j: int| 0 <= j < a.len() && a[j] == od[i].orderkey;
        assert(ikeys(a)[j] == od[i].orderkey as int);
    }
    assert forall|i: int| 0 <= i < ld_view(ld).len() implies ikeys(a).contains((#[trigger] ld_view(ld)[i]).0.orderkey) by {
        let j = choose|j: int| 0 <= j < a.len() && a[j] == ld[i].row.orderkey;
        assert(ikeys(a)[j] == ld[i].row.orderkey as int);
    }
}

// ── 'F' multiplicity deltas per affected key ───────────────────────────────
// For each affected order, total up the batch's +1/-1 changes to its number of 'F' order rows; give up (None) if the sum overflows.
// This is the net change to how many times the order counts toward the answer.
fn fm_deltas(od: &[OrdDelta], a: &Vec<i64>, pos: &HashMap<i64, usize>) -> (r: Option<Vec<i64>>)
    requires
        keys_ok(a@, pos@),
        forall|i: int| 0 <= i < od@.len() && (#[trigger] od@[i]).status_is_f != 0 ==> a@.contains(od@[i].orderkey),
    ensures
        r matches Some(v) ==> v@.len() == a@.len() && forall|j: int|
            0 <= j < a@.len() ==> (#[trigger] v@[j]) as int == sum(od_view(od@), od_w(a@[j] as int)),
{
    assert(vstd::std_specs::hash::obeys_key_model::<i64>());
    let na = a.len();
    let mut v: Vec<i64> = Vec::new();
    let mut j: usize = 0;
    while j < na
        invariant
            0 <= j <= na == a@.len(),
            v@.len() == j,
            forall|x: int| 0 <= x < j ==> v@[x] == 0,
        decreases na - j,
    {
        v.push(0);
        j += 1;
    }
    let mut t: usize = 0;
    while t < od.len()
        invariant
            0 <= t <= od@.len(),
            na == a@.len(),
            keys_ok(a@, pos@),
            vstd::std_specs::hash::obeys_key_model::<i64>(),
            forall|i: int| 0 <= i < od@.len() && (#[trigger] od@[i]).status_is_f != 0 ==> a@.contains(od@[i].orderkey),
            v@.len() == na,
            forall|x: int| 0 <= x < na ==> (#[trigger] v@[x]) as int == sum(od_view(od@.take(t as int)), od_w(a@[x] as int)),
        decreases od@.len() - t,
    {
        let d = od[t];
        let ghost v0 = v@;
        proof {
            assert(od_view(od@.take(t as int + 1)) =~= od_view(od@.take(t as int)).push(od_view(od@)[t as int]));
            assert forall|x: int| 0 <= x < na implies sum(od_view(od@.take(t as int + 1)), od_w(a@[x] as int)) == sum(od_view(od@.take(t as int)), od_w(a@[x] as int)) + od_w(a@[x] as int)(od_view(od@)[t as int]) by {
                lemma_sum_push(od_view(od@.take(t as int)), od_view(od@)[t as int], od_w(a@[x] as int));
            }
        }
        if d.status_is_f != 0 {
            proof {
                assert(od@[t as int].status_is_f != 0);
                assert(a@.contains(d.orderkey));
                let q = choose|q: int| 0 <= q < a@.len() && a@[q] == d.orderkey;
                assert(pos@.contains_key(d.orderkey));
            }
            let jj = *pos.get(&d.orderkey).unwrap();
            let nv = match v[jj].checked_add(d.sign) {
                Some(x) => x,
                None => {
                    return None;
                },
            };
            v.set(jj, nv);
            proof {
                assert forall|x: int| 0 <= x < na implies (#[trigger] v@[x]) as int == sum(od_view(od@.take(t as int + 1)), od_w(a@[x] as int)) by {
                    if x != jj as int {
                        assert(a@[x] != a@[jj as int]);
                    }
                }
            }
        } else {
            proof {
                assert forall|x: int| 0 <= x < na implies (#[trigger] v@[x]) as int == sum(od_view(od@.take(t as int + 1)), od_w(a@[x] as int)) by {}
            }
        }
        t += 1;
    }
    proof {
        assert(od@.take(od@.len() as int) =~= od@);
    }
    Some(v)
}

// ── lineitem deltas into the working entries ───────────────────────────────
// Fold each lineitem change into the per-order, per-supplier line counts (total and late) of its order;
// returns false if a counter would overflow. After this, the counts reflect the batch's inserts and deletes.
fn apply_ld(work: &mut Vec<Vec<Ent>>, ld: &[LiDelta], a: &Vec<i64>, pos: &HashMap<i64, usize>) -> (ok: bool)
    requires
        keys_ok(a@, pos@),
        old(work)@.len() == a@.len(),
        forall|j: int| 0 <= j < a@.len() ==> edistinct((#[trigger] old(work)@[j])@),
        forall|i: int| 0 <= i < ld@.len() ==> a@.contains((#[trigger] ld@[i]).row.orderkey),
    ensures
        ok ==> final(work)@.len() == a@.len(),
        ok ==> forall|j: int| 0 <= j < a@.len() ==> edistinct((#[trigger] final(work)@[j])@),
        ok ==> forall|j: int, sk: int| 0 <= j < a@.len() ==> #[trigger] etot(final(work)@[j]@, sk) == etot(old(work)@[j]@, sk) + sum(ld_view(ld@), ld_t(a@[j] as int, sk)),
        ok ==> forall|j: int, sk: int| 0 <= j < a@.len() ==> #[trigger] elate(final(work)@[j]@, sk) == elate(old(work)@[j]@, sk) + sum(ld_view(ld@), ld_l(a@[j] as int, sk)),
{
    assert(vstd::std_specs::hash::obeys_key_model::<i64>());
    let na = a.len();
    let mut t: usize = 0;
    while t < ld.len()
        invariant
            0 <= t <= ld@.len(),
            na == a@.len(),
            keys_ok(a@, pos@),
            vstd::std_specs::hash::obeys_key_model::<i64>(),
            forall|i: int| 0 <= i < ld@.len() ==> a@.contains((#[trigger] ld@[i]).row.orderkey),
            work@.len() == na,
            forall|j: int| 0 <= j < na ==> edistinct((#[trigger] work@[j])@),
            forall|j: int, sk: int| 0 <= j < na ==> #[trigger] etot(work@[j]@, sk) == etot(old(work)@[j]@, sk) + sum(ld_view(ld@.take(t as int)), ld_t(a@[j] as int, sk)),
            forall|j: int, sk: int| 0 <= j < na ==> #[trigger] elate(work@[j]@, sk) == elate(old(work)@[j]@, sk) + sum(ld_view(ld@.take(t as int)), ld_l(a@[j] as int, sk)),
        decreases ld@.len() - t,
    {
        let d = ld[t];
        let ghost w0 = work@;
        proof {
            assert(ld_view(ld@.take(t as int + 1)) =~= ld_view(ld@.take(t as int)).push(ld_view(ld@)[t as int]));
            assert(a@.contains(d.row.orderkey));
            assert(pos@.contains_key(d.row.orderkey));
        }
        let jj = *pos.get(&d.row.orderkey).unwrap();
        let is_l = is_late(&d.row);
        let dl: i64 = if is_l {
            d.sign
        } else {
            0
        };
        let added = ents_add(&mut work[jj], d.row.suppkey, d.sign, dl);
        if !added {
            return false;
        }
        proof {
            assert forall|j: int, sk: int| 0 <= j < na implies #[trigger] etot(work@[j]@, sk) == etot(old(work)@[j]@, sk) + sum(ld_view(ld@.take(t as int + 1)), ld_t(a@[j] as int, sk)) by {
                lemma_sum_push(ld_view(ld@.take(t as int)), ld_view(ld@)[t as int], ld_t(a@[j] as int, sk));
                if j != jj as int {
                    assert(a@[j] != a@[jj as int]);
                    assert(work@[j] == w0[j]);
                }
            }
            assert forall|j: int, sk: int| 0 <= j < na implies #[trigger] elate(work@[j]@, sk) == elate(old(work)@[j]@, sk) + sum(ld_view(ld@.take(t as int + 1)), ld_l(a@[j] as int, sk)) by {
                lemma_sum_push(ld_view(ld@.take(t as int)), ld_view(ld@)[t as int], ld_l(a@[j] as int, sk));
                if j != jj as int {
                    assert(a@[j] != a@[jj as int]);
                    assert(work@[j] == w0[j]);
                }
            }
            assert forall|j: int| 0 <= j < na implies edistinct((#[trigger] work@[j])@) by {
                if j != jj as int {
                    assert(work@[j] == w0[j]);
                }
            }
        }
        t += 1;
    }
    proof {
        assert(ld@.take(ld@.len() as int) =~= ld@);
    }
    true
}

// ── targeted scan into fresh entries ───────────────────────────────────────
// For orders that just became 'F', recount lines per supplier (total and late) from the rows the host scanned for them;
// None on overflow. Needed because these orders have no stored counts to update incrementally.
fn scan_accumulate(srows: &[LiRow], a: &Vec<i64>, pos: &HashMap<i64, usize>) -> (r: Option<Vec<Vec<Ent>>>)
    requires
        keys_ok(a@, pos@),
    ensures
        r matches Some(sw) ==> {
            &&& sw@.len() == a@.len()
            &&& forall|j: int| 0 <= j < a@.len() ==> edistinct((#[trigger] sw@[j])@) && epos(sw@[j]@)
            &&& forall|j: int, sk: int| 0 <= j < a@.len() ==> #[trigger] etot(sw@[j]@, sk) == cnt_rows(srows@, a@[j] as int, sk)
            &&& forall|j: int, sk: int| 0 <= j < a@.len() ==> #[trigger] elate(sw@[j]@, sk) == lcnt_rows(srows@, a@[j] as int, sk)
        },
{
    assert(vstd::std_specs::hash::obeys_key_model::<i64>());
    let na = a.len();
    let mut sw: Vec<Vec<Ent>> = Vec::new();
    let mut j: usize = 0;
    while j < na
        invariant
            0 <= j <= na == a@.len(),
            sw@.len() == j,
            forall|x: int| 0 <= x < j ==> (#[trigger] sw@[x])@ == Seq::<Ent>::empty(),
        decreases na - j,
    {
        sw.push(Vec::new());
        j += 1;
    }
    proof {
        assert forall|x: int, sk: int| 0 <= x < na implies #[trigger] etot(sw@[x]@, sk) == cnt_rows(srows@.take(0), a@[x] as int, sk) by {
            assert(lis_view(srows@.take(0)).len() == 0);
        }
        assert forall|x: int, sk: int| 0 <= x < na implies #[trigger] elate(sw@[x]@, sk) == lcnt_rows(srows@.take(0), a@[x] as int, sk) by {
            assert(lis_view(srows@.take(0)).len() == 0);
        }
    }
    let mut t: usize = 0;
    while t < srows.len()
        invariant
            0 <= t <= srows@.len(),
            na == a@.len(),
            keys_ok(a@, pos@),
            vstd::std_specs::hash::obeys_key_model::<i64>(),
            sw@.len() == na,
            forall|x: int| 0 <= x < na ==> edistinct((#[trigger] sw@[x])@) && epos(sw@[x]@),
            forall|x: int, sk: int| 0 <= x < na ==> #[trigger] etot(sw@[x]@, sk) == cnt_rows(srows@.take(t as int), a@[x] as int, sk),
            forall|x: int, sk: int| 0 <= x < na ==> #[trigger] elate(sw@[x]@, sk) == lcnt_rows(srows@.take(t as int), a@[x] as int, sk),
        decreases srows@.len() - t,
    {
        let r = srows[t];
        let ghost w0 = sw@;
        let ghost pre = srows@.take(t as int);
        proof {
            lemma_lis_take(srows@, t as int);
        }
        match pos.get(&r.orderkey) {
            Some(jp) => {
                let jj = *jp;
                let is_l = is_late(&r);
                let dl: i64 = if is_l {
                    1
                } else {
                    0
                };
                let added = ents_add(&mut sw[jj], r.suppkey, 1, dl);
                if !added {
                    return None;
                }
                proof {
                    assert forall|x: int, sk: int| 0 <= x < na implies #[trigger] etot(sw@[x]@, sk) == cnt_rows(srows@.take(t as int + 1), a@[x] as int, sk) by {
                        lemma_sum_push(lis_view(pre), li_view(r), |l: Lineitem| ind(l.orderkey == a@[x] as int && l.suppkey == sk));
                        if x != jj as int {
                            assert(a@[x] != a@[jj as int]);
                            assert(sw@[x] == w0[x]);
                        }
                    }
                    assert forall|x: int, sk: int| 0 <= x < na implies #[trigger] elate(sw@[x]@, sk) == lcnt_rows(srows@.take(t as int + 1), a@[x] as int, sk) by {
                        lemma_sum_push(lis_view(pre), li_view(r), |l: Lineitem| ind(l.orderkey == a@[x] as int && l.suppkey == sk && late(l)));
                        if x != jj as int {
                            assert(a@[x] != a@[jj as int]);
                            assert(sw@[x] == w0[x]);
                        }
                    }
                    assert forall|x: int| 0 <= x < na implies edistinct((#[trigger] sw@[x])@) && epos(sw@[x]@) by {
                        if x != jj as int {
                            assert(sw@[x] == w0[x]);
                        }
                    }
                }
            },
            None => {
                proof {
                    assert(!a@.contains(r.orderkey));
                    assert forall|x: int, sk: int| 0 <= x < na implies #[trigger] etot(sw@[x]@, sk) == cnt_rows(srows@.take(t as int + 1), a@[x] as int, sk) by {
                        lemma_sum_push(lis_view(pre), li_view(r), |l: Lineitem| ind(l.orderkey == a@[x] as int && l.suppkey == sk));
                        assert(a@[x] != r.orderkey);
                    }
                    assert forall|x: int, sk: int| 0 <= x < na implies #[trigger] elate(sw@[x]@, sk) == lcnt_rows(srows@.take(t as int + 1), a@[x] as int, sk) by {
                        lemma_sum_push(lis_view(pre), li_view(r), |l: Lineitem| ind(l.orderkey == a@[x] as int && l.suppkey == sk && late(l)));
                        assert(a@[x] != r.orderkey);
                    }
                }
            },
        }
        t += 1;
    }
    proof {
        assert(srows@.take(srows@.len() as int) =~= srows@);
    }
    Some(sw)
}

// The host-scanned rows give correct per-supplier line counts for the scanned order in the new database,
// so records built from them describe the post-transaction data.
proof fn lemma_scan_counts(db2: Db, skeys: Seq<i64>, srows: Seq<LiRow>, ok: int, sk: int)
    requires
        scan_contract(db2, skeys, srows),
        ikeys(skeys).contains(ok),
    ensures
        cnt_rows(srows, ok, sk) == cnt(db2, ok, sk),
        lcnt_rows(srows, ok, sk) == lcnt(db2, ok, sk),
{
    let fc = |l: Lineitem| ind(l.orderkey == ok && l.suppkey == sk);
    let fl = |l: Lineitem| ind(l.orderkey == ok && l.suppkey == sk && late(l));
    assert(sum(lis_view(srows), fc) == sum(db2.lineitem, |l: Lineitem| if ikeys(skeys).contains(l.orderkey) { fc(l) } else { 0 }));
    assert(sum(lis_view(srows), fl) == sum(db2.lineitem, |l: Lineitem| if ikeys(skeys).contains(l.orderkey) { fl(l) } else { 0 }));
    lemma_sum_ext_all(db2.lineitem, |l: Lineitem| if ikeys(skeys).contains(l.orderkey) { fc(l) } else { 0 }, fc);
    lemma_sum_ext_all(db2.lineitem, |l: Lineitem| if ikeys(skeys).contains(l.orderkey) { fl(l) } else { 0 }, fl);
}

// ── old records of the affected orders ─────────────────────────────────────
// Fetch each affected order's pre-transaction data: its F count, per-supplier line counts, and whether it was stored.
// These are the starting values that the batch's deltas are added to.
fn load_old(orders: &OrderMap, a: &Vec<i64>) -> (r: (Vec<i64>, Vec<Vec<Ent>>, Vec<bool>))
    ensures
        r.0@.len() == a@.len(),
        r.1@.len() == a@.len(),
        r.2@.len() == a@.len(),
        forall|j: int| 0 <= j < a@.len() ==> (#[trigger] r.2@[j]) == om_get(*orders, a@[j]).is_some(),
        forall|j: int| 0 <= j < a@.len() && #[trigger] r.2@[j] ==> r.0@[j] == om_get(*orders, a@[j]).unwrap().fm && r.1@[j]@ == om_get(*orders, a@[j]).unwrap().ents@,
        forall|j: int| 0 <= j < a@.len() && !#[trigger] r.2@[j] ==> r.0@[j] == 0 && r.1@[j]@ == Seq::<Ent>::empty(),
{
    let na = a.len();
    let mut fo: Vec<i64> = Vec::new();
    let mut eo: Vec<Vec<Ent>> = Vec::new();
    let mut tr: Vec<bool> = Vec::new();
    let mut j: usize = 0;
    while j < na
        invariant
            0 <= j <= na == a@.len(),
            fo@.len() == j && eo@.len() == j && tr@.len() == j,
            forall|x: int| 0 <= x < j ==> (#[trigger] tr@[x]) == om_get(*orders, a@[x]).is_some(),
            forall|x: int| 0 <= x < j && #[trigger] tr@[x] ==> fo@[x] == om_get(*orders, a@[x]).unwrap().fm && eo@[x]@ == om_get(*orders, a@[x]).unwrap().ents@,
            forall|x: int| 0 <= x < j && !#[trigger] tr@[x] ==> fo@[x] == 0 && eo@[x]@ == Seq::<Ent>::empty(),
        decreases na - j,
    {
        match om_get_exec(orders, a[j]) {
            Some(o) => {
                let e = o.ents.clone();
                proof {
                    assert(e@ =~= o.ents@);
                }
                fo.push(o.fm);
                eo.push(e);
                tr.push(true);
            },
            None => {
                fo.push(0);
                eo.push(Vec::new());
                tr.push(false);
            },
        }
        j += 1;
    }
    (fo, eo, tr)
}

// Copy a list of per-supplier count entries, guaranteeing the copy has identical contents; used so updates work on a copy.
fn clone_ents(e: &Vec<Ent>) -> (r: Vec<Ent>)
    ensures
        r@ == e@,
{
    let r = e.clone();
    proof {
        assert(r@ =~= e@);
    }
    r
}

// Look up an order key in the immutable base table, returning nothing if absent.
pub open spec fn base_get(base: Map<i64, OrdE>, k: i64) -> Option<OrdE> {
    if base.contains_key(k) {
        Some(base[k])
    } else {
        None
    }
}

/// Fold the overlay into a fresh base (the old base stays shared by older
/// versions).
fn compact(m: &OrderMap) -> (r: OrderMap)
    requires
        forall|k: i64| #[trigger] m.overlay@.contains_key(k) ==> m.okeys@.contains(k),
    ensures
        forall|k: i64| ord_eq(#[trigger] om_get(r, k), om_get(*m, k)),
        r.okeys@.len() == 0,
        r.overlay@ == Map::<i64, Option<OrdE>>::empty(),
{
    assert(vstd::std_specs::hash::obeys_key_model::<i64>());
    let mut nb: HashMap<i64, OrdE> = (*m.base).clone();
    proof {
        assert forall|k: i64| ord_eq(#[trigger] base_get(nb@, k), base_get(m.base@, k)) by {
            if nb@.contains_key(k) {
                assert(nb@.dom().contains(k));
                assert(cloned::<OrdE>(m.base@[k], nb@[k]));
            } else {
                assert(!m.base@.dom().contains(k));
            }
        }
    }
    let n = m.okeys.len();
    let mut t: usize = 0;
    while t < n
        invariant
            0 <= t <= n == m.okeys@.len(),
            vstd::std_specs::hash::obeys_key_model::<i64>(),
            forall|k: i64| ord_eq(#[trigger] base_get(nb@, k), if m.okeys@.take(t as int).contains(k) && m.overlay@.contains_key(k) {
                m.overlay@[k]
            } else {
                base_get(m.base@, k)
            }),
        decreases n - t,
    {
        let k = m.okeys[t];
        let ghost nb0 = nb@;
        proof {
            assert(m.okeys@.take(t as int + 1) =~= m.okeys@.take(t as int).push(k));
        }
        match m.overlay.get(&k) {
            Some(v) => match v {
                Some(o) => {
                    let c = o.clone();
                    nb.insert(k, c);
                },
                None => {
                    nb.remove(&k);
                },
            },
            None => {},
        }
        proof {
            assert forall|x: i64| ord_eq(#[trigger] base_get(nb@, x), if m.okeys@.take(t as int + 1).contains(x) && m.overlay@.contains_key(x) {
                m.overlay@[x]
            } else {
                base_get(m.base@, x)
            }) by {
                if x != k {
                    assert(base_get(nb@, x) == base_get(nb0, x));
                    if m.okeys@.take(t as int + 1).contains(x) {
                        let q = choose|q: int| 0 <= q < t + 1 && #[trigger] m.okeys@.take(t as int + 1)[q] == x;
                        assert(q != t as int);
                        assert(m.okeys@.take(t as int)[q] == x);
                    }
                    if m.okeys@.take(t as int).contains(x) {
                        let q = choose|q: int| 0 <= q < t && #[trigger] m.okeys@.take(t as int)[q] == x;
                        assert(m.okeys@.take(t as int + 1)[q] == x);
                    }
                } else {
                    assert(m.okeys@.take(t as int + 1)[t as int] == k);
                }
            }
        }
        t += 1;
    }
    proof {
        assert(m.okeys@.take(n as int) =~= m.okeys@);
    }
    let r = OrderMap { base: Arc::new(nb), overlay: HashMap::new(), okeys: Vec::new() };
    proof {
        assert forall|k: i64| ord_eq(#[trigger] om_get(r, k), om_get(*m, k)) by {
            assert(base_get(nb@, k) == om_get(r, k));
            if m.overlay@.contains_key(k) {
                assert(m.okeys@.contains(k));
            }
        }
    }
    r
}

pub const COMPACT_THRESHOLD: usize = 16384;

/// Keys (with an 'F' insert) whose orders are not tracked: the host must scan
/// their db2 lineitems.  Any output is safe: `apply` checks coverage.
pub fn needs_scan(s: &State, od: &[OrdDelta]) -> Vec<i64> {
    let (_st, orders, _wm) = s.parts();
    let mut r: Vec<i64> = Vec::new();
    let mut t: usize = 0;
    while t < od.len()
        decreases od@.len() - t,
    {
        let d = od[t];
        if d.status_is_f != 0 && d.sign > 0 {
            match om_get_exec(orders, d.orderkey) {
                Some(_) => {},
                None => {
                    r.push(d.orderkey);
                },
            }
        }
        t += 1;
    }
    r
}

/// Same record up to cloning (ord_ok only looks at fm and the entries).
pub open spec fn ord_eq(x: Option<OrdE>, y: Option<OrdE>) -> bool {
    match (x, y) {
        (Some(p), Some(q)) => p.fm == q.fm && p.ents@ == q.ents@,
        (None, None) => true,
        _ => false,
    }
}

// Copying an optional order record yields an equivalent record, so cloned records remain valid stand-ins for the originals.
proof fn lemma_cloned_ord_eq(x: Option<OrdE>, y: Option<OrdE>)
    requires
        cloned::<Option<OrdE>>(x, y),
    ensures
        ord_eq(x, y),
{
    if x.is_some() && y.is_some() && x.unwrap() != y.unwrap() {
        assert(cloned::<OrdE>(x.unwrap(), y.unwrap()));
    }
}

// Progress invariant while rewriting the order table for the first j affected orders: untouched orders keep their records,
// rewritten ones match the new database (or are absent with zero F rows), and the table's bookkeeping stays consistent.
pub open spec fn om_step_ok(m2: OrderMap, m: OrderMap, a: Seq<i64>, j: int, db: Db, db2: Db) -> bool {
    &&& forall|k: i64|
        !a.take(j).contains(k) ==> ord_eq(#[trigger] om_get(m2, k), om_get(m, k))
    &&& forall|k: i64|
        a.take(j).contains(k) ==> match #[trigger] om_get(m2, k) {
            Some(o) => ord_ok(o, db2, k as int),
            None => fm(db2, k as int) == 0,
        }
    &&& forall|k: i64| #[trigger] m2.overlay@.contains_key(k) ==> m2.okeys@.contains(k)
    &&& m2.base@ == m.base@
}

/// Apply one transaction.  Err(1): a newly 'F' order was not scanned;
/// Err(2): arithmetic overflow / inconsistent deltas.  The host rebuilds on Err.
pub fn apply(
    s: &State,
    od: &[OrdDelta],
    ld: &[LiDelta],
    sd: &Vec<(SupE, i64)>,
    nd: &Vec<(NatE, i64)>,
    skeys: &[i64],
    srows: &[LiRow],
    Ghost(db2): Ghost<Db>,
) -> (r: Result<State, u32>)
    requires
        db_step(s.db(), db2, od_view(od@), ld_view(ld@)),
        dims_step(s.db(), db2, dview(sd@), dview(nd@)),
        scan_contract(db2, skeys@, srows@),
    ensures
        r matches Ok(s2) ==> s2.db() == db2,
{
    assert(vstd::std_specs::hash::obeys_key_model::<i64>());
    let ghost db = s.db();
    let (st, orders, wm) = s.parts();
    let ghost odv = od_view(od@);
    let ghost ldv = ld_view(ld@);
    // affected keys
    let (a, pos) = collect_keys(od, ld);
    proof {
        lemma_within(a@, od@, ld@);
        lemma_count_distinct_keys(a@);
    }
    let fmd = match fm_deltas(od, &a, &pos) {
        Some(v) => v,
        None => {
            return Err(2);
        },
    };
    let (fo, eo, tr) = load_old(orders, &a);
    let mut work: Vec<Vec<Ent>> = Vec::new();
    let mut j: usize = 0;
    while j < a.len()
        invariant
            0 <= j <= a@.len(),
            eo@.len() == a@.len(),
            work@.len() == j,
            forall|x: int| 0 <= x < j ==> (#[trigger] work@[x])@ == eo@[x]@,
        decreases a@.len() - j,
    {
        work.push(clone_ents(&eo[j]));
        j += 1;
    }
    proof {
        assert forall|x: int| 0 <= x < a@.len() implies edistinct((#[trigger] work@[x])@) by {
            if tr@[x] {
                assert(ord_ok(om_get(*orders, a@[x]).unwrap(), db, a@[x] as int));
            }
        }
    }
    let ghost work0 = work@;
    if !apply_ld(&mut work, ld, &a, &pos) {
        return Err(2);
    }
    let sw = match scan_accumulate(srows, &a, &pos) {
        Some(x) => x,
        None => {
            return Err(2);
        },
    };
    // scanned key set
    let mut sset: HashMap<i64, bool> = HashMap::new();
    let mut t: usize = 0;
    while t < skeys.len()
        invariant
            0 <= t <= skeys@.len(),
            vstd::std_specs::hash::obeys_key_model::<i64>(),
            forall|k: i64| #[trigger] sset@.contains_key(k) ==> skeys@.contains(k),
        decreases skeys@.len() - t,
    {
        sset.insert(skeys[t], true);
        proof {
            assert(skeys@[t as int] == skeys[t as int]);
        }
        t += 1;
    }
    // new records and W deltas
    let mut wm2: HashMap<i64, i64> = wm.clone();
    proof {
        assert forall|k: i64| wm2@.contains_key(k) implies wm2@[k] == wm@[k] by {
            assert(wm2@.dom().contains(k));
        }
        assert(wm2@ =~= wm@);
    }
    let mut overlay: HashMap<i64, Option<OrdE>> = orders.overlay.clone();
    let mut okeys: Vec<i64> = orders.okeys.clone();
    let base = orders.base.clone();
    let ghost m0 = *orders;
    let ghost sups = st.sups@;
    proof {
        assert(okeys@ =~= orders.okeys@);
        assert forall|k: i64| #[trigger] overlay@.contains_key(k) implies okeys@.contains(k) by {
            assert(orders.overlay@.dom().contains(k));
        }
        let m2i = OrderMap { base, overlay, okeys };
        assert forall|k: i64| !a@.take(0).contains(k) implies ord_eq(#[trigger] om_get(m2i, k), om_get(m0, k)) by {
            if overlay@.contains_key(k) {
                assert(overlay@.dom().contains(k));
                lemma_cloned_ord_eq(orders.overlay@[k], overlay@[k]);
            } else {
                assert(!orders.overlay@.dom().contains(k));
            }
        }
    }
    let na = a.len();
    let mut j: usize = 0;
    while j < na
        invariant
            0 <= j <= na == a@.len(),
            vstd::std_specs::hash::obeys_key_model::<i64>(),
            keys_ok(a@, pos@),
            state_ok(*st, *orders, wm@, db),
            db_step(db, db2, odv, ldv),
            odv == od_view(od@),
            ldv == ld_view(ld@),
            scan_contract(db2, skeys@, srows@),
            deltas_within(ikeys(a@), odv, ldv),
            distinct(ikeys(a@)),
            sups == st.sups@,
            m0 == *orders,
            fmd@.len() == na && fo@.len() == na && eo@.len() == na && tr@.len() == na && work@.len() == na && sw@.len() == na,
            forall|x: int| 0 <= x < na ==> (#[trigger] fmd@[x]) as int == sum(odv, od_w(a@[x] as int)),
            forall|x: int| 0 <= x < na ==> (#[trigger] tr@[x]) == om_get(m0, a@[x]).is_some(),
            forall|x: int| 0 <= x < na && #[trigger] tr@[x] ==> fo@[x] == om_get(m0, a@[x]).unwrap().fm && eo@[x]@ == om_get(m0, a@[x]).unwrap().ents@,
            forall|x: int| 0 <= x < na && !#[trigger] tr@[x] ==> fo@[x] == 0 && eo@[x]@ == Seq::<Ent>::empty(),
            forall|x: int| 0 <= x < na ==> edistinct((#[trigger] work@[x])@),
            forall|x: int, sk: int| 0 <= x < na ==> #[trigger] etot(work@[x]@, sk) == etot(work0[x]@, sk) + sum(ldv, ld_t(a@[x] as int, sk)),
            forall|x: int, sk: int| 0 <= x < na ==> #[trigger] elate(work@[x]@, sk) == elate(work0[x]@, sk) + sum(ldv, ld_l(a@[x] as int, sk)),
            forall|x: int| 0 <= x < na ==> (#[trigger] work0[x])@ == eo@[x]@,
            forall|x: int| 0 <= x < na ==> edistinct((#[trigger] sw@[x])@) && epos(sw@[x]@),
            forall|x: int, sk: int| 0 <= x < na ==> #[trigger] etot(sw@[x]@, sk) == cnt_rows(srows@, a@[x] as int, sk),
            forall|x: int, sk: int| 0 <= x < na ==> #[trigger] elate(sw@[x]@, sk) == lcnt_rows(srows@, a@[x] as int, sk),
            forall|k: i64| #[trigger] sset@.contains_key(k) ==> skeys@.contains(k),
            forall|sk: i64| #[trigger] wget(wm2@, sk) == wget(wm@, sk) + sum(ikeys(a@.take(j as int)), dpo(db, db2, sk as int)),
            om_step_ok(OrderMap { base, overlay, okeys }, m0, a@, j as int, db, db2),
        decreases na - j,
    {
        let ok = a[j];
        let ghost oki = ok as int;
        let f_old = fo[j];
        let f_new = match f_old.checked_add(fmd[j]) {
            Some(x) => x,
            None => {
                return Err(2);
            },
        };
        proof {
            // fm(db, ok) is the tracked multiplicity (or 0 when untracked)
            if tr@[j as int] {
                assert(ord_ok(om_get(m0, ok).unwrap(), db, oki));
            } else {
                assert(om_get(m0, ok).is_none());
            }
            assert(fm(db, oki) == f_old as int);
            lemma_step_counts(db, db2, odv, ldv, oki, 0);
            assert(fm(db2, oki) == f_new as int);
            lemma_fm_nonneg(db2, oki);
        }
        let is_tr = tr[j];
        let newrec: Option<OrdE> = if f_new == 0 {
            None
        } else if is_tr {
            proof {
                let o = om_get(m0, ok).unwrap();
                assert(ents_ok(o.ents@, db, oki));
                assert forall|q: int| 0 <= q < work@[j as int]@.len() implies (#[trigger] work@[j as int]@[q]).total >= 0 && work@[j as int]@[q].late >= 0 && (work@[j as int]@[q].total == 0 ==> work@[j as int]@[q].late == 0) by {
                    let e = work@[j as int]@;
                    let sk = e[q].sk as int;
                    lemma_etot_entry(e, q);
                    lemma_step_counts(db, db2, odv, ldv, oki, sk);
                    assert(etot(work0[j as int]@, sk) == cnt(db, oki, sk));
                    assert(elate(work0[j as int]@, sk) == lcnt(db, oki, sk));
                    lemma_cnt_nonneg(db2, oki, sk);
                }
            }
            let cleaned = ents_clean(&work[j]);
            proof {
                assert forall|sk: int| #[trigger] etot(cleaned@, sk) == cnt(db2, oki, sk) by {
                    lemma_step_counts(db, db2, odv, ldv, oki, sk);
                    assert(etot(work0[j as int]@, sk) == cnt(db, oki, sk));
                }
                assert forall|sk: int| #[trigger] elate(cleaned@, sk) == lcnt(db2, oki, sk) by {
                    lemma_step_counts(db, db2, odv, ldv, oki, sk);
                    assert(elate(work0[j as int]@, sk) == lcnt(db, oki, sk));
                }
            }
            Some(OrdE { fm: f_new, ents: cleaned })
        } else {
            if !sset.contains_key(&ok) {
                return Err(1);
            }
            proof {
                assert(skeys@.contains(ok));
                let q = choose|q: int| 0 <= q < skeys@.len() && skeys@[q] == ok;
                assert(ikeys(skeys@)[q] == oki);
                assert forall|sk: int| #[trigger] etot(sw@[j as int]@, sk) == cnt(db2, oki, sk) by {
                    lemma_scan_counts(db2, skeys@, srows@, oki, sk);
                }
                assert forall|sk: int| #[trigger] elate(sw@[j as int]@, sk) == lcnt(db2, oki, sk) by {
                    lemma_scan_counts(db2, skeys@, srows@, oki, sk);
                }
            }
            Some(OrdE { fm: f_new, ents: clone_ents(&sw[j]) })
        };
        proof {
            if newrec.is_some() {
                assert(ord_ok(newrec.unwrap(), db2, oki));
            } else {
                assert(fm(db2, oki) == 0);
            }
        }
        // contributions before / after
        let c_old = if is_tr {
            ents_contrib(&eo[j])
        } else {
            None
        };
        let c_new = match &newrec {
            Some(o) => ents_contrib(&o.ents),
            None => None,
        };
        proof {
            assert forall|sk: int| #[trigger] per_order(db, oki, sk) == f_old as int * opt_val(c_old, sk) by {
                if is_tr {
                    lemma_contrib_from_ents(eo@[j as int]@, db, oki, sk);
                } else {
                    assert(f_old as int * contrib(db, oki, sk) == 0) by (nonlinear_arith)
                        requires f_old == 0;
                    assert(f_old as int * 0 == 0) by (nonlinear_arith);
                }
            }
            assert forall|sk: int| #[trigger] per_order(db2, oki, sk) == f_new as int * opt_val(c_new, sk) by {
                if newrec.is_some() {
                    lemma_contrib_from_ents(newrec.unwrap().ents@, db2, oki, sk);
                } else {
                    assert(f_new as int * contrib(db2, oki, sk) == 0) by (nonlinear_arith)
                        requires f_new == 0;
                    assert(f_new as int * 0 == 0) by (nonlinear_arith);
                }
            }
        }
        let ghost wa = wm2@;
        // subtract the old contribution
        match c_old {
            Some((sa, v)) => {
                let prod = match f_old.checked_mul(v) {
                    Some(x) => x,
                    None => {
                        return Err(2);
                    },
                };
                let nv = match wget_exec(&wm2, sa).checked_sub(prod) {
                    Some(x) => x,
                    None => {
                        return Err(2);
                    },
                };
                wm2.insert(sa, nv);
            },
            None => {},
        }
        let ghost wb = wm2@;
        proof {
            assert forall|sk: i64| #[trigger] wget(wb, sk) == wget(wa, sk) - f_old as int * opt_val(c_old, sk as int) by {
                match c_old {
                    Some(p) => {
                        if sk != p.0 {
                            assert(f_old as int * 0 == 0) by (nonlinear_arith);
                        }
                    },
                    None => {
                        assert(f_old as int * 0 == 0) by (nonlinear_arith);
                    },
                }
            }
        }
        // add the new contribution
        match c_new {
            Some((sa, v)) => {
                let prod = match f_new.checked_mul(v) {
                    Some(x) => x,
                    None => {
                        return Err(2);
                    },
                };
                let nv = match wget_exec(&wm2, sa).checked_add(prod) {
                    Some(x) => x,
                    None => {
                        return Err(2);
                    },
                };
                wm2.insert(sa, nv);
            },
            None => {},
        }
        proof {
            assert forall|sk: i64| #[trigger] wget(wm2@, sk) == wget(wm@, sk) + sum(ikeys(a@.take(j as int + 1)), dpo(db, db2, sk as int)) by {
                assert(ikeys(a@.take(j as int + 1)) =~= ikeys(a@.take(j as int)).push(oki));
                lemma_sum_push(ikeys(a@.take(j as int)), oki, dpo(db, db2, sk as int));
                assert(wget(wb, sk) == wget(wa, sk) - f_old as int * opt_val(c_old, sk as int));
                match c_new {
                    Some(p) => {
                        if sk != p.0 {
                            assert(f_new as int * 0 == 0) by (nonlinear_arith);
                        }
                    },
                    None => {
                        assert(f_new as int * 0 == 0) by (nonlinear_arith);
                    },
                }
            }
        }
        // record the new state of this order
        let ghost mprev = OrderMap { base, overlay, okeys };
        overlay.insert(ok, newrec);
        okeys.push(ok);
        proof {
            let mnew = OrderMap { base, overlay, okeys };
            assert(a@.take(j as int + 1) =~= a@.take(j as int).push(ok));
            assert forall|k: i64| !a@.take(j as int + 1).contains(k) implies ord_eq(#[trigger] om_get(mnew, k), om_get(m0, k)) by {
                {
                    assert(k != ok) by {
                        if k == ok {
                            assert(a@.take(j as int + 1)[j as int] == ok);
                        }
                    }
                    assert(!a@.take(j as int).contains(k)) by {
                        if a@.take(j as int).contains(k) {
                            let q = choose|q: int| 0 <= q < j && #[trigger] a@.take(j as int)[q] == k;
                            assert(a@.take(j as int + 1)[q] == k);
                        }
                    }
                    assert(om_get(mnew, k) == om_get(mprev, k));
                }
            }
            assert forall|k: i64| a@.take(j as int + 1).contains(k) implies match #[trigger] om_get(mnew, k) {
                Some(o) => ord_ok(o, db2, k as int),
                None => fm(db2, k as int) == 0,
            } by {
                if k == ok {
                } else {
                    let q = choose|q: int| 0 <= q < j + 1 && #[trigger] a@.take(j as int + 1)[q] == k;
                    assert(q != j as int);
                    assert(a@.take(j as int)[q] == k);
                    assert(a@.take(j as int).contains(k));
                    assert(om_get(mnew, k) == om_get(mprev, k));
                }
            }
            assert forall|k: i64| #[trigger] mnew.overlay@.contains_key(k) implies mnew.okeys@.contains(k) by {
                if k == ok {
                    assert(mnew.okeys@[mnew.okeys@.len() - 1] == ok);
                } else {
                    assert(mprev.overlay@.contains_key(k));
                    assert(mprev.okeys@.contains(k));
                    let q = choose|q: int| 0 <= q < mprev.okeys@.len() && mprev.okeys@[q] == k;
                    assert(mnew.okeys@[q] == k);
                }
            }
        }
        j += 1;
    }
    let m2 = OrderMap { base, overlay, okeys };
    proof {
        assert(a@.take(na as int) =~= a@);
        // W for db2
        assert forall|k: i64| #[trigger] wget(wm2@, k) == w(db2, k as int) by {
            let sk = k as int;
            lemma_w_step(db, db2, odv, ldv, ikeys(a@), sk);
            lemma_sum_ext_all(
                ikeys(a@),
                |ok: int| per_order(db2, ok, sk) - per_order(db, ok, sk),
                dpo(db, db2, sk),
            );
        }
        // orders invariant for db2
        assert forall|k: i64| match #[trigger] om_get(m2, k) {
            Some(o) => ord_ok(o, db2, k as int),
            None => fm(db2, k as int) == 0,
        } by {
            if !a@.contains(k) {
                assert(!ikeys(a@).contains(k as int)) by {
                    if ikeys(a@).contains(k as int) {
                        let q = choose|q: int| 0 <= q < ikeys(a@).len() && ikeys(a@)[q] == k as int;
                        assert(a@[q] == k);
                    }
                }
                lemma_unchanged_order(db, db2, odv, ldv, ikeys(a@), k as int);
                assert(!a@.take(na as int).contains(k));
                assert(ord_eq(om_get(m2, k), om_get(m0, k)));
                if om_get(m0, k).is_some() {
                    let o = om_get(m0, k).unwrap();
                    let o2 = om_get(m2, k).unwrap();
                    assert(ord_ok(o, db, k as int));
                    assert forall|sk: int| #[trigger] etot(o2.ents@, sk) == cnt(db2, k as int, sk) by {
                        assert(cnt(db2, k as int, sk) == cnt(db, k as int, sk));
                    }
                    assert forall|sk: int| #[trigger] elate(o2.ents@, sk) == lcnt(db2, k as int, sk) by {
                        assert(lcnt(db2, k as int, sk) == lcnt(db, k as int, sk));
                    }
                }
            }
        }
        assert forall|ok: int| #[trigger] fm(db2, ok) > 0 implies i64::MIN <= ok <= i64::MAX by {
            if !(i64::MIN <= ok <= i64::MAX) {
                lemma_fm_nonneg(db, ok);
                lemma_step_counts(db, db2, odv, ldv, ok, 0);
                assert forall|q: int| 0 <= q < odv.len() implies od_w(ok)(#[trigger] odv[q]) == 0 by {
                    assert(odv[q].0.orderkey == od@[q].orderkey as int);
                }
                lemma_sum_zero(odv, od_w(ok));
            }
        }
    }
    // supplier / nation changes: apply them to the (small) static part
    let st2 = if sd.len() == 0 && nd.len() == 0 {
        proof {
            lemma_bag_step_empty(db.supplier, db2.supplier, dview(sd@));
            lemma_bag_step_empty(db.nation, db2.nation, dview(nd@));
        }
        s.static_arc()
    } else {
        let ns = match apply_bag(&st.sups, sd) {
            Some(x) => x,
            None => {
                return Err(2);
            },
        };
        let nn = match apply_bag(&st.nats, nd) {
            Some(x) => x,
            None => {
                return Err(2);
            },
        };
        if ns.len() >= usize::MAX - 1 {
            return Err(2);
        }
        let nat_sups = build_nat_sups(&ns, &nn);
        proof {
            lemma_rows_v_sups(st.sups@);
            lemma_rows_v_sups(ns@);
            lemma_rows_v_nats(st.nats@);
            lemma_rows_v_nats(nn@);
            lemma_apply_bag_step(st.sups@, ns@, sd@, db.supplier, db2.supplier);
            lemma_apply_bag_step(st.nats@, nn@, nd@, db.nation, db2.nation);
        }
        std::sync::Arc::new(Static { sups: ns, nats: nn, nat_sups })
    };
    proof {
        assert(static_ok(*st2, db2));
    }
    if m2.okeys.len() > COMPACT_THRESHOLD {
        let m3 = compact(&m2);
        proof {
            assert forall|k: i64| match #[trigger] om_get(m3, k) {
                Some(o) => ord_ok(o, db2, k as int),
                None => fm(db2, k as int) == 0,
            } by {
                assert(ord_eq(om_get(m3, k), om_get(m2, k)));
                match om_get(m2, k) {
                    Some(o) => {
                        assert(ord_ok(o, db2, k as int));
                        let o3 = om_get(m3, k).unwrap();
                        assert(o3.ents@ == o.ents@);
                    },
                    None => {},
                }
            }
        }
        return Ok(State::new(st2, m3, wm2, Ghost(db2)));
    }
    Ok(State::new(st2, m2, wm2, Ghost(db2)))
}

} // verus!
