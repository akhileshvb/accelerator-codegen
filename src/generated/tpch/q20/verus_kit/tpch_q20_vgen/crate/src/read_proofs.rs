use vstd::prelude::*;
use verus_common::sums::*;
use verus_common::rel::*;
use verus_prooflib::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::model::*;
use crate::profiles::*;
use crate::dimensions::*;
use crate::storage::*;
use crate::predicate_proofs::*;

verus! {
pub open spec fn variant_weight(rows: Seq<SupplierVariant>, weight: spec_fn(OutRow) -> int) -> int {
    sum(rows, |row: SupplierVariant| row.count as int * weight(OutRow { s_name: row.name@, s_address: row.address@ }))
}
pub proof fn lemma_variant_take(rows: Seq<SupplierVariant>, index: int, weight: spec_fn(OutRow) -> int)
    requires 0 <= index < rows.len(),
    ensures variant_weight(rows.take(index + 1), weight) == variant_weight(rows.take(index), weight)
        + rows[index].count as int * weight(OutRow { s_name: rows[index].name@, s_address: rows[index].address@ }),
{
    lemma_sum_take(rows, index, |row: SupplierVariant| row.count as int * weight(OutRow { s_name: row.name@, s_address: row.address@ }));
}
pub open spec fn bucket_rows(bucket: Table<Vec<SupplierVariant>>, key: i64) -> Seq<SupplierVariant> {
    match bucket.lookup(key) { Some(rows) => rows@, None => Seq::empty() }
}
pub open spec fn bucket_weight(parts: Table<Part>, directory: Table<Vec<i64>>, bucket: Table<Vec<SupplierVariant>>, keys: Seq<i64>, parameters: Params, weight: spec_fn(OutRow) -> int) -> int {
    sum(keys, |key: i64| if cached_supplier_passes(parts, directory, key, parameters.p1, parameters.p3) { variant_weight(bucket_rows(bucket, key), weight) } else { 0 })
}
pub proof fn lemma_bucket_take(parts: Table<Part>, directory: Table<Vec<i64>>, bucket: Table<Vec<SupplierVariant>>, keys: Seq<i64>, index: int, parameters: Params, weight: spec_fn(OutRow) -> int)
    requires 0 <= index < keys.len(),
    ensures bucket_weight(parts, directory, bucket, keys.take(index + 1), parameters, weight) == bucket_weight(parts, directory, bucket, keys.take(index), parameters, weight)
        + if cached_supplier_passes(parts, directory, keys[index], parameters.p1, parameters.p3) { variant_weight(bucket_rows(bucket, keys[index]), weight) } else { 0 },
{
    lemma_sum_take(keys, index, |key: i64| if cached_supplier_passes(parts, directory, key, parameters.p1, parameters.p3) { variant_weight(bucket_rows(bucket, key), weight) } else { 0 });
}
pub open spec fn nation_weight(db: Db, parameters: Params, nation: int, weight: spec_fn(OutRow) -> int) -> int {
    sum(db.supplier, |row: SupplierRow| if row.s_nationkey == nation && in_1(db, parameters, row.s_suppkey) { weight(OutRow { s_name: row.s_name, s_address: row.s_address }) } else { 0 })
}
pub open spec fn nation_output_weight(db: Db, parameters: Params, rows: Seq<NationVariant>, weight: spec_fn(OutRow) -> int) -> int {
    sum(rows, |row: NationVariant| if row.name@ == parameters.p2 { row.count as int * nation_weight(db, parameters, row.key as int, weight) } else { 0 })
}
pub open spec fn sql_nation_weight(db: Db, parameters: Params, weight: spec_fn(OutRow) -> int) -> int {
    sum(db.nation, |row: NationRow| if row.n_name == parameters.p2 { nation_weight(db, parameters, row.n_nationkey, weight) } else { 0 })
}
pub proof fn lemma_nation_weight_as_sum(db: Db, parameters: Params, nation: int, weight: spec_fn(OutRow) -> int, term: spec_fn(SupplierRow) -> int)
    requires forall|row: SupplierRow| #[trigger] term(row) == if row.s_nationkey == nation && in_1(db, parameters, row.s_suppkey) { weight(OutRow { s_name: row.s_name, s_address: row.s_address }) } else { 0 },
    ensures nation_weight(db, parameters, nation, weight) == sum(db.supplier, term),
{
    lemma_sum_ext_all(db.supplier,
        |row: SupplierRow| if row.s_nationkey == nation && in_1(db, parameters, row.s_suppkey) { weight(OutRow { s_name: row.s_name, s_address: row.s_address }) } else { 0 }, term);
}
pub proof fn lemma_query_weight(db: Db, parameters: Params, weight: spec_fn(OutRow) -> int)
    ensures sum(crate::gen_spec::rows(db, parameters), weight) == sql_nation_weight(db, parameters, weight),
{
    let tuple_weight = |tuple: Tuple| weight(OutRow { s_name: tuple.supplier.s_name, s_address: tuple.supplier.s_address });
    let joint = |supplier: SupplierRow, nation: NationRow| if where_clause(db, parameters, supplier, nation) { weight(OutRow { s_name: supplier.s_name, s_address: supplier.s_address }) } else { 0int };
    let by_supplier = |supplier: SupplierRow| sum(db.nation, |nation: NationRow| joint(supplier, nation));
    let by_nation = |nation: NationRow| sum(db.supplier, |supplier: SupplierRow| joint(supplier, nation));
    lemma_sum_map(tuples(db, parameters), |tuple: Tuple| OutRow { s_name: tuple.supplier.s_name, s_address: tuple.supplier.s_address }, weight, tuple_weight);
    let outer = |supplier: SupplierRow| tuples_1(db, parameters, supplier);
    lemma_sum_flat_map(db.supplier, outer, tuple_weight);
    assert forall|supplier: SupplierRow| #[trigger] sum(outer(supplier), tuple_weight) == by_supplier(supplier) by {
        let inner = |nation: NationRow| tuples_2(db, parameters, supplier, nation);
        lemma_sum_flat_map(db.nation, inner, tuple_weight);
        assert forall|nation: NationRow| #[trigger] sum(inner(nation), tuple_weight) == joint(supplier, nation) by {
            if where_clause(db, parameters, supplier, nation) { lemma_sum_singleton(Tuple { supplier, nation }, tuple_weight); }
        }
        lemma_sum_ext_all(db.nation, |nation: NationRow| sum(inner(nation), tuple_weight), |nation: NationRow| joint(supplier, nation));
    }
    lemma_sum_ext_all(db.supplier, |supplier: SupplierRow| sum(outer(supplier), tuple_weight), by_supplier);
    lemma_sum_swap(db.supplier, db.nation, joint, by_supplier, by_nation);
    assert forall|nation: NationRow| #[trigger] by_nation(nation) == if nation.n_name == parameters.p2 { nation_weight(db, parameters, nation.n_nationkey, weight) } else { 0int } by {
        if nation.n_name == parameters.p2 {
            lemma_nation_weight_as_sum(db, parameters, nation.n_nationkey, weight, |supplier: SupplierRow| joint(supplier, nation));
        } else { lemma_sum_zero(db.supplier, |supplier: SupplierRow| joint(supplier, nation)); }
    }
    lemma_sum_ext_all(db.nation, by_nation, |nation: NationRow| if nation.n_name == parameters.p2 { nation_weight(db, parameters, nation.n_nationkey, weight) } else { 0int });
}
pub proof fn lemma_nation_rows(db: Db, parameters: Params, nations: Nations, rows: Seq<NationVariant>, weight: spec_fn(OutRow) -> int)
    requires nation_correct(db, nations), domain_valid(db), nations.lookup(name_hash(parameters.p2) as i64) matches Some(bucket) && bucket@ == rows,
    ensures nation_output_weight(db, parameters, rows, weight) == sum(crate::gen_spec::rows(db, parameters), weight),
{
    let left_project = |row: NationVariant| row.key as int;
    let right_project = |row: NationRow| row.n_nationkey;
    let left_count = |row: NationVariant| if row.name@ == parameters.p2 { row.count as int } else { 0int };
    let right_count = |row: NationRow| if row.n_name == parameters.p2 { 1int } else { 0int };
    assert forall|key: int| #[trigger] class_total(rows, left_project, left_count, key) == class_total(db.nation, right_project, right_count, key) by {
        if machine_key(key) {
            let bounded = key as i64;
            assert(bounded as int == key);
            lemma_sum_ext_all(rows, |row: NationVariant| if left_project(row) == key { left_count(row) } else { 0 },
                |row: NationVariant| if row.name@ == parameters.p2 && row.key == bounded { row.count as int } else { 0 });
            lemma_sum_ext_all(db.nation, |row: NationRow| if right_project(row) == key { right_count(row) } else { 0 },
                |row: NationRow| if row.n_name == parameters.p2 && row.n_nationkey == bounded { 1int } else { 0int });
            assert(nation_measure(rows, parameters.p2, bounded) == nation_count(nations, parameters.p2, bounded));
            lemma_nation_measure_weight(rows, parameters.p2, bounded, |row: NationVariant| if left_project(row) == key { left_count(row) } else { 0 });
            assert(class_total(rows, left_project, left_count, key) == nation_measure(rows, parameters.p2, bounded));
            assert(class_total(db.nation, right_project, right_count, key) == sum(db.nation,
                |row: NationRow| if row.n_name == parameters.p2 && row.n_nationkey == bounded { 1int } else { 0int }));
            assert(nation_count(nations, parameters.p2, bounded) == sum(db.nation,
                |row: NationRow| if row.n_name == parameters.p2 && row.n_nationkey == bounded { 1int } else { 0int }));
        } else {
            lemma_sum_zero(rows, |row: NationVariant| if left_project(row) == key { left_count(row) } else { 0 });
            lemma_sum_zero(db.nation, |row: NationRow| if right_project(row) == key { right_count(row) } else { 0 });
        }
    }
    let class_weight = |key: int| nation_weight(db, parameters, key, weight);
    lemma_equal_classes(rows, db.nation, left_project, right_project, left_count, right_count, class_weight);
    lemma_sum_ext_all(rows, |row: NationVariant| left_count(row) * class_weight(left_project(row)),
        |row: NationVariant| if row.name@ == parameters.p2 { row.count as int * nation_weight(db, parameters, row.key as int, weight) } else { 0 });
    lemma_sum_ext_all(db.nation, |row: NationRow| right_count(row) * class_weight(right_project(row)),
        |row: NationRow| if row.n_name == parameters.p2 { nation_weight(db, parameters, row.n_nationkey, weight) } else { 0 });
    lemma_query_weight(db, parameters, weight);
}
pub proof fn lemma_variants(db: Db, nation: i64, supplier: i64, rows: Seq<SupplierVariant>, weight: spec_fn(OutRow) -> int)
    requires forall|name: Seq<u8>, address: Seq<u8>| #[trigger] supplier_measure(rows, name, address) ==
        sum(db.supplier, |row: SupplierRow| if row.s_nationkey == nation && row.s_suppkey == supplier && row.s_name == name && row.s_address == address { 1 } else { 0 }),
    ensures variant_weight(rows, weight) == sum(db.supplier, |row: SupplierRow| if row.s_nationkey == nation && row.s_suppkey == supplier { weight(OutRow { s_name: row.s_name, s_address: row.s_address }) } else { 0 }),
{
    let left_project = |row: SupplierVariant| (row.name@, row.address@);
    let right_project = |row: SupplierRow| (row.s_name, row.s_address);
    let left_count = |row: SupplierVariant| row.count as int;
    let right_count = |row: SupplierRow| if row.s_nationkey == nation && row.s_suppkey == supplier { 1int } else { 0int };
    assert forall|key: (Seq<u8>, Seq<u8>)| #[trigger] class_total(rows, left_project, left_count, key) == class_total(db.supplier, right_project, right_count, key) by {
        lemma_sum_ext_all(rows, |row: SupplierVariant| if left_project(row) == key { left_count(row) } else { 0 },
            |row: SupplierVariant| if row.name@ == key.0 && row.address@ == key.1 { row.count as int } else { 0 });
        lemma_sum_ext_all(db.supplier, |row: SupplierRow| if right_project(row) == key { right_count(row) } else { 0 },
            |row: SupplierRow| if row.s_nationkey == nation && row.s_suppkey == supplier && row.s_name == key.0 && row.s_address == key.1 { 1int } else { 0int });
        assert(supplier_measure(rows, key.0, key.1) == sum(db.supplier,
            |row: SupplierRow| if row.s_nationkey == nation && row.s_suppkey == supplier && row.s_name == key.0 && row.s_address == key.1 { 1int } else { 0int }));
    }
    let class_weight = |key: (Seq<u8>, Seq<u8>)| weight(OutRow { s_name: key.0, s_address: key.1 });
    lemma_equal_classes(rows, db.supplier, left_project, right_project, left_count, right_count, class_weight);
    lemma_sum_ext_all(rows, |row: SupplierVariant| left_count(row) * class_weight(left_project(row)),
        |row: SupplierVariant| row.count as int * weight(OutRow { s_name: row.name@, s_address: row.address@ }));
    lemma_sum_ext_all(db.supplier, |row: SupplierRow| right_count(row) * class_weight(right_project(row)),
        |row: SupplierRow| if row.s_nationkey == nation && row.s_suppkey == supplier { weight(OutRow { s_name: row.s_name, s_address: row.s_address }) } else { 0 });
}
pub proof fn lemma_bucket(db: Db, parameters: Params, parts: Table<Part>, directory: Table<Vec<i64>>, suppliers: Suppliers, nations: Nations,
    nation: i64, bucket: Table<Vec<SupplierVariant>>, keys: Seq<i64>, weight: spec_fn(OutRow) -> int)
    requires meaning(db, parts, directory, suppliers, nations), domain_valid(db), parameters.p1 < parameters.p3,
        suppliers.lookup(nation) == Some(bucket), keys.no_duplicates(),
        forall|key: i64| #![trigger bucket.lookup(key)] keys.contains(key) == bucket.lookup(key).is_some(),
    ensures bucket_weight(parts, directory, bucket, keys, parameters, weight) == nation_weight(db, parameters, nation as int, weight),
{
    let joint = |key: i64, row: SupplierRow| if row.s_nationkey == nation && row.s_suppkey == key && in_1(db, parameters, key as int) {
        weight(OutRow { s_name: row.s_name, s_address: row.s_address })
    } else { 0int };
    let by_key = |key: i64| if cached_supplier_passes(parts, directory, key, parameters.p1, parameters.p3) { variant_weight(bucket_rows(bucket, key), weight) } else { 0int };
    let by_row = |row: SupplierRow| sum(keys, |key: i64| joint(key, row));
    assert forall|key: i64| #[trigger] by_key(key) == sum(db.supplier, |row: SupplierRow| joint(key, row)) by {
        lemma_cached_supplier(db, parameters, parts, directory, suppliers, nations, key);
        if cached_supplier_passes(parts, directory, key, parameters.p1, parameters.p3) {
            assert forall|name: Seq<u8>, address: Seq<u8>| #[trigger] supplier_measure(bucket_rows(bucket, key), name, address) ==
                sum(db.supplier, |row: SupplierRow| if row.s_nationkey == nation && row.s_suppkey == key && row.s_name == name && row.s_address == address { 1 } else { 0 }) by {
                assert(supplier_count(suppliers, nation, key, name, address) == supplier_measure(bucket_rows(bucket, key), name, address));
            }
            lemma_variants(db, nation, key, bucket_rows(bucket, key), weight);
            lemma_sum_ext_all(db.supplier, |row: SupplierRow| joint(key, row),
                |row: SupplierRow| if row.s_nationkey == nation && row.s_suppkey == key { weight(OutRow { s_name: row.s_name, s_address: row.s_address }) } else { 0 });
        } else { lemma_sum_zero(db.supplier, |row: SupplierRow| joint(key, row)); }
    }
    lemma_sum_swap(keys, db.supplier, joint, by_key, by_row);
    assert forall|index: int| #![trigger db.supplier[index]] 0 <= index < db.supplier.len() implies by_row(db.supplier[index]) ==
        if db.supplier[index].s_nationkey == nation && in_1(db, parameters, db.supplier[index].s_suppkey) {
            weight(OutRow { s_name: db.supplier[index].s_name, s_address: db.supplier[index].s_address })
        } else { 0int } by {
        let row = db.supplier[index];
        let per_key = |key: i64| joint(key, row);
        if row.s_nationkey == nation && in_1(db, parameters, row.s_suppkey) {
            let key = row.s_suppkey as i64;
            assert(key as int == row.s_suppkey);
            let occurrence = |item: SupplierRow| if item.s_nationkey == nation && item.s_suppkey == key && item.s_name == row.s_name && item.s_address == row.s_address { 1int } else { 0int };
            lemma_sum_pos(db.supplier, occurrence, index);
            assert(supplier_count(suppliers, nation, key, row.s_name, row.s_address) > 0);
            assert(bucket.lookup(key).is_some());
            assert(keys.contains(key));
            let selected = choose|selected: int| #![trigger keys[selected]] 0 <= selected < keys.len() && keys[selected] == key;
            assert forall|other: int| #![trigger per_key(keys[other])] 0 <= other < keys.len() && other != selected implies per_key(keys[other]) == 0 by {
                assert(keys[other] != keys[selected]);
            }
            lemma_sum_single_match(keys, per_key, selected);
        } else { lemma_sum_zero(keys, per_key); }
    }
    lemma_sum_ext(db.supplier, by_row, |row: SupplierRow| if row.s_nationkey == nation && in_1(db, parameters, row.s_suppkey) { weight(OutRow { s_name: row.s_name, s_address: row.s_address }) } else { 0int });
}
pub proof fn lemma_missing_bucket(db: Db, parameters: Params, suppliers: Suppliers, nation: i64, weight: spec_fn(OutRow) -> int)
    requires supplier_correct(db, suppliers), domain_valid(db), suppliers.lookup(nation).is_none(),
    ensures nation_weight(db, parameters, nation as int, weight) == 0,
{
    assert forall|index: int| #![trigger db.supplier[index]] 0 <= index < db.supplier.len() implies db.supplier[index].s_nationkey != nation by {
        let row = db.supplier[index];
        if row.s_nationkey == nation {
            let key = row.s_suppkey as i64;
            let occurrence = |item: SupplierRow| if item.s_nationkey == nation && item.s_suppkey == key && item.s_name == row.s_name && item.s_address == row.s_address { 1int } else { 0int };
            lemma_sum_pos(db.supplier, occurrence, index);
            assert(supplier_count(suppliers, nation, key, row.s_name, row.s_address) == 0);
        }
    }
    lemma_sum_zero(db.supplier, |row: SupplierRow| if row.s_nationkey == nation && in_1(db, parameters, row.s_suppkey) { weight(OutRow { s_name: row.s_name, s_address: row.s_address }) } else { 0int });
}
}
