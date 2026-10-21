use crate::semantics::params0;
use crate::packed::{self, Packed, PackedCursor};
use vstd::prelude::*;
use verus_prooflib::wide::Wide;
use verus_common::sums::{sum, lemma_sum_take};
use verus_prooflib::ordered::{Index, KeyView};
use crate::model::*;
use crate::inventory::*;
use crate::construction::*;
use crate::maintenance::merge_inventory;
use crate::gen_spec::*;
use crate::gen_obligations::params;
use crate::semantics::*;

verus! {

pub open spec fn measure(inventory: Inventory, part: Option<i64>, value: bool) -> int {
    moment_measure(match part { Some(part) => moment_at(inventory.groups, part as int), None => inventory.total@ }, value)
}

pub open spec fn source_measure(inventories: Index<Inventory>, key: KeyView, part: Option<i64>, value: bool) -> int {
    match inventories.at(key) { Some(inventory) => measure(inventory, part, value), None => 0 }
}

pub open spec fn support_measure(inventories: Index<Inventory>, support: Index<Wide>, part: Option<i64>, value: bool) -> int {
    sum(support.entries(), |row: (KeyView, Wide)| source_measure(inventories, row.0, part, value) * row.1@)
}

pub open spec fn binding_ok(data: Data, key: KeyView) -> bool {
    match data.nations_reverse.at(key) {
        None => data.names.at(key).is_none(),
        Some(support) => data.names.at(key).is_some() && match data.names.at(key).unwrap() {
            NameView::Singleton { nation, multiplicity } => support.entries().len() == 1
                && support.at(numeric_key(nation as int)).is_some() && support.at(numeric_key(nation as int)).unwrap()@ == multiplicity@,
            NameView::Merged { inventory } => support.entries().len() >= 2 && inventory.valid()
                && forall|part: Option<i64>, value: bool| #[trigger] measure(inventory, part, value)
                    == support_measure(data.inventories, support, part, value),
        },
    }
}

pub open spec fn indexed_names_ok(data: Data) -> bool {
    forall|key: KeyView| #[trigger] binding_ok(data, key)
}

pub open spec fn row_weight(row: PartsuppRow, part: Option<i64>, value: bool) -> int {
    if part.is_none() || row.ps_partkey == part.unwrap() { if value { row.ps_supplycost * row.ps_availqty } else { 1 } } else { 0 }
}

pub open spec fn h_measure(db: Db, nation: i64, part: Option<i64>, value: bool) -> int {
    sum(db.partsupp, |row: PartsuppRow| row_weight(row, part, value) * s_count(db, row.ps_suppkey, nation as int))
}

pub proof fn lemma_source_measure(data: Data, db: Db, nation: i64, part: Option<i64>, value: bool)
    requires data.inventories_ok(db), db_bounded(db),
    ensures source_measure(data.inventories, numeric_key(nation as int), part, value) == h_measure(db, nation, part, value),
{
    if data.inventories.at(numeric_key(nation as int)).is_none() { lemma_absent_inventory(data, db, nation); }
    let weighted = |row: PartsuppRow| row_weight(row, part, value) * s_count(db, row.ps_suppkey, nation as int);
    let normal = |row: PartsuppRow| if part.is_none() || row.ps_partkey == part.unwrap() {
        if value { row.ps_availqty * row.ps_supplycost * s_count(db, row.ps_suppkey, nation as int) }
        else { s_count(db, row.ps_suppkey, nation as int) }
    } else { 0 };
    assert forall|row: PartsuppRow| #[trigger] weighted(row) == normal(row) by {
        if value { assert(row.ps_supplycost * row.ps_availqty * s_count(db, row.ps_suppkey, nation as int)
            == row.ps_availqty * row.ps_supplycost * s_count(db, row.ps_suppkey, nation as int)) by (nonlinear_arith); }
    }
    verus_common::sums::lemma_sum_ext_all(db.partsupp, weighted, normal);
    if let Some(part) = part {
        if value { assert(normal =~= (|row: PartsuppRow| if row.ps_partkey == part { row.ps_availqty * row.ps_supplycost * s_count(db, row.ps_suppkey, nation as int) } else { 0 })); }
        else { assert(normal =~= (|row: PartsuppRow| if row.ps_partkey == part { s_count(db, row.ps_suppkey, nation as int) } else { 0 })); }
        assert(inventory_at(data.inventories, nation as int, part as int).count as int == h_count(db, nation as int, part as int));
        assert(inventory_at(data.inventories, nation as int, part as int).value as int == h_value(db, nation as int, part as int));
    } else {
        if value { assert(normal =~= (|row: PartsuppRow| row.ps_availqty * row.ps_supplycost * s_count(db, row.ps_suppkey, nation as int))); }
        else { assert(normal =~= (|row: PartsuppRow| s_count(db, row.ps_suppkey, nation as int))); }
    }
}

pub proof fn lemma_support_supplier(data: Data, db: Db, name: Seq<u8>, support: Index<Wide>, supplier: int)
    requires data.nations_ok(db), db_bounded(db), data.nations_reverse.at(text_key(name)) == Some(support),
    ensures sum(support.entries(), |entry: (KeyView, Wide)| s_count(db, supplier, entry.0.0) * entry.1@)
        == supplier_weight(db, name, supplier),
{
    support.model();
    let left = |entry: (KeyView, Wide)| s_count(db, supplier, entry.0.0) * entry.1@;
    let pair = |entry: (KeyView, Wide), row: SupplierRow| if row.s_suppkey == supplier && numeric_key(row.s_nationkey) == entry.0 { entry.1@ } else { 0 };
    let expanded = |entry: (KeyView, Wide)| sum(db.supplier, |row: SupplierRow| pair(entry, row));
    assert forall|position: int| 0 <= position < support.entries().len() implies left(#[trigger] support.entries()[position]) == expanded(support.entries()[position]) by {
        let entry = support.entries()[position];
        assert(support.at(entry.0).is_some()); assert(numeric(entry.0));
        let raw = |row: SupplierRow| if row.s_suppkey == supplier && row.s_nationkey == entry.0.0 { 1int } else { 0 };
        let scaled = |row: SupplierRow| pair(entry, row);
        verus_common::sums::lemma_sum_scale(db.supplier, raw, scaled, entry.1@);
        assert(left(entry) == expanded(entry)) by (nonlinear_arith)
            requires left(entry) == s_count(db, supplier, entry.0.0) * entry.1@,
                expanded(entry) == entry.1@ * s_count(db, supplier, entry.0.0);
    }
    verus_common::sums::lemma_sum_ext(support.entries(), left, expanded);
    let right = |row: SupplierRow| sum(support.entries(), |entry: (KeyView, Wide)| pair(entry, row));
    verus_common::sums::lemma_sum_swap(support.entries(), db.supplier, pair, expanded, right);
    let expected = |row: SupplierRow| if row.s_suppkey == supplier { n_count(db, row.s_nationkey, name) } else { 0 };
    assert forall|position: int| 0 <= position < db.supplier.len() implies right(#[trigger] db.supplier[position]) == expected(db.supplier[position]) by {
        let row = db.supplier[position];
        let weight = |entry: (KeyView, Wide)| if row.s_suppkey == supplier { entry.1@ } else { 0 };
        verus_prooflib::ordered::lemma_key_contribution(&support, numeric_key(row.s_nationkey), weight);
        assert((|entry: (KeyView, Wide)| pair(entry, row)) =~= (|entry: (KeyView, Wide)| if entry.0 == numeric_key(row.s_nationkey) { weight(entry) } else { 0 }));
        reveal(verus_prooflib::ordered::contribution);
        let nation = row.s_nationkey as i64;
        assert(nation as int == row.s_nationkey);
        assert(count_at(data.nations, numeric_key(nation as int), text_key(name)) == n_count(db, nation as int, name));
        assert(count_at(data.nations_reverse, text_key(name), numeric_key(nation as int)) == n_count(db, nation as int, name));
        if support.at(numeric_key(row.s_nationkey)).is_none() { assert(n_count(db, row.s_nationkey, name) == 0); }
    }
    verus_common::sums::lemma_sum_ext(db.supplier, right, expected);
}

pub proof fn lemma_support_query(data: Data, db: Db, name: Seq<u8>, support: Index<Wide>, part: Option<i64>, value: bool)
    requires data.nations_ok(db), data.inventories_ok(db), db_bounded(db), data.nations_reverse.at(text_key(name)) == Some(support),
    ensures support_measure(data.inventories, support, part, value) == query_fold(db, name, |row: PartsuppRow| row_weight(row, part, value)),
{
    support.model();
    let left = |entry: (KeyView, Wide)| source_measure(data.inventories, entry.0, part, value) * entry.1@;
    let pair = |entry: (KeyView, Wide), row: PartsuppRow| row_weight(row, part, value) * s_count(db, row.ps_suppkey, entry.0.0) * entry.1@;
    let expanded = |entry: (KeyView, Wide)| sum(db.partsupp, |row: PartsuppRow| pair(entry, row));
    assert forall|position: int| 0 <= position < support.entries().len() implies left(#[trigger] support.entries()[position]) == expanded(support.entries()[position]) by {
        let entry = support.entries()[position];
        assert(support.at(entry.0).is_some()); assert(numeric(entry.0));
        lemma_source_measure(data, db, entry.0.0 as i64, part, value);
        let raw = |row: PartsuppRow| row_weight(row, part, value) * s_count(db, row.ps_suppkey, entry.0.0);
        let scaled = |row: PartsuppRow| pair(entry, row);
        assert forall|row: PartsuppRow| #[trigger] scaled(row) == entry.1@ * raw(row) by {
            assert(row_weight(row, part, value) * s_count(db, row.ps_suppkey, entry.0.0) * entry.1@
                == entry.1@ * (row_weight(row, part, value) * s_count(db, row.ps_suppkey, entry.0.0))) by (nonlinear_arith);
        }
        verus_common::sums::lemma_sum_scale(db.partsupp, raw, scaled, entry.1@);
        assert(left(entry) == expanded(entry)) by (nonlinear_arith)
            requires source_measure(data.inventories, entry.0, part, value) == sum(db.partsupp, raw),
                expanded(entry) == entry.1@ * sum(db.partsupp, raw),
                left(entry) == source_measure(data.inventories, entry.0, part, value) * entry.1@;
    }
    verus_common::sums::lemma_sum_ext(support.entries(), left, expanded);
    let right = |row: PartsuppRow| sum(support.entries(), |entry: (KeyView, Wide)| pair(entry, row));
    verus_common::sums::lemma_sum_swap(support.entries(), db.partsupp, pair, expanded, right);
    let expected = |row: PartsuppRow| row_weight(row, part, value) * supplier_weight(db, name, row.ps_suppkey);
    assert forall|row: PartsuppRow| #[trigger] right(row) == expected(row) by {
        lemma_support_supplier(data, db, name, support, row.ps_suppkey);
        let raw = |entry: (KeyView, Wide)| s_count(db, row.ps_suppkey, entry.0.0) * entry.1@;
        let scaled = |entry: (KeyView, Wide)| pair(entry, row);
        assert forall|entry: (KeyView, Wide)| #[trigger] scaled(entry) == row_weight(row, part, value) * raw(entry) by {
            assert(row_weight(row, part, value) * s_count(db, row.ps_suppkey, entry.0.0) * entry.1@
                == row_weight(row, part, value) * (s_count(db, row.ps_suppkey, entry.0.0) * entry.1@)) by (nonlinear_arith);
        }
        verus_common::sums::lemma_sum_scale(support.entries(), raw, scaled, row_weight(row, part, value));
    }
    verus_common::sums::lemma_sum_ext_all(db.partsupp, right, expected);
    assert((|row: PartsuppRow| (|other: PartsuppRow| row_weight(other, part, value))(row) * supplier_weight(db, name, row.ps_suppkey)) =~= expected);
}

pub proof fn lemma_n_count_nonneg(db: Db, nation: int, name: Seq<u8>)
    ensures n_count(db, nation, name) >= 0,
{
    verus_common::sums::lemma_sum_nonneg(db.nation, |row: NationRow| if row.n_nationkey == nation && row.n_name == name { 1int } else { 0 });
}

pub proof fn lemma_indexed_names(data: Data, db: Db)
    requires data.nations_ok(db), data.inventories_ok(db), db_bounded(db), indexed_names_ok(data),
    ensures data.names_ok(db),
{
    assert forall|key: KeyView| (#[trigger] data.names.at(key)).is_some() <==> data.nations_reverse.at(key).is_some() by { assert(binding_ok(data, key)); }
    assert forall|key: KeyView| (#[trigger] data.names.at(key)).is_some() implies {
        let support = data.nations_reverse.at(key).unwrap();
        &&& key.0 == 0
        &&& match data.names.at(key).unwrap() {
            NameView::Singleton { nation, multiplicity } => support.entries().len() == 1 && support.at(numeric_key(nation as int)).is_some() && support.at(numeric_key(nation as int)).unwrap()@ == multiplicity@ && multiplicity@ > 0,
            NameView::Merged { inventory } => support.entries().len() >= 2 && inventory.valid()
                && inventory.total.count@ == scalar_1_group_count(db, params0(key.1))
                && inventory.total.value@ == scalar_1_group_sum(db, params0(key.1), |tuple: Scalar1Tuple| tuple.partsupp_2.ps_supplycost * tuple.partsupp_2.ps_availqty)
                && forall|part: i64| #[trigger] moment_at(inventory.groups, part as int).count as int == group_count(db, params0(key.1), part as int)
                    && moment_at(inventory.groups, part as int).value as int == query_group_value(db, key.1, part as int),
        }
    } by {
        assert(binding_ok(data, key));
        let support = data.nations_reverse.at(key).unwrap();
        assert(key.0 == 0);
        if let NameView::Singleton { nation, multiplicity } = data.names.at(key).unwrap() {
            assert(key == text_key(key.1));
            lemma_n_count_nonneg(db, nation as int, key.1);
            assert(data.nations_ok(db));
            assert(count_at(data.nations, numeric_key(nation as int), text_key(key.1)) == n_count(db, nation as int, key.1));
            assert(count_at(data.nations_reverse, text_key(key.1), numeric_key(nation as int)) == n_count(db, nation as int, key.1));
            assert(nonzero_counts(data.nations_reverse));
            assert(data.nations_reverse.at(key).is_some());
            assert(support.at(numeric_key(nation as int)).unwrap()@ != 0);
            assert(count_at(data.nations_reverse, key, numeric_key(nation as int)) == support.at(numeric_key(nation as int)).unwrap()@);
            assert(support.at(numeric_key(nation as int)).unwrap()@ != 0);
        }
        if let NameView::Merged { inventory } = data.names.at(key).unwrap() {
            assert(measure(inventory, None, false) == support_measure(data.inventories, support, None, false));
            assert(measure(inventory, None, true) == support_measure(data.inventories, support, None, true));
            let one = |row: PartsuppRow| row_weight(row, None, false);
            let price = |row: PartsuppRow| row_weight(row, None, true);
            lemma_support_query(data, db, key.1, support, None, false);
            lemma_support_query(data, db, key.1, support, None, true);
            lemma_query_fold(db, key.1, one); lemma_query_fold(db, key.1, price);
            assert((|tuple: Scalar1Tuple| one(tuple.partsupp_2)) =~= (|tuple: Scalar1Tuple| 1int));
            assert((|tuple: Scalar1Tuple| price(tuple.partsupp_2)) =~= (|tuple: Scalar1Tuple| tuple.partsupp_2.ps_supplycost * tuple.partsupp_2.ps_availqty));
            verus_prooflib::lemma_sum_constant_one(scalar_1_tuples(db, params0(key.1)));
            assert(inventory.total.count@ == query_fold(db, key.1, one));
            assert(inventory.total.value@ == query_fold(db, key.1, price));
            assert forall|part: i64| #[trigger] moment_at(inventory.groups, part as int).count as int == group_count(db, params0(key.1), part as int)
                && moment_at(inventory.groups, part as int).value as int == query_group_value(db, key.1, part as int) by {
                let count = |row: PartsuppRow| row_weight(row, Some(part), false);
                let price = |row: PartsuppRow| row_weight(row, Some(part), true);
                assert(measure(inventory, Some(part), false) == support_measure(data.inventories, support, Some(part), false));
                assert(measure(inventory, Some(part), true) == support_measure(data.inventories, support, Some(part), true));
                lemma_support_query(data, db, key.1, support, Some(part), false);
                lemma_support_query(data, db, key.1, support, Some(part), true);
                lemma_query_fold(db, key.1, count); lemma_query_fold(db, key.1, price);
                assert((|tuple: Tuple| count(tuple.partsupp)) =~= (|tuple: Tuple| if tuple.partsupp.ps_partkey == part { 1int } else { 0 }));
                assert((|tuple: Tuple| price(tuple.partsupp)) =~= (|tuple: Tuple| if tuple.partsupp.ps_partkey == part { tuple.partsupp.ps_supplycost * tuple.partsupp.ps_availqty } else { 0 }));
                verus_common::sums::lemma_sum_ext_all(tuples(db, params0(key.1)), |tuple: Tuple| price(tuple.partsupp),
                    |tuple: Tuple| if tuple.partsupp.ps_partkey == part { (|source: Tuple| source.partsupp.ps_supplycost * source.partsupp.ps_availqty)(tuple) } else { 0int });
                assert(query_group_value(db, key.1, part as int) == query_fold(db, key.1, price));
                assert(group_count(db, params0(key.1), part as int) == query_fold(db, key.1, count));
            }
        }
    }
}

pub fn merge_support(data: &Data, support: &Index<Wide>) -> (inventory: Inventory)
    requires data.structures_valid(), support.wf(),
    ensures inventory.valid()
        && forall|part: Option<i64>, value: bool| #[trigger] measure(inventory, part, value)
            == support_measure(data.inventories, *support, part, value),
{
    let mut merged = Inventory::new();
    let mut cursor = support.cursor();
    let mut processed = Ghost(0int);
    loop
        invariant merged.valid(), data.structures_valid(), support.wf(),
            0 <= processed@ <= support.entries().len(), cursor.remaining() == support.entries().skip(processed@),
            forall|part: Option<i64>, value: bool| #[trigger] measure(merged, part, value)
                == sum(support.entries().take(processed@), |row: (KeyView, Wide)| source_measure(data.inventories, row.0, part, value) * row.1@),
        decreases cursor.remaining().len(),
    {
        let (nation, weight) = match cursor.next() {
            Some(row) => row,
            None => {
                proof { assert(processed@ == support.entries().len()); assert(support.entries().take(processed@) =~= support.entries()); }
                return merged;
            },
        };
        let ghost previous = merged;
        if let Some(source) = data.inventories.get(&nation) {
            merge_inventory(&mut merged, &*source, &*weight);
        }
        proof {
            assert forall|part: Option<i64>, value: bool| #[trigger] measure(merged, part, value)
                == sum(support.entries().take(processed@ + 1), |row: (KeyView, Wide)| source_measure(data.inventories, row.0, part, value) * row.1@) by {
                lemma_sum_take(support.entries(), processed@, |row: (KeyView, Wide)| source_measure(data.inventories, row.0, part, value) * row.1@);
                assert(measure(previous, part, value)
                    == sum(support.entries().take(processed@), |row: (KeyView, Wide)| source_measure(data.inventories, row.0, part, value) * row.1@));
                if let Some(part) = part {
                    if let Some(source) = data.inventories.at(nation.view()) {
                        assert(moment_at(merged.groups, part as int).count as int == moment_at(previous.groups, part as int).count as int + moment_at(source.groups, part as int).count as int * (*weight)@);
                        assert(moment_at(merged.groups, part as int).value as int == moment_at(previous.groups, part as int).value as int + moment_at(source.groups, part as int).value as int * (*weight)@);
                    }
                }
            }
        }
        processed = Ghost(processed@ + 1);
    }
}

pub proof fn lemma_database_names(data: Data, db: Db)
    requires data.valid(db),
    ensures indexed_names_ok(data),
{
    assert forall|key: KeyView| #[trigger] binding_ok(data, key) by {
        if data.nations_reverse.at(key).is_some() {
            assert(data.names.at(key).is_some());
            let support = data.nations_reverse.at(key).unwrap();
            assert(key.0 == 0);
            if let NameView::Merged { inventory } = data.names.at(key).unwrap() {
                assert forall|part: Option<i64>, value: bool| #[trigger] measure(inventory, part, value)
                    == support_measure(data.inventories, support, part, value) by {
                    if let Some(part) = part {
                        assert(moment_at(inventory.groups, part as int).count as int == group_count(db, params0(key.1), part as int));
                        assert(moment_at(inventory.groups, part as int).value as int == query_group_value(db, key.1, part as int));
                    }
                    assert(measure(inventory, part, value) == sql_measure(db, key.1, part, value));
                    lemma_support_sql(data, db, key.1, support, part, value);
                }
            }
        }
    }
}

pub open spec fn sql_measure(db: Db, name: Seq<u8>, part: Option<i64>, value: bool) -> int {
    match part {
        Some(part) => if value { query_group_value(db, name, part as int) } else { group_count(db, params0(name), part as int) },
        None => if value { scalar_1_group_sum(db, params0(name), |tuple: Scalar1Tuple| tuple.partsupp_2.ps_supplycost * tuple.partsupp_2.ps_availqty) } else { scalar_1_group_count(db, params0(name)) },
    }
}

pub proof fn lemma_support_sql(data: Data, db: Db, name: Seq<u8>, support: Index<Wide>, part: Option<i64>, value: bool)
    requires data.nations_ok(db), data.inventories_ok(db), db_bounded(db), data.nations_reverse.at(text_key(name)) == Some(support),
    ensures support_measure(data.inventories, support, part, value) == sql_measure(db, name, part, value),
{
    if let Some(part) = part {
        let count = |row: PartsuppRow| row_weight(row, Some(part), false);
        let price = |row: PartsuppRow| row_weight(row, Some(part), true);
        lemma_support_query(data, db, name, support, Some(part), false);
        lemma_support_query(data, db, name, support, Some(part), true);
        lemma_query_fold(db, name, count); lemma_query_fold(db, name, price);
        assert((|tuple: Tuple| count(tuple.partsupp)) =~= (|tuple: Tuple| if tuple.partsupp.ps_partkey == part { 1int } else { 0 }));
        assert((|tuple: Tuple| price(tuple.partsupp)) =~= (|tuple: Tuple| if tuple.partsupp.ps_partkey == part { tuple.partsupp.ps_supplycost * tuple.partsupp.ps_availqty } else { 0 }));
        verus_common::sums::lemma_sum_ext_all(tuples(db, params0(name)), |tuple: Tuple| price(tuple.partsupp),
            |tuple: Tuple| if tuple.partsupp.ps_partkey == part { (|source: Tuple| source.partsupp.ps_supplycost * source.partsupp.ps_availqty)(tuple) } else { 0int });
    } else {
        let one = |row: PartsuppRow| row_weight(row, None, false);
        let price = |row: PartsuppRow| row_weight(row, None, true);
        lemma_support_query(data, db, name, support, None, false);
        lemma_support_query(data, db, name, support, None, true);
        lemma_query_fold(db, name, one); lemma_query_fold(db, name, price);
        assert((|tuple: Scalar1Tuple| one(tuple.partsupp_2)) =~= (|tuple: Scalar1Tuple| 1int));
        assert((|tuple: Scalar1Tuple| price(tuple.partsupp_2)) =~= (|tuple: Scalar1Tuple| tuple.partsupp_2.ps_supplycost * tuple.partsupp_2.ps_availqty));
        verus_prooflib::lemma_sum_constant_one(scalar_1_tuples(db, params0(name)));
    }
}

}
