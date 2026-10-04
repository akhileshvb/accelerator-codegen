use vstd::prelude::*;
use verus_common::sums::*;
use verus_common::sqlfns::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::meaning::*;
use crate::maintenance::part_change_weight;

verus! {

pub proof fn lemma_key_ranges_step(db: Db, db2: Db, parts: Seq<PartDelta>, full: Seq<(PartRow, int)>,
    suppliers: Seq<SupplierDelta>, nations: Seq<NationDelta>)
    requires database_keys_fit(db), part_delta_ok(parts, full),
        bag_step(db.part, db2.part, full),
        bag_step(db.supplier, db2.supplier, supplier_delta_seq_view(suppliers)),
        bag_step(db.nation, db2.nation, nation_delta_seq_view(nations)),
    ensures database_keys_fit(db2),
{
    assert forall|key: int| #[trigger] green(db2, key) > 0 implies i64::MIN <= key <= i64::MAX by {
        if !(i64::MIN <= key <= i64::MAX) {
            lemma_green_support(db, key);
            assert(green(db, key) == 0);
            lemma_activity_range(db, db2, parts, full, key);
        }
    }
    let supplier_fit = |row: SupplierRow| i64::MIN <= row.s_suppkey <= i64::MAX && i64::MIN <= row.s_nationkey <= i64::MAX;
    assert forall|index: int| 0 <= index < suppliers.len() implies supplier_fit((#[trigger] supplier_delta_seq_view(suppliers)[index]).0) by {}
    verus_prooflib::lemma_bag_step_preserves_predicate(db.supplier, db2.supplier, supplier_delta_seq_view(suppliers), supplier_fit);
    let nation_fit = |row: NationRow| i64::MIN <= row.n_nationkey <= i64::MAX;
    assert forall|index: int| 0 <= index < nations.len() implies nation_fit((#[trigger] nation_delta_seq_view(nations)[index]).0) by {}
    verus_prooflib::lemma_bag_step_preserves_predicate(db.nation, db2.nation, nation_delta_seq_view(nations), nation_fit);
}

pub proof fn lemma_green_support(db: Db, key: int)
    ensures green(db, key) >= 0,
        green(db, key) > 0 <==> exists|index: int| 0 <= index < db.part.len()
            && (#[trigger] db.part[index]).p_partkey == key
            && like_match(db.part[index].p_name, seq![b'%', b'g', b'r', b'e', b'e', b'n', b'%']),
{
    let weight = green_weight(key);
    lemma_sum_nonneg(db.part, weight);
    if green(db, key) > 0 {
        let index = lemma_sum_nonzero_witness(db.part, weight);
        assert(db.part[index].p_partkey == key);
    }
    if exists|index: int| 0 <= index < db.part.len() && (#[trigger] db.part[index]).p_partkey == key
        && like_match(db.part[index].p_name, seq![b'%', b'g', b'r', b'e', b'e', b'n', b'%']) {
        let index = choose|index: int| 0 <= index < db.part.len() && (#[trigger] db.part[index]).p_partkey == key
            && like_match(db.part[index].p_name, seq![b'%', b'g', b'r', b'e', b'e', b'n', b'%']);
        lemma_sum_pos(db.part, weight, index);
    }
}

pub proof fn lemma_active_line_support(db: Db, rows: Seq<LineIn>)
    requires scan_active_lines_ok(db, rows),
    ensures forall|index: int| 0 <= index < rows.len() ==> {
        let row = #[trigger] rows[index];
        green(db, row.l_partkey as int) > 0 && references(db, row.l_orderkey as int) > 0
    },
{
    let missing = |row: LineitemRow| if green(db, row.l_partkey) > 0 && references(db, row.l_orderkey) > 0 { 0int } else { 1int };
    let filtered = |row: LineitemRow| if scan_active_lines_exists_1(db, row) { missing(row) } else { 0int };
    assert forall|index: int| 0 <= index < db.lineitem.len() implies filtered(#[trigger] db.lineitem[index]) == 0 by {
        let row = db.lineitem[index];
        lemma_green_support(db, row.l_partkey);
        if scan_active_lines_exists_1(db, row) {
            lemma_reference_support(db, row.l_orderkey);
            assert(green(db, row.l_partkey) > 0);
            assert(exists|position: int| 0 <= position < db.lineitem.len() && (#[trigger] db.lineitem[position]).l_orderkey == row.l_orderkey
                && green(db, db.lineitem[position].l_partkey) > 0);
        }
    }
    lemma_sum_zero(db.lineitem, filtered);
    assert(sum(line_in_seq_view(rows), missing) == 0);
    let projected = |row: LineIn| missing(line_in_view(row));
    lemma_sum_map(rows, |row: LineIn| line_in_view(row), missing, projected);
    assert forall|index: int| 0 <= index < rows.len() implies {
        let row = #[trigger] rows[index];
        green(db, row.l_partkey as int) > 0 && references(db, row.l_orderkey as int) > 0
    } by {
        if projected(rows[index]) != 0 { lemma_sum_pos(rows, projected, index); }
    }
}

pub proof fn lemma_scan_moment(db: Db, rows: Seq<LineIn>, orders: crate::storage::Work<crate::values::Order>, key: i64, supplier: int, year: int, field: int)
    requires scan_active_lines_ok(db, rows), orders_ready(orders, db), green(db, key as int) > 0,
    ensures sum(rows, |row: LineIn| if row.l_partkey == key && row.l_suppkey as int == supplier {
            work_year(orders, row.l_orderkey, year) * line_weight(line_in_view(row), field)
        } else { 0int }) == moments(db, key as int, supplier, year, field),
{
    lemma_active_line_support(db, rows);
    let weight = |row: LineitemRow| if row.l_partkey == key as int && row.l_suppkey == supplier {
        dates(db, row.l_orderkey, year) * line_weight(row, field)
    } else { 0int };
    let projected = |row: LineIn| weight(line_in_view(row));
    let accumulated = |row: LineIn| if row.l_partkey == key && row.l_suppkey as int == supplier {
        work_year(orders, row.l_orderkey, year) * line_weight(line_in_view(row), field)
    } else { 0int };
    let filtered = |row: LineitemRow| if scan_active_lines_exists_1(db, row) { weight(row) } else { 0int };
    assert(sum(line_in_seq_view(rows), weight) == sum(db.lineitem, filtered));
    lemma_sum_map(rows, |row: LineIn| line_in_view(row), weight, projected);
    assert forall|index: int| 0 <= index < rows.len() implies accumulated(#[trigger] rows[index]) == projected(rows[index]) by {
        let row = rows[index];
        assert(references(db, row.l_orderkey as int) > 0);
        assert(orders.rows@.contains_key(row.l_orderkey));
        assert(year_at(orders.rows@[row.l_orderkey].years@, year) == dates(db, row.l_orderkey as int, year));
    }
    lemma_sum_ext(rows, accumulated, projected);
    lemma_green_support(db, key as int);
    lemma_sum_ext_all(db.lineitem, filtered, weight);
}

pub proof fn lemma_reference_support(db: Db, key: int)
    ensures references(db, key) >= 0,
        references(db, key) > 0 <==> exists|index: int| 0 <= index < db.lineitem.len()
            && (#[trigger] db.lineitem[index]).l_orderkey == key && green(db, db.lineitem[index].l_partkey) > 0,
{
    let weight = reference_weight(db, key);
    lemma_sum_nonneg(db.lineitem, weight);
    if references(db, key) > 0 { let index = lemma_sum_nonzero_witness(db.lineitem, weight); }
    if exists|index: int| 0 <= index < db.lineitem.len() && (#[trigger] db.lineitem[index]).l_orderkey == key
        && green(db, db.lineitem[index].l_partkey) > 0 {
        let index = choose|index: int| 0 <= index < db.lineitem.len() && (#[trigger] db.lineitem[index]).l_orderkey == key
            && green(db, db.lineitem[index].l_partkey) > 0;
        lemma_sum_pos(db.lineitem, weight, index);
    }
}

pub proof fn lemma_order_filter(db: Db, row: OrdersRow)
    ensures scan_order_dates_exists_1(db, row) <==> references(db, row.o_orderkey) > 0,
{
    lemma_reference_support(db, row.o_orderkey);
    assert forall|index: int| 0 <= index < db.lineitem.len() implies
        scan_order_dates_exists_2(db, #[trigger] db.lineitem[index]) == (green(db, db.lineitem[index].l_partkey) > 0) by {
        lemma_green_support(db, db.lineitem[index].l_partkey);
    }
}

pub proof fn lemma_scan_green(db: Db, rows: Seq<GreenPartIn>, key: i64)
    requires scan_green_parts_ok(db, rows),
    ensures sum(rows, |row: GreenPartIn| if row.p_partkey == key { 1int } else { 0int }) == green(db, key as int),
{
    let weight = |part_key: int| if part_key == key as int { 1int } else { 0int };
    let filtered = |row: PartRow| if like_match(row.p_name, seq![b'%', b'g', b'r', b'e', b'e', b'n', b'%']) { weight(row.p_partkey) } else { 0int };
    assert(sum(green_part_in_seq_view(rows), weight) == sum(db.part, filtered));
    lemma_sum_map(rows, |row: GreenPartIn| green_part_in_view(row), weight,
        |row: GreenPartIn| if row.p_partkey == key { 1int } else { 0int });
    lemma_sum_ext_all(db.part, filtered, green_weight(key as int));
}

pub proof fn lemma_green_key_range(db: Db, rows: Seq<GreenPartIn>, key: int)
    requires scan_green_parts_ok(db, rows), green(db, key) > 0,
    ensures i64::MIN <= key <= i64::MAX,
{
    let weight = |part_key: int| if part_key == key { 1int } else { 0int };
    let filtered = |row: PartRow| if like_match(row.p_name, seq![b'%', b'g', b'r', b'e', b'e', b'n', b'%']) { weight(row.p_partkey) } else { 0int };
    let projected = |row: GreenPartIn| weight(row.p_partkey as int);
    assert(sum(green_part_in_seq_view(rows), weight) == sum(db.part, filtered));
    lemma_sum_map(rows, |row: GreenPartIn| green_part_in_view(row), weight, projected);
    lemma_sum_ext_all(db.part, filtered, green_weight(key));
    let index = lemma_sum_nonzero_witness(rows, projected);
    assert(rows[index].p_partkey as int == key);
}

pub proof fn lemma_scan_references(db: Db, rows: Seq<LineIn>, key: i64)
    requires scan_active_lines_ok(db, rows),
    ensures sum(rows, |row: LineIn| if row.l_orderkey == key { 1int } else { 0int }) == references(db, key as int),
{
    let weight = |row: LineitemRow| if row.l_orderkey == key as int { 1int } else { 0int };
    let filtered = |row: LineitemRow| if scan_active_lines_exists_1(db, row) { weight(row) } else { 0int };
    assert(sum(line_in_seq_view(rows), weight) == sum(db.lineitem, filtered));
    lemma_sum_map(rows, |row: LineIn| line_in_view(row), weight,
        |row: LineIn| if row.l_orderkey == key { 1int } else { 0int });
    assert forall|index: int| 0 <= index < db.lineitem.len() implies
        filtered(#[trigger] db.lineitem[index]) == reference_weight(db, key as int)(db.lineitem[index]) by {
        lemma_green_support(db, db.lineitem[index].l_partkey);
    }
    lemma_sum_ext(db.lineitem, filtered, reference_weight(db, key as int));
}

pub proof fn lemma_scan_dates(db: Db, rows: Seq<OrderIn>, key: i64, year: int)
    requires scan_order_dates_ok(db, rows), references(db, key as int) > 0,
    ensures sum(rows, |row: OrderIn| if row.o_orderkey == key && civil_year(row.o_orderdate as int) == year { 1int } else { 0int })
        == dates(db, key as int, year),
{
    let weight = |row: OrdersRow| if row.o_orderkey == key as int && civil_year(row.o_orderdate) == year { 1int } else { 0int };
    let filtered = |row: OrdersRow| if scan_order_dates_exists_1(db, row) { weight(row) } else { 0int };
    assert(sum(order_in_seq_view(rows), weight) == sum(db.orders, filtered));
    lemma_sum_map(rows, |row: OrderIn| order_in_view(row), weight,
        |row: OrderIn| if row.o_orderkey == key && civil_year(row.o_orderdate as int) == year { 1int } else { 0int });
    assert forall|index: int| 0 <= index < db.orders.len() implies filtered(#[trigger] db.orders[index]) == weight(db.orders[index]) by {
        if db.orders[index].o_orderkey == key as int { lemma_order_filter(db, db.orders[index]); }
    }
    lemma_sum_ext(db.orders, filtered, weight);
}

pub proof fn lemma_scan_pair(db: Db, rows: Seq<PartsuppIn>, key: i64, supplier: int, field: int)
    requires scan_active_partsupp_ok(db, rows), green(db, key as int) > 0, 0 <= field <= 1,
    ensures sum(rows, |row: PartsuppIn| if row.ps_partkey == key && row.ps_suppkey as int == supplier {
            if field == 0 { 1int } else { row.ps_supplycost as int }
        } else { 0int }) == if field == 0 { pair_count(db, key as int, supplier) } else { pair_cost(db, key as int, supplier) },
{
    let weight = |row: PartsuppRow| if row.ps_partkey == key as int && row.ps_suppkey == supplier {
        if field == 0 { 1int } else { row.ps_supplycost }
    } else { 0int };
    let filtered = |row: PartsuppRow| if scan_active_partsupp_exists_1(db, row) { weight(row) } else { 0int };
    assert(sum(partsupp_in_seq_view(rows), weight) == sum(db.partsupp, filtered));
    lemma_sum_map(rows, |row: PartsuppIn| partsupp_in_view(row), weight,
        |row: PartsuppIn| if row.ps_partkey == key && row.ps_suppkey as int == supplier { if field == 0 { 1int } else { row.ps_supplycost as int } } else { 0int });
    lemma_green_support(db, key as int);
    assert forall|index: int| 0 <= index < db.partsupp.len() implies filtered(#[trigger] db.partsupp[index]) == weight(db.partsupp[index]) by {}
    lemma_sum_ext(db.partsupp, filtered, weight);
    if field == 0 {
        lemma_sum_ext_all(db.partsupp, weight, |row: PartsuppRow| if row.ps_partkey == key as int && row.ps_suppkey == supplier { 1int } else { 0int });
    } else {
        lemma_sum_ext_all(db.partsupp, weight, |row: PartsuppRow| if row.ps_partkey == key as int && row.ps_suppkey == supplier { row.ps_supplycost } else { 0int });
    }
}

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

pub proof fn lemma_part_capture(db: Db, db2: Db, changes: Seq<PartDelta>, full: Seq<(PartRow, int)>, key: i64)
    requires part_delta_ok(changes, full), bag_step(db.part, db2.part, full),
    ensures green(db2, key as int) == green(db, key as int) + sum(changes, part_change_weight(key)),
{
    let before_weight = green_weight(key as int);
    let signed_weight = |entry: (PartRow, int)| entry.1 * before_weight(entry.0);
    assert(sum(db2.part, before_weight) == sum(db.part, before_weight) + sum(full, signed_weight));
    assert forall|index: int| #![trigger changes[index], full[index]] 0 <= index < changes.len() implies
        part_change_weight(key)(changes[index]) == signed_weight(full[index]) by {
        let change = changes[index];
        let entry = full[index];
        assert(change.p_partkey as int == entry.0.p_partkey);
        assert((change.is_green != 0) == verus_common::sqlfns::like_match(entry.0.p_name, seq![b'%', b'g', b'r', b'e', b'e', b'n', b'%']));
    }
    verus_prooflib::lemma_sum_aligned(changes, full, part_change_weight(key), signed_weight);
}

pub proof fn lemma_activity_range(db: Db, db2: Db, changes: Seq<PartDelta>, full: Seq<(PartRow, int)>, key: int)
    requires part_delta_ok(changes, full), bag_step(db.part, db2.part, full),
        (green(db, key) > 0) != (green(db2, key) > 0),
    ensures i64::MIN <= key <= i64::MAX,
{
    let weight = green_weight(key);
    let signed = |entry: (PartRow, int)| entry.1 * weight(entry.0);
    let projected = |change: PartDelta| if change.p_partkey as int == key && change.is_green != 0 { change.sign as int } else { 0int };
    assert(sum(db2.part, weight) == sum(db.part, weight) + sum(full, signed));
    assert forall|index: int| #![trigger changes[index], full[index]] 0 <= index < changes.len() implies projected(changes[index]) == signed(full[index]) by {}
    verus_prooflib::lemma_sum_aligned(changes, full, projected, signed);
    assert(sum(changes, projected) != 0);
    let position = lemma_sum_nonzero_witness(changes, projected);
    assert(changes[position].p_partkey as int == key);
}

}
