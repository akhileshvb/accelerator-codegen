use vstd::prelude::*;
use verus_common::sums::*;
use verus_common::sqlfns::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::storage::*;
use crate::values::*;
use crate::meaning::*;
use crate::aggregate::prune_years;

verus! {

broadcast use vstd::seq_lib::group_seq_properties;
pub fn has_key(keys: &[i64], key: i64) -> (result: bool)
    ensures result == keys@.contains(key),
{
    let mut index = 0;
    while index < keys.len()
        invariant index <= keys.len(), !keys@.take(index as int).contains(key),
        decreases keys.len() - index,
    {
        if keys[index] == key { return true; }
        index += 1;
    }
    proof { assert(keys@.take(index as int) =~= keys@); }
    false
}

pub open spec fn post_year(work: Work<Vec<Year>>, key: i64, year: int) -> int {
    if work.rows@.contains_key(key) { year_at(work.rows@[key]@, year) } else { 0int }
}

pub open spec fn order_input_weight(key: i64, year: int) -> spec_fn(OrderIn) -> int {
    |row: OrderIn| if row.o_orderkey == key && civil_year(row.o_orderdate as int) == year { 1int } else { 0int }
}

pub proof fn lemma_year_count(rows: Seq<Year>, position: int)
    requires 0 <= position < rows.len(), rows.map_values(|row: Year| row.year).no_duplicates(),
    ensures year_at(rows, rows[position].year as int) == rows[position].count as int,
{
    let years = rows.map_values(|row: Year| row.year);
    let target = rows[position].year as int;
    let weight = |row: Year| if row.year == target { row.count as int } else { 0int };
    assert forall|index: int| 0 <= index < rows.len() && index != position implies weight(#[trigger] rows[index]) == 0 by {
        assert(years[index] != years[position]);
    }
    lemma_sum_single_match(rows, weight, position);
}

pub proof fn lemma_order_input_bound(rows: Seq<OrderIn>, key: i64, year: int)
    ensures 0 <= sum(rows, order_input_weight(key, year)) <= rows.len(),
{
    lemma_sum_nonneg(rows, order_input_weight(key, year));
    lemma_sum_le(rows, order_input_weight(key, year), |row: OrderIn| 1int);
    verus_prooflib::lemma_sum_constant_one(rows);
}

#[verifier::spinoff_prover]
pub fn post_dates(keys: &[i64], rows: &[OrderIn]) -> (result: Work<Vec<Year>>)
    ensures result.valid(),
        forall|key: i64| #[trigger] keys@.contains(key) ==> result.rows@.contains_key(key),
        forall|key: i64, year: int| #[trigger] post_year(result, key, year) == sum(rows@, order_input_weight(key, year)),
        forall|key: i64| #[trigger] result.rows@.contains_key(key) ==> years_positive(result.rows@[key]@)
            && result.rows@[key]@.map_values(|row: Year| row.year).no_duplicates(),
{
    let mut work: Work<Vec<Year>> = Work::new();
    let mut index = 0;
    while index < keys.len()
        invariant index <= keys.len(),
            work.valid(),
            forall|key: i64| #[trigger] keys@.take(index as int).contains(key) ==> work.rows@.contains_key(key),
            forall|key: i64| #[trigger] work.rows@.contains_key(key) ==> work.rows@[key]@ == Seq::<Year>::empty(),
        decreases keys.len() - index,
    {
        if !work.rows.contains_key(&keys[index]) { work.put(keys[index], Vec::new()); }
        proof {
            assert(keys@.take(index as int + 1) =~= keys@.take(index as int).push(keys[index as int]));
            verus_prooflib::lemma_push_membership(keys@.take(index as int), keys[index as int]);
        }
        index += 1;
    }
    proof {
        assert(keys@.take(index as int) =~= keys@);
        assert forall|key: i64, year: int| #[trigger] post_year(work, key, year) == sum(rows@.take(0), order_input_weight(key, year)) by {
            lemma_sum_empty(rows@.take(0), order_input_weight(key, year));
            if work.rows@.contains_key(key) {
                lemma_sum_empty(work.rows@[key]@, |row: Year| if row.year == year { row.count as int } else { 0int });
            }
        }
    }
    index = 0;
    let mut processed = Ghost(0int);
    while index < rows.len()
        invariant index <= rows.len(), processed@ == index as int,
            work.valid(),
            forall|key: i64| #[trigger] keys@.contains(key) ==> work.rows@.contains_key(key),
            forall|key: i64, year: int| #[trigger] post_year(work, key, year) == sum(rows@.take(processed@), order_input_weight(key, year)),
            forall|key: i64| #[trigger] work.rows@.contains_key(key) ==> years_positive(work.rows@[key]@)
                && work.rows@[key]@.map_values(|row: Year| row.year).no_duplicates(),
        decreases rows.len() - index,
    {
        let row = rows[index];
        let ghost before = work;
        let mut years = match work.take(row.o_orderkey) { Some(years) => years, None => Vec::new() };
        let target_year = year(row.o_orderdate);
        proof {
            assert(years_positive(years@));
            assert(years@.map_values(|row: Year| row.year).no_duplicates());
            assert forall|target: int| #[trigger] year_at(years@, target) == post_year(before, row.o_orderkey, target) by {
                if !before.rows@.contains_key(row.o_orderkey) {
                    lemma_sum_empty(years@, |row: Year| if row.year == target { row.count as int } else { 0int });
                }
            }
            assert forall|position: int| 0 <= position < years.len() && (#[trigger] years[position]).year == target_year implies
                i128::MIN <= years[position].count as int + 1 <= i128::MAX by {
                lemma_year_count(years@, position);
                lemma_order_input_bound(rows@.take(processed@), row.o_orderkey, target_year as int);
            }
        }
        let changed = bump_year(&mut years, target_year, 1);
        proof { assert(changed.is_some()); }
        work.put(row.o_orderkey, years);
        proof {
            assert forall|key: i64, target: int| #[trigger] post_year(work, key, target) == sum(rows@.take(index as int + 1), order_input_weight(key, target)) by {
                lemma_sum_take(rows@, index as int, order_input_weight(key, target));
                if key != row.o_orderkey { assert(post_year(work, key, target) == post_year(before, key, target)); }
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    proof { assert(rows@.take(processed@) =~= rows@); }
    work
}

pub open spec fn order_change_weight(key: i64, year: int) -> spec_fn(OrderDelta) -> int {
    |change: OrderDelta| change.sign as int * order_input_weight(key, year)(change.row)
}

pub open spec fn post_dates_ready(work: Work<Vec<Year>>, keys: Seq<i64>, db: Db) -> bool {
    work.valid()
        && (forall|key: i64| #[trigger] keys.contains(key) ==> work.rows@.contains_key(key))
        && (forall|key: i64, year: int| keys.contains(key) ==> #[trigger] post_year(work, key, year) == dates(db, key as int, year))
        && forall|key: i64| #[trigger] work.rows@.contains_key(key) ==> years_positive(work.rows@[key]@)
            && work.rows@[key]@.map_values(|row: Year| row.year).no_duplicates()
}

pub proof fn lemma_rescan_dates(db: Db, keys: Seq<i64>, rows: Seq<OrderIn>, key: i64, year: int)
    requires rescan_orders_for_keys_ok(db, keys, rows), keys.contains(key),
    ensures sum(rows, order_input_weight(key, year)) == dates(db, key as int, year),
{
    let position = choose|position: int| 0 <= position < keys.len() && (#[trigger] keys[position]) == key;
    assert(seq_view_i64(keys)[position] == key as int);
    assert(seq_view_i64(keys).contains(key as int));
    let weight = |row: OrdersRow| if row.o_orderkey == key as int && civil_year(row.o_orderdate) == year { 1int } else { 0int };
    let filtered = |row: OrdersRow| if seq_view_i64(keys).contains(row.o_orderkey) { weight(row) } else { 0int };
    assert(sum(order_in_seq_view(rows), weight) == sum(db.orders, filtered));
    lemma_sum_map(rows, |row: OrderIn| order_in_view(row), weight, order_input_weight(key, year));
    lemma_sum_ext_all(db.orders, filtered, weight);
}

pub proof fn lemma_order_capture(db: Db, db2: Db, changes: Seq<OrderDelta>, key: i64, year: int)
    requires bag_step(db.orders, db2.orders, order_delta_seq_view(changes)),
    ensures dates(db2, key as int, year) == dates(db, key as int, year) + sum(changes, order_change_weight(key, year)),
{
    let weight = |row: OrdersRow| if row.o_orderkey == key as int && civil_year(row.o_orderdate) == year { 1int } else { 0int };
    let signed = |entry: (OrdersRow, int)| entry.1 * weight(entry.0);
    assert(sum(db2.orders, weight) == sum(db.orders, weight) + sum(order_delta_seq_view(changes), signed));
    lemma_sum_map(changes, |change: OrderDelta| (order_in_view(change.row), change.sign as int), signed, order_change_weight(key, year));
}

pub fn rescan_dates(keys: &[i64], rows: &[OrderIn], Ghost(db): Ghost<Db>) -> (result: Work<Vec<Year>>)
    requires rescan_orders_for_keys_ok(db, keys@, rows@),
    ensures post_dates_ready(result, keys@, db),
{
    let work = post_dates(keys, rows);
    proof {
        assert forall|key: i64, year: int| keys@.contains(key) implies #[trigger] post_year(work, key, year) == dates(db, key as int, year) by {
            lemma_rescan_dates(db, keys@, rows@, key, year);
        }
    }
    work
}

pub open spec fn order_key_of() -> spec_fn(OrderDelta) -> i64 {
    |change: OrderDelta| change.row.o_orderkey
}

pub open spec fn order_changes_ready(work: Work<Vec<Year>>, changes: Seq<OrderDelta>) -> bool {
    work.valid()
        && (forall|key: i64| #[trigger] work.rows@.contains_key(key) <==> changes.map_values(order_key_of()).contains(key))
        && (forall|key: i64, year: int| #[trigger] post_year(work, key, year) == sum(changes, order_change_weight(key, year)))
        && forall|key: i64| #[trigger] work.rows@.contains_key(key) ==> work.rows@[key]@.map_values(|row: Year| row.year).no_duplicates()
}

#[verifier::spinoff_prover]
pub fn collect_order_changes(keys: &[i64], changes: &[OrderDelta]) -> (result: Option<Work<Vec<Year>>>)
    ensures result matches Some(work) ==> order_changes_ready(work, changes@)
        && forall|key: i64| #[trigger] work.rows@.contains_key(key) ==> keys@.contains(key),
{
    let mut work: Work<Vec<Year>> = Work::new();
    proof {
        assert forall|key: i64, year: int| #[trigger] post_year(work, key, year) == sum(changes@.take(0), order_change_weight(key, year)) by {
            lemma_sum_empty(changes@.take(0), order_change_weight(key, year));
        }
    }
    let mut index = 0;
    let mut processed = Ghost(0int);
    while index < changes.len()
        invariant index <= changes.len(), processed@ == index as int, work.valid(),
            forall|key: i64| #[trigger] work.rows@.contains_key(key) <==> changes@.take(processed@).map_values(order_key_of()).contains(key),
            forall|key: i64, year: int| #[trigger] post_year(work, key, year) == sum(changes@.take(processed@), order_change_weight(key, year)),
            forall|key: i64| #[trigger] work.rows@.contains_key(key) ==> keys@.contains(key)
                && work.rows@[key]@.map_values(|row: Year| row.year).no_duplicates(),
        decreases changes.len() - index,
    {
        let change = changes[index];
        let key = change.row.o_orderkey;
        if !has_key(keys, key) { return None; }
        let ghost before = work;
        let mut years = match work.take(key) { Some(years) => years, None => Vec::new() };
        proof {
            assert(years@.map_values(|row: Year| row.year).no_duplicates());
            assert forall|target: int| #[trigger] year_at(years@, target) == post_year(before, key, target) by {
                if !before.rows@.contains_key(key) {
                    lemma_sum_empty(years@, |row: Year| if row.year == target { row.count as int } else { 0int });
                }
            }
        }
        bump_year(&mut years, year(change.row.o_orderdate), change.sign as i128)?;
        work.put(key, years);
        proof {
            assert forall|target: i64, target_year: int| #[trigger] post_year(work, target, target_year) == sum(changes@.take(index as int + 1), order_change_weight(target, target_year)) by {
                lemma_sum_take(changes@, index as int, order_change_weight(target, target_year));
                if target != key { assert(post_year(work, target, target_year) == post_year(before, target, target_year)); }
            }
            assert(changes@.take(index as int + 1).map_values(order_key_of()) =~= changes@.take(index as int).map_values(order_key_of()).push(key));
            verus_prooflib::lemma_push_membership(changes@.take(index as int).map_values(order_key_of()), key);
        }
        index += 1;
        processed = Ghost(index as int);
    }
    proof { assert(changes@.take(processed@) =~= changes@); }
    Some(work)
}

}
