// The answer rows of a class, summed over its cells.
use vstd::prelude::*;
use verus_common::sums::*;
use verus_common::rel::*;
use verus_prooflib::ordered::{Index, Key, KeyView, key_lt, lemma_key_order, ordered_rows};
use crate::gen_spec::*;
use crate::model::*;
use crate::cells::*;
use crate::bridge::*;
use crate::f64bits::*;

verus! {

/// Every row that can head an answer row has the value of a finite double.
pub open spec fn finite_num(db: Db) -> bool {
    forall|n: NumRow| db.num.contains(n) && is_qual(n) ==> exists|b: u64| f64_finite(b) && f64_val(b) == n.value->0
}

pub open spec fn class_entries(c: Cells, fy: int, uom: Seq<u8>) -> Seq<(KeyView, Vec<Row>)> {
    match c.at((fy, uom)) {
        Some(inner) => inner.entries(),
        None => Seq::empty(),
    }
}

pub open spec fn entry_views(e: (KeyView, Vec<Row>)) -> Seq<OutRow> {
    views(e.1@)
}

pub open spec fn flat_rows(es: Seq<(KeyView, Vec<Row>)>) -> Seq<OutRow> {
    flat(es.map_values(|e: (KeyView, Vec<Row>)| entry_views(e)))
}

pub open spec fn dkey_of(v: int) -> int {
    choose|d: int| vkey(v, d)
}

pub proof fn lemma_vkey_unique(v: int, d1: int, d2: int)
    requires
        vkey(v, d1),
        vkey(v, d2),
    ensures
        d1 == d2,
{
    let b1 = choose|b: u64| f64_finite(b) && f64_val(b) == v && f64_desc_key(b) == d1;
    let b2 = choose|b: u64| f64_finite(b) && f64_val(b) == v && f64_desc_key(b) == d2;
    lemma_desc_key(b1, b2);
}

pub open spec fn nonneg(g: spec_fn(OutRow) -> int) -> bool {
    forall|x: OutRow| #[trigger] g(x) >= 0
}

pub proof fn lemma_pair_w_nonneg(sub: Seq<SubRow>, n: NumRow, fy: int, g: spec_fn(OutRow) -> int)
    requires
        nonneg(g),
    ensures
        pair_w(sub, n, fy, g) >= 0,
{
    let t = |s: SubRow| if sub_hits(n, s, fy) { g(ent_row(n, s)) } else { 0 };
    assert forall|i: int| 0 <= i < sub.len() implies t(#[trigger] sub[i]) >= 0 by {}
    lemma_sum_nonneg(sub, t);
}

pub open spec fn grp_key(n: NumRow) -> KeyView {
    (dkey_of(n.value->0), n.adsh->0)
}

pub open spec fn class_term(db: Db, fy: int, uom: Seq<u8>, g: spec_fn(OutRow) -> int, n: NumRow) -> int {
    if picked(db.num, n, uom) { pair_w(db.sub, n, fy, g) } else { 0 }
}

pub open spec fn key_part(db: Db, fy: int, uom: Seq<u8>, g: spec_fn(OutRow) -> int, k: KeyView) -> int {
    sum(db.num, |n: NumRow| if grp_key(n) == k { class_term(db, fy, uom, g, n) } else { 0 })
}

/// key_part(k) is cell_w
pub proof fn lemma_per_is_cell_w(db: Db, fy: int, uom: Seq<u8>, g: spec_fn(OutRow) -> int, k: KeyView)
    requires
        finite_num(db),
    ensures
        key_part(db, fy, uom, g, k) == cell_w(db, CellId { fy, uom, d: k.0, a: k.1 }, g),
{
    let l = |n: NumRow| if grp_key(n) == k { class_term(db, fy, uom, g, n) } else { 0 };
    let id = CellId { fy, uom, d: k.0, a: k.1 };
    let r = |n: NumRow| if picked(db.num, n, id.uom) && n.adsh == Some(id.a) && vkey(n.value->0, id.d) { pair_w(db.sub, n, id.fy, g) } else { 0 };
    assert forall|n: NumRow| db.num.contains(n) implies #[trigger] l(n) == r(n) by {
        if picked(db.num, n, uom) {
            assert(is_qual(n));
            let b = choose|b: u64| f64_finite(b) && f64_val(b) == n.value->0;
            assert(vkey(n.value->0, f64_desc_key(b)));
            let dk = dkey_of(n.value->0);
            lemma_vkey_unique(n.value->0, dk, f64_desc_key(b));
            if grp_key(n) == k {
                assert(vkey(n.value->0, k.0));
            }
            if n.adsh == Some(id.a) && vkey(n.value->0, id.d) {
                lemma_vkey_unique(n.value->0, id.d, dk);
                assert(grp_key(n) == k);
            }
        }
    }
    assert forall|i: int| 0 <= i < db.num.len() implies l(#[trigger] db.num[i]) == r(db.num[i]) by { assert(db.num.contains(db.num[i])); }
    lemma_sum_ext(db.num, l, r);
}

pub open spec fn dk_le(v: int, b: int) -> bool {
    exists|d: int| vkey(v, d) && d <= b
}

/// g vanishes on every row whose value key exceeds b.
pub open spec fn supp_le(g: spec_fn(OutRow) -> int, b: int) -> bool {
    forall|x: OutRow| #[trigger] g(x) != 0 ==> x.value is Some && dk_le(x.value->0, b)
}

/// The cells of the class hold what the database says up to dmax, and every cell's rows have the cell's value key.
pub open spec fn class_good(c: Cells, fy: int, uom: Seq<u8>, db: Db, dmax: int) -> bool {
    &&& c.wf()
    &&& forall|k: KeyView| #[trigger] c.at(k) is Some ==> c.at(k)->0.wf()
    &&& forall|id: CellId| id.fy == fy && id.uom == uom && id.d <= dmax ==> cell_ok(#[trigger] cell_at(c, id), cw(db), id)
    &&& forall|id: CellId| id.fy == fy && id.uom == uom ==> cell_keyed(#[trigger] cell_at(c, id), id.d)
}

pub open spec fn cell_keyed(co: Option<Vec<Row>>, d: int) -> bool {
    match co {
        Some(v) => forall|k: int| 0 <= k < v@.len() ==> f64_finite((#[trigger] v@[k]).val) && f64_desc_key(v@[k].val) == d,
        None => true,
    }
}

/// n yields a nonzero amount of g only if its value key is within b.
pub proof fn lemma_pair_supp(sub: Seq<SubRow>, n: NumRow, fy: int, g: spec_fn(OutRow) -> int, b: int)
    requires
        supp_le(g, b),
        pair_w(sub, n, fy, g) != 0,
    ensures
        n.value is Some && dk_le(n.value->0, b),
{
    let t = |s: SubRow| if sub_hits(n, s, fy) { g(ent_row(n, s)) } else { 0 };
    let i = lemma_sum_nonzero_witness(sub, t);
    assert(t(sub[i]) != 0);
    assert(g(ent_row(n, sub[i])) != 0);
    assert(ent_row(n, sub[i]).value == n.value);
}

/// The class's cells hold the answer rows of the class that g counts (those of value key at most dmax).
pub proof fn lemma_class_sum(db: Db, c: Cells, fy: int, uom: Seq<u8>, dmax: int, g: spec_fn(OutRow) -> int)
    requires
        class_good(c, fy, uom, db, dmax),
        finite_num(db),
        nonneg(g),
        supp_le(g, dmax),
    ensures
        class_w(db, fy, uom, g) == sum(flat_rows(class_entries(c, fy, uom)), g),
{
    let es = class_entries(c, fy, uom);
    let ks = es.map_values(|e: (KeyView, Vec<Row>)| e.0);
    let f = |n: NumRow| class_term(db, fy, uom, g, n);
    let per = |k: KeyView| key_part(db, fy, uom, g, k);
    if c.at((fy, uom)) is Some {
        c.at((fy, uom))->0.model();
    }
    assert(ordered_rows(es));
    assert(distinct(ks)) by {
        assert forall|i: int, j: int| 0 <= i < ks.len() && 0 <= j < ks.len() && i != j implies ks[i] != ks[j] by {
            if i < j { lemma_key_order(es[i].0, es[j].0); } else { lemma_key_order(es[j].0, es[i].0); }
        }
    }
    let cell_id = |k: KeyView| CellId { fy, uom, d: k.0, a: k.1 };
    assert forall|i: int| 0 <= i < es.len() implies sum(entry_views(#[trigger] es[i]), g) == per(es[i].0) by {
        let k = es[i].0;
        let id = cell_id(k);
        assert(c.at((fy, uom)) is Some);
        let inner = c.at((fy, uom))->0;
        inner.model();
        assert(inner.at(k) == Some(es[i].1));
        assert(cell_at(c, id) == Some(es[i].1));
        if k.0 <= dmax {
            assert(cell_ok(cell_at(c, id), cw(db), id));
            assert(sum(views(es[i].1@), g) == cell_w(db, id, g));
            lemma_per_is_cell_w(db, fy, uom, g, k);
        } else {
            let v = es[i].1@;
            assert forall|q: int| 0 <= q < views(v).len() implies g(#[trigger] views(v)[q]) == 0 by {
                let r = v[q];
                assert(views(v)[q] == r.view());
                assert(f64_finite(r.val) && f64_desc_key(r.val) == k.0);
                if g(r.view()) != 0 {
                    assert(dk_le(f64_val(r.val), dmax));
                    let d2 = choose|d2: int| vkey(f64_val(r.val), d2) && d2 <= dmax;
                    lemma_vkey_iff(r.val, d2);
                }
            }
            lemma_sum_zero(views(v), g);
            let lk = |m: NumRow| if grp_key(m) == k { class_term(db, fy, uom, g, m) } else { 0 };
            assert forall|n: NumRow| #[trigger] lk(n) == 0 by {
                if grp_key(n) == k && picked(db.num, n, uom) {
                    if pair_w(db.sub, n, fy, g) != 0 {
                        lemma_pair_supp(db.sub, n, fy, g, dmax);
                        let d2 = choose|d2: int| vkey(n.value->0, d2) && d2 <= dmax;
                        assert(db.num.contains(n) || true);
                        let dk = dkey_of(n.value->0);
                        assert(vkey(n.value->0, dk));
                        lemma_vkey_unique(n.value->0, dk, d2);
                        assert(k.0 == dk);
                    }
                }
            }
            lemma_sum_zero(db.num, lk);
        }
    }
    assert forall|i: int| 0 <= i < db.num.len() && f(#[trigger] db.num[i]) != 0 implies ks.contains(grp_key(db.num[i])) by {
        let n = db.num[i];
        assert(picked(db.num, n, uom));
        assert(db.num.contains(n));
        assert(pair_w(db.sub, n, fy, g) != 0);
        lemma_pair_supp(db.sub, n, fy, g, dmax);
        let d2 = choose|d2: int| vkey(n.value->0, d2) && d2 <= dmax;
        let b = choose|b: u64| f64_finite(b) && f64_val(b) == n.value->0;
        let d = f64_desc_key(b);
        assert(vkey(n.value->0, d));
        assert(dkey_of(n.value->0) == d) by { lemma_vkey_unique(n.value->0, dkey_of(n.value->0), d); }
        lemma_vkey_unique(n.value->0, d, d2);
        let id = cell_id(grp_key(n));
        assert(id.d <= dmax);
        lemma_pair_w_nonneg(db.sub, n, fy, g);
        let cwt = |m: NumRow| if picked(db.num, m, id.uom) && m.adsh == Some(id.a) && vkey(m.value->0, id.d) { pair_w(db.sub, m, id.fy, g) } else { 0 };
        assert forall|j: int| 0 <= j < db.num.len() implies cwt(#[trigger] db.num[j]) >= 0 by {
            lemma_pair_w_nonneg(db.sub, db.num[j], fy, g);
        }
        lemma_sum_ge_term(db.num, cwt, i);
        assert(cwt(n) == f(n));
        assert(cell_w(db, id, g) == sum(db.num, cwt));
        assert(cell_w(db, id, g) > 0);
        assert(cell_ok(cell_at(c, id), cw(db), id));
        assert(fam_w(cw(db), id, g) == cell_w(db, id, g));
        assert(cell_at(c, id) is Some);
        let inner = c.at((fy, uom))->0;
        inner.model();
        let pos = choose|p: int| 0 <= p < inner.entries().len() && inner.entries()[p].0 == grp_key(n);
        assert(ks[pos] == grp_key(n));
    }
    let keyf = |n: NumRow| grp_key(n);
    assert forall|k: KeyView| #[trigger] per(k) == sum(db.num, |x: NumRow| if keyf(x) == k { f(x) } else { 0 }) by {
        let l1 = |n: NumRow| if grp_key(n) == k { class_term(db, fy, uom, g, n) } else { 0 };
        let l2 = |x: NumRow| if keyf(x) == k { f(x) } else { 0 };
        assert forall|n: NumRow| #[trigger] l1(n) == l2(n) by {}
        lemma_sum_ext_all(db.num, l1, l2);
    }
    lemma_sum_group(db.num, ks, keyf, f, per);
    lemma_sum_map(es, |e: (KeyView, Vec<Row>)| e.0, per, |e: (KeyView, Vec<Row>)| per(e.0));
    let cellsum = |e: (KeyView, Vec<Row>)| sum(entry_views(e), g);
    assert forall|i: int| 0 <= i < es.len() implies (|e: (KeyView, Vec<Row>)| per(e.0))(#[trigger] es[i]) == cellsum(es[i]) by {}
    lemma_sum_ext(es, |e: (KeyView, Vec<Row>)| per(e.0), cellsum);
    let ev = |e: (KeyView, Vec<Row>)| entry_views(e);
    lemma_sum_flat_map(es, ev, g);
    let viaev = |a: (KeyView, Vec<Row>)| sum(ev(a), g);
    assert forall|a: (KeyView, Vec<Row>)| #[trigger] viaev(a) == cellsum(a) by {}
    lemma_sum_ext_all(es, viaev, cellsum);
    assert(flat_rows(es) == flat(es.map_values(ev)));
    let target = |n: NumRow| if picked(db.num, n, uom) { pair_w(db.sub, n, fy, g) } else { 0 };
    assert forall|n: NumRow| #[trigger] f(n) == target(n) by {}
    lemma_sum_ext_all(db.num, f, target);
}

} // verus!
