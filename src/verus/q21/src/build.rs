// Build: construct the state from full scans of the four tables.
use vstd::prelude::*;
use std::collections::HashMap;
use std::sync::Arc;
use verus_common::sums::*;
use verus_common::bytes::*;
use verus_common::sort::*;
use crate::gen_spec::*;
use crate::bridge::*;
use crate::orderspec::*;
use crate::ents::*;
use crate::state::*;

verus! {

broadcast use vstd::std_specs::hash::group_hash_axioms;

/// What the host's scans deliver (trusted: the scan SQL implements this).
pub open spec fn build_contract(
    sups: Seq<SupE>,
    nats: Seq<NatE>,
    fkeys: Seq<i64>,
    lis: Seq<LiRow>,
    db: Db,
) -> bool {
    // SELECT s_suppkey, s_name, s_nationkey FROM supplier
    &&& sups_view(sups) == db.supplier
    // SELECT n_nationkey, n_name FROM nation
    &&& nats_view(nats) == db.nation
    // SELECT o_orderkey FROM orders WHERE o_orderstatus = 'F'   (as a bag)
    &&& forall|f: spec_fn(int) -> int| #[trigger]
        sum(ikeys(fkeys), f) == sum(
            db.orders,
            |o: OrdersRow| if is_f(o) { f(o.o_orderkey) } else { 0 },
        )
    // SELECT l_orderkey, l_suppkey, l_commitdate, l_receiptdate FROM lineitem l
    //  WHERE EXISTS (SELECT 1 FROM orders o
    //                WHERE o.o_orderkey = l.l_orderkey AND o.o_orderstatus = 'F')
    &&& forall|f: spec_fn(LineitemRow) -> int| #[trigger]
        sum(lis_view(lis), f) == sum(
            db.lineitem,
            |l: LineitemRow| if fm(db, l.l_orderkey) > 0 { f(l) } else { 0 },
        )
}

// Widens the scanned order keys to unbounded integers, so they can be fed to the sum/count
// machinery that reasons over mathematical ints.
pub open spec fn ikeys(ks: Seq<i64>) -> Seq<int> {
    ks.map_values(|k: i64| k as int)
}

// 0/1 indicator of 'this key equals ok'; summing it over the scanned keys counts how many
// times that order appears, i.e. a COUNT(*) ... WHERE key = ok.
pub open spec fn key_is(ok: int) -> spec_fn(int) -> int {
    |k: int| ind(k == ok)
}

// How many times an order key appears among the scanned finished-order keys; this is the
// order's multiplicity, which later scales its contribution to each supplier's total.
pub open spec fn fcount(fkeys: Seq<i64>, ok: int) -> int {
    sum(ikeys(fkeys), key_is(ok))
}

// Per-order, per-supplier lineitem count (GROUP BY orderkey, suppkey) taken from the scanned rows
// instead of the abstract table; it is the running quantity while building and updating.
pub open spec fn cnt_rows(lis: Seq<LiRow>, ok: int, sk: int) -> int {
    sum(lis_view(lis), |l: LineitemRow| ind(l.l_orderkey == ok && l.l_suppkey == sk))
}

// Same per-order, per-supplier count as above, but only over late lineitems
// (receipt date after commit date).
pub open spec fn lcnt_rows(lis: Seq<LiRow>, ok: int, sk: int) -> int {
    sum(lis_view(lis), |l: LineitemRow| ind(l.l_orderkey == ok && l.l_suppkey == sk && late(l)))
}

// Appending one key to a scanned prefix appends the same key to its integer version;
// this is the one-row step when counting order-key occurrences incrementally.
pub proof fn lemma_ikeys_take(ks: Seq<i64>, t: int)
    requires
        0 <= t < ks.len(),
    ensures
        ikeys(ks.take(t + 1)) == ikeys(ks.take(t)).push(ks[t] as int),
{
    assert(ikeys(ks.take(t + 1)) =~= ikeys(ks.take(t)).push(ks[t] as int));
}

// Appending one lineitem row to a prefix appends its logical row to the abstract table;
// this is the one-row step when maintaining the per-supplier counts incrementally.
pub proof fn lemma_lis_take(lis: Seq<LiRow>, t: int)
    requires
        0 <= t < lis.len(),
    ensures
        lis_view(lis.take(t + 1)) == lis_view(lis.take(t)).push(li_view(lis[t])),
{
    assert(lis_view(lis.take(t + 1)) =~= lis_view(lis.take(t)).push(li_view(lis[t])));
}

// If the scanned distinct order keys have no repeats as machine numbers, they have none as
// integers either, so summing per distinct order is a valid group-by over orders.
pub proof fn lemma_count_distinct_keys(ks: Seq<i64>)
    requires
        forall|i: int, j: int| 0 <= i < ks.len() && 0 <= j < ks.len() && i != j ==> ks[i] != ks[j],
    ensures
        distinct(ikeys(ks)),
{
    let ik = ikeys(ks);
    assert forall|i: int, j: int|
        0 <= i < ik.len() && 0 <= j < ik.len() && i != j implies ik[i] != ik[j] by {
        assert(ks[i] != ks[j]);
    }
}

// ── per-nation supplier lists ──────────────────────────────────────────────
// Scanning suppliers one at a time: adding supplier t to the prefix keeps it in the nation's
// list exactly when its nation matches (one step of a WHERE nationkey = nk filter).
pub proof fn lemma_nat_filter_step(sups: Seq<SupE>, nk: i32, t: int)
    requires
        0 <= t < usize::MAX,
    ensures
        nat_filter(sups, nk, t + 1) == if sups[t].nk == nk {
            nat_filter(sups, nk, t).push(t as usize)
        } else {
            nat_filter(sups, nk, t)
        },
{
    let s1 = Seq::new((t + 1) as nat, |i: int| i as usize);
    let s0 = Seq::new(t as nat, |i: int| i as usize);
    assert(s1.drop_last() =~= s0);
    assert(s1.last() == t as usize);
    reveal_with_fuel(Seq::filter, 1);
}

// Builds a nation-to-suppliers index (like a hash-join build side) so a nation's suppliers
// can be found without rescanning all suppliers.  Each list is sorted by s_name, so a
// nation's suppliers are already in GROUP BY order when execute reads them.
pub fn build_nat_sups(sups: &Vec<SupE>, nats: &Vec<NatE>) -> (r: Vec<Vec<usize>>)
    requires
        sups@.len() < usize::MAX,
    ensures
        r@.len() == nats@.len(),
        forall|j: int| 0 <= j < nats@.len() ==> nat_list_ok(sups@, nats@[j].nk, (#[trigger] r@[j])@),
{
    let mut r: Vec<Vec<usize>> = Vec::new();
    let nn = nats.len();
    let ns = sups.len();
    let name_cmp = |a: usize, b: usize| -> (res: bool)
        requires
            a < ns && b < ns && ns == sups@.len(),
        ensures
            res == name_le(sups@)(a, b),
        { !bytes_lt_exec(sups[b].name.as_slice(), sups[a].name.as_slice()) };
    proof {
        lemma_name_le_preorder(sups@);
        assert(cmp_ok(&name_cmp, name_le(sups@), valid_sup(sups@)));
    }
    let mut j: usize = 0;
    while j < nn
        invariant
            0 <= j <= nn == nats@.len(),
            ns == sups@.len() < usize::MAX,
            r@.len() == j,
            cmp_ok(&name_cmp, name_le(sups@), valid_sup(sups@)),
            preorder_on(name_le(sups@), valid_sup(sups@)),
            forall|jj: int| 0 <= jj < j ==> nat_list_ok(sups@, nats@[jj].nk, (#[trigger] r@[jj])@),
        decreases nn - j,
    {
        let nk = nats[j].nk;
        let mut v: Vec<usize> = Vec::new();
        let mut i: usize = 0;
        proof {
            assert(nat_filter(sups@, nk, 0) =~= Seq::<usize>::empty()) by {
                reveal_with_fuel(Seq::filter, 1);
            }
        }
        while i < ns
            invariant
                0 <= i <= ns == sups@.len() < usize::MAX,
                v@ == nat_filter(sups@, nk, i as int),
                all_valid(v@, valid_sup(sups@)),
            decreases ns - i,
        {
            proof {
                lemma_nat_filter_step(sups@, nk, i as int);
            }
            if sups[i].nk == nk {
                v.push(i);
            }
            i += 1;
        }
        let sv = sort_idx(&v, &name_cmp, Ghost(name_le(sups@)), Ghost(valid_sup(sups@)));
        let ghost r0 = r@;
        r.push(sv);
        proof {
            assert(r@[j as int]@ == sv@);
            assert forall|jj: int| 0 <= jj < j + 1 implies nat_list_ok(sups@, nats@[jj].nk, (#[trigger] r@[jj])@) by {
                if jj < j {
                    assert(r@[jj] == r0[jj]);
                }
            }
        }
        j += 1;
    }
    r
}

// ── 'F' order multiplicities ───────────────────────────────────────────────
pub open spec fn fmap_ok(m: Map<i64, OrdE>, ks: Seq<i64>, fkeys: Seq<i64>) -> bool {
    &&& forall|ok: i64| #[trigger] m.contains_key(ok) <==> fcount(fkeys, ok as int) > 0
    &&& forall|ok: i64| #[trigger] m.contains_key(ok) <==> ks.contains(ok)
    &&& forall|i: int, j: int| 0 <= i < ks.len() && 0 <= j < ks.len() && i != j ==> ks[i] != ks[j]
}

// Groups the finished ('F') orders by key into a lookup table with each key's multiplicity
// and no lineitem info yet, plus the distinct keys; None if a counter would overflow.
pub fn build_fmap(fkeys: &[i64]) -> (r: Option<(HashMap<i64, OrdE>, Vec<i64>)>)
    ensures
        r matches Some(p) ==> {
            &&& fmap_ok(p.0@, p.1@, fkeys@)
            &&& forall|ok: i64| #[trigger]
                p.0@.contains_key(ok) ==> p.0@[ok].fm as int == fcount(fkeys@, ok as int)
                    && p.0@[ok].ents@ == Seq::<Ent>::empty()
        },
{
    assert(vstd::std_specs::hash::obeys_key_model::<i64>());
    let mut m: HashMap<i64, OrdE> = HashMap::new();
    let mut ks: Vec<i64> = Vec::new();
    let n = fkeys.len();
    let mut t: usize = 0;
    proof {
        assert forall|ok: i64| #[trigger] fcount(fkeys@.take(0), ok as int) == 0 by {
            assert(ikeys(fkeys@.take(0)).len() == 0);
        }
    }
    while t < n
        invariant
            0 <= t <= n == fkeys@.len(),
            vstd::std_specs::hash::obeys_key_model::<i64>(),
            fmap_ok(m@, ks@, fkeys@.take(t as int)),
            forall|ok: i64| #[trigger]
                m@.contains_key(ok) ==> m@[ok].fm as int == fcount(fkeys@.take(t as int), ok as int)
                    && m@[ok].ents@ == Seq::<Ent>::empty(),
        decreases n - t,
    {
        let k = fkeys[t];
        let ghost m0 = m@;
        let ghost ks0 = ks@;
        proof {
            lemma_ikeys_take(fkeys@, t as int);
            assert forall|ok: i64| #[trigger] fcount(fkeys@.take(t as int + 1), ok as int) == fcount(
                fkeys@.take(t as int),
                ok as int,
            ) + ind(k as int == ok as int) by {
                lemma_sum_push(ikeys(fkeys@.take(t as int)), k as int, key_is(ok as int));
            }
            assert forall|ok: i64| #[trigger] fcount(fkeys@.take(t as int), ok as int) >= 0 by {
                lemma_sum_nonneg(ikeys(fkeys@.take(t as int)), key_is(ok as int));
            }
        }
        match m.remove(&k) {
            Some(o) => {
                let f2 = o.fm.checked_add(1);
                match f2 {
                    Some(f2) => {
                        m.insert(k, OrdE { fm: f2, ents: o.ents });
                    },
                    None => {
                        return None;
                    },
                }
            },
            None => {
                m.insert(k, OrdE { fm: 1, ents: Vec::new() });
                ks.push(k);
                proof {
                    assert forall|i: int, j: int|
                        0 <= i < ks@.len() && 0 <= j < ks@.len() && i != j implies ks@[i] != ks@[j] by {
                        assert(ks@ == ks0.push(k));
                        assert(!ks0.contains(k));
                        if i == ks@.len() - 1 {
                            assert(ks@[j] == ks0[j]);
                        } else if j == ks@.len() - 1 {
                            assert(ks@[i] == ks0[i]);
                        } else {
                            assert(ks@[i] == ks0[i]);
                            assert(ks@[j] == ks0[j]);
                        }
                    }
                    assert(ks@ == ks0.push(k));
                    assert(!m0.contains_key(k));
                    assert forall|ok: i64| #[trigger] m@.contains_key(ok) <==> ks@.contains(ok) by {
                        if ok == k {
                            assert(ks@[ks@.len() - 1] == k);
                        } else {
                            assert(m@.contains_key(ok) == m0.contains_key(ok));
                            assert(m0.contains_key(ok) == ks0.contains(ok));
                            if ks@.contains(ok) {
                                let i = choose|i: int| 0 <= i < ks@.len() && ks@[i] == ok;
                                assert(ks0[i] == ok);
                            }
                            if ks0.contains(ok) {
                                let i = choose|i: int| 0 <= i < ks0.len() && ks0[i] == ok;
                                assert(ks@[i] == ok);
                            }
                        }
                    }
                }
            },
        }
        t += 1;
    }
    proof {
        assert(fkeys@.take(n as int) =~= fkeys@);
    }
    Some((m, ks))
}

// ── accumulate lineitems into the per-order entries ─────────────────────────
// Each order's per-supplier (total, late) counts are well formed (one entry per supplier)
// and equal the counts over the scanned lineitem rows; holds throughout the accumulation loop.
pub open spec fn acc_ok(m: Map<i64, OrdE>, lis: Seq<LiRow>) -> bool {
    forall|ok: i64| #[trigger]
        m.contains_key(ok) ==> {
            &&& edistinct(m[ok].ents@)
            &&& epos(m[ok].ents@)
            &&& forall|sk: int| #[trigger] etot(m[ok].ents@, sk) == cnt_rows(lis, ok as int, sk)
            &&& forall|sk: int| #[trigger] elate(m[ok].ents@, sk) == lcnt_rows(lis, ok as int, sk)
        }
}

// Scans lineitems and, for each, bumps its supplier's total (and late) count within its
// finished order (a GROUP BY orderkey, suppkey); orders themselves are unchanged. False on overflow.
pub fn build_accumulate(m: &mut HashMap<i64, OrdE>, lis: &[LiRow]) -> (ok: bool)
    requires
        acc_ok(old(m)@, lis@.take(0)),
    ensures
        ok ==> final(m)@.dom() == old(m)@.dom(),
        ok ==> forall|k: i64| #[trigger] final(m)@.contains_key(k) ==> final(m)@[k].fm == old(m)@[k].fm,
        ok ==> acc_ok(final(m)@, lis@),
{
    assert(vstd::std_specs::hash::obeys_key_model::<i64>());
    let n = lis.len();
    let mut t: usize = 0;
    while t < n
        invariant
            0 <= t <= n == lis@.len(),
            vstd::std_specs::hash::obeys_key_model::<i64>(),
            m@.dom() == old(m)@.dom(),
            forall|k: i64| #[trigger] m@.contains_key(k) ==> m@[k].fm == old(m)@[k].fm,
            acc_ok(m@, lis@.take(t as int)),
        decreases n - t,
    {
        let r = lis[t];
        let ghost m0 = m@;
        let ghost pre = lis@.take(t as int);
        proof {
            lemma_lis_take(lis@, t as int);
        }
        let is_l = is_late(&r);
        let dl: i64 = if is_l {
            1
        } else {
            0
        };
        match m.remove(&r.orderkey) {
            Some(o) => {
                let mut ents = o.ents;
                let ghost e0 = ents@;
                let added = ents_add(&mut ents, r.suppkey, 1, dl);
                if !added {
                    return false;
                }
                m.insert(r.orderkey, OrdE { fm: o.fm, ents });
                proof {
                    assert forall|ok: i64| #[trigger] m@.contains_key(ok) implies {
                        &&& edistinct(m@[ok].ents@)
                        &&& epos(m@[ok].ents@)
                        &&& forall|sk: int| #[trigger]
                            etot(m@[ok].ents@, sk) == cnt_rows(lis@.take(t as int + 1), ok as int, sk)
                        &&& forall|sk: int| #[trigger]
                            elate(m@[ok].ents@, sk) == lcnt_rows(lis@.take(t as int + 1), ok as int, sk)
                    } by {
                        assert forall|sk: int| #[trigger]
                            cnt_rows(lis@.take(t as int + 1), ok as int, sk) == cnt_rows(pre, ok as int, sk)
                                + ind(r.orderkey as int == ok as int && r.suppkey as int == sk) by {
                            lemma_sum_push(lis_view(pre), li_view(r), |l: LineitemRow| ind(l.l_orderkey == ok as int && l.l_suppkey == sk));
                        }
                        assert forall|sk: int| #[trigger]
                            lcnt_rows(lis@.take(t as int + 1), ok as int, sk) == lcnt_rows(pre, ok as int, sk)
                                + ind(r.orderkey as int == ok as int && r.suppkey as int == sk && late(li_view(r))) by {
                            lemma_sum_push(lis_view(pre), li_view(r), |l: LineitemRow| ind(l.l_orderkey == ok as int && l.l_suppkey == sk && late(l)));
                        }
                        if ok == r.orderkey {
                            assert(m0.contains_key(ok));
                        } else {
                            assert(m0.contains_key(ok));
                            assert(m@[ok] == m0[ok]);
                        }
                    }
                }
            },
            None => {
                proof {
                    assert forall|ok: i64| #[trigger] m@.contains_key(ok) implies {
                        &&& edistinct(m@[ok].ents@)
                        &&& epos(m@[ok].ents@)
                        &&& forall|sk: int| #[trigger]
                            etot(m@[ok].ents@, sk) == cnt_rows(lis@.take(t as int + 1), ok as int, sk)
                        &&& forall|sk: int| #[trigger]
                            elate(m@[ok].ents@, sk) == lcnt_rows(lis@.take(t as int + 1), ok as int, sk)
                    } by {
                        assert(ok != r.orderkey);
                        assert forall|sk: int| #[trigger]
                            cnt_rows(lis@.take(t as int + 1), ok as int, sk) == cnt_rows(pre, ok as int, sk) by {
                            lemma_sum_push(lis_view(pre), li_view(r), |l: LineitemRow| ind(l.l_orderkey == ok as int && l.l_suppkey == sk));
                        }
                        assert forall|sk: int| #[trigger]
                            lcnt_rows(lis@.take(t as int + 1), ok as int, sk) == lcnt_rows(pre, ok as int, sk) by {
                            lemma_sum_push(lis_view(pre), li_view(r), |l: LineitemRow| ind(l.l_orderkey == ok as int && l.l_suppkey == sk && late(l)));
                        }
                    }
                }
            },
        }
        t += 1;
    }
    proof {
        assert(lis@.take(n as int) =~= lis@);
    }
    true
}

// ── Build ──────────────────────────────────────────────────────────────────
// Given the scan inputs really come from the database, counting an order key among the
// scanned finished-order keys gives the order's true multiplicity in the orders table.
pub proof fn lemma_fcount_is_fm(fkeys: Seq<i64>, lis: Seq<LiRow>, sups: Seq<SupE>, nats: Seq<NatE>, db: Db, ok: int)
    requires
        build_contract(sups, nats, fkeys, lis, db),
    ensures
        fcount(fkeys, ok) == fm(db, ok),
{
    let f = key_is(ok);
    let g = |o: OrdersRow| if is_f(o) { f(o.o_orderkey) } else { 0 };
    assert(sum(ikeys(fkeys), f) == sum(db.orders, g));
    assert(fcount(fkeys, ok) == sum(ikeys(fkeys), f));
    let h = |o: OrdersRow| ind(o.o_orderkey == ok && is_f(o));
    assert forall|o: OrdersRow| #[trigger] g(o) == h(o) by {}
    lemma_sum_ext_all(db.orders, g, h);
    assert(fm(db, ok) == sum(db.orders, h));
}

// For an order that is a finished order, counts from the scanned lineitem rows agree
// with the true per-supplier counts in the database (filtering to 'F' orders loses nothing).
pub proof fn lemma_rows_are_counts(fkeys: Seq<i64>, lis: Seq<LiRow>, sups: Seq<SupE>, nats: Seq<NatE>, db: Db, ok: int, sk: int)
    requires
        build_contract(sups, nats, fkeys, lis, db),
        fm(db, ok) > 0,
    ensures
        cnt_rows(lis, ok, sk) == cnt(db, ok, sk),
        lcnt_rows(lis, ok, sk) == lcnt(db, ok, sk),
{
    let fc = |l: LineitemRow| ind(l.l_orderkey == ok && l.l_suppkey == sk);
    let fl = |l: LineitemRow| ind(l.l_orderkey == ok && l.l_suppkey == sk && late(l));
    assert(sum(lis_view(lis), fc) == sum(db.lineitem, |l: LineitemRow| if fm(db, l.l_orderkey) > 0 { fc(l) } else { 0 }));
    assert(sum(lis_view(lis), fl) == sum(db.lineitem, |l: LineitemRow| if fm(db, l.l_orderkey) > 0 { fl(l) } else { 0 }));
    lemma_sum_ext_all(db.lineitem, |l: LineitemRow| if fm(db, l.l_orderkey) > 0 { fc(l) } else { 0 }, fc);
    lemma_sum_ext_all(db.lineitem, |l: LineitemRow| if fm(db, l.l_orderkey) > 0 { fl(l) } else { 0 }, fl);
}

/// fm > 0 only for orderkeys that occur among the scanned 'F' keys.
pub proof fn lemma_fm_pos_in_keys(fkeys: Seq<i64>, lis: Seq<LiRow>, sups: Seq<SupE>, nats: Seq<NatE>, db: Db, ok: int)
    requires
        build_contract(sups, nats, fkeys, lis, db),
        fm(db, ok) > 0,
    ensures
        exists|i: int| 0 <= i < fkeys.len() && fkeys[i] as int == ok,
{
    lemma_fcount_is_fm(fkeys, lis, sups, nats, db, ok);
    let i = lemma_sum_nonzero_witness(ikeys(fkeys), key_is(ok));
    assert(ikeys(fkeys)[i] == fkeys[i] as int);
}

/// Per-order contribution to W(sk), as a function of the orderkey.
pub open spec fn po(db: Db, sk: int) -> spec_fn(int) -> int {
    |ok: int| per_order(db, ok, sk)
}

// Computes each supplier's final count (numwait) by visiting every distinct finished order,
// adding the order's multiplicity times its qualifying-late-line count to the one supplier who qualifies.
pub fn compute_w(
    m: &HashMap<i64, OrdE>,
    ks: &Vec<i64>,
    Ghost(db): Ghost<Db>,
) -> (r: Option<HashMap<i64, i64>>)
    requires
        forall|i: int| 0 <= i < ks@.len() ==> m@.contains_key(#[trigger] ks@[i]),
        forall|ok: i64| #[trigger] m@.contains_key(ok) ==> ord_ok(m@[ok], db, ok as int),
    ensures
        r matches Some(wm) ==> forall|sk: i64| #[trigger] wget(wm@, sk) == sum(ikeys(ks@), po(db, sk as int)),
{
    assert(vstd::std_specs::hash::obeys_key_model::<i64>());
    let mut wm: HashMap<i64, i64> = HashMap::new();
    let nk = ks.len();
    let mut t: usize = 0;
    proof {
        assert forall|sk: i64| #[trigger] wget(wm@, sk) == sum(ikeys(ks@.take(0)), po(db, sk as int)) by {
            assert(ikeys(ks@.take(0)).len() == 0);
        }
    }
    while t < nk
        invariant
            0 <= t <= nk == ks@.len(),
            vstd::std_specs::hash::obeys_key_model::<i64>(),
            forall|i: int| 0 <= i < ks@.len() ==> m@.contains_key(#[trigger] ks@[i]),
            forall|ok: i64| #[trigger] m@.contains_key(ok) ==> ord_ok(m@[ok], db, ok as int),
            forall|sk: i64| #[trigger] wget(wm@, sk) == sum(ikeys(ks@.take(t as int)), po(db, sk as int)),
        decreases nk - t,
    {
        let ok = ks[t];
        assert(m@.contains_key(ks@[t as int]));
        let o = m.get(&ok).unwrap();
        let c = ents_contrib(&o.ents);
        let ghost w0 = wm@;
        proof {
            assert(ikeys(ks@.take(t as int + 1)) =~= ikeys(ks@.take(t as int)).push(ok as int));
            assert forall|sk: i64| #[trigger] per_order(db, ok as int, sk as int)
                == o.fm as int * opt_val(c, sk as int) by {
                lemma_contrib_from_ents(o.ents@, db, ok as int, sk as int);
            }
        }
        match c {
            Some((a, cv)) => {
                let prod = match o.fm.checked_mul(cv) {
                    Some(x) => x,
                    None => {
                        return None;
                    },
                };
                let nv = match wget_exec(&wm, a).checked_add(prod) {
                    Some(x) => x,
                    None => {
                        return None;
                    },
                };
                wm.insert(a, nv);
            },
            None => {},
        }
        proof {
            assert forall|sk: i64| #[trigger] wget(wm@, sk) == sum(ikeys(ks@.take(t as int + 1)), po(db, sk as int)) by {
                lemma_sum_push(ikeys(ks@.take(t as int)), ok as int, po(db, sk as int));
                match c {
                    Some(p) => {
                        if sk != p.0 {
                            assert(wget(wm@, sk) == wget(w0, sk));
                            assert(o.fm as int * 0 == 0) by (nonlinear_arith);
                        }
                    },
                    None => {
                        assert(o.fm as int * 0 == 0) by (nonlinear_arith);
                    },
                }
            }
        }
        t += 1;
    }
    proof {
        assert(ks@.take(nk as int) =~= ks@);
    }
    Some(wm)
}

// Builds the full incremental state (indexes, per-order counts, per-supplier totals) from
// the base-table scans and proves it represents the database; None on overflow.
pub fn build(
    sups: Vec<SupE>,
    nats: Vec<NatE>,
    fkeys: &[i64],
    lis: &[LiRow],
    Ghost(db): Ghost<Db>,
) -> (r: Option<State>)
    requires
        build_contract(sups@, nats@, fkeys@, lis@, db),
    ensures
        r matches Some(s) ==> s.db() == db,
{
    assert(vstd::std_specs::hash::obeys_key_model::<i64>());
    if sups.len() >= usize::MAX - 1 {
        return None;
    }
    let nat_sups = build_nat_sups(&sups, &nats);
    let st = Static { sups, nats, nat_sups };
    proof {
        assert(static_ok(st, db));
    }
    let (mut m, ks) = match build_fmap(fkeys) {
        Some(p) => p,
        None => {
            return None;
        },
    };
    proof {
        assert forall|ok: i64| #[trigger] m@.contains_key(ok) implies {
            &&& edistinct(m@[ok].ents@)
            &&& epos(m@[ok].ents@)
            &&& forall|sk: int| #[trigger] etot(m@[ok].ents@, sk) == cnt_rows(lis@.take(0), ok as int, sk)
            &&& forall|sk: int| #[trigger] elate(m@[ok].ents@, sk) == lcnt_rows(lis@.take(0), ok as int, sk)
        } by {
            assert(lis_view(lis@.take(0)).len() == 0);
            assert forall|sk: int| #[trigger] etot(m@[ok].ents@, sk) == cnt_rows(lis@.take(0), ok as int, sk) by {
                assert(m@[ok].ents@.len() == 0);
            }
            assert forall|sk: int| #[trigger] elate(m@[ok].ents@, sk) == lcnt_rows(lis@.take(0), ok as int, sk) by {
                assert(m@[ok].ents@.len() == 0);
            }
        }
    }
    let ghost m_f = m@;
    if !build_accumulate(&mut m, lis) {
        return None;
    }
    proof {
        assert forall|ok: i64| #[trigger] m@.contains_key(ok) implies ord_ok(m@[ok], db, ok as int) by {
            assert(m_f.contains_key(ok));
            lemma_fcount_is_fm(fkeys@, lis@, st.sups@, st.nats@, db, ok as int);
            assert(fm(db, ok as int) > 0);
            assert forall|sk: int| #[trigger] etot(m@[ok].ents@, sk) == cnt(db, ok as int, sk) by {
                lemma_rows_are_counts(fkeys@, lis@, st.sups@, st.nats@, db, ok as int, sk);
            }
            assert forall|sk: int| #[trigger] elate(m@[ok].ents@, sk) == lcnt(db, ok as int, sk) by {
                lemma_rows_are_counts(fkeys@, lis@, st.sups@, st.nats@, db, ok as int, sk);
            }
        }
        assert forall|i: int| 0 <= i < ks@.len() implies m@.contains_key(#[trigger] ks@[i]) by {
            assert(ks@.contains(ks@[i]));
        }
    }
    let wm = match compute_w(&m, &ks, Ghost(db)) {
        Some(x) => x,
        None => {
            return None;
        },
    };
    let orders = OrderMap { base: Arc::new(m), overlay: HashMap::new(), okeys: Vec::new() };
    proof {
        lemma_count_distinct_keys(ks@);
        // W decomposes over the scanned 'F' keys.
        assert forall|ok: int| #[trigger] fm(db, ok) > 0 implies ikeys(ks@).contains(ok) by {
            lemma_fm_pos_in_keys(fkeys@, lis@, st.sups@, st.nats@, db, ok);
            let i = choose|i: int| 0 <= i < fkeys@.len() && fkeys@[i] as int == ok;
            let k = fkeys@[i];
            lemma_fcount_is_fm(fkeys@, lis@, st.sups@, st.nats@, db, ok);
            assert(m_f.contains_key(k));
            assert(ks@.contains(k));
            let j = choose|j: int| 0 <= j < ks@.len() && ks@[j] == k;
            assert(ikeys(ks@)[j] == ok);
        }
        assert forall|sk: i64| #[trigger] wget(wm@, sk) == w(db, sk as int) by {
            lemma_w_by_orders(db, sk as int, ikeys(ks@));
            lemma_sum_ext_all(ikeys(ks@), |ok: int| per_order(db, ok, sk as int), po(db, sk as int));
        }
        assert forall|ok: i64| match #[trigger] om_get(orders, ok) {
            Some(o) => ord_ok(o, db, ok as int),
            None => fm(db, ok as int) == 0,
        } by {
            assert(orders.overlay@ == Map::<i64, Option<OrdE>>::empty());
            assert(orders.base@ == m@);
            if !m@.contains_key(ok) {
                assert(!m_f.contains_key(ok));
                lemma_fcount_is_fm(fkeys@, lis@, st.sups@, st.nats@, db, ok as int);
                lemma_fm_nonneg(db, ok as int);
            }
        }
        assert forall|ok: int| #[trigger] fm(db, ok) > 0 implies i64::MIN <= ok <= i64::MAX by {
            lemma_fm_pos_in_keys(fkeys@, lis@, st.sups@, st.nats@, db, ok);
        }
    }
    Some(State::new(Arc::new(st), orders, wm, Ghost(db)))
}

} // verus!
