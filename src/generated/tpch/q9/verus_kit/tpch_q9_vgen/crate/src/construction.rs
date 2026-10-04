use vstd::prelude::*;
use verus_common::sums::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::meaning::*;
use crate::values::*;
use crate::storage::*;
use crate::database::*;

verus! {

pub open spec fn pair_at(rows: Seq<Pair>, supplier: int, field: int) -> int {
    sum(rows, |row: Pair| if row.supplier == supplier { if field == 0 { row.count as int } else { row.cost as int } } else { 0int })
}

pub proof fn lemma_nation_per_supplier(db: Db, key: int, nation: int, year: int, field: int)
    ensures sum(db.supplier, |row: SupplierRow| if row.s_suppkey == key {
        if row.s_nationkey == nation { supplier_total(db, row.s_suppkey, year, field) } else { 0int }
    } else { 0int }) == supplier_count(db, key, nation) * supplier_total(db, key, year, field),
{
    let count = |row: SupplierRow| if row.s_suppkey == key && row.s_nationkey == nation { 1int } else { 0int };
    let filtered = |row: SupplierRow| if row.s_suppkey == key {
        if row.s_nationkey == nation { supplier_total(db, row.s_suppkey, year, field) } else { 0int }
    } else { 0int };
    let amount = supplier_total(db, key, year, field);
    lemma_sum_scale(db.supplier, count, filtered, amount);
    assert(amount * supplier_count(db, key, nation) == supplier_count(db, key, nation) * amount) by (nonlinear_arith);
}

pub proof fn lemma_nation_fold(db: Db, rows: Seq<SupplierIn>, totals: Work<Vec<Total>>, suppliers: Work<Vec<Number>>, nation: i64, year: int, field: int)
    requires scan_supplier_nations_ok(db, rows), supplier_totals_ready(totals, db), suppliers_ready(suppliers, db), 0 <= field <= 1,
    ensures sum(totals.keys@, supplier_work_weight(totals, suppliers, nation, year, field)) == nation_total(db, nation as int, year, field),
{
    broadcast use vstd::seq_lib::group_seq_properties;
    lemma_supplier_key_ranges(db, rows);
    let source = |row: SupplierRow| if row.s_nationkey == nation as int { supplier_total(db, row.s_suppkey, year, field) } else { 0int };
    let keys = totals.keys@.map_values(|key: i64| key as int);
    let key_of = |row: SupplierRow| row.s_suppkey;
    let per = |key: int| supplier_count(db, key, nation as int) * supplier_total(db, key, year, field);
    assert(distinct(keys));
    assert forall|index: int| 0 <= index < db.supplier.len() && source(db.supplier[index]) != 0 implies keys.contains(key_of(#[trigger] db.supplier[index])) by {
        let key = db.supplier[index].s_suppkey;
        assert((key as i64) as int == key);
        assert(work_total(totals, key as i64, year, field) == supplier_total(db, key, year, field));
        assert(totals.rows@.contains_key(key as i64));
        assert(totals.keys@.contains(key as i64));
        let position = choose|position: int| 0 <= position < totals.keys.len() && #[trigger] totals.keys[position] == key as i64;
        assert(keys[position] == key);
    }
    assert forall|key: int| #[trigger] per(key) == sum(db.supplier, |row: SupplierRow| if key_of(row) == key { source(row) } else { 0int }) by {
        lemma_nation_per_supplier(db, key, nation as int, year, field);
        lemma_sum_ext_all(db.supplier,
            |row: SupplierRow| if row.s_suppkey == key { if row.s_nationkey == nation as int { supplier_total(db, row.s_suppkey, year, field) } else { 0int } } else { 0int },
            |row: SupplierRow| if key_of(row) == key { source(row) } else { 0int });
    }
    lemma_sum_group(db.supplier, keys, key_of, source, per);
    let projected = |key: i64| per(key as int);
    lemma_sum_map(totals.keys@, |key: i64| key as int, per, projected);
    assert forall|index: int| 0 <= index < totals.keys.len() implies
        supplier_work_weight(totals, suppliers, nation, year, field)(#[trigger] totals.keys[index]) == projected(totals.keys[index]) by {
        let key = totals.keys[index];
        assert(work_number(suppliers, key, nation as int) == supplier_count(db, key as int, nation as int));
        assert(work_total(totals, key, year, field) == supplier_total(db, key as int, year, field));
    }
    lemma_sum_ext(totals.keys@, supplier_work_weight(totals, suppliers, nation, year, field), projected);
}

pub proof fn lemma_group_per_nation(db: Db, key: int, name: Seq<u8>, year: int, field: int)
    ensures sum(db.nation, |row: NationRow| if row.n_nationkey == key {
        if row.n_name == name { nation_total(db, row.n_nationkey, year, field) } else { 0int }
    } else { 0int }) == nation_count(db, key, name) * nation_total(db, key, year, field),
{
    let count = |row: NationRow| if row.n_nationkey == key && row.n_name == name { 1int } else { 0int };
    let filtered = |row: NationRow| if row.n_nationkey == key {
        if row.n_name == name { nation_total(db, row.n_nationkey, year, field) } else { 0int }
    } else { 0int };
    let amount = nation_total(db, key, year, field);
    lemma_sum_scale(db.nation, count, filtered, amount);
    assert(amount * nation_count(db, key, name) == nation_count(db, key, name) * amount) by (nonlinear_arith);
}

pub proof fn lemma_group_fold(db: Db, rows: Seq<NationIn>, totals: Work<Vec<Total>>, names: Work<Vec<Name>>, name: Seq<u8>, year: int, field: int)
    requires scan_nation_names_ok(db, rows), nation_totals_ready(totals, db), names_ready(names, db), 0 <= field <= 1,
    ensures sum(totals.keys@, nation_work_weight(totals, names, name, year, field)) == group_database(db, name, year, field),
{
    broadcast use vstd::seq_lib::group_seq_properties;
    lemma_nation_key_ranges(db, rows);
    let source = |row: NationRow| if row.n_name == name { nation_total(db, row.n_nationkey, year, field) } else { 0int };
    let keys = totals.keys@.map_values(|key: i64| key as int);
    let key_of = |row: NationRow| row.n_nationkey;
    let per = |key: int| nation_count(db, key, name) * nation_total(db, key, year, field);
    assert(distinct(keys));
    assert forall|index: int| 0 <= index < db.nation.len() && source(db.nation[index]) != 0 implies keys.contains(key_of(#[trigger] db.nation[index])) by {
        let key = db.nation[index].n_nationkey;
        assert((key as i64) as int == key);
        assert(work_total(totals, key as i64, year, field) == nation_total(db, key, year, field));
        assert(totals.rows@.contains_key(key as i64));
        assert(totals.keys@.contains(key as i64));
        let position = choose|position: int| 0 <= position < totals.keys.len() && #[trigger] totals.keys[position] == key as i64;
        assert(keys[position] == key);
    }
    assert forall|key: int| #[trigger] per(key) == sum(db.nation, |row: NationRow| if key_of(row) == key { source(row) } else { 0int }) by {
        lemma_group_per_nation(db, key, name, year, field);
        lemma_sum_ext_all(db.nation,
            |row: NationRow| if row.n_nationkey == key { if row.n_name == name { nation_total(db, row.n_nationkey, year, field) } else { 0int } } else { 0int },
            |row: NationRow| if key_of(row) == key { source(row) } else { 0int });
    }
    lemma_sum_group(db.nation, keys, key_of, source, per);
    let projected = |key: i64| per(key as int);
    lemma_sum_map(totals.keys@, |key: i64| key as int, per, projected);
    assert forall|index: int| 0 <= index < totals.keys.len() implies
        nation_work_weight(totals, names, name, year, field)(#[trigger] totals.keys[index]) == projected(totals.keys[index]) by {
        let key = totals.keys[index];
        assert(work_name(names, key, name) == nation_count(db, key as int, name));
        assert(work_total(totals, key, year, field) == nation_total(db, key as int, year, field));
    }
    lemma_sum_ext(totals.keys@, nation_work_weight(totals, names, name, year, field), projected);
}

pub open spec fn moment_at(rows: Seq<Moment>, supplier: int, year: int, field: int) -> int {
    sum(rows, |row: Moment| if row.supplier == supplier && row.year == year {
        if field == 0 { row.cell.count as int } else if field == 1 { row.cell.revenue as int } else { row.cell.quantity as int }
    } else { 0int })
}

pub proof fn lemma_pair_fold(part: Part, moment: Moment, rows: Seq<Pair>, supplier: i64, year: int, field: int)
    requires 0 <= field <= 1,
    ensures pair_fold(part, moment, rows, supplier, year, field) ==
        if moment.supplier == supplier && moment.year as int == year {
            part.green as int * if field == 0 { pair_at(rows, supplier as int, 0) * moment.cell.count as int }
                else { pair_at(rows, supplier as int, 0) * moment.cell.revenue as int - pair_at(rows, supplier as int, 1) * moment.cell.quantity as int }
        } else { 0int },
    decreases rows.len(),
{
    if rows.len() > 0 {
        lemma_pair_fold(part, moment, rows.drop_last(), supplier, year, field);
        let last = rows.last();
        assert(pair_fold(part, moment, rows, supplier, year, field)
            == pair_fold(part, moment, rows.drop_last(), supplier, year, field) + part_term(part, moment, last, supplier, year, field));
        let before_count = pair_at(rows.drop_last(), supplier as int, 0);
        let before_cost = pair_at(rows.drop_last(), supplier as int, 1);
        let count = if last.supplier == supplier { last.count as int } else { 0int };
        let cost = if last.supplier == supplier { last.cost as int } else { 0int };
        assert(pair_at(rows, supplier as int, 0) == before_count + count);
        assert(pair_at(rows, supplier as int, 1) == before_cost + cost);
        let green = part.green as int;
        let lines = moment.cell.count as int;
        let revenue = moment.cell.revenue as int;
        let quantity = moment.cell.quantity as int;
        assert(green * ((before_count + count) * lines) == green * (before_count * lines) + green * (count * lines)) by (nonlinear_arith);
        assert(green * ((before_count + count) * revenue - (before_cost + cost) * quantity)
            == green * (before_count * revenue - before_cost * quantity) + green * (count * revenue - cost * quantity)) by (nonlinear_arith);
        if moment.supplier != supplier || moment.year as int != year { assert(part_term(part, moment, last, supplier, year, field) == 0); }
    } else {
        assert(pair_at(rows, supplier as int, 0) == 0);
        assert(pair_at(rows, supplier as int, 1) == 0);
        assert(part.green as int * (0int * moment.cell.count as int) == 0) by (nonlinear_arith);
        assert(part.green as int * (0int * moment.cell.revenue as int - 0int * moment.cell.quantity as int) == 0) by (nonlinear_arith);
    }
}

pub proof fn lemma_part_fold(part: Part, rows: Seq<Moment>, supplier: i64, year: int, field: int)
    requires 0 <= field <= 1,
    ensures part_fold(part, rows, supplier, year, field) == part.green as int *
        if field == 0 { stored_pair(part, supplier as int, 0) * moment_at(rows, supplier as int, year, 0) }
        else { stored_pair(part, supplier as int, 0) * moment_at(rows, supplier as int, year, 1)
            - stored_pair(part, supplier as int, 1) * moment_at(rows, supplier as int, year, 2) },
    decreases rows.len(),
{
    if rows.len() > 0 {
        lemma_part_fold(part, rows.drop_last(), supplier, year, field);
        let last = rows.last();
        assert(part_fold(part, rows, supplier, year, field)
            == part_fold(part, rows.drop_last(), supplier, year, field) + pair_fold(part, last, part.pairs@, supplier, year, field));
        lemma_pair_fold(part, last, part.pairs@, supplier, year, field);
        assert(pair_at(part.pairs@, supplier as int, 0) == stored_pair(part, supplier as int, 0));
        assert(pair_at(part.pairs@, supplier as int, 1) == stored_pair(part, supplier as int, 1));
        let before_count = moment_at(rows.drop_last(), supplier as int, year, 0);
        let before_revenue = moment_at(rows.drop_last(), supplier as int, year, 1);
        let before_quantity = moment_at(rows.drop_last(), supplier as int, year, 2);
        let count = if last.supplier == supplier && last.year as int == year { last.cell.count as int } else { 0int };
        let revenue = if last.supplier == supplier && last.year as int == year { last.cell.revenue as int } else { 0int };
        let quantity = if last.supplier == supplier && last.year as int == year { last.cell.quantity as int } else { 0int };
        assert(moment_at(rows, supplier as int, year, 0) == before_count + count);
        assert(moment_at(rows, supplier as int, year, 1) == before_revenue + revenue);
        assert(moment_at(rows, supplier as int, year, 2) == before_quantity + quantity);
        let green = part.green as int;
        let copies = stored_pair(part, supplier as int, 0);
        let cost = stored_pair(part, supplier as int, 1);
        assert(green * (copies * (before_count + count)) == green * (copies * before_count) + green * (copies * count)) by (nonlinear_arith);
        assert(green * (copies * (before_revenue + revenue) - cost * (before_quantity + quantity))
            == green * (copies * before_revenue - cost * before_quantity) + green * (copies * revenue - cost * quantity)) by (nonlinear_arith);
    } else {
        assert(moment_at(rows, supplier as int, year, 0) == 0);
        assert(moment_at(rows, supplier as int, year, 1) == 0);
        assert(moment_at(rows, supplier as int, year, 2) == 0);
        assert(part.green as int * (stored_pair(part, supplier as int, 0) * 0int) == 0) by (nonlinear_arith);
        assert(part.green as int * (stored_pair(part, supplier as int, 0) * 0int - stored_pair(part, supplier as int, 1) * 0int) == 0) by (nonlinear_arith);
    }
}

pub open spec fn part_database(db: Db, key: int, supplier: int, year: int, field: int) -> int {
    green(db, key) * if field == 0 { pair_count(db, key, supplier) * moments(db, key, supplier, year, 0) }
        else { pair_count(db, key, supplier) * moments(db, key, supplier, year, 1)
            - pair_cost(db, key, supplier) * moments(db, key, supplier, year, 2) }
}

pub open spec fn supplier_source(db: Db, supplier: int, year: int, field: int) -> spec_fn(PartsuppRow) -> int {
    |pair: PartsuppRow| if pair.ps_suppkey == supplier {
        green(db, pair.ps_partkey) * if field == 0 { moments(db, pair.ps_partkey, supplier, year, 0) }
        else { moments(db, pair.ps_partkey, supplier, year, 1) - pair.ps_supplycost * moments(db, pair.ps_partkey, supplier, year, 2) }
    } else { 0int }
}

pub proof fn lemma_supplier_per_part(db: Db, key: int, supplier: int, year: int, field: int)
    requires 0 <= field <= 1,
    ensures sum(db.partsupp, |row: PartsuppRow| if row.ps_partkey == key { supplier_source(db, supplier, year, field)(row) } else { 0int })
        == part_database(db, key, supplier, year, field),
{
    let count_weight = |row: PartsuppRow| if row.ps_partkey == key && row.ps_suppkey == supplier { 1int } else { 0int };
    let cost_weight = |row: PartsuppRow| if row.ps_partkey == key && row.ps_suppkey == supplier { row.ps_supplycost } else { 0int };
    let count = moments(db, key, supplier, year, 0);
    let revenue = moments(db, key, supplier, year, 1);
    let quantity = moments(db, key, supplier, year, 2);
    let green_count = green(db, key);
    if field == 0 {
        let filtered = |row: PartsuppRow| if row.ps_partkey == key { supplier_source(db, supplier, year, field)(row) } else { 0int };
        assert forall|row: PartsuppRow| #[trigger] filtered(row) == (green_count * count) * count_weight(row) by {
            assert(green_count * count == (green_count * count) * 1int) by (nonlinear_arith);
        }
        lemma_sum_scale(db.partsupp, count_weight, filtered, green_count * count);
        assert((green_count * count) * pair_count(db, key, supplier) == green_count * (pair_count(db, key, supplier) * count)) by (nonlinear_arith);
    } else {
        let revenue_weight = |row: PartsuppRow| (green_count * revenue) * count_weight(row);
        let quantity_weight = |row: PartsuppRow| (green_count * quantity) * cost_weight(row);
        let filtered = |row: PartsuppRow| if row.ps_partkey == key { supplier_source(db, supplier, year, field)(row) } else { 0int };
        assert forall|row: PartsuppRow| #[trigger] filtered(row) == revenue_weight(row) - quantity_weight(row) by {
            if row.ps_partkey == key && row.ps_suppkey == supplier {
                assert(green_count * (revenue - row.ps_supplycost * quantity)
                    == (green_count * revenue) - (green_count * quantity) * row.ps_supplycost) by (nonlinear_arith);
            }
        }
        lemma_sum_scale(db.partsupp, count_weight, revenue_weight, green_count * revenue);
        lemma_sum_scale(db.partsupp, cost_weight, quantity_weight, green_count * quantity);
        lemma_sum_sub(db.partsupp, revenue_weight, quantity_weight, filtered);
        let copies = pair_count(db, key, supplier);
        let cost = pair_cost(db, key, supplier);
        assert((green_count * revenue) * copies - (green_count * quantity) * cost
            == green_count * (copies * revenue - cost * quantity)) by (nonlinear_arith);
    }
}

pub proof fn lemma_supplier_fold(db: Db, green_rows: Seq<GreenPartIn>, parts: Work<Part>, supplier: i64, year: int, field: int)
    requires scan_green_parts_ok(db, green_rows), parts_ready(parts, db), 0 <= field <= 1,
    ensures sum(parts.keys@, part_work_weight(parts, supplier, year, field)) == supplier_total(db, supplier as int, year, field),
{
    broadcast use vstd::seq_lib::group_seq_properties;
    let source = supplier_source(db, supplier as int, year, field);
    let key_of = |row: PartsuppRow| row.ps_partkey;
    let keys = parts.keys@.map_values(|key: i64| key as int);
    let per = |key: int| part_database(db, key, supplier as int, year, field);
    assert(distinct(keys));
    assert forall|index: int| 0 <= index < db.partsupp.len() && source(db.partsupp[index]) != 0 implies keys.contains(key_of(#[trigger] db.partsupp[index])) by {
        let key = db.partsupp[index].ps_partkey;
        lemma_green_support(db, key);
        if green(db, key) == 0 {
            let amount = if field == 0 { moments(db, key, supplier as int, year, 0) }
                else { moments(db, key, supplier as int, year, 1) - db.partsupp[index].ps_supplycost * moments(db, key, supplier as int, year, 2) };
            assert(0int * amount == 0) by (nonlinear_arith);
            assert(source(db.partsupp[index]) == 0);
        }
        assert(green(db, key) > 0);
        lemma_green_key_range(db, green_rows, key);
        assert((key as i64) as int == key);
        assert(parts.rows@.contains_key(key as i64));
        assert(parts.keys@.contains(key as i64));
        let position = choose|position: int| 0 <= position < parts.keys.len() && #[trigger] parts.keys[position] == key as i64;
        assert(keys[position] == key);
    }
    assert forall|key: int| #[trigger] per(key) == sum(db.partsupp, |row: PartsuppRow| if key_of(row) == key { source(row) } else { 0int }) by {
        lemma_supplier_per_part(db, key, supplier as int, year, field);
        lemma_sum_ext_all(db.partsupp,
            |row: PartsuppRow| if row.ps_partkey == key { supplier_source(db, supplier as int, year, field)(row) } else { 0int },
            |row: PartsuppRow| if key_of(row) == key { source(row) } else { 0int });
    }
    lemma_sum_group(db.partsupp, keys, key_of, source, per);
    let projected = |key: i64| per(key as int);
    lemma_sum_map(parts.keys@, |key: i64| key as int, per, projected);
    assert forall|index: int| 0 <= index < parts.keys.len() implies
        part_work_weight(parts, supplier, year, field)(#[trigger] parts.keys[index]) == projected(parts.keys[index]) by {
        let key = parts.keys[index];
        if parts.rows@.contains_key(key) {
            let part = parts.rows@[key];
            lemma_part_fold(part, part.moments@, supplier, year, field);
            assert(moment_at(part.moments@, supplier as int, year, 0) == stored_moment(part, supplier as int, year, 0));
            assert(moment_at(part.moments@, supplier as int, year, 1) == stored_moment(part, supplier as int, year, 1));
            assert(moment_at(part.moments@, supplier as int, year, 2) == stored_moment(part, supplier as int, year, 2));
            assert(work_pair(parts, key, supplier as int, 0) == pair_count(db, key as int, supplier as int));
            assert(work_pair(parts, key, supplier as int, 1) == pair_cost(db, key as int, supplier as int));
            assert(work_moment(parts, key, supplier as int, year, 0) == moments(db, key as int, supplier as int, year, 0));
            assert(work_moment(parts, key, supplier as int, year, 1) == moments(db, key as int, supplier as int, year, 1));
            assert(work_moment(parts, key, supplier as int, year, 2) == moments(db, key as int, supplier as int, year, 2));
        } else {
            lemma_green_support(db, key as int);
            assert(green(db, key as int) == 0);
        }
    }
    lemma_sum_ext(parts.keys@, part_work_weight(parts, supplier, year, field), projected);
    lemma_sum_ext_all(db.partsupp, source, |pair: PartsuppRow| if pair.ps_suppkey == supplier as int {
        green(db, pair.ps_partkey) * if field == 0 { moments(db, pair.ps_partkey, supplier as int, year, 0) }
        else { moments(db, pair.ps_partkey, supplier as int, year, 1) - pair.ps_supplycost * moments(db, pair.ps_partkey, supplier as int, year, 2) }
    } else { 0int });
}

}
