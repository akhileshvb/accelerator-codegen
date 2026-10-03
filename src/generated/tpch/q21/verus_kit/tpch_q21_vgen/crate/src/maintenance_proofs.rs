use vstd::prelude::*;
use verus_common::sums::*;
use crate::aggregates::*;
use crate::gen_obligations::*;
use crate::gen_spec::*;
use crate::index::*;
use crate::maintenance::*;

verus! {

pub open spec fn supplier_delta_for(changes: Slices, db: Db, db2: Db) -> bool {
    forall|outer: KeyView, inner: KeyView| #![trigger nested_count(changes, outer, inner)] nested_count(changes, outer, inner) == if outer.1 == Seq::<u8>::empty() {
        supplier_multiplicity(db2, outer.0, inner.0, inner.1) - supplier_multiplicity(db, outer.0, inner.0, inner.1)
    } else { 0 }
}

pub open spec fn nation_delta_for(changes: Slices, db: Db, db2: Db) -> bool {
    forall|outer: KeyView, inner: KeyView| #![trigger nested_count(changes, outer, inner)] nested_count(changes, outer, inner) == if outer.0 == 0 && inner.1 == Seq::<u8>::empty() {
        nation_multiplicity(db2, inner.0, outer.1) - nation_multiplicity(db, inner.0, outer.1)
    } else { 0 }
}

pub proof fn lemma_supplier_delta_step(db: Db, db2: Db, deltas: Seq<SupplierDelta>, outer: KeyView, inner: KeyView)
    requires bag_step(db.supplier, db2.supplier, supplier_delta_seq_view(deltas)),
    ensures sum(deltas, supplier_delta_weight(outer, inner)) == if outer.1 == Seq::<u8>::empty() {
        supplier_multiplicity(db2, outer.0, inner.0, inner.1) - supplier_multiplicity(db, outer.0, inner.0, inner.1)
    } else { 0 },
{
    let weight = |row: SupplierRow| if outer == (row.s_suppkey, Seq::<u8>::empty()) && inner == (row.s_nationkey, row.s_name) { 1int } else { 0int };
    let signed = |delta: (SupplierRow, int)| delta.1 * weight(delta.0);
    assert forall|delta: SupplierDelta| #[trigger] supplier_delta_weight(outer, inner)(delta) == signed((supplier_input_view(delta.row), delta.sign as int)) by {
        if outer == (delta.row.s_suppkey as int, Seq::<u8>::empty()) && inner == (delta.row.s_nationkey as int, delta.row.s_name@) {
            assert(weight(supplier_input_view(delta.row)) == 1);
        } else { assert(weight(supplier_input_view(delta.row)) == 0); }
    }
    lemma_sum_map(deltas, |delta: SupplierDelta| (supplier_input_view(delta.row), delta.sign as int), signed, supplier_delta_weight(outer, inner));
    assert(sum(db2.supplier, weight) == sum(db.supplier, weight) + sum(supplier_delta_seq_view(deltas), signed));
    if outer.1 == Seq::<u8>::empty() {
        let full = |row: SupplierRow| if row.s_suppkey == outer.0 && row.s_nationkey == inner.0 && row.s_name == inner.1 { 1int } else { 0int };
        lemma_sum_ext(db.supplier, weight, full);
        lemma_sum_ext(db2.supplier, weight, full);
    } else {
        lemma_sum_zero(db.supplier, weight);
        lemma_sum_zero(db2.supplier, weight);
    }
}

pub proof fn lemma_nation_delta_step(db: Db, db2: Db, deltas: Seq<NationDelta>, outer: KeyView, inner: KeyView)
    requires bag_step(db.nation, db2.nation, nation_delta_seq_view(deltas)),
    ensures sum(deltas, nation_delta_weight(outer, inner)) == if outer.0 == 0 && inner.1 == Seq::<u8>::empty() {
        nation_multiplicity(db2, inner.0, outer.1) - nation_multiplicity(db, inner.0, outer.1)
    } else { 0 },
{
    let weight = |row: NationRow| if outer == (0int, row.n_name) && inner == (row.n_nationkey, Seq::<u8>::empty()) { 1int } else { 0int };
    let signed = |delta: (NationRow, int)| delta.1 * weight(delta.0);
    assert forall|delta: NationDelta| #[trigger] nation_delta_weight(outer, inner)(delta) == signed((nation_input_view(delta.row), delta.sign as int)) by {
        if outer == (0int, delta.row.n_name@) && inner == (delta.row.n_nationkey as int, Seq::<u8>::empty()) {
            assert(weight(nation_input_view(delta.row)) == 1);
        } else { assert(weight(nation_input_view(delta.row)) == 0); }
    }
    lemma_sum_map(deltas, |delta: NationDelta| (nation_input_view(delta.row), delta.sign as int), signed, nation_delta_weight(outer, inner));
    assert(sum(db2.nation, weight) == sum(db.nation, weight) + sum(nation_delta_seq_view(deltas), signed));
    if outer.0 == 0 && inner.1 == Seq::<u8>::empty() {
        let full = |row: NationRow| if row.n_nationkey == inner.0 && row.n_name == outer.1 { 1int } else { 0int };
        lemma_sum_ext(db.nation, weight, full);
        lemma_sum_ext(db2.nation, weight, full);
    } else {
        lemma_sum_zero(db.nation, weight);
        lemma_sum_zero(db2.nation, weight);
    }
}

pub proof fn lemma_name_delta_step(db: Db, db2: Db, suppliers: Seq<SupplierDelta>, nations: Seq<NationDelta>, key: KeyView)
    requires
        bag_step(db.supplier, db2.supplier, supplier_delta_seq_view(suppliers)),
        bag_step(db.nation, db2.nation, nation_delta_seq_view(nations)),
    ensures sum(suppliers, supplier_name_delta_weight(key)) + sum(nations, nation_name_delta_weight(key)) == if key.0 == 0 {
        name_references(db2, key.1) - name_references(db, key.1)
    } else { 0 },
{
    let supplier_weight = |row: SupplierRow| if key == (0int, row.s_name) { 1int } else { 0int };
    let nation_weight = |row: NationRow| if key == (0int, row.n_name) { 1int } else { 0int };
    let supplier_signed = |delta: (SupplierRow, int)| delta.1 * supplier_weight(delta.0);
    let nation_signed = |delta: (NationRow, int)| delta.1 * nation_weight(delta.0);
    lemma_sum_map(suppliers, |delta: SupplierDelta| (supplier_input_view(delta.row), delta.sign as int), supplier_signed, supplier_name_delta_weight(key));
    lemma_sum_map(nations, |delta: NationDelta| (nation_input_view(delta.row), delta.sign as int), nation_signed, nation_name_delta_weight(key));
    assert(sum(db2.supplier, supplier_weight) == sum(db.supplier, supplier_weight) + sum(supplier_delta_seq_view(suppliers), supplier_signed));
    assert(sum(db2.nation, nation_weight) == sum(db.nation, nation_weight) + sum(nation_delta_seq_view(nations), nation_signed));
    if key.0 == 0 {
        let supplier_full = |row: SupplierRow| if row.s_name == key.1 { 1int } else { 0int };
        let nation_full = |row: NationRow| if row.n_name == key.1 { 1int } else { 0int };
        lemma_sum_ext(db.supplier, supplier_weight, supplier_full);
        lemma_sum_ext(db2.supplier, supplier_weight, supplier_full);
        lemma_sum_ext(db.nation, nation_weight, nation_full);
        lemma_sum_ext(db2.nation, nation_weight, nation_full);
    } else {
        lemma_sum_zero(db.supplier, supplier_weight);
        lemma_sum_zero(db2.supplier, supplier_weight);
        lemma_sum_zero(db.nation, nation_weight);
        lemma_sum_zero(db2.nation, nation_weight);
    }
}

}
