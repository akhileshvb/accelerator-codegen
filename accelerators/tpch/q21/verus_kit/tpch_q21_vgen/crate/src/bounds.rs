use vstd::prelude::*;
use verus_common::sums::*;
use crate::aggregates::*;
use crate::gen_spec::*;
use crate::index::*;

verus! {

pub proof fn lemma_finished_nonnegative(db: Db, order: int)
    ensures finished(db, order) >= 0,
{
    lemma_sum_nonneg(db.orders, |row: OrdersRow| if row.o_orderkey == order && row.o_orderstatus == seq![b'F'] { 1int } else { 0int });
}

pub proof fn lemma_waiting_nonnegative(db: Db, supplier: int)
    ensures waiting(db, supplier) >= 0,
{
    let term = |row: LineitemRow| if row.l_suppkey == supplier
        && row.l_receiptdate > row.l_commitdate && exists_1(db, row) && !exists_2(db, row) {
        finished(db, row.l_orderkey)
    } else { 0 };
    assert forall|index: int| 0 <= index < db.lineitem.len() implies term(#[trigger] db.lineitem[index]) >= 0 by {
        lemma_finished_nonnegative(db, db.lineitem[index].l_orderkey);
    }
    lemma_sum_nonneg(db.lineitem, term);
}

pub proof fn lemma_bucket_nonnegative(db: Db, nation: int, name: Seq<u8>)
    ensures bucket(db, nation, name) >= 0,
{
    let term = |row: SupplierRow| if row.s_nationkey == nation && row.s_name == name { waiting(db, row.s_suppkey) } else { 0 };
    assert forall|index: int| 0 <= index < db.supplier.len() implies term(#[trigger] db.supplier[index]) >= 0 by {
        lemma_waiting_nonnegative(db, db.supplier[index].s_suppkey);
    }
    lemma_sum_nonneg(db.supplier, term);
}

pub proof fn lemma_supplier_multiplicity_nonnegative(db: Db, supplier: int, nation: int, name: Seq<u8>)
    ensures supplier_multiplicity(db, supplier, nation, name) >= 0,
{
    lemma_sum_nonneg(db.supplier, |row: SupplierRow| if row.s_suppkey == supplier && row.s_nationkey == nation && row.s_name == name { 1int } else { 0int });
}

pub proof fn lemma_nation_multiplicity_nonnegative(db: Db, nation: int, name: Seq<u8>)
    ensures nation_multiplicity(db, nation, name) >= 0,
{
    lemma_sum_nonneg(db.nation, |row: NationRow| if row.n_nationkey == nation && row.n_name == name { 1int } else { 0int });
}

pub proof fn lemma_name_references_nonnegative(db: Db, name: Seq<u8>)
    ensures name_references(db, name) >= 0,
{
    lemma_sum_nonneg(db.supplier, |row: SupplierRow| if row.s_name == name { 1int } else { 0int });
    lemma_sum_nonneg(db.nation, |row: NationRow| if row.n_name == name { 1int } else { 0int });
}

pub proof fn lemma_name_count_nonnegative(names: NamePool, key: KeyView)
    requires names_valid(names),
    ensures name_count(names, key) >= 0,
{
    names.model();
    if names.at(key).is_some() {
        let position = choose|position: int| #![auto] 0 <= position < names.entries().len() && names.entries()[position].0 == key;
        assert(names.entries()[position].1.references@ > 0);
    }
}

}
