// Execute: answer Q21 for a nation name from the state.
//
//   1. G = supplier rows of the nations named p with positive W
//   2. sort G by s_name, merge runs            (GROUP BY s_name)
//   3. sort groups by (numwait DESC, s_name)   (ORDER BY)
use vstd::prelude::*;
use verus_common::sums::*;
use crate::spec::*;
use crate::bridge::*;
use verus_common::bytes::*;
use verus_common::sort::*;
use crate::state::*;
use std::collections::HashMap;

verus! {

pub open spec fn out_view(o: Seq<(Vec<u8>, i64)>) -> Seq<(Seq<u8>, int)> {
    o.map_values(|r: (Vec<u8>, i64)| (r.0@, r.1 as int))
}

pub open spec fn sname(sups: Seq<SupE>, i: usize) -> Seq<u8> {
    sups[i as int].name@
}

/// Weight of supplier row i in group g.
pub open spec fn hg(sups: Seq<SupE>, db: Db, g: Seq<u8>) -> spec_fn(usize) -> int {
    |i: usize| if (i as int) < sups.len() && sups[i as int].name@ == g { w(db, sups[i as int].sk as int) } else { 0 }
}

/// Contribution of nation row jj to group g.
pub open spec fn outer_term(st: Static, db: Db, p: Seq<u8>, g: Seq<u8>) -> spec_fn(int) -> int {
    |jj: int| if st.nats@[jj].name@ == p { sum(st.nat_sups@[jj]@, hg(st.sups@, db, g)) } else { 0 }
}

pub open spec fn idxs(n: int) -> Seq<usize> {
    Seq::new(n as nat, |i: int| i as usize)
}

proof fn lemma_sum_idxs(n: int, f: spec_fn(usize) -> int)
    requires
        0 <= n <= usize::MAX,
    ensures
        sum(idxs(n), f) == sum(rng(n), |i: int| f(i as usize)),
{
    assert(idxs(n) =~= rng(n).map_values(|i: int| i as usize));
    lemma_sum_map(rng(n), |i: int| i as usize, f, |i: int| f(i as usize));
}

pub open spec fn tj(st: Static, db: Db, p: Seq<u8>, g: Seq<u8>) -> spec_fn(int, int) -> int {
    |jj: int, i: int|
        if st.nats@[jj].name@ == p && st.sups@[i].nk == st.nats@[jj].nk {
            hg(st.sups@, db, g)(i as usize)
        } else {
            0
        }
}

/// Σ over the gathered rows is numwait.
proof fn lemma_gather_total(st: Static, orders: OrderMap, wm: Map<i64, i64>, db: Db, p: Seq<u8>, g: Seq<u8>)
    requires
        state_ok(st, orders, wm, db),
    ensures
        sum(rng(st.nats@.len() as int), outer_term(st, db, p, g)) == numwait(db, p, g),
{
    let sups = st.sups@;
    let nats = st.nats@;
    let ns = sups.len() as int;
    let nn = nats.len() as int;
    let h = hg(sups, db, g);
    let t = tj(st, db, p, g);
    // (a) each nation row: Σ over its supplier list = Σ_i t(jj, i)
    let ra = |jj: int| sum(rng(ns), |i: int| t(jj, i));
    assert forall|jj: int| 0 <= jj < nn implies #[trigger] outer_term(st, db, p, g)(jj) == ra(jj) by {
        let nk = nats[jj].nk;
        let pred = |i: usize| sups[i as int].nk == nk;
        let hf = |i: usize| if pred(i) { h(i) } else { 0 };
        assert(st.nat_sups@[jj]@ == nat_filter(sups, nk, ns));
        lemma_sum_filter(idxs(ns), pred, h, hf);
        lemma_sum_idxs(ns, hf);
        if nats[jj].name@ == p {
            assert forall|k: int| 0 <= k < ns implies (|i: int| hf(i as usize))(#[trigger] rng(ns)[k]) == (|i: int| t(jj, i))(rng(ns)[k]) by {
                assert(rng(ns)[k] == k);
                assert((k as usize) as int == k);
            }
            lemma_sum_ext(rng(ns), |i: int| hf(i as usize), |i: int| t(jj, i));
        } else {
            lemma_sum_ext_all(rng(ns), |i: int| t(jj, i), |i: int| 0int);
            lemma_sum_zero(rng(ns), |i: int| 0int);
        }
    }
    lemma_sum_ext(rng(nn), outer_term(st, db, p, g), ra);
    // (b) swap
    let cb = |i: int| sum(rng(nn), |jj: int| t(jj, i));
    assert forall|x: int| #[trigger] ra(x) == sum(rng(ns), |i: int| t(x, i)) by {}
    assert forall|y: int| #[trigger] cb(y) == sum(rng(nn), |jj: int| t(jj, y)) by {}
    lemma_sum_swap(rng(nn), rng(ns), t, ra, cb);
    // (c) Σ_jj t(jj, i) = h(i) · nm(s_nationkey, p)
    let st_i = |i: int| sup_term(db, p, g, sups_view(sups)[i]);
    assert forall|i: int| 0 <= i < ns implies #[trigger] cb(i) == st_i(i) by {
        let s = sups_view(sups)[i];
        assert(s == sup_view(sups[i]));
        let ind_n = |n: Nation| ind(n.nationkey == s.nationkey && n.name == p);
        let ind_j = |jj: int| ind_n(nats_view(nats)[jj]);
        lemma_sum_index(nats_view(nats), ind_n, ind_j);
        lemma_sum_perm(nats_view(nats), db.nation, ind_n);
        let c = h(i as usize);
        assert forall|jj: int| 0 <= jj < nn implies #[trigger] t(jj, i) == c * ind_j(jj) by {
            assert(nats_view(nats)[jj] == nat_view(nats[jj]));
        }
        // Σ_jj t(jj, i) = c · Σ_jj ind_j(jj)
        let tjs = |jj: int| t(jj, i);
        let cj = |jj: int| c * ind_j(jj);
        lemma_sum_ext(rng(nn), tjs, cj);
        lemma_sum_scale(rng(nn), ind_j, cj, c);
        lemma_w_nonneg(db, s.suppkey);
        if sups[i].name@ == g {
            let x = nm(db, s.nationkey, p);
            let y = w(db, s.suppkey);
            assert(c == y);
            assert(c * x == x * y) by (nonlinear_arith)
                requires
                    c == y,
            ;
        } else {
            assert(c == 0);
            assert(0 * nm(db, s.nationkey, p) == 0) by (nonlinear_arith);
        }
    }
    lemma_sum_ext(rng(ns), cb, st_i);
    // (d) Σ_i over supplier rows = numwait (bridge)
    lemma_sum_index(sups_view(sups), |s: Supplier| sup_term(db, p, g, s), st_i);
    lemma_sum_perm(sups_view(sups), db.supplier, |s: Supplier| sup_term(db, p, g, s));
    lemma_numwait_by_supplier(db, p, g);
}

pub open spec fn uind(v: usize) -> spec_fn(usize) -> int {
    |x: usize| ind(x == v)
}

/// A sorted permutation of 0..n contains each index exactly once.
proof fn lemma_perm_once(order: Seq<usize>, n: int, v: usize)
    requires
        0 <= n <= usize::MAX,
        same_bag(order, idxs(n)),
        (v as int) < n,
    ensures
        sum(order, uind(v)) == 1,
{
    assert(sum(order, uind(v)) == sum(idxs(n), uind(v)));
    lemma_sum_idxs(n, uind(v));
    let f = |j: int| uind(v)(j as usize);
    assert forall|i: int| 0 <= i < n && i != v as int implies f(#[trigger] rng(n)[i]) == 0 by {}
    lemma_sum_single_match(rng(n), f, v as int);
}

proof fn lemma_once_distinct(order: Seq<usize>, i: int, j: int)
    requires
        0 <= i < j < order.len(),
        sum(order, uind(order[i])) == 1,
    ensures
        order[i] != order[j],
{
    let v = order[i];
    if order[j] == v {
        let f = uind(v);
        lemma_sum_remove(order, j, f);
        assert forall|k: int| 0 <= k < order.remove(j).len() implies f(#[trigger] order.remove(j)[k]) >= 0 by {}
        lemma_sum_nonneg(order.remove(j), f);
        assert(order.remove(j)[i] == v);
        lemma_sum_pos(order.remove(j), f, i);
    }
}

// ── ordering helpers ───────────────────────────────────────────────────────
pub open spec fn name_le(sups: Seq<SupE>) -> spec_fn(usize, usize) -> bool {
    |a: usize, b: usize| !bytes_lt(sups[b as int].name@, sups[a as int].name@)
}

pub open spec fn valid_sup(sups: Seq<SupE>) -> spec_fn(usize) -> bool {
    |i: usize| (i as int) < sups.len()
}

proof fn lemma_name_le_preorder(sups: Seq<SupE>)
    ensures
        preorder_on(name_le(sups), valid_sup(sups)),
{
    let le = name_le(sups);
    assert forall|a: usize, b: usize| #![trigger le(a, b)] valid_sup(sups)(a) && valid_sup(sups)(b) implies le(a, b) || le(b, a) by {
        lemma_bytes_lt_asym(sups[a as int].name@, sups[b as int].name@);
    }
    assert forall|a: usize, b: usize, c: usize|
        valid_sup(sups)(a) && valid_sup(sups)(b) && valid_sup(sups)(c) && #[trigger] le(a, b) && #[trigger] le(b, c) implies le(a, c) by {
        let na = sups[a as int].name@;
        let nb = sups[b as int].name@;
        let nc = sups[c as int].name@;
        if bytes_lt(nc, na) {
            if na == nb {
            } else {
                lemma_bytes_lt_total(na, nb);
                lemma_bytes_lt_trans(nc, na, nb);
            }
        }
    }
}

pub open spec fn rowv(sups: Seq<SupE>, rows: Seq<(usize, i64)>, r: usize) -> (Seq<u8>, int) {
    (sups[rows[r as int].0 as int].name@, rows[r as int].1 as int)
}

pub open spec fn row_le(sups: Seq<SupE>, rows: Seq<(usize, i64)>) -> spec_fn(usize, usize) -> bool {
    |a: usize, b: usize| !row_before(rowv(sups, rows, b), rowv(sups, rows, a))
}

pub open spec fn valid_row(rows: Seq<(usize, i64)>) -> spec_fn(usize) -> bool {
    |i: usize| (i as int) < rows.len()
}

proof fn lemma_row_before_asym(x: (Seq<u8>, int), y: (Seq<u8>, int))
    ensures
        !(row_before(x, y) && row_before(y, x)),
{
    lemma_bytes_lt_asym(x.0, y.0);
}

proof fn lemma_row_before_total(x: (Seq<u8>, int), y: (Seq<u8>, int))
    requires
        x.0 != y.0,
    ensures
        row_before(x, y) || row_before(y, x),
{
    lemma_bytes_lt_total(x.0, y.0);
}

proof fn lemma_row_le_preorder(sups: Seq<SupE>, rows: Seq<(usize, i64)>)
    ensures
        preorder_on(row_le(sups, rows), valid_row(rows)),
{
    let le = row_le(sups, rows);
    assert forall|a: usize, b: usize| #![trigger le(a, b)] valid_row(rows)(a) && valid_row(rows)(b) implies le(a, b) || le(b, a) by {
        lemma_row_before_asym(rowv(sups, rows, a), rowv(sups, rows, b));
    }
    assert forall|a: usize, b: usize, c: usize|
        valid_row(rows)(a) && valid_row(rows)(b) && valid_row(rows)(c) && #[trigger] le(a, b) && #[trigger] le(b, c) implies le(a, c) by {
        let xa = rowv(sups, rows, a);
        let xb = rowv(sups, rows, b);
        let xc = rowv(sups, rows, c);
        // not (c < b), not (b < a) ⇒ not (c < a)
        if row_before(xc, xa) {
            if xa.1 != xb.1 {
            } else if xa.0 == xb.0 {
            } else {
                lemma_bytes_lt_total(xa.0, xb.0);
                if xc.1 == xa.1 {
                    lemma_bytes_lt_trans(xc.0, xa.0, xb.0);
                }
            }
        }
    }
}

// ── gather ─────────────────────────────────────────────────────────────────
fn gather(st: &Static, wm: &HashMap<i64, i64>, p: &[u8], Ghost(db): Ghost<Db>, Ghost(orders): Ghost<OrderMap>) -> (gv: Vec<usize>)
    requires
        state_ok(*st, orders, wm@, db),
    ensures
        all_valid(gv@, valid_sup(st.sups@)),
        forall|i: int| 0 <= i < gv@.len() ==> w(db, st.sups@[gv@[i] as int].sk as int) > 0,
        forall|g: Seq<u8>| #[trigger] sum(gv@, hg(st.sups@, db, g)) == numwait(db, p@, g),
{
    let sups = &st.sups;
    let nats = &st.nats;
    let mut gv: Vec<usize> = Vec::new();
    let nn = nats.len();
    let mut j: usize = 0;
    while j < nn
        invariant
            0 <= j <= nn == nats@.len(),
            state_ok(*st, orders, wm@, db),
            *sups == st.sups,
            *nats == st.nats,
            all_valid(gv@, valid_sup(st.sups@)),
            forall|i: int| 0 <= i < gv@.len() ==> w(db, st.sups@[gv@[i] as int].sk as int) > 0,
            forall|g: Seq<u8>| #[trigger] sum(gv@, hg(st.sups@, db, g)) == sum(rng(j as int), outer_term(*st, db, p@, g)),
        decreases nn - j,
    {
        let ghost g0 = gv@;
        proof {
            assert(rng(j as int + 1).drop_last() =~= rng(j as int));
        }
        if bytes_eq_exec(nats[j].name.as_slice(), p) {
            let list = &st.nat_sups[j];
            proof {
                assert(st.nat_sups@[j as int]@ == nat_filter(st.sups@, st.nats@[j as int].nk, st.sups@.len() as int));
            }
            let m = list.len();
            let mut t: usize = 0;
            while t < m
                invariant
                    0 <= t <= m == list@.len(),
                    j < nn == nats@.len(),
                    list@ == nat_filter(st.sups@, st.nats@[j as int].nk, st.sups@.len() as int),
                    *sups == st.sups,
                    state_ok(*st, orders, wm@, db),
                    all_valid(gv@, valid_sup(st.sups@)),
                    forall|i: int| 0 <= i < gv@.len() ==> w(db, st.sups@[gv@[i] as int].sk as int) > 0,
                    forall|g: Seq<u8>| #[trigger] sum(gv@, hg(st.sups@, db, g)) == sum(g0, hg(st.sups@, db, g)) + sum(list@.take(t as int), hg(st.sups@, db, g)),
                decreases m - t,
            {
                let i = list[t];
                proof {
                    // list entries are supplier indices
                    broadcast use vstd::seq_lib::group_filter_ensures;
                    let full = idxs(st.sups@.len() as int);
                    let pr = |x: usize| st.sups@[x as int].nk == st.nats@[j as int].nk;
                    assert(list@ == full.filter(pr));
                    assert(list@.contains(i)) by { assert(list@[t as int] == i); }
                    full.lemma_filter_contains_rev(pr, i);
                    assert(full.contains(i));
                    let k = choose|k: int| 0 <= k < full.len() && full[k] == i;
                    assert(full[k] == k as usize);
                    assert((i as int) < st.sups@.len());
                    assert(list@.take(t as int + 1) =~= list@.take(t as int).push(i));
                    lemma_w_nonneg(db, st.sups@[i as int].sk as int);
                }
                let ghost gb = gv@;
                let wi = wget_exec(wm, sups[i].sk);
                if wi > 0 {
                    gv.push(i);
                    proof {
                        assert forall|g: Seq<u8>| #[trigger] sum(gv@, hg(st.sups@, db, g)) == sum(g0, hg(st.sups@, db, g)) + sum(list@.take(t as int + 1), hg(st.sups@, db, g)) by {
                            lemma_sum_push(gb, i, hg(st.sups@, db, g));
                            lemma_sum_push(list@.take(t as int), i, hg(st.sups@, db, g));
                        }
                    }
                } else {
                    proof {
                        assert forall|g: Seq<u8>| #[trigger] sum(gv@, hg(st.sups@, db, g)) == sum(g0, hg(st.sups@, db, g)) + sum(list@.take(t as int + 1), hg(st.sups@, db, g)) by {
                            lemma_sum_push(list@.take(t as int), i, hg(st.sups@, db, g));
                        }
                    }
                }
                t += 1;
            }
            proof {
                assert(list@.take(m as int) =~= list@);
                assert forall|g: Seq<u8>| #[trigger] sum(gv@, hg(st.sups@, db, g)) == sum(rng(j as int + 1), outer_term(*st, db, p@, g)) by {
                    lemma_sum_push(rng(j as int), j as int, outer_term(*st, db, p@, g));
                    assert(rng(j as int + 1) == rng(j as int).push(j as int));
                }
            }
        } else {
            proof {
                assert forall|g: Seq<u8>| #[trigger] sum(gv@, hg(st.sups@, db, g)) == sum(rng(j as int + 1), outer_term(*st, db, p@, g)) by {
                    lemma_sum_push(rng(j as int), j as int, outer_term(*st, db, p@, g));
                    assert(rng(j as int + 1) == rng(j as int).push(j as int));
                }
            }
        }
        j += 1;
    }
    proof {
        assert forall|g: Seq<u8>| #[trigger] sum(gv@, hg(st.sups@, db, g)) == numwait(db, p@, g) by {
            lemma_gather_total(*st, orders, wm@, db, p@, g);
        }
    }
    gv
}

// ── group by name ──────────────────────────────────────────────────────────
pub open spec fn row_w(sups: Seq<SupE>, g: Seq<u8>) -> spec_fn((usize, i64)) -> int {
    |r: (usize, i64)| if sups[r.0 as int].name@ == g { r.1 as int } else { 0 }
}

pub open spec fn rows_ok(sups: Seq<SupE>, rows: Seq<(usize, i64)>) -> bool {
    &&& forall|a: int| 0 <= a < rows.len() ==> ((#[trigger] rows[a]).0 as int) < sups.len() && rows[a].1 > 0
    &&& forall|a: int, b: int|
        0 <= a < b < rows.len() ==> bytes_lt(sups[(#[trigger] rows[a]).0 as int].name@, sups[(#[trigger] rows[b]).0 as int].name@)
}

proof fn lemma_row_w_single(sups: Seq<SupE>, rows: Seq<(usize, i64)>, a: int)
    requires
        rows_ok(sups, rows),
        0 <= a < rows.len(),
    ensures
        sum(rows, row_w(sups, sups[rows[a].0 as int].name@)) == rows[a].1,
{
    let g = sups[rows[a].0 as int].name@;
    assert forall|b: int| 0 <= b < rows.len() && b != a implies row_w(sups, g)(#[trigger] rows[b]) == 0 by {
        let nb = sups[rows[b].0 as int].name@;
        if b < a {
            assert(bytes_lt(nb, g));
        } else {
            assert(bytes_lt(g, nb));
        }
        lemma_bytes_lt_irrefl(g);
    }
    lemma_sum_single_match(rows, row_w(sups, g), a);
}

fn group_by_name(st: &Static, wm: &HashMap<i64, i64>, gs: &Vec<usize>, Ghost(db): Ghost<Db>) -> (r: Option<Vec<(usize, i64)>>)
    requires
        w_ok(wm@, db),
        all_valid(gs@, valid_sup(st.sups@)),
        sorted_le(gs@, name_le(st.sups@)),
    ensures
        r matches Some(rows) ==> {
            &&& rows_ok(st.sups@, rows@)
            &&& forall|g: Seq<u8>| #[trigger] sum(rows@, row_w(st.sups@, g)) == sum(gs@, hg(st.sups@, db, g))
        },
    // (requires every gathered weight to be positive)
{
    let sups = &st.sups;
    let mut rows: Vec<(usize, i64)> = Vec::new();
    let n = gs.len();
    let mut t: usize = 0;
    while t < n
        invariant
            0 <= t <= n == gs@.len(),
            *sups == st.sups,
            w_ok(wm@, db),
            all_valid(gs@, valid_sup(st.sups@)),
            sorted_le(gs@, name_le(st.sups@)),
            rows_ok(st.sups@, rows@),
            forall|g: Seq<u8>| #[trigger] sum(rows@, row_w(st.sups@, g)) == sum(gs@.take(t as int), hg(st.sups@, db, g)),
            t > 0 ==> rows@.len() > 0 && st.sups@[rows@.last().0 as int].name@ == st.sups@[gs@[t - 1] as int].name@,
            t == 0 ==> rows@.len() == 0,
        decreases n - t,
    {
        let x = gs[t];
        proof {
            assert(valid_sup(st.sups@)(x));
        }
        let wx = wget_exec(wm, sups[x].sk);
        if wx <= 0 {
            // gathered rows all have positive weight; defensive
            return None;
        }
        let ghost r0 = rows@;
        proof {
            assert(valid_sup(st.sups@)(x));
            assert(gs@.take(t as int + 1) =~= gs@.take(t as int).push(x));
        }
        let rl = rows.len();
        let same = if rl > 0 {
            bytes_eq_exec(sups[rows[rl - 1].0].name.as_slice(), sups[x].name.as_slice())
        } else {
            false
        };
        if same {
            let last = rows[rl - 1];
            let nt = match last.1.checked_add(wx) {
                Some(v) => v,
                None => {
                    return None;
                },
            };
            rows.set(rl - 1, (last.0, nt));
            proof {
                assert forall|g: Seq<u8>| #[trigger] sum(rows@, row_w(st.sups@, g)) == sum(gs@.take(t as int + 1), hg(st.sups@, db, g)) by {
                    lemma_sum_update(r0, rl as int - 1, (last.0, nt), row_w(st.sups@, g));
                    lemma_sum_push(gs@.take(t as int), x, hg(st.sups@, db, g));
                }
                assert(rows_ok(st.sups@, rows@)) by {
                    assert forall|a: int, b: int| 0 <= a < b < rows@.len() implies bytes_lt(st.sups@[(#[trigger] rows@[a]).0 as int].name@, st.sups@[(#[trigger] rows@[b]).0 as int].name@) by {
                        assert(rows@[a].0 == r0[a].0 && rows@[b].0 == r0[b].0);
                    }
                }
            }
        } else {
            rows.push((x, wx));
            proof {
                let nx = st.sups@[x as int].name@;
                if t > 0 {
                    // previous gathered name ≤ nx, and differs from it ⇒ strictly less
                    let prev = st.sups@[gs@[t - 1] as int].name@;
                    assert(name_le(st.sups@)(gs@[t - 1], x));
                    assert(prev != nx);
                    lemma_bytes_lt_total(prev, nx);
                    assert(bytes_lt(prev, nx));
                    assert forall|a: int| 0 <= a < r0.len() implies bytes_lt(st.sups@[(#[trigger] r0[a]).0 as int].name@, nx) by {
                        if a < r0.len() - 1 {
                            lemma_bytes_lt_trans(st.sups@[r0[a].0 as int].name@, prev, nx);
                        }
                    }
                }
                assert(rows_ok(st.sups@, rows@)) by {
                    assert forall|a: int, b: int| 0 <= a < b < rows@.len() implies bytes_lt(st.sups@[(#[trigger] rows@[a]).0 as int].name@, st.sups@[(#[trigger] rows@[b]).0 as int].name@) by {
                        if b < r0.len() {
                            assert(rows@[a] == r0[a] && rows@[b] == r0[b]);
                        } else {
                            assert(rows@[a] == r0[a]);
                        }
                    }
                }
                assert forall|g: Seq<u8>| #[trigger] sum(rows@, row_w(st.sups@, g)) == sum(gs@.take(t as int + 1), hg(st.sups@, db, g)) by {
                    lemma_sum_push(r0, (x, wx), row_w(st.sups@, g));
                    lemma_sum_push(gs@.take(t as int), x, hg(st.sups@, db, g));
                }
            }
        }
        t += 1;
    }
    proof {
        assert(gs@.take(n as int) =~= gs@);
    }
    Some(rows)
}

// ── Execute ────────────────────────────────────────────────────────────────
pub fn execute(s: &State, p: &[u8]) -> (r: Option<Vec<(Vec<u8>, i64)>>)
    ensures
        r matches Some(out) ==> q21_result(s.db(), p@, out_view(out@)),
{
    let (st, orders, wm) = s.parts();
    let ghost db = s.db();
    let gv = gather(st, wm, p, Ghost(db), Ghost(*orders));
    let sups = &st.sups;
    let ns = sups.len();
    // 2. sort by name
    let name_cmp = |a: usize, b: usize| -> (r: bool)
        requires
            a < ns && b < ns,
        ensures
            r == name_le(sups@)(a, b),
        { !bytes_lt_exec(sups[b].name.as_slice(), sups[a].name.as_slice()) };
    proof {
        lemma_name_le_preorder(sups@);
        assert(cmp_ok(&name_cmp, name_le(sups@), valid_sup(sups@)));
    }
    let gs = sort_idx(&gv, &name_cmp, Ghost(name_le(sups@)), Ghost(valid_sup(sups@)));
    // 3. group
    let rows = match group_by_name(st, wm, &gs, Ghost(db)) {
        Some(r) => r,
        None => {
            return None;
        },
    };
    proof {
        assert forall|g: Seq<u8>| #[trigger] sum(rows@, row_w(sups@, g)) == numwait(db, p@, g) by {
            assert(sum(gs@, hg(sups@, db, g)) == sum(gv@, hg(sups@, db, g)));
        }
    }
    // 4. order groups
    let nr = rows.len();
    let mut perm: Vec<usize> = Vec::new();
    let mut k: usize = 0;
    while k < nr
        invariant
            0 <= k <= nr == rows@.len(),
            perm@ == idxs(k as int),
        decreases nr - k,
    {
        perm.push(k);
        proof {
            assert(perm@ =~= idxs(k as int + 1));
        }
        k += 1;
    }
    let rws = &rows;
    let row_cmp = |a: usize, b: usize| -> (r: bool)
        requires
            a < nr && b < nr && rws@.len() == nr && forall|x: int| 0 <= x < nr ==> ((#[trigger] rws@[x]).0 as int) < ns,
        ensures
            r == row_le(sups@, rws@)(a, b),
        {
            let ra = rws[a];
            let rb = rws[b];
            // not (b before a)
            if rb.1 > ra.1 {
                false
            } else if rb.1 < ra.1 {
                true
            } else {
                !bytes_lt_exec(sups[rb.0].name.as_slice(), sups[ra.0].name.as_slice())
            }
        };
    proof {
        lemma_row_le_preorder(sups@, rows@);
        assert(cmp_ok(&row_cmp, row_le(sups@, rows@), valid_row(rows@)));
        assert(all_valid(perm@, valid_row(rows@)));
    }
    let order = sort_idx(&perm, &row_cmp, Ghost(row_le(sups@, rows@)), Ghost(valid_row(rows@)));
    // 5. emit
    let mut out: Vec<(Vec<u8>, i64)> = Vec::new();
    let no = order.len();
    let mut q: usize = 0;
    while q < no
        invariant
            0 <= q <= no == order@.len(),
            all_valid(order@, valid_row(rows@)),
            rows_ok(sups@, rows@),
            out@.len() == q,
            forall|a: int| 0 <= a < q ==> out_view(out@)[a] == rowv(sups@, rows@, #[trigger] order@[a]),
        decreases no - q,
    {
        let ri = order[q];
        proof {
            assert(valid_row(rows@)(ri));
        }
        let rr = rows[ri];
        let nm = sups[rr.0].name.clone();
        proof {
            assert(nm@ =~= sups@[rr.0 as int].name@);
        }
        let ghost o0 = out@;
        out.push((nm, rr.1));
        proof {
            assert forall|a: int| 0 <= a < q + 1 implies out_view(out@)[a] == rowv(sups@, rows@, #[trigger] order@[a]) by {
                if a < q {
                    assert(out@[a] == o0[a]);
                    assert(out_view(o0)[a] == rowv(sups@, rows@, order@[a]));
                }
            }
        }
        q += 1;
    }
    proof {
        let ov = out_view(out@);
        // order is a permutation of 0..nr: each index occurs exactly once
        assert forall|i: int, j: int| 0 <= i < j < order@.len() implies order@[i] != order@[j] by {
            assert(valid_row(rows@)(order@[i]));
            lemma_perm_once(order@, nr as int, order@[i]);
            lemma_once_distinct(order@, i, j);
        }
        // strict order
        assert forall|i: int, j: int| 0 <= i < j < ov.len() implies row_before(#[trigger] ov[i], #[trigger] ov[j]) by {
            let a = order@[i];
            let b = order@[j];
            assert(row_le(sups@, rows@)(a, b));
            assert(a != b);
            assert(valid_row(rows@)(a) && valid_row(rows@)(b));
            // distinct rows have distinct names
            if (a as int) < (b as int) {
                assert(bytes_lt(sups@[rows@[a as int].0 as int].name@, sups@[rows@[b as int].0 as int].name@));
            } else {
                assert(bytes_lt(sups@[rows@[b as int].0 as int].name@, sups@[rows@[a as int].0 as int].name@));
            }
            lemma_bytes_lt_irrefl(sups@[rows@[a as int].0 as int].name@);
            lemma_row_before_total(rowv(sups@, rows@, a), rowv(sups@, rows@, b));
        }
        // each row is its group
        assert forall|i: int| 0 <= i < ov.len() implies (#[trigger] ov[i]).1 == numwait(db, p@, ov[i].0) && ov[i].1 > 0 by {
            let a = order@[i];
            assert(valid_row(rows@)(a));
            lemma_row_w_single(sups@, rows@, a as int);
        }
        // every group appears
        assert forall|g: Seq<u8>| #[trigger] numwait(db, p@, g) > 0 implies exists|i: int| 0 <= i < ov.len() && (#[trigger] ov[i]).0 == g by {
            let a = lemma_sum_nonzero_witness(rows@, row_w(sups@, g));
            let v = a as usize;
            lemma_perm_once(order@, nr as int, v);
            let i = lemma_sum_nonzero_witness(order@, uind(v));
            assert(order@[i] == v);
            assert(ov[i].0 == g);
        }
    }
    Some(out)
}

} // verus!
