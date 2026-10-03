use vstd::prelude::*;
use verus_common::sums::*;
use verus_common::rel::*;

verus! {

/// Use to equate sums of aligned projections, including lossy captured rows.
pub proof fn lemma_sum_aligned<A, B>(left: Seq<A>, right: Seq<B>,
    left_weight: spec_fn(A) -> int, right_weight: spec_fn(B) -> int)
    requires left.len() == right.len(),
        forall|index: int| 0 <= index < left.len() ==> left_weight(#[trigger] left[index]) == right_weight(right[index]),
    ensures sum(left, left_weight) == sum(right, right_weight),
    decreases left.len(),
{
    if left.len() > 0 {
        assert forall|index: int| 0 <= index < left.drop_last().len() implies
            left_weight(#[trigger] left.drop_last()[index]) == right_weight(right.drop_last()[index]) by {
            assert(left.drop_last()[index] == left[index]);
            assert(right.drop_last()[index] == right[index]);
        }
        lemma_sum_aligned(left.drop_last(), right.drop_last(), left_weight, right_weight);
    }
}

/// Use to carry finite-support membership through a vector push.
pub proof fn lemma_contains_push<A>(rows: Seq<A>, value: A)
    ensures forall|key: A| #[trigger] rows.push(value).contains(key)
        == (rows.contains(key) || key == value),
{
    assert forall|key: A| #[trigger] rows.push(value).contains(key)
        == (rows.contains(key) || key == value) by {
        if rows.push(value).contains(key) {
            let index = choose|index: int| 0 <= index < rows.push(value).len()
                && (#[trigger] rows.push(value)[index]) == key;
            if index < rows.len() { assert(rows[index] == key); }
        }
        if rows.contains(key) {
            let index = choose|index: int| 0 <= index < rows.len() && (#[trigger] rows[index]) == key;
            assert(rows.push(value)[index] == key);
        }
        if key == value { assert(rows.push(value)[rows.len() as int] == key); }
    }
}

/// Use to establish membership of a combined finite key universe.
pub proof fn lemma_contains_concat<A>(left: Seq<A>, right: Seq<A>)
    ensures forall|key: A| #[trigger] (left + right).contains(key)
        == (left.contains(key) || right.contains(key)),
{
    assert forall|key: A| #[trigger] (left + right).contains(key)
        == (left.contains(key) || right.contains(key)) by {
        if (left + right).contains(key) {
            let index = choose|index: int| 0 <= index < (left + right).len()
                && (#[trigger] (left + right)[index]) == key;
            if index < left.len() { assert(left[index] == key); }
            else { assert(right[index - left.len()] == key); }
        }
        if left.contains(key) {
            let index = choose|index: int| 0 <= index < left.len() && (#[trigger] left[index]) == key;
            assert((left + right)[index] == key);
        }
        if right.contains(key) {
            let index = choose|index: int| 0 <= index < right.len() && (#[trigger] right[index]) == key;
            assert((left + right)[left.len() as int + index] == key);
        }
    }
}

/// Use when enumerating distinct keys to exclude the next key from the processed prefix.
pub proof fn lemma_key_not_in_prefix<A>(keys: Seq<A>, index: int)
    requires no_dups(keys), 0 <= index < keys.len(),
    ensures !keys.take(index).contains(keys[index]),
{
    if keys.take(index).contains(keys[index]) {
        let earlier = choose|earlier: int| 0 <= earlier < index
            && (#[trigger] keys.take(index)[earlier]) == keys[index];
        assert(keys[earlier] == keys[index]);
        assert(false);
    }
}

/// Use to change the distinct key universe of a sum when both universes cover its nonzero support.
pub proof fn lemma_sum_support<A>(left: Seq<A>, right: Seq<A>, weight: spec_fn(A) -> int)
    requires no_dups(left), no_dups(right),
        forall|key: A| #[trigger] weight(key) != 0 ==> left.contains(key) && right.contains(key),
    ensures sum(left, weight) == sum(right, weight),
{
    assert(distinct(left));
    assert(distinct(right));
    let per = |key: A| sum(left, |other: A| if other == key { weight(other) } else { 0 });
    let identity = |key: A| key;
    assert forall|key: A| #[trigger] per(key)
        == sum(left, |other: A| if identity(other) == key { weight(other) } else { 0 }) by {
        lemma_sum_ext_all(left, |other: A| if other == key { weight(other) } else { 0 },
            |other: A| if identity(other) == key { weight(other) } else { 0 });
    }
    lemma_sum_group(left, right, identity, weight, per);
    assert forall|key: A| #[trigger] per(key) == weight(key) by {
        lemma_sum_indicator_distinct(left, key, weight(key),
            |other: A| if other == key { weight(other) } else { 0 });
    }
    lemma_sum_ext_all(right, per, weight);
}

/// Use to relate a zero-or-one weighted sum to existence of a contributing row.
pub proof fn lemma_indicator<A>(rows: Seq<A>, weight: spec_fn(A) -> int)
    requires forall|row: A| #[trigger] weight(row) == 0 || weight(row) == 1,
    ensures sum(rows, weight) >= 0,
        (sum(rows, weight) > 0)
            <==> exists|index: int| 0 <= index < rows.len() && #[trigger] weight(rows[index]) > 0,
{
    lemma_sum_nonneg(rows, weight);
    if sum(rows, weight) > 0 {
        let index = lemma_sum_nonzero_witness(rows, weight);
        assert(weight(rows[index]) > 0);
    }
    if exists|index: int| 0 <= index < rows.len() && #[trigger] weight(rows[index]) > 0 {
        let index = choose|index: int| 0 <= index < rows.len() && #[trigger] weight(rows[index]) > 0;
        lemma_sum_pos(rows, weight, index);
    }
}

/// Use to identify a bag cardinality with its constant-one sum.
pub proof fn lemma_sum_one<A>(rows: Seq<A>)
    ensures sum(rows, |row: A| 1int) == rows.len(),
    decreases rows.len(),
{
    if rows.len() > 0 { lemma_sum_one(rows.drop_last()); }
}

/// Use to eliminate a filter that keeps every row.
pub proof fn lemma_filter_true<A>(rows: Seq<A>)
    ensures rows.filter(|row: A| true) == rows,
    decreases rows.len(),
{
    reveal_with_fuel(Seq::filter, 2);
    if rows.len() > 0 {
        lemma_filter_true(rows.drop_last());
        assert(rows.drop_last().push(rows.last()) =~= rows);
    }
}

/// Use to prove positivity of a nonnegative count multiplied by a positive factor.
pub proof fn lemma_positive_product(count: int, factor: int)
    requires count >= 0, factor > 0,
    ensures (count * factor > 0) <==> count > 0,
{
    assert((count * factor > 0) <==> count > 0) by (nonlinear_arith)
        requires count >= 0, factor > 0;
}

/// Use to transfer sequence membership through multiset equalities.
pub proof fn lemma_bag_contains<A>(rows: Seq<A>, row: A)
    ensures rows.contains(row) <==> rows.to_multiset().count(row) > 0,
{
    lemma_count_as_sum(rows, row);
    lemma_indicator(rows, eqv(row));
    if rows.contains(row) {
        let index = choose|index: int| 0 <= index < rows.len() && rows[index] == row;
        assert(rows[index] == row);
        lemma_sum_pos(rows, eqv(row), index);
    }
    if rows.to_multiset().count(row) > 0 {
        let index = lemma_sum_nonzero_witness(rows, eqv(row));
        assert(rows[index] == row);
        assert(rows.contains(row));
    }
}

/// Use to transfer key uniqueness through an item-preserving sort.
pub proof fn lemma_no_dups_bag<A>(original: Seq<A>, reordered: Seq<A>)
    requires no_dups(original), original.to_multiset() == reordered.to_multiset(),
    ensures no_dups(reordered),
{
    assert(distinct(original));
    assert forall|first: int, second: int| #![trigger reordered[first], reordered[second]]
        0 <= first < second < reordered.len() implies reordered[first] != reordered[second] by {
        if reordered[first] == reordered[second] {
            let row = reordered[first];
            lemma_bag_contains(reordered, row);
            lemma_bag_contains(original, row);
            assert(original.contains(row));
            lemma_sum_indicator_distinct(original, row, 1, eqv(row));
            lemma_count_as_sum(original, row);
            lemma_count_as_sum(reordered, row);
            lemma_sum_remove(reordered, second, eqv(row));
            let removed = reordered.remove(second);
            assert(removed[first] == row);
            lemma_sum_pos(removed, eqv(row), first);
            assert(false);
        }
    }
}

}
