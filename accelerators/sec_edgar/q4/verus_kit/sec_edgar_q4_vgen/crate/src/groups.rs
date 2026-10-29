// Grouping the join records by (sic, tlabel): per group the exact count, sum and absolute sum of
// the values and the set of companies, held in an Index keyed by the group.
use vstd::prelude::*;
use std::sync::Arc;
use verus_common::rel::*;
use verus_common::sums::*;
use verus_prooflib::ordered::{Index, Key, KeyView};
use verus_prooflib::wide::Wide;
use crate::gen_spec::*;
use crate::join::*;
use crate::join_spec::*;
use crate::rows::*;

verus! {

pub struct Cell {
    pub sic: Option<i64>,
    pub tlabel: Option<Vec<u8>>,
    pub count: Wide,
    pub sum: Wide,
    pub abs: Wide,
    pub ciks: Index<()>,
    pub nciks: usize,
    pub cs: Ghost<Seq<int>>,
}

pub open spec fn flag<A>(x: Option<A>) -> u8 {
    if x is Some { 1u8 } else { 0u8 }
}

pub open spec fn gk(sic: Option<int>, tl: Option<Seq<u8>>) -> KeyView {
    (match sic { Some(a) => a, None => 0int },
     seq![flag(sic), flag(tl)] + match tl { Some(b) => b, None => Seq::empty() })
}

pub proof fn lemma_gk_inj(sic: Option<int>, tl: Option<Seq<u8>>, sic2: Option<int>, tl2: Option<Seq<u8>>)
    requires gk(sic, tl) == gk(sic2, tl2),
    ensures sic == sic2, tl == tl2,
{
    let (a, b) = (gk(sic, tl).1, gk(sic2, tl2).1);
    assert(a[0] == flag(sic) && a[1] == flag(tl));
    assert(b[0] == flag(sic2) && b[1] == flag(tl2));
    if tl is Some && tl2 is Some {
        assert(a.skip(2) =~= tl->0);
        assert(b.skip(2) =~= tl2->0);
    }
}

pub open spec fn in_group(p: Params, r: Rec, sic: Option<int>, tl: Option<Seq<u8>>) -> bool {
    rec_jk(p, r).sic == sic && rec_jk(p, r).tlabel == tl
}

pub open spec fn gsum(l: Seq<Rec>, p: Params, sic: Option<int>, tl: Option<Seq<u8>>, f: spec_fn(JK) -> int) -> int {
    sum(l, |r: Rec| if in_group(p, r, sic, tl) { r.w as int * f(rec_jk(p, r)) } else { 0 })
}

pub open spec fn cikseq(l: Seq<Rec>, p: Params, sic: Option<int>, tl: Option<Seq<u8>>) -> Seq<int>
    decreases l.len(),
{
    if l.len() == 0 {
        Seq::empty()
    } else {
        let rest = cikseq(l.drop_last(), p, sic, tl);
        let r = l.last();
        if in_group(p, r, sic, tl) && r.w > 0 && r.cik is Some { rest.push(r.cik->0 as int) } else { rest }
    }
}

pub open spec fn one() -> spec_fn(JK) -> int {
    |j: JK| 1int
}

pub open spec fn value() -> spec_fn(JK) -> int {
    |j: JK| j.v
}

pub open spec fn magnitude() -> spec_fn(JK) -> int {
    |j: JK| abs(j.v)
}

pub open spec fn cell_ok(c: Cell, l: Seq<Rec>, p: Params, sic: Option<int>, tl: Option<Seq<u8>>) -> bool {
    &&& opt_int(c.sic) == sic
    &&& opt_bytes(c.tlabel) == tl
    &&& c.count@ == gsum(l, p, sic, tl, one())
    &&& c.sum@ == gsum(l, p, sic, tl, value())
    &&& c.abs@ == gsum(l, p, sic, tl, magnitude())
    &&& c.cs@ == dedup(cikseq(l, p, sic, tl))
    &&& c.nciks as int == c.cs@.len()
    &&& c.ciks.wf()
    &&& forall|c0: int| #[trigger] c.ciks.at((c0, Seq::<u8>::empty())).is_some() == c.cs@.contains(c0)
}

pub open spec fn groups_ok(cells: Index<Cell>, l: Seq<Rec>, p: Params) -> bool {
    &&& cells.wf()
    &&& forall|sic: Option<int>, tl: Option<Seq<u8>>| #[trigger] cells.at(gk(sic, tl)) is Some
        ==> cell_ok(cells.at(gk(sic, tl))->0, l, p, sic, tl)
    &&& forall|sic: Option<int>, tl: Option<Seq<u8>>| #[trigger] cells.at(gk(sic, tl)) is None
        ==> gsum(l, p, sic, tl, one()) == 0
    &&& forall|k: KeyView| #[trigger] cells.at(k) is Some ==> k == gk(opt_int(cells.at(k)->0.sic), opt_bytes(cells.at(k)->0.tlabel))
}

proof fn lemma_dedup_push(s: Seq<int>, x: int)
    ensures dedup(s.push(x)) == if dedup(s).contains(x) { dedup(s) } else { dedup(s).push(x) },
{
    assert(s.push(x).drop_last() =~= s);
    assert(s.push(x).last() == x);
}

proof fn lemma_dedup_len(s: Seq<int>)
    ensures dedup(s).len() <= s.len(),
    decreases s.len(),
{
    if s.len() > 0 {
        lemma_dedup_len(s.drop_last());
    }
}

proof fn lemma_cikseq_len(l: Seq<Rec>, p: Params, sic: Option<int>, tl: Option<Seq<u8>>)
    ensures cikseq(l, p, sic, tl).len() <= l.len(),
    decreases l.len(),
{
    if l.len() > 0 {
        lemma_cikseq_len(l.drop_last(), p, sic, tl);
    }
}

proof fn lemma_cikseq_push(l: Seq<Rec>, r: Rec, p: Params, sic: Option<int>, tl: Option<Seq<u8>>)
    ensures
        cikseq(l.push(r), p, sic, tl)
            == if in_group(p, r, sic, tl) && r.w > 0 && r.cik is Some { cikseq(l, p, sic, tl).push(r.cik->0 as int) } else { cikseq(l, p, sic, tl) },
{
    assert(l.push(r).drop_last() =~= l);
    assert(l.push(r).last() == r);
}

/// A record adds its weight to its group's sums, and to no other group's.
proof fn lemma_gsum_push(l: Seq<Rec>, r: Rec, p: Params, sic: Option<int>, tl: Option<Seq<u8>>, f: spec_fn(JK) -> int)
    ensures
        gsum(l.push(r), p, sic, tl, f)
            == gsum(l, p, sic, tl, f) + if in_group(p, r, sic, tl) { r.w as int * f(rec_jk(p, r)) } else { 0 },
{
    lemma_sum_push(l, r, |x: Rec| if in_group(p, x, sic, tl) { x.w as int * f(rec_jk(p, x)) } else { 0 });
}

fn gk_key(sic: Option<i64>, tl: &Option<Vec<u8>>) -> (k: Key)
    ensures k.view() == gk(opt_int(sic), match tl { Some(b) => Some(b@), None => None }),
{
    let mut text: Vec<u8> = Vec::new();
    text.push(if sic.is_some() { 1 } else { 0 });
    text.push(if tl.is_some() { 1 } else { 0 });
    if let Some(b) = tl {
        let mut i: usize = 0;
        while i < b.len()
            invariant
                i <= b.len(),
                text@ == seq![if sic.is_some() { 1u8 } else { 0u8 }, if tl.is_some() { 1u8 } else { 0u8 }] + b@.take(i as int),
            decreases b.len() - i,
        {
            text.push(b[i]);
            proof { assert(text@ =~= seq![if sic.is_some() { 1u8 } else { 0u8 }, if tl.is_some() { 1u8 } else { 0u8 }] + b@.take(i as int + 1)); }
            i += 1;
        }
        proof { assert(b@.take(b.len() as int) =~= b@); }
    }
    let number = match sic { Some(a) => a, None => 0 };
    Key::composite(number, text)
}

fn copy_cell(c: &Cell) -> (r: Cell)
    ensures
        r.sic == c.sic,
        opt_bytes(r.tlabel) == opt_bytes(c.tlabel),
        r.count@ == c.count@,
        r.sum@ == c.sum@,
        r.abs@ == c.abs@,
        r.ciks.wf() == c.ciks.wf(),
        r.ciks.view() == c.ciks.view(),
        forall|k: KeyView| #[trigger] r.ciks.at(k) == c.ciks.at(k),
        r.nciks == c.nciks,
        r.cs@ == c.cs@,
{
    Cell {
        sic: c.sic,
        tlabel: match &c.tlabel { Some(b) => Some(copy_bytes(b.as_slice())), None => None },
        count: c.count.dup(),
        sum: c.sum.dup(),
        abs: c.abs.dup(),
        ciks: c.ciks.share(),
        nciks: c.nciks,
        cs: Ghost(c.cs@),
    }
}

fn empty_cell(sic: Option<i64>, tl: &Option<Vec<u8>>) -> (r: Cell)
    ensures
        r.sic == sic,
        opt_bytes(r.tlabel) == (match tl { Some(b) => Some(b@), None => None }),
        r.count@ == 0, r.sum@ == 0, r.abs@ == 0,
        r.ciks.wf(),
        forall|k: KeyView| #[trigger] r.ciks.at(k).is_none(),
        r.nciks == 0,
        r.cs@ == Seq::<int>::empty(),
{
    Cell {
        sic,
        tlabel: match tl { Some(b) => Some(copy_bytes(b.as_slice())), None => None },
        count: Wide::zero(),
        sum: Wide::zero(),
        abs: Wide::zero(),
        ciks: Index::new(),
        nciks: 0,
        cs: Ghost(Seq::empty()),
    }
}


proof fn lemma_group_empty(l: Seq<Rec>, p: Params, sic: Option<int>, tl: Option<Seq<u8>>)
    requires
        forall|i: int| 0 <= i < l.len() ==> (#[trigger] l[i]).w >= 0,
        gsum(l, p, sic, tl, one()) == 0,
    ensures
        forall|f: spec_fn(JK) -> int| #[trigger] gsum(l, p, sic, tl, f) == 0,
        cikseq(l, p, sic, tl) == Seq::<int>::empty(),
    decreases l.len(),
{
    if l.len() > 0 {
        let rest = l.drop_last();
        let last = l.last();
        assert(l =~= rest.push(last));
        assert forall|i: int| 0 <= i < rest.len() implies (#[trigger] rest[i]).w >= 0 by { assert(rest[i] == l[i]); }
        assert(last.w >= 0) by { assert(l[l.len() - 1] == last); }
        let t1 = |x: Rec| if in_group(p, x, sic, tl) { x.w as int * one()(rec_jk(p, x)) } else { 0 };
        assert forall|i: int| 0 <= i < rest.len() implies t1(#[trigger] rest[i]) >= 0 by {}
        lemma_sum_nonneg(rest, t1);
        lemma_gsum_push(rest, last, p, sic, tl, one());
        lemma_group_empty(rest, p, sic, tl);
        assert forall|f: spec_fn(JK) -> int| #[trigger] gsum(l, p, sic, tl, f) == 0 by {
            lemma_gsum_push(rest, last, p, sic, tl, f);
            if in_group(p, last, sic, tl) { assert(last.w == 0); }
        }
        lemma_cikseq_push(rest, last, p, sic, tl);
    } else {
        assert(l =~= Seq::<Rec>::empty());
    }
}

/// A record of weight 0 changes nothing.
fn add_nothing(cells: &Index<Cell>, r: &Rec, Ghost(l): Ghost<Seq<Rec>>, Ghost(p): Ghost<Params>)
    requires
        groups_ok(*cells, l, p),
        r.w == 0,
    ensures
        groups_ok(*cells, l.push(*r), p),
{
    assert forall|sic: Option<int>, tl: Option<Seq<u8>>| (#[trigger] cells.at(gk(sic, tl)) is Some)
        implies cell_ok(cells.at(gk(sic, tl))->0, l.push(*r), p, sic, tl) by {
        let c = cells.at(gk(sic, tl))->0;
        lemma_gsum_push(l, *r, p, sic, tl, one());
        lemma_gsum_push(l, *r, p, sic, tl, value());
        lemma_gsum_push(l, *r, p, sic, tl, magnitude());
        lemma_cikseq_push(l, *r, p, sic, tl);
    }
    assert forall|sic: Option<int>, tl: Option<Seq<u8>>| (#[trigger] cells.at(gk(sic, tl)) is None)
        implies gsum(l.push(*r), p, sic, tl, one()) == 0 by {
        lemma_gsum_push(l, *r, p, sic, tl, one());
    }
}

/// The cell of the record's group before the record: the stored one, or an empty one.
fn cell_before(cells: &Index<Cell>, key: &Key, r: &Rec, Ghost(l): Ghost<Seq<Rec>>, Ghost(p): Ghost<Params>) -> (c: Cell)
    requires
        groups_ok(*cells, l, p),
        key.view() == gk(opt_int(r.sic), opt_bytes(r.tlabel)),
        forall|i: int| 0 <= i < l.len() ==> (#[trigger] l[i]).w >= 0,
    ensures
        cell_ok(c, l, p, opt_int(r.sic), opt_bytes(r.tlabel)),
{
    let ghost gs = opt_int(r.sic);
    let ghost gt = opt_bytes(r.tlabel);
    let found = cells.get(key);
    match found {
        Some(a) => {
            let c = copy_cell(&*a);
            proof {
                assert(cells.at(gk(gs, gt)) == Some(*a));
                assert(cell_ok(*a, l, p, gs, gt));
                assert forall|c0: int| #[trigger] c.ciks.at((c0, Seq::<u8>::empty())).is_some() == c.cs@.contains(c0) by {
                    assert(a.ciks.at((c0, Seq::<u8>::empty())) == c.ciks.at((c0, Seq::<u8>::empty())));
                }
            }
            c
        },
        None => {
            let c = empty_cell(r.sic, &r.tlabel);
            proof {
                assert(cells.at(gk(gs, gt)) is None);
                lemma_group_empty(l, p, gs, gt);
                reveal_with_fuel(dedup, 1);
                assert(cikseq(l, p, gs, gt) =~= Seq::<int>::empty());
                assert forall|c0: int| #[trigger] c.ciks.at((c0, Seq::<u8>::empty())).is_some() == c.cs@.contains(c0) by {
                    assert(c.ciks.at((c0, Seq::<u8>::empty())).is_none());
                    assert(!c.cs@.contains(c0));
                }
            }
            c
        },
    }
}

/// Add the record to its group's cell.
fn extend_cell(c: &mut Cell, r: &Rec, Ghost(l): Ghost<Seq<Rec>>, Ghost(p): Ghost<Params>)
    requires
        cell_ok(*old(c), l, p, opt_int(r.sic), opt_bytes(r.tlabel)),
        r.w > 0,
        l.len() < usize::MAX,
    ensures
        cell_ok(*final(c), l.push(*r), p, opt_int(r.sic), opt_bytes(r.tlabel)),
{
    let ghost gs = opt_int(r.sic);
    let ghost gt = opt_bytes(r.tlabel);
    let ghost ciks_init = c.ciks;
    let ghost cs_init = c.cs@;
    proof {
        assert forall|x: int| #[trigger] ciks_init.at((x, Seq::<u8>::empty())).is_some() == cs_init.contains(x) by {}
        lemma_dedup_len(cikseq(l, p, gs, gt));
        lemma_cikseq_len(l, p, gs, gt);
        let l2 = l.push(*r);
        lemma_gsum_push(l, *r, p, gs, gt, one());
        lemma_gsum_push(l, *r, p, gs, gt, value());
        lemma_gsum_push(l, *r, p, gs, gt, magnitude());
        lemma_cikseq_push(l, *r, p, gs, gt);
        assert(in_group(p, *r, gs, gt));
    }
    let wv = Wide::from_i128(r.w);
    c.count = c.count.add(&wv);
    let prod = wv.mul(&r.v);
    c.sum = c.sum.add(&prod);
    let mag = if r.v.is_neg() { r.v.neg() } else { r.v.dup() };
    let pa = wv.mul(&mag);
    c.abs = c.abs.add(&pa);
    proof { assert(abs(r.v@) == mag@); }
    let ghost cs0 = c.cs@;
    let ghost ciks0 = c.ciks;
    if let Some(c0) = r.cik {
        let ck = Key::number(c0);
        if c.ciks.get(&ck).is_none() {
            c.ciks.put(ck, Arc::new(()));
            c.nciks += 1;
            c.cs = Ghost(cs0.push(c0 as int));
            proof {
                lemma_dedup_push(cikseq(l, p, gs, gt), c0 as int);
                assert(!cs0.contains(c0 as int)) by {
                    assert(ciks0.at((c0 as int, Seq::<u8>::empty())) is None);
                }
                assert(c.cs@ =~= cs0.push(c0 as int));
                assert forall|x: int| #[trigger] c.ciks.at((x, Seq::<u8>::empty())).is_some() == c.cs@.contains(x) by {
                    assert(ciks0.at((x, Seq::<u8>::empty())).is_some() == cs_init.contains(x));
                    assert(cs0 == cs_init);
                    assert(ck.view() == (c0 as int, Seq::<u8>::empty()));
                    if x == c0 as int {
                        assert(c.ciks.at((x, Seq::<u8>::empty())) == Some(()));
                        assert(c.cs@.last() == x);
                        assert(c.cs@.contains(x));
                    } else {
                        assert(c.ciks.at((x, Seq::<u8>::empty())) == ciks0.at((x, Seq::<u8>::empty())));
                        assert(c.cs@.contains(x) == cs0.contains(x));
                    }
                }
            }
        } else {
            proof {
                lemma_dedup_push(cikseq(l, p, gs, gt), c0 as int);
                assert(cs0.contains(c0 as int));
            }
        }
    }
}

pub fn add_rec(cells: &mut Index<Cell>, r: &Rec, Ghost(l): Ghost<Seq<Rec>>, Ghost(p): Ghost<Params>)
    requires
        groups_ok(*old(cells), l, p),
        r.w >= 0,
        forall|i: int| 0 <= i < l.len() ==> (#[trigger] l[i]).w >= 0,
        l.len() < usize::MAX,
    ensures
        groups_ok(*final(cells), l.push(*r), p),
{
    if r.w == 0 {
        add_nothing(cells, r, Ghost(l), Ghost(p));
        return;
    }
    let ghost before = *cells;
    let ghost gs = opt_int(r.sic);
    let ghost gt = opt_bytes(r.tlabel);
    let key = gk_key(r.sic, &r.tlabel);
    let mut c = cell_before(cells, &key, r, Ghost(l), Ghost(p));
    extend_cell(&mut c, r, Ghost(l), Ghost(p));
    cells.put(key.share(), Arc::new(c));
    proof {
        let l2 = l.push(*r);
        assert forall|sic: Option<int>, tl: Option<Seq<u8>>| (#[trigger] cells.at(gk(sic, tl)) is Some)
            implies cell_ok(cells.at(gk(sic, tl))->0, l2, p, sic, tl) by {
            if gk(sic, tl) == key.view() {
                lemma_gk_inj(sic, tl, gs, gt);
            } else {
                assert(cells.at(gk(sic, tl)) == before.at(gk(sic, tl)));
                assert(!in_group(p, *r, sic, tl));
                lemma_gsum_push(l, *r, p, sic, tl, one());
                lemma_gsum_push(l, *r, p, sic, tl, value());
                lemma_gsum_push(l, *r, p, sic, tl, magnitude());
                lemma_cikseq_push(l, *r, p, sic, tl);
            }
        }
        assert forall|sic: Option<int>, tl: Option<Seq<u8>>| (#[trigger] cells.at(gk(sic, tl)) is None)
            implies gsum(l2, p, sic, tl, one()) == 0 by {
            assert(gk(sic, tl) != key.view());
            assert(before.at(gk(sic, tl)) is None);
            assert(!in_group(p, *r, sic, tl));
            lemma_gsum_push(l, *r, p, sic, tl, one());
        }
        assert forall|k: KeyView| #[trigger] cells.at(k) is Some
            implies k == gk(opt_int(cells.at(k)->0.sic), opt_bytes(cells.at(k)->0.tlabel)) by {
            if k != key.view() { assert(cells.at(k) == before.at(k)); }
        }
    }
}

}
