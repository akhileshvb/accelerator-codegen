// A class below its threshold keeps at least a hundred cells, so a read never reaches a filing's first cell beyond it.
use vstd::prelude::*;
use verus_common::sums::*;
use verus_prooflib::ordered::{Index, Key, KeyView, key_lt, ordered_rows, lower_position, lemma_lower_suffix, lemma_key_order};
use crate::gen_spec::*;
use crate::model::*;
use crate::cells::*;
use crate::classes::*;
use crate::thr::*;

verus! {

/// The query's LIMIT.
pub open spec fn lim() -> int { 100 }

/// ks lists a hundred distinct cells of the class, all within dmax, all stored.
pub open spec fn witnessed(cells: Cells, fy: int, uom: Seq<u8>, dmax: int, ks: Seq<KeyView>) -> bool {
    &&& ks.len() >= lim()
    &&& ks.no_duplicates()
    &&& forall|i: int| 0 <= i < ks.len() ==> (#[trigger] ks[i]).0 <= dmax && cell_at(cells, CellId { fy, uom, d: ks[i].0, a: ks[i].1 }) is Some
}

/// Every class with a threshold stores a hundred cells within it.
pub open spec fn thr_ok(cells: Cells, t: Thr) -> bool {
    forall|fy: int, uom: Seq<u8>| #[trigger] thr_of(t, fy, uom) == D_INF as int
        || exists|ks: Seq<KeyView>| witnessed(cells, fy, uom, thr_of(t, fy, uom), ks)
}

/// The key below which the cells of value keys at most d lie.
pub open spec fn bound_of(d: int) -> KeyView { (d + 1, Seq::<u8>::empty()) }

/// Number of entries with value key at most d.
pub open spec fn lead(es: Seq<(KeyView, Vec<Row>)>, d: int) -> int {
    lower_position(es, bound_of(d))
}

pub proof fn lemma_lead(es: Seq<(KeyView, Vec<Row>)>, d: int)
    requires
        ordered_rows(es),
    ensures
        0 <= lead(es, d) <= es.len(),
        forall|i: int| 0 <= i < lead(es, d) ==> (#[trigger] es[i]).0.0 <= d,
        forall|i: int| lead(es, d) <= i < es.len() ==> (#[trigger] es[i]).0.0 > d,
{
    lemma_lower_suffix(es, bound_of(d));
    assert forall|i: int| 0 <= i < lead(es, d) implies (#[trigger] es[i]).0.0 <= d by {
        assert(key_lt(es[i].0, bound_of(d)));
    }
    assert forall|i: int| lead(es, d) <= i < es.len() implies (#[trigger] es[i]).0.0 > d by {
        assert(!key_lt(es[i].0, bound_of(d)));
    }
}

/// A witness lists no more cells than the class has within its threshold.
pub proof fn lemma_witness_lead(cells: Cells, fy: int, uom: Seq<u8>, dmax: int, ks: Seq<KeyView>)
    requires
        cells.wf(),
        forall|k: KeyView| #[trigger] cells.at(k) is Some ==> cells.at(k)->0.wf(),
        witnessed(cells, fy, uom, dmax, ks),
    ensures
        lead(class_entries(cells, fy, uom), dmax) >= ks.len(),
{
    let es = class_entries(cells, fy, uom);
    if ks.len() > 0 {
        assert(cell_at(cells, CellId { fy, uom, d: ks[0].0, a: ks[0].1 }) is Some);
        assert(cells.at((fy, uom)) is Some);
    }
    if cells.at((fy, uom)) is Some {
        let inner = cells.at((fy, uom))->0;
        inner.map_model();
        lemma_lead(es, dmax);
        let lp = lead(es, dmax);
        let pre = es.take(lp).map_values(|e: (KeyView, Vec<Row>)| e.0);
        assert forall|k: KeyView| ks.contains(k) implies pre.to_set().contains(k) by {
            let i = choose|i: int| 0 <= i < ks.len() && ks[i] == k;
            let id = CellId { fy, uom, d: k.0, a: k.1 };
            assert(cell_at(cells, id) is Some);
            assert(inner.at(k) is Some);
            let row = (k, inner.at(k)->0);
            assert(es.contains(row));
            let p = choose|p: int| 0 <= p < es.len() && es[p] == row;
            if p >= lp {
                assert(es[p].0.0 > dmax);
            }
            assert(es.take(lp)[p] == row);
            assert(pre[p] == k);
            pre.to_set_ensures();
        }
        ks.unique_seq_to_set();
        assert(ks.to_set().subset_of(pre.to_set())) by {
            ks.to_set_ensures();
        }
        vstd::set_lib::lemma_len_subset(ks.to_set(), pre.to_set());
        pre.lemma_cardinality_of_set();
        assert(pre.len() == lp);
    } else {
        assert(es =~= Seq::<(KeyView, Vec<Row>)>::empty());
    }
}

} // verus!
