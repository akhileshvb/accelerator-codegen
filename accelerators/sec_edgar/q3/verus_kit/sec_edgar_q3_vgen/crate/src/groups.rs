// Grouping the join records by (name, cik): per group the exact count, sum and absolute sum of the
// values, held in an Index keyed by the group.
use vstd::prelude::*;
use std::sync::Arc;
use verus_common::rel::*;
use verus_common::sums::*;
use verus_prooflib::ordered::{Index, Key, KeyView};
use verus_prooflib::wide::Wide;
use crate::f64bits::*;
use crate::gen_spec::*;
use crate::join::*;
use crate::join_spec::*;
use crate::rows::*;

verus! {

pub struct Cell {
    pub name: Option<Vec<u8>>,
    pub cik: Option<i64>,
    pub count: usize,
    pub sum: Wide,
    pub abs: Wide,
    pub v1: u64,
}

pub open spec fn flag<A>(x: Option<A>) -> u8 {
    if x is Some { 1u8 } else { 0u8 }
}

pub open spec fn gk(name: Option<Seq<u8>>, cik: Option<int>) -> KeyView {
    (match cik { Some(a) => a, None => 0int },
     seq![flag(cik), flag(name)] + match name { Some(b) => b, None => Seq::empty() })
}

pub proof fn lemma_gk_inj(name: Option<Seq<u8>>, cik: Option<int>, name2: Option<Seq<u8>>, cik2: Option<int>)
    requires gk(name, cik) == gk(name2, cik2),
    ensures name == name2, cik == cik2,
{
    let (a, b) = (gk(name, cik).1, gk(name2, cik2).1);
    assert(a[0] == flag(cik) && a[1] == flag(name));
    assert(b[0] == flag(cik2) && b[1] == flag(name2));
    if name is Some && name2 is Some {
        assert(a.skip(2) =~= name->0);
        assert(b.skip(2) =~= name2->0);
    }
}

pub open spec fn in_group(r: Rec, name: Option<Seq<u8>>, cik: Option<int>) -> bool {
    rec_jk(r).name == name && rec_jk(r).cik == cik
}

pub open spec fn gsum(l: Seq<Rec>, name: Option<Seq<u8>>, cik: Option<int>, f: spec_fn(JK) -> int) -> int {
    sum(l, |r: Rec| if in_group(r, name, cik) { f(rec_jk(r)) } else { 0 })
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

pub open spec fn cell_ok(c: Cell, l: Seq<Rec>, name: Option<Seq<u8>>, cik: Option<int>) -> bool {
    &&& opt_bytes(c.name) == name
    &&& opt_int(c.cik) == cik
    &&& c.count as int == gsum(l, name, cik, one())
    &&& c.sum@ == gsum(l, name, cik, value())
    &&& c.abs@ == gsum(l, name, cik, magnitude())
    &&& c.count >= 1
    &&& f64_finite(c.v1)
    &&& c.count == 1 ==> c.sum@ == f64_val(c.v1)
}

pub open spec fn groups_ok(cells: Index<Cell>, l: Seq<Rec>) -> bool {
    &&& cells.wf()
    &&& forall|name: Option<Seq<u8>>, cik: Option<int>| #[trigger] cells.at(gk(name, cik)) is Some
        ==> cell_ok(cells.at(gk(name, cik))->0, l, name, cik)
    &&& forall|name: Option<Seq<u8>>, cik: Option<int>| #[trigger] cells.at(gk(name, cik)) is None
        ==> gsum(l, name, cik, one()) == 0
    &&& forall|k: KeyView| #[trigger] cells.at(k) is Some ==> k == gk(opt_bytes(cells.at(k)->0.name), opt_int(cells.at(k)->0.cik))
}

pub proof fn lemma_gsum_push(l: Seq<Rec>, r: Rec, name: Option<Seq<u8>>, cik: Option<int>, f: spec_fn(JK) -> int)
    ensures
        gsum(l.push(r), name, cik, f) == gsum(l, name, cik, f) + if in_group(r, name, cik) { f(rec_jk(r)) } else { 0 },
{
    lemma_sum_push(l, r, |x: Rec| if in_group(x, name, cik) { f(rec_jk(x)) } else { 0 });
}

fn gk_key(name: &Option<Vec<u8>>, cik: Option<i64>) -> (k: Key)
    ensures k.view() == gk(match name { Some(b) => Some(b@), None => None }, opt_int(cik)),
{
    let mut text: Vec<u8> = Vec::new();
    text.push(if cik.is_some() { 1 } else { 0 });
    text.push(if name.is_some() { 1 } else { 0 });
    if let Some(b) = name {
        let mut i: usize = 0;
        while i < b.len()
            invariant
                i <= b.len(),
                text@ == seq![if cik.is_some() { 1u8 } else { 0u8 }, if name.is_some() { 1u8 } else { 0u8 }] + b@.take(i as int),
            decreases b.len() - i,
        {
            text.push(b[i]);
            proof { assert(text@ =~= seq![if cik.is_some() { 1u8 } else { 0u8 }, if name.is_some() { 1u8 } else { 0u8 }] + b@.take(i as int + 1)); }
            i += 1;
        }
        proof { assert(b@.take(b.len() as int) =~= b@); }
    }
    let number = match cik { Some(a) => a, None => 0 };
    Key::composite(number, text)
}

fn copy_cell(c: &Cell) -> (r: Cell)
    ensures
        r.cik == c.cik,
        opt_bytes(r.name) == opt_bytes(c.name),
        r.count == c.count, r.sum@ == c.sum@, r.abs@ == c.abs@, r.v1 == c.v1,
{
    Cell {
        name: match &c.name { Some(b) => Some(copy_bytes(b.as_slice())), None => None },
        cik: c.cik, count: c.count, sum: c.sum.dup(), abs: c.abs.dup(), v1: c.v1,
    }
}

proof fn lemma_group_empty(l: Seq<Rec>, name: Option<Seq<u8>>, cik: Option<int>)
    requires gsum(l, name, cik, one()) == 0,
    ensures forall|f: spec_fn(JK) -> int| #[trigger] gsum(l, name, cik, f) == 0,
    decreases l.len(),
{
    if l.len() > 0 {
        let rest = l.drop_last();
        let last = l.last();
        assert(l =~= rest.push(last));
        let t1 = |x: Rec| if in_group(x, name, cik) { one()(rec_jk(x)) } else { 0 };
        assert forall|i: int| 0 <= i < rest.len() implies t1(#[trigger] rest[i]) >= 0 by {}
        lemma_sum_nonneg(rest, t1);
        lemma_gsum_push(rest, last, name, cik, one());
        lemma_group_empty(rest, name, cik);
        assert forall|f: spec_fn(JK) -> int| #[trigger] gsum(l, name, cik, f) == 0 by {
            lemma_gsum_push(rest, last, name, cik, f);
            if in_group(last, name, cik) { assert(false); }
        }
    }
}

proof fn lemma_gsum_one_len(l: Seq<Rec>, name: Option<Seq<u8>>, cik: Option<int>)
    ensures gsum(l, name, cik, one()) <= l.len(),
    decreases l.len(),
{
    if l.len() > 0 {
        let rest = l.drop_last();
        let last = l.last();
        assert(l =~= rest.push(last));
        lemma_gsum_one_len(rest, name, cik);
        lemma_gsum_push(rest, last, name, cik, one());
    }
}

pub fn add_rec(cells: &mut Index<Cell>, r: &Rec, Ghost(l): Ghost<Seq<Rec>>)
    requires
        groups_ok(*old(cells), l),
        rec_ok(*r),
        l.len() < usize::MAX,
    ensures
        groups_ok(*final(cells), l.push(*r)),
{
    let ghost before = *cells;
    let ghost gn = opt_bytes(r.name);
    let ghost gc = opt_int(r.cik);
    let key = gk_key(&r.name, r.cik);
    let found = cells.get(&key);
    let mut c: Cell = match &found {
        Some(a) => copy_cell(&**a),
        None => Cell { name: match &r.name { Some(b) => Some(copy_bytes(b.as_slice())), None => None },
                       cik: r.cik, count: 0, sum: Wide::zero(), abs: Wide::zero(), v1: 0 },
    };
    proof {
        match &found {
            Some(a) => {
                assert(cells.at(gk(gn, gc)) == Some(**a));
                assert(cell_ok(**a, l, gn, gc));
            },
            None => {
                assert(cells.at(gk(gn, gc)) is None);
                lemma_group_empty(l, gn, gc);
                assert(gsum(l, gn, gc, value()) == 0);
                assert(gsum(l, gn, gc, magnitude()) == 0);
            },
        }
        lemma_gsum_one_len(l, gn, gc);
        lemma_gsum_push(l, *r, gn, gc, one());
        lemma_gsum_push(l, *r, gn, gc, value());
        lemma_gsum_push(l, *r, gn, gc, magnitude());
        assert(in_group(*r, gn, gc));
    }
    let was = c.count;
    proof { assert(found is None ==> was == 0 && c.sum@ == 0 && c.abs@ == 0); assert(was as int == gsum(l, gn, gc, one()) || found is None); }
    c.count = c.count + 1;
    c.sum = c.sum.add(&r.v);
    let mag = if r.v.is_neg() { r.v.neg() } else { r.v.dup() };
    c.abs = c.abs.add(&mag);
    c.v1 = r.vb;
    proof {
        assert(abs(r.v@) == mag@);
        if c.count == 1 {
            assert(was == 0);
            assert(found is None);
            assert(c.sum@ == r.v@);
        }
    }
    cells.put(key.share(), Arc::new(c));
    proof {
        let l2 = l.push(*r);
        assert forall|name: Option<Seq<u8>>, cik: Option<int>| (#[trigger] cells.at(gk(name, cik)) is Some)
            implies cell_ok(cells.at(gk(name, cik))->0, l2, name, cik) by {
            if gk(name, cik) == key.view() {
                lemma_gk_inj(name, cik, gn, gc);
            } else {
                assert(cells.at(gk(name, cik)) == before.at(gk(name, cik)));
                assert(!in_group(*r, name, cik));
                lemma_gsum_push(l, *r, name, cik, one());
                lemma_gsum_push(l, *r, name, cik, value());
                lemma_gsum_push(l, *r, name, cik, magnitude());
            }
        }
        assert forall|name: Option<Seq<u8>>, cik: Option<int>| (#[trigger] cells.at(gk(name, cik)) is None)
            implies gsum(l2, name, cik, one()) == 0 by {
            assert(gk(name, cik) != key.view());
            assert(before.at(gk(name, cik)) is None);
            assert(!in_group(*r, name, cik));
            lemma_gsum_push(l, *r, name, cik, one());
        }
        assert forall|k: KeyView| #[trigger] cells.at(k) is Some
            implies k == gk(opt_bytes(cells.at(k)->0.name), opt_int(cells.at(k)->0.cik)) by {
            if k != key.view() { assert(cells.at(k) == before.at(k)); }
        }
    }
}

}
