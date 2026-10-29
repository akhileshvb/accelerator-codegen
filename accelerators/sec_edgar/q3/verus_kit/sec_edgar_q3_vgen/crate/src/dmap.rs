// A batch of changes, accumulated per class and group as signed count, sum and absolute sum (stage A).
// The maps hold the sums of a ledger of contributions (`Ct`); nothing is rounded yet.
use vstd::prelude::*;
use std::sync::Arc;
use verus_common::rel::*;
use verus_common::sums::*;
use verus_prooflib::ordered::{Index, Key, KeyView};
use verus_prooflib::wide::Wide;
use crate::ent::*;
use crate::gen_spec::*;
use crate::gkey::*;
use crate::join_spec::*;
use crate::qty::*;

verus! {

pub struct DCls {
    pub cd: Index<Ent>,
    pub od: Index<Ent>,
}

pub open spec fn dent_ok(e: Ent, q: Q) -> bool {
    e.count@ == q.n && e.sum@ == q.s && e.abs@ == q.a
}

pub open spec fn contrib_to(inner: bool, c: Ct, p: Params, gn: Option<Seq<u8>>, gc: Option<int>) -> bool {
    ct_in(c, p) && c.j.cik == gc && (inner || c.j.name == gn)
}

/// The map holds, per group, the ledger's sums over the contributions to it, and has no entry for a group
/// without a contribution.
pub open spec fn dmap_ok(inner: bool, m: Index<Ent>, l: Seq<Ct>, p: Params) -> bool {
    &&& m.wf()
    &&& forall|gn: Option<Seq<u8>>, gc: Option<int>| #[trigger] m.at(ek(inner, gn, gc)) is Some
        ==> dent_ok(m.at(ek(inner, gn, gc))->0, dq(l, p, inner, gn, gc)) && ent_names(inner, m.at(ek(inner, gn, gc))->0, gn, gc)
    &&& forall|gn: Option<Seq<u8>>, gc: Option<int>| #[trigger] m.at(ek(inner, gn, gc)) is None
        ==> forall|i: int| 0 <= i < l.len() ==> !contrib_to(inner, #[trigger] l[i], p, gn, gc)
    &&& forall|k: KeyView| #[trigger] m.at(k) is Some ==>
        k == gk(opt_bytes(m.at(k)->0.name), opt_int(m.at(k)->0.cik)) && (inner ==> m.at(k)->0.name is None)
}

pub open spec fn ddm_ok(dm: Index<DCls>, l: Seq<Ct>) -> bool {
    &&& dm.wf()
    &&& forall|fy: i64, uom: Seq<u8>| #[trigger] dm.at(ckey(fy as int, uom)) is Some ==>
        dmap_ok(true, dm.at(ckey(fy as int, uom))->0.cd, l, cp(fy, uom)) && dmap_ok(false, dm.at(ckey(fy as int, uom))->0.od, l, cp(fy, uom))
    &&& forall|fy: i64, uom: Seq<u8>| #[trigger] dm.at(ckey(fy as int, uom)) is None ==>
        forall|i: int| 0 <= i < l.len() ==> !ct_in(#[trigger] l[i], cp(fy, uom))
}

/// A contribution adds its sign times the weight to the sums of the group it belongs to, and nothing elsewhere.
proof fn lemma_dq_push(l: Seq<Ct>, c: Ct, p: Params, inner: bool, gn: Option<Seq<u8>>, gc: Option<int>)
    requires c.j.vs,
    ensures
        dq(l.push(c), p, inner, gn, gc) == if contrib_to(inner, c, p, gn, gc) {
            Q { n: dq(l, p, inner, gn, gc).n + c.sign, s: dq(l, p, inner, gn, gc).s + c.sign * c.j.v, a: dq(l, p, inner, gn, gc).a + c.sign * abs(c.j.v) }
        } else {
            dq(l, p, inner, gn, gc)
        },
{
    lemma_sum_push(l, c, |x: Ct| if ct_in(x, p) { x.sign * gh(inner, gn, gc, fvs())(x.j) } else { 0 });
    lemma_sum_push(l, c, |x: Ct| if ct_in(x, p) { x.sign * gh(inner, gn, gc, fv())(x.j) } else { 0 });
    lemma_sum_push(l, c, |x: Ct| if ct_in(x, p) { x.sign * gh(inner, gn, gc, fm())(x.j) } else { 0 });
    if contrib_to(inner, c, p, gn, gc) {
        assert(gh(inner, gn, gc, fvs())(c.j) == 1);
        assert(gh(inner, gn, gc, fv())(c.j) == c.j.v);
        assert(gh(inner, gn, gc, fm())(c.j) == abs(c.j.v));
        assert(c.sign * 1 == c.sign);
    } else {
        assert(!ct_in(c, p) || gh(inner, gn, gc, fvs())(c.j) == 0);
        assert(!ct_in(c, p) || gh(inner, gn, gc, fv())(c.j) == 0);
        assert(!ct_in(c, p) || gh(inner, gn, gc, fm())(c.j) == 0);
        assert(c.sign * 0 == 0);
    }
}

pub proof fn lemma_dq_zero(l: Seq<Ct>, p: Params, inner: bool, gn: Option<Seq<u8>>, gc: Option<int>)
    requires forall|i: int| 0 <= i < l.len() ==> !contrib_to(inner, #[trigger] l[i], p, gn, gc),
    ensures dq(l, p, inner, gn, gc) == (Q { n: 0, s: 0, a: 0 }),
{
    lemma_sum_zero(l, |x: Ct| if ct_in(x, p) { x.sign * gh(inner, gn, gc, fvs())(x.j) } else { 0 });
    lemma_sum_zero(l, |x: Ct| if ct_in(x, p) { x.sign * gh(inner, gn, gc, fv())(x.j) } else { 0 });
    lemma_sum_zero(l, |x: Ct| if ct_in(x, p) { x.sign * gh(inner, gn, gc, fm())(x.j) } else { 0 });
}

/// Add a signed contribution to the map of its class.
pub fn acc_ent(m: &mut Index<Ent>, inner: bool, name: &Option<Vec<u8>>, cik: Option<i64>,
    sg: &Wide, v: &Wide, mag: &Wide, Ghost(l): Ghost<Seq<Ct>>, Ghost(p): Ghost<Params>, Ghost(c): Ghost<Ct>)
    requires
        dmap_ok(inner, *old(m), l, p),
        ct_in(c, p),
        c.j.cik == opt_int(cik),
        c.j.name == opt_bytes(*name),
        c.j.vs,
        c.sign == sg@,
        c.j.v == v@,
        mag@ == abs(v@),
    ensures
        dmap_ok(inner, *final(m), l.push(c), p),
{
    let ghost before = *m;
    let ghost (gn0, gc0) = (opt_bytes(*name), opt_int(cik));
    let key = ent_key(inner, name, cik);
    let found = m.get(&key);
    let (c0, s0, a0) = match &found {
        Some(a) => (a.count.dup(), a.sum.dup(), a.abs.dup()),
        None => (Wide::zero(), Wide::zero(), Wide::zero()),
    };
    proof {
        assert(key.view() == ek(inner, gn0, gc0));
        match &found {
            Some(a) => { assert(before.at(ek(inner, gn0, gc0)) == Some(**a)); },
            None => {
                assert(before.at(ek(inner, gn0, gc0)) is None);
                lemma_dq_zero(l, p, inner, gn0, gc0);
            },
        }
    }
    let e = Ent {
        name: if inner { None } else { copy_name(name) },
        cik,
        count: c0.add(sg),
        sum: s0.add(&sg.mul(v)),
        abs: a0.add(&sg.mul(mag)),
        cv: Wide::zero(),
        bits: None,
    };
    m.put(key.share(), Arc::new(e));
    proof {
        let l2 = l.push(c);
        assert forall|gn: Option<Seq<u8>>, gc: Option<int>| #[trigger] m.at(ek(inner, gn, gc)) is Some
            implies dent_ok(m.at(ek(inner, gn, gc))->0, dq(l2, p, inner, gn, gc)) && ent_names(inner, m.at(ek(inner, gn, gc))->0, gn, gc) by {
            lemma_dq_push(l, c, p, inner, gn, gc);
            if ek(inner, gn, gc) == key.view() {
                lemma_gk_inj(if inner { None } else { gn }, gc, if inner { None } else { gn0 }, gc0);
                assert(gc == gc0 && (inner || gn == gn0));
                assert(contrib_to(inner, c, p, gn, gc));
                lemma_gh_ext(inner, gn, gn0, gc, fvs());
                lemma_gh_ext(inner, gn, gn0, gc, fv());
                lemma_gh_ext(inner, gn, gn0, gc, fm());
                assert(dq(l, p, inner, gn, gc) == dq(l, p, inner, gn0, gc0));
            } else {
                assert(m.at(ek(inner, gn, gc)) == before.at(ek(inner, gn, gc)));
                assert(!contrib_to(inner, c, p, gn, gc)) by {
                    if contrib_to(inner, c, p, gn, gc) {
                        assert(gc == gc0 && (inner || gn == gn0));
                        assert(ek(inner, gn, gc) == ek(inner, gn0, gc0));
                    }
                }
            }
        }
        assert forall|gn: Option<Seq<u8>>, gc: Option<int>| #[trigger] m.at(ek(inner, gn, gc)) is None
            implies forall|i: int| 0 <= i < l2.len() ==> !contrib_to(inner, #[trigger] l2[i], p, gn, gc) by {
            assert(ek(inner, gn, gc) != key.view());
            assert(before.at(ek(inner, gn, gc)) is None);
            assert forall|i: int| 0 <= i < l2.len() implies !contrib_to(inner, #[trigger] l2[i], p, gn, gc) by {
                if i < l.len() { assert(l2[i] == l[i]); } else {
                    assert(l2[i] == c);
                    if contrib_to(inner, c, p, gn, gc) { assert(ek(inner, gn, gc) == ek(inner, gn0, gc0)); }
                }
            }
        }
        assert forall|k: KeyView| #[trigger] m.at(k) is Some implies
            k == gk(opt_bytes(m.at(k)->0.name), opt_int(m.at(k)->0.cik)) && (inner ==> m.at(k)->0.name is None) by {
            if k != key.view() { assert(m.at(k) == before.at(k)); }
        }
    }
}

/// Add a signed contribution to the batch's classes.
#[verifier::spinoff_prover]
pub fn acc_ct(dm: &mut Index<DCls>, fy: i64, uom: &[u8], name: &Option<Vec<u8>>, cik: Option<i64>,
    sg: &Wide, v: &Wide, mag: &Wide, Ghost(l): Ghost<Seq<Ct>>, Ghost(c): Ghost<Ct>)
    requires
        ddm_ok(*old(dm), l),
        c.fy == fy as int,
        c.uom == uom@,
        c.j.cik == opt_int(cik),
        c.j.name == opt_bytes(*name),
        c.j.vs,
        c.sign == sg@,
        c.j.v == v@,
        mag@ == abs(v@),
    ensures
        ddm_ok(*final(dm), l.push(c)),
{
    let ghost before = *dm;
    let ghost p = cp(fy, uom@);
    let key = class_key(fy, uom);
    let found = dm.get(&key);
    let (mut cd, mut od) = match &found {
        Some(a) => (a.cd.share(), a.od.share()),
        None => (Index::new(), Index::new()),
    };
    proof {
        match &found {
            Some(a) => {
                assert(before.at(ckey(fy as int, uom@)) == Some(**a));
                lemma_dmap_same(true, &a.cd, &cd, l, p);
                lemma_dmap_same(false, &a.od, &od, l, p);
            },
            None => {
                assert(before.at(ckey(fy as int, uom@)) is None);
                lemma_empty_dmap(true, cd, l, p);
                lemma_empty_dmap(false, od, l, p);
            },
        }
    }
    acc_ent(&mut cd, true, name, cik, sg, v, mag, Ghost(l), Ghost(p), Ghost(c));
    acc_ent(&mut od, false, name, cik, sg, v, mag, Ghost(l), Ghost(p), Ghost(c));
    let ghost cls = DCls { cd, od };
    dm.put(key.share(), Arc::new(DCls { cd, od }));
    proof {
        assert(ct_in(c, p));
        lemma_ddm_after(before, *dm, key.view(), cls, l, c, p, fy, uom@);
    }
}

proof fn lemma_ddm_after(before: Index<DCls>, after: Index<DCls>, key: KeyView, cls: DCls, l: Seq<Ct>, c: Ct, p: Params, fy: i64, uom: Seq<u8>)
    requires
        ddm_ok(before, l),
        after.wf(),
        forall|q: KeyView| #[trigger] after.at(q) == if q == key { Some(cls) } else { before.at(q) },
        key == ckey(fy as int, uom),
        p == cp(fy, uom),
        dmap_ok(true, cls.cd, l.push(c), p),
        dmap_ok(false, cls.od, l.push(c), p),
        ct_in(c, p),
        c.j.vs,
    ensures
        ddm_ok(after, l.push(c)),
{
    let l2 = l.push(c);
    assert forall|fy2: i64, uom2: Seq<u8>| #[trigger] after.at(ckey(fy2 as int, uom2)) is Some implies
        dmap_ok(true, after.at(ckey(fy2 as int, uom2))->0.cd, l2, cp(fy2, uom2)) && dmap_ok(false, after.at(ckey(fy2 as int, uom2))->0.od, l2, cp(fy2, uom2)) by {
        if ckey(fy2 as int, uom2) != key {
            assert(after.at(ckey(fy2 as int, uom2)) == before.at(ckey(fy2 as int, uom2)));
            let q2 = cp(fy2, uom2);
            let cc = after.at(ckey(fy2 as int, uom2))->0;
            assert(!ct_in(c, q2)) by {
                if ct_in(c, q2) { assert(ckey(fy2 as int, uom2) == key); }
            }
            lemma_dmap_unrelated(true, cc.cd, l, c, q2);
            lemma_dmap_unrelated(false, cc.od, l, c, q2);
        } else {
            assert(fy2 == fy && uom2 == uom);
        }
    }
    assert forall|fy2: i64, uom2: Seq<u8>| #[trigger] after.at(ckey(fy2 as int, uom2)) is None implies
        forall|i: int| 0 <= i < l2.len() ==> !ct_in(#[trigger] l2[i], cp(fy2, uom2)) by {
        assert(ckey(fy2 as int, uom2) != key);
        assert(before.at(ckey(fy2 as int, uom2)) is None);
        assert forall|i: int| 0 <= i < l2.len() implies !ct_in(#[trigger] l2[i], cp(fy2, uom2)) by {
            if i < l.len() { assert(l2[i] == l[i]); } else {
                assert(l2[i] == c);
                if ct_in(c, cp(fy2, uom2)) { assert(ckey(fy2 as int, uom2) == key); }
            }
        }
    }
}

proof fn lemma_dmap_same(inner: bool, m1: &Index<Ent>, m2: &Index<Ent>, l: Seq<Ct>, p: Params)
    requires
        dmap_ok(inner, *m1, l, p),
        m2.wf(),
        forall|k: KeyView| #[trigger] m1.at(k) == m2.at(k),
    ensures dmap_ok(inner, *m2, l, p),
{
    assert forall|gn: Option<Seq<u8>>, gc: Option<int>| #[trigger] m2.at(ek(inner, gn, gc)) is Some
        implies dent_ok(m2.at(ek(inner, gn, gc))->0, dq(l, p, inner, gn, gc)) && ent_names(inner, m2.at(ek(inner, gn, gc))->0, gn, gc) by {
        assert(m1.at(ek(inner, gn, gc)) == m2.at(ek(inner, gn, gc)));
    }
    assert forall|gn: Option<Seq<u8>>, gc: Option<int>| #[trigger] m2.at(ek(inner, gn, gc)) is None
        implies forall|i: int| 0 <= i < l.len() ==> !contrib_to(inner, #[trigger] l[i], p, gn, gc) by {
        assert(m1.at(ek(inner, gn, gc)) == m2.at(ek(inner, gn, gc)));
    }
    assert forall|k: KeyView| #[trigger] m2.at(k) is Some implies
        k == gk(opt_bytes(m2.at(k)->0.name), opt_int(m2.at(k)->0.cik)) && (inner ==> m2.at(k)->0.name is None) by {
        assert(m1.at(k) == m2.at(k));
    }
}

proof fn lemma_empty_dmap(inner: bool, m: Index<Ent>, l: Seq<Ct>, p: Params)
    requires
        m.wf(),
        forall|k: KeyView| #[trigger] m.at(k) is None,
        forall|i: int| 0 <= i < l.len() ==> !ct_in(#[trigger] l[i], p),
    ensures dmap_ok(inner, m, l, p),
{
    assert forall|gn: Option<Seq<u8>>, gc: Option<int>| #[trigger] m.at(ek(inner, gn, gc)) is None
        implies forall|i: int| 0 <= i < l.len() ==> !contrib_to(inner, #[trigger] l[i], p, gn, gc) by {
        assert forall|i: int| 0 <= i < l.len() implies !contrib_to(inner, #[trigger] l[i], p, gn, gc) by {
            assert(!ct_in(l[i], p));
        }
    }
}

/// A contribution outside the class changes none of its map's sums.
proof fn lemma_dmap_unrelated(inner: bool, m: Index<Ent>, l: Seq<Ct>, c: Ct, p: Params)
    requires dmap_ok(inner, m, l, p), !ct_in(c, p), c.j.vs,
    ensures dmap_ok(inner, m, l.push(c), p),
{
    let l2 = l.push(c);
    assert forall|gn: Option<Seq<u8>>, gc: Option<int>| #[trigger] m.at(ek(inner, gn, gc)) is Some
        implies dent_ok(m.at(ek(inner, gn, gc))->0, dq(l2, p, inner, gn, gc)) && ent_names(inner, m.at(ek(inner, gn, gc))->0, gn, gc) by {
        lemma_dq_push(l, c, p, inner, gn, gc);
    }
    assert forall|gn: Option<Seq<u8>>, gc: Option<int>| #[trigger] m.at(ek(inner, gn, gc)) is None
        implies forall|i: int| 0 <= i < l2.len() ==> !contrib_to(inner, #[trigger] l2[i], p, gn, gc) by {
        assert forall|i: int| 0 <= i < l2.len() implies !contrib_to(inner, #[trigger] l2[i], p, gn, gc) by {
            if i < l.len() { assert(l2[i] == l[i]); } else { assert(l2[i] == c); }
        }
    }
}

}
