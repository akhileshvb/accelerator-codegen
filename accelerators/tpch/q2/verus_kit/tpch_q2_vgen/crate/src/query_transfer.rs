use vstd::prelude::*;
use verus_common::sums::*;
use verus_common::bag::BagRow;
use verus_prooflib::{class_total, lemma_equal_classes};
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::model::*;
use crate::bags::*;
use crate::storage::*;
use crate::maintenance::*;

verus! {

pub(crate) proof fn lemma_supplier_point(state: State, key: i64, row: SupplierRow)
    requires state.meaning(), state.db() == state.database@,
    ensures supplier_amount(state.suppliers, key, row) == sum(state.db().supplier,
        |value: SupplierRow| if value == row && value.s_suppkey == key as int { 1int } else { 0int }),
{
    lemma_supplier_count_weight(state.db(), key, row,
        |value: SupplierRow| if value == row && value.s_suppkey == key as int { 1int } else { 0int });
}

pub(crate) proof fn lemma_nation_point(state: State, key: i64, row: NationRow)
    requires state.meaning(), state.db() == state.database@,
    ensures nation_amount(state.nations, key, row) == sum(state.db().nation,
        |value: NationRow| if value == row && value.n_nationkey == key as int { 1int } else { 0int }),
{
    lemma_nation_count_weight(state.db(), key, row,
        |value: NationRow| if value == row && value.n_nationkey == key as int { 1int } else { 0int });
}

pub(crate) proof fn lemma_supplier_weight(state: State, key: i64, weight: spec_fn(SupplierRow) -> int)
    requires state.meaning(), state.db() == state.database@,
    ensures index_weight(state.suppliers, key, weight) ==
        sum(state.db().supplier, |row: SupplierRow| if row.s_suppkey == key as int { weight(row) } else { 0int }),
{
    let db = state.db();
    if state.suppliers.lookup(key) is Some {
        let bag = state.suppliers.lookup(key)->0;
        let left_project = |row: SupplierRow| row;
        let right_project = |entry: Counted<SupplierScan>| supplier_scan_view(*entry.row);
        let left_count = |row: SupplierRow| if row.s_suppkey == key as int { 1int } else { 0int };
        let right_count = |entry: Counted<SupplierScan>| entry.count as int;
        assert forall|row: SupplierRow| #[trigger] class_total(db.supplier, left_project, left_count, row) ==
            class_total((*bag).entries@, right_project, right_count, row) by {
            reveal(class_total);
            lemma_sum_ext_all(db.supplier,
                |value: SupplierRow| if left_project(value) == row { left_count(value) } else { 0int },
                |value: SupplierRow| if value == row && value.s_suppkey == key as int { 1int } else { 0int });
            lemma_sum_ext_all((*bag).entries@,
                |entry: Counted<SupplierScan>| if right_project(entry) == row { right_count(entry) } else { 0int },
                |entry: Counted<SupplierScan>| if (*entry.row).rv() == row { entry.count as int } else { 0int });
            lemma_supplier_point(state, key, row);
            lemma_supplier_count_weight(db, key, row,
                |value: SupplierRow| if left_project(value) == row { left_count(value) } else { 0int });
            assert(class_total(db.supplier, left_project, left_count, row) == supplier_database_count(db, key, row));
            assert(class_total((*bag).entries@, right_project, right_count, row) == (*bag).amount(row));
        }
        lemma_equal_classes(db.supplier, (*bag).entries@, left_project, right_project, left_count, right_count, weight);
        lemma_sum_ext_all(db.supplier, |row: SupplierRow| left_count(row) * weight(left_project(row)),
            |row: SupplierRow| if row.s_suppkey == key as int { weight(row) } else { 0int });
        lemma_sum_ext_all((*bag).entries@, |entry: Counted<SupplierScan>| right_count(entry) * weight(right_project(entry)),
            |entry: Counted<SupplierScan>| entry.count as int * weight((*entry.row).rv()));
    } else {
        assert forall|index: int| 0 <= index < db.supplier.len() implies (#[trigger] db.supplier[index]).s_suppkey != key as int by {
            let row = db.supplier[index];
            if row.s_suppkey == key as int {
                let count = |value: SupplierRow| if value == row && value.s_suppkey == key as int { 1int } else { 0int };
                lemma_supplier_point(state, key, row);
                assert(supplier_amount(state.suppliers, key, row) == sum(db.supplier, count));
                lemma_sum_pos(db.supplier, count, index);
            }
        }
        lemma_sum_zero(db.supplier, |row: SupplierRow| if row.s_suppkey == key as int { weight(row) } else { 0int });
    }
}

pub(crate) proof fn lemma_nation_weight(state: State, key: i64, weight: spec_fn(NationRow) -> int)
    requires state.meaning(), state.db() == state.database@,
    ensures index_weight(state.nations, key, weight) ==
        sum(state.db().nation, |row: NationRow| if row.n_nationkey == key as int { weight(row) } else { 0int }),
{
    let db = state.db();
    if state.nations.lookup(key) is Some {
        let bag = state.nations.lookup(key)->0;
        let left_project = |row: NationRow| row;
        let right_project = |entry: Counted<NationScan>| nation_scan_view(*entry.row);
        let left_count = |row: NationRow| if row.n_nationkey == key as int { 1int } else { 0int };
        let right_count = |entry: Counted<NationScan>| entry.count as int;
        assert forall|row: NationRow| #[trigger] class_total(db.nation, left_project, left_count, row) ==
            class_total((*bag).entries@, right_project, right_count, row) by {
            reveal(class_total);
            lemma_sum_ext_all(db.nation,
                |value: NationRow| if left_project(value) == row { left_count(value) } else { 0int },
                |value: NationRow| if value == row && value.n_nationkey == key as int { 1int } else { 0int });
            lemma_sum_ext_all((*bag).entries@,
                |entry: Counted<NationScan>| if right_project(entry) == row { right_count(entry) } else { 0int },
                |entry: Counted<NationScan>| if (*entry.row).rv() == row { entry.count as int } else { 0int });
            lemma_nation_point(state, key, row);
            lemma_nation_count_weight(db, key, row,
                |value: NationRow| if left_project(value) == row { left_count(value) } else { 0int });
            assert(class_total(db.nation, left_project, left_count, row) == nation_database_count(db, key, row));
            assert(class_total((*bag).entries@, right_project, right_count, row) == (*bag).amount(row));
        }
        lemma_equal_classes(db.nation, (*bag).entries@, left_project, right_project, left_count, right_count, weight);
        lemma_sum_ext_all(db.nation, |row: NationRow| left_count(row) * weight(left_project(row)),
            |row: NationRow| if row.n_nationkey == key as int { weight(row) } else { 0int });
        lemma_sum_ext_all((*bag).entries@, |entry: Counted<NationScan>| right_count(entry) * weight(right_project(entry)),
            |entry: Counted<NationScan>| entry.count as int * weight((*entry.row).rv()));
    } else {
        assert forall|index: int| 0 <= index < db.nation.len() implies (#[trigger] db.nation[index]).n_nationkey != key as int by {
            let row = db.nation[index];
            if row.n_nationkey == key as int {
                let count = |value: NationRow| if value == row && value.n_nationkey == key as int { 1int } else { 0int };
                lemma_nation_point(state, key, row);
                assert(nation_amount(state.nations, key, row) == sum(db.nation, count));
                lemma_sum_pos(db.nation, count, index);
            }
        }
        lemma_sum_zero(db.nation, |row: NationRow| if row.n_nationkey == key as int { weight(row) } else { 0int });
    }
}

pub proof fn lemma_brass_entries(entries: Seq<Counted<BrassPartScan>>, weight: spec_fn(PartRow) -> int,
    raw: spec_fn(Counted<BrassPartScan>) -> int)
    requires forall|entry: Counted<BrassPartScan>| #[trigger] raw(entry) == entry.count as int * weight((*entry.row).rv()),
    ensures weighted_entries(entries, weight) == sum(entries, raw),
{
    lemma_sum_ext_all(entries, |entry: Counted<BrassPartScan>| entry.count as int * weight((*entry.row).rv()), raw);
}

pub proof fn lemma_quote_entries(entries: Seq<Counted<BrassPartsuppScan>>, weight: spec_fn(PartsuppRow) -> int,
    raw: spec_fn(Counted<BrassPartsuppScan>) -> int)
    requires forall|entry: Counted<BrassPartsuppScan>| #[trigger] raw(entry) == entry.count as int * weight((*entry.row).rv()),
    ensures weighted_entries(entries, weight) == sum(entries, raw),
{
    lemma_sum_ext_all(entries, |entry: Counted<BrassPartsuppScan>| entry.count as int * weight((*entry.row).rv()), raw);
}

pub(crate) proof fn lemma_quote_class(state: State, key: i64, supplier: int, cost: int, raw: spec_fn(PartsuppRow) -> int)
    requires state.meaning(), state.db() == state.database@, eligible(state.db(), key) > 0,
        forall|row: PartsuppRow| #[trigger] raw(row) == if row.ps_partkey == key as int && row.ps_suppkey == supplier && row.ps_supplycost == cost { 1int } else { 0int },
    ensures quote_amount(state.quotes, key, supplier, cost) == sum(state.db().partsupp, raw),
{
    lemma_sum_ext_all(state.db().partsupp,
        |row: PartsuppRow| if row.ps_partkey == key as int && row.ps_suppkey == supplier && row.ps_supplycost == cost { 1int } else { 0int }, raw);
}

pub(crate) proof fn lemma_quote_weight(state: State, key: i64, weight: spec_fn(PartsuppRow) -> int)
    requires state.meaning(), state.db() == state.database@, eligible(state.db(), key) > 0,
    ensures index_weight(state.quotes, key, weight) ==
        sum(state.db().partsupp, |row: PartsuppRow| if row.ps_partkey == key as int { weight(row) } else { 0int }),
{
    let db = state.db();
    let left_project = |row: PartsuppRow| (row.ps_suppkey, row.ps_supplycost);
    let left_count = |row: PartsuppRow| if row.ps_partkey == key as int { 1int } else { 0int };
    let projected_weight = |pair: (int, int)| weight(PartsuppRow { ps_partkey: key as int, ps_suppkey: pair.0, ps_supplycost: pair.1 });
    if state.quotes.lookup(key) is Some {
        let bag = state.quotes.lookup(key)->0;
        let right_project = |entry: Counted<BrassPartsuppScan>| ((*entry.row).ps_suppkey as int, (*entry.row).ps_supplycost as int);
        let right_count = |entry: Counted<BrassPartsuppScan>| entry.count as int;
        assert forall|pair: (int, int)| #[trigger] class_total(db.partsupp, left_project, left_count, pair) ==
            class_total((*bag).entries@, right_project, right_count, pair) by {
            lemma_quote_class(state, key, pair.0, pair.1,
                |row: PartsuppRow| if left_project(row) == pair { left_count(row) } else { 0int });
            lemma_quote_entries((*bag).entries@,
                |row: PartsuppRow| if row.ps_suppkey == pair.0 && row.ps_supplycost == pair.1 { 1int } else { 0int },
                |entry: Counted<BrassPartsuppScan>| if right_project(entry) == pair { right_count(entry) } else { 0int });
        }
        lemma_equal_classes(db.partsupp, (*bag).entries@, left_project, right_project, left_count, right_count, projected_weight);
        lemma_sum_ext_all(db.partsupp, |row: PartsuppRow| left_count(row) * projected_weight(left_project(row)),
            |row: PartsuppRow| if row.ps_partkey == key as int { weight(row) } else { 0int });
        assert forall|index: int| 0 <= index < (*bag).entries@.len() implies
            right_count(#[trigger] (*bag).entries@[index]) * projected_weight(right_project((*bag).entries@[index])) ==
            (*bag).entries@[index].count as int * weight((*(*bag).entries@[index].row).rv()) by {
            assert((*(*bag).entries@[index].row).rv().ps_partkey == key as int);
        }
        lemma_sum_ext((*bag).entries@, |entry: Counted<BrassPartsuppScan>| right_count(entry) * projected_weight(right_project(entry)),
            |entry: Counted<BrassPartsuppScan>| entry.count as int * weight((*entry.row).rv()));
    } else {
        assert forall|index: int| 0 <= index < db.partsupp.len() implies (#[trigger] db.partsupp[index]).ps_partkey != key as int by {
            let row = db.partsupp[index];
            if row.ps_partkey == key as int {
                let raw = |value: PartsuppRow| if value.ps_partkey == key as int && value.ps_suppkey == row.ps_suppkey && value.ps_supplycost == row.ps_supplycost { 1int } else { 0int };
                lemma_quote_class(state, key, row.ps_suppkey, row.ps_supplycost, raw);
                lemma_sum_pos(db.partsupp, raw, index);
            }
        }
        lemma_sum_zero(db.partsupp, |row: PartsuppRow| if row.ps_partkey == key as int { weight(row) } else { 0int });
    }
}


}
