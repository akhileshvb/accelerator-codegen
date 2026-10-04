use vstd::prelude::*;
use verus_common::sums::*;
use verus_common::sqlfns::*;
use crate::gen_spec::*;
use crate::gen_obligations::NationIn;
use crate::values::*;
use crate::storage::*;

verus! {

pub open spec fn green(db: Db, key: int) -> int {
    sum(db.part, green_weight(key))
}

pub open spec fn database_keys_fit(db: Db) -> bool {
    &&& forall|key: int| #[trigger] green(db, key) > 0 ==> i64::MIN <= key <= i64::MAX
    &&& forall|index: int| 0 <= index < db.supplier.len() ==> {
        let row = #[trigger] db.supplier[index];
        i64::MIN <= row.s_suppkey <= i64::MAX && i64::MIN <= row.s_nationkey <= i64::MAX
    }
    &&& forall|index: int| 0 <= index < db.nation.len() ==> i64::MIN <= (#[trigger] db.nation[index]).n_nationkey <= i64::MAX
}

pub open spec fn green_weight(key: int) -> spec_fn(PartRow) -> int {
    |row: PartRow| if row.p_partkey == key && like_match(row.p_name, seq![b'%', b'g', b'r', b'e', b'e', b'n', b'%']) { 1 } else { 0 }
}

pub open spec fn references(db: Db, key: int) -> int {
    sum(db.lineitem, reference_weight(db, key))
}

pub open spec fn reference_weight(db: Db, key: int) -> spec_fn(LineitemRow) -> int {
    |row: LineitemRow| if row.l_orderkey == key && green(db, row.l_partkey) > 0 { 1 } else { 0 }
}

pub open spec fn dates(db: Db, key: int, year: int) -> int {
    sum(db.orders, |row: OrdersRow| if row.o_orderkey == key && civil_year(row.o_orderdate) == year { 1 } else { 0 })
}

pub open spec fn pair_count(db: Db, part: int, supplier: int) -> int {
    sum(db.partsupp, |row: PartsuppRow| if row.ps_partkey == part && row.ps_suppkey == supplier { 1 } else { 0 })
}

pub open spec fn pair_cost(db: Db, part: int, supplier: int) -> int {
    sum(db.partsupp, |row: PartsuppRow| if row.ps_partkey == part && row.ps_suppkey == supplier { row.ps_supplycost } else { 0 })
}

pub open spec fn line_weight(row: LineitemRow, field: int) -> int {
    if field == 0 { 1 } else if field == 1 { row.l_extendedprice * (100 - row.l_discount) } else { row.l_quantity }
}

pub open spec fn moments(db: Db, part: int, supplier: int, year: int, field: int) -> int {
    sum(db.lineitem, |row: LineitemRow| if row.l_partkey == part && row.l_suppkey == supplier {
        dates(db, row.l_orderkey, year) * line_weight(row, field)
    } else { 0 })
}

pub open spec fn year_at(rows: Seq<Year>, year: int) -> int {
    sum(rows, |row: Year| if row.year == year { row.count as int } else { 0 })
}

pub open spec fn number_at(rows: Seq<Number>, key: int) -> int {
    sum(rows, |row: Number| if row.key == key { row.count as int } else { 0 })
}

pub open spec fn name_at(rows: Seq<Name>, name: Seq<u8>) -> int {
    sum(rows, |row: Name| if row.bytes@ == name { row.count as int } else { 0 })
}

pub open spec fn total_at(rows: Seq<Total>, year: int, field: int) -> int {
    sum(rows, |row: Total| if row.year == year { if field == 0 { row.count as int } else { row.profit as int } } else { 0 })
}

pub open spec fn stored_pair(part: Part, supplier: int, field: int) -> int {
    sum(part.pairs@, |row: Pair| if row.supplier == supplier { if field == 0 { row.count as int } else { row.cost as int } } else { 0 })
}

pub open spec fn stored_moment(part: Part, supplier: int, year: int, field: int) -> int {
    sum(part.moments@, |row: Moment| if row.supplier == supplier && row.year == year {
        if field == 0 { row.cell.count as int } else if field == 1 { row.cell.revenue as int } else { row.cell.quantity as int }
    } else { 0 })
}

pub open spec fn supplier_total(db: Db, supplier: int, year: int, field: int) -> int {
    sum(db.partsupp, |pair: PartsuppRow| if pair.ps_suppkey == supplier {
        green(db, pair.ps_partkey) * if field == 0 { moments(db, pair.ps_partkey, supplier, year, 0) }
        else { moments(db, pair.ps_partkey, supplier, year, 1) - pair.ps_supplycost * moments(db, pair.ps_partkey, supplier, year, 2) }
    } else { 0 })
}

pub open spec fn supplier_count(db: Db, supplier: int, nation: int) -> int {
    sum(db.supplier, |row: SupplierRow| if row.s_suppkey == supplier && row.s_nationkey == nation { 1 } else { 0 })
}

pub open spec fn nation_count(db: Db, nation: int, name: Seq<u8>) -> int {
    sum(db.nation, |row: NationRow| if row.n_nationkey == nation && row.n_name == name { 1 } else { 0 })
}

pub open spec fn nation_input_weight(key: i64, name: Seq<u8>) -> spec_fn(NationIn) -> int {
    |row: NationIn| if row.n_nationkey == key && row.n_name@ == name { 1int } else { 0int }
}

pub open spec fn nation_total(db: Db, nation: int, year: int, field: int) -> int {
    sum(db.supplier, |row: SupplierRow| if row.s_nationkey == nation { supplier_total(db, row.s_suppkey, year, field) } else { 0 })
}

pub open spec fn groups_at(groups: Seq<Group>, name: Seq<u8>, year: int, field: int) -> int {
    sum(groups, group_weight(name, year, field))
}

pub open spec fn group_weight(name: Seq<u8>, year: int, field: int) -> spec_fn(Group) -> int {
    |group: Group| if group.name@ == name { total_at(group.totals@, year, field) } else { 0 }
}

pub open spec fn part_valid(part: Part, db: Db, key: int) -> bool {
    &&& part.green == green(db, key)
    &&& 0 < part.green <= u64::MAX
    &&& forall|supplier: int, field: int| 0 <= field <= 1 ==>
        #[trigger] stored_pair(part, supplier, field) == if field == 0 { pair_count(db, key, supplier) } else { pair_cost(db, key, supplier) }
    &&& forall|supplier: int, year: int, field: int| 0 <= field <= 2 ==>
        #[trigger] stored_moment(part, supplier, year, field) == moments(db, key, supplier, year, field)
    &&& forall|index: int| 0 <= index < part.pairs.len() ==> #[trigger] part.pairs[index].count > 0
    &&& forall|index: int| 0 <= index < part.moments.len() ==> #[trigger] part.moments[index].cell.count > 0
    &&& part.pairs@.map_values(|row: Pair| row.supplier).no_duplicates()
    &&& part.moments@.map_values(|row: Moment| (row.supplier, row.year)).no_duplicates()
}

pub open spec fn part_unique(part: Part) -> bool {
    part.pairs@.map_values(|row: Pair| row.supplier).no_duplicates()
        && part.moments@.map_values(|row: Moment| (row.supplier, row.year)).no_duplicates()
}

pub open spec fn parts_unique(work: Work<Part>) -> bool {
    forall|key: i64| #[trigger] work.rows@.contains_key(key) ==> part_unique(work.rows@[key])
}

pub open spec fn part_pages_ready(pages: Pages<Part>, db: Db) -> bool {
    pages.valid()
        && (forall|key: i64| #[trigger] pages.at(key).is_some() <==> green(db, key as int) > 0)
        && forall|key: i64| #[trigger] pages.at(key).is_some() ==> part_valid(pages.at(key).unwrap(), db, key as int)
}

pub open spec fn totals_valid(rows: Seq<Total>) -> bool {
    rows.map_values(|row: Total| row.year).no_duplicates()
        && forall|index: int| 0 <= index < rows.len() ==> #[trigger] rows[index].count > 0
}

pub open spec fn years_positive(rows: Seq<Year>) -> bool {
    forall|index: int| 0 <= index < rows.len() ==> (#[trigger] rows[index]).count > 0
}

pub open spec fn numbers_positive(rows: Seq<Number>) -> bool {
    forall|index: int| 0 <= index < rows.len() ==> (#[trigger] rows[index]).count > 0
}

pub open spec fn names_positive(rows: Seq<Name>) -> bool {
    forall|index: int| 0 <= index < rows.len() ==> (#[trigger] rows[index]).count > 0
}

pub open spec fn pairs_positive(rows: Seq<Pair>) -> bool {
    forall|index: int| 0 <= index < rows.len() ==> (#[trigger] rows[index]).count > 0
}

pub open spec fn moments_positive(rows: Seq<Moment>) -> bool {
    forall|index: int| 0 <= index < rows.len() ==> (#[trigger] rows[index]).cell.count > 0
}

pub open spec fn totals_positive(rows: Seq<Total>) -> bool {
    forall|index: int| 0 <= index < rows.len() ==> (#[trigger] rows[index]).count > 0
}

pub open spec fn totals_work_valid(work: Work<Vec<Total>>) -> bool {
    work.valid() && forall|key: i64| #[trigger] work.rows@.contains_key(key) ==> {
        let rows = work.rows@[key];
        rows.len() > 0 && totals_valid(rows@)
    }
}

pub open spec fn work_group(work: Work<Vec<Group>>, name: Seq<u8>, year: int, field: int) -> int {
    let key = name_hash(name) as i64;
    if work.rows@.contains_key(key) { groups_at(work.rows@[key]@, name, year, field) } else { 0 }
}

pub open spec fn group_bucket_valid(rows: Seq<Group>, key: i64) -> bool {
    rows.len() > 0 && rows.map_values(|group: Group| group.name@).no_duplicates()
        && forall|index: int| 0 <= index < rows.len() ==> {
            let group = #[trigger] rows[index];
            name_hash(group.name@) == key as int && group.totals.len() > 0 && totals_valid(group.totals@)
        }
}

pub open spec fn group_entries_unique(rows: Seq<Group>) -> bool {
    rows.map_values(|group: Group| group.name@).no_duplicates()
        && forall|position: int| 0 <= position < rows.len() ==>
            (#[trigger] rows[position]).totals@.map_values(|row: Total| row.year).no_duplicates()
}

pub open spec fn group_hashes(rows: Seq<Group>, key: i64) -> bool {
    forall|position: int| 0 <= position < rows.len() ==> name_hash((#[trigger] rows[position]).name@) == key as int
}

pub open spec fn groups_work_valid(work: Work<Vec<Group>>) -> bool {
    work.valid() && forall|key: i64| #[trigger] work.rows@.contains_key(key) ==> group_bucket_valid(work.rows@[key]@, key)
}

pub open spec fn groups_work_hashed(work: Work<Vec<Group>>) -> bool {
    work.valid() && forall|key: i64| #[trigger] work.rows@.contains_key(key) ==> group_hashes(work.rows@[key]@, key)
}

pub open spec fn supplier_totals_ready(work: Work<Vec<Total>>, db: Db) -> bool {
    totals_work_valid(work) && forall|key: i64, year: int, field: int| 0 <= field <= 1 ==>
        #[trigger] work_total(work, key, year, field) == supplier_total(db, key as int, year, field)
}

pub open spec fn nation_totals_ready(work: Work<Vec<Total>>, db: Db) -> bool {
    totals_work_valid(work) && forall|key: i64, year: int, field: int| 0 <= field <= 1 ==>
        #[trigger] work_total(work, key, year, field) == nation_total(db, key as int, year, field)
}

pub open spec fn group_database(db: Db, name: Seq<u8>, year: int, field: int) -> int {
    sum(db.nation, |row: NationRow| if row.n_name == name { nation_total(db, row.n_nationkey, year, field) } else { 0int })
}

pub open spec fn parts_base(work: Work<Part>, db: Db) -> bool {
    work.valid()
        && (forall|key: i64| #[trigger] work.rows@.contains_key(key) <==> green(db, key as int) > 0)
        && forall|key: i64| #[trigger] work.rows@.contains_key(key) ==> {
            let part = work.rows@[key];
            part.green == green(db, key as int) && 0 < part.green <= u64::MAX
                && pairs_positive(part.pairs@) && moments_positive(part.moments@)
                && part.pairs@.map_values(|row: Pair| row.supplier).no_duplicates()
                && part.moments@.map_values(|row: Moment| (row.supplier, row.year)).no_duplicates()
        }
}

pub open spec fn parts_pairs(work: Work<Part>, db: Db) -> bool {
    parts_base(work, db) && forall|key: i64, supplier: int, field: int| work.rows@.contains_key(key) && 0 <= field <= 1 ==>
        #[trigger] work_pair(work, key, supplier, field) == if field == 0 { pair_count(db, key as int, supplier) } else { pair_cost(db, key as int, supplier) }
}

pub open spec fn parts_ready(work: Work<Part>, db: Db) -> bool {
    parts_pairs(work, db) && forall|key: i64, supplier: int, year: int, field: int| work.rows@.contains_key(key) && 0 <= field <= 2 ==>
        #[trigger] work_moment(work, key, supplier, year, field) == moments(db, key as int, supplier, year, field)
}

pub open spec fn orders_ready(work: Work<Order>, db: Db) -> bool {
    work.valid()
        && (forall|key: i64| #[trigger] work.rows@.contains_key(key) <==> references(db, key as int) > 0)
        && forall|key: i64| #[trigger] work.rows@.contains_key(key) ==> {
            let order = work.rows@[key];
            order.refs == references(db, key as int) && 0 < order.refs <= u64::MAX
                && years_positive(order.years@) && order.years@.map_values(|row: Year| row.year).no_duplicates()
                && forall|year: int| #[trigger] year_at(order.years@, year) == dates(db, key as int, year)
        }
}

pub open spec fn order_value_valid(order: Order, db: Db, key: int) -> bool {
    order.refs == references(db, key) && 0 < order.refs <= u64::MAX
        && years_positive(order.years@) && order.years@.map_values(|row: Year| row.year).no_duplicates()
        && forall|year: int| #[trigger] year_at(order.years@, year) == dates(db, key, year)
}

pub open spec fn order_pages_ready(pages: Pages<Order>, db: Db) -> bool {
    pages.valid()
        && (forall|key: i64| #[trigger] pages.at(key).is_some() <==> references(db, key as int) > 0)
        && forall|key: i64| #[trigger] pages.at(key).is_some() ==> order_value_valid(pages.at(key).unwrap(), db, key as int)
}

pub open spec fn suppliers_ready(work: Work<Vec<Number>>, db: Db) -> bool {
    work.valid()
        && (forall|key: i64, nation: int| #[trigger] work_number(work, key, nation) == supplier_count(db, key as int, nation))
        && forall|key: i64| #[trigger] work.rows@.contains_key(key) ==> {
            let rows = work.rows@[key];
            rows.len() > 0 && numbers_positive(rows@) && rows@.map_values(|row: Number| row.key).no_duplicates()
        }
}

pub open spec fn names_ready(work: Work<Vec<Name>>, db: Db) -> bool {
    work.valid()
        && (forall|key: i64, name: Seq<u8>| #[trigger] work_name(work, key, name) == nation_count(db, key as int, name))
        && forall|key: i64| #[trigger] work.rows@.contains_key(key) ==> {
            let rows = work.rows@[key];
            rows.len() > 0 && names_positive(rows@) && rows@.map_values(|row: Name| row.bytes@).no_duplicates()
        }
}

pub open spec fn work_green(work: Work<Part>, key: i64) -> int {
    if work.rows@.contains_key(key) { work.rows@[key].green as int } else { 0 }
}

pub open spec fn work_refs(work: Work<Order>, key: i64) -> int {
    if work.rows@.contains_key(key) { work.rows@[key].refs as int } else { 0 }
}

pub open spec fn work_year(work: Work<Order>, key: i64, year: int) -> int {
    if work.rows@.contains_key(key) { year_at(work.rows@[key].years@, year) } else { 0 }
}

pub open spec fn work_pair(work: Work<Part>, key: i64, supplier: int, field: int) -> int {
    if work.rows@.contains_key(key) { stored_pair(work.rows@[key], supplier, field) } else { 0 }
}

pub open spec fn work_moment(work: Work<Part>, key: i64, supplier: int, year: int, field: int) -> int {
    if work.rows@.contains_key(key) { stored_moment(work.rows@[key], supplier, year, field) } else { 0 }
}

pub open spec fn work_number(work: Work<Vec<Number>>, key: i64, nation: int) -> int {
    if work.rows@.contains_key(key) { number_at(work.rows@[key]@, nation) } else { 0 }
}

pub open spec fn work_name(work: Work<Vec<Name>>, key: i64, name: Seq<u8>) -> int {
    if work.rows@.contains_key(key) { name_at(work.rows@[key]@, name) } else { 0 }
}

pub open spec fn work_total(work: Work<Vec<Total>>, key: i64, year: int, field: int) -> int {
    if work.rows@.contains_key(key) { total_at(work.rows@[key]@, year, field) } else { 0 }
}

pub open spec fn page_total(pages: Pages<Vec<Total>>, key: i64, year: int, field: int) -> int {
    match pages.at(key) { Some(rows) => total_at(rows@, year, field), None => 0int }
}

pub open spec fn page_group(pages: Pages<Vec<Group>>, name: Seq<u8>, year: int, field: int) -> int {
    match pages.at(name_hash(name) as i64) { Some(groups) => groups_at(groups@, name, year, field), None => 0int }
}

pub open spec fn part_term(part: Part, moment: Moment, pair: Pair, target: i64, year: int, field: int) -> int {
    if pair.supplier == moment.supplier && moment.supplier == target && moment.year as int == year {
        part.green as int * if field == 0 { pair.count as int * moment.cell.count as int }
            else { pair.count as int * moment.cell.revenue as int - pair.cost as int * moment.cell.quantity as int }
    } else { 0 }
}

pub open spec fn pair_fold(part: Part, moment: Moment, pairs: Seq<Pair>, target: i64, year: int, field: int) -> int {
    sum(pairs, |pair: Pair| part_term(part, moment, pair, target, year, field))
}

pub open spec fn part_fold(part: Part, moments: Seq<Moment>, target: i64, year: int, field: int) -> int {
    sum(moments, |moment: Moment| pair_fold(part, moment, part.pairs@, target, year, field))
}

pub open spec fn part_work_weight(work: Work<Part>, target: i64, year: int, field: int) -> spec_fn(i64) -> int {
    |key: i64| if work.rows@.contains_key(key) {
        let part = work.rows@[key];
        part_fold(part, part.moments@, target, year, field)
    } else { 0int }
}

pub open spec fn supplier_work_weight(totals: Work<Vec<Total>>, suppliers: Work<Vec<Number>>, target: i64, year: int, field: int) -> spec_fn(i64) -> int {
    |key: i64| work_number(suppliers, key, target as int) * work_total(totals, key, year, field)
}

pub open spec fn nation_work_weight(totals: Work<Vec<Total>>, names: Work<Vec<Name>>, target: Seq<u8>, year: int, field: int) -> spec_fn(i64) -> int {
    |key: i64| work_name(names, key, target) * work_total(totals, key, year, field)
}

pub open spec fn part_prefix(part: Part, moment_index: int, pair_index: int, target: i64, year: int, field: int) -> int {
    part_fold(part, part.moments@.take(moment_index), target, year, field)
        + if 0 <= moment_index < part.moments.len() {
            pair_fold(part, part.moments[moment_index], part.pairs@.take(pair_index), target, year, field)
        } else { 0 }
}

pub proof fn lemma_part_prefix_step(part: Part, moment_index: int, pair_index: int, target: i64, year: int, field: int)
    requires 0 <= moment_index < part.moments.len(), 0 <= pair_index < part.pairs.len(),
    ensures part_prefix(part, moment_index, pair_index + 1, target, year, field)
        == part_prefix(part, moment_index, pair_index, target, year, field)
            + part_term(part, part.moments[moment_index], part.pairs[pair_index], target, year, field),
{
    lemma_sum_take(part.pairs@, pair_index, |pair: Pair| part_term(part, part.moments[moment_index], pair, target, year, field));
}

pub proof fn lemma_part_prefix_finish(part: Part, moment_index: int, target: i64, year: int, field: int)
    requires 0 <= moment_index < part.moments.len(),
    ensures part_prefix(part, moment_index, part.pairs.len() as int, target, year, field)
        == part_prefix(part, moment_index + 1, 0, target, year, field),
{
    assert(part.pairs@.take(part.pairs.len() as int) =~= part.pairs@);
    lemma_sum_take(part.moments@, moment_index, |moment: Moment| pair_fold(part, moment, part.pairs@, target, year, field));
    if moment_index + 1 < part.moments.len() {
        lemma_sum_empty(part.pairs@.take(0), |pair: Pair| part_term(part, part.moments[moment_index + 1], pair, target, year, field));
    }
}

}
