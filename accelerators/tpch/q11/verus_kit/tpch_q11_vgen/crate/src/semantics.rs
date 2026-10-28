use crate::packed::{self, Packed, PackedCursor};
use vstd::prelude::*;
use verus_prooflib::ordered::KeyView;
use verus_prooflib::wide::Wide;
use verus_common::sums::*;
use verus_prooflib::{lemma_scan_property, lemma_bag_step_property, lemma_flat_map_contains};
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::inventory::*;
use crate::model::*;

verus! {

pub open spec fn part_bounded(row: PartsuppRow) -> bool {
    i64::MIN <= row.ps_partkey <= i64::MAX && i64::MIN <= row.ps_suppkey <= i64::MAX
}

pub open spec fn supplier_bounded(row: SupplierRow) -> bool {
    i64::MIN <= row.s_suppkey <= i64::MAX && i64::MIN <= row.s_nationkey <= i64::MAX
}

pub open spec fn nation_bounded(row: NationRow) -> bool {
    i64::MIN <= row.n_nationkey <= i64::MAX
}

pub open spec fn db_bounded(db: Db) -> bool {
    &&& forall|position: int| 0 <= position < db.partsupp.len() ==> part_bounded(#[trigger] db.partsupp[position])
    &&& forall|position: int| 0 <= position < db.supplier.len() ==> supplier_bounded(#[trigger] db.supplier[position])
    &&& forall|position: int| 0 <= position < db.nation.len() ==> nation_bounded(#[trigger] db.nation[position])
}

pub proof fn lemma_build_ranges(db: Db, parts: Seq<PartsuppInput>, suppliers: Seq<SupplierInput>, nations: Seq<NationInput>)
    requires scan_partsupp_profiles_ok(db, parts), scan_supplier_links_ok(db, suppliers), scan_nation_names_ok(db, nations),
    ensures db_bounded(db),
{
    assert forall|position: int| 0 <= position < parts.len() implies
        part_bounded(#[trigger] partsupp_input_seq_view(parts)[position]) by {}
    assert forall|position: int| 0 <= position < suppliers.len() implies
        supplier_bounded(#[trigger] supplier_input_seq_view(suppliers)[position]) by {}
    assert forall|position: int| 0 <= position < nations.len() implies
        nation_bounded(#[trigger] nation_input_seq_view(nations)[position]) by {}
    lemma_scan_property(partsupp_input_seq_view(parts), db.partsupp, |row: PartsuppRow| part_bounded(row));
    lemma_scan_property(supplier_input_seq_view(suppliers), db.supplier, |row: SupplierRow| supplier_bounded(row));
    lemma_scan_property(nation_input_seq_view(nations), db.nation, |row: NationRow| nation_bounded(row));
}

pub proof fn lemma_profile_scan(data: Data, db: Db, parts: Seq<PartsuppInput>)
    requires scan_partsupp_profiles_ok(db, parts), profile_storage_ok(data.profiles),
        forall|supplier: i64, part: i64| #[trigger] profile_at(data.profiles, supplier as int, part as int).count as int
            == sum(parts, |row: PartsuppInput| if row.ps_suppkey == supplier && row.ps_partkey == part { 1int } else { 0 }),
        forall|supplier: i64, part: i64| #[trigger] profile_at(data.profiles, supplier as int, part as int).value as int
            == sum(parts, |row: PartsuppInput| if row.ps_suppkey == supplier && row.ps_partkey == part { row.ps_availqty as int * row.ps_supplycost as int } else { 0 }),
    ensures data.profiles_ok(db),
{
    assert forall|supplier: i64, part: i64| #[trigger] profile_at(data.profiles, supplier as int, part as int).count as int
        == p_count(db, supplier as int, part as int)
        && profile_at(data.profiles, supplier as int, part as int).value as int == p_value(db, supplier as int, part as int) by {
        let count = |row: PartsuppRow| if row.ps_suppkey == supplier && row.ps_partkey == part { 1int } else { 0 };
        let value = |row: PartsuppRow| if row.ps_suppkey == supplier && row.ps_partkey == part { row.ps_availqty * row.ps_supplycost } else { 0 };
        lemma_sum_map(parts, |row: PartsuppInput| partsupp_input_view(row), count,
            |row: PartsuppInput| if row.ps_suppkey == supplier && row.ps_partkey == part { 1int } else { 0 });
        lemma_sum_map(parts, |row: PartsuppInput| partsupp_input_view(row), value,
            |row: PartsuppInput| if row.ps_suppkey == supplier && row.ps_partkey == part { row.ps_availqty as int * row.ps_supplycost as int } else { 0 });
        assert(sum(partsupp_input_seq_view(parts), count) == sum(db.partsupp, count));
        assert(sum(partsupp_input_seq_view(parts), value) == sum(db.partsupp, value));
    }
}

pub proof fn lemma_supplier_scan(data: Data, db: Db, suppliers: Seq<SupplierInput>)
    requires scan_supplier_links_ok(db, suppliers), nonzero_counts(data.suppliers), nonzero_counts(data.suppliers_reverse),
        counts_layout(data.suppliers, true, true), counts_layout(data.suppliers_reverse, true, true),
        forall|supplier: i64, nation: i64| #[trigger] count_at(data.suppliers, numeric_key(supplier as int), numeric_key(nation as int))
            == sum(suppliers, |row: SupplierInput| if row.s_suppkey == supplier && row.s_nationkey == nation { 1int } else { 0 }),
        forall|supplier: i64, nation: i64| #[trigger] count_at(data.suppliers_reverse, numeric_key(nation as int), numeric_key(supplier as int))
            == count_at(data.suppliers, numeric_key(supplier as int), numeric_key(nation as int)),
    ensures data.suppliers_ok(db),
{
    assert forall|supplier: i64, nation: i64| #[trigger] count_at(data.suppliers, numeric_key(supplier as int), numeric_key(nation as int))
        == s_count(db, supplier as int, nation as int)
        && count_at(data.suppliers_reverse, numeric_key(nation as int), numeric_key(supplier as int)) == s_count(db, supplier as int, nation as int) by {
        let count = |row: SupplierRow| if row.s_suppkey == supplier && row.s_nationkey == nation { 1int } else { 0 };
        lemma_sum_map(suppliers, |row: SupplierInput| supplier_input_view(row), count,
            |row: SupplierInput| if row.s_suppkey == supplier && row.s_nationkey == nation { 1int } else { 0 });
        assert(sum(supplier_input_seq_view(suppliers), count) == sum(db.supplier, count));
    }
}

pub proof fn lemma_nation_scan(data: Data, db: Db, nations: Seq<NationInput>)
    requires scan_nation_names_ok(db, nations), nonzero_counts(data.nations), nonzero_counts(data.nations_reverse),
        counts_layout(data.nations, true, false), counts_layout(data.nations_reverse, false, true),
        forall|nation: i64, name: Seq<u8>| #[trigger] count_at(data.nations, numeric_key(nation as int), text_key(name))
            == sum(nations, |row: NationInput| if row.n_nationkey == nation && row.n_name@ == name { 1int } else { 0 }),
        forall|nation: i64, name: Seq<u8>| #[trigger] count_at(data.nations_reverse, text_key(name), numeric_key(nation as int))
            == count_at(data.nations, numeric_key(nation as int), text_key(name)),
    ensures data.nations_ok(db),
{
    assert forall|nation: i64, name: Seq<u8>| #[trigger] count_at(data.nations, numeric_key(nation as int), text_key(name))
        == n_count(db, nation as int, name)
        && count_at(data.nations_reverse, text_key(name), numeric_key(nation as int)) == n_count(db, nation as int, name) by {
        let count = |row: NationRow| if row.n_nationkey == nation && row.n_name == name { 1int } else { 0 };
        lemma_sum_map(nations, |row: NationInput| nation_input_view(row), count,
            |row: NationInput| if row.n_nationkey == nation && row.n_name@ == name { 1int } else { 0 });
        assert(sum(nation_input_seq_view(nations), count) == sum(db.nation, count));
    }
}

pub proof fn lemma_apply_ranges(before: Db, after: Db, parts: Seq<PartsuppDelta>, suppliers: Seq<SupplierDelta>, nations: Seq<NationDelta>)
    requires db_bounded(before), capture_ok(before, after, nation_delta_seq_view(nations), partsupp_delta_seq_view(parts), supplier_delta_seq_view(suppliers)),
    ensures db_bounded(after),
{
    assert forall|position: int| 0 <= position < parts.len() implies
        part_bounded((#[trigger] partsupp_delta_seq_view(parts)[position]).0) by {}
    assert forall|position: int| 0 <= position < suppliers.len() implies
        supplier_bounded((#[trigger] supplier_delta_seq_view(suppliers)[position]).0) by {}
    assert forall|position: int| 0 <= position < nations.len() implies
        nation_bounded((#[trigger] nation_delta_seq_view(nations)[position]).0) by {}
    lemma_bag_step_property(before.partsupp, after.partsupp, partsupp_delta_seq_view(parts), |row: PartsuppRow| part_bounded(row));
    lemma_bag_step_property(before.supplier, after.supplier, supplier_delta_seq_view(suppliers), |row: SupplierRow| supplier_bounded(row));
    lemma_bag_step_property(before.nation, after.nation, nation_delta_seq_view(nations), |row: NationRow| nation_bounded(row));
}

pub proof fn lemma_partsupp_capture(before: Db, after: Db, changes: Seq<PartsuppDelta>, supplier: i64, part: i64)
    requires bag_step(before.partsupp, after.partsupp, partsupp_delta_seq_view(changes)),
    ensures p_count(after, supplier as int, part as int) == p_count(before, supplier as int, part as int)
            + crate::maintenance::p_delta_count(changes, supplier as int, part as int),
        p_value(after, supplier as int, part as int) == p_value(before, supplier as int, part as int)
            + crate::maintenance::p_delta_value(changes, supplier as int, part as int),
{
    let count = |row: PartsuppRow| if row.ps_suppkey == supplier && row.ps_partkey == part { 1int } else { 0 };
    let price = |row: PartsuppRow| if row.ps_suppkey == supplier && row.ps_partkey == part { row.ps_availqty * row.ps_supplycost } else { 0 };
    let signed_count = |change: (PartsuppRow, int)| change.1 * count(change.0);
    let signed_price = |change: (PartsuppRow, int)| change.1 * price(change.0);
    let project = |change: PartsuppDelta| (partsupp_input_view(change.row), change.sign as int);
    assert(sum(after.partsupp, count) == sum(before.partsupp, count) + sum(partsupp_delta_seq_view(changes), signed_count));
    assert(sum(after.partsupp, price) == sum(before.partsupp, price) + sum(partsupp_delta_seq_view(changes), signed_price));
    let raw_count = |change: PartsuppDelta| if change.row.ps_suppkey == supplier && change.row.ps_partkey == part { change.sign as int } else { 0 };
    let raw_price = |change: PartsuppDelta| if change.row.ps_suppkey == supplier && change.row.ps_partkey == part { change.sign as int * change.row.ps_availqty as int * change.row.ps_supplycost as int } else { 0 };
    assert((|change: PartsuppDelta| signed_count(project(change))) =~= raw_count);
    assert forall|change: PartsuppDelta| #[trigger] signed_price(project(change)) == raw_price(change) by {
        if change.row.ps_suppkey == supplier && change.row.ps_partkey == part {
            assert(change.sign as int * (change.row.ps_availqty as int * change.row.ps_supplycost as int)
                == change.sign as int * change.row.ps_availqty as int * change.row.ps_supplycost as int) by (nonlinear_arith);
        }
    }
    lemma_sum_ext_all(changes, |change: PartsuppDelta| signed_price(project(change)), raw_price);
    lemma_sum_map(changes, project, signed_count, raw_count);
    lemma_sum_map(changes, project, signed_price, raw_price);
}

pub proof fn lemma_group_key_bounded(db: Db, name: Seq<u8>, part: int)
    requires db_bounded(db), group_count(db, params0(name), part) > 0,
    ensures i64::MIN <= part <= i64::MAX,
{
    let parameter = params0(name);
    let joined = tuples(db, parameter);
    assert forall|position: int| 0 <= position < joined.len() implies
        part_bounded((#[trigger] joined[position]).partsupp) by {
        let tuple = joined[position];
        assert(joined.contains(tuple));
        lemma_flat_map_contains(db.partsupp, |row: PartsuppRow| tuples_1(db, parameter, row), tuple);
        let part_position = choose|offset: int| 0 <= offset < db.partsupp.len()
            && #[trigger] tuples_1(db, parameter, db.partsupp[offset]).contains(tuple);
        let source = db.partsupp[part_position];
        lemma_flat_map_contains(db.supplier, |row: SupplierRow| tuples_2(db, parameter, source, row), tuple);
        let supplier_position = choose|offset: int| 0 <= offset < db.supplier.len()
            && #[trigger] tuples_2(db, parameter, source, db.supplier[offset]).contains(tuple);
        let supplier = db.supplier[supplier_position];
        lemma_flat_map_contains(db.nation, |row: NationRow| tuples_3(db, parameter, source, supplier, row), tuple);
        let nation_position = choose|offset: int| 0 <= offset < db.nation.len()
            && #[trigger] tuples_3(db, parameter, source, supplier, db.nation[offset]).contains(tuple);
        assert(tuple.partsupp == source);
        assert(part_bounded(source));
    }
    if !(i64::MIN <= part <= i64::MAX) {
        lemma_sum_zero(joined, |tuple: Tuple| if tuple.partsupp.ps_partkey == part { 1int } else { 0 });
    }
}

pub proof fn lemma_supplier_capture(before: Db, after: Db, changes: Seq<SupplierDelta>, supplier: i64, nation: i64)
    requires bag_step(before.supplier, after.supplier, supplier_delta_seq_view(changes)),
    ensures s_count(after, supplier as int, nation as int) == s_count(before, supplier as int, nation as int)
        + sum(changes, |change: SupplierDelta| if change.row.s_suppkey == supplier && change.row.s_nationkey == nation { change.sign as int } else { 0 }),
{
    let weight = |row: SupplierRow| if row.s_suppkey == supplier && row.s_nationkey == nation { 1int } else { 0 };
    let signed = |change: (SupplierRow, int)| change.1 * weight(change.0);
    let project = |change: SupplierDelta| (supplier_input_view(change.row), change.sign as int);
    let raw = |change: SupplierDelta| if change.row.s_suppkey == supplier && change.row.s_nationkey == nation { change.sign as int } else { 0 };
    assert(sum(after.supplier, weight) == sum(before.supplier, weight) + sum(supplier_delta_seq_view(changes), signed));
    assert((|change: SupplierDelta| signed(project(change))) =~= raw);
    lemma_sum_map(changes, project, signed, raw);
}

pub proof fn lemma_supplier_commit(before: Data, after: Data, db: Db, db2: Db, changes: crate::model::Counts, rows: Seq<SupplierDelta>)
    requires before.suppliers_ok(db), bag_step(db.supplier, db2.supplier, supplier_delta_seq_view(rows)),
        nonzero_counts(after.suppliers), nonzero_counts(after.suppliers_reverse),
        counts_layout(after.suppliers, true, true), counts_layout(after.suppliers_reverse, true, true),
        forall|supplier: i64, nation: i64| #[trigger] count_at(changes, numeric_key(supplier as int), numeric_key(nation as int))
            == sum(rows, |change: SupplierDelta| if change.row.s_suppkey == supplier && change.row.s_nationkey == nation { change.sign as int } else { 0 }),
        forall|outer: verus_prooflib::ordered::KeyView, inner: verus_prooflib::ordered::KeyView| #[trigger] count_at(after.suppliers, outer, inner)
            == count_at(before.suppliers, outer, inner) + count_at(changes, outer, inner),
        forall|outer: verus_prooflib::ordered::KeyView, inner: verus_prooflib::ordered::KeyView| #[trigger] count_at(after.suppliers_reverse, inner, outer)
            == count_at(before.suppliers_reverse, inner, outer) + count_at(changes, outer, inner),
    ensures after.suppliers_ok(db2),
{
    assert forall|supplier: i64, nation: i64| #[trigger] count_at(after.suppliers, numeric_key(supplier as int), numeric_key(nation as int)) == s_count(db2, supplier as int, nation as int)
        && count_at(after.suppliers_reverse, numeric_key(nation as int), numeric_key(supplier as int)) == s_count(db2, supplier as int, nation as int) by {
        lemma_supplier_capture(db, db2, rows, supplier, nation);
        assert(count_at(before.suppliers, numeric_key(supplier as int), numeric_key(nation as int)) == s_count(db, supplier as int, nation as int));
    }
}

pub proof fn lemma_nation_commit(before: Data, after: Data, db: Db, db2: Db, changes: crate::model::Counts, rows: Seq<NationDelta>)
    requires before.nations_ok(db), bag_step(db.nation, db2.nation, nation_delta_seq_view(rows)),
        nonzero_counts(after.nations), nonzero_counts(after.nations_reverse),
        counts_layout(after.nations, true, false), counts_layout(after.nations_reverse, false, true),
        forall|nation: i64, name: Seq<u8>| #[trigger] count_at(changes, text_key(name), numeric_key(nation as int))
            == crate::maintenance::nation_delta_count(rows, nation, name),
        forall|name: verus_prooflib::ordered::KeyView, nation: verus_prooflib::ordered::KeyView| #[trigger] count_at(after.nations_reverse, name, nation)
            == count_at(before.nations_reverse, name, nation) + count_at(changes, name, nation),
        forall|name: verus_prooflib::ordered::KeyView, nation: verus_prooflib::ordered::KeyView| #[trigger] count_at(after.nations, nation, name)
            == count_at(before.nations, nation, name) + count_at(changes, name, nation),
    ensures after.nations_ok(db2),
{
    assert forall|nation: i64, name: Seq<u8>| #[trigger] count_at(after.nations, numeric_key(nation as int), text_key(name)) == n_count(db2, nation as int, name)
        && count_at(after.nations_reverse, text_key(name), numeric_key(nation as int)) == n_count(db2, nation as int, name) by {
        lemma_nation_capture(db, db2, rows, nation, name);
        assert(count_at(before.nations, numeric_key(nation as int), text_key(name)) == n_count(db, nation as int, name));
        assert(count_at(before.nations_reverse, text_key(name), numeric_key(nation as int)) == n_count(db, nation as int, name));
        assert(count_at(changes, text_key(name), numeric_key(nation as int))
            == crate::maintenance::nation_delta_count(rows, nation, name));
        assert(count_at(after.nations, numeric_key(nation as int), text_key(name))
            == count_at(before.nations, numeric_key(nation as int), text_key(name)) + count_at(changes, text_key(name), numeric_key(nation as int)));
        assert(count_at(after.nations_reverse, text_key(name), numeric_key(nation as int))
            == count_at(before.nations_reverse, text_key(name), numeric_key(nation as int)) + count_at(changes, text_key(name), numeric_key(nation as int)));
        assert(n_count(db2, nation as int, name) == n_count(db, nation as int, name)
            + crate::maintenance::nation_delta_count(rows, nation, name));
        assert(count_at(after.nations, numeric_key(nation as int), text_key(name)) == n_count(db2, nation as int, name));
        assert(count_at(after.nations_reverse, text_key(name), numeric_key(nation as int)) == n_count(db2, nation as int, name));
    }
}

pub proof fn lemma_nation_capture(before: Db, after: Db, changes: Seq<NationDelta>, nation: i64, name: Seq<u8>)
    requires bag_step(before.nation, after.nation, nation_delta_seq_view(changes)),
    ensures n_count(after, nation as int, name) == n_count(before, nation as int, name)
        + crate::maintenance::nation_delta_count(changes, nation, name),
{
    let weight = |row: NationRow| if row.n_nationkey == nation && row.n_name == name { 1int } else { 0 };
    let signed = |change: (NationRow, int)| change.1 * weight(change.0);
    let project = |change: NationDelta| (nation_input_view(change.row), change.sign as int);
    let raw = crate::maintenance::nation_delta_weight(nation, name);
    assert(sum(after.nation, weight) == sum(before.nation, weight) + sum(nation_delta_seq_view(changes), signed));
    assert((|change: NationDelta| signed(project(change))) =~= raw);
    lemma_sum_map(changes, project, signed, raw);
}

pub open spec fn params0(name: Seq<u8>) -> Params { Params { p1: name, p2: 0 } }

pub open spec fn inventory_answer(inventory: Inventory, weight: int, output: Seq<(i64, int)>, fraction: int) -> bool {
    &&& forall|position: int| 0 <= position < output.len() ==> {
        let row = #[trigger] output[position];
        &&& moment_at(inventory.groups, row.0 as int).count > 0
        &&& row.1 == weight * moment_at(inventory.groups, row.0 as int).value
        &&& inventory.total.count@ > 0
        &&& 1000000000000 * moment_at(inventory.groups, row.0 as int).value as int > inventory.total.value@ * fraction
    }
    &&& forall|left: int, right: int| 0 <= left < right < output.len() ==>
        (#[trigger] output[left]).0 != (#[trigger] output[right]).0 && output[left].1 >= output[right].1
    &&& forall|part: i64| #[trigger] moment_at(inventory.groups, part as int).count > 0
        && 1000000000000 * moment_at(inventory.groups, part as int).value as int > inventory.total.value@ * fraction ==>
        exists|position: int| 0 <= position < output.len() && (#[trigger] output[position]).0 == part
}

pub open spec fn query_group_value(db: Db, name: Seq<u8>, part: int) -> int {
    group_sum(db, params0(name), part, |tuple: Tuple| tuple.partsupp.ps_supplycost * tuple.partsupp.ps_availqty)
}

pub open spec fn supplier_weight(db: Db, name: Seq<u8>, supplier: int) -> int {
    sum(db.supplier, |row: SupplierRow| if row.s_suppkey == supplier { n_count(db, row.s_nationkey, name) } else { 0 })
}

pub open spec fn query_fold(db: Db, name: Seq<u8>, weight: spec_fn(PartsuppRow) -> int) -> int {
    sum(db.partsupp, |row: PartsuppRow| weight(row) * supplier_weight(db, name, row.ps_suppkey))
}

pub proof fn lemma_nation_fold(db: Db, name: Seq<u8>, part: PartsuppRow, supplier: SupplierRow, weight: spec_fn(PartsuppRow) -> int)
    ensures
        sum(tuples_2(db, params0(name), part, supplier), |row: Tuple| weight(row.partsupp))
            == if part.ps_suppkey == supplier.s_suppkey { weight(part) * n_count(db, supplier.s_nationkey, name) } else { 0 },
        sum(scalar_1_tuples_2(db, params0(name), part, supplier), |row: Scalar1Tuple| weight(row.partsupp_2))
            == if part.ps_suppkey == supplier.s_suppkey { weight(part) * n_count(db, supplier.s_nationkey, name) } else { 0 },
{
    let matches = |row: NationRow| if supplier.s_nationkey == row.n_nationkey && row.n_name == name { 1int } else { 0 };
    let nested = |row: NationRow| if part.ps_suppkey == supplier.s_suppkey { weight(part) * matches(row) } else { 0int };
    assert forall|row: NationRow| #[trigger] nested(row)
        == sum(tuples_3(db, params0(name), part, supplier, row), |tuple: Tuple| weight(tuple.partsupp)) by { reveal_with_fuel(sum, 2); }
    assert forall|row: NationRow| #[trigger] nested(row)
        == sum(scalar_1_tuples_3(db, params0(name), part, supplier, row), |tuple: Scalar1Tuple| weight(tuple.partsupp_2)) by { reveal_with_fuel(sum, 2); }
    verus_prooflib::lemma_flat_map_weight(db.nation, |row: NationRow| tuples_3(db, params0(name), part, supplier, row), |tuple: Tuple| weight(tuple.partsupp), nested);
    verus_prooflib::lemma_flat_map_weight(db.nation, |row: NationRow| scalar_1_tuples_3(db, params0(name), part, supplier, row), |tuple: Scalar1Tuple| weight(tuple.partsupp_2), nested);
    if part.ps_suppkey == supplier.s_suppkey {
        lemma_sum_scale(db.nation, matches, nested, weight(part));
    } else { lemma_sum_zero(db.nation, nested); }
    assert(matches =~= (|row: NationRow| if row.n_nationkey == supplier.s_nationkey && row.n_name == name { 1int } else { 0 }));
}

pub proof fn lemma_supplier_fold(db: Db, name: Seq<u8>, part: PartsuppRow, weight: spec_fn(PartsuppRow) -> int)
    ensures
        sum(tuples_1(db, params0(name), part), |row: Tuple| weight(row.partsupp)) == weight(part) * supplier_weight(db, name, part.ps_suppkey),
        sum(scalar_1_tuples_1(db, params0(name), part), |row: Scalar1Tuple| weight(row.partsupp_2)) == weight(part) * supplier_weight(db, name, part.ps_suppkey),
{
    let matches = |row: SupplierRow| if part.ps_suppkey == row.s_suppkey { n_count(db, row.s_nationkey, name) } else { 0int };
    let nested = |row: SupplierRow| if part.ps_suppkey == row.s_suppkey { weight(part) * n_count(db, row.s_nationkey, name) } else { 0int };
    assert forall|row: SupplierRow| #[trigger] nested(row) == sum(tuples_2(db, params0(name), part, row), |tuple: Tuple| weight(tuple.partsupp)) by {
        lemma_nation_fold(db, name, part, row, weight);
    }
    assert forall|row: SupplierRow| #[trigger] nested(row) == sum(scalar_1_tuples_2(db, params0(name), part, row), |tuple: Scalar1Tuple| weight(tuple.partsupp_2)) by {
        lemma_nation_fold(db, name, part, row, weight);
    }
    verus_prooflib::lemma_flat_map_weight(db.supplier, |row: SupplierRow| tuples_2(db, params0(name), part, row), |tuple: Tuple| weight(tuple.partsupp), nested);
    verus_prooflib::lemma_flat_map_weight(db.supplier, |row: SupplierRow| scalar_1_tuples_2(db, params0(name), part, row), |tuple: Scalar1Tuple| weight(tuple.partsupp_2), nested);
    lemma_sum_scale(db.supplier, matches, nested, weight(part));
    assert(matches =~= (|row: SupplierRow| if row.s_suppkey == part.ps_suppkey { n_count(db, row.s_nationkey, name) } else { 0int }));
}

pub proof fn lemma_query_fold(db: Db, name: Seq<u8>, weight: spec_fn(PartsuppRow) -> int)
    ensures sum(tuples(db, params0(name)), |tuple: Tuple| weight(tuple.partsupp)) == query_fold(db, name, weight),
        sum(scalar_1_tuples(db, params0(name)), |tuple: Scalar1Tuple| weight(tuple.partsupp_2)) == query_fold(db, name, weight),
{
    let nested = |row: PartsuppRow| weight(row) * supplier_weight(db, name, row.ps_suppkey);
    assert forall|row: PartsuppRow| #[trigger] nested(row) == sum(tuples_1(db, params0(name), row), |tuple: Tuple| weight(tuple.partsupp)) by {
        lemma_supplier_fold(db, name, row, weight);
    }
    assert forall|row: PartsuppRow| #[trigger] nested(row) == sum(scalar_1_tuples_1(db, params0(name), row), |tuple: Scalar1Tuple| weight(tuple.partsupp_2)) by {
        lemma_supplier_fold(db, name, row, weight);
    }
    verus_prooflib::lemma_flat_map_weight(db.partsupp, |row: PartsuppRow| tuples_1(db, params0(name), row), |tuple: Tuple| weight(tuple.partsupp), nested);
    verus_prooflib::lemma_flat_map_weight(db.partsupp, |row: PartsuppRow| scalar_1_tuples_1(db, params0(name), row), |tuple: Scalar1Tuple| weight(tuple.partsupp_2), nested);
}

pub proof fn lemma_singleton_nations(data: Data, db: Db, name: Seq<u8>, nation: i64, multiplicity: Wide)
    requires data.valid(db), data.names.at(text_key(name)) == Some(NameView::Singleton { nation, multiplicity }),
    ensures forall|other: i64| #[trigger] n_count(db, other as int, name) == if other == nation { multiplicity@ } else { 0int },
{
    assert(data.names_ok(db));
    assert(data.names.at(text_key(name)).is_some());
    assert(data.nations_reverse.at(text_key(name)).is_some());
    assert(data.nations_ok(db));
    let support = data.nations_reverse.at(text_key(name)).unwrap();
    support.model();
    assert(support.entries().len() == 1);
    assert(support.at(numeric_key(nation as int)).is_some() && support.at(numeric_key(nation as int)).unwrap()@ == multiplicity@);
    let position = choose|position: int| 0 <= position < support.entries().len()
        && (#[trigger] support.entries()[position]).0 == numeric_key(nation as int);
    assert(position == 0);
    assert forall|other: i64| #[trigger] n_count(db, other as int, name) == if other == nation { multiplicity@ } else { 0int } by {
        assert(count_at(data.nations, numeric_key(other as int), text_key(name)) == n_count(db, other as int, name));
        assert(count_at(data.nations_reverse, text_key(name), numeric_key(other as int)) == n_count(db, other as int, name));
        if other != nation {
            if support.at(numeric_key(other as int)).is_some() {
                let offset = choose|offset: int| 0 <= offset < support.entries().len() && (#[trigger] support.entries()[offset]).0 == numeric_key(other as int);
                assert(offset == position);
                assert(false);
            }
        }
    }
}

pub proof fn lemma_singleton_supply(data: Data, db: Db, name: Seq<u8>, nation: i64, multiplicity: Wide, supplier: int)
    requires data.valid(db), data.names.at(text_key(name)) == Some(NameView::Singleton { nation, multiplicity }),
    ensures supplier_weight(db, name, supplier) == multiplicity@ * s_count(db, supplier, nation as int),
{
    lemma_singleton_nations(data, db, name, nation, multiplicity);
    let raw = |row: SupplierRow| if row.s_suppkey == supplier && row.s_nationkey == nation { 1int } else { 0 };
    let scaled = |row: SupplierRow| multiplicity@ * raw(row);
    let joined = |row: SupplierRow| if row.s_suppkey == supplier { n_count(db, row.s_nationkey, name) } else { 0int };
    assert forall|position: int| 0 <= position < db.supplier.len() implies #[trigger] joined(db.supplier[position]) == scaled(db.supplier[position]) by {
        let row = db.supplier[position];
        assert(supplier_bounded(row));
        let bounded = row.s_nationkey as i64;
        assert(bounded as int == row.s_nationkey);
        assert(n_count(db, bounded as int, name) == if bounded == nation { multiplicity@ } else { 0 });
    }
    lemma_sum_ext(db.supplier, joined, scaled);
    lemma_sum_scale(db.supplier, raw, scaled, multiplicity@);
}

pub proof fn lemma_singleton_fold(data: Data, db: Db, name: Seq<u8>, nation: i64, multiplicity: Wide, weight: spec_fn(PartsuppRow) -> int)
    requires data.valid(db), data.names.at(text_key(name)) == Some(NameView::Singleton { nation, multiplicity }),
    ensures query_fold(db, name, weight) == multiplicity@ * sum(db.partsupp, |row: PartsuppRow| weight(row) * s_count(db, row.ps_suppkey, nation as int)),
{
    let raw = |row: PartsuppRow| weight(row) * s_count(db, row.ps_suppkey, nation as int);
    let scaled = |row: PartsuppRow| multiplicity@ * raw(row);
    let joined = |row: PartsuppRow| weight(row) * supplier_weight(db, name, row.ps_suppkey);
    assert forall|row: PartsuppRow| #[trigger] joined(row) == scaled(row) by {
        lemma_singleton_supply(data, db, name, nation, multiplicity, row.ps_suppkey);
        assert(weight(row) * (multiplicity@ * s_count(db, row.ps_suppkey, nation as int))
            == multiplicity@ * (weight(row) * s_count(db, row.ps_suppkey, nation as int))) by (nonlinear_arith);
    }
    lemma_sum_ext_all(db.partsupp, joined, scaled);
    lemma_sum_scale(db.partsupp, raw, scaled, multiplicity@);
}

pub proof fn lemma_singleton_group(data: Data, db: Db, name: Seq<u8>, nation: i64, multiplicity: Wide, part: int)
    requires data.valid(db), data.names.at(text_key(name)) == Some(NameView::Singleton { nation, multiplicity }),
    ensures group_count(db, params0(name), part) == multiplicity@ * h_count(db, nation as int, part),
        query_group_value(db, name, part) == multiplicity@ * h_value(db, nation as int, part),
{
    let count = |row: PartsuppRow| if row.ps_partkey == part { 1int } else { 0 };
    let value = |row: PartsuppRow| if row.ps_partkey == part { row.ps_supplycost * row.ps_availqty } else { 0 };
    lemma_query_fold(db, name, count);
    lemma_query_fold(db, name, value);
    lemma_singleton_fold(data, db, name, nation, multiplicity, count);
    lemma_singleton_fold(data, db, name, nation, multiplicity, value);
    assert((|tuple: Tuple| count(tuple.partsupp)) =~=
        (|tuple: Tuple| if tuple.partsupp.ps_partkey == part { 1int } else { 0 }));
    assert((|row: PartsuppRow| count(row) * s_count(db, row.ps_suppkey, nation as int)) =~=
        (|row: PartsuppRow| if row.ps_partkey == part { s_count(db, row.ps_suppkey, nation as int) } else { 0 }));
    let normalized = |row: PartsuppRow| if row.ps_partkey == part { row.ps_availqty * row.ps_supplycost * s_count(db, row.ps_suppkey, nation as int) } else { 0 };
    let weighted = |row: PartsuppRow| value(row) * s_count(db, row.ps_suppkey, nation as int);
    assert forall|row: PartsuppRow| #[trigger] weighted(row) == normalized(row) by {
        if row.ps_partkey == part {
            assert((row.ps_supplycost * row.ps_availqty) * s_count(db, row.ps_suppkey, nation as int)
                == row.ps_availqty * row.ps_supplycost * s_count(db, row.ps_suppkey, nation as int)) by (nonlinear_arith);
        }
    }
    lemma_sum_ext_all(db.partsupp, weighted, normalized);
    assert((|tuple: Tuple| value(tuple.partsupp)) =~=
        (|tuple: Tuple| if tuple.partsupp.ps_partkey == part { tuple.partsupp.ps_supplycost * tuple.partsupp.ps_availqty } else { 0int }));
    lemma_sum_ext_all(tuples(db, params0(name)), |tuple: Tuple| value(tuple.partsupp),
        |tuple: Tuple| if tuple.partsupp.ps_partkey == part { (|source: Tuple| source.partsupp.ps_supplycost * source.partsupp.ps_availqty)(tuple) } else { 0int });
    assert(query_group_value(db, name, part) == query_fold(db, name, value));
    assert(query_fold(db, name, value) == multiplicity@ * sum(db.partsupp, weighted));
    assert(h_value(db, nation as int, part) == sum(db.partsupp, normalized));
}

pub proof fn lemma_singleton_total(data: Data, db: Db, name: Seq<u8>, nation: i64, multiplicity: Wide)
    requires data.valid(db), data.names.at(text_key(name)) == Some(NameView::Singleton { nation, multiplicity }),
    ensures scalar_1_group_count(db, params0(name)) == multiplicity@ * h_total_count(db, nation as int),
        scalar_1_group_sum(db, params0(name), |tuple: Scalar1Tuple| tuple.partsupp_2.ps_supplycost * tuple.partsupp_2.ps_availqty)
            == multiplicity@ * h_total_value(db, nation as int),
{
    let one = |row: PartsuppRow| 1int;
    let price = |row: PartsuppRow| row.ps_supplycost * row.ps_availqty;
    lemma_query_fold(db, name, one);
    lemma_query_fold(db, name, price);
    lemma_singleton_fold(data, db, name, nation, multiplicity, one);
    lemma_singleton_fold(data, db, name, nation, multiplicity, price);
    assert((|tuple: Scalar1Tuple| one(tuple.partsupp_2)) =~= (|tuple: Scalar1Tuple| 1int));
    assert((|row: PartsuppRow| one(row) * s_count(db, row.ps_suppkey, nation as int)) =~=
        (|row: PartsuppRow| s_count(db, row.ps_suppkey, nation as int)));
    assert((|tuple: Scalar1Tuple| price(tuple.partsupp_2)) =~=
        (|tuple: Scalar1Tuple| tuple.partsupp_2.ps_supplycost * tuple.partsupp_2.ps_availqty));
    verus_prooflib::lemma_sum_constant_one(scalar_1_tuples(db, params0(name)));
    let weighted = |row: PartsuppRow| price(row) * s_count(db, row.ps_suppkey, nation as int);
    let normalized = |row: PartsuppRow| row.ps_availqty * row.ps_supplycost * s_count(db, row.ps_suppkey, nation as int);
    assert forall|row: PartsuppRow| #[trigger] weighted(row) == normalized(row) by {
        assert((row.ps_supplycost * row.ps_availqty) * s_count(db, row.ps_suppkey, nation as int)
            == row.ps_availqty * row.ps_supplycost * s_count(db, row.ps_suppkey, nation as int)) by (nonlinear_arith);
    }
    lemma_sum_ext_all(db.partsupp, weighted, normalized);
    assert(scalar_1_group_count(db, params0(name)) == query_fold(db, name, one));
    assert(query_fold(db, name, one) == multiplicity@ * h_total_count(db, nation as int));
    assert(scalar_1_group_sum(db, params0(name), |tuple: Scalar1Tuple| tuple.partsupp_2.ps_supplycost * tuple.partsupp_2.ps_availqty)
        == query_fold(db, name, price));
    assert(h_total_value(db, nation as int) == sum(db.partsupp, normalized));
}

pub proof fn lemma_missing_name(data: Data, db: Db, name: Seq<u8>)
    requires data.valid(db), data.names.at(text_key(name)).is_none(),
    ensures scalar_1_group_count(db, params0(name)) == 0,
{
    assert(data.names_ok(db));
    assert(data.nations_reverse.at(text_key(name)).is_none());
    assert forall|nation: i64| #[trigger] n_count(db, nation as int, name) == 0 by {
        assert(count_at(data.nations_reverse, text_key(name), numeric_key(nation as int)) == 0);
        assert(count_at(data.nations, numeric_key(nation as int), text_key(name)) == n_count(db, nation as int, name));
    }
    assert forall|supplier: int| #[trigger] supplier_weight(db, name, supplier) == 0 by {
        let weighted = |row: SupplierRow| if row.s_suppkey == supplier { n_count(db, row.s_nationkey, name) } else { 0int };
        assert forall|position: int| 0 <= position < db.supplier.len() implies #[trigger] weighted(db.supplier[position]) == 0 by {
            let row = db.supplier[position];
            assert(supplier_bounded(row));
            let bounded = row.s_nationkey as i64;
            assert(bounded as int == row.s_nationkey);
            assert(n_count(db, bounded as int, name) == 0);
        }
        lemma_sum_zero(db.supplier, weighted);
    }
    let one = |row: PartsuppRow| 1int;
    lemma_query_fold(db, name, one);
    assert((|tuple: Scalar1Tuple| one(tuple.partsupp_2)) =~= (|tuple: Scalar1Tuple| 1int));
    lemma_sum_zero(db.partsupp, |row: PartsuppRow| one(row) * supplier_weight(db, name, row.ps_suppkey));
    verus_prooflib::lemma_sum_constant_one(scalar_1_tuples(db, params0(name)));
    assert(scalar_1_group_count(db, params0(name)) == 0);
}

/// The parameters other than $1 do not select tuples.
pub proof fn lemma_params_agree(db: Db, name: Seq<u8>, fraction: int)
    ensures tuples(db, params(name, fraction)) == tuples(db, params0(name)),
        scalar_1_tuples(db, params(name, fraction)) == scalar_1_tuples(db, params0(name)),
{
    let p = params(name, fraction);
    let q = params0(name);
    assert forall|a: PartsuppRow, b: SupplierRow, c: NationRow| #[trigger] tuples_3(db, p, a, b, c) == tuples_3(db, q, a, b, c) by {}
    assert forall|a: PartsuppRow, b: SupplierRow, c: NationRow| #[trigger] scalar_1_tuples_3(db, p, a, b, c) == scalar_1_tuples_3(db, q, a, b, c) by {}
    assert forall|a: PartsuppRow, b: SupplierRow| #[trigger] tuples_2(db, p, a, b) == tuples_2(db, q, a, b) by {
        assert(db.nation.map_values(|c: NationRow| tuples_3(db, p, a, b, c)) =~= db.nation.map_values(|c: NationRow| tuples_3(db, q, a, b, c)));
    }
    assert forall|a: PartsuppRow, b: SupplierRow| #[trigger] scalar_1_tuples_2(db, p, a, b) == scalar_1_tuples_2(db, q, a, b) by {
        assert(db.nation.map_values(|c: NationRow| scalar_1_tuples_3(db, p, a, b, c)) =~= db.nation.map_values(|c: NationRow| scalar_1_tuples_3(db, q, a, b, c)));
    }
    assert forall|a: PartsuppRow| #[trigger] tuples_1(db, p, a) == tuples_1(db, q, a) by {
        assert(db.supplier.map_values(|b: SupplierRow| tuples_2(db, p, a, b)) =~= db.supplier.map_values(|b: SupplierRow| tuples_2(db, q, a, b)));
    }
    assert forall|a: PartsuppRow| #[trigger] scalar_1_tuples_1(db, p, a) == scalar_1_tuples_1(db, q, a) by {
        assert(db.supplier.map_values(|b: SupplierRow| scalar_1_tuples_2(db, p, a, b)) =~= db.supplier.map_values(|b: SupplierRow| scalar_1_tuples_2(db, q, a, b)));
    }
    assert(db.partsupp.map_values(|a: PartsuppRow| tuples_1(db, p, a)) =~= db.partsupp.map_values(|a: PartsuppRow| tuples_1(db, q, a)));
    assert(db.partsupp.map_values(|a: PartsuppRow| scalar_1_tuples_1(db, p, a)) =~= db.partsupp.map_values(|a: PartsuppRow| scalar_1_tuples_1(db, q, a)));
}

pub proof fn lemma_empty_answer(db: Db, name: Seq<u8>, fraction: int)
    requires scalar_1_group_count(db, params0(name)) == 0,
    ensures query_result(db, params(name, fraction), Seq::<OutRow>::empty()),
{
    lemma_params_agree(db, name, fraction);
    assert(scalar_1_group_count(db, params(name, fraction)) == 0);
    assert forall|part: int| !#[trigger] having(db, params(name, fraction), part) by {}
}

pub proof fn lemma_absent_inventory(data: Data, db: Db, nation: i64)
    requires data.inventories_ok(db), db_bounded(db), data.inventories.at(numeric_key(nation as int)).is_none(),
    ensures h_total_count(db, nation as int) == 0, h_total_value(db, nation as int) == 0,
{
    let weight = |row: PartsuppRow| s_count(db, row.ps_suppkey, nation as int);
    assert forall|row: PartsuppRow| #[trigger] weight(row) >= 0 by {
        lemma_sum_nonneg(db.supplier, |supplier: SupplierRow| if supplier.s_suppkey == row.ps_suppkey && supplier.s_nationkey == nation { 1int } else { 0 });
    }
    lemma_sum_nonneg(db.partsupp, weight);
    if h_total_count(db, nation as int) != 0 {
        let position = lemma_sum_nonzero_witness(db.partsupp, weight);
        let row = db.partsupp[position];
        assert(part_bounded(row));
        let bounded = row.ps_partkey as i64;
        assert(bounded as int == row.ps_partkey);
        let grouped = |other: PartsuppRow| if other.ps_partkey == row.ps_partkey { s_count(db, other.ps_suppkey, nation as int) } else { 0int };
        assert forall|offset: int| 0 <= offset < db.partsupp.len() implies #[trigger] grouped(db.partsupp[offset]) >= 0 by {
            assert(weight(db.partsupp[offset]) >= 0);
        }
        assert(grouped(db.partsupp[position]) > 0);
        lemma_sum_pos(db.partsupp, grouped, position);
        assert(h_count(db, nation as int, bounded as int) > 0);
        assert(inventory_at(data.inventories, nation as int, bounded as int).count as int == h_count(db, nation as int, bounded as int));
        assert(inventory_at(data.inventories, nation as int, bounded as int).count == 0);
    }
    let priced = |row: PartsuppRow| row.ps_availqty * row.ps_supplycost * s_count(db, row.ps_suppkey, nation as int);
    assert forall|position: int| 0 <= position < db.partsupp.len() implies priced(#[trigger] db.partsupp[position]) == 0 by {
        lemma_sum_ge_term(db.partsupp, weight, position);
        assert(weight(db.partsupp[position]) == 0);
    }
    lemma_sum_zero(db.partsupp, priced);
}

pub open spec fn exact_view(output: Seq<(i64, int)>) -> Seq<OutRow> {
    output.map_values(|row: (i64, int)| OutRow { ps_partkey: row.0 as int, value: row.1 })
}

pub proof fn lemma_execute_model(db: Db, name: Seq<u8>, fraction: int, inventory: &Inventory, weight: int, output: Seq<(i64, int)>)
    requires db_bounded(db), weight > 0, inventory_answer(*inventory, weight, output, fraction),
        forall|part: i64| #[trigger] group_count(db, params0(name), part as int)
            == weight * moment_at(inventory.groups, part as int).count as int,
        forall|part: i64| #[trigger] query_group_value(db, name, part as int)
            == weight * moment_at(inventory.groups, part as int).value as int,
        scalar_1_group_count(db, params0(name)) == weight * inventory.total.count@,
        scalar_1_group_sum(db, params0(name), |tuple: Scalar1Tuple|
            tuple.partsupp_2.ps_supplycost * tuple.partsupp_2.ps_availqty) == weight * inventory.total.value@,
    ensures query_result(db, params(name, fraction), exact_view(output)),
{
    lemma_params_agree(db, name, fraction);
    let wanted = params(name, fraction);
    let price = |tuple: Scalar1Tuple| tuple.partsupp_2.ps_supplycost * tuple.partsupp_2.ps_availqty;
    assert(scalar_1_group_count(db, wanted) == weight * inventory.total.count@);
    assert(scalar_1_group_sum(db, wanted, price) == weight * inventory.total.value@);
    assert forall|part: int| #[trigger] group_count(db, wanted, part) == group_count(db, params0(name), part) by {}
    assert forall|part: int| group_sum(db, wanted, part, |t: Tuple| t.partsupp.ps_supplycost * t.partsupp.ps_availqty)
        == #[trigger] query_group_value(db, name, part) by {}
    assert forall|position: int| 0 <= position < output.len() implies
        row_ok(db, wanted, #[trigger] exact_view(output)[position]) by {
        let part = output[position].0;
        let value = moment_at(inventory.groups, part as int).value as int;
        let count = moment_at(inventory.groups, part as int).count as int;
        assert(count > 0);
        assert(inventory.total.count@ > 0);
        assert(group_count(db, wanted, part as int) == weight * count);
        assert(query_group_value(db, name, part as int) == weight * value);
        assert(weight * count > 0) by (nonlinear_arith) requires weight > 0, count > 0;
        assert(weight * inventory.total.count@ > 0) by (nonlinear_arith)
            requires weight > 0, inventory.total.count@ > 0;
        assert(1000000000000 * (weight * value) > (weight * inventory.total.value@) * fraction) by (nonlinear_arith)
            requires weight > 0, 1000000000000 * value > inventory.total.value@ * fraction;
        assert(scalar_1_value(db, wanted) == Some((weight * inventory.total.value@) * fraction));
    }
    assert forall|left: int, right: int| 0 <= left < right < output.len() implies
        !same_group(#[trigger] exact_view(output)[left], #[trigger] exact_view(output)[right])
            && !row_lt(exact_view(output)[right], exact_view(output)[left]) by {}
    assert forall|part: int| #[trigger] group_count(db, wanted, part) > 0 && having(db, wanted, part) implies
        exists|position: int| 0 <= position < output.len() && (#[trigger] exact_view(output)[position]).ps_partkey == part by {
        lemma_group_key_bounded(db, name, part);
        let bounded = part as i64;
        assert(bounded as int == part);
        let count = moment_at(inventory.groups, part).count as int;
        let value = moment_at(inventory.groups, part).value as int;
        assert(group_count(db, wanted, bounded as int) == weight * count);
        assert(query_group_value(db, name, bounded as int) == weight * value);
        assert(count > 0) by (nonlinear_arith) requires weight > 0, weight * count > 0;
        assert(scalar_1_group_count(db, wanted) > 0);
        assert(inventory.total.count@ > 0) by (nonlinear_arith)
            requires weight > 0, weight * inventory.total.count@ > 0;
        assert(scalar_1_value(db, wanted) == Some((weight * inventory.total.value@) * fraction));
        assert(1000000000000 * (weight * value) > (weight * inventory.total.value@) * fraction);
        assert(1000000000000 * value > inventory.total.value@ * fraction) by (nonlinear_arith)
            requires weight > 0, 1000000000000 * (weight * value) > (weight * inventory.total.value@) * fraction;
        let position = choose|position: int| 0 <= position < output.len() && (#[trigger] output[position]).0 == bounded;
        assert(exact_view(output)[position].ps_partkey == part);
    }
}


pub proof fn lemma_h_count_nonneg(db: Db, nation: int, part: int)
    ensures h_count(db, nation, part) >= 0,
{
    let weight = |row: PartsuppRow| if row.ps_partkey == part { s_count(db, row.ps_suppkey, nation) } else { 0 };
    assert forall|row: PartsuppRow| #[trigger] weight(row) >= 0 by {
        lemma_sum_nonneg(db.supplier, |supplier: SupplierRow| if supplier.s_suppkey == row.ps_suppkey && supplier.s_nationkey == nation { 1int } else { 0 });
    }
    lemma_sum_nonneg(db.partsupp, weight);
}

pub proof fn lemma_group_count_nonneg(db: Db, name: Seq<u8>, part: int)
    ensures group_count(db, params0(name), part) >= 0,
{
    lemma_sum_nonneg(tuples(db, params0(name)), |t: Tuple| if t.partsupp.ps_partkey == part { 1int } else { 0 });
}

pub proof fn lemma_stored_nonneg(data: Data, db: Db, nation: i64)
    requires data.valid(db), data.inventories.at(numeric_key(nation as int)).is_some(),
    ensures nonneg(data.inventories.at(numeric_key(nation as int)).unwrap()),
{
    let inventory = data.inventories.at(numeric_key(nation as int)).unwrap();
    assert(inventory.valid());
    assert forall|key: KeyView| #[trigger] inventory.groups.at(key).is_some() implies inventory.groups.at(key).unwrap().count >= 0 by {
        let part = key.0 as i64;
        assert(key == numeric_key(part as int));
        assert(inventory_at(data.inventories, nation as int, part as int) == moment_at(inventory.groups, part as int));
        lemma_h_count_nonneg(db, nation as int, part as int);
    }
}

pub proof fn lemma_merged_nonneg(data: Data, db: Db, name: Seq<u8>, inventory: Inventory)
    requires data.valid(db), data.names.at(text_key(name)) == Some(NameView::Merged { inventory }),
    ensures nonneg(inventory),
{
    assert(data.names_ok(db));
    assert forall|key: KeyView| #[trigger] inventory.groups.at(key).is_some() implies inventory.groups.at(key).unwrap().count >= 0 by {
        let part = key.0 as i64;
        assert(key == numeric_key(part as int));
        assert(inventory.valid());
        assert(moment_at(inventory.groups, part as int).count == group_count(db, params0(name), part as int));
        lemma_group_count_nonneg(db, name, part as int);
    }
}

}
