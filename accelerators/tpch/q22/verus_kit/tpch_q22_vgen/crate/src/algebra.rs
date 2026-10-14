use vstd::prelude::*;
use verus_common::sums::*;

verus! {

broadcast use vstd::seq_lib::group_seq_properties;

pub proof fn lemma_mapped_prefix<A, B>(rows: Seq<A>, index: int,
    view: spec_fn(A) -> B, weight: spec_fn(B) -> int)
    requires 0 <= index < rows.len(),
    ensures sum(rows.take(index + 1).map_values(view), weight) ==
        sum(rows.take(index).map_values(view), weight) + weight(view(rows[index])),
{
    let mapped = rows.map_values(view);
    assert(rows.take(index + 1).map_values(view) =~= mapped.take(index + 1));
    assert(rows.take(index).map_values(view) =~= mapped.take(index));
    lemma_sum_take(mapped, index, weight);
}

pub proof fn lemma_sum_support<A>(rows: Seq<A>, weight: spec_fn(A) -> int, predicate: spec_fn(A) -> bool)
    requires forall|index: int| 0 <= index < rows.len() ==>
        weight(#[trigger] rows[index]) >= 0 && (weight(rows[index]) > 0 <==> predicate(rows[index])),
    ensures sum(rows, weight) == 0 <==> !(exists|index: int| 0 <= index < rows.len() && predicate(#[trigger] rows[index])),
{
    lemma_sum_nonneg(rows, weight);
    if exists|index: int| 0 <= index < rows.len() && predicate(#[trigger] rows[index]) {
        let index = choose|index: int| 0 <= index < rows.len() && predicate(#[trigger] rows[index]);
        lemma_sum_pos(rows, weight, index);
    } else {
        lemma_sum_zero(rows, weight);
    }
}

pub proof fn lemma_signed_product<A>(before: Seq<A>, after: Seq<A>, delta: Seq<(A, int)>,
    weight: spec_fn(A) -> int, old_factor: spec_fn(A) -> int, new_factor: spec_fn(A) -> int)
    requires bag_step(before, after, delta),
    ensures sum(after, |row: A| weight(row) * new_factor(row)) ==
        sum(before, |row: A| weight(row) * old_factor(row)) +
        sum(delta, |entry: (A, int)| entry.1 * (weight(entry.0) * old_factor(entry.0))) +
        sum(after, |row: A| weight(row) * (new_factor(row) - old_factor(row))),
{
    let old_weight = |row: A| weight(row) * old_factor(row);
    let correction = |row: A| weight(row) * (new_factor(row) - old_factor(row));
    let new_weight = |row: A| weight(row) * new_factor(row);
    assert(sum(after, old_weight) == sum(before, old_weight) +
        sum(delta, |entry: (A, int)| entry.1 * old_weight(entry.0)));
    assert forall|row: A| #[trigger] new_weight(row) == old_weight(row) + correction(row) by {
        assert(weight(row) * new_factor(row) == weight(row) * old_factor(row) +
            weight(row) * (new_factor(row) - old_factor(row))) by (nonlinear_arith);
    }
    lemma_sum_add(after, old_weight, correction, new_weight);
    lemma_sum_ext_all(delta, |entry: (A, int)| entry.1 * old_weight(entry.0),
        |entry: (A, int)| entry.1 * (weight(entry.0) * old_factor(entry.0)));
}

pub proof fn lemma_push_set<A>(rows: Seq<A>, value: A)
    ensures rows.push(value).to_set() == rows.to_set().insert(value),
{
    assert forall|element: A| #[trigger] rows.push(value).to_set().contains(element) <==>
        rows.to_set().insert(value).contains(element) by {
        if rows.push(value).contains(element) {
            let index = choose|index: int| 0 <= index < rows.push(value).len() && (#[trigger] rows.push(value)[index]) == element;
            if index < rows.len() { assert(rows[index] == element); }
        }
        if rows.contains(element) {
            let index = choose|index: int| 0 <= index < rows.len() && (#[trigger] rows[index]) == element;
            assert(rows.push(value)[index] == element);
        }
    }
    assert(rows.push(value).to_set() =~= rows.to_set().insert(value));
}

pub proof fn lemma_scan_property<A>(database: Seq<A>, rows: Seq<A>, filter: spec_fn(A) -> bool, good: spec_fn(A) -> bool)
    requires forall|weight: spec_fn(A) -> int| #[trigger] sum(rows, weight) ==
            sum(database, |row: A| if filter(row) { weight(row) } else { 0 }),
        forall|index: int| 0 <= index < rows.len() ==> good(#[trigger] rows[index]),
    ensures forall|index: int| 0 <= index < database.len() ==> filter(#[trigger] database[index]) ==> good(database[index]),
{
    let bad = |row: A| ind(!good(row));
    let filtered = |row: A| if filter(row) { bad(row) } else { 0int };
    lemma_sum_zero(rows, bad);
    assert(sum(rows, bad) == sum(database, filtered));
    assert forall|index: int| 0 <= index < database.len() implies filter(#[trigger] database[index]) ==> good(database[index]) by {
        if filter(database[index]) && !good(database[index]) {
            lemma_sum_pos(database, filtered, index);
        }
    }
}

pub proof fn lemma_capture_property<A>(before: Seq<A>, after: Seq<A>, delta: Seq<(A, int)>, good: spec_fn(A) -> bool)
    requires bag_step(before, after, delta),
        forall|index: int| 0 <= index < before.len() ==> good(#[trigger] before[index]),
        forall|index: int| 0 <= index < delta.len() ==> good((#[trigger] delta[index]).0),
    ensures forall|index: int| 0 <= index < after.len() ==> good(#[trigger] after[index]),
{
    let bad = |row: A| ind(!good(row));
    lemma_sum_zero(before, bad);
    lemma_sum_zero(delta, |entry: (A, int)| entry.1 * bad(entry.0));
    assert(sum(after, bad) == sum(before, bad) + sum(delta, |entry: (A, int)| entry.1 * bad(entry.0)));
    assert forall|index: int| 0 <= index < after.len() implies good(#[trigger] after[index]) by {
        if !good(after[index]) { lemma_sum_pos(after, bad, index); }
    }
}

}
