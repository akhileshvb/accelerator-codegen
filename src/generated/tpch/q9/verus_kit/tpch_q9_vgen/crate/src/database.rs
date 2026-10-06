use vstd::prelude::*;
use verus_common::sums::*;
use verus_common::sqlfns::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::meaning::*;

verus! {

pub proof fn lemma_scan_supplier(db: Db, rows: Seq<SupplierIn>, key: i64, nation: int)
    requires scan_supplier_nations_ok(db, rows),
    ensures sum(rows, |row: SupplierIn| if row.s_suppkey == key && row.s_nationkey as int == nation { 1int } else { 0int })
        == supplier_count(db, key as int, nation),
{
    let weight = |row: SupplierRow| if row.s_suppkey == key as int && row.s_nationkey == nation { 1int } else { 0int };
    assert(sum(supplier_in_seq_view(rows), weight) == sum(db.supplier, weight));
    lemma_sum_map(rows, |row: SupplierIn| supplier_in_view(row), weight,
        |row: SupplierIn| if row.s_suppkey == key && row.s_nationkey as int == nation { 1int } else { 0int });
}

pub proof fn lemma_scan_nation(db: Db, rows: Seq<NationIn>, key: i64, name: Seq<u8>)
    requires scan_nation_names_ok(db, rows),
    ensures sum(rows, nation_input_weight(key, name))
        == nation_count(db, key as int, name),
{
    let weight = |row: NationRow| if row.n_nationkey == key as int && row.n_name == name { 1int } else { 0int };
    assert(sum(nation_in_seq_view(rows), weight) == sum(db.nation, weight));
    lemma_sum_map(rows, |row: NationIn| nation_in_view(row), weight,
        nation_input_weight(key, name));
}

pub proof fn lemma_supplier_key_ranges(db: Db, rows: Seq<SupplierIn>)
    requires scan_supplier_nations_ok(db, rows),
    ensures forall|index: int| 0 <= index < db.supplier.len() ==> {
        let row = #[trigger] db.supplier[index];
        i64::MIN <= row.s_suppkey <= i64::MAX && i64::MIN <= row.s_nationkey <= i64::MAX
    },
{
    let missing = |row: SupplierRow| if i64::MIN <= row.s_suppkey <= i64::MAX && i64::MIN <= row.s_nationkey <= i64::MAX { 0int } else { 1int };
    let projected = |row: SupplierIn| missing(supplier_in_view(row));
    lemma_sum_zero(rows, projected);
    lemma_sum_map(rows, |row: SupplierIn| supplier_in_view(row), missing, projected);
    assert(sum(supplier_in_seq_view(rows), missing) == sum(db.supplier, missing));
    assert forall|index: int| 0 <= index < db.supplier.len() implies {
        let row = #[trigger] db.supplier[index];
        i64::MIN <= row.s_suppkey <= i64::MAX && i64::MIN <= row.s_nationkey <= i64::MAX
    } by {
        if missing(db.supplier[index]) != 0 { lemma_sum_pos(db.supplier, missing, index); }
    }
}

pub proof fn lemma_nation_key_ranges(db: Db, rows: Seq<NationIn>)
    requires scan_nation_names_ok(db, rows),
    ensures forall|index: int| 0 <= index < db.nation.len() ==> i64::MIN <= (#[trigger] db.nation[index]).n_nationkey <= i64::MAX,
{
    let missing = |row: NationRow| if i64::MIN <= row.n_nationkey <= i64::MAX { 0int } else { 1int };
    let projected = |row: NationIn| missing(nation_in_view(row));
    lemma_sum_zero(rows, projected);
    lemma_sum_map(rows, |row: NationIn| nation_in_view(row), missing, projected);
    assert(sum(nation_in_seq_view(rows), missing) == sum(db.nation, missing));
    assert forall|index: int| 0 <= index < db.nation.len() implies i64::MIN <= (#[trigger] db.nation[index]).n_nationkey <= i64::MAX by {
        if missing(db.nation[index]) != 0 { lemma_sum_pos(db.nation, missing, index); }
    }
}

}
