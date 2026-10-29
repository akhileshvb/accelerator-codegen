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
use crate::thr::*;
use verus_common::rel::*;

verus! {

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
pub proof fn lemma_entry_rows(db: Db, c: Cells, fy: int, uom: Seq<u8>, dmax: int, i: int)
    requires
        class_good(c, fy, uom, db, dmax),
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
    assert(id.fy == fy && id.uom == uom);
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


/// What the class walk delivers: a prefix of the entries within the threshold, a hundred rows or all of an unbounded class, ending at a value boundary.
pub open spec fn coll_ok(coll: Seq<Row>, es: Seq<(KeyView, Vec<Row>)>, dmax: int) -> bool {
    &&& crate::sortout::rows_finite(coll)
    &&& exists|m: int| {
        &&& 0 <= m <= es.len()
        &&& views(coll) == flat_rows(es.take(m))
        &&& (coll.len() >= 100 || (m == es.len() && dmax == D_INF as int))
        &&& (m == es.len() || (m > 0 && es[m].0.0 > es[m - 1].0.0))
        &&& (m == 0 || es[m - 1].0.0 <= dmax)
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

/// Every answer row has the value of a finite double.
pub proof fn lemma_rows_good(db: Db, p: Params)
    requires
        finite_num(db),
    ensures
        forall|y: OutRow| rows(db, p).contains(y) ==> y.value is Some && exists|d: int| vkey(y.value->0, d),
{
    let bad = |y: OutRow| if y.value is Some && exists|d: int| vkey(y.value->0, d) { 0int } else { 1int };
    lemma_rows_sum(db, p, bad);
    let t = |n: NumRow| if picked(db.num, n, p.p2) { pair_w(db.sub, n, p.p1, bad) } else { 0 };
    assert forall|n: NumRow| db.num.contains(n) implies #[trigger] t(n) == 0 by {
        if picked(db.num, n, p.p2) {
            let b = choose|b: u64| f64_finite(b) && f64_val(b) == n.value->0;
            assert(vkey(n.value->0, f64_desc_key(b)));
            let z = |s: SubRow| if sub_hits(n, s, p.p1) { bad(ent_row(n, s)) } else { 0 };
            assert forall|k: int| 0 <= k < db.sub.len() implies z(#[trigger] db.sub[k]) == 0 by {}
            lemma_sum_zero(db.sub, z);
        }
    }
    assert forall|i: int| 0 <= i < db.num.len() implies t(#[trigger] db.num[i]) == 0 by { assert(db.num.contains(db.num[i])); }
    lemma_sum_zero(db.num, t);
    assert(class_w(db, p.p1, p.p2, bad) == 0);
    assert(sum(rows(db, p), bad) == 0);
    let rs = rows(db, p);
    assert forall|k: int| 0 <= k < rs.len() implies bad(#[trigger] rs[k]) >= 0 by {}
    assert forall|y: OutRow| rs.contains(y) implies y.value is Some && exists|d: int| vkey(y.value->0, d) by {
        let k = choose|k: int| 0 <= k < rs.len() && rs[k] == y;
        if !(y.value is Some && exists|d: int| vkey(y.value->0, d)) {
            lemma_sum_pos(rs, bad, k);
        }
    }
}

/// Counting x in a sequence.
pub proof fn lemma_count_sum(s: Seq<OutRow>, x: OutRow)
    ensures
        s.to_multiset().count(x) == sum(s, eqv(x)),
{
    lemma_count_as_sum(s, x);
}

pub proof fn lemma_collected(db: Db, c: Cells, fy: int, uom: Seq<u8>, dmax: int, coll: Seq<Row>, sorted: Seq<usize>)
    requires
        class_good(c, fy, uom, db, dmax),
        finite_num(db),
        coll_ok(coll, class_entries(c, fy, uom), dmax),
        forall|k: int| 0 <= k < sorted.len() ==> (#[trigger] sorted[k]) < coll.len(),
        nondesc(crate::sortout::sorted_views(coll, sorted), ext_order()),
        crate::sortout::sorted_views(coll, sorted).to_multiset() =~= views(coll).to_multiset(),
    ensures
        query_result(db, Params { p1: fy, p2: uom }, limit_rows(crate::sortout::sorted_views(coll, sorted), 100)),
{
    broadcast use vstd::seq_lib::group_to_multiset_ensures;
    broadcast use vstd::multiset::group_multiset_axioms;
    let es = class_entries(c, fy, uom);
    let m = choose|m: int| {
        &&& 0 <= m <= es.len()
        &&& views(coll) == flat_rows(es.take(m))
        &&& (coll.len() >= 100 || (m == es.len() && dmax == D_INF as int))
        &&& (m == es.len() || (m > 0 && es[m].0.0 > es[m - 1].0.0))
        &&& (m == 0 || es[m - 1].0.0 <= dmax)
    };
    let p = Params { p1: fy, p2: uom };
    let sc = crate::sortout::sorted_views(coll, sorted);
    let cr = flat_rows(es.take(m));
    let rest = flat_rows(es.skip(m));
    let bnd: int = if m > 0 { es[m - 1].0.0 } else { dmax };
    assert(bnd <= dmax);
    if c.at((fy, uom)) is Some { c.at((fy, uom))->0.model(); }
    lemma_flat_rows_split(es, m);
    lemma_rows_good(db, p);
    let rs = rows(db, p);
    let le_b = |y: OutRow| y.value is Some && dk_le(y.value->0, bnd);
    let keepf = |y: OutRow| !le_b(y);
    let r_rows = rs.filter(keepf);
    // rows of the walked entries have value keys at most bnd, those of the rest more
    assert forall|x: OutRow| cr.contains(x) implies x.value is Some && exists|d: int| vkey(x.value->0, d) && d <= bnd by {
        let (i, k) = lemma_flat_rows_member(es.take(m), x);
        assert(es.take(m)[i] == es[i]);
        lemma_entry_rows(db, c, fy, uom, dmax, i);
        let rx = es[i].1@[k];
        assert(x == rx.view());
        assert(f64_finite(rx.val) && f64_desc_key(rx.val) == es[i].0.0);
        assert(vkey(f64_val(rx.val), es[i].0.0));
        assert(es[i].0.0 <= bnd) by {
            if i < m - 1 { assert(key_lt(es[i].0, es[m - 1].0)); }
        }
        assert(x == rx.view());
        let dd = es[i].0.0;
        assert(vkey(x.value->0, dd) && dd <= bnd);
    }
    assert forall|y: OutRow| rest.contains(y) implies y.value is Some && exists|d: int| vkey(y.value->0, d) && d > bnd by {
        let (j, l) = lemma_flat_rows_member(es.skip(m), y);
        assert(es.skip(m)[j] == es[m + j]);
        lemma_entry_rows(db, c, fy, uom, dmax, m + j);
        let ry = es[m + j].1@[l];
        assert(y == ry.view());
        assert(f64_finite(ry.val) && f64_desc_key(ry.val) == es[m + j].0.0);
        assert(vkey(f64_val(ry.val), es[m + j].0.0));
        assert(m < es.len() && m > 0);
        assert(es[m].0.0 <= es[m + j].0.0) by {
            if j > 0 { assert(key_lt(es[m].0, es[m + j].0)); }
        }
        assert(y == ry.view());
        let dd = es[m + j].0.0;
        assert(vkey(y.value->0, dd) && dd > bnd);
    }
    // counts
    assert forall|x: OutRow| #[trigger] rs.to_multiset().count(x) == cr.to_multiset().add(r_rows.to_multiset()).count(x) by {
        lemma_count_sum(rs, x);
        lemma_count_sum(cr, x);
        lemma_count_sum(r_rows, x);
        let ex = eqv(x);
        let gg = |y: OutRow| if !le_b(y) { ex(y) } else { 0 };
        lemma_sum_filter(rs, keepf, ex, gg);
        let target = |y: OutRow| if le_b(x) { 0int } else { ex(y) };
        assert forall|y: OutRow| #[trigger] gg(y) == target(y) by { if ex(y) != 0 { assert(y == x); } }
        lemma_sum_ext_all(rs, gg, target);
        if le_b(x) {
            lemma_sum_zero(rs, target);
            assert(nonneg(ex)) by { assert forall|y: OutRow| #[trigger] ex(y) >= 0 by {} }
            assert(supp_le(ex, dmax)) by {
                assert forall|y: OutRow| #[trigger] ex(y) != 0 implies y.value is Some && dk_le(y.value->0, dmax) by {
                    assert(y == x);
                    let d = choose|d: int| vkey(x.value->0, d) && d <= bnd;
                }
            }
            lemma_rows_sum(db, p, ex);
            lemma_class_sum(db, c, fy, uom, dmax, ex);
            assert(flat_rows(es) == cr + rest);
            lemma_sum_concat(cr, rest, ex);
            assert forall|k: int| 0 <= k < rest.len() implies ex(#[trigger] rest[k]) == 0 by {
                assert(rest.contains(rest[k]));
                if rest[k] == x {
                    let d1 = choose|d: int| vkey(x.value->0, d) && d <= bnd;
                    let d2 = choose|d: int| vkey(x.value->0, d) && d > bnd;
                    lemma_vkey_unique(x.value->0, d1, d2);
                }
            }
            lemma_sum_zero(rest, ex);
        } else {
            lemma_sum_ext_all(rs, gg, ex);
            assert forall|k: int| 0 <= k < cr.len() implies ex(#[trigger] cr[k]) == 0 by {
                assert(cr.contains(cr[k]));
                if cr[k] == x {
                    let d = choose|d: int| vkey(x.value->0, d) && d <= bnd;
                    assert(le_b(x));
                }
            }
            lemma_sum_zero(cr, ex);
        }
    }
    assert(rs.to_multiset() =~= cr.to_multiset().add(r_rows.to_multiset()));
    // order between the walked rows and the rest
    assert forall|x: OutRow, y: OutRow| cr.contains(x) && r_rows.contains(y) implies x.value is Some && y.value is Some && y.value->0 < x.value->0 by {
        rs.lemma_filter_contains_rev(keepf, y);
        assert(rs.contains(y));
        let dx = choose|d: int| vkey(x.value->0, d) && d <= bnd;
        let dy = choose|d: int| vkey(y.value->0, d);
        assert(!le_b(y));
        assert(dy > bnd);
        let bx = choose|b: u64| f64_finite(b) && f64_val(b) == x.value->0 && f64_desc_key(b) == dx;
        let by_ = choose|b: u64| f64_finite(b) && f64_val(b) == y.value->0 && f64_desc_key(b) == dy;
        lemma_desc_key(by_, bx);
    }
    assert(sc.len() == coll.len()) by {
        assert(sc.to_multiset().len() == sc.len());
        assert(views(coll).to_multiset().len() == views(coll).len());
    }
    assert(sc.to_multiset() =~= cr.to_multiset());
    if coll.len() < 100 {
        assert(m == es.len() && dmax == D_INF as int);
        assert(es.skip(m) =~= Seq::<(KeyView, Vec<Row>)>::empty());
        let ev = |e: (KeyView, Vec<Row>)| entry_views(e);
        assert(es.skip(m).map_values(ev) =~= Seq::<Seq<OutRow>>::empty());
        assert(rest.len() == 0);
        if r_rows.len() > 0 {
            let y = r_rows[0];
            assert(r_rows.contains(y));
            rs.lemma_filter_contains_rev(keepf, y);
            assert(rs.contains(y));
            let k = choose|k: int| 0 <= k < rs.len() && rs[k] == y;
            let ey = eqv(y);
            assert forall|z: int| 0 <= z < rs.len() implies ey(#[trigger] rs[z]) >= 0 by {}
            lemma_sum_pos(rs, ey, k);
            let dy = choose|d: int| vkey(y.value->0, d);
            let by_ = choose|b: u64| f64_val(b) == y.value->0 && f64_desc_key(b) == dy && f64_finite(b);
            lemma_key_range(by_);
            assert(nonneg(ey)) by { assert forall|z: OutRow| #[trigger] ey(z) >= 0 by {} }
            assert(supp_le(ey, dmax)) by {
                assert forall|z: OutRow| #[trigger] ey(z) != 0 implies z.value is Some && dk_le(z.value->0, dmax) by { assert(z == y); }
            }
            lemma_rows_sum(db, p, ey);
            lemma_class_sum(db, c, fy, uom, dmax, ey);
            assert(flat_rows(es) == cr + rest);
            assert(flat_rows(es) =~= cr);
            assert forall|z: int| 0 <= z < cr.len() implies ey(#[trigger] cr[z]) == 0 by {
                assert(cr.contains(cr[z]));
                if cr[z] == y {
                    let d = choose|d: int| vkey(y.value->0, d) && d <= bnd;
                    assert(le_b(y));
                    assert(!le_b(y));
                }
            }
            lemma_sum_zero(cr, ey);
            assert(false);
        }
    }
    lemma_finish(db, p, cr, sc, r_rows);
}

} // verus!
