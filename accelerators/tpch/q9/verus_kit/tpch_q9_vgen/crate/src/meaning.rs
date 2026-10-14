use vstd::prelude::*;
use verus_common::sums::*;
use verus_common::sqlfns::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::values::*;
use crate::storage::*;

verus! {

pub open spec fn match_part(p: Seq<u8>) -> spec_fn(PartRow) -> bool {
    |row: PartRow| like_match(row.p_name, p)
}

/// The database restricted to the parts whose name matches `p`: the other tables are those of `db`.
pub open spec fn filter_db(db: Db, p: Seq<u8>) -> Db {
    Db { part: db.part.filter(match_part(p)), ..db }
}

pub proof fn lemma_filter_same(db: Db, p: Seq<u8>)
    ensures
        filter_db(db, p).lineitem == db.lineitem, filter_db(db, p).orders == db.orders,
        filter_db(db, p).partsupp == db.partsupp, filter_db(db, p).supplier == db.supplier,
        filter_db(db, p).nation == db.nation,
        forall|key: int, supplier: int| #[trigger] pair_count(filter_db(db, p), key, supplier) == pair_count(db, key, supplier),
        forall|key: int, supplier: int| #[trigger] pair_cost(filter_db(db, p), key, supplier) == pair_cost(db, key, supplier),
        forall|key: int, supplier: int, year: int, field: int| #[trigger] moments(filter_db(db, p), key, supplier, year, field) == moments(db, key, supplier, year, field),
        forall|key: int, nation: int| #[trigger] supplier_count(filter_db(db, p), key, nation) == supplier_count(db, key, nation),
        forall|key: int, name: Seq<u8>| #[trigger] nation_count(filter_db(db, p), key, name) == nation_count(db, key, name),
{
    let dbp = filter_db(db, p);
    assert forall|key: int, supplier: int, year: int, field: int| #[trigger] moments(dbp, key, supplier, year, field) == moments(db, key, supplier, year, field) by {
        let f = |row: LineitemRow| if row.l_partkey == key && row.l_suppkey == supplier {
            dates(dbp, row.l_orderkey, year) * line_weight(row, field)
        } else { 0 };
        let g = |row: LineitemRow| if row.l_partkey == key && row.l_suppkey == supplier {
            dates(db, row.l_orderkey, year) * line_weight(row, field)
        } else { 0 };
        assert forall|row: LineitemRow| #[trigger] f(row) == g(row) by {
            assert(dates(dbp, row.l_orderkey, year) == dates(db, row.l_orderkey, year));
        }
        lemma_sum_ext_all(db.lineitem, f, g);
    }
}

pub open spec fn green(db: Db, key: int) -> int {
    sum(db.part, green_weight(key))
}

pub open spec fn green_weight(key: int) -> spec_fn(PartRow) -> int {
    |row: PartRow| if row.p_partkey == key { 1 } else { 0 }
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

pub open spec fn name_weight(name: Seq<u8>) -> spec_fn(Name) -> int {
    |row: Name| if row.bytes@ == name { row.count as int } else { 0 }
}

pub proof fn lemma_name_at_unfold(rows: Seq<Name>, name: Seq<u8>)
    ensures name_at(rows, name) == sum(rows, name_weight(name)),
    decreases rows.len(),
{
    if rows.len() > 0 {
        lemma_name_at_unfold(rows.drop_last(), name);
    }
}

pub open spec fn total_at(rows: Seq<Total>, year: int, field: int) -> int {
    sum(rows, |row: Total| if row.year == year { if field == 0 { row.count as int } else { row.profit as int } } else { 0 })
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

pub open spec fn nation_totals_ready(work: Work<Vec<Total>>, db: Db) -> bool {
    totals_work_valid(work) && forall|key: i64, year: int, field: int| 0 <= field <= 1 ==>
        #[trigger] work_total(work, key, year, field) == nation_total(db, key as int, year, field)
}

pub open spec fn group_database(db: Db, name: Seq<u8>, year: int, field: int) -> int {
    sum(db.nation, |row: NationRow| if row.n_name == name { nation_total(db, row.n_nationkey, year, field) } else { 0int })
}

pub open spec fn names_ready(work: Work<Vec<Name>>, db: Db) -> bool {
    work.valid()
        && (forall|key: i64, name: Seq<u8>| #[trigger] work_name(work, key, name) == nation_count(db, key as int, name))
        && forall|key: i64| #[trigger] work.rows@.contains_key(key) ==> {
            let rows = work.rows@[key];
            rows.len() > 0 && names_positive(rows@) && rows@.map_values(|row: Name| row.bytes@).no_duplicates()
        }
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

pub open spec fn nation_work_weight(totals: Work<Vec<Total>>, names: Work<Vec<Name>>, target: Seq<u8>, year: int, field: int) -> spec_fn(i64) -> int {
    |key: i64| work_name(names, key, target) * work_total(totals, key, year, field)
}


pub open spec fn database_keys_fit(db: Db) -> bool {
    &&& forall|index: int| 0 <= index < db.supplier.len() ==> {
        let row = #[trigger] db.supplier[index];
        i64::MIN <= row.s_suppkey <= i64::MAX && i64::MIN <= row.s_nationkey <= i64::MAX
    }
    &&& forall|index: int| 0 <= index < db.nation.len() ==> i64::MIN <= (#[trigger] db.nation[index]).n_nationkey <= i64::MAX
}

}