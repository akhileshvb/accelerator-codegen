// Retracting and inserting one filing's cells, with the directory that lists them.
use vstd::prelude::*;
use std::sync::Arc;
use verus_common::sums::*;
use verus_common::rel::*;
use verus_prooflib::ordered::{Index, Key, KeyView};
use crate::gen_spec::*;
use crate::model::*;
use crate::cells::*;
use crate::f64bits::*;

verus! {

/// A cell key: the class (fy, uom) and the value's index number.
pub struct CK {
    pub fy: i64,
    pub uom: Arc<Vec<u8>>,
    pub d: i64,
}

/// One answer row of a filing with its cell.
pub struct Ent {
    pub ck: CK,
    pub row: Row,
}

pub open spec fn ck_matches(ck: CK, id: CellId) -> bool {
    ck.fy as int == id.fy && (*ck.uom)@ == id.uom && ck.d as int == id.d
}

pub open spec fn ent_ok(e: Ent) -> bool {
    f64_finite(e.row.val) && f64_desc_key(e.row.val) == e.ck.d as int
}

pub open spec fn ent_sum(ents: Seq<Ent>, id: CellId, g: spec_fn(OutRow) -> int) -> int {
    sum(ents, |e: Ent| if ck_matches(e.ck, id) { g(e.row.view()) } else { 0 })
}

pub open spec fn listed(l: Seq<CK>, id: CellId) -> bool {
    exists|j: int| 0 <= j < l.len() && ck_matches(#[trigger] l[j], id)
}

pub open spec fn zero_fam(w: Fam, id: CellId) -> bool {
    forall|g: spec_fn(OutRow) -> int| #[trigger] fam_w(w, id, g) == 0
}

pub open spec fn dir_of(h: Index<Vec<CK>>, a: Seq<u8>) -> Seq<CK> {
    match h.at((0int, a)) { Some(v) => v@, None => Seq::empty() }
}

/// Every cell the family fills is listed in its filing's directory entry.
pub open spec fn cover(h: Index<Vec<CK>>, w: Fam) -> bool {
    &&& h.wf()
    &&& forall|id: CellId| !(#[trigger] zero_fam(w, id)) ==> listed(dir_of(h, id.a), id)
}

pub open spec fn w_zero(w: Fam, a: Seq<u8>) -> Fam {
    |id: CellId, g: spec_fn(OutRow) -> int| if id.a == a { 0int } else { w(id, g) }
}

pub proof fn lemma_zero_congr(w1: Fam, w2: Fam, id: CellId)
    requires
        forall|g: spec_fn(OutRow) -> int| #[trigger] fam_w(w2, id, g) == fam_w(w1, id, g),
    ensures
        zero_fam(w1, id) == zero_fam(w2, id),
{
    if zero_fam(w1, id) {
        assert forall|g: spec_fn(OutRow) -> int| #[trigger] fam_w(w2, id, g) == 0 by { assert(fam_w(w1, id, g) == 0); }
    }
    if zero_fam(w2, id) {
        assert forall|g: spec_fn(OutRow) -> int| #[trigger] fam_w(w1, id, g) == 0 by { assert(fam_w(w2, id, g) == 0); }
    }
}

pub proof fn lemma_zero_w_zero(w: Fam, a: Seq<u8>, id: CellId)
    ensures
        zero_fam(w_zero(w, a), id) == (id.a == a || zero_fam(w, id)),
{
    if id.a != a && zero_fam(w, id) {
        assert forall|g: spec_fn(OutRow) -> int| #[trigger] fam_w(w_zero(w, a), id, g) == 0 by {
            assert(fam_w(w, id, g) == 0);
        }
    }
    if id.a == a {
        assert forall|g: spec_fn(OutRow) -> int| #[trigger] fam_w(w_zero(w, a), id, g) == 0 by {}
    }
    if id.a != a && !zero_fam(w, id) {
        let g = choose|g: spec_fn(OutRow) -> int| fam_w(w, id, g) != 0;
        assert(fam_w(w_zero(w, a), id, g) != 0);
    }
}

pub open spec fn w_cut(w: Fam, a: Seq<u8>, l: Seq<CK>) -> Fam {
    |id: CellId, g: spec_fn(OutRow) -> int| if id.a == a && listed(l, id) { 0int } else { w(id, g) }
}

pub open spec fn w_ins(w: Fam, a: Seq<u8>, ents: Seq<Ent>) -> Fam {
    |id: CellId, g: spec_fn(OutRow) -> int| if id.a == a { ent_sum(ents, id, g) } else { w(id, g) }
}

pub open spec fn ck_ids(ents: Seq<Ent>) -> Seq<CK> {
    ents.map_values(|e: Ent| e.ck)
}

pub fn copy_ck(k: &CK) -> (c: CK)
    ensures c == *k,
{
    CK { fy: k.fy, uom: k.uom.clone(), d: k.d }
}

pub fn copy_dir(src: &Vec<CK>) -> (out: Vec<CK>)
    ensures out@ == src@,
{
    let mut out: Vec<CK> = Vec::new();
    let mut i: usize = 0;
    while i < src.len()
        invariant
            i <= src@.len(),
            out@ == src@.take(i as int),
        decreases src.len() - i,
    {
        out.push(copy_ck(&src[i]));
        proof { assert(src@.take(i as int + 1) =~= src@.take(i as int).push(src@[i as int])); }
        i += 1;
    }
    proof { assert(src@.take(src@.len() as int) =~= src@); }
    out
}

/// Remove every cell of filing a that the directory lists; forget the directory entry.
pub fn retract(c: &mut Cells, h: &mut Index<Vec<CK>>, a: &Arc<Vec<u8>>, Ghost(w): Ghost<Fam>)
    requires
        cells_rep(*old(c), w),
        cover(*old(h), w),
    ensures
        cells_rep(*final(c), w_zero(w, (**a)@)),
        cover(*final(h), w_zero(w, (**a)@)),
{
    let ghost av = (**a)@;
    let key = kkey(0, a);
    let ghost h0 = *old(h);
    match h.get(&key) {
        None => {
            proof {
                assert(dir_of(h0, av) == Seq::<CK>::empty());
                assert forall|id: CellId, g: spec_fn(OutRow) -> int| #[trigger] fam_w(w_zero(w, av), id, g) == fam_w(w, id, g) by {
                    if id.a == av {
                        assert(!listed(Seq::<CK>::empty(), id));
                        assert(zero_fam(w, id));
                        assert(fam_w(w, id, g) == 0);
                    }
                }
                lemma_rep_congr(*c, w, w_zero(w, av));
                assert forall|id: CellId| !(#[trigger] zero_fam(w_zero(w, av), id)) implies listed(dir_of(*h, id.a), id) by {
                    lemma_zero_w_zero(w, av, id);
                    assert(!zero_fam(w, id));
                }
            }
        }
        Some(l) => {
            let ghost lv = l@;
            proof {
                assert forall|id: CellId, g: spec_fn(OutRow) -> int| #[trigger] fam_w(w_cut(w, av, lv.take(0)), id, g) == fam_w(w, id, g) by {
                    assert(!listed(lv.take(0), id));
                }
                lemma_rep_congr(*c, w, w_cut(w, av, lv.take(0)));
            }
            let mut i: usize = 0;
            while i < l.len()
                invariant
                    i <= l@.len(),
                    l@ == lv,
                    av == (**a)@,
                    *h == h0,
                    dir_of(h0, av) == lv,
                    cover(h0, w),
                    cells_rep(*c, w_cut(w, av, lv.take(i as int))),
                decreases l.len() - i,
            {
                let ghost me = CellId { fy: l@[i as int].fy as int, uom: (*l@[i as int].uom)@, d: l@[i as int].d as int, a: av };
                let ghost before = *c;
                cells_remove(c, l[i].fy, &l[i].uom, l[i].d, a);
                proof {
                    assert(l[i as int].fy == l@[i as int].fy);
                    assert((*l[i as int].uom)@ == (*l@[i as int].uom)@);
                    assert(cell_at(*c, CellId { fy: l[i as int].fy as int, uom: (*l[i as int].uom)@, d: l[i as int].d as int, a: (**a)@ }) is None);
                    assert(av == (**a)@);
                    assert(me == CellId { fy: l[i as int].fy as int, uom: (*l[i as int].uom)@, d: l[i as int].d as int, a: (**a)@ });
                    let pre = lv.take(i as int);
                    let post = lv.take(i as int + 1);
                    assert(post =~= pre.push(lv[i as int]));
                    assert forall|id: CellId| #[trigger] cell_ok(cell_at(*c, id), w_cut(w, av, post), id) by {
                        if id == me {
                            assert(ck_matches(post[i as int], id));
                            assert(listed(post, id));
                            assert(cell_at(*c, id) is None);
                            assert forall|g: spec_fn(OutRow) -> int| #[trigger] fam_w(w_cut(w, av, post), id, g) == 0 by {}
                        } else {
                            assert(cell_at(*c, id) == cell_at(before, id));
                            assert(cell_ok(cell_at(before, id), w_cut(w, av, pre), id));
                            assert(id.a == av ==> (listed(post, id) == listed(pre, id))) by {
                                if id.a == av {
                                    assert(ck_matches(lv[i as int], id) ==> id == me);
                                    if listed(post, id) {
                                        let j = choose|j: int| 0 <= j < post.len() && ck_matches(post[j], id);
                                        if j < pre.len() { assert(pre[j] == post[j]); }
                                    }
                                    if listed(pre, id) {
                                        let j = choose|j: int| 0 <= j < pre.len() && ck_matches(pre[j], id);
                                        assert(post[j] == pre[j]);
                                    }
                                }
                            }
                            lemma_cell_ok_congr(cell_at(before, id), w_cut(w, av, pre), w_cut(w, av, post), id);
                        }
                    }
                }
                i += 1;
            }
            h.remove(&key);
            proof {
                assert(lv.take(lv.len() as int) =~= lv);
                assert forall|id: CellId, g: spec_fn(OutRow) -> int| #[trigger] fam_w(w_zero(w, av), id, g) == fam_w(w_cut(w, av, lv), id, g) by {
                    if id.a == av && !listed(lv, id) {
                        assert(dir_of(h0, id.a) == lv);
                        assert(zero_fam(w, id));
                        assert(fam_w(w, id, g) == 0);
                    }
                }
                lemma_rep_congr(*c, w_cut(w, av, lv), w_zero(w, av));
                assert forall|id: CellId| !(#[trigger] zero_fam(w_zero(w, av), id)) implies listed(dir_of(*h, id.a), id) by {
                    lemma_zero_w_zero(w, av, id);
                    assert(!zero_fam(w, id));
                    assert(dir_of(*h, id.a) == dir_of(h0, id.a));
                }
            }
        }
    }
}


pub proof fn lemma_ent_sum_push(ents: Seq<Ent>, e: Ent, id: CellId, g: spec_fn(OutRow) -> int)
    ensures
        ent_sum(ents.push(e), id, g) == ent_sum(ents, id, g) + if ck_matches(e.ck, id) { g(e.row.view()) } else { 0 },
{
    lemma_sum_push(ents, e, |x: Ent| if ck_matches(x.ck, id) { g(x.row.view()) } else { 0 });
}

pub proof fn lemma_views_push(c: Seq<Row>, r: Row)
    ensures
        views(c.push(r)) == views(c).push(r.view()),
{
    assert(views(c.push(r)) =~= views(c).push(r.view()));
}

/// Add every row of filing a (whose cells are empty) to its cell and list its cells.
pub fn insert_filing(c: &mut Cells, h: &mut Index<Vec<CK>>, a: &Arc<Vec<u8>>, ents: &Vec<Ent>, Ghost(w): Ghost<Fam>)
    requires
        cells_rep(*old(c), w),
        cover(*old(h), w),
        forall|id: CellId| id.a == (**a)@ ==> zero_fam(w, id),
        forall|j: int| 0 <= j < ents@.len() ==> ent_ok(#[trigger] ents@[j]),
    ensures
        cells_rep(*final(c), w_ins(w, (**a)@, ents@)),
        cover(*final(h), w_ins(w, (**a)@, ents@)),
{
    let ghost av = (**a)@;
    let ghost h0 = *old(h);
    proof {
        assert forall|id: CellId, g: spec_fn(OutRow) -> int| #[trigger] fam_w(w_ins(w, av, ents@.take(0)), id, g) == fam_w(w, id, g) by {
            if id.a == av {
                assert(ents@.take(0) =~= Seq::<Ent>::empty());
                lemma_sum_empty(Seq::<Ent>::empty(), |x: Ent| if ck_matches(x.ck, id) { g(x.row.view()) } else { 0 });
                assert(zero_fam(w, id));
                assert(fam_w(w, id, g) == 0);
            }
        }
        lemma_rep_congr(*c, w, w_ins(w, av, ents@.take(0)));
    }
    let mut i: usize = 0;
    while i < ents.len()
        invariant
            i <= ents@.len(),
            av == (**a)@,
            forall|j: int| 0 <= j < ents@.len() ==> ent_ok(#[trigger] ents@[j]),
            cells_rep(*c, w_ins(w, av, ents@.take(i as int))),
        decreases ents.len() - i,
    {
        let e = &ents[i];
        let ghost before = *c;
        let ghost pre = ents@.take(i as int);
        let ghost post = ents@.take(i as int + 1);
        let ghost me = CellId { fy: e.ck.fy as int, uom: (*e.ck.uom)@, d: e.ck.d as int, a: av };
        proof { assert(post =~= pre.push(ents@[i as int])); }
        cells_push(c, e.ck.fy, &e.ck.uom, e.ck.d, a, copy_row(&e.row));
        proof {
            assert(ents@[i as int] == *e);
            assert(ck_matches(e.ck, me));
            let cell = cell_at(*c, me)->0;
            let base = match cell_at(before, me) { Some(v) => v@, None => Seq::empty() };
            assert(cell@ == base.push(e.row));
            assert forall|id: CellId| #[trigger] cell_ok(cell_at(*c, id), w_ins(w, av, post), id) by {
                if id == me {
                    assert(cell_ok(cell_at(before, me), w_ins(w, av, pre), me));
                    assert forall|g: spec_fn(OutRow) -> int| #[trigger] sum(views(cell@), g) == fam_w(w_ins(w, av, post), me, g) by {
                        lemma_views_push(base, e.row);
                        lemma_sum_push(views(base), e.row.view(), g);
                        lemma_ent_sum_push(pre, *e, me, g);
                        match cell_at(before, me) {
                            Some(v) => { assert(sum(views(v@), g) == fam_w(w_ins(w, av, pre), me, g)); }
                            None => {
                                assert(fam_w(w_ins(w, av, pre), me, g) == 0);
                                lemma_sum_empty(Seq::<OutRow>::empty(), g);
                                assert(views(base) =~= Seq::<OutRow>::empty());
                            }
                        }
                    }
                    assert forall|k: int| 0 <= k < cell@.len() implies f64_finite((#[trigger] cell@[k]).val) && f64_desc_key(cell@[k].val) == id.d by {
                        if k < base.len() {
                            match cell_at(before, me) { Some(v) => { assert(cell@[k] == v@[k]); } None => {} }
                        } else {
                            assert(cell@[k] == e.row);
                            assert(ent_ok(*e));
                        }
                    }
                } else {
                    assert(cell_at(*c, id) == cell_at(before, id));
                    assert(cell_ok(cell_at(before, id), w_ins(w, av, pre), id));
                    assert forall|g: spec_fn(OutRow) -> int| #[trigger] fam_w(w_ins(w, av, post), id, g) == fam_w(w_ins(w, av, pre), id, g) by {
                        if id.a == av {
                            lemma_ent_sum_push(pre, *e, id, g);
                            assert(!ck_matches(e.ck, id));
                        }
                    }
                    lemma_cell_ok_congr(cell_at(before, id), w_ins(w, av, pre), w_ins(w, av, post), id);
                }
            }
        }
        i += 1;
    }
    proof { assert(ents@.take(ents@.len() as int) =~= ents@); }
    let mut keys: Vec<CK> = Vec::new();
    let mut j: usize = 0;
    while j < ents.len()
        invariant
            j <= ents@.len(),
            keys@ == ck_ids(ents@.take(j as int)),
        decreases ents.len() - j,
    {
        keys.push(copy_ck(&ents[j].ck));
        proof { assert(ents@.take(j as int + 1) =~= ents@.take(j as int).push(ents@[j as int])); }
        j += 1;
    }
    proof { assert(ents@.take(ents@.len() as int) =~= ents@); }
    let key = kkey(0, a);
    h.put(key, Arc::new(keys));
    proof {
        assert(dir_of(*h, av) == ck_ids(ents@));
        assert forall|id: CellId| !(#[trigger] zero_fam(w_ins(w, av, ents@), id)) implies listed(dir_of(*h, id.a), id) by {
            if id.a == av {
                let g = choose|g: spec_fn(OutRow) -> int| fam_w(w_ins(w, av, ents@), id, g) != 0;
                if !listed(ck_ids(ents@), id) {
                    assert forall|k: int| 0 <= k < ents@.len() implies (|x: Ent| if ck_matches(x.ck, id) { g(x.row.view()) } else { 0 })(#[trigger] ents@[k]) == 0 by {
                        assert(ck_ids(ents@)[k] == ents@[k].ck);
                        assert(!ck_matches(ck_ids(ents@)[k], id));
                    }
                    lemma_sum_zero(ents@, |x: Ent| if ck_matches(x.ck, id) { g(x.row.view()) } else { 0 });
                }
            } else {
                assert(dir_of(*h, id.a) == dir_of(h0, id.a));
                assert forall|g: spec_fn(OutRow) -> int| #[trigger] fam_w(w_ins(w, av, ents@), id, g) == fam_w(w, id, g) by {}
                lemma_zero_congr(w, w_ins(w, av, ents@), id);
            }
        }
    }
}

} // verus!
