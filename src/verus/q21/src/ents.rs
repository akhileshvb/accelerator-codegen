// Per-order supplier entries: for one order, (suppkey, #lineitems, #late
// lineitems) for every supplier with at least one lineitem on the order.
use vstd::prelude::*;
use verus_common::sums::*;
use crate::gen_spec::*;
use crate::bridge::*;
use crate::orderspec::*;

verus! {

#[derive(Clone, Copy)]
pub struct Ent {
    pub sk: i64,
    pub total: i64,
    pub late: i64,
}

// Number of lineitems on this order from supplier sk, summed over its per-supplier
// entries (COUNT(*) grouped by supplier); should match the true count.
pub open spec fn etot(e: Seq<Ent>, sk: int) -> int {
    sum(e, |x: Ent| if x.sk == sk { x.total as int } else { 0 })
}

// Number of late lineitems on this order from supplier sk, summed over its per-supplier
// entries; should match the true late count.
pub open spec fn elate(e: Seq<Ent>, sk: int) -> int {
    sum(e, |x: Ent| if x.sk == sk { x.late as int } else { 0 })
}

// Lineitems on this order from suppliers other than sk; being positive is the
// EXISTS (another supplier on the same order) test.
pub open spec fn eother_tot(e: Seq<Ent>, sk: int) -> int {
    sum(e, |x: Ent| if x.sk != sk { x.total as int } else { 0 })
}

// Late lineitems on this order from suppliers other than sk; being zero is the
// NOT EXISTS (another supplier who was also late) test.
pub open spec fn eother_late(e: Seq<Ent>, sk: int) -> int {
    sum(e, |x: Ent| if x.sk != sk { x.late as int } else { 0 })
}

/// The per-order contribution computed from the entries alone.
pub open spec fn econtrib(e: Seq<Ent>, sk: int) -> int {
    if eother_tot(e, sk) > 0 && eother_late(e, sk) == 0 {
        elate(e, sk)
    } else {
        0
    }
}

// Each supplier appears in at most one entry, like a GROUP BY suppkey result,
// so an entry's counts are the supplier's whole counts.
pub open spec fn edistinct(e: Seq<Ent>) -> bool {
    forall|i: int, j: int| 0 <= i < e.len() && 0 <= j < e.len() && i != j ==> e[i].sk != e[j].sk
}

// No empty entries: every listed supplier has at least one lineitem on the order
// and a non-negative late count.
pub open spec fn epos(e: Seq<Ent>) -> bool {
    forall|i: int| 0 <= i < e.len() ==> (#[trigger] e[i]).total > 0 && e[i].late >= 0
}

/// The entries are exactly the per-supplier (count, late count) of order ok.
pub open spec fn ents_ok(e: Seq<Ent>, db: Db, ok: int) -> bool {
    &&& edistinct(e)
    &&& epos(e)
    &&& forall|sk: int| #[trigger] etot(e, sk) == cnt(db, ok, sk)
    &&& forall|sk: int| #[trigger] elate(e, sk) == lcnt(db, ok, sk)
}

// Reads the (supplier, late count) answer for an order as a per-supplier number:
// the late count for that supplier, 0 for everyone else (no answer = 0).
pub open spec fn opt_val(r: Option<(i64, i64)>, sk: int) -> int {
    match r {
        Some(p) => if p.0 as int == sk {
            p.1 as int
        } else {
            0
        },
        None => 0,
    }
}

// With one entry per supplier, the totals for an entry's supplier are exactly that
// entry's own fields, nothing hidden in other entries.
pub proof fn lemma_etot_entry(e: Seq<Ent>, i: int)
    requires
        edistinct(e),
        0 <= i < e.len(),
    ensures
        etot(e, e[i].sk as int) == e[i].total,
        elate(e, e[i].sk as int) == e[i].late,
{
    let s = e[i].sk as int;
    let ft = |x: Ent| if x.sk == s { x.total as int } else { 0 };
    let fl = |x: Ent| if x.sk == s { x.late as int } else { 0 };
    assert forall|j: int| 0 <= j < e.len() && j != i implies ft(#[trigger] e[j]) == 0 && fl(e[j])
        == 0 by {}
    lemma_sum_single_match(e, ft, i);
    lemma_sum_single_match(e, fl, i);
}

// A supplier with no entry on the order has zero total and zero late lineitems;
// so any supplier with lineitems there must have an entry.
pub proof fn lemma_etot_absent(e: Seq<Ent>, sk: int)
    requires
        forall|i: int| 0 <= i < e.len() ==> (#[trigger] e[i]).sk != sk,
    ensures
        etot(e, sk) == 0,
        elate(e, sk) == 0,
{
    lemma_sum_zero(e, |x: Ent| if x.sk == sk { x.total as int } else { 0 });
    lemma_sum_zero(e, |x: Ent| if x.sk == sk { x.late as int } else { 0 });
}

// Restricted to one supplier k's lineitems on the order, 'other than sk' counts all of
// them if k differs from sk and none if k is sk; the per-group step of a group-by regrouping.
pub proof fn lemma_per_sk_tot(db: Db, ok: int, sk: int, k: int)
    ensures
        sum(
            db.lineitem,
            |x: LineitemRow| if x.l_suppkey == k { ind(x.l_orderkey == ok && x.l_suppkey != sk) } else { 0 },
        ) == if k != sk {
            cnt(db, ok, k)
        } else {
            0
        },
{
    if k != sk {
        lemma_sum_ext_all(
            db.lineitem,
            |x: LineitemRow| if x.l_suppkey == k { ind(x.l_orderkey == ok && x.l_suppkey != sk) } else { 0 },
            |x: LineitemRow| ind(x.l_orderkey == ok && x.l_suppkey == k),
        );
    } else {
        lemma_sum_ext_all(
            db.lineitem,
            |x: LineitemRow| if x.l_suppkey == k { ind(x.l_orderkey == ok && x.l_suppkey != sk) } else { 0 },
            |x: LineitemRow| 0int,
        );
        lemma_sum_zero(db.lineitem, |x: LineitemRow| 0int);
    }
}

// Same as the total-count case but for late lineitems: supplier k's late lines count
// fully toward 'others than sk' unless k is sk.
pub proof fn lemma_per_sk_late(db: Db, ok: int, sk: int, k: int)
    ensures
        sum(
            db.lineitem,
            |x: LineitemRow|
                if x.l_suppkey == k {
                    ind(x.l_orderkey == ok && x.l_suppkey != sk && late(x))
                } else {
                    0
                },
        ) == if k != sk {
            lcnt(db, ok, k)
        } else {
            0
        },
{
    if k != sk {
        lemma_sum_ext_all(
            db.lineitem,
            |x: LineitemRow|
                if x.l_suppkey == k {
                    ind(x.l_orderkey == ok && x.l_suppkey != sk && late(x))
                } else {
                    0
                },
            |x: LineitemRow| ind(x.l_orderkey == ok && x.l_suppkey == k && late(x)),
        );
    } else {
        lemma_sum_ext_all(
            db.lineitem,
            |x: LineitemRow|
                if x.l_suppkey == k {
                    ind(x.l_orderkey == ok && x.l_suppkey != sk && late(x))
                } else {
                    0
                },
            |x: LineitemRow| 0int,
        );
        lemma_sum_zero(db.lineitem, |x: LineitemRow| 0int);
    }
}

/// Every lineitem of order ok has its supplier among the entries.
pub proof fn lemma_ents_cover(e: Seq<Ent>, db: Db, ok: int, i: int) -> (j: int)
    requires
        ents_ok(e, db, ok),
        0 <= i < db.lineitem.len(),
        db.lineitem[i].l_orderkey == ok,
    ensures
        0 <= j < e.len(),
        e[j].sk == db.lineitem[i].l_suppkey,
{
    let s2 = db.lineitem[i].l_suppkey;
    let g = |x: LineitemRow| ind(x.l_orderkey == ok && x.l_suppkey == s2);
    lemma_sum_nonneg(db.lineitem, g);
    lemma_sum_pos(db.lineitem, g, i);
    assert(cnt(db, ok, s2) > 0);
    if forall|j: int| 0 <= j < e.len() ==> (#[trigger] e[j]).sk != s2 {
        lemma_etot_absent(e, s2);
        assert(etot(e, s2) == cnt(db, ok, s2));
    }
    choose|j: int| 0 <= j < e.len() && (#[trigger] e[j]).sk == s2
}

// One entry per supplier means the list of supplier keys has no duplicates,
// which lets us regroup the order's lineitems by supplier without double counting.
pub proof fn lemma_ents_keys_distinct(e: Seq<Ent>)
    requires
        edistinct(e),
    ensures
        distinct(e.map_values(|x: Ent| x.sk as int)),
{
    let ks = e.map_values(|x: Ent| x.sk as int);
    assert forall|i: int, j: int|
        0 <= i < ks.len() && 0 <= j < ks.len() && i != j implies ks[i] != ks[j] by {
        assert(e[i].sk != e[j].sk);
    }
}

// The number of lineitems from other suppliers, counted directly over the order's rows,
// equals the sum of the other suppliers' entry totals (regrouping by supplier).
pub proof fn lemma_other_tot(e: Seq<Ent>, db: Db, ok: int, sk: int)
    requires
        ents_ok(e, db, ok),
    ensures
        other_cnt(db, ok, sk) == eother_tot(e, sk),
{
    let ks = e.map_values(|x: Ent| x.sk as int);
    lemma_ents_keys_distinct(e);
    let key = |l: LineitemRow| l.l_suppkey;
    let f = |l: LineitemRow| ind(l.l_orderkey == ok && l.l_suppkey != sk);
    let per = |k: int|
        sum(
            db.lineitem,
            |x: LineitemRow| if x.l_suppkey == k { ind(x.l_orderkey == ok && x.l_suppkey != sk) } else { 0 },
        );
    assert forall|i: int|
        0 <= i < db.lineitem.len() && f(db.lineitem[i]) != 0 implies ks.contains(
        key(#[trigger] db.lineitem[i]),
    ) by {
        let j = lemma_ents_cover(e, db, ok, i);
        assert(ks[j] == db.lineitem[i].l_suppkey);
    }
    assert forall|k: int| #[trigger] per(k) == sum(
        db.lineitem,
        |x: LineitemRow| if key(x) == k { f(x) } else { 0 },
    ) by {
        lemma_sum_ext_all(
            db.lineitem,
            |x: LineitemRow| if key(x) == k { f(x) } else { 0 },
            |x: LineitemRow| if x.l_suppkey == k { ind(x.l_orderkey == ok && x.l_suppkey != sk) } else { 0 },
        );
    }
    lemma_sum_group(db.lineitem, ks, key, f, per);
    let h = |x: Ent| per(x.sk as int);
    lemma_sum_map(e, |x: Ent| x.sk as int, per, h);
    assert forall|i: int| 0 <= i < e.len() implies #[trigger] h(e[i]) == (if e[i].sk != sk {
        e[i].total as int
    } else {
        0
    }) by {
        lemma_per_sk_tot(db, ok, sk, e[i].sk as int);
        lemma_etot_entry(e, i);
        assert(etot(e, e[i].sk as int) == cnt(db, ok, e[i].sk as int));
    }
    lemma_sum_ext(e, h, |x: Ent| if x.sk != sk { x.total as int } else { 0 });
}

// Likewise, late lineitems from other suppliers counted over the rows equal
// the sum of the other suppliers' entry late counts.
pub proof fn lemma_other_late(e: Seq<Ent>, db: Db, ok: int, sk: int)
    requires
        ents_ok(e, db, ok),
    ensures
        other_late_cnt(db, ok, sk) == eother_late(e, sk),
{
    let ks = e.map_values(|x: Ent| x.sk as int);
    lemma_ents_keys_distinct(e);
    let key = |l: LineitemRow| l.l_suppkey;
    let f = |l: LineitemRow| ind(l.l_orderkey == ok && l.l_suppkey != sk && late(l));
    let per = |k: int|
        sum(
            db.lineitem,
            |x: LineitemRow|
                if x.l_suppkey == k {
                    ind(x.l_orderkey == ok && x.l_suppkey != sk && late(x))
                } else {
                    0
                },
        );
    assert forall|i: int|
        0 <= i < db.lineitem.len() && f(db.lineitem[i]) != 0 implies ks.contains(
        key(#[trigger] db.lineitem[i]),
    ) by {
        let j = lemma_ents_cover(e, db, ok, i);
        assert(ks[j] == db.lineitem[i].l_suppkey);
    }
    assert forall|k: int| #[trigger] per(k) == sum(
        db.lineitem,
        |x: LineitemRow| if key(x) == k { f(x) } else { 0 },
    ) by {
        lemma_sum_ext_all(
            db.lineitem,
            |x: LineitemRow| if key(x) == k { f(x) } else { 0 },
            |x: LineitemRow|
                if x.l_suppkey == k {
                    ind(x.l_orderkey == ok && x.l_suppkey != sk && late(x))
                } else {
                    0
                },
        );
    }
    lemma_sum_group(db.lineitem, ks, key, f, per);
    let h = |x: Ent| per(x.sk as int);
    lemma_sum_map(e, |x: Ent| x.sk as int, per, h);
    assert forall|i: int| 0 <= i < e.len() implies #[trigger] h(e[i]) == (if e[i].sk != sk {
        e[i].late as int
    } else {
        0
    }) by {
        lemma_per_sk_late(db, ok, sk, e[i].sk as int);
        lemma_etot_entry(e, i);
        assert(elate(e, e[i].sk as int) == lcnt(db, ok, e[i].sk as int));
    }
    lemma_sum_ext(e, h, |x: Ent| if x.sk != sk { x.late as int } else { 0 });
}

/// Summing a per-supplier quantity over the order's lineitems groups by
/// entry: other_cnt / other_late_cnt from the entries.
pub proof fn lemma_other_counts(e: Seq<Ent>, db: Db, ok: int, sk: int)
    requires
        ents_ok(e, db, ok),
    ensures
        other_cnt(db, ok, sk) == eother_tot(e, sk),
        other_late_cnt(db, ok, sk) == eother_late(e, sk),
{
    lemma_other_tot(e, db, ok, sk);
    lemma_other_late(e, db, ok, sk);
}

/// The entries determine the order's contribution to every supplier.
pub proof fn lemma_contrib_from_ents(e: Seq<Ent>, db: Db, ok: int, sk: int)
    requires
        ents_ok(e, db, ok),
    ensures
        contrib(db, ok, sk) == econtrib(e, sk),
{
    lemma_other_counts(e, db, ok, sk);
    assert(elate(e, sk) == lcnt(db, ok, sk));
}

/// The "exactly one late supplier among more than one supplier" rule.
pub fn ents_contrib(e: &Vec<Ent>) -> (r: Option<(i64, i64)>)
    requires
        edistinct(e@),
        epos(e@),
    ensures
        forall|sk: int| #[trigger] econtrib(e@, sk) == opt_val(r, sk),
        r matches Some(p) ==> p.1 > 0,
{
    let n = e.len();
    let mut n_late: usize = 0;
    let mut sole: usize = 0;
    let mut i: usize = 0;
    while i < n
        invariant
            0 <= i <= n == e.len(),
            edistinct(e@),
            epos(e@),
            n_late <= i,
            n_late >= 1 ==> sole < i && e@[sole as int].late > 0,
            n_late == 1 ==> forall|j: int|
                0 <= j < i && j != sole ==> (#[trigger] e@[j]).late == 0,
            n_late == 0 ==> forall|j: int| 0 <= j < i ==> (#[trigger] e@[j]).late == 0,
            n_late >= 2 ==> exists|j: int|
                0 <= j < i && j != sole && (#[trigger] e@[j]).late > 0,
        decreases n - i,
    {
        if e[i].late > 0 {
            if n_late >= 1 {
                assert(e@[sole as int].late > 0);
                n_late = 2;
                assert(i as int != sole as int);
                // keep `sole` as a late index; the new one is another late index
                assert(e@[i as int].late > 0);
            } else {
                n_late = 1;
                sole = i;
            }
        }
        i += 1;
    }
    let ghost es = e@;
    proof {
        assert forall|j: int| 0 <= j < es.len() implies (#[trigger] es[j]).late >= 0 by {}
    }
    if n_late == 1 && n >= 2 {
        let a = e[sole].sk;
        let c = e[sole].late;
        proof {
            let si = sole as int;
            lemma_etot_entry(es, si);
            assert forall|sk: int| #[trigger] econtrib(es, sk) == opt_val(Some((a, c)), sk) by {
                let fo = |x: Ent| if x.sk != sk { x.total as int } else { 0 };
                let fl = |x: Ent| if x.sk != sk { x.late as int } else { 0 };
                assert forall|j: int| 0 <= j < es.len() implies fo(#[trigger] es[j]) >= 0 && fl(
                    es[j],
                ) >= 0 by {}
                if sk == a as int {
                    // another supplier exists
                    let other: int = if si == 0 { 1 } else { 0 };
                    assert(es[other].sk != es[si].sk);
                    lemma_sum_pos(es, fo, other);
                    assert forall|j: int| 0 <= j < es.len() implies fl(#[trigger] es[j]) == 0 by {
                        if j != si {
                        } else {
                        }
                    }
                    lemma_sum_zero(es, fl);
                } else {
                    assert(fl(es[si]) > 0);
                    lemma_sum_pos(es, fl, si);
                }
            }
        }
        Some((a, c))
    } else {
        proof {
            assert forall|sk: int| #[trigger] econtrib(es, sk) == 0 by {
                let fo = |x: Ent| if x.sk != sk { x.total as int } else { 0 };
                let fl = |x: Ent| if x.sk != sk { x.late as int } else { 0 };
                let fs = |x: Ent| if x.sk == sk { x.late as int } else { 0 };
                assert forall|j: int| 0 <= j < es.len() implies fo(#[trigger] es[j]) >= 0 && fl(
                    es[j],
                ) >= 0 by {}
                if n_late == 0 {
                    lemma_sum_zero(es, fs);
                } else if n_late >= 2 {
                    let j2 = choose|j: int| 0 <= j < n && j != sole && (#[trigger] es[j]).late > 0;
                    // one of the two late entries differs from sk
                    if es[j2].sk != sk {
                        lemma_sum_pos(es, fl, j2);
                    } else {
                        assert(es[sole as int].sk != es[j2].sk);
                        lemma_sum_pos(es, fl, sole as int);
                    }
                } else {
                    // exactly one entry, and it is late
                    assert(n == 1);
                    if es[0].sk == sk {
                        lemma_sum_zero(es, fo);
                    } else {
                        assert(sole == 0);
                        lemma_sum_pos(es, fl, 0);
                    }
                }
            }
        }
        None
    }
}

/// Add (dt, dl) to supplier sk's entry (creating it if absent).  Returns
/// false on arithmetic overflow (the caller then gives up).
pub fn ents_add(e: &mut Vec<Ent>, sk: i64, dt: i64, dl: i64) -> (ok: bool)
    requires
        edistinct(old(e)@),
    ensures
        ok ==> edistinct(final(e)@),
        ok ==> forall|s: int| #[trigger]
            etot(final(e)@, s) == etot(old(e)@, s) + if s == sk as int {
                dt as int
            } else {
                0
            },
        ok ==> forall|s: int| #[trigger]
            elate(final(e)@, s) == elate(old(e)@, s) + if s == sk as int {
                dl as int
            } else {
                0
            },
        ok && dt > 0 && dl >= 0 && epos(old(e)@) ==> epos(final(e)@),
{
    let n = e.len();
    let mut i: usize = 0;
    while i < n
        invariant
            0 <= i <= n == e.len(),
            e@ == old(e)@,
            edistinct(old(e)@),
            forall|j: int| 0 <= j < i ==> (#[trigger] e@[j]).sk != sk,
        decreases n - i,
    {
        if e[i].sk == sk {
            let cur = e[i];
            let t = cur.total.checked_add(dt);
            let l = cur.late.checked_add(dl);
            match (t, l) {
                (Some(t), Some(l)) => {
                    let ne = Ent { sk, total: t, late: l };
                    let ghost e0 = e@;
                    e.set(i, ne);
                    proof {
                        let ii = i as int;
                        assert(e@ == e0.update(ii, ne));
                        assert forall|s: int| #[trigger]
                            etot(e@, s) == etot(e0, s) + if s == sk as int {
                                dt as int
                            } else {
                                0
                            } by {
                            lemma_sum_update(e0, ii, ne, |x: Ent| if x.sk == s {
                                x.total as int
                            } else {
                                0
                            });
                        }
                        assert forall|s: int| #[trigger]
                            elate(e@, s) == elate(e0, s) + if s == sk as int {
                                dl as int
                            } else {
                                0
                            } by {
                            lemma_sum_update(e0, ii, ne, |x: Ent| if x.sk == s {
                                x.late as int
                            } else {
                                0
                            });
                        }
                        assert(edistinct(e@)) by {
                            assert forall|a: int, b: int|
                                0 <= a < e@.len() && 0 <= b < e@.len() && a != b implies e@[a].sk
                                != e@[b].sk by {
                                assert(e0[a].sk != e0[b].sk);
                            }
                        }
                    }
                    return true;
                },
                _ => {
                    return false;
                },
            }
        }
        i += 1;
    }
    let ne = Ent { sk, total: dt, late: dl };
    let ghost e0 = e@;
    e.push(ne);
    proof {
        assert forall|s: int| #[trigger]
            etot(e@, s) == etot(e0, s) + if s == sk as int {
                dt as int
            } else {
                0
            } by {
            lemma_sum_push(e0, ne, |x: Ent| if x.sk == s { x.total as int } else { 0 });
        }
        assert forall|s: int| #[trigger]
            elate(e@, s) == elate(e0, s) + if s == sk as int {
                dl as int
            } else {
                0
            } by {
            lemma_sum_push(e0, ne, |x: Ent| if x.sk == s { x.late as int } else { 0 });
        }
        assert(edistinct(e@)) by {
            assert forall|a: int, b: int|
                0 <= a < e@.len() && 0 <= b < e@.len() && a != b implies e@[a].sk != e@[b].sk by {
                if a < e0.len() && b < e0.len() {
                    assert(e0[a].sk != e0[b].sk);
                }
            }
        }
    }
    true
}

/// Drop entries whose count reached zero.
pub fn ents_clean(e: &Vec<Ent>) -> (r: Vec<Ent>)
    requires
        edistinct(e@),
        forall|i: int|
            0 <= i < e@.len() ==> (#[trigger] e@[i]).total >= 0 && e@[i].late >= 0 && (
            e@[i].total == 0 ==> e@[i].late == 0),
    ensures
        edistinct(r@),
        epos(r@),
        forall|s: int| #[trigger] etot(r@, s) == etot(e@, s),
        forall|s: int| #[trigger] elate(r@, s) == elate(e@, s),
{
    let mut r: Vec<Ent> = Vec::new();
    let n = e.len();
    let mut i: usize = 0;
    while i < n
        invariant
            0 <= i <= n == e.len(),
            edistinct(e@),
            forall|j: int|
                0 <= j < e@.len() ==> (#[trigger] e@[j]).total >= 0 && e@[j].late >= 0 && (
                e@[j].total == 0 ==> e@[j].late == 0),
            epos(r@),
            // r holds a subsequence of e.take(i): every r entry's key is some e[j], j < i
            forall|a: int|
                0 <= a < r@.len() ==> exists|j: int|
                    0 <= j < i && (#[trigger] e@[j]).sk == (#[trigger] r@[a]).sk,
            edistinct(r@),
            forall|s: int| #[trigger] etot(r@, s) == etot(e@.take(i as int), s),
            forall|s: int| #[trigger] elate(r@, s) == elate(e@.take(i as int), s),
        decreases n - i,
    {
        let x = e[i];
        let ghost r0 = r@;
        proof {
            assert forall|s: int| #[trigger]
                etot(e@.take(i as int + 1), s) == etot(e@.take(i as int), s) + (if x.sk == s {
                    x.total as int
                } else {
                    0
                }) by {
                lemma_sum_take(e@, i as int, |y: Ent| if y.sk == s { y.total as int } else { 0 });
            }
            assert forall|s: int| #[trigger]
                elate(e@.take(i as int + 1), s) == elate(e@.take(i as int), s) + (if x.sk == s {
                    x.late as int
                } else {
                    0
                }) by {
                lemma_sum_take(e@, i as int, |y: Ent| if y.sk == s { y.late as int } else { 0 });
            }
        }
        if x.total != 0 {
            r.push(x);
            proof {
                assert forall|s: int| #[trigger]
                    etot(r@, s) == etot(r0, s) + (if x.sk == s { x.total as int } else { 0 }) by {
                    lemma_sum_push(r0, x, |y: Ent| if y.sk == s { y.total as int } else { 0 });
                }
                assert forall|s: int| #[trigger]
                    elate(r@, s) == elate(r0, s) + (if x.sk == s { x.late as int } else { 0 }) by {
                    lemma_sum_push(r0, x, |y: Ent| if y.sk == s { y.late as int } else { 0 });
                }
                assert(edistinct(r@)) by {
                    assert forall|a: int, b: int|
                        0 <= a < r@.len() && 0 <= b < r@.len() && a != b implies r@[a].sk
                        != r@[b].sk by {
                        if a < r0.len() && b < r0.len() {
                        } else if a < r0.len() {
                            let j = choose|j: int| 0 <= j < i && (#[trigger] e@[j]).sk == r0[a].sk;
                            assert(e@[j].sk != e@[i as int].sk);
                        } else {
                            let j = choose|j: int| 0 <= j < i && (#[trigger] e@[j]).sk == r0[b].sk;
                            assert(e@[j].sk != e@[i as int].sk);
                        }
                    }
                }
                assert forall|a: int| 0 <= a < r@.len() implies exists|j: int|
                    0 <= j < i + 1 && (#[trigger] e@[j]).sk == (#[trigger] r@[a]).sk by {
                    if a < r0.len() {
                        let j = choose|j: int| 0 <= j < i && (#[trigger] e@[j]).sk == r0[a].sk;
                        assert(e@[j].sk == r@[a].sk);
                    } else {
                        assert(e@[i as int].sk == r@[a].sk);
                    }
                }
            }
        }
        i += 1;
    }
    proof {
        assert(e@.take(n as int) =~= e@);
    }
    r
}

} // verus!
