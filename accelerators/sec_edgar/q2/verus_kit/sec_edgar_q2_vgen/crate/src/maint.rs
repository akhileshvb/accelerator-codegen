// Retracting and inserting one filing's cells, with the directory that lists them.
use vstd::prelude::*;
use std::sync::Arc;
use verus_common::sums::*;
use verus_common::rel::*;
use verus_common::bytes::*;
use verus_common::sort::*;
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

pub open spec fn ck_lt(a: CK, b: CK) -> bool {
    a.fy < b.fy || (a.fy == b.fy && (bytes_lt((*a.uom)@, (*b.uom)@) || ((*a.uom)@ == (*b.uom)@ && a.d < b.d)))
}

pub open spec fn ck_same(a: CK, b: CK) -> bool {
    a.fy == b.fy && (*a.uom)@ == (*b.uom)@ && a.d == b.d
}

pub open spec fn pick_ents(ents: Seq<Ent>, s: Seq<usize>) -> Seq<Ent> {
    s.map_values(|i: usize| ents[i as int])
}

pub proof fn lemma_ck_order(a: CK, b: CK, c: CK)
    ensures
        ck_lt(a, b) || ck_lt(b, a) || ck_same(a, b),
        !(ck_lt(a, b) && ck_lt(b, a)),
        ck_lt(a, b) && ck_lt(b, c) ==> ck_lt(a, c),
        ck_same(a, b) ==> !ck_lt(a, b),
{
    if (*a.uom)@ != (*b.uom)@ { lemma_bytes_lt_total((*a.uom)@, (*b.uom)@); }
    lemma_bytes_lt_asym((*a.uom)@, (*b.uom)@);
    lemma_bytes_lt_irrefl((*a.uom)@);
    if ck_lt(a, b) && ck_lt(b, c) && a.fy == b.fy && b.fy == c.fy {
        if bytes_lt((*a.uom)@, (*b.uom)@) && bytes_lt((*b.uom)@, (*c.uom)@) { lemma_bytes_lt_trans((*a.uom)@, (*b.uom)@, (*c.uom)@); }
    }
}

pub fn ck_lt_exec(a: &CK, b: &CK) -> (r: bool)
    ensures r == ck_lt(*a, *b),
{
    if a.fy != b.fy { return a.fy < b.fy; }
    let c = verus_prooflib::ordered::compare_bytes(a.uom.as_slice(), b.uom.as_slice());
    if c != 0 { return c < 0; }
    a.d < b.d
}

pub fn ck_same_exec(a: &CK, b: &CK) -> (r: bool)
    ensures r == ck_same(*a, *b),
{
    a.fy == b.fy && a.d == b.d && bytes_eq_exec(a.uom.as_slice(), b.uom.as_slice())
}

/// The entries' positions with equal cells next to each other.
pub fn sort_ents(ents: &Vec<Ent>) -> (sorted: Vec<usize>)
    ensures
        forall|k: int| 0 <= k < sorted@.len() ==> (#[trigger] sorted@[k]) < ents@.len(),
        forall|f: spec_fn(Ent) -> int| #[trigger] sum(pick_ents(ents@, sorted@), f) == sum(ents@, f),
{
    let mut pos: Vec<usize> = Vec::new();
    let mut i: usize = 0;
    while i < ents.len()
        invariant
            i <= ents@.len(),
            pos@.len() == i,
            forall|k: int| 0 <= k < pos@.len() ==> (#[trigger] pos@[k]) as int == k,
        decreases ents.len() - i,
    {
        pos.push(i);
        i += 1;
    }
    let ghost ev = ents@;
    let ghost le = |x: usize, y: usize| !ck_lt(ev[y as int].ck, ev[x as int].ck);
    let ghost valid = |x: usize| x < ev.len();
    let ents_ref = ents;
    let compare = |x: usize, y: usize| -> (r: bool)
        requires x < ents_ref.len(), y < ents_ref.len(),
        ensures r == !ck_lt(ents_ref@[y as int].ck, ents_ref@[x as int].ck),
    { !ck_lt_exec(&ents_ref[y].ck, &ents_ref[x].ck) };
    proof {
        assert forall|x: usize, y: usize| valid(x) && valid(y) implies #[trigger] le(x, y) || le(y, x) by {
            lemma_ck_order(ev[x as int].ck, ev[y as int].ck, ev[y as int].ck);
        }
        assert forall|x: usize, y: usize, z: usize| valid(x) && valid(y) && valid(z) && #[trigger] le(x, y) && #[trigger] le(y, z) implies le(x, z) by {
            let (a, b, c) = (ev[x as int].ck, ev[y as int].ck, ev[z as int].ck);
            lemma_ck_order(c, b, a);
            lemma_ck_order(c, a, b);
            lemma_ck_order(b, a, c);
        }
        assert(preorder_on(le, valid));
        assert(all_valid(pos@, valid));
        assert(cmp_ok(&compare, le, valid)) by {
            assert forall|x: usize, y: usize| valid(x) && valid(y) implies #[trigger] compare.requires((x, y)) by {}
            assert forall|x: usize, y: usize, r: bool| #[trigger] compare.ensures((x, y), r) implies r == le(x, y) by {}
        }
    }
    let sorted = sort_idx(&pos, &compare, Ghost(le), Ghost(valid));
    proof {
        assert forall|k: int| 0 <= k < sorted@.len() implies (#[trigger] sorted@[k]) < ents@.len() by { assert(valid(sorted@[k])); }
        assert forall|f: spec_fn(Ent) -> int| #[trigger] sum(pick_ents(ents@, sorted@), f) == sum(ents@, f) by {
            let fi = |x: usize| f(ev[x as int]);
            assert(sum(sorted@, fi) == sum(pos@, fi));
            lemma_sum_map(sorted@, |x: usize| ev[x as int], f, fi);
            assert(ev =~= pos@.map_values(|x: usize| ev[x as int]));
            lemma_sum_map(pos@, |x: usize| ev[x as int], f, fi);
        }
    }
    sorted
}

/// Flushing a run of entries with one cell key into that cell.
pub proof fn lemma_flush(
    before: Cells, after: Cells, w: Fam, av: Seq<u8>, s: Seq<Ent>, f: int, j: int, acc: Seq<Row>, me: CellId,
)
    requires
        cells_rep(before, w_ins(w, av, s.take(f))),
        0 <= f < j <= s.len(),
        me.a == av,
        forall|p: int| f <= p < j ==> ck_matches((#[trigger] s[p]).ck, me),
        forall|p: int| f <= p < j ==> ent_ok(#[trigger] s[p]),
        acc == s.subrange(f, j).map_values(|e: Ent| e.row),
        after.wf(),
        forall|k: KeyView| #[trigger] after.at(k) is Some ==> after.at(k)->0.wf(),
        cell_at(after, me) is Some,
        cell_at(after, me)->0@ == (match cell_at(before, me) { Some(v) => v@, None => Seq::empty() }) + acc,
        forall|id: CellId| id != me ==> #[trigger] cell_at(after, id) == cell_at(before, id),
    ensures
        cells_rep(after, w_ins(w, av, s.take(j))),
{
    let pre = s.take(f);
    let post = s.take(j);
    let run = s.subrange(f, j);
    assert(post =~= pre + run);
    assert forall|id: CellId| #[trigger] cell_ok(cell_at(after, id), w_ins(w, av, post), id) by {
        if id == me {
            let base = match cell_at(before, me) { Some(v) => v@, None => Seq::empty() };
            let cell = cell_at(after, me)->0;
            assert(cell_ok(cell_at(before, me), w_ins(w, av, pre), me));
            assert forall|g: spec_fn(OutRow) -> int| #[trigger] sum(views(cell@), g) == fam_w(w_ins(w, av, post), me, g) by {
                let phi = |e: Ent| if ck_matches(e.ck, me) { g(e.row.view()) } else { 0 };
                lemma_sum_concat(pre, run, phi);
                assert(ent_sum(post, me, g) == ent_sum(pre, me, g) + ent_sum(run, me, g));
                let gr = |e: Ent| g(e.row.view());
                assert forall|e: Ent| run.contains(e) implies #[trigger] phi(e) == gr(e) by {
                    let q = choose|q: int| 0 <= q < run.len() && run[q] == e;
                    assert(run[q] == s[f + q]);
                }
                assert forall|q: int| 0 <= q < run.len() implies phi(#[trigger] run[q]) == gr(run[q]) by { assert(run[q] == s[f + q]); }
                lemma_sum_ext(run, phi, gr);
                lemma_cell_concat(base, acc, g);
                lemma_run_sum(run, g);
                match cell_at(before, me) {
                    Some(v) => { assert(sum(views(v@), g) == fam_w(w_ins(w, av, pre), me, g)); }
                    None => {
                        assert(fam_w(w_ins(w, av, pre), me, g) == 0);
                        assert(views(base) =~= Seq::<OutRow>::empty());
                        lemma_sum_empty(Seq::<OutRow>::empty(), g);
                    }
                }
            }
            assert forall|k: int| 0 <= k < cell@.len() implies f64_finite((#[trigger] cell@[k]).val) && f64_desc_key(cell@[k].val) == me.d by {
                if k < base.len() {
                    match cell_at(before, me) { Some(v) => { assert(cell@[k] == v@[k]); } None => {} }
                } else {
                    let q = k - base.len();
                    assert(cell@[k] == acc[q]);
                    assert(acc[q] == run[q].row);
                    assert(run[q] == s[f + q]);
                    assert(ent_ok(s[f + q]) && ck_matches(s[f + q].ck, me));
                }
            }
        } else {
            assert(cell_ok(cell_at(before, id), w_ins(w, av, pre), id));
            assert forall|g: spec_fn(OutRow) -> int| #[trigger] fam_w(w_ins(w, av, post), id, g) == fam_w(w_ins(w, av, pre), id, g) by {
                if id.a == av {
                    let phi = |e: Ent| if ck_matches(e.ck, id) { g(e.row.view()) } else { 0 };
                    lemma_sum_concat(pre, run, phi);
                    let z = |e: Ent| 0int;
                    assert forall|q: int| 0 <= q < run.len() implies phi(#[trigger] run[q]) == z(run[q]) by {
                        assert(run[q] == s[f + q]);
                        assert(ck_matches(s[f + q].ck, me));
                        assert(!ck_matches(s[f + q].ck, id));
                    }
                    lemma_sum_ext(run, phi, z);
                    lemma_sum_zero(run, z);
                }
            }
            lemma_cell_ok_congr(cell_at(before, id), w_ins(w, av, pre), w_ins(w, av, post), id);
        }
    }
}

pub proof fn lemma_cell_concat(a: Seq<Row>, b: Seq<Row>, g: spec_fn(OutRow) -> int)
    ensures
        sum(views(a + b), g) == sum(views(a), g) + sum(views(b), g),
{
    assert(views(a + b) =~= views(a) + views(b));
    lemma_sum_concat(views(a), views(b), g);
}

pub proof fn lemma_run_sum(run: Seq<Ent>, g: spec_fn(OutRow) -> int)
    ensures
        sum(run, |e: Ent| g(e.row.view())) == sum(views(run.map_values(|e: Ent| e.row)), g),
{
    let rows = run.map_values(|e: Ent| e.row);
    lemma_sum_map(rows, |r: Row| r.view(), g, |r: Row| g(r.view()));
    lemma_sum_map(run, |e: Ent| e.row, |r: Row| g(r.view()), |e: Ent| g(e.row.view()));
    assert(views(rows) =~= rows.map_values(|r: Row| r.view()));
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
    let sorted = sort_ents(ents);
    let ghost sv = pick_ents(ents@, sorted@);
    proof {
        assert forall|id: CellId, g: spec_fn(OutRow) -> int| #[trigger] fam_w(w_ins(w, av, sv.take(0)), id, g) == fam_w(w, id, g) by {
            if id.a == av {
                assert(sv.take(0) =~= Seq::<Ent>::empty());
                lemma_sum_empty(Seq::<Ent>::empty(), |x: Ent| if ck_matches(x.ck, id) { g(x.row.view()) } else { 0 });
                assert(zero_fam(w, id));
                assert(fam_w(w, id, g) == 0);
            }
        }
        lemma_rep_congr(*c, w, w_ins(w, av, sv.take(0)));
        assert forall|j: int| 0 <= j < sv.len() implies ent_ok(#[trigger] sv[j]) by { assert(sv[j] == ents@[sorted@[j] as int]); }
    }
    let n = sorted.len();
    let mut acc: Vec<Row> = Vec::new();
    let mut f: usize = 0;
    let mut j: usize = 0;
    while j < n
        invariant
            j <= n,
            f <= j,
            n == sorted@.len(),
            av == (**a)@,
            sv == pick_ents(ents@, sorted@),
            forall|k: int| 0 <= k < sorted@.len() ==> (#[trigger] sorted@[k]) < ents@.len(),
            forall|q: int| 0 <= q < sv.len() ==> ent_ok(#[trigger] sv[q]),
            cells_rep(*c, w_ins(w, av, sv.take(f as int))),
            acc@ == sv.subrange(f as int, j as int).map_values(|e: Ent| e.row),
            forall|p: int| f <= p < j ==> ck_same((#[trigger] sv[p]).ck, sv[f as int].ck),
        decreases n - j,
    {
        let e = &ents[sorted[j]];
        proof { assert(sv[j as int] == *e); }
        if j > f && !ck_same_exec(&ents[sorted[f]].ck, &e.ck) {
            let ghost before = *c;
            let ghost me = CellId { fy: sv[f as int].ck.fy as int, uom: (*sv[f as int].ck.uom)@, d: sv[f as int].ck.d as int, a: av };
            proof { assert(sv[f as int] == ents@[sorted@[f as int] as int]); }
            cells_append(c, ents[sorted[f]].ck.fy, &ents[sorted[f]].ck.uom, ents[sorted[f]].ck.d, a, &acc);
            proof {
                assert forall|p: int| f <= p < j implies ck_matches((#[trigger] sv[p]).ck, me) by { assert(ck_same(sv[p].ck, sv[f as int].ck)); }
                lemma_flush(before, *c, w, av, sv, f as int, j as int, acc@, me);
            }
            acc = Vec::new();
            f = j;
            proof { assert(sv.subrange(f as int, f as int) =~= Seq::<Ent>::empty()); }
        }
        acc.push(copy_row(&e.row));
        proof {
            assert(sv.subrange(f as int, j as int + 1) =~= sv.subrange(f as int, j as int).push(sv[j as int]));
            assert(sv.subrange(f as int, j as int + 1).map_values(|e: Ent| e.row) =~= sv.subrange(f as int, j as int).map_values(|e: Ent| e.row).push(sv[j as int].row));
            assert forall|p: int| f <= p < j + 1 implies ck_same((#[trigger] sv[p]).ck, sv[f as int].ck) by {
                if p == j as int { assert(ck_same(ents@[sorted@[j as int] as int].ck, sv[f as int].ck)) by {
                    if j > f { assert(ck_same(sv[f as int].ck, e.ck)); }
                } }
            }
        }
        j += 1;
    }
    if j > f {
        let ghost before = *c;
        let ghost me = CellId { fy: sv[f as int].ck.fy as int, uom: (*sv[f as int].ck.uom)@, d: sv[f as int].ck.d as int, a: av };
        proof { assert(sv[f as int] == ents@[sorted@[f as int] as int]); }
        cells_append(c, ents[sorted[f]].ck.fy, &ents[sorted[f]].ck.uom, ents[sorted[f]].ck.d, a, &acc);
        proof {
            assert forall|p: int| f <= p < j implies ck_matches((#[trigger] sv[p]).ck, me) by { assert(ck_same(sv[p].ck, sv[f as int].ck)); }
            lemma_flush(before, *c, w, av, sv, f as int, j as int, acc@, me);
        }
    }
    proof {
        assert(sv.take(sv.len() as int) =~= sv);
        assert forall|id: CellId, g: spec_fn(OutRow) -> int| #[trigger] fam_w(w_ins(w, av, ents@), id, g) == fam_w(w_ins(w, av, sv), id, g) by {
            if id.a == av {
                let phi = |x: Ent| if ck_matches(x.ck, id) { g(x.row.view()) } else { 0 };
                assert(sum(sv, phi) == sum(ents@, phi));
            }
        }
        lemma_rep_congr(*c, w_ins(w, av, sv), w_ins(w, av, ents@));
    }
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
