// A batch of changes, accumulated per class and group as signed count, sum and absolute sum (stage A).
// The maps hold the sums of a ledger of contributions (`Ct`); nothing is rounded yet.
use vstd::prelude::*;
use std::sync::Arc;
use verus_common::rel::*;
use verus_common::sums::*;
use verus_prooflib::ordered::{Index, Key, KeyView};
use verus_prooflib::wide::Wide;
use verus_common::bytes::*;
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

pub open spec fn seg_cnt(seg: Seq<Ct>) -> int {
    sum(seg, |c: Ct| c.sign)
}

pub open spec fn seg_sum(seg: Seq<Ct>) -> int {
    sum(seg, |c: Ct| c.sign * c.j.v)
}

pub open spec fn seg_abs(seg: Seq<Ct>) -> int {
    sum(seg, |c: Ct| c.sign * abs(c.j.v))
}

/// The contributions of `seg` are all to one group (name rn, company rc) of class p, each with a value.
pub open spec fn seg_of(seg: Seq<Ct>, p: Params, rn: Option<Seq<u8>>, rc: Option<int>) -> bool {
    forall|i: int| 0 <= i < seg.len() ==> ct_in(#[trigger] seg[i], p) && seg[i].j.vs && seg[i].j.name == rn && seg[i].j.cik == rc
}

/// A run of contributions to one group adds its sums to the sums of that group, and nothing elsewhere.
pub proof fn lemma_dq_seg(l: Seq<Ct>, seg: Seq<Ct>, p: Params, inner: bool, gn: Option<Seq<u8>>, gc: Option<int>, rn: Option<Seq<u8>>, rc: Option<int>)
    requires seg_of(seg, p, rn, rc),
    ensures
        dq(l + seg, p, inner, gn, gc) == if rc == gc && (inner || rn == gn) {
            Q { n: dq(l, p, inner, gn, gc).n + seg_cnt(seg), s: dq(l, p, inner, gn, gc).s + seg_sum(seg), a: dq(l, p, inner, gn, gc).a + seg_abs(seg) }
        } else {
            dq(l, p, inner, gn, gc)
        },
{
    let t1 = |x: Ct| if ct_in(x, p) { x.sign * gh(inner, gn, gc, fvs())(x.j) } else { 0 };
    let t2 = |x: Ct| if ct_in(x, p) { x.sign * gh(inner, gn, gc, fv())(x.j) } else { 0 };
    let t3 = |x: Ct| if ct_in(x, p) { x.sign * gh(inner, gn, gc, fm())(x.j) } else { 0 };
    lemma_sum_concat(l, seg, t1);
    lemma_sum_concat(l, seg, t2);
    lemma_sum_concat(l, seg, t3);
    if rc == gc && (inner || rn == gn) {
        assert forall|i: int| 0 <= i < seg.len() implies t1(#[trigger] seg[i]) == (|c: Ct| c.sign)(seg[i]) by {
            assert(gh(inner, gn, gc, fvs())(seg[i].j) == 1);
        }
        assert forall|i: int| 0 <= i < seg.len() implies t2(#[trigger] seg[i]) == (|c: Ct| c.sign * c.j.v)(seg[i]) by {
            assert(gh(inner, gn, gc, fv())(seg[i].j) == seg[i].j.v);
        }
        assert forall|i: int| 0 <= i < seg.len() implies t3(#[trigger] seg[i]) == (|c: Ct| c.sign * abs(c.j.v))(seg[i]) by {
            assert(gh(inner, gn, gc, fm())(seg[i].j) == abs(seg[i].j.v));
        }
        lemma_sum_ext(seg, t1, |c: Ct| c.sign);
        lemma_sum_ext(seg, t2, |c: Ct| c.sign * c.j.v);
        lemma_sum_ext(seg, t3, |c: Ct| c.sign * abs(c.j.v));
    } else {
        assert forall|i: int| 0 <= i < seg.len() implies t1(#[trigger] seg[i]) == 0 by {
            assert(gh(inner, gn, gc, fvs())(seg[i].j) == 0);
        }
        assert forall|i: int| 0 <= i < seg.len() implies t2(#[trigger] seg[i]) == 0 by {
            assert(gh(inner, gn, gc, fv())(seg[i].j) == 0);
        }
        assert forall|i: int| 0 <= i < seg.len() implies t3(#[trigger] seg[i]) == 0 by {
            assert(gh(inner, gn, gc, fm())(seg[i].j) == 0);
        }
        lemma_sum_zero(seg, t1);
        lemma_sum_zero(seg, t2);
        lemma_sum_zero(seg, t3);
    }
}

/// Contributions to another class change none of this class's sums.
pub proof fn lemma_dq_other(l: Seq<Ct>, seg: Seq<Ct>, p: Params, inner: bool, gn: Option<Seq<u8>>, gc: Option<int>)
    requires forall|i: int| 0 <= i < seg.len() ==> !ct_in(#[trigger] seg[i], p),
    ensures dq(l + seg, p, inner, gn, gc) == dq(l, p, inner, gn, gc),
{
    let t1 = |x: Ct| if ct_in(x, p) { x.sign * gh(inner, gn, gc, fvs())(x.j) } else { 0 };
    let t2 = |x: Ct| if ct_in(x, p) { x.sign * gh(inner, gn, gc, fv())(x.j) } else { 0 };
    let t3 = |x: Ct| if ct_in(x, p) { x.sign * gh(inner, gn, gc, fm())(x.j) } else { 0 };
    lemma_sum_concat(l, seg, t1);
    lemma_sum_concat(l, seg, t2);
    lemma_sum_concat(l, seg, t3);
    lemma_sum_zero(seg, t1);
    lemma_sum_zero(seg, t2);
    lemma_sum_zero(seg, t3);
}

pub proof fn lemma_dq_zero(l: Seq<Ct>, p: Params, inner: bool, gn: Option<Seq<u8>>, gc: Option<int>)
    requires forall|i: int| 0 <= i < l.len() ==> !contrib_to(inner, #[trigger] l[i], p, gn, gc),
    ensures dq(l, p, inner, gn, gc) == (Q { n: 0, s: 0, a: 0 }),
{
    lemma_sum_zero(l, |x: Ct| if ct_in(x, p) { x.sign * gh(inner, gn, gc, fvs())(x.j) } else { 0 });
    lemma_sum_zero(l, |x: Ct| if ct_in(x, p) { x.sign * gh(inner, gn, gc, fv())(x.j) } else { 0 });
    lemma_sum_zero(l, |x: Ct| if ct_in(x, p) { x.sign * gh(inner, gn, gc, fm())(x.j) } else { 0 });
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

/// Add a run of contributions to one group to the map of its class.
pub fn acc_ent(m: &mut Index<Ent>, inner: bool, name: &Option<Vec<u8>>, cik: Option<i64>,
    cn: &Wide, sm: &Wide, ab: &Wide, Ghost(l): Ghost<Seq<Ct>>, Ghost(p): Ghost<Params>, Ghost(seg): Ghost<Seq<Ct>>)
    requires
        dmap_ok(inner, *old(m), l, p),
        seg_of(seg, p, opt_bytes(*name), opt_int(cik)),
        cn@ == seg_cnt(seg),
        sm@ == seg_sum(seg),
        ab@ == seg_abs(seg),
    ensures
        dmap_ok(inner, *final(m), l + seg, p),
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
        count: c0.add(cn),
        sum: s0.add(sm),
        abs: a0.add(ab),
        cv: Wide::zero(),
        bits: None,
    };
    m.put(key.share(), Arc::new(e));
    proof {
        let l2 = l + seg;
        assert forall|gn: Option<Seq<u8>>, gc: Option<int>| #[trigger] m.at(ek(inner, gn, gc)) is Some
            implies dent_ok(m.at(ek(inner, gn, gc))->0, dq(l2, p, inner, gn, gc)) && ent_names(inner, m.at(ek(inner, gn, gc))->0, gn, gc) by {
            lemma_dq_seg(l, seg, p, inner, gn, gc, gn0, gc0);
            if ek(inner, gn, gc) == key.view() {
                lemma_gk_inj(if inner { None } else { gn }, gc, if inner { None } else { gn0 }, gc0);
                assert(gc == gc0 && (inner || gn == gn0));
                lemma_gh_ext(inner, gn, gn0, gc, fvs());
                lemma_gh_ext(inner, gn, gn0, gc, fv());
                lemma_gh_ext(inner, gn, gn0, gc, fm());
                assert(dq(l, p, inner, gn, gc) == dq(l, p, inner, gn0, gc0));
            } else {
                assert(m.at(ek(inner, gn, gc)) == before.at(ek(inner, gn, gc)));
                assert(!(gc0 == gc && (inner || gn0 == gn))) by {
                    if gc0 == gc && (inner || gn0 == gn) { assert(ek(inner, gn, gc) == ek(inner, gn0, gc0)); }
                }
            }
        }
        assert forall|gn: Option<Seq<u8>>, gc: Option<int>| #[trigger] m.at(ek(inner, gn, gc)) is None
            implies forall|i: int| 0 <= i < l2.len() ==> !contrib_to(inner, #[trigger] l2[i], p, gn, gc) by {
            assert(ek(inner, gn, gc) != key.view());
            assert(before.at(ek(inner, gn, gc)) is None);
            assert forall|i: int| 0 <= i < l2.len() implies !contrib_to(inner, #[trigger] l2[i], p, gn, gc) by {
                if i < l.len() { assert(l2[i] == l[i]); } else {
                    let j = i - l.len();
                    assert(l2[i] == seg[j]);
                    if contrib_to(inner, seg[j], p, gn, gc) { assert(ek(inner, gn, gc) == ek(inner, gn0, gc0)); }
                }
            }
        }
        assert forall|k: KeyView| #[trigger] m.at(k) is Some implies
            k == gk(opt_bytes(m.at(k)->0.name), opt_int(m.at(k)->0.cik)) && (inner ==> m.at(k)->0.name is None) by {
            if k != key.view() { assert(m.at(k) == before.at(k)); }
        }
    }
}

/// Add a run of contributions to one group to the batch's classes.
#[verifier::spinoff_prover]
#[verifier::rlimit(100)]
pub fn acc_ct(dm: &mut Index<DCls>, fy: i64, uom: &[u8], name: &Option<Vec<u8>>, cik: Option<i64>,
    cn: &Wide, sm: &Wide, ab: &Wide, Ghost(l): Ghost<Seq<Ct>>, Ghost(seg): Ghost<Seq<Ct>>)
    requires
        ddm_ok(*old(dm), l),
        seg_of(seg, cp(fy, uom@), opt_bytes(*name), opt_int(cik)),
        cn@ == seg_cnt(seg),
        sm@ == seg_sum(seg),
        ab@ == seg_abs(seg),
    ensures
        ddm_ok(*final(dm), l + seg),
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
    acc_ent(&mut cd, true, name, cik, cn, sm, ab, Ghost(l), Ghost(p), Ghost(seg));
    acc_ent(&mut od, false, name, cik, cn, sm, ab, Ghost(l), Ghost(p), Ghost(seg));
    let ghost cls = DCls { cd, od };
    dm.put(key.share(), Arc::new(DCls { cd, od }));
    proof {
        lemma_ddm_after(before, *dm, key.view(), cls, l, seg, p, fy, uom@);
    }
}

proof fn lemma_ddm_after(before: Index<DCls>, after: Index<DCls>, key: KeyView, cls: DCls, l: Seq<Ct>, seg: Seq<Ct>, p: Params, fy: i64, uom: Seq<u8>)
    requires
        ddm_ok(before, l),
        after.wf(),
        forall|q: KeyView| #[trigger] after.at(q) == if q == key { Some(cls) } else { before.at(q) },
        key == ckey(fy as int, uom),
        p == cp(fy, uom),
        dmap_ok(true, cls.cd, l + seg, p),
        dmap_ok(false, cls.od, l + seg, p),
        forall|i: int| 0 <= i < seg.len() ==> ct_in(#[trigger] seg[i], p) && seg[i].j.vs,
    ensures
        ddm_ok(after, l + seg),
{
    let l2 = l + seg;
    assert forall|fy2: i64, uom2: Seq<u8>| #[trigger] after.at(ckey(fy2 as int, uom2)) is Some implies
        dmap_ok(true, after.at(ckey(fy2 as int, uom2))->0.cd, l2, cp(fy2, uom2)) && dmap_ok(false, after.at(ckey(fy2 as int, uom2))->0.od, l2, cp(fy2, uom2)) by {
        if ckey(fy2 as int, uom2) != key {
            assert(after.at(ckey(fy2 as int, uom2)) == before.at(ckey(fy2 as int, uom2)));
            let q2 = cp(fy2, uom2);
            let cc = after.at(ckey(fy2 as int, uom2))->0;
            assert forall|i: int| 0 <= i < seg.len() implies !ct_in(#[trigger] seg[i], q2) by {
                if ct_in(seg[i], q2) { assert(ckey(fy2 as int, uom2) == key); }
            }
            lemma_dmap_unrelated(true, cc.cd, l, seg, q2);
            lemma_dmap_unrelated(false, cc.od, l, seg, q2);
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
                let j = i - l.len();
                assert(l2[i] == seg[j]);
                if ct_in(seg[j], cp(fy2, uom2)) { assert(ckey(fy2 as int, uom2) == key); }
            }
        }
    }
}

/// Contributions outside the class change none of its map's sums.
proof fn lemma_dmap_unrelated(inner: bool, m: Index<Ent>, l: Seq<Ct>, seg: Seq<Ct>, p: Params)
    requires dmap_ok(inner, m, l, p), forall|i: int| 0 <= i < seg.len() ==> !ct_in(#[trigger] seg[i], p),
    ensures dmap_ok(inner, m, l + seg, p),
{
    let l2 = l + seg;
    assert forall|gn: Option<Seq<u8>>, gc: Option<int>| #[trigger] m.at(ek(inner, gn, gc)) is Some
        implies dent_ok(m.at(ek(inner, gn, gc))->0, dq(l2, p, inner, gn, gc)) && ent_names(inner, m.at(ek(inner, gn, gc))->0, gn, gc) by {
        lemma_dq_other(l, seg, p, inner, gn, gc);
    }
    assert forall|gn: Option<Seq<u8>>, gc: Option<int>| #[trigger] m.at(ek(inner, gn, gc)) is None
        implies forall|i: int| 0 <= i < l2.len() ==> !contrib_to(inner, #[trigger] l2[i], p, gn, gc) by {
        assert forall|i: int| 0 <= i < l2.len() implies !contrib_to(inner, #[trigger] l2[i], p, gn, gc) by {
            if i < l.len() { assert(l2[i] == l[i]); } else { assert(l2[i] == seg[i - l.len()]); }
        }
    }
}


pub proof fn lemma_empty_ddm(dm: Index<DCls>)
    requires dm.wf(), forall|k: KeyView| #[trigger] dm.at(k) is None,
    ensures ddm_ok(dm, Seq::empty()),
{
}

fn same_name(a: &Option<Vec<u8>>, b: &Option<Vec<u8>>) -> (r: bool)
    ensures r == (opt_bytes(*a) == opt_bytes(*b)),
{
    match (a, b) {
        (Some(x), Some(y)) => bytes_eq_exec(x.as_slice(), y.as_slice()),
        (None, None) => true,
        _ => false,
    }
}

/// The batch's contributions so far: those merged into the classes (`flushed`), and a pending run to one
/// group of one class whose sums are held aside (`seg`), so that a stretch of rows of a filing costs one update.
pub struct Batcher {
    pub dm: Index<DCls>,
    pub has: bool,
    pub fy: i64,
    pub uom: Vec<u8>,
    pub name: Option<Vec<u8>>,
    pub cik: Option<i64>,
    pub cn: Wide,
    pub sm: Wide,
    pub ab: Wide,
    pub flushed: Ghost<Seq<Ct>>,
    pub seg: Ghost<Seq<Ct>>,
}

impl Batcher {
    pub open spec fn ledger(&self) -> Seq<Ct> {
        self.flushed@ + self.seg@
    }

    pub open spec fn ok(&self) -> bool {
        &&& ddm_ok(self.dm, self.flushed@)
        &&& self.has ==> seg_of(self.seg@, cp(self.fy, self.uom@), opt_bytes(self.name), opt_int(self.cik))
            && self.cn@ == seg_cnt(self.seg@) && self.sm@ == seg_sum(self.seg@) && self.ab@ == seg_abs(self.seg@)
        &&& !self.has ==> self.seg@.len() == 0
    }
}

pub fn batch_new() -> (b: Batcher)
    ensures b.ok(), b.ledger() == Seq::<Ct>::empty(),
{
    let dm: Index<DCls> = Index::new();
    proof { lemma_empty_ddm(dm); }
    Batcher { dm, has: false, fy: 0, uom: Vec::new(), name: None, cik: None, cn: Wide::zero(), sm: Wide::zero(), ab: Wide::zero(),
        flushed: Ghost(Seq::empty()), seg: Ghost(Seq::empty()) }
}

pub fn batch_flush(b: &mut Batcher)
    requires old(b).ok(),
    ensures final(b).ok(), final(b).ledger() == old(b).ledger(), !final(b).has,
{
    if b.has {
        acc_ct(&mut b.dm, b.fy, b.uom.as_slice(), &b.name, b.cik, &b.cn, &b.sm, &b.ab, Ghost(b.flushed@), Ghost(b.seg@));
        let ghost all = b.flushed@ + b.seg@;
        b.flushed = Ghost(all);
        b.seg = Ghost(Seq::empty());
        b.has = false;
        proof { assert(b.flushed@ + b.seg@ =~= all); }
    }
}

/// One more signed contribution, to the pending run if it is of the same group.
pub fn batch_add(b: &mut Batcher, fy: i64, uom: &[u8], name: &Option<Vec<u8>>, cik: Option<i64>,
    sg: &Wide, v: &Wide, mag: &Wide, Ghost(c): Ghost<Ct>)
    requires
        old(b).ok(),
        c.fy == fy as int,
        c.uom == uom@,
        c.j.name == opt_bytes(*name),
        c.j.cik == opt_int(cik),
        c.j.vs,
        c.sign == sg@,
        c.j.v == v@,
        mag@ == abs(v@),
    ensures
        final(b).ok(),
        final(b).ledger() == old(b).ledger().push(c),
{
    let same = b.has && b.fy == fy && bytes_eq_exec(b.uom.as_slice(), uom) && same_name(&b.name, name) && b.cik == cik;
    if !same {
        batch_flush(b);
        b.has = true;
        b.fy = fy;
        b.uom = vstd::slice::slice_to_vec(uom);
        b.name = copy_name(name);
        b.cik = cik;
        b.cn = Wide::zero();
        b.sm = Wide::zero();
        b.ab = Wide::zero();
        proof {
            assert(b.seg@.len() == 0);
            lemma_sum_empty(b.seg@, |x: Ct| x.sign);
            lemma_sum_empty(b.seg@, |x: Ct| x.sign * x.j.v);
            lemma_sum_empty(b.seg@, |x: Ct| x.sign * abs(x.j.v));
        }
    }
    let ghost seg0 = b.seg@;
    b.cn = b.cn.add(sg);
    b.sm = b.sm.add(&sg.mul(v));
    b.ab = b.ab.add(&sg.mul(mag));
    b.seg = Ghost(seg0.push(c));
    proof {
        lemma_sum_push(seg0, c, |x: Ct| x.sign);
        lemma_sum_push(seg0, c, |x: Ct| x.sign * x.j.v);
        lemma_sum_push(seg0, c, |x: Ct| x.sign * abs(x.j.v));
        let p = cp(b.fy, b.uom@);
        assert(seg_of(seg0.push(c), p, opt_bytes(b.name), opt_int(b.cik))) by {
            assert forall|i: int| 0 <= i < seg0.push(c).len() implies
                ct_in(#[trigger] seg0.push(c)[i], p) && seg0.push(c)[i].j.vs && seg0.push(c)[i].j.name == opt_bytes(b.name) && seg0.push(c)[i].j.cik == opt_int(b.cik) by {
                if i < seg0.len() { assert(seg0.push(c)[i] == seg0[i]); } else { assert(seg0.push(c)[i] == c); }
            }
        }
        assert(b.flushed@ + seg0.push(c) =~= (b.flushed@ + seg0).push(c));
    }
}

/// The batch's classes, once the pending run is merged in.
pub fn batch_finish(b: Batcher) -> (dm: Index<DCls>)
    requires b.ok(),
    ensures ddm_ok(dm, b.ledger()),
{
    let ghost led = b.ledger();
    let mut b = b;
    batch_flush(&mut b);
    proof { assert(b.ledger() == led); }
    b.dm
}

}
