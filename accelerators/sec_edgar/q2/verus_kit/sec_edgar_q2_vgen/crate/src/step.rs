// What an apply keeps of the stored prefix: the cells of unchanged filings that stay within the new threshold, and the filings no one asked for.
use vstd::prelude::*;
use verus_common::sums::*;
use verus_common::rel::*;
use verus_prooflib::ordered::{Index, Key, KeyView, key_lt, ordered_rows, lemma_key_order};
use crate::gen_spec::*;
use crate::model::*;
use crate::cells::*;
use crate::classes::*;
use crate::maint::*;
use crate::f64bits::*;
use crate::agree::*;
use crate::thr::*;
use crate::cnt::*;
use crate::plan::*;

verus! {

pub proof fn lemma_sfam_zero(w: Fam, th: Th, id: CellId)
    requires
        zero_fam(w, id),
    ensures
        zero_fam(sfam(w, th), id),
{
    assert forall|g: spec_fn(OutRow) -> int| #[trigger] fam_w(sfam(w, th), id, g) == 0 by {
        assert(fam_w(w, id, g) == 0);
    }
}

/// Two thresholds of a filing, the first below the second, store the same when the family has no cell between them.
pub proof fn lemma_keep_eq(w: Fam, th1: Th, th2: Th, id: CellId)
    requires
        th_id(th1, id) <= th_id(th2, id),
        forall|d: int| th_id(th1, id) < d <= th_id(th2, id) ==> zero_fam(w, #[trigger] at_d(id, d)),
    ensures
        forall|g: spec_fn(OutRow) -> int| #[trigger] fam_w(sfam(w, th1), id, g) == fam_w(sfam(w, th2), id, g),
{
    let (d1, d2) = (th_id(th1, id), th_id(th2, id));
    if id.d > d1 && id.d <= d2 {
        assert(zero_fam(w, at_d(id, id.d)));
        assert(at_d(id, id.d) == id);
    }
    if id.d > d2 {
        assert(isb(w, th1, id) == isb(w, th2, id)) by {
            if isb(w, th1, id) {
                assert forall|d: int| d2 < d < id.d implies zero_fam(w, #[trigger] at_d(id, d)) by {}
            }
            if isb(w, th2, id) {
                assert forall|d: int| d1 < d < id.d implies zero_fam(w, #[trigger] at_d(id, d)) by {
                    if d <= d2 {} else {}
                }
            }
        }
    }
    assert forall|g: spec_fn(OutRow) -> int| #[trigger] fam_w(sfam(w, th1), id, g) == fam_w(sfam(w, th2), id, g) by {
        if id.d > d1 && id.d <= d2 { assert(fam_w(w, id, g) == 0); }
    }
}

/// A nonzero cell beyond d1 and up to d2 gives a first one.
pub proof fn lemma_first_nonzero(w: Fam, id: CellId, d1: int, d2: int) -> (m: int)
    requires
        d1 < d2,
        !zero_fam(w, at_d(id, d2)),
    ensures
        d1 < m <= d2,
        !zero_fam(w, at_d(id, m)),
        forall|d: int| d1 < d < m ==> zero_fam(w, #[trigger] at_d(id, d)),
    decreases d2 - d1,
{
    if forall|d: int| d1 < d < d2 ==> zero_fam(w, #[trigger] at_d(id, d)) {
        d2
    } else {
        let d3 = choose|d: int| d1 < d < d2 && !zero_fam(w, at_d(id, d));
        lemma_first_nonzero(w, id, d1, d3)
    }
}

/// A stored cell is not empty.
pub proof fn lemma_stored_nonzero(c: Cells, w: Fam, id: CellId)
    requires
        cells_rep(c, w),
        cell_at(c, id) is Some,
    ensures
        !zero_fam(w, id),
{
    let v = cell_at(c, id)->0;
    assert(cell_ok(cell_at(c, id), w, id));
    let one = |o: OutRow| 1int;
    let vs = views(v@);
    assert forall|k: int| 0 <= k < vs.len() implies one(#[trigger] vs[k]) >= 0 by {}
    assert(vs.len() > 0);
    lemma_sum_pos(vs, one, 0);
    assert(sum(vs, one) == fam_w(w, id, one));
}

/// A nonzero family weight is stored.
pub proof fn lemma_nonzero_stored(c: Cells, w: Fam, id: CellId)
    requires
        cells_rep(c, w),
        !zero_fam(w, id),
    ensures
        cell_at(c, id) is Some,
{
    assert(cell_ok(cell_at(c, id), w, id));
}

/// A cell of a filing no one touched stays stored if its value key is within the new threshold.
pub proof fn lemma_stay(c1: Cells, c2: Cells, db: Db, db2: Db, th1: Th, th2: Th, id: CellId)
    requires
        cells_rep(c1, sfam(cw(db), th1)),
        cells_rep(c2, sfam(cw(db2), th2)),
        cell_at(c1, id) is Some,
        agree(db, db2, id.a),
        id.d <= th_id(th2, id),
    ensures
        cell_at(c2, id) is Some,
{
    lemma_stored_nonzero(c1, sfam(cw(db), th1), id);
    let g = choose|g: spec_fn(OutRow) -> int| fam_w(sfam(cw(db), th1), id, g) != 0;
    assert(keep(cw(db), th1, id));
    assert(cell_w(db, id, g) != 0);
    lemma_cell_w_agree(db, db2, id.a, id, g);
    assert(fam_w(sfam(cw(db2), th2), id, g) == cell_w(db2, id, g));
    assert(!zero_fam(sfam(cw(db2), th2), id));
    lemma_nonzero_stored(c2, sfam(cw(db2), th2), id);
}

pub proof fn lemma_push_ordered(f: Seq<(KeyView, Vec<Row>)>, x: (KeyView, Vec<Row>))
    requires
        ordered_rows(f),
        forall|i: int| 0 <= i < f.len() ==> key_lt((#[trigger] f[i]).0, x.0),
    ensures
        ordered_rows(f.push(x)),
{
    let r = f.push(x);
    assert forall|i: int, j: int| 0 <= i < j < r.len() implies key_lt(r[i].0, r[j].0) by {
        if j < f.len() {
            assert(r[i] == f[i] && r[j] == f[j]);
        } else {
            assert(r[j] == x);
            assert(r[i] == f[i]);
        }
    }
}

pub proof fn lemma_filter_ordered(es: Seq<(KeyView, Vec<Row>)>, p: spec_fn((KeyView, Vec<Row>)) -> bool)
    requires
        ordered_rows(es),
    ensures
        ordered_rows(es.filter(p)),
    decreases es.len(),
{
    reveal_with_fuel(Seq::filter, 2);
    if es.len() > 0 {
        let d = es.drop_last();
        assert(ordered_rows(d)) by {
            assert forall|i: int, j: int| 0 <= i < j < d.len() implies key_lt(d[i].0, d[j].0) by { assert(es[i] == d[i] && es[j] == d[j]); }
        }
        lemma_filter_ordered(d, p);
        if p(es.last()) {
            let f = d.filter(p);
            assert forall|i: int| 0 <= i < f.len() implies key_lt((#[trigger] f[i]).0, es.last().0) by {
                assert(f.contains(f[i]));
                d.lemma_filter_contains_rev(p, f[i]);
                let k = choose|k: int| 0 <= k < d.len() && d[k] == f[i];
                assert(es[k] == d[k]);
                assert(es[es.len() - 1] == es.last());
                assert(key_lt(es[k].0, es[es.len() - 1].0));
            }
            lemma_push_ordered(f, es.last());
        }
    }
}

/// An ordered list's value keys do not decrease.
pub proof fn lemma_tail_ordered(cells: Cells, thr: Thr, tk: Seq<Seq<u8>>, fy: int, uom: Seq<u8>)
    requires
        cells_wf(cells),
    ensures
        ordered_rows(tail_of(cells, thr, tk, fy, uom)),
        forall|p: int, q: int| 0 <= p < q < tail_of(cells, thr, tk, fy, uom).len() ==> (#[trigger] tail_of(cells, thr, tk, fy, uom)[p]).0.0 <= (#[trigger] tail_of(cells, thr, tk, fy, uom)[q]).0.0,
{
    let es = class_entries(cells, fy, uom);
    if cells.at((fy, uom)) is Some { cells.at((fy, uom))->0.model(); }
    lemma_filter_ordered(es, |e: (KeyView, Vec<Row>)| e.0.0 > thr_of(thr, fy, uom) && !tk.contains(e.0.1));
    let tl = tail_of(cells, thr, tk, fy, uom);
    assert forall|p: int, q: int| 0 <= p < q < tl.len() implies (#[trigger] tl[p]).0.0 <= (#[trigger] tl[q]).0.0 by {
        assert(key_lt(tl[p].0, tl[q].0));
    }
}

/// Nothing unchanged and unasked for has a cell between the old and new threshold of a class.
pub proof fn lemma_nohole(
    cells: Cells, dir: Index<Vec<CK>>, thr: Thr, db: Db, tk: Seq<Seq<u8>>, kv: Seq<Seq<u8>>, id: CellId, d: int,
)
    requires
        cells_wf(cells),
        keys_in_range(cells),
        cells_rep(cells, sfam(cw(db), th_of(thr))),
        is_wanted(cells, dir, thr, tk, kv),
        !kv.contains(id.a),
        thr_of(thr, id.fy, id.uom) < d <= dnew(cells, dir, thr, tk, id.fy, id.uom),
    ensures
        zero_fam(cw(db), at_d(id, d)),
{
    let (fy, uom) = (id.fy, id.uom);
    let dd = thr_of(thr, fy, uom);
    assert(cls_hit(cells, dir, thr, tk, fy, uom));
    if !zero_fam(cw(db), at_d(id, d)) {
        let m = lemma_first_nonzero(cw(db), id, dd, d);
        let id1 = at_d(id, m);
        let th = th_of(thr);
        assert(isb(cw(db), th, id1)) by {
            assert(th_id(th, id1) == dd);
            assert forall|x: int| dd < x < id1.d implies zero_fam(cw(db), #[trigger] at_d(id1, x)) by { assert(at_d(id1, x) == at_d(id, x)); }
        }
        assert(keep(cw(db), th, id1));
        assert(!zero_fam(sfam(cw(db), th), id1)) by {
            let g = choose|g: spec_fn(OutRow) -> int| fam_w(cw(db), id1, g) != 0;
            assert(fam_w(sfam(cw(db), th), id1, g) != 0);
        }
        lemma_nonzero_stored(cells, sfam(cw(db), th), id1);
        // id1 is an entry of the class beyond the threshold, of a filing not asked for: it is in the tail
        let es = class_entries(cells, fy, uom);
        assert(cells.at((fy, uom)) is Some);
        let inner = cells.at((fy, uom))->0;
        inner.map_model();
        let key: KeyView = (m, id.a);
        assert(inner.at(key) is Some);
        let row = (key, inner.at(key)->0);
        assert(es.contains(row));
        assert(!tk.contains(id.a)) by {
            if tk.contains(id.a) { assert(wanted_has(cells, dir, thr, tk, id.a)); assert(kv.contains(id.a)); }
        }
        let tail = tail_of(cells, thr, tk, fy, uom);
        let pe = |e: (KeyView, Vec<Row>)| e.0.0 > dd && !tk.contains(e.0.1);
        let p = choose|p: int| 0 <= p < es.len() && es[p] == row;
        es.lemma_filter_contains(pe, p);
        assert(tail.contains(row));
        let piv = pivot_of(tail);
        assert(m <= d && d <= piv);
        let le_p = |e: (KeyView, Vec<Row>)| e.0.0 <= pivot_of(tail);
        let sel = tail.filter(le_p);
        let tp = choose|tp: int| 0 <= tp < tail.len() && tail[tp] == row;
        tail.lemma_filter_contains(le_p, tp);
        assert(sel.contains(row));
        let units = picked_of(tail);
        let sp = choose|sp: int| 0 <= sp < sel.len() && sel[sp] == row;
        assert(units[sp] == id.a);
        assert(units.contains(id.a));
        assert(wanted_has(cells, dir, thr, tk, id.a));
        assert(kv.contains(id.a));
    }
}

/// The new threshold of a class keeps a hundred cells stored.
pub proof fn lemma_thr_ok_after(
    cells: Cells, dir: Index<Vec<CK>>, thr: Thr, db: Db, db2: Db, tk: Seq<Seq<u8>>, thr_new: Thr, cells2: Cells,
)
    requires
        cells_wf(cells),
        keys_in_range(cells),
        cells_rep(cells, sfam(cw(db), th_of(thr))),
        cover(dir, sfam(cw(db), th_of(thr))),
        thr_ok(cells, thr),
        cells_rep(cells2, sfam(cw(db2), th_of(thr_new))),
        forall|a: Seq<u8>| !tk.contains(a) ==> agree(db, db2, a),
        forall|fy: int, uom: Seq<u8>| thr_of(thr_new, fy, uom) == dnew(cells, dir, thr, tk, fy, uom),
    ensures
        thr_ok(cells2, thr_new),
{
    assert forall|fy: int, uom: Seq<u8>| #[trigger] thr_of(thr_new, fy, uom) == D_INF as int
        || exists|ks: Seq<KeyView>| witnessed(cells2, fy, uom, thr_of(thr_new, fy, uom), ks) by {
        let dn = thr_of(thr_new, fy, uom);
        if dn != D_INF as int {
            if cls_hit(cells, dir, thr, tk, fy, uom) {
                let tail = tail_of(cells, thr, tk, fy, uom);
                assert(dn == pivot_of(tail));
                assert(tail.len() > npick());
                lemma_tail_ordered(cells, thr, tk, fy, uom);
                let ks = tail.take(npick()).map_values(|e: (KeyView, Vec<Row>)| e.0);
                assert(ks.len() == npick());
                assert forall|i: int, j: int| 0 <= i < j < ks.len() implies ks[i] != ks[j] by {
                    assert(key_lt(tail[i].0, tail[j].0));
                    lemma_key_order(tail[i].0, tail[j].0);
                }
                assert(ks.no_duplicates());
                assert forall|i: int| 0 <= i < ks.len() implies (#[trigger] ks[i]).0 <= dn && cell_at(cells2, CellId { fy, uom, d: ks[i].0, a: ks[i].1 }) is Some by {
                    let e = tail[i];
                    assert(ks[i] == e.0);
                    if i < npick() - 1 { assert(tail[i].0.0 <= tail[npick() - 1].0.0); }
                    let es = class_entries(cells, fy, uom);
                    let inner = cells.at((fy, uom))->0;
                    let pe = |x: (KeyView, Vec<Row>)| x.0.0 > thr_of(thr, fy, uom) && !tk.contains(x.0.1);
                    es.lemma_filter_contains_rev(pe, e);
                    assert(es.contains(e));
                    assert(cells.at((fy, uom)) is Some);
                    inner.map_model();
                    let id = CellId { fy, uom, d: e.0.0, a: e.0.1 };
                    assert(inner.at(e.0) == Some(e.1));
                    assert(cell_at(cells, id) is Some);
                    assert(!tk.contains(id.a));
                    lemma_stay(cells, cells2, db, db2, th_of(thr), th_of(thr_new), id);
                }
                assert(witnessed(cells2, fy, uom, dn, ks));
            } else {
                assert(dn == thr_of(thr, fy, uom));
                let es = class_entries(cells, fy, uom);
                let sv = survivors_in(es, dn, tk);
                if sv.len() >= lim() {
                    if cells.at((fy, uom)) is Some { cells.at((fy, uom))->0.model(); }
                    let pe = |e: (KeyView, Vec<Row>)| e.0.0 <= dn && !tk.contains(e.0.1);
                    lemma_filter_ordered(es, pe);
                    assert(sv == es.filter(pe));
                    let ks = sv.take(lim()).map_values(|e: (KeyView, Vec<Row>)| e.0);
                    assert(ks.len() == lim());
                    assert forall|i: int, j: int| 0 <= i < j < ks.len() implies ks[i] != ks[j] by {
                        assert(key_lt(sv[i].0, sv[j].0));
                        lemma_key_order(sv[i].0, sv[j].0);
                    }
                    assert(ks.no_duplicates());
                    assert forall|i: int| 0 <= i < ks.len() implies (#[trigger] ks[i]).0 <= dn && cell_at(cells2, CellId { fy, uom, d: ks[i].0, a: ks[i].1 }) is Some by {
                        let e = sv[i];
                        assert(ks[i] == e.0);
                        es.lemma_filter_contains_rev(pe, e);
                        assert(es.contains(e));
                        assert(cells.at((fy, uom)) is Some);
                        let inner = cells.at((fy, uom))->0;
                        inner.map_model();
                        let id = CellId { fy, uom, d: e.0.0, a: e.0.1 };
                        assert(inner.at(e.0) == Some(e.1));
                        assert(cell_at(cells, id) is Some);
                        assert(!tk.contains(id.a)) by {
                            es.lemma_filter_pred(pe, i);
                        }
                        lemma_stay(cells, cells2, db, db2, th_of(thr), th_of(thr_new), id);
                    }
                    assert(witnessed(cells2, fy, uom, dn, ks));
                } else {
                    assert(!touched(dir, thr, tk, fy, uom));
                    let ks = choose|ks: Seq<KeyView>| witnessed(cells, fy, uom, dn, ks);
                    assert forall|i: int| 0 <= i < ks.len() implies cell_at(cells2, CellId { fy, uom, d: ks[i].0, a: ks[i].1 }) is Some by {
                        let id = CellId { fy, uom, d: ks[i].0, a: ks[i].1 };
                        assert(cell_at(cells, id) is Some);
                        lemma_stored_nonzero(cells, sfam(cw(db), th_of(thr)), id);
                        if tk.contains(id.a) {
                            assert(listed(dir_of(dir, id.a), id)) by {
                                assert(!zero_fam(sfam(cw(db), th_of(thr)), id));
                            }
                            let j = choose|j: int| 0 <= j < dir_of(dir, id.a).len() && ck_matches(#[trigger] dir_of(dir, id.a)[j], id);
                            let ti = choose|ti: int| 0 <= ti < tk.len() && tk[ti] == id.a;
                            assert(hit_by(dir, thr, id.a, fy, uom));
                            assert(touched(dir, thr, tk, fy, uom));
                        }
                        assert(!tk.contains(id.a));
                        lemma_stay(cells, cells2, db, db2, th_of(thr), th_of(thr_new), id);
                    }
                    assert(witnessed(cells2, fy, uom, dn, ks));
                }
            }
        }
    }
}

pub proof fn lemma_rep_good(db: Db, c: Cells, t: Thr, fy: int, uom: Seq<u8>)
    requires
        cells_rep(c, sfam(cw(db), th_of(t))),
    ensures
        class_good(c, fy, uom, db, thr_of(t, fy, uom)),
{
    assert forall|id: CellId| id.fy == fy && id.uom == uom && id.d <= thr_of(t, fy, uom) implies cell_ok(#[trigger] cell_at(c, id), cw(db), id) by {
        assert(cell_ok(cell_at(c, id), sfam(cw(db), th_of(t)), id));
        assert(keep(cw(db), th_of(t), id));
        assert forall|g: spec_fn(OutRow) -> int| #[trigger] fam_w(cw(db), id, g) == fam_w(sfam(cw(db), th_of(t)), id, g) by {}
        lemma_cell_ok_congr(cell_at(c, id), sfam(cw(db), th_of(t)), cw(db), id);
    }
    assert forall|id: CellId| id.fy == fy && id.uom == uom implies cell_keyed(#[trigger] cell_at(c, id), id.d) by {
        assert(cell_ok(cell_at(c, id), sfam(cw(db), th_of(t)), id));
    }
}

pub open spec fn entry_good(e: (KeyView, Vec<Row>)) -> bool {
    &&& e.1@.len() > 0
    &&& forall|k: int| 0 <= k < e.1@.len() ==> f64_finite((#[trigger] e.1@[k]).val) && f64_desc_key(e.1@[k].val) == e.0.0
}

/// Every stored cell is not empty, its rows have its value key, and that fits 64 bits.
pub proof fn lemma_cells_range(c: Cells, w: Fam)
    requires
        cells_rep(c, w),
    ensures
        cells_wf(c),
        keys_in_range(c),
        forall|fy: int, uom: Seq<u8>, i: int| 0 <= i < class_entries(c, fy, uom).len() ==> entry_good(#[trigger] class_entries(c, fy, uom)[i]),
{
    assert forall|fy: int, uom: Seq<u8>, i: int| 0 <= i < class_entries(c, fy, uom).len() implies entry_good(#[trigger] class_entries(c, fy, uom)[i]) by {
        let es = class_entries(c, fy, uom);
        let inner = c.at((fy, uom))->0;
        inner.model();
        let e = es[i];
        let id = CellId { fy, uom, d: e.0.0, a: e.0.1 };
        assert(inner.at(e.0) == Some(e.1));
        assert(cell_at(c, id) == Some(e.1));
        assert(cell_ok(cell_at(c, id), w, id));
    }
    assert forall|fy: int, uom: Seq<u8>, i: int| 0 <= i < class_entries(c, fy, uom).len() implies (#[trigger] class_entries(c, fy, uom)[i]).0.0 <= D_INF as int by {
        let e = class_entries(c, fy, uom)[i];
        lemma_key_range(e.1@[0].val);
    }
}

/// A class's threshold never decreases.
pub proof fn lemma_dnew_ge(cells: Cells, dir: Index<Vec<CK>>, thr: Thr, tk: Seq<Seq<u8>>, fy: int, uom: Seq<u8>)
    requires
        keys_in_range(cells),
    ensures
        thr_of(thr, fy, uom) <= dnew(cells, dir, thr, tk, fy, uom),
        dnew(cells, dir, thr, tk, fy, uom) <= D_INF as int,
{
    let tail = tail_of(cells, thr, tk, fy, uom);
    if cls_hit(cells, dir, thr, tk, fy, uom) {
        if tail.len() > npick() {
            let pe = |e: (KeyView, Vec<Row>)| e.0.0 > thr_of(thr, fy, uom) && !tk.contains(e.0.1);
            let es = class_entries(cells, fy, uom);
            es.lemma_filter_pred(pe, npick() - 1);
            assert(tail.contains(tail[npick() - 1]));
            es.lemma_filter_contains_rev(pe, tail[npick() - 1]);
            let p = choose|p: int| 0 <= p < es.len() && es[p] == tail[npick() - 1];
        }
    }
}

} // verus!
