use vstd::prelude::*;
use verus_common::sums::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::moments::*;
use crate::pages::*;
use crate::algebra::*;

verus! {

broadcast use vstd::seq_lib::group_seq_properties;

pub proof fn lemma_len_from_sum<A>(rows: Seq<A>)
    ensures sum(rows, |row: A| 1int) == rows.len(),
{
    lemma_sum_between(rows, |row: A| 1int, 1, 1);
}

pub proof fn lemma_moment_bounded(rows: Seq<CustomerRow>, predicate: spec_fn(CustomerRow) -> bool)
    requires rows.len() <= max_rows(),
        forall|index: int| 0 <= index < rows.len() && predicate(#[trigger] rows[index]) ==>
            0 <= rows[index].c_acctbal <= LIMIT,
    ensures bounded(moment_sum(rows, predicate)),
{
    lemma_sum_between(rows, |row: CustomerRow| ind(predicate(row)), 0, 1);
    lemma_sum_between(rows, |row: CustomerRow| if predicate(row) { row.c_acctbal } else { 0int }, 0, LIMIT as int);
    let n = rows.len() as int;
    assert(n * (LIMIT as int) <= max_rows() * (LIMIT as int)) by (nonlinear_arith) requires n <= max_rows();
}

pub proof fn lemma_database_bounded(db: Db)
    requires envelope(db),
    ensures
        forall|code: u32| code <= 65792 ==> bounded(#[trigger] positive_moment(db, code)),
        forall|code: u32, depth: u32, interval: u64| code <= 65792 && depth <= 50 && interval < (1u64 << depth) ==>
            bounded(#[trigger] histogram_moment(db, code, depth, interval)),
{
    assert forall|code: u32| code <= 65792 implies bounded(#[trigger] positive_moment(db, code)) by {
        lemma_moment_bounded(db.customer, |row: CustomerRow| positive_weight(row, code));
    }
    assert forall|code: u32, depth: u32, interval: u64| code <= 65792 && depth <= 50 && interval < (1u64 << depth) implies
        bounded(#[trigger] histogram_moment(db, code, depth, interval)) by {
        lemma_moment_bounded(db.customer, |row: CustomerRow| histogram_weight(db, row, code, depth, interval));
    }
}

/// Rows delivered by a scan are among the database's rows, so they satisfy what every database row of the filter does.
pub proof fn lemma_scan_rows_good<A>(database: Seq<A>, rows: Seq<A>, filter: spec_fn(A) -> bool, good: spec_fn(A) -> bool)
    requires forall|weight: spec_fn(A) -> int| #[trigger] sum(rows, weight) ==
            sum(database, |row: A| if filter(row) { weight(row) } else { 0 }),
        forall|index: int| 0 <= index < database.len() && filter(#[trigger] database[index]) ==> good(database[index]),
    ensures forall|index: int| 0 <= index < rows.len() ==> good(#[trigger] rows[index]),
{
    let bad = |row: A| ind(!good(row));
    let filtered = |row: A| if filter(row) { bad(row) } else { 0int };
    lemma_sum_zero(database, filtered);
    assert(sum(rows, bad) == 0);
    assert forall|index: int| 0 <= index < rows.len() implies good(#[trigger] rows[index]) by {
        if !good(rows[index]) { lemma_sum_pos(rows, bad, index); }
    }
}

pub proof fn lemma_scan_len<A>(database: Seq<A>, rows: Seq<A>, filter: spec_fn(A) -> bool)
    requires forall|weight: spec_fn(A) -> int| #[trigger] sum(rows, weight) ==
            sum(database, |row: A| if filter(row) { weight(row) } else { 0 }),
    ensures rows.len() <= database.len(),
{
    let one = |row: A| 1int;
    lemma_len_from_sum(rows);
    assert(sum(rows, one) == sum(database, |row: A| if filter(row) { one(row) } else { 0 }));
    lemma_sum_between(database, |row: A| if filter(row) { one(row) } else { 0 }, 0, 1);
}

pub proof fn lemma_sum_exact_len<A>(database: Seq<A>, rows: Seq<A>)
    requires forall|weight: spec_fn(A) -> int| #[trigger] sum(rows, weight) == sum(database, weight),
    ensures rows.len() == database.len(),
{
    let one = |row: A| 1int;
    lemma_len_from_sum(rows);
    lemma_len_from_sum(database);
    assert(sum(rows, one) == sum(database, one));
}


pub open spec fn in_range(row: CustomerRow) -> bool { row.c_acctbal < LIMIT }

pub open spec fn outside_weight(entry: (CustomerRow, int)) -> int { entry.1 * ind(!in_range(entry.0)) }

/// Rows out of range cancel in a transaction between two databases that hold none.
pub proof fn lemma_after_in_range(before: Seq<CustomerRow>, after: Seq<CustomerRow>, delta: Seq<(CustomerRow, int)>)
    requires bag_step(before, after, delta),
        forall|index: int| 0 <= index < before.len() ==> in_range(#[trigger] before[index]),
        sum(delta, |entry: (CustomerRow, int)| outside_weight(entry)) == 0,
    ensures forall|index: int| 0 <= index < after.len() ==> in_range(#[trigger] after[index]),
{
    let outside = |row: CustomerRow| ind(!in_range(row));
    lemma_sum_zero(before, outside);
    lemma_sum_ext_all(delta, |entry: (CustomerRow, int)| entry.1 * outside(entry.0), |entry: (CustomerRow, int)| outside_weight(entry));
    assert(sum(after, outside) == 0);
    assert forall|index: int| 0 <= index < after.len() implies in_range(#[trigger] after[index]) by {
        if !in_range(after[index]) { lemma_sum_pos(after, outside, index); }
    }
}

pub proof fn lemma_signed_kept(before: Seq<CustomerRow>, after: Seq<CustomerRow>, delta: Seq<(CustomerRow, int)>,
    predicate: spec_fn(CustomerRow) -> bool, kept: spec_fn(CustomerRow) -> bool)
    requires bag_step(before, after, delta),
        forall|row: CustomerRow| #[trigger] kept(row) == (predicate(row) && in_range(row)),
        forall|index: int| 0 <= index < before.len() ==> in_range(#[trigger] before[index]),
        sum(delta, |entry: (CustomerRow, int)| outside_weight(entry)) == 0,
    ensures signed_moment(delta, predicate) == signed_moment(delta, kept),
{
    lemma_after_in_range(before, after, delta);
    let count = |row: CustomerRow| ind(predicate(row) && !in_range(row));
    let total = |row: CustomerRow| if predicate(row) && !in_range(row) { row.c_acctbal } else { 0int };
    lemma_sum_zero(before, count);
    lemma_sum_zero(after, count);
    lemma_sum_zero(before, total);
    lemma_sum_zero(after, total);
    assert(sum(after, count) == sum(before, count) + sum(delta, |entry: (CustomerRow, int)| entry.1 * count(entry.0)));
    assert(sum(after, total) == sum(before, total) + sum(delta, |entry: (CustomerRow, int)| entry.1 * total(entry.0)));
    let all_count = |entry: (CustomerRow, int)| entry.1 * ind(predicate(entry.0));
    let all_total = |entry: (CustomerRow, int)| if predicate(entry.0) { entry.1 * entry.0.c_acctbal } else { 0int };
    let kept_count = |entry: (CustomerRow, int)| entry.1 * ind(kept(entry.0));
    let kept_total = |entry: (CustomerRow, int)| if kept(entry.0) { entry.1 * entry.0.c_acctbal } else { 0int };
    let lost_count = |entry: (CustomerRow, int)| entry.1 * count(entry.0);
    let lost_total = |entry: (CustomerRow, int)| entry.1 * total(entry.0);
    lemma_sum_add(delta, kept_count, lost_count, all_count);
    lemma_sum_add(delta, kept_total, lost_total, all_total);
    lemma_signed_weights(delta, predicate, all_count, all_total);
    lemma_signed_weights(delta, kept, kept_count, kept_total);
}


pub proof fn lemma_outside_zero(before: Seq<CustomerRow>, after: Seq<CustomerRow>, delta: Seq<(CustomerRow, int)>)
    requires bag_step(before, after, delta),
        forall|index: int| 0 <= index < before.len() ==> (#[trigger] before[index]).c_acctbal <= 999999999999999,
        forall|index: int| 0 <= index < after.len() ==> (#[trigger] after[index]).c_acctbal <= 999999999999999,
    ensures sum(delta, |entry: (CustomerRow, int)| outside_weight(entry)) == 0,
{
    let outside = |row: CustomerRow| ind(!in_range(row));
    lemma_sum_zero(before, outside);
    lemma_sum_zero(after, outside);
    assert(sum(after, outside) == sum(before, outside) + sum(delta, |entry: (CustomerRow, int)| entry.1 * outside(entry.0)));
    lemma_sum_ext_all(delta, |entry: (CustomerRow, int)| entry.1 * outside(entry.0), |entry: (CustomerRow, int)| outside_weight(entry));
}

pub proof fn lemma_rescan_rows_in_type(db: Db, keys: Seq<i64>, rows: Seq<CustomerInput>)
    requires envelope(db), rescan_positive_customers_by_key_ok(db, keys, rows),
    ensures forall|position: int| 0 <= position < rows.len() ==>
        (#[trigger] rows[position]).c_acctbal <= 999999999999999,
{
    let view = customer_input_seq_view(rows);
    let filter = |row: CustomerRow| row.c_acctbal > 0 && seq_view_i64(keys).contains(row.c_custkey);
    assert forall|weight: spec_fn(CustomerRow) -> int| #[trigger] sum(view, weight) ==
        sum(db.customer, |row: CustomerRow| if filter(row) { weight(row) } else { 0 }) by {
        assert(sum(view, weight) == sum(db.customer,
            |customer: CustomerRow| if (customer.c_acctbal > 0) && (seq_view_i64(keys).contains(customer.c_custkey)) { weight(customer) } else { 0 }));
        lemma_sum_ext_all(db.customer, |customer: CustomerRow| if (customer.c_acctbal > 0) && (seq_view_i64(keys).contains(customer.c_custkey)) { weight(customer) } else { 0 },
            |row: CustomerRow| if filter(row) { weight(row) } else { 0 });
    }
    lemma_scan_rows_good(db.customer, view, filter,
        |row: CustomerRow| row.c_acctbal <= 999999999999999);
    assert forall|position: int| 0 <= position < rows.len() implies
        (#[trigger] rows[position]).c_acctbal <= 999999999999999 by {
        assert(view[position] == customer_input_view(rows[position]));
    }
}

}
