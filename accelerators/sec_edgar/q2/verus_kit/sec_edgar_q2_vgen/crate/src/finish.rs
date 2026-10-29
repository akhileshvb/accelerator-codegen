// The answer from the collected rows: they come first, in order, and the rest follow.
use vstd::prelude::*;
use verus_common::sums::*;
use verus_common::rel::*;
use verus_common::order::*;
use verus_prooflib::ordered::{Index, Key, KeyView, key_lt, ordered_rows};
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::model::*;
use crate::cells::*;
use crate::bridge::*;
use crate::classes::*;
use crate::f64bits::*;

verus! {

pub proof fn lemma_rows_multiset(db: Db, c: Cells, fy: int, uom: Seq<u8>)
    requires
        cells_rep(c, cw(db)),
        finite_num(db),
    ensures
        rows(db, Params { p1: fy, p2: uom }).to_multiset() =~= flat_rows(class_entries(c, fy, uom)).to_multiset(),
{
    broadcast use vstd::multiset::group_multiset_axioms;
    let p = Params { p1: fy, p2: uom };
    let fl = flat_rows(class_entries(c, fy, uom));
    assert forall|x: OutRow| rows(db, p).to_multiset().count(x) == fl.to_multiset().count(x) by {
        lemma_count_as_sum(rows(db, p), x);
        lemma_count_as_sum(fl, x);
        assert(nonneg(eqv(x))) by { assert forall|y: OutRow| #[trigger] eqv(x)(y) >= 0 by {} }
        lemma_rows_sum(db, p, eqv(x));
        lemma_class_sum(db, c, fy, uom, eqv(x));
    }
}

pub proof fn lemma_flat_concat<A>(a: Seq<Seq<A>>, b: Seq<Seq<A>>)
    ensures
        flat(a + b) == flat(a) + flat(b),
    decreases b.len(),
{
    if b.len() == 0 {
        assert(a + b =~= a);
        assert(flat(b) == Seq::<A>::empty());
    } else {
        let b0 = b.drop_last();
        lemma_flat_concat(a, b0);
        assert(a + b =~= (a + b0).push(b.last()));
        assert((a + b).drop_last() =~= a + b0);
        assert((a + b).last() == b.last());
        assert(flat(a + b) == flat(a + b0) + b.last());
        assert(flat(b) == flat(b0) + b.last());
        assert(flat(a) + flat(b) =~= (flat(a) + flat(b0)) + b.last());
    }
}

/// The rows of the entries before and after a position make the rows of all.
pub proof fn lemma_flat_rows_split(es: Seq<(KeyView, Vec<Row>)>, m: int)
    requires
        0 <= m <= es.len(),
    ensures
        flat_rows(es) == flat_rows(es.take(m)) + flat_rows(es.skip(m)),
{
    let ev = |e: (KeyView, Vec<Row>)| entry_views(e);
    assert(es.map_values(ev) =~= es.take(m).map_values(ev) + es.skip(m).map_values(ev));
    lemma_flat_concat(es.take(m).map_values(ev), es.skip(m).map_values(ev));
}

/// The rows of a cell hold its key's value.
pub proof fn lemma_entry_rows(db: Db, c: Cells, fy: int, uom: Seq<u8>, i: int)
    requires
        cells_rep(c, cw(db)),
        0 <= i < class_entries(c, fy, uom).len(),
    ensures
        ({
            let e = class_entries(c, fy, uom)[i];
            forall|k: int| 0 <= k < e.1@.len() ==> f64_finite((#[trigger] e.1@[k]).val) && f64_desc_key(e.1@[k].val) == e.0.0
        }),
{
    let es = class_entries(c, fy, uom);
    let inner = c.at((fy, uom))->0;
    inner.model();
    let e = es[i];
    let id = CellId { fy, uom, d: e.0.0, a: e.0.1 };
    assert(inner.at(e.0) == Some(e.1));
    assert(cell_at(c, id) == Some(e.1));
    assert(cell_ok(cell_at(c, id), cw(db), id));
}

/// Collected rows C (value keys at most d), the rest R (value keys beyond): the answer starts with C sorted.
pub proof fn lemma_finish(db: Db, params: Params, c_rows: Seq<OutRow>, sc: Seq<OutRow>, r_rows: Seq<OutRow>)
    requires
        rows(db, params).to_multiset() =~= c_rows.to_multiset().add(r_rows.to_multiset()),
        forall|x: OutRow, y: OutRow| c_rows.contains(x) && r_rows.contains(y) ==> x.value is Some && y.value is Some && y.value->0 < x.value->0,
        nondesc(sc, ext_order()),
        sc.to_multiset() =~= c_rows.to_multiset(),
        sc.len() >= 100 || r_rows.len() == 0,
    ensures
        query_result(db, params, limit_rows(sc, 100)),
{
    broadcast use vstd::seq_lib::group_to_multiset_ensures;
    lemma_ext_order();
    assert forall|x: OutRow, y: OutRow| r_rows.contains(x) && r_rows.contains(y) && x != y implies #[trigger] ext_order()(x, y) || #[trigger] ext_order()(y, x) by {
        lemma_ext_total(x, y);
    }
    lemma_sort_dups(r_rows, ext_order());
    let sr = sort_dups(r_rows, ext_order());
    let full = sc + sr;
    lemma_to_multiset_concat(sc, sr);
    assert(full.to_multiset() =~= rows(db, params).to_multiset());
    assert forall|i: int, j: int| 0 <= i < j < full.len() implies !row_lt(#[trigger] full[j], #[trigger] full[i]) by {
        lemma_row_lt_ext(full[j], full[i]);
        if j < sc.len() {
            assert(full[i] == sc[i] && full[j] == sc[j]);
        } else if i >= sc.len() {
            assert(full[i] == sr[i - sc.len()] && full[j] == sr[j - sc.len()]);
        } else {
            let x = sc[i];
            let y = sr[j - sc.len()];
            assert(full[i] == x && full[j] == y);
            assert(sc.contains(x)) by { assert(sc[i] == x); }
            assert(c_rows.contains(x)) by {
                assert(sc.to_multiset().count(x) > 0);
            }
            assert(sr.contains(y)) by { assert(sr[j - sc.len()] == y); }
            assert(r_rows.contains(y));
        }
    }
    assert(full_result(db, params, full));
    assert(limit_rows(sc, 100) == limit_rows(full, 100)) by {
        if sc.len() >= 100 {
            if full.len() <= 100 {
                assert(sr.len() == 0);
                assert(full =~= sc);
            } else {
                assert(full.take(100) =~= sc.take(100));
                if sc.len() <= 100 { assert(sc.take(100) =~= sc); }
            }
        } else {
            assert(r_rows.to_multiset().len() == 0);
            assert(sr.len() == 0);
            assert(full =~= sc);
        }
    }
}


pub proof fn lemma_flat_rows_push(es: Seq<(KeyView, Vec<Row>)>, m: int)
    requires
        0 <= m < es.len(),
    ensures
        flat_rows(es.take(m + 1)) == flat_rows(es.take(m)) + entry_views(es[m]),
{
    let ev = |e: (KeyView, Vec<Row>)| entry_views(e);
    assert(es.take(m + 1).map_values(ev) =~= es.take(m).map_values(ev).push(ev(es[m])));
    assert(es.take(m + 1).map_values(ev).drop_last() =~= es.take(m).map_values(ev));
    assert(es.take(m + 1).map_values(ev).last() == ev(es[m]));
}


/// What the class walk delivers: a prefix of the entries, a hundred rows or all, ending at a value boundary.
pub open spec fn coll_ok(coll: Seq<Row>, es: Seq<(KeyView, Vec<Row>)>) -> bool {
    &&& crate::sortout::rows_finite(coll)
    &&& exists|m: int| {
        &&& 0 <= m <= es.len()
        &&& views(coll) == flat_rows(es.take(m))
        &&& (coll.len() >= 100 || m == es.len())
        &&& (m == es.len() || (m > 0 && es[m].0.0 > es[m - 1].0.0))
    }
}

pub proof fn lemma_flat_rows_member(es: Seq<(KeyView, Vec<Row>)>, x: OutRow) -> (ik: (int, int))
    requires
        flat_rows(es).contains(x),
    ensures
        0 <= ik.0 < es.len(),
        0 <= ik.1 < es[ik.0].1@.len(),
        es[ik.0].1@[ik.1].view() == x,
{
    let ev = |e: (KeyView, Vec<Row>)| entry_views(e);
    lemma_flat_contains(es.map_values(ev), x);
    let i = choose|i: int| 0 <= i < es.map_values(ev).len() && (#[trigger] es.map_values(ev)[i]).contains(x);
    assert(es.map_values(ev)[i] == entry_views(es[i]));
    let k = choose|k: int| 0 <= k < entry_views(es[i]).len() && entry_views(es[i])[k] == x;
    (i, k)
}

pub proof fn lemma_collected(db: Db, c: Cells, fy: int, uom: Seq<u8>, coll: Seq<Row>, sorted: Seq<usize>)
    requires
        cells_rep(c, cw(db)),
        finite_num(db),
        coll_ok(coll, class_entries(c, fy, uom)),
        forall|k: int| 0 <= k < sorted.len() ==> (#[trigger] sorted[k]) < coll.len(),
        nondesc(crate::sortout::sorted_views(coll, sorted), ext_order()),
        crate::sortout::sorted_views(coll, sorted).to_multiset() =~= views(coll).to_multiset(),
    ensures
        query_result(db, Params { p1: fy, p2: uom }, limit_rows(crate::sortout::sorted_views(coll, sorted), 100)),
{
    broadcast use vstd::seq_lib::group_to_multiset_ensures;
    let es = class_entries(c, fy, uom);
    let m = choose|m: int| {
        &&& 0 <= m <= es.len()
        &&& views(coll) == flat_rows(es.take(m))
        &&& (coll.len() >= 100 || m == es.len())
        &&& (m == es.len() || (m > 0 && es[m].0.0 > es[m - 1].0.0))
    };
    let p = Params { p1: fy, p2: uom };
    let sc = crate::sortout::sorted_views(coll, sorted);
    let cr = flat_rows(es.take(m));
    let rr = flat_rows(es.skip(m));
    lemma_rows_multiset(db, c, fy, uom);
    lemma_flat_rows_split(es, m);
    lemma_to_multiset_concat(cr, rr);
    if c.at((fy, uom)) is Some { c.at((fy, uom))->0.model(); }
    assert forall|x: OutRow, y: OutRow| cr.contains(x) && rr.contains(y) implies x.value is Some && y.value is Some && y.value->0 < x.value->0 by {
        let (i, k) = lemma_flat_rows_member(es.take(m), x);
        let (j, l) = lemma_flat_rows_member(es.skip(m), y);
        assert(es.take(m)[i] == es[i]);
        assert(es.skip(m)[j] == es[m + j]);
        lemma_entry_rows(db, c, fy, uom, i);
        lemma_entry_rows(db, c, fy, uom, m + j);
        let rx = es[i].1@[k];
        let ry = es[m + j].1@[l];
        assert(f64_desc_key(rx.val) == es[i].0.0);
        assert(f64_desc_key(ry.val) == es[m + j].0.0);
        assert(m < es.len() && m > 0);
        // keys are ordered: first components do not decrease
        assert(es[i].0.0 <= es[m - 1].0.0) by {
            if i < m - 1 { assert(key_lt(es[i].0, es[m - 1].0)); }
        }
        assert(es[m].0.0 <= es[m + j].0.0) by {
            if j > 0 { assert(key_lt(es[m].0, es[m + j].0)); }
        }
        lemma_desc_key(ry.val, rx.val);
    }
    assert(sc.len() == coll.len()) by {
        assert(sc.to_multiset().len() == sc.len());
        assert(views(coll).to_multiset().len() == views(coll).len());
    }
    if m == es.len() {
        assert(es.skip(m) =~= Seq::<(KeyView, Vec<Row>)>::empty());
        let ev = |e: (KeyView, Vec<Row>)| entry_views(e);
        assert(es.skip(m).map_values(ev) =~= Seq::<Seq<OutRow>>::empty());
        assert(rr.len() == 0);
    }
    lemma_finish(db, p, cr, sc, rr);
}

} // verus!
