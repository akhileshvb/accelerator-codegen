// The proof library: lemmas shared by Verus kernels, written by the pipeline's
// agents (seeded empty).  Rules: a lemma is general (no query names; stated
// over the spec vocabulary of verus_common or general structures), verifies
// on its own without assume, says when to use it in a doc comment, and
// records where it came from.  Adding one re-verifies every kernel that uses
// the library (pipeline tool prooflib_publish).
#![allow(unused_imports)]
use vstd::prelude::*;
use verus_common::bytes::*;
use verus_common::sums::*;
pub mod like;
pub mod wide;
pub use like::like_match_exec;

verus! {

/// Use when nonnegative row counts determine the support of an independently signed measure.
pub proof fn lemma_sum_measure_support<Row>(rows: Seq<Row>, count: spec_fn(Row) -> int, measure: spec_fn(Row) -> int)
    requires forall|index: int| 0 <= index < rows.len() ==> count(#[trigger] rows[index]) >= 0
        && (count(rows[index]) == 0 ==> measure(rows[index]) == 0),
    ensures sum(rows, count) >= 0, sum(rows, count) == 0 ==> sum(rows, measure) == 0,
{
    lemma_sum_nonneg(rows, count);
    if sum(rows, count) == 0 {
        assert forall|index: int| 0 <= index < rows.len() implies measure(#[trigger] rows[index]) == 0 by {
            lemma_sum_ge_term(rows, count, index);
        }
        lemma_sum_zero(rows, measure);
    }
}

/// Use when executable copies preserve row projections rather than Rust object identity.
/// Origin: Q19 profile and byte-key copy proofs.
pub proof fn lemma_sum_aligned<A, B>(
    left: Seq<A>, right: Seq<B>,
    left_weight: spec_fn(A) -> int, right_weight: spec_fn(B) -> int,
)
    requires
        left.len() == right.len(),
        forall|index: int| #![trigger left[index], right[index]]
            0 <= index < left.len() ==>
                left_weight(left[index]) == right_weight(right[index]),
    ensures sum(left, left_weight) == sum(right, right_weight),
    decreases left.len(),
{
    if left.len() > 0 {
        assert forall|index: int|
            #![trigger left.drop_last()[index], right.drop_last()[index]]
            0 <= index < left.drop_last().len() implies
                left_weight(left.drop_last()[index]) == right_weight(right.drop_last()[index])
        by {
            assert(left.drop_last()[index] == left[index]);
            assert(right.drop_last()[index] == right[index]);
        }
        lemma_sum_aligned(left.drop_last(), right.drop_last(), left_weight, right_weight);
        assert(left.last() == left[left.len() - 1]);
        assert(right.last() == right[right.len() - 1]);
    }
}

/// Use for a loop accumulating consecutive integer cells with a processed-range invariant.
/// Origin: Q19 quantity-window probes.
pub proof fn lemma_range_step(end: int, weight: spec_fn(int) -> int)
    requires end >= 0,
    ensures sum(rng(end + 1), weight) == sum(rng(end), weight) + weight(end),
{
    assert(rng(end + 1) =~= rng(end).push(end));
    lemma_sum_push(rng(end), end, weight);
}

/// Use to turn exact-cell probes into an interval predicate after exchanging summation order.
/// Origin: Q19 quantity-window aggregation.
pub proof fn lemma_range_single_cell(length: int, quantity: int, parameter: int, value: int)
    requires length >= 0,
    ensures sum(rng(length), |offset: int| if parameter + offset == quantity { value } else { 0 })
        == if parameter <= quantity < parameter + length { value } else { 0 },
{
    let weight = |offset: int| if parameter + offset == quantity { value } else { 0 };
    if parameter <= quantity < parameter + length {
        let offset = quantity - parameter;
        assert(rng(length)[offset] == offset);
        assert forall|index: int| #![trigger rng(length)[index]]
            0 <= index < length && index != offset implies weight(rng(length)[index]) == 0 by {}
        lemma_sum_single_match(rng(length), weight, offset);
    } else {
        assert forall|index: int| #![trigger rng(length)[index]]
            0 <= index < length implies weight(rng(length)[index]) == 0 by {}
        lemma_sum_zero(rng(length), weight);
    }
}

/// Use to connect a SQL tuple count to the constant-one weight used by sum algebra.
/// Origin: Q19 aggregate-result proof.
pub proof fn lemma_sum_ones<A>(rows: Seq<A>)
    ensures sum(rows, |row: A| 1) == rows.len(),
    decreases rows.len(),
{
    if rows.len() > 0 { lemma_sum_ones(rows.drop_last()); }
}

pub open spec fn int_bytes_lt(left: (int, Seq<u8>), right: (int, Seq<u8>)) -> bool {
    left.0 < right.0 || (left.0 == right.0 && bytes_lt(left.1, right.1))
}

/// Use for ordered indexes whose composite keys contain an integer and C-collated bytes.
pub proof fn lemma_int_bytes_order(left: (int, Seq<u8>), right: (int, Seq<u8>))
    ensures
        !int_bytes_lt(left, left),
        !(int_bytes_lt(left, right) && int_bytes_lt(right, left)),
        left != right ==> int_bytes_lt(left, right) || int_bytes_lt(right, left),
{
    lemma_bytes_lt_irrefl(left.1);
    lemma_bytes_lt_asym(left.1, right.1);
    if left.0 == right.0 && left != right {
        if left.1 == right.1 { assert(left == right); }
        lemma_bytes_lt_total(left.1, right.1);
    }
}

/// Use when an ordered-tree rotation or a range bound needs composite-key transitivity.
pub proof fn lemma_int_bytes_trans(first: (int, Seq<u8>), middle: (int, Seq<u8>), last: (int, Seq<u8>))
    requires int_bytes_lt(first, middle), int_bytes_lt(middle, last),
    ensures int_bytes_lt(first, last),
{
    if first.0 == middle.0 && middle.0 == last.0 {
        lemma_bytes_lt_trans(first.1, middle.1, last.1);
    }
}

/// Use when a nested SQL tuple expansion needs a normalized outer weight, avoiding distinct beta-equivalent closure identities.
pub proof fn lemma_flat_map_weight<Source, Target>(source: Seq<Source>, mapper: spec_fn(Source) -> Seq<Target>, weight: spec_fn(Target) -> int, nested: spec_fn(Source) -> int)
    requires forall|item: Source| #[trigger] nested(item) == verus_common::sums::sum(mapper(item), weight),
    ensures verus_common::sums::sum(verus_common::rel::flat(source.map_values(mapper)), weight)
        == verus_common::sums::sum(source, nested),
{
    verus_common::rel::lemma_sum_flat_map(source, mapper, weight);
    assert((|item: Source| verus_common::sums::sum(mapper(item), weight)) =~= nested);
}

/// Use before appending a processed-prefix element to a duplicate-free support vector.
pub proof fn lemma_prefix_fresh<Value>(rows: Seq<Value>, index: int)
    requires rows.no_duplicates(), 0 <= index < rows.len(),
    ensures !rows.take(index).contains(rows[index]),
{
    if rows.take(index).contains(rows[index]) {
        let offset = choose|offset: int| 0 <= offset < rows.take(index).len()
            && (#[trigger] rows.take(index)[offset]) == rows[index];
        assert(rows[offset] == rows[index]);
        assert(false);
    }
}

/// Use for complete support invariants when a checked map update introduces one new key.
pub proof fn lemma_unique_push<Value>(rows: Seq<Value>, value: Value)
    requires rows.no_duplicates(), !rows.contains(value),
    ensures rows.push(value).no_duplicates(),
{
    assert forall|left: int, right: int| 0 <= left < rows.push(value).len()
        && 0 <= right < rows.push(value).len() && left != right implies
            (#[trigger] rows.push(value)[left]) != (#[trigger] rows.push(value)[right]) by {
        if left < rows.len() && right < rows.len() {
            assert(rows[left] != rows[right]);
        } else if left == rows.len() {
            assert(rows.contains(rows[right]));
        } else {
            assert(rows.contains(rows[left]));
        }
    }
}

/// Use for simultaneous updates of both sides of a multiplicity-weighted join.
pub proof fn lemma_bilinear_delta(before_left: int, after_left: int, before_right: int, after_right: int)
    ensures after_left * after_right - before_left * before_right
        == (after_left - before_left) * before_right + after_left * (after_right - before_right),
{
    assert(after_left * after_right - before_left * before_right
        == (after_left - before_left) * before_right + after_left * (after_right - before_right)) by (nonlinear_arith);
}

/// Use to compare sparse-support folds whose distinct key enumerations have different orders.
pub proof fn lemma_sum_same_support<Key>(left: Seq<Key>, right: Seq<Key>, weight: spec_fn(Key) -> int)
    requires left.no_duplicates(), right.no_duplicates(), left.to_set() == right.to_set(),
    ensures sum(left, weight) == sum(right, weight),
    decreases left.len(),
{
    broadcast use vstd::seq_lib::group_seq_properties;
    left.to_set_ensures();
    right.to_set_ensures();
    if left.len() == 0 {
        assert(right.len() == 0) by {
            if right.len() > 0 {
                assert(right.contains(right[0]));
                assert(right.to_set().contains(right[0]));
                assert(left.to_set().contains(right[0]));
                assert(left.contains(right[0]));
            }
        }
    } else {
        let key = left.last();
        assert(left.contains(key));
        assert(left.to_set().contains(key));
        assert(right.to_set().contains(key));
        assert(right.contains(key));
        let position = choose|position: int| 0 <= position < right.len() && #[trigger] right[position] == key;
        let smaller_left = left.drop_last();
        let smaller_right = right.remove(position);
        assert(smaller_left.no_duplicates());
        assert(smaller_right.no_duplicates()) by {
            assert forall|first: int, second: int| #![trigger smaller_right[first], smaller_right[second]]
                0 <= first < second < smaller_right.len() implies smaller_right[first] != smaller_right[second] by {
                let old_first = if first < position { first } else { first + 1 };
                let old_second = if second < position { second } else { second + 1 };
                assert(smaller_right[first] == right[old_first]);
                assert(smaller_right[second] == right[old_second]);
            }
        }
        assert(smaller_left.to_set() =~= smaller_right.to_set()) by {
            assert forall|other: Key| #[trigger] smaller_left.contains(other) == smaller_right.contains(other) by {
                if smaller_left.contains(other) {
                    let source = choose|source: int| 0 <= source < smaller_left.len() && #[trigger] smaller_left[source] == other;
                    assert(left[source] == other);
                    assert(other != key);
                    assert(left.to_set().contains(other));
                    assert(right.to_set().contains(other));
                    assert(right.contains(other));
                    let target = choose|target: int| 0 <= target < right.len() && #[trigger] right[target] == other;
                    assert(target != position);
                    if target < position { assert(smaller_right[target] == other); }
                    else { assert(smaller_right[target - 1] == other); }
                }
                if smaller_right.contains(other) {
                    let target = choose|target: int| 0 <= target < smaller_right.len() && #[trigger] smaller_right[target] == other;
                    let old_target = if target < position { target } else { target + 1 };
                    assert(right[old_target] == other);
                    assert(other != key);
                    assert(right.to_set().contains(other));
                    assert(left.to_set().contains(other));
                    assert(left.contains(other));
                    let source = choose|source: int| 0 <= source < left.len() && #[trigger] left[source] == other;
                    assert(source < left.len() - 1);
                    assert(smaller_left[source] == other);
                }
            }
        }
        smaller_left.to_set_ensures();
        smaller_right.to_set_ensures();
        lemma_sum_same_support(smaller_left, smaller_right, weight);
        lemma_sum_remove(right, position, weight);
    }
}

pub open spec fn join_sum<A, B>(left: Seq<A>, right: Seq<B>, weight: spec_fn(A, B) -> int) -> int {
    sum(left, |entry: A| sum(right, |other: B| weight(entry, other)))
}

pub open spec fn left_join_delta<A, B>(changes: Seq<(A, int)>, right: Seq<B>, weight: spec_fn(A, B) -> int) -> int {
    sum(changes, |entry: (A, int)| entry.1 * sum(right, |other: B| weight(entry.0, other)))
}

pub open spec fn right_join_delta<A, B>(left: Seq<A>, changes: Seq<(B, int)>, weight: spec_fn(A, B) -> int) -> int {
    sum(changes, |entry: (B, int)| entry.1 * sum(left, |other: A| weight(other, entry.0)))
}

/// Use for simultaneous bag updates of a join: delta-left times old-right plus new-left times delta-right.
pub proof fn lemma_join_bag_step<A, B>(before_left: Seq<A>, after_left: Seq<A>, left_changes: Seq<(A, int)>,
    before_right: Seq<B>, after_right: Seq<B>, right_changes: Seq<(B, int)>, weight: spec_fn(A, B) -> int)
    requires bag_step(before_left, after_left, left_changes), bag_step(before_right, after_right, right_changes),
    ensures join_sum(after_left, after_right, weight) == join_sum(before_left, before_right, weight)
        + left_join_delta(left_changes, before_right, weight) + right_join_delta(after_left, right_changes, weight),
{
    let after_by_left = |entry: A| sum(after_right, |other: B| weight(entry, other));
    let after_by_right = |entry: B| sum(after_left, |other: A| weight(other, entry));
    let before_by_right = |entry: B| sum(before_left, |other: A| weight(other, entry));
    let delta_by_right = |entry: B| sum(left_changes, |change: (A, int)| change.1 * weight(change.0, entry));
    let delta_by_left = |change: (A, int)| sum(before_right, |entry: B| change.1 * weight(change.0, entry));
    lemma_sum_swap(after_left, after_right, weight, after_by_left, after_by_right);
    assert(sum(after_right, after_by_right) == sum(before_right, after_by_right)
        + sum(right_changes, |change: (B, int)| change.1 * after_by_right(change.0)));
    assert forall|entry: B| #[trigger] after_by_right(entry) == before_by_right(entry) + delta_by_right(entry) by {
        let inner = |other: A| weight(other, entry);
        assert(sum(after_left, inner) == sum(before_left, inner)
            + sum(left_changes, |change: (A, int)| change.1 * inner(change.0)));
        lemma_sum_ext_all(left_changes, |change: (A, int)| change.1 * inner(change.0),
            |change: (A, int)| change.1 * weight(change.0, entry));
    }
    lemma_sum_add(before_right, before_by_right, delta_by_right, after_by_right);
    let delta_weight = |change: (A, int), entry: B| change.1 * weight(change.0, entry);
    assert forall|entry: B| #[trigger] delta_by_right(entry) == sum(left_changes, |change: (A, int)| delta_weight(change, entry)) by {
        lemma_sum_ext_all(left_changes, |change: (A, int)| change.1 * weight(change.0, entry),
            |change: (A, int)| delta_weight(change, entry));
    }
    assert forall|change: (A, int)| #[trigger] delta_by_left(change) == sum(before_right, |entry: B| delta_weight(change, entry)) by {
        lemma_sum_ext_all(before_right, |entry: B| change.1 * weight(change.0, entry),
            |entry: B| delta_weight(change, entry));
    }
    lemma_sum_swap(left_changes, before_right, delta_weight,
        delta_by_left, delta_by_right);
    let scaled_left = |change: (A, int)| change.1 * sum(before_right, |entry: B| weight(change.0, entry));
    assert forall|change: (A, int)| #[trigger] delta_by_left(change) == scaled_left(change) by {
        lemma_sum_scale(before_right, |entry: B| weight(change.0, entry),
            |entry: B| change.1 * weight(change.0, entry), change.1);
    }
    lemma_sum_ext_all(left_changes, delta_by_left, scaled_left);
    let before_by_left = |entry: A| sum(before_right, |other: B| weight(entry, other));
    lemma_sum_swap(before_left, before_right, weight, before_by_left, before_by_right);
    lemma_sum_ext_all(right_changes, |change: (B, int)| change.1 * after_by_right(change.0),
        |change: (B, int)| change.1 * sum(after_left, |other: A| weight(other, change.0)));
}
/// Use when a sparse-map key catalog gains one fresh key.
pub proof fn lemma_push_membership<A>(items: Seq<A>, item: A)
    ensures
        forall|value: A| #[trigger] items.push(item).contains(value) == (items.contains(value) || value == item),
        items.no_duplicates() && !items.contains(item) ==> items.push(item).no_duplicates(),
{
    assert forall|value: A| #[trigger] items.push(item).contains(value) == (items.contains(value) || value == item) by {
        if items.push(item).contains(value) {
            let index = choose|index: int| 0 <= index < items.len() + 1 && (#[trigger] items.push(item)[index]) == value;
            if index < items.len() { assert(items[index] == value); assert(items.contains(value)); }
        }
        if items.contains(value) {
            let index = choose|index: int| 0 <= index < items.len() && (#[trigger] items[index]) == value;
            assert(items.push(item)[index] == value);
        }
        if value == item { assert(items.push(item)[items.len() as int] == value); }
    }
    if items.no_duplicates() && !items.contains(item) {
        assert forall|first: int, second: int| 0 <= first < second < items.len() + 1 implies
            (#[trigger] items.push(item)[first]) != (#[trigger] items.push(item)[second]) by {
            if second == items.len() { assert(items[first] != item); }
        }
    }
}
/// Use when a checked signed update changes a sparse count's contribution to an arbitrary scan weight.
/// Origin: Q4 daily-count scan and signed-delta accumulator proofs.
pub proof fn lemma_count_weight_add(before: int, delta: int, weight: int)
    ensures (before + delta) * weight == before * weight + delta * weight,
{
    assert((before + delta) * weight == before * weight + delta * weight) by(nonlinear_arith);
}
/// Use when a bag-step signed row weight and a count-times-weight accumulator group their products differently.
/// Origin: Q4 simultaneous order and witness-count maintenance.
pub proof fn lemma_count_weight_assoc(sign: int, count: int, weight: int)
    ensures (sign * count) * weight == sign * (count * weight),
{
    assert((sign * count) * weight == sign * (count * weight)) by(nonlinear_arith);
}

/// Use to pack two bounded numeric components into a primitive hash-map key.
pub proof fn lemma_numeric_key_parts(quotient: int, remainder: int, base: int)
    requires base > 0, 0 <= remainder < base,
    ensures (quotient * base + remainder) / base == quotient,
        (quotient * base + remainder) % base == remainder,
{
    assert((quotient * base + remainder) / base == quotient
        && (quotient * base + remainder) % base == remainder) by (nonlinear_arith)
        requires base > 0, 0 <= remainder < base;
}

/// Use to show that two aligned intervals of one width covering a point have the same index.
pub proof fn lemma_aligned_interval_unique(point: int, width: int, left: int, right: int)
    requires width > 0, left * width <= point < (left + 1) * width,
        right * width <= point < (right + 1) * width,
    ensures left == right,
{
    assert(left == right) by (nonlinear_arith)
        requires width > 0, left * width <= point < (left + 1) * width,
            right * width <= point < (right + 1) * width;
}

/// Use when replacing a dyadic interval by its parent during aggregate construction.
pub proof fn lemma_binary_parent_interval(point: int, width: int, bucket: int)
    requires width > 0, bucket >= 0, bucket * width <= point < (bucket + 1) * width,
    ensures (bucket / 2) * (2 * width) <= point < (bucket / 2 + 1) * (2 * width),
{
    assert(bucket == 2 * (bucket / 2) + bucket % 2 && 0 <= bucket % 2 < 2) by (nonlinear_arith);
    assert((bucket / 2) * (2 * width) <= point < (bucket / 2 + 1) * (2 * width)) by (nonlinear_arith)
        requires width > 0, bucket * width <= point < (bucket + 1) * width,
            bucket == 2 * (bucket / 2) + bucket % 2, 0 <= bucket % 2 < 2;
}

/// Use to identify the exact lower boundary of an aligned range probe.
pub proof fn lemma_aligned_boundary(point: int, width: int)
    requires width > 0, point % width == 0,
    ensures point == (point / width) * width,
{
    assert(point == (point / width) * width) by (nonlinear_arith)
        requires width > 0, point % width == 0;
}
  /// Use to count live keys after a finite, duplicate-free sequence of boolean replacements.
  pub proof fn lemma_replacement_support<Key>(before: Set<Key>, keys: Seq<Key>, present: spec_fn(Key) -> bool)
      requires keys.no_duplicates(),
      ensures before.difference(keys.to_set()).union(keys.to_set().filter(present)).len() as int
          == before.len() as int + sum(keys, |key: Key|
              (if present(key) { 1int } else { 0int }) - (if before.contains(key) { 1int } else { 0int })),
      decreases keys.len(),
  {
      broadcast use vstd::seq_lib::group_seq_properties;
      keys.to_set_ensures();
      if keys.len() == 0 {
          assert(keys.to_set() =~= Set::empty());
          assert(before.difference(keys.to_set()).union(keys.to_set().filter(present)) =~= before);
      } else {
          let prefix = keys.drop_last();
          let last = keys.last();
          prefix.to_set_ensures();
          assert(!prefix.contains(last));
          lemma_replacement_support(before, prefix, present);
          let partial = before.difference(prefix.to_set()).union(prefix.to_set().filter(present));
          let after = before.difference(keys.to_set()).union(keys.to_set().filter(present));
          assert(partial.contains(last) == before.contains(last));
          if present(last) {
              assert(after =~= partial.insert(last));
          } else {
              assert(after =~= partial.remove(last));
          }
          lemma_sum_push(prefix, last, |key: Key|
              (if present(key) { 1int } else { 0int }) - (if before.contains(key) { 1int } else { 0int }));
          assert(keys =~= prefix.push(last));
      }
  }
/// Use to relate a distinct executable key enumeration to a finite live-support count.
pub proof fn lemma_distinct_selected_count<Key>(keys: Seq<Key>, keep: spec_fn(Key) -> bool)
    requires distinct(keys),
    ensures keys.to_set().filter(keep).finite(),
        keys.to_set().filter(keep).len() as int == sum(keys, |key: Key| if keep(key) { 1 } else { 0 }),
    decreases keys.len(),
{
    broadcast use vstd::seq_lib::group_seq_properties;
    keys.to_set_ensures();
    if keys.len() == 0 {
        assert(keys.to_set().filter(keep) =~= Set::<Key>::empty());
    } else {
        let previous = keys.drop_last();
        let key = keys.last();
        assert(keys =~= previous.push(key));
        assert(keys.to_set() =~= previous.to_set().insert(key));
        previous.to_set_ensures();
        assert(distinct(previous));
        assert(!previous.to_set().contains(key)) by {
            if previous.to_set().contains(key) {
                let offset = choose|offset: int| 0 <= offset < previous.len() && #[trigger] previous[offset] == key;
                assert(keys[offset] == keys[keys.len() - 1]);
            }
        }
        lemma_distinct_selected_count(previous, keep);
        let selected = previous.to_set().filter(keep);
        if keep(key) {
            assert(keys.to_set().filter(keep) =~= selected.insert(key));
            assert(!selected.contains(key));
            assert(selected.insert(key).len() == selected.len() + 1);
        } else {
            assert(keys.to_set().filter(keep) =~= selected);
        }
    }
}
/// Use to reduce exact rational comparisons with positive denominators to cross products.
/// Origin: aggregate-threshold kernel proof, October 3, 2026.
pub proof fn lemma_ratio_lt_positive(left_num: int, left_den: int, right_num: int, right_den: int) requires left_den > 0, right_den > 0, ensures verus_common::sqlfns::ratio_lt(left_num, left_den, right_num, right_den) == (left_num * right_den < right_num * left_den), { reveal(verus_common::sqlfns::ratio_lt); assert(left_num * left_den * right_den * right_den == (left_num * right_den) * (left_den * right_den)) by (nonlinear_arith); assert(right_num * right_den * left_den * left_den == (right_num * left_den) * (left_den * right_den)) by (nonlinear_arith); assert((left_num * right_den) * (left_den * right_den) < (right_num * left_den) * (left_den * right_den) <==> left_num * right_den < right_num * left_den) by (nonlinear_arith) requires left_den > 0, right_den > 0; }
/// Use when a generated FROM expansion maps each input to exactly one output tuple.
pub proof fn lemma_flat_singleton_map<Source, Target>(rows: Seq<Source>, mapper: spec_fn(Source) -> Target) ensures verus_common::rel::flat(rows.map_values(|row: Source| seq![mapper(row)])) == rows.map_values(mapper), decreases rows.len(), { if rows.len() > 0 { lemma_flat_singleton_map(rows.drop_last(), mapper); assert(rows.map_values(|row: Source| seq![mapper(row)]).drop_last() =~= rows.drop_last().map_values(|row: Source| seq![mapper(row)])); assert(rows.map_values(mapper) =~= rows.drop_last().map_values(mapper).push(mapper(rows.last()))); } else { assert(rows.map_values(mapper) =~= Seq::empty()); } }
/// Use when a generated map_where has a predicate that keeps every input position.
pub proof fn lemma_map_where_all<Source, Target>(rows: Seq<Source>, keep: spec_fn(Source) -> bool, mapper: spec_fn(Source) -> Target) requires forall|index: int| 0 <= index < rows.len() ==> keep(#[trigger] rows[index]), ensures verus_common::order::map_where(rows, keep, mapper) == rows.map_values(mapper), decreases rows.len(), { if rows.len() > 0 { assert forall|index: int| 0 <= index < rows.drop_last().len() implies keep(#[trigger] rows.drop_last()[index]) by { assert(rows.drop_last()[index] == rows[index]); } lemma_map_where_all(rows.drop_last(), keep, mapper); assert(keep(rows.last())); assert(rows.map_values(mapper) =~= rows.drop_last().map_values(mapper).push(mapper(rows.last()))); } else { assert(rows.map_values(mapper) =~= Seq::empty()); } }
/// Use for zero-default keyed sums in processed-prefix accumulator invariants.
pub open spec fn key_sum<A>(rows: Seq<A>, key_of: spec_fn(A) -> int, key: int, weight: spec_fn(A) -> int) -> int { sum(rows, |row: A| if key_of(row) == key { weight(row) } else { 0 }) }
/// Use for a keyed accumulator updated from one row in a processed-prefix loop.
pub proof fn lemma_key_sum_step<A>(rows: Seq<A>, key_of: spec_fn(A) -> int, weight: spec_fn(A) -> int, index: int, key: int, before: spec_fn(int) -> int, after: spec_fn(int) -> int) requires 0 <= index < rows.len(), key_of(rows[index]) == key, forall|target: int| #[trigger] before(target) == key_sum(rows.take(index), key_of, target, weight), after(key) == before(key) + (if key_of(rows[index]) == key { weight(rows[index]) } else { 0 }), forall|target: int| target != key ==> #[trigger] after(target) == before(target), ensures forall|target: int| #[trigger] after(target) == key_sum(rows.take(index + 1), key_of, target, weight), { assert forall|target: int| #[trigger] after(target) == key_sum(rows.take(index + 1), key_of, target, weight) by { lemma_sum_take(rows, index, |row: A| if key_of(row) == target { weight(row) } else { 0 }); assert(key_sum(rows.take(index + 1), key_of, target, weight) == key_sum(rows.take(index), key_of, target, weight) + (if key_of(rows[index]) == target { weight(rows[index]) } else { 0 })); } }
/// Use when maintaining a map domain alongside an executable live-key vector.
pub proof fn lemma_push_to_set<Value>(rows: Seq<Value>, value: Value) ensures rows.push(value).to_set() == rows.to_set().insert(value), { broadcast use vstd::seq_lib::group_seq_properties; rows.to_set_ensures(); rows.push(value).to_set_ensures(); assert(rows.push(value).to_set() =~= rows.to_set().insert(value)); }
/// Use when pruning a zero-valued cell from a duplicate-free support sequence.
pub proof fn lemma_unique_remove<Value>(rows: Seq<Value>, position: int) requires rows.no_duplicates(), 0 <= position < rows.len(), ensures rows.remove(position).no_duplicates(), { assert forall|left: int, right: int| 0 <= left < right < rows.remove(position).len() implies (#[trigger] rows.remove(position)[left]) != (#[trigger] rows.remove(position)[right]) by { let source_left = if left < position { left } else { left + 1 }; let source_right = if right < position { right } else { right + 1 }; assert(rows.remove(position)[left] == rows[source_left]); assert(rows.remove(position)[right] == rows[source_right]); } }
/// Use to reconstruct a cumulative measure from its neighboring-prefix differences.
pub proof fn lemma_prefix_differences(values: Seq<int>, bound: int) requires 0 <= bound <= values.len(), ensures sum(rng(bound), |index: int| values[index] - if index == 0 { 0 } else { values[index - 1] }) == if bound == 0 { 0 } else { values[bound - 1] }, decreases bound, { if bound > 0 { lemma_prefix_differences(values, bound - 1); lemma_range_step(bound - 1, |index: int| values[index] - if index == 0 { 0 } else { values[index - 1] }); } }
/// Use after a bag-counted emitter or sort preserves every weighted sum.
pub proof fn lemma_weighted_bag_equal<Value>(left: Seq<Value>, right: Seq<Value>) requires forall|weight: spec_fn(Value) -> int| #[trigger] sum(left, weight) == sum(right, weight), ensures left.to_multiset() == right.to_multiset(), { assert(left.to_multiset() =~= right.to_multiset()) by { assert forall|value: Value| #[trigger] left.to_multiset().count(value) == right.to_multiset().count(value) by { lemma_count_as_sum(left, value); lemma_count_as_sum(right, value); } } }
/// Use after an executable scan finds an insertion position separating all smaller and larger entries.
pub proof fn lemma_sorted_insert_at<Value>(items: Seq<Value>, position: int, value: Value, less: spec_fn(Value, Value) -> bool) requires 0 <= position <= items.len(), verus_common::order::sorted_by(items, less), forall|index: int| #![trigger items[index]] 0 <= index < position ==> less(items[index], value), forall|index: int| #![trigger items[index]] position <= index < items.len() ==> less(value, items[index]), ensures verus_common::order::sorted_by(items.insert(position, value), less), { let out = items.insert(position, value); assert forall|left: int, right: int| 0 <= left < right < out.len() implies less(#[trigger] out[left], #[trigger] out[right]) by { if left == position { assert(out[right] == items[right - 1]); } else if right == position { assert(out[left] == items[left]); } else { let old_left = if left < position { left } else { left - 1 }; let old_right = if right < position { right } else { right - 1 }; assert(out[left] == items[old_left]); assert(out[right] == items[old_right]); } } }
/// Use when every inner bag of a generated FROM product is empty.
pub proof fn lemma_flat_empty<Value>(values: Seq<Seq<Value>>) requires forall|index: int| 0 <= index < values.len() ==> #[trigger] values[index].len() == 0, ensures verus_common::rel::flat(values).len() == 0, decreases values.len(), { if values.len() > 0 { assert(values.last() == values[values.len() - 1]); assert forall|index: int| 0 <= index < values.drop_last().len() implies #[trigger] values.drop_last()[index].len() == 0 by { assert(values.drop_last()[index] == values[index]); } lemma_flat_empty(values.drop_last()); } }
/// Use when every row is rejected by the predicate of a generated filtered projection.
pub proof fn lemma_map_where_empty<Source, Target>(values: Seq<Source>, keep: spec_fn(Source) -> bool, project: spec_fn(Source) -> Target) requires forall|index: int| 0 <= index < values.len() ==> !keep(#[trigger] values[index]), ensures verus_common::order::map_where(values, keep, project).len() == 0, decreases values.len(), { if values.len() > 0 { assert(values.last() == values[values.len() - 1]); assert forall|index: int| 0 <= index < values.drop_last().len() implies !keep(#[trigger] values.drop_last()[index]) by { assert(values.drop_last()[index] == values[index]); } lemma_map_where_empty(values.drop_last(), keep, project); } }
/// Use to enumerate disjoint persistent pages without introducing duplicate live keys.
pub proof fn lemma_flat_unique<Value>(pages: Seq<Seq<Value>>) requires forall|position: int| 0 <= position < pages.len() ==> (#[trigger] pages[position]).no_duplicates(), forall|left: int, right: int, value: Value| #![trigger pages[left].contains(value), pages[right].contains(value)] 0 <= left < right < pages.len() ==> !(pages[left].contains(value) && pages[right].contains(value)), ensures verus_common::rel::flat(pages).no_duplicates(), decreases pages.len(), { if pages.len() > 0 { let prefix = pages.drop_last(); let last = pages.last(); assert forall|position: int| 0 <= position < prefix.len() implies (#[trigger] prefix[position]).no_duplicates() by { assert(prefix[position] == pages[position]); } assert forall|left: int, right: int, value: Value| #![trigger prefix[left].contains(value), prefix[right].contains(value)] 0 <= left < right < prefix.len() implies !(prefix[left].contains(value) && prefix[right].contains(value)) by { assert(prefix[left] == pages[left]); assert(prefix[right] == pages[right]); } lemma_flat_unique(prefix); let earlier = verus_common::rel::flat(prefix); assert forall|left: int, right: int| 0 <= left < right < verus_common::rel::flat(pages).len() implies (#[trigger] verus_common::rel::flat(pages)[left]) != (#[trigger] verus_common::rel::flat(pages)[right]) by { if right < earlier.len() { assert(verus_common::rel::flat(pages)[left] == earlier[left]); assert(verus_common::rel::flat(pages)[right] == earlier[right]); } else if left >= earlier.len() { assert(verus_common::rel::flat(pages)[left] == last[left - earlier.len()]); assert(verus_common::rel::flat(pages)[right] == last[right - earlier.len()]); } else { let value = earlier[left]; assert(earlier.contains(value)); verus_common::rel::lemma_flat_contains(prefix, value); let position = choose|position: int| 0 <= position < prefix.len() && (#[trigger] prefix[position]).contains(value); assert(pages[position].contains(value)); assert(last.contains(last[right - earlier.len()])); assert(last == pages[pages.len() as int - 1]); } } } }
/// Use to advance a weighted count prefix after adding one multiplicity.
pub proof fn lemma_scalar_distribute(prefix: int, term: int, scale: int)
    ensures (prefix + term) * scale == prefix * scale + term * scale,
{
    assert((prefix + term) * scale == prefix * scale + term * scale) by (nonlinear_arith);
}
/// Use to match a checked join-product chain to a grouped-prefix coefficient.
pub proof fn lemma_join_product(first: int, second: int, third: int, sign: int, measure: int)
    ensures (((first * second) * third) * sign) * measure == third * (first * (second * (sign * measure))),
{
    assert((((first * second) * third) * sign) * measure == third * (first * (second * (sign * measure)))) by (nonlinear_arith);
}
/// Use for a histogram insertion that changes a weighted sum by exactly the inserted cell.
pub proof fn lemma_sum_insert<Source>(values: Seq<Source>, position: int, value: Source, weight: spec_fn(Source) -> int) requires 0 <= position <= values.len(), ensures sum(values.insert(position, value), weight) == sum(values, weight) + weight(value), { let left = values.take(position); let right = values.skip(position); assert(values =~= left + right); assert(values.insert(position, value) =~= left.push(value) + right); lemma_sum_concat(left, right, weight); lemma_sum_concat(left.push(value), right, weight); lemma_sum_push(left, value, weight); }
/// Use for a fixed join multiplicity multiplying a newly extended inner sum.
pub proof fn lemma_left_scalar_distribute(prefix: int, term: int, scale: int) ensures scale * (prefix + term) == scale * prefix + scale * term, { assert(scale * (prefix + term) == scale * prefix + scale * term) by (nonlinear_arith); }
/// Use to move a transaction sign from outside a grouped join into its coefficient.
pub proof fn lemma_group_coefficient(first: int, second: int, sign: int, measure: int) ensures sign * ((first * second) * measure) == first * (second * (sign * measure)), ((first * second) * sign) * measure == first * (second * (sign * measure)), { assert(sign * ((first * second) * measure) == first * (second * (sign * measure))) by (nonlinear_arith); assert(((first * second) * sign) * measure == first * (second * (sign * measure))) by (nonlinear_arith); }
/// Last replacement for a key in an immutable shared-bucket overlay.
/// Origin: Q19 persistent-page storage; independent of database row types.
pub open spec fn bucket_overlay_value<T>(edits: Seq<(u64, std::sync::Arc<Vec<T>>)>, key: u64) -> Option<Seq<T>> decreases edits.len(), { if edits.len() == 0 { None } else if edits.last().0 == key { Some((*edits.last().1)@) } else { bucket_overlay_value(edits.drop_last(), key) } }
pub open spec fn bucket_overlay_keys<T>(edits: Seq<(u64, std::sync::Arc<Vec<T>>)>) -> Seq<u64> { edits.map_values(|entry: (u64, std::sync::Arc<Vec<T>>)| entry.0) }
/// Use when appending a replacement to a persistent bucket overlay.
/// Origin: Q19 persistent-page replacement proof.
pub proof fn lemma_bucket_overlay_push<T>(edits: Seq<(u64, std::sync::Arc<Vec<T>>)>, entry: (u64, std::sync::Arc<Vec<T>>), key: u64) ensures bucket_overlay_value(edits.push(entry), key) == if entry.0 == key { Some((*entry.1)@) } else { bucket_overlay_value(edits, key) }, { broadcast use vstd::seq_lib::group_seq_properties; assert(edits.push(entry).drop_last() =~= edits); }
/// Use to fall through to the immutable base when an overlay does not contain a key.
/// Origin: Q19 persistent-page lookup and compaction proofs.
pub proof fn lemma_bucket_overlay_missing<T>(edits: Seq<(u64, std::sync::Arc<Vec<T>>)>, key: u64) requires !bucket_overlay_keys(edits).contains(key), ensures bucket_overlay_value(edits, key) == None, decreases edits.len(), { broadcast use vstd::seq_lib::group_seq_properties; if edits.len() > 0 { assert(bucket_overlay_keys(edits)[edits.len() - 1] == edits.last().0); assert(bucket_overlay_keys(edits.drop_last()) =~= bucket_overlay_keys(edits).drop_last()); lemma_bucket_overlay_missing(edits.drop_last(), key); } }
pub open spec fn unique_key_lookup<Key, Value>(rows: Seq<(Key, Value)>, key: Key) -> Option<Value> { if exists|index: int| #![trigger rows[index]] 0 <= index < rows.len() && rows[index].0 == key { let index = choose|index: int| #![trigger rows[index]] 0 <= index < rows.len() && rows[index].0 == key; Some(rows[index].1) } else { None } }
pub proof fn lemma_unique_key_lookup<Key, Value>(rows: Seq<(Key, Value)>, key: Key) requires rows.map_values(|entry: (Key, Value)| entry.0).no_duplicates(), ensures unique_key_lookup(rows, key).is_none() <==> !rows.map_values(|entry: (Key, Value)| entry.0).contains(key), forall|index: int| #![trigger rows[index]] 0 <= index < rows.len() && rows[index].0 == key ==> unique_key_lookup(rows, key) == Some(rows[index].1), { let keys = rows.map_values(|entry: (Key, Value)| entry.0); if keys.contains(key) { let index = choose|index: int| #![trigger keys[index]] 0 <= index < keys.len() && keys[index] == key; assert(rows[index].0 == key); } if unique_key_lookup(rows, key).is_some() { let selected = choose|index: int| #![trigger rows[index]] 0 <= index < rows.len() && rows[index].0 == key; assert(0 <= selected < rows.len() && rows[selected].0 == key); assert(keys[selected] == key); assert(keys.contains(key)); assert forall|index: int| #![trigger rows[index]] 0 <= index < rows.len() && rows[index].0 == key implies unique_key_lookup(rows, key) == Some(rows[index].1) by { assert(keys[index] == keys[selected]); assert(index == selected); } } }
use verus_common::rel::*;
/// Use when relating COUNT(*) to a weighted sum over a sequence.
pub proof fn lemma_sum_constant_one<A>(rows: Seq<A>)
    ensures sum(rows, |row: A| 1int) == rows.len(),
                                decreases rows.len(),
                                {
    if rows.len() > 0 { lemma_sum_constant_one(rows.drop_last()); }
                                }
/// Use when a range seek bypasses a segment whose contributions are all zero.
pub proof fn lemma_sum_prefix_skip_zero<A>(rows: Seq<A>, begin: int, end: int, weight: spec_fn(A) -> int)
                                requires
        0 <= begin <= end <= rows.len(),
        forall|index: int| begin <= index < end ==> weight(#[trigger] rows[index]) == 0,
    ensures sum(rows.take(begin), weight) == sum(rows.take(end), weight),
    decreases end - begin,
                                {
    if begin < end {
        lemma_sum_prefix_skip_zero(rows, begin, end - 1, weight);
        lemma_sum_take(rows, end - 1, weight);
                                    }
                                }
/// Persistent ordered maps for generic values and integer/byte-string composite keys.
/// Origin: the verified Arc-shared AVL index copied into lift_source/index.rs.
/// Sharing is O(1); lookup, updates and lower-bound seeks traverse O(log n) nodes
/// in the AVL tree. Key comparisons additionally cost O(common byte prefix).
/// Updates allocate only along the changed path, preserving shared versions.
/// A full traversal costs O(n), with O(log n) cursor space.
/// Import with `use verus_prooflib::ordered::{Key, Index, Cursor};`.
/// For branching versions, call `let mut newer = old.share()` and update newer.
/// Range reads seek a lower bound and stop before the exclusive upper bound.
/// Costs count key comparisons; the source AVL rotations and height maintenance
/// are retained, while the verified contracts certify ordering and map contents.
pub mod ordered {
                    use vstd::prelude::*;
                    use std::sync::Arc;
                    use verus_common::bytes::*;
                    use verus_common::sums::*;
                    verus! {
                    /// Composite key: signed integer first, then lexicographic bytes (binary collation).
                    pub struct Key {
                        pub number: i64,
                        pub text: Arc<Vec<u8>>,
                    }
                    pub type KeyView = (int, Seq<u8>);
                    impl Key {
                        pub open spec fn view(self) -> KeyView {
                            (self.number as int, (*self.text)@)
                        }
                        /// Construct an integer/byte composite without copying the byte buffer, in O(1).
                        pub fn composite(number: i64, text: Vec<u8>) -> (key: Self)
                            ensures key.view() == (number as int, text@),
                        {
                            Key { number, text: Arc::new(text) }
                        }
                        /// Construct an integer/byte composite sharing the byte buffer, in O(1).
                        pub fn shared(number: i64, text: Arc<Vec<u8>>) -> (key: Self)
                            ensures key.view() == (number as int, (*text)@),
                        {
                            Key { number, text }
                        }
                        /// Construct an integer-only key in O(1).
                        pub fn number(number: i64) -> (key: Self)
                            ensures key.view() == (number as int, Seq::<u8>::empty()),
                        {
                            Key { number, text: Arc::new(Vec::new()) }
                        }
                        /// Construct a byte-only key without copying its buffer, in O(1).
                        pub fn text(text: Vec<u8>) -> (key: Self)
                            ensures key.view() == (0int, text@),
                        {
                            Key { number: 0, text: Arc::new(text) }
                        }
                        /// Share the key and its byte buffer in O(1).
                        pub fn share(&self) -> (key: Self)
                            ensures key.view() == self.view(),
                        {
                            Key { number: self.number, text: self.text.clone() }
                        }
                        /// Replace the integer component while sharing the bytes, in O(1).
                        pub fn with_number(&self, number: i64) -> (key: Self)
                            ensures key.view() == (number as int, self.view().1),
                        {
                            Key { number, text: self.text.clone() }
                        }
                    }
                    pub open spec fn key_lt(left: KeyView, right: KeyView) -> bool {
                        left.0 < right.0 || (left.0 == right.0 && bytes_lt(left.1, right.1))
                    }
                    pub proof fn lemma_key_order(left: KeyView, right: KeyView)
                        ensures
                            !key_lt(left, left),
                            !(key_lt(left, right) && key_lt(right, left)),
                            left != right ==> key_lt(left, right) || key_lt(right, left),
                    {
                        lemma_bytes_lt_irrefl(left.1);
                        lemma_bytes_lt_asym(left.1, right.1);
                        if left.0 == right.0 && left != right {
                            if left.1 == right.1 { assert(left == right); }
                            lemma_bytes_lt_total(left.1, right.1);
                        }
                    }
                    pub proof fn lemma_key_trans(first: KeyView, middle: KeyView, last: KeyView)
                        requires key_lt(first, middle), key_lt(middle, last),
                        ensures key_lt(first, last),
                    {
                        if first.0 == middle.0 && middle.0 == last.0 {
                            lemma_bytes_lt_trans(first.1, middle.1, last.1);
                        }
                    }
                    pub fn compare(left: &Key, right: &Key) -> (order: i8)
                        ensures
                            (order < 0) == key_lt(left.view(), right.view()),
                            (order == 0) == (left.view() == right.view()),
                            (order > 0) == key_lt(right.view(), left.view()),
                    {
                        if left.number < right.number { return -1; }
                        if left.number > right.number { return 1; }
                        compare_bytes(left.text.as_slice(), right.text.as_slice())
                    }
                    pub fn compare_bytes(left: &[u8], right: &[u8]) -> (order: i8)
                        ensures
                            (order < 0) == bytes_lt(left@, right@),
                            (order == 0) == (left@ == right@),
                            (order > 0) == bytes_lt(right@, left@),
                    {
                        if bytes_eq_exec(left, right) {
                            proof { lemma_bytes_lt_irrefl(left@); }
                            return 0;
                        }
                        proof {
                            lemma_bytes_lt_total(left@, right@);
                            lemma_bytes_lt_asym(left@, right@);
                        }
                        if bytes_lt_exec(left, right) { -1 } else { 1 }
                    }
                    pub enum Tree<Value> {
                        Empty,
                        Branch {
                            key: Key,
                            value: Arc<Value>,
                            left: Arc<Tree<Value>>,
                            right: Arc<Tree<Value>>,
                            height: usize,
                        },
                    }
                    /// Persistent AVL map; values need no Clone, comparison, or equality trait.
                    pub struct Index<Value> {
                        root: Arc<Tree<Value>>,
                    }
                    /// Ascending snapshot cursor. It owns shared nodes and remains valid after map updates.
                    pub struct Cursor<Value> {
                        stack: Vec<Arc<Tree<Value>>>,
                        current: Arc<Tree<Value>>,
                    }
                    impl<Value> Cursor<Value> {
                        pub closed spec fn work(self) -> int { pending_work(*self.current, self.stack@) }
                        pub closed spec fn remaining(self) -> Seq<(KeyView, Value)> {
                            (*self.current).entries() + pending_entries(self.stack@)
                        }
                        #[verifier::rlimit(20)]
                        /// Return the next ascending entry; O(log n) worst case, O(1) amortized.
                        pub fn next(&mut self) -> (result: Option<(Key, Arc<Value>)>)
                            ensures
                                final(self).work() >= 0,
                                match result {
                                    Some(row) => {
                                        &&& old(self).remaining().len() > 0
                                        &&& (row.0.view(), *row.1) == old(self).remaining()[0]
                                        &&& final(self).remaining() == old(self).remaining().skip(1)
                                        &&& final(self).work() < old(self).work()
                                    },
                                    None => old(self).remaining().len() == 0 && final(self).remaining().len() == 0,
                                },
                        {
                            let ghost initial_remaining = self.remaining();
                            let ghost initial_work = self.work();
                            proof { lemma_pending_nonnegative(&*self.current, self.stack@); }
                            loop
                                invariant
                                    initial_remaining == old(self).remaining(),
                                    initial_work == old(self).work(),
                                    self.remaining() == initial_remaining,
                                    0 <= self.work() <= initial_work,
                                decreases pending_work(*self.current, self.stack@),
                            {
                                match &*self.current {
                                    Tree::Branch { left, .. } => {
                                        let next = left.clone();
                                        let ghost stack_before = self.stack@;
                                        self.stack.push(self.current.clone());
                                        self.current = next;
                                        proof {
                                            lemma_sum_push(stack_before, self.stack@.last(), |node: Arc<Tree<Value>>| pending_cost(node));
                                            assert(self.stack@.drop_last() == stack_before);
                                            lemma_pending_nonnegative(&*self.current, self.stack@);
                                            assert(self.remaining() == initial_remaining);
                                        }
                                    },
                                    Tree::Empty => {
                                        let ghost stack_before = self.stack@;
                                        let node = match self.stack.pop() {
                                            Some(node) => node,
                                            None => {
                                                proof {
                                                    assert(initial_remaining.len() == 0);
                                                    assert(self.remaining().len() == 0);
                                                }
                                                return None;
                                            },
                                        };
                                        proof {
                                            assert(stack_before == self.stack@.push(node));
                                            lemma_sum_push(self.stack@, node, |node: Arc<Tree<Value>>| pending_cost(node));
                                        }
                                        match &*node {
                                            Tree::Empty => {
                                                proof { lemma_pending_nonnegative(&*self.current, self.stack@); }
                                            },
                                            Tree::Branch { key, value, right, .. } => {
                                                self.current = right.clone();
                                                proof {
                                                    lemma_pending_nonnegative(&*self.current, self.stack@);
                                                    assert(initial_remaining =~= seq![(key.view(), **value)] + self.remaining());
                                                    assert(self.remaining() =~= initial_remaining.skip(1));
                                                    assert(initial_remaining.len() > 0);
                                                    assert(initial_remaining[0] == (key.view(), **value));
                                                    assert(self.work() < initial_work);
                                                }
                                                return Some((key.share(), value.clone()));
                                            },
                                        }
                                    },
                                }
                            }
                        }
                    }
                    impl<Value> Tree<Value> {
                        pub open spec fn at(self, query: KeyView) -> Option<Value>
                            decreases self,
                        {
                            match self {
                                Tree::Empty => None,
                                Tree::Branch { key, value, left, right, .. } => {
                                    if query == key.view() { Some(*value) }
                                    else if key_lt(query, key.view()) { (*left).at(query) }
                                    else { (*right).at(query) }
                                },
                            }
                        }
                        pub open spec fn size(self) -> nat
                            decreases self,
                        {
                            match self {
                                Tree::Empty => 0,
                                Tree::Branch { left, right, .. } => 1 + (*left).size() + (*right).size(),
                            }
                        }
                        pub open spec fn entries(self) -> Seq<(KeyView, Value)>
                            decreases self,
                        {
                            match self {
                                Tree::Empty => Seq::empty(),
                                Tree::Branch { key, value, left, right, .. } =>
                                    (*left).entries() + seq![(key.view(), *value)] + (*right).entries(),
                            }
                        }
                        pub open spec fn depth(self) -> nat
                            decreases self,
                        {
                            match self {
                                Tree::Empty => 0,
                                Tree::Branch { left, right, .. } =>
                                    1 + if (*left).depth() > (*right).depth() { (*left).depth() } else { (*right).depth() },
                            }
                        }
                        pub open spec fn wf(self) -> bool
                            decreases self,
                        {
                            match self {
                                Tree::Empty => true,
                                Tree::Branch { key, left, right, .. } => {
                                    &&& (*left).wf() && (*right).wf()
                                    &&& forall|index: int| #![auto] 0 <= index < (*left).entries().len() ==>
                                        key_lt((*left).entries()[index].0, key.view())
                                    &&& forall|index: int| #![auto] 0 <= index < (*right).entries().len() ==>
                                        key_lt(key.view(), (*right).entries()[index].0)
                                },
                            }
                        }
                    }
                    pub open spec fn pending_cost<Value>(node: Arc<Tree<Value>>) -> int {
                        match *node {
                            Tree::Empty => 1,
                            Tree::Branch { right, .. } => 1 + 2 * (*right).size() as int,
                        }
                    }
                    pub open spec fn pending_work<Value>(current: Tree<Value>, stack: Seq<Arc<Tree<Value>>>) -> int {
                        2 * current.size() + sum(stack, |node: Arc<Tree<Value>>| pending_cost(node))
                    }
                    pub proof fn lemma_pending_nonnegative<Value>(current: &Tree<Value>, stack: Seq<Arc<Tree<Value>>>)
                        ensures pending_work(*current, stack) >= 0,
                    {
                        assert forall|position: int| #![trigger stack[position]] 0 <= position < stack.len() implies pending_cost(stack[position]) >= 0 by {};
                        lemma_sum_nonneg(stack, |node: Arc<Tree<Value>>| pending_cost(node));
                    }
                    pub open spec fn pending_entries<Value>(stack: Seq<Arc<Tree<Value>>>) -> Seq<(KeyView, Value)>
                        decreases stack.len(),
                    {
                        if stack.len() == 0 { Seq::empty() } else {
                            let tail = match *stack.last() {
                                Tree::Empty => Seq::empty(),
                                Tree::Branch { key, value, right, .. } => seq![(key.view(), *value)] + (*right).entries(),
                            };
                            tail + pending_entries(stack.drop_last())
                        }
                    }
                    pub open spec fn ordered_rows<Value>(rows: Seq<(KeyView, Value)>) -> bool {
                        forall|first: int, last: int| #![auto] 0 <= first < last < rows.len() ==> key_lt(rows[first].0, rows[last].0)
                    }
                    pub open spec fn rows_of<Value>(tree: &Tree<Value>) -> Seq<(KeyView, Value)> { tree.entries() }
                    pub open spec fn branch_row<Value>(key: &Key, value: &Arc<Value>) -> Seq<(KeyView, Value)> { seq![(key.view(), **value)] }
                    pub open spec fn drop_key<Value>(rows: Seq<(KeyView, Value)>, key: KeyView) -> Seq<(KeyView, Value)>
                        decreases rows.len(),
                    {
                        if rows.len() == 0 { Seq::empty() }
                        else if rows.last().0 == key { drop_key(rows.drop_last(), key) }
                        else { drop_key(rows.drop_last(), key).push(rows.last()) }
                    }
                    pub proof fn lemma_push_contains<Value>(rows: Seq<Value>, last: Value, query: Value)
                        ensures rows.push(last).contains(query) <==> rows.contains(query) || last == query,
                    {
                        if rows.contains(query) {
                            let position = choose|position: int| 0 <= position < rows.len() && rows[position] == query;
                            assert(rows.push(last)[position] == query);
                        }
                        if last == query { assert(rows.push(last)[rows.len() as int] == query); }
                        if rows.push(last).contains(query) {
                            let position = choose|position: int| 0 <= position < rows.push(last).len() && rows.push(last)[position] == query;
                            if position < rows.len() { assert(rows[position] == query); }
                            else { assert(last == query); }
                        }
                    }
                    pub proof fn lemma_drop_concat<Value>(left: Seq<(KeyView, Value)>, right: Seq<(KeyView, Value)>, key: KeyView)
                        ensures drop_key(left + right, key) == drop_key(left, key) + drop_key(right, key),
                        decreases right.len(),
                    {
                        if right.len() == 0 { assert(left + right =~= left); }
                        else {
                            assert((left + right).drop_last() =~= left + right.drop_last());
                            lemma_drop_concat(left, right.drop_last(), key);
                            if right.last().0 != key {
                                assert((drop_key(left, key) + drop_key(right.drop_last(), key)).push(right.last()) =~=
                                    drop_key(left, key) + drop_key(right.drop_last(), key).push(right.last()));
                            }
                        }
                    }
                    pub proof fn lemma_drop_absent<Value>(rows: Seq<(KeyView, Value)>, key: KeyView)
                        requires forall|position: int| #![auto] 0 <= position < rows.len() ==> rows[position].0 != key,
                        ensures drop_key(rows, key) == rows,
                        decreases rows.len(),
                    {
                        if rows.len() > 0 {
                            lemma_drop_absent(rows.drop_last(), key);
                            assert(rows =~= rows.drop_last().push(rows.last()));
                        }
                    }
                    pub proof fn lemma_drop_bindings<Value>(rows: Seq<(KeyView, Value)>, key: KeyView)
                        ensures forall|row: (KeyView, Value)| #![trigger drop_key(rows, key).contains(row)]
                            drop_key(rows, key).contains(row) <==> rows.contains(row) && row.0 != key,
                        decreases rows.len(),
                    {
                        if rows.len() > 0 {
                            lemma_drop_bindings(rows.drop_last(), key);
                            assert(rows =~= rows.drop_last().push(rows.last()));
                            assert forall|row: (KeyView, Value)| #![trigger drop_key(rows, key).contains(row)]
                                drop_key(rows, key).contains(row) <==> rows.contains(row) && row.0 != key by {
                                lemma_push_contains(rows.drop_last(), rows.last(), row);
                                assert(drop_key(rows.drop_last(), key).contains(row) <==> rows.drop_last().contains(row) && row.0 != key);
                                if rows.last().0 != key {
                                    lemma_push_contains(drop_key(rows.drop_last(), key), rows.last(), row);
                                }
                            }
                        }
                    }
                    pub proof fn lemma_drop_ordered<Value>(rows: Seq<(KeyView, Value)>, key: KeyView)
                        requires ordered_rows(rows),
                        ensures ordered_rows(drop_key(rows, key)),
                        decreases rows.len(),
                    {
                        if rows.len() > 0 {
                            let prefix = rows.drop_last();
                            assert(ordered_rows(prefix));
                            lemma_drop_ordered(prefix, key);
                            lemma_drop_bindings(prefix, key);
                            if rows.last().0 != key {
                                let retained = drop_key(prefix, key);
                                assert forall|first: int, last: int| #![auto] 0 <= first < last < retained.len() + 1 implies
                                    key_lt(retained.push(rows.last())[first].0, retained.push(rows.last())[last].0) by {
                                    if last < retained.len() { assert(ordered_rows(retained)); }
                                    else {
                                        assert(retained.contains(retained[first]));
                                        let position = choose|position: int| 0 <= position < prefix.len() && prefix[position] == retained[first];
                                        assert(prefix[position] == rows[position]);
                                        assert(key_lt(rows[position].0, rows.last().0));
                                    }
                                }
                            }
                        }
                    }
                    pub proof fn lemma_drop_first<Value>(rows: Seq<(KeyView, Value)>)
                        requires ordered_rows(rows), rows.len() > 0,
                        ensures drop_key(rows, rows[0].0) == rows.skip(1),
                    {
                        reveal_with_fuel(drop_key, 2);
                        let tail = rows.skip(1);
                        assert forall|position: int| #![auto] 0 <= position < tail.len() implies tail[position].0 != rows[0].0 by {
                            assert(tail[position] == rows[position + 1]);
                            lemma_key_order(rows[0].0, tail[position].0);
                        }
                        lemma_drop_absent(tail, rows[0].0);
                        assert(rows =~= seq![rows[0]] + tail);
                        lemma_drop_concat(seq![rows[0]], tail, rows[0].0);
                    }
                    pub proof fn lemma_join_ordered<Value>(left: Seq<(KeyView, Value)>, key: KeyView, value: &Value, right: Seq<(KeyView, Value)>)
                        requires
                            ordered_rows(left), ordered_rows(right),
                            forall|position: int| #![auto] 0 <= position < left.len() ==> key_lt(left[position].0, key),
                            forall|position: int| #![auto] 0 <= position < right.len() ==> key_lt(key, right[position].0),
                        ensures ordered_rows(left + seq![(key, *value)] + right),
                    {
                        let rows = left + seq![(key, *value)] + right;
                        assert forall|first: int, last: int| #![auto] 0 <= first < last < rows.len() implies key_lt(rows[first].0, rows[last].0) by {
                            if last < left.len() {
                                assert(rows[first] == left[first] && rows[last] == left[last]);
                            } else if first > left.len() {
                                assert(rows[first] == right[first - left.len() - 1]);
                                assert(rows[last] == right[last - left.len() - 1]);
                            } else if first == left.len() {
                                assert(rows[first].0 == key);
                                assert(rows[last] == right[last - left.len() - 1]);
                            } else if last == left.len() {
                                assert(rows[first] == left[first]);
                                assert(rows[last].0 == key);
                            } else {
                                assert(rows[first] == left[first]);
                                assert(rows[last] == right[last - left.len() - 1]);
                                lemma_key_trans(rows[first].0, key, rows[last].0);
                            }
                        }
                    }
                    pub proof fn lemma_wf_rows<Value>(tree: &Tree<Value>)
                        ensures tree.wf() <==> ordered_rows(tree.entries()),
                        decreases tree.depth(),
                    {
                        match tree {
                            Tree::Empty => {},
                            Tree::Branch { key, value, left, right, .. } => {
                                lemma_wf_rows(&**left);
                                lemma_wf_rows(&**right);
                                let rows = tree.entries();
                                let low = rows_of(&**left);
                                let high = rows_of(&**right);
                                if tree.wf() {
                                    lemma_join_ordered(low, key.view(), &**value, high);
                                }
                                if ordered_rows(rows) {
                                    assert forall|first: int, last: int| #![auto] 0 <= first < last < low.len() implies key_lt(low[first].0, low[last].0) by {
                                        assert(low[first] == rows[first]);
                                        assert(low[last] == rows[last]);
                                    }
                                    assert forall|first: int, last: int| #![auto] 0 <= first < last < high.len() implies key_lt(high[first].0, high[last].0) by {
                                        assert(high[first] == rows[low.len() + 1 + first]);
                                        assert(high[last] == rows[low.len() + 1 + last]);
                                    }
                                    assert(rows[low.len() as int].0 == key.view());
                                    assert forall|position: int| #![auto] 0 <= position < low.len() implies key_lt(low[position].0, key.view()) by {
                                        assert(low[position] == rows[position]);
                                    }
                                    assert forall|position: int| #![auto] 0 <= position < high.len() implies key_lt(key.view(), high[position].0) by {
                                        assert(high[position] == rows[low.len() + 1 + position]);
                                    }
                                }
                            },
                        }
                    }
                    pub proof fn lemma_at_rows<Value>(tree: &Tree<Value>, query: KeyView)
                        requires tree.wf(),
                        ensures
                            forall|position: int| #![auto] 0 <= position < tree.entries().len() && tree.entries()[position].0 == query ==>
                                tree.at(query) == Some(tree.entries()[position].1),
                            tree.at(query).is_some() <==> exists|position: int| #![auto] 0 <= position < tree.entries().len() && tree.entries()[position].0 == query,
                        decreases tree.depth(),
                    {
                        match tree {
                            Tree::Empty => {},
                            Tree::Branch { key, left, right, .. } => {
                                lemma_at_rows(&**left, query);
                                lemma_at_rows(&**right, query);
                                lemma_key_order(query, key.view());
                                let rows = tree.entries();
                                let low = rows_of(&**left);
                                let high = rows_of(&**right);
                                assert forall|position: int| #![auto] 0 <= position < rows.len() && rows[position].0 == query implies tree.at(query) == Some(rows[position].1) by {
                                    if position < low.len() {
                                        assert(rows[position] == low[position]);
                                        assert(key_lt(query, key.view()));
                                    } else if position == low.len() {
                                        assert(rows[position].0 == key.view());
                                    } else {
                                        let offset = position - low.len() - 1;
                                        assert(rows[position] == high[offset]);
                                        assert(key_lt(key.view(), query));
                                    }
                                }
                                if tree.at(query).is_some() {
                                    if query == key.view() {
                                        assert(rows[low.len() as int].0 == query);
                                    } else if key_lt(query, key.view()) {
                                        let position = choose|position: int| #![auto] 0 <= position < low.len() && low[position].0 == query;
                                        assert(rows[position].0 == query);
                                    } else {
                                        let position = choose|position: int| #![auto] 0 <= position < high.len() && high[position].0 == query;
                                        assert(rows[low.len() + 1 + position].0 == query);
                                    }
                                }
                            },
                        }
                    }
                    pub proof fn lemma_same_rows<Value>(first: &Tree<Value>, second: &Tree<Value>)
                        requires first.wf(), second.wf(), first.entries() == second.entries(),
                        ensures forall|query: KeyView| #![trigger first.at(query), second.at(query)] first.at(query) == second.at(query),
                    {
                        hide(Tree::at);
                        assert forall|query: KeyView| #![trigger first.at(query), second.at(query)] first.at(query) == second.at(query) by {
                            lemma_at_rows(first, query);
                            lemma_at_rows(second, query);
                            if first.at(query).is_some() {
                                let position = choose|position: int| #![auto] 0 <= position < first.entries().len() && first.entries()[position].0 == query;
                                assert(second.entries()[position].0 == query);
                            } else if second.at(query).is_some() {
                                let position = choose|position: int| #![auto] 0 <= position < second.entries().len() && second.entries()[position].0 == query;
                                assert(first.entries()[position].0 == query);
                            }
                        }
                    }
                    pub open spec fn joined<Value>(key: Key, value: Arc<Value>, left: Arc<Tree<Value>>, right: Arc<Tree<Value>>) -> Tree<Value> {
                        Tree::Branch { key, value, left, right, height: 0 }
                    }
                    fn finish_balance<Value>(result: Arc<Tree<Value>>, Ghost(expected): Ghost<Tree<Value>>) -> (output: Arc<Tree<Value>>)
                        requires (*result).entries() == expected.entries(),
                        ensures
                            output == result,
                            (*output).wf() == expected.wf(),
                            expected.wf() ==> forall|query: KeyView| #![trigger (*output).at(query), expected.at(query)] (*output).at(query) == expected.at(query),
                    {
                        proof {
                            lemma_wf_rows(&*result);
                            lemma_wf_rows(&expected);
                            if expected.wf() { lemma_same_rows(&*result, &expected); }
                        }
                        result
                    }
                    #[verifier::rlimit(20)]
                    fn finish_erase<Value>(result: Arc<Tree<Value>>, original: &Arc<Tree<Value>>, key: &Key) -> (output: Arc<Tree<Value>>)
                        requires (**original).wf(), (*result).entries() == drop_key((**original).entries(), key.view()),
                        ensures
                            output == result, (*output).wf(),
                            (*output).entries() == drop_key((**original).entries(), key.view()),
                            forall|query: KeyView| #![trigger (*output).at(query)] (*output).at(query) == if query == key.view() { None } else { (**original).at(query) },
                    {
                        proof {
                            lemma_wf_rows(&**original);
                            lemma_drop_ordered(rows_of(&**original), key.view());
                            lemma_wf_rows(&*result);
                            lemma_drop_bindings(rows_of(&**original), key.view());
                            assert forall|query: KeyView| #![trigger (*result).at(query)] (*result).at(query) == if query == key.view() { None } else { (**original).at(query) } by {
                                lemma_at_rows(&**original, query);
                                lemma_at_rows(&*result, query);
                                if query != key.view() && (**original).at(query).is_some() {
                                    let old_position = choose|position: int| #![auto] 0 <= position < (**original).entries().len() && (**original).entries()[position].0 == query;
                                    let row = rows_of(&**original)[old_position];
                                    assert((**original).entries().contains(row));
                                    assert((*result).entries().contains(row));
                                    let new_position = choose|position: int| #![auto] 0 <= position < (*result).entries().len() && (*result).entries()[position] == row;
                                } else if (*result).at(query).is_some() {
                                    let new_position = choose|position: int| #![auto] 0 <= position < (*result).entries().len() && (*result).entries()[position].0 == query;
                                    let row = rows_of(&*result)[new_position];
                                    assert((*result).entries().contains(row));
                                    assert((**original).entries().contains(row) && row.0 != key.view());
                                    let old_position = choose|position: int| #![auto] 0 <= position < (**original).entries().len() && (**original).entries()[position] == row;
                                }
                            }
                        }
                        result
                    }
                    /// Entries at or above a lower bound, retaining their original order.
                    pub open spec fn lower_rows<Value>(rows: Seq<(KeyView, Value)>, bound: KeyView) -> Seq<(KeyView, Value)>
                        decreases rows.len(),
                    {
                        if rows.len() == 0 { Seq::empty() }
                        else if key_lt(rows.last().0, bound) { lower_rows(rows.drop_last(), bound) }
                        else { lower_rows(rows.drop_last(), bound).push(rows.last()) }
                    }
                    /// Use to split a lower-bound read at a tree branch.
                    pub proof fn lemma_lower_concat<Value>(left: Seq<(KeyView, Value)>, right: Seq<(KeyView, Value)>, bound: KeyView)
                        ensures lower_rows(left + right, bound) == lower_rows(left, bound) + lower_rows(right, bound),
                        decreases right.len(),
                    {
                        if right.len() == 0 { assert(left + right =~= left); }
                        else {
                            assert((left + right).drop_last() =~= left + right.drop_last());
                            lemma_lower_concat(left, right.drop_last(), bound);
                            if !key_lt(right.last().0, bound) {
                                assert((lower_rows(left, bound) + lower_rows(right.drop_last(), bound)).push(right.last()) =~=
                                    lower_rows(left, bound) + lower_rows(right.drop_last(), bound).push(right.last()));
                            }
                        }
                    }
                    /// Use when a whole subtree is retained or bypassed by a lower-bound seek.
                    pub proof fn lemma_lower_segment<Value>(rows: Seq<(KeyView, Value)>, bound: KeyView)
                        ensures
                            (forall|position: int| #![auto] 0 <= position < rows.len() ==> !key_lt(rows[position].0, bound)) ==>
                                lower_rows(rows, bound) == rows,
                            (forall|position: int| #![auto] 0 <= position < rows.len() ==> key_lt(rows[position].0, bound)) ==>
                                lower_rows(rows, bound) == Seq::<(KeyView, Value)>::empty(),
                        decreases rows.len(),
                    {
                        if rows.len() > 0 {
                            lemma_lower_segment(rows.drop_last(), bound);
                            assert(rows =~= rows.drop_last().push(rows.last()));
                        }
                    }
                    /// Lower-bound filtering is exactly the ordered range suffix (no qualifying entry is lost).
                    pub proof fn lemma_lower_bindings<Value>(rows: Seq<(KeyView, Value)>, bound: KeyView)
                        ensures forall|row: (KeyView, Value)| #![trigger lower_rows(rows, bound).contains(row)]
                            lower_rows(rows, bound).contains(row) <==> rows.contains(row) && !key_lt(row.0, bound),
                        decreases rows.len(),
                    {
                        if rows.len() > 0 {
                            lemma_lower_bindings(rows.drop_last(), bound);
                            assert(rows =~= rows.drop_last().push(rows.last()));
                            assert forall|row: (KeyView, Value)| #![trigger lower_rows(rows, bound).contains(row)]
                                lower_rows(rows, bound).contains(row) <==> rows.contains(row) && !key_lt(row.0, bound) by {
                                lemma_push_contains(rows.drop_last(), rows.last(), row);
                                if !key_lt(rows.last().0, bound) {
                                    lemma_push_contains(lower_rows(rows.drop_last(), bound), rows.last(), row);
                                }
                            }
                        }
                    }
                    impl<Value> Index<Value> {
                        /// Seek the first key >= bound in O(log n) comparisons and O(log n) cursor space.
                        /// For [lo, hi), seek lo and stop when compare(&key, hi) >= 0.
                        /// Ranking keys can put a negated integer rank first and use bytes to break ties.
                        #[verifier::rlimit(40)]
                        pub fn cursor_from(&self, bound: &Key) -> (result: Cursor<Value>)
                        ensures result.remaining() == lower_rows(self.entries(), bound.view()), result.work() >= 0,
                            result.remaining() == self.entries().skip(lower_position(self.entries(), bound.view())),
                            ordered_rows(result.remaining()),
                        {
                            proof { use_type_invariant(self); }
                            let mut current = self.root.clone();
                            let mut stack: Vec<Arc<Tree<Value>>> = Vec::new();
                            loop
                            invariant
                                self.wf(),
                                (*current).wf(),
                                    lower_rows((*current).entries(), bound.view()) + pending_entries(stack@)
                                        == lower_rows(self.entries(), bound.view()),
                                decreases (*current).depth(),
                            {
                                match &*current {
                                Tree::Empty => {
                                    proof { lemma_pending_nonnegative(&*current, stack@); }
                                    proof { self.model(); lemma_lower_suffix(self.entries(), bound.view()); }
                                    return Cursor { stack, current };
                                    },
                                    Tree::Branch { key, value, left, right, .. } => {
                                        let ghost low = rows_of(&**left);
                                        let ghost middle = branch_row(key, value);
                                        let ghost high = rows_of(&**right);
                                        proof {
                                            lemma_lower_concat(low, middle, bound.view());
                                            lemma_lower_concat(low + middle, high, bound.view());
                                            lemma_key_order(key.view(), bound.view());
                                        }
                                        if compare(key, bound) < 0 {
                                            proof {
                                                assert forall|position: int| #![auto] 0 <= position < low.len() implies
                                                    key_lt(low[position].0, bound.view()) by {
                                                    lemma_key_trans(low[position].0, key.view(), bound.view());
                                                }
                                                lemma_lower_segment(low, bound.view());
                                                lemma_lower_segment(middle, bound.view());
                                                assert(lower_rows(low, bound.view()).len() == 0);
                                                assert(lower_rows(middle, bound.view()).len() == 0);
                                                assert(lower_rows((*current).entries(), bound.view()) == lower_rows(high, bound.view()));
                                            }
                                            current = right.clone();
                                        } else {
                                            proof {
                                                assert forall|position: int| #![auto] 0 <= position < high.len() implies
                                                    !key_lt(high[position].0, bound.view()) by {
                                                    if key_lt(bound.view(), key.view()) {
                                                        lemma_key_trans(bound.view(), key.view(), high[position].0);
                                                    }
                                                    lemma_key_order(bound.view(), high[position].0);
                                                }
                                                lemma_lower_segment(middle, bound.view());
                                                lemma_lower_segment(high, bound.view());
                                                assert(lower_rows(middle, bound.view()) == middle);
                                                assert(lower_rows(high, bound.view()) == high);
                                            }
                                            let next = left.clone();
                                            let ghost stack_before = stack@;
                                            stack.push(current.clone());
                                            current = next;
                                            proof {
                                                assert(stack@.drop_last() == stack_before);
                                                assert(pending_entries(stack@) == middle + high + pending_entries(stack_before));
                                            }
                                        }
                                    },
                                }
                            }
                        }
                        /// Alias for cursor_from; O(log n) to start at the first key >= bound.
                        pub fn seek(&self, bound: &Key) -> (result: Cursor<Value>)
                        ensures result.remaining() == lower_rows(self.entries(), bound.view()), result.work() >= 0,
                            result.remaining() == self.entries().skip(lower_position(self.entries(), bound.view())),
                            ordered_rows(result.remaining()),
                        {
                            self.cursor_from(bound)
                        }
                    }
                    /// The contribution removed when an update replaces or deletes one binding.
                    pub open spec fn contribution<Value>(index: Index<Value>, key: KeyView, weight: spec_fn((KeyView, Value)) -> int) -> int {
                        match index.at(key) { Some(value) => weight((key, value)), None => 0 }
                    }
                    /// Use to subtract all occurrences of a key from a sequence fold.
                    pub proof fn lemma_drop_sum<Value>(rows: Seq<(KeyView, Value)>, key: KeyView, weight: spec_fn((KeyView, Value)) -> int)
                        ensures sum(drop_key(rows, key), weight) == sum(rows, weight)
                            - sum(rows, |row: (KeyView, Value)| if row.0 == key { weight(row) } else { 0 }),
                        decreases rows.len(),
                    {
                        if rows.len() > 0 {
                            lemma_drop_sum(rows.drop_last(), key, weight);
                            if rows.last().0 != key { lemma_sum_push(drop_key(rows.drop_last(), key), rows.last(), weight); }
                        }
                    }
                    /// Strict key ordering gives distinct entries and distinct keys.
                    pub proof fn lemma_ordered_unique<Value>(rows: Seq<(KeyView, Value)>)
                        requires ordered_rows(rows),
                        ensures rows.no_duplicates(),
                            forall|first: int, last: int| #![trigger rows[first], rows[last]]
                                0 <= first < last < rows.len() ==> rows[first].0 != rows[last].0,
                    {
                        assert forall|first: int, last: int| #![trigger rows[first], rows[last]]
                            0 <= first < last < rows.len() implies rows[first].0 != rows[last].0 by {
                            lemma_key_order(rows[first].0, rows[last].0);
                        }
                    }
                    /// Use to identify the unique old binding's contribution, including absent keys.
                    pub proof fn lemma_key_contribution<Value>(index: &Index<Value>, key: KeyView, weight: spec_fn((KeyView, Value)) -> int)
                        requires index.wf(),
                        ensures sum(index.entries(), |row: (KeyView, Value)| if row.0 == key { weight(row) } else { 0 })
                            == contribution(*index, key, weight),
                    {
                        index.model();
                        lemma_ordered_unique(index.entries());
                        let rows = index.entries();
                        let selected = |row: (KeyView, Value)| if row.0 == key { weight(row) } else { 0 };
                        if index.at(key).is_some() {
                            let position = choose|position: int| #![auto] 0 <= position < rows.len() && rows[position].0 == key;
                            assert forall|other: int| #![trigger rows[other]]
                                0 <= other < rows.len() && other != position implies selected(rows[other]) == 0 by {
                                if other < position { assert(rows[other].0 != rows[position].0); }
                                else { assert(rows[position].0 != rows[other].0); }
                            }
                            lemma_sum_single_match(rows, selected, position);
                        } else {
                            assert forall|position: int| #![trigger rows[position]] 0 <= position < rows.len() implies selected(rows[position]) == 0 by {};
                            lemma_sum_zero(rows, selected);
                        }
                    }
                    /// Use when two versions differ only at one key; all other contributions are preserved.
                    pub proof fn lemma_sum_unchanged_except<Value>(before: &Index<Value>, after: &Index<Value>, key: KeyView,
                        weight: spec_fn((KeyView, Value)) -> int)
                        requires before.wf(), after.wf(),
                            forall|query: KeyView| #![trigger before.at(query), after.at(query)] query != key ==> before.at(query) == after.at(query),
                        ensures sum(after.entries(), weight) - contribution(*after, key, weight)
                            == sum(before.entries(), weight) - contribution(*before, key, weight),
                    {
                        before.map_model();
                        after.map_model();
                        let low = drop_key(before.entries(), key);
                        let high = drop_key(after.entries(), key);
                        lemma_drop_bindings(before.entries(), key);
                        lemma_drop_bindings(after.entries(), key);
                        assert forall|row: (KeyView, Value)| #![trigger low.contains(row), high.contains(row)]
                            low.contains(row) <==> high.contains(row) by {
                            if row.0 != key {
                                assert(before.at(row.0) == after.at(row.0));
                                assert(before.entries().contains(row) <==> before.at(row.0) == Some(row.1));
                                assert(after.entries().contains(row) <==> after.at(row.0) == Some(row.1));
                            }
                        }
                        low.to_set_ensures();
                        high.to_set_ensures();
                        assert(low.to_set() =~= high.to_set());
                        lemma_drop_ordered(before.entries(), key);
                        lemma_drop_ordered(after.entries(), key);
                        lemma_ordered_unique(low);
                        lemma_ordered_unique(high);
                        crate::lemma_sum_same_support(low, high, weight);
                        lemma_drop_sum(before.entries(), key, weight);
                        lemma_drop_sum(after.entries(), key, weight);
                        lemma_key_contribution(before, key, weight);
                        lemma_key_contribution(after, key, weight);
                    }
                    /// Use after put to maintain a sum over all entries: subtract the old binding, add the new one.
                    /// Keep before = index.share() before the executable update, then call this lemma.
                    pub proof fn lemma_sum_put<Value>(before: &Index<Value>, after: &Index<Value>, key: KeyView, value: Value,
                        weight: spec_fn((KeyView, Value)) -> int)
                        requires before.wf(), after.wf(), after.view() == before.view().insert(key, value),
                        ensures sum(after.entries(), weight) == sum(before.entries(), weight)
                            - contribution(*before, key, weight) + weight((key, value)),
                    {
                        before.map_model();
                        after.map_model();
                        assert forall|query: KeyView| #![trigger before.at(query), after.at(query)] query != key implies before.at(query) == after.at(query) by {
                            assert(before.view().dom().contains(query) == after.view().dom().contains(query));
                            if before.view().dom().contains(query) { assert(before.view()[query] == after.view()[query]); }
                        }
                        assert(after.view().dom().contains(key));
                        assert(after.view()[key] == value);
                        assert(after.at(key) == Some(value));
                        lemma_sum_unchanged_except(before, after, key, weight);
                    }
                    /// Use after remove to subtract the deleted binding; removing an absent key changes no sum.
                    pub proof fn lemma_sum_remove<Value>(before: &Index<Value>, after: &Index<Value>, key: KeyView,
                        weight: spec_fn((KeyView, Value)) -> int)
                        requires before.wf(), after.wf(), after.view() == before.view().remove(key),
                        ensures sum(after.entries(), weight) == sum(before.entries(), weight) - contribution(*before, key, weight),
                    {
                        before.map_model();
                        after.map_model();
                        assert forall|query: KeyView| #![trigger before.at(query), after.at(query)] query != key implies before.at(query) == after.at(query) by {
                            assert(before.view().dom().contains(query) == after.view().dom().contains(query));
                            if before.view().dom().contains(query) { assert(before.view()[query] == after.view()[query]); }
                        }
                        assert(!after.view().dom().contains(key));
                        assert(after.at(key).is_none());
                        lemma_sum_unchanged_except(before, after, key, weight);
                    }
                    /// Index of the first key at or above bound in an ordered sequence (length if absent).
                    pub open spec fn lower_position<Value>(rows: Seq<(KeyView, Value)>, bound: KeyView) -> int
                        decreases rows.len(),
                    {
                        if rows.len() == 0 { 0 }
                        else if key_lt(rows.last().0, bound) { rows.len() as int }
                        else { lower_position(rows.drop_last(), bound) }
                    }
                    /// Use to turn a seek's filtered view into an exact ordered suffix, with both range bounds certified.
                    pub proof fn lemma_lower_suffix<Value>(rows: Seq<(KeyView, Value)>, bound: KeyView)
                        requires ordered_rows(rows),
                        ensures
                            0 <= lower_position(rows, bound) <= rows.len(),
                            lower_rows(rows, bound) == rows.skip(lower_position(rows, bound)),
                            ordered_rows(lower_rows(rows, bound)),
                            forall|position: int| #![trigger rows[position]] 0 <= position < lower_position(rows, bound) ==> key_lt(rows[position].0, bound),
                            forall|position: int| #![trigger rows[position]] lower_position(rows, bound) <= position < rows.len() ==> !key_lt(rows[position].0, bound),
                        decreases rows.len(),
                    {
                        if rows.len() > 0 {
                            let prefix = rows.drop_last();
                            assert(ordered_rows(prefix));
                            lemma_lower_suffix(prefix, bound);
                            if key_lt(rows.last().0, bound) {
                                assert forall|position: int| #![trigger rows[position]] 0 <= position < rows.len() implies key_lt(rows[position].0, bound) by {
                                    if position < rows.len() - 1 { lemma_key_trans(rows[position].0, rows.last().0, bound); }
                                }
                                lemma_lower_segment(rows, bound);
                                assert(rows.skip(rows.len() as int) =~= Seq::<(KeyView, Value)>::empty());
                            } else {
                                let begin = lower_position(prefix, bound);
                                assert(rows.skip(begin) =~= prefix.skip(begin).push(rows.last()));
                                assert forall|position: int| #![trigger rows[position]] 0 <= position < begin implies key_lt(rows[position].0, bound) by {
                                    assert(rows[position] == prefix[position]);
                                }
                                assert forall|position: int| #![trigger rows[position]] begin <= position < rows.len() implies !key_lt(rows[position].0, bound) by {
                                    if position < prefix.len() { assert(rows[position] == prefix[position]); }
                                    else { assert(rows[position] == rows.last()); }
                                }
                            }
                            let begin = lower_position(rows, bound);
                            assert(0 <= begin <= rows.len());
                            assert(lower_rows(rows, bound) == rows.skip(begin));
                            assert forall|first: int, last: int| #![auto] 0 <= first < last < lower_rows(rows, bound).len() implies
                                key_lt(lower_rows(rows, bound)[first].0, lower_rows(rows, bound)[last].0) by {
                                assert(lower_rows(rows, bound)[first] == rows[begin + first]);
                                assert(lower_rows(rows, bound)[last] == rows[begin + last]);
                            }
                        }
                    }
                    impl<Value> Index<Value> {
                        #[verifier::type_invariant]
                        spec fn valid(self) -> bool { self.wf() }
                        pub closed spec fn tree(self) -> Tree<Value> { *self.root }
                        pub open spec fn entries(self) -> Seq<(KeyView, Value)> { self.tree().entries() }
                        pub open spec fn wf(self) -> bool { self.tree().wf() }
                        /// Mathematical map; model() connects its bindings to the ordered entries.
                        pub open spec fn view(self) -> Map<KeyView, Value> {
                            Map::new(self.entries().map_values(|row: (KeyView, Value)| row.0).to_set(),
                                |key: KeyView| self.at(key).unwrap())
                        }
                        /// Use to transfer between map-domain membership, lookups and ordered entries.
                        pub proof fn map_model(&self)
                            requires self.wf(),
                            ensures
                                ordered_rows(self.entries()),
                                self.view().dom().finite(),
                                forall|key: KeyView| #![trigger self.at(key)] self.at(key) ==
                                    if self.view().dom().contains(key) { Some(self.view()[key]) } else { None },
                                forall|key: KeyView| #![trigger self.view().dom().contains(key)]
                                    self.view().dom().contains(key) <==> self.at(key).is_some(),
                                forall|row: (KeyView, Value)| #![trigger self.entries().contains(row)]
                                    self.entries().contains(row) <==> self.view().dom().contains(row.0) && self.view()[row.0] == row.1,
                        {
                            self.model();
                            let keys = self.entries().map_values(|row: (KeyView, Value)| row.0);
                            keys.to_set_ensures();
                            assert forall|key: KeyView| #![trigger self.view().dom().contains(key)]
                                self.view().dom().contains(key) <==> self.at(key).is_some() by {
                                if keys.contains(key) {
                                    let position = choose|position: int| 0 <= position < keys.len() && keys[position] == key;
                                    assert(self.entries()[position].0 == key);
                                }
                                if self.at(key).is_some() {
                                    let position = choose|position: int| #![auto] 0 <= position < self.entries().len() && self.entries()[position].0 == key;
                                    assert(keys[position] == key);
                                }
                            }
                            assert forall|row: (KeyView, Value)| #![trigger self.entries().contains(row)]
                                self.entries().contains(row) <==> self.view().dom().contains(row.0) && self.view()[row.0] == row.1 by {
                                if self.entries().contains(row) {
                                    let position = choose|position: int| 0 <= position < self.entries().len() && self.entries()[position] == row;
                                }
                                if self.view().dom().contains(row.0) && self.view()[row.0] == row.1 {
                                    lemma_at_rows(&*self.root, row.0);
                                    let position = choose|position: int| #![auto] 0 <= position < self.entries().len() && self.entries()[position].0 == row.0;
                                    assert(self.entries()[position] == row);
                                }
                            }
                        }
                        pub open spec fn at(self, key: KeyView) -> Option<Value> {
                            self.tree().at(key)
                        }
                        pub proof fn model(&self)
                            requires self.wf(),
                            ensures
                                self.wf(),
                                ordered_rows(self.entries()),
                                forall|position: int| #![trigger self.entries()[position]] 0 <= position < self.entries().len() ==>
                                    self.at(self.entries()[position].0) == Some(self.entries()[position].1),
                                forall|query: KeyView| #![trigger self.at(query)] self.at(query).is_some() <==>
                                    exists|position: int| #![auto] 0 <= position < self.entries().len() && self.entries()[position].0 == query,
                        {
                            lemma_wf_rows(&*self.root);
                            assert forall|position: int| #![trigger self.entries()[position]] 0 <= position < self.entries().len() implies
                                self.at(self.entries()[position].0) == Some(self.entries()[position].1) by {
                                lemma_at_rows(&*self.root, self.entries()[position].0);
                            }
                            assert forall|query: KeyView| #![trigger self.at(query)] self.at(query).is_some() <==>
                                exists|position: int| #![auto] 0 <= position < self.entries().len() && self.entries()[position].0 == query by {
                                lemma_at_rows(&*self.root, query);
                            }
                        }
                        /// Create an empty persistent map in O(1).
                        pub fn new() -> (result: Self)
                            ensures result.wf(), result.entries() == Seq::<(KeyView, Value)>::empty(),
                                result.view() == Map::<KeyView, Value>::empty(),
                                forall|query: KeyView| #![trigger result.at(query)] result.at(query).is_none(),
                        {
                            let result = Index { root: Arc::new(Tree::Empty) };
                            proof { assert(result.view() =~= Map::<KeyView, Value>::empty()); }
                            result
                        }
                        /// Share a version in O(1); later updates leave shared versions unchanged.
                        pub fn share(&self) -> (result: Self)
                            ensures self.wf(), result.wf(), result.entries() == self.entries(), result.tree() == self.tree(),
                                result.view() == self.view(),
                        {
                            proof { use_type_invariant(self); }
                            Index { root: self.root.clone() }
                        }
                        /// Look up a key in O(log n) key comparisons.
                        pub fn get(&self, key: &Key) -> (result: Option<Arc<Value>>)
                        ensures
                            match result { Some(value) => self.at(key.view()) == Some(*value), None => self.at(key.view()).is_none() },
                            match result { Some(value) => self.view().dom().contains(key.view()) && self.view()[key.view()] == *value,
                                None => !self.view().dom().contains(key.view()) },
                        {
                            proof { use_type_invariant(self); }
                            proof { self.map_model(); }
                            let mut cursor = self.root.clone();
                            loop
                                invariant
                                    self.at(key.view()) == (*cursor).at(key.view()),
                                    self.view().dom().contains(key.view()) <==> self.at(key.view()).is_some(),
                                    self.view().dom().contains(key.view()) ==> self.view()[key.view()] == self.at(key.view()).unwrap(),
                                decreases (*cursor).depth(),
                            {
                                match &*cursor {
                                    Tree::Empty => return None,
                                    Tree::Branch { key: stored, value, left, right, .. } => {
                                        let order = compare(key, stored);
                                        if order == 0 { return Some(value.clone()); }
                                        cursor = if order < 0 { left.clone() } else { right.clone() };
                                    },
                                }
                            }
                        }
                        /// Insert or replace in O(log n), copying the changed AVL path. Share first to retain the old version.
                        pub fn put(&mut self, key: Key, value: Arc<Value>)
                            ensures
                                final(self).wf(),
                                final(self).view() == old(self).view().insert(key.view(), *value),
                                forall|query: KeyView| #![auto] final(self).at(query) == if query == key.view() { Some(*value) } else { old(self).at(query) },
                        {
                            proof { use_type_invariant(&*self); }
                            proof { self.map_model(); }
                            let ghost before = self.view();
                            self.root = insert(&self.root, key, value);
                            proof { self.map_model(); assert(self.view() =~= before.insert(key.view(), *value)); }
                        }
                        /// Remove in O(log n), copying the changed path; shared versions remain valid.
                        pub fn remove(&mut self, key: &Key)
                            ensures
                                final(self).wf(),
                                final(self).view() == old(self).view().remove(key.view()),
                                final(self).entries() == drop_key(old(self).entries(), key.view()),
                                forall|query: KeyView| #![auto] final(self).at(query) == if query == key.view() { None } else { old(self).at(query) },
                        {
                            proof { use_type_invariant(&*self); }
                            proof { self.map_model(); }
                            let ghost before = self.view();
                            self.root = erase(&self.root, key);
                            proof { self.map_model(); assert(self.view() =~= before.remove(key.view())); }
                        }
                        /// Check emptiness in O(1).
                        pub fn is_empty(&self) -> (result: bool)
                            ensures result == (self.entries().len() == 0),
                        {
                            match &*self.root { Tree::Empty => true, _ => false }
                        }
                        /// Create an ascending snapshot cursor in O(1); its first next() walks the left spine.
                        pub fn cursor(&self) -> (result: Cursor<Value>)
                            ensures result.remaining() == self.entries(), result.work() >= 0,
                        {
                            Cursor { stack: Vec::new(), current: self.root.clone() }
                        }
                        /// Materialize ascending entries in O(n) time and space.
                        pub fn rows(&self) -> (result: Vec<(Key, Arc<Value>)>)
                            ensures self.wf(), result@.map_values(|row: (Key, Arc<Value>)| (row.0.view(), *row.1)) == self.entries(),
                        {
                            proof { use_type_invariant(self); }
                            let mut result = Vec::new();
                            let mut cursor = self.cursor();
                            loop
                                invariant
                                    self.wf(),
                                    result@.map_values(|row: (Key, Arc<Value>)| (row.0.view(), *row.1)) + cursor.remaining() == self.entries(),
                                    cursor.work() >= 0,
                                decreases cursor.work(),
                            {
                                let ghost previous_remaining = cursor.remaining();
                                let ghost prefix = result@.map_values(|row: (Key, Arc<Value>)| (row.0.view(), *row.1));
                                let row = match cursor.next() { Some(row) => row, None => return result };
                                let ghost item = previous_remaining[0];
                                result.push(row);
                                proof {
                                    assert(previous_remaining =~= seq![item] + cursor.remaining());
                                    assert(result@.map_values(|row: (Key, Arc<Value>)| (row.0.view(), *row.1)) =~= prefix.push(item));
                                }
                            }
                        }
                    }
                    fn height<Value>(tree: &Arc<Tree<Value>>) -> usize {
                        match &**tree { Tree::Empty => 0, Tree::Branch { height, .. } => *height }
                    }
                    fn branch<Value>(key: Key, value: Arc<Value>, left: Arc<Tree<Value>>, right: Arc<Tree<Value>>) -> (result: Arc<Tree<Value>>)
                        ensures
                            (*result).entries() == joined(key, value, left, right).entries(),
                            (*result).wf() == joined(key, value, left, right).wf(),
                            forall|query: KeyView| #![trigger (*result).at(query)] (*result).at(query) == joined(key, value, left, right).at(query),
                    {
                        let left_height = height(&left);
                        let right_height = height(&right);
                        let maximum = if left_height > right_height { left_height } else { right_height };
                        let height = if maximum == usize::MAX { maximum } else { maximum + 1 };
                        Arc::new(Tree::Branch { key, value, left, right, height })
                    }
                    #[verifier::rlimit(40)]
                    fn balance<Value>(key: Key, value: Arc<Value>, left: Arc<Tree<Value>>, right: Arc<Tree<Value>>) -> (result: Arc<Tree<Value>>)
                        ensures
                            (*result).entries() == joined(key, value, left, right).entries(),
                            (*result).wf() == joined(key, value, left, right).wf(),
                            joined(key, value, left, right).wf() ==> forall|query: KeyView| #![trigger (*result).at(query)]
                                (*result).at(query) == joined(key, value, left, right).at(query),
                    {
                        hide(Tree::wf);
                        hide(Tree::at);
                        let ghost expected = joined(key, value, left, right);
                        let left_height = height(&left);
                        let right_height = height(&right);
                        if left_height > right_height && left_height - right_height > 1 {
                            if let Tree::Branch { key: pivot, value: pivot_value, left: outer, right: inner, .. } = &*left {
                                proof {
                                    assert((*left).entries() == (**outer).entries() + seq![(pivot.view(), **pivot_value)] + (**inner).entries());
                                    assert(expected.entries() == (*left).entries() + seq![(key.view(), *value)] + (*right).entries());
                                }
                                if height(outer) >= height(inner) {
                                    let down = branch(key, value, inner.clone(), right);
                                    let result = branch(pivot.share(), pivot_value.clone(), outer.clone(), down);
                                    proof { assert((*result).entries() =~= expected.entries()); }
                                    return finish_balance(result, Ghost(expected));
                                }
                                if let Tree::Branch { key: middle, value: middle_value, left: inner_left, right: inner_right, .. } = &**inner {
                                    proof {
                                        assert((**inner).entries() == (**inner_left).entries() + seq![(middle.view(), **middle_value)] + (**inner_right).entries());
                                    }
                                    let low = branch(pivot.share(), pivot_value.clone(), outer.clone(), inner_left.clone());
                                    let high = branch(key, value, inner_right.clone(), right);
                                    let result = branch(middle.share(), middle_value.clone(), low, high);
                                    proof { assert((*result).entries() =~= expected.entries()); }
                                    return finish_balance(result, Ghost(expected));
                                }
                            }
                        }
                        if right_height > left_height && right_height - left_height > 1 {
                            if let Tree::Branch { key: pivot, value: pivot_value, left: inner, right: outer, .. } = &*right {
                                proof {
                                    assert((*right).entries() == (**inner).entries() + seq![(pivot.view(), **pivot_value)] + (**outer).entries());
                                    assert(expected.entries() == (*left).entries() + seq![(key.view(), *value)] + (*right).entries());
                                }
                                if height(outer) >= height(inner) {
                                    let down = branch(key, value, left, inner.clone());
                                    let result = branch(pivot.share(), pivot_value.clone(), down, outer.clone());
                                    proof { assert((*result).entries() =~= expected.entries()); }
                                    return finish_balance(result, Ghost(expected));
                                }
                                if let Tree::Branch { key: middle, value: middle_value, left: inner_left, right: inner_right, .. } = &**inner {
                                    proof {
                                        assert((**inner).entries() == (**inner_left).entries() + seq![(middle.view(), **middle_value)] + (**inner_right).entries());
                                    }
                                    let low = branch(key, value, left, inner_left.clone());
                                    let high = branch(pivot.share(), pivot_value.clone(), inner_right.clone(), outer.clone());
                                    let result = branch(middle.share(), middle_value.clone(), low, high);
                                    proof { assert((*result).entries() =~= expected.entries()); }
                                    return finish_balance(result, Ghost(expected));
                                }
                            }
                        }
                        let result = branch(key, value, left, right);
                        finish_balance(result, Ghost(expected))
                    }
                    #[verifier::rlimit(20)]
                    fn insert<Value>(tree: &Arc<Tree<Value>>, key: Key, value: Arc<Value>) -> (result: Arc<Tree<Value>>)
                        requires (**tree).wf(),
                        ensures
                            (*result).wf(),
                            forall|query: KeyView| #![trigger (*result).at(query)] (*result).at(query) == if query == key.view() { Some(*value) } else { (**tree).at(query) },
                        decreases (**tree).depth(),
                    {
                        match &**tree {
                            Tree::Empty => branch(key, value, Arc::new(Tree::Empty), Arc::new(Tree::Empty)),
                            Tree::Branch { key: stored, value: stored_value, left, right, .. } => {
                                let order = compare(&key, stored);
                                if order == 0 { return branch(stored.share(), value, left.clone(), right.clone()); }
                                if order < 0 {
                                    let low = insert(left, key, value);
                                    proof {
                                        assert forall|position: int| #![auto] 0 <= position < (*low).entries().len() implies key_lt((*low).entries()[position].0, stored.view()) by {
                                            let query = (*low).entries()[position].0;
                                            lemma_at_rows(&*low, query);
                                            if query != key.view() {
                                                lemma_at_rows(&**left, query);
                                                let old_position = choose|old_position: int| #![auto] 0 <= old_position < (**left).entries().len() && (**left).entries()[old_position].0 == query;
                                                assert(key_lt((**left).entries()[old_position].0, stored.view()));
                                            }
                                        }
                                    }
                                    let result = balance(stored.share(), stored_value.clone(), low.clone(), right.clone());
                                    proof {
                                        assert forall|query: KeyView| #![trigger (*result).at(query)] (*result).at(query) == if query == key.view() { Some(*value) } else { (**tree).at(query) } by {
                                            assert((*low).at(query) == if query == key.view() { Some(*value) } else { (**left).at(query) });
                                            assert((*result).at(query) == joined(*stored, *stored_value, low, *right).at(query));
                                        }
                                    }
                                    result
                                } else {
                                    let high = insert(right, key, value);
                                    proof {
                                        assert forall|position: int| #![auto] 0 <= position < (*high).entries().len() implies key_lt(stored.view(), (*high).entries()[position].0) by {
                                            let query = (*high).entries()[position].0;
                                            lemma_at_rows(&*high, query);
                                            if query != key.view() {
                                                lemma_at_rows(&**right, query);
                                                let old_position = choose|old_position: int| #![auto] 0 <= old_position < (**right).entries().len() && (**right).entries()[old_position].0 == query;
                                                assert(key_lt(stored.view(), (**right).entries()[old_position].0));
                                            }
                                        }
                                    }
                                    let result = balance(stored.share(), stored_value.clone(), left.clone(), high.clone());
                                    proof {
                                        assert forall|query: KeyView| #![trigger (*result).at(query)] (*result).at(query) == if query == key.view() { Some(*value) } else { (**tree).at(query) } by {
                                            assert((*high).at(query) == if query == key.view() { Some(*value) } else { (**right).at(query) });
                                            assert((*result).at(query) == joined(*stored, *stored_value, *left, high).at(query));
                                        }
                                    }
                                    result
                                }
                            },
                        }
                    }
                    #[verifier::rlimit(20)]
                    fn erase<Value>(tree: &Arc<Tree<Value>>, key: &Key) -> (result: Arc<Tree<Value>>)
                        requires (**tree).wf(),
                        ensures
                            (*result).wf(),
                            (*result).entries() == drop_key((**tree).entries(), key.view()),
                            forall|query: KeyView| #![trigger (*result).at(query)] (*result).at(query) == if query == key.view() { None } else { (**tree).at(query) },
                        decreases (**tree).depth(),
                    {
                        match &**tree {
                            Tree::Empty => tree.clone(),
                            Tree::Branch { key: stored, value, left, right, .. } => {
                                let ghost low_rows = rows_of(&**left);
                                let ghost high_rows = rows_of(&**right);
                                let ghost middle = branch_row(stored, value);
                                proof {
                                    lemma_drop_concat(low_rows, middle, key.view());
                                    lemma_drop_concat(low_rows + middle, high_rows, key.view());
                                }
                                let order = compare(key, stored);
                                if order < 0 {
                                    proof {
                                        assert forall|position: int| #![auto] 0 <= position < high_rows.len() implies high_rows[position].0 != key.view() by {
                                            lemma_key_trans(key.view(), stored.view(), high_rows[position].0);
                                            lemma_key_order(key.view(), high_rows[position].0);
                                        }
                                        lemma_drop_absent(high_rows, key.view());
                                        lemma_drop_absent(middle, key.view());
                                    }
                                    let low = erase(left, key);
                                    let result = balance(stored.share(), value.clone(), low, right.clone());
                                    proof { assert((*result).entries() =~= drop_key((**tree).entries(), key.view())); }
                                    return finish_erase(result, tree, key);
                                }
                                if order > 0 {
                                    proof {
                                        assert forall|position: int| #![auto] 0 <= position < low_rows.len() implies low_rows[position].0 != key.view() by {
                                            lemma_key_trans(low_rows[position].0, stored.view(), key.view());
                                            lemma_key_order(low_rows[position].0, key.view());
                                        }
                                        lemma_drop_absent(low_rows, key.view());
                                        lemma_drop_absent(middle, key.view());
                                    }
                                    let high = erase(right, key);
                                    let result = balance(stored.share(), value.clone(), left.clone(), high);
                                    proof { assert((*result).entries() =~= drop_key((**tree).entries(), key.view())); }
                                    return finish_erase(result, tree, key);
                                }
                                proof {
                                    assert forall|position: int| #![auto] 0 <= position < low_rows.len() implies low_rows[position].0 != key.view() by {
                                        lemma_key_order(low_rows[position].0, key.view());
                                    }
                                    assert forall|position: int| #![auto] 0 <= position < high_rows.len() implies high_rows[position].0 != key.view() by {
                                        lemma_key_order(key.view(), high_rows[position].0);
                                    }
                                    lemma_drop_absent(low_rows, key.view());
                                    lemma_drop_absent(high_rows, key.view());
                                    lemma_drop_first(middle);
                                    assert(middle[0].0 == key.view());
                                    assert(drop_key(middle, key.view()) =~= Seq::<(KeyView, Value)>::empty());
                                    assert(drop_key((**tree).entries(), key.view()) == low_rows + high_rows);
                                }
                                if let Tree::Empty = &**left { return finish_erase(right.clone(), tree, key); }
                                if let Tree::Empty = &**right { return finish_erase(left.clone(), tree, key); }
                                proof { lemma_wf_rows(&**right); }
                                let mut cursor = right.clone();
                                loop
                                    invariant
                                        (**tree).wf(),
                                        drop_key((**tree).entries(), key.view()) == low_rows + high_rows,
                                        (**left).entries() == low_rows,
                                        (**right).entries() == high_rows,
                                        (**right).wf(),
                                        ordered_rows(high_rows), high_rows.len() > 0,
                                        (*cursor).wf(), (*cursor).entries().len() > 0,
                                        (*cursor).entries()[0] == high_rows[0],
                                        (*right).depth() < (**tree).depth(),
                                        (*cursor).depth() <= (*right).depth(),
                                    decreases (*cursor).depth(),
                                {
                                    match &*cursor {
                                        Tree::Empty => return left.clone(),
                                        Tree::Branch { key: next, value: next_value, left: next_left, .. } => {
                                            if let Tree::Empty = &**next_left {
                                                proof {
                                                    assert(branch_row(next, next_value) == seq![high_rows[0]]);
                                                    lemma_drop_first(high_rows);
                                                    assert(high_rows =~= seq![high_rows[0]] + high_rows.skip(1));
                                                }
                                                let high = erase(right, next);
                                                proof { assert((*high).entries() == high_rows.skip(1)); }
                                                let result = balance(next.share(), next_value.clone(), left.clone(), high);
                                                proof { assert((*result).entries() =~= low_rows + high_rows); }
                                                return finish_erase(result, tree, key);
                                            }
                                            cursor = next_left.clone();
                                        },
                                    }
                                }
                            },
                        }
                    }
                    }
                    }
/// Lift pointwise weighted equality through a flattened join expansion.
pub proof fn lemma_flat_map_weight_equal<Source, Target>(rows: Seq<Source>, left: spec_fn(Source) -> Seq<Target>, right: spec_fn(Source) -> Seq<Target>, weight: spec_fn(Target) -> int, scale: int) requires forall|item: Source| #[trigger] sum(left(item), weight) == scale * sum(right(item), weight), ensures sum(verus_common::rel::flat(rows.map_values(left)), weight) == scale * sum(verus_common::rel::flat(rows.map_values(right)), weight), { verus_common::rel::lemma_sum_flat_map(rows, left, weight); verus_common::rel::lemma_sum_flat_map(rows, right, weight); lemma_sum_scale(rows, |item: Source| sum(right(item), weight), |item: Source| sum(left(item), weight), scale); }
use vstd::hash_map::HashMapWithView;
broadcast use vstd::std_specs::hash::group_hash_axioms;
/// Use to transfer a scan's row coverage through its universal weighted-sum contract.
pub proof fn lemma_weighted_equal_preserves_predicate<Row>(before: Seq<Row>, after: Seq<Row>, predicate: spec_fn(Row) -> bool) requires forall|weight: spec_fn(Row) -> int| #[trigger] sum(before, weight) == sum(after, weight), forall|index: int| 0 <= index < before.len() ==> predicate(#[trigger] before[index]), ensures forall|index: int| 0 <= index < after.len() ==> predicate(#[trigger] after[index]), { let missing = |row: Row| if predicate(row) { 0int } else { 1int }; lemma_sum_zero(before, missing); assert(sum(after, missing) == 0); assert forall|index: int| 0 <= index < after.len() implies predicate(#[trigger] after[index]) by { if !predicate(after[index]) { lemma_sum_pos(after, missing, index); } } }
/// Use to preserve row coverage after capture when both old rows and delta occurrences satisfy it.
pub proof fn lemma_bag_step_preserves_predicate<Row>(before: Seq<Row>, after: Seq<Row>, deltas: Seq<(Row, int)>, predicate: spec_fn(Row) -> bool) requires bag_step(before, after, deltas), forall|index: int| 0 <= index < before.len() ==> predicate(#[trigger] before[index]), forall|index: int| 0 <= index < deltas.len() ==> predicate((#[trigger] deltas[index]).0), ensures forall|index: int| 0 <= index < after.len() ==> predicate(#[trigger] after[index]), { let missing = |row: Row| if predicate(row) { 0int } else { 1int }; lemma_sum_zero(before, missing); lemma_sum_zero(deltas, |delta: (Row, int)| delta.1 * missing(delta.0)); assert(sum(after, missing) == 0); assert forall|index: int| 0 <= index < after.len() implies predicate(#[trigger] after[index]) by { if !predicate(after[index]) { lemma_sum_pos(after, missing, index); } } }
/// Use to model compound integer keys through specified primitive-key vstd maps.
pub type IntegerTriple = (i64, i64, i64);
#[verifier::reject_recursive_types(Value)] pub struct IntegerTripleMap<Value> { pub entries: HashMapWithView<i64, HashMapWithView<i64, HashMapWithView<i64, Value>>>, } impl<Value> View for IntegerTripleMap<Value> { type V = IMap<IntegerTriple, Value>; open spec fn view(&self) -> IMap<IntegerTriple, Value> { IMap::new( |key: IntegerTriple| self.entries@.contains_key(key.0) && self.entries@[key.0]@.contains_key(key.1) && self.entries@[key.0]@[key.1]@.contains_key(key.2), |key: IntegerTriple| self.entries@[key.0]@[key.1]@[key.2], ) } } impl<Value> IntegerTripleMap<Value> { pub fn new() -> (result: Self) ensures result@ == IMap::<IntegerTriple, Value>::empty(), { IntegerTripleMap { entries: HashMapWithView::new() } } pub fn contains_key(&self, key: &IntegerTriple) -> (result: bool) ensures result == self@.contains_key(*key), { match self.entries.get(&key.0) { Some(middle) => match middle.get(&key.1) { Some(last) => last.contains_key(&key.2), None => false, }, None => false, } } pub fn get(&self, key: &IntegerTriple) -> (result: Option<&Value>) ensures result matches Some(value) ==> self@.contains_key(*key) && *value == self@[*key], result is None ==> !self@.contains_key(*key), { match self.entries.get(&key.0) { Some(middle) => match middle.get(&key.1) { Some(last) => last.get(&key.2), None => None, }, None => None, } } pub fn insert(&mut self, key: IntegerTriple, value: Value) -> (result: Option<Value>) ensures final(self)@ == old(self)@.insert(key, value), result matches Some(previous) ==> old(self)@.contains_key(key) && previous == old(self)@[key], result is None ==> !old(self)@.contains_key(key), { let ghost before = self@; let mut middle = match self.entries.remove(&key.0) { Some(middle) => middle, None => HashMapWithView::new() }; let mut last = match middle.remove(&key.1) { Some(last) => last, None => HashMapWithView::new() }; let result = last.remove(&key.2); last.insert(key.2, value); middle.insert(key.1, last); self.entries.insert(key.0, middle); proof { assert(self@ =~= before.insert(key, value)); } result } pub fn remove(&mut self, key: &IntegerTriple) -> (result: Option<Value>) ensures final(self)@ == old(self)@.remove(*key), result matches Some(previous) ==> old(self)@.contains_key(*key) && previous == old(self)@[*key], result is None ==> !old(self)@.contains_key(*key), { let ghost before = self@; let mut middle = match self.entries.remove(&key.0) { Some(middle) => middle, None => return None }; let mut last = match middle.remove(&key.1) { Some(last) => last, None => { self.entries.insert(key.0, middle); return None; }, }; let result = last.remove(&key.2); middle.insert(key.1, last); self.entries.insert(key.0, middle); proof { assert(self@ =~= before.remove(*key)); } result } }
/// Recover the source position of a member of a flattened join expansion.
pub proof fn lemma_flat_map_contains<Source, Target>(rows: Seq<Source>, mapper: spec_fn(Source) -> Seq<Target>, item: Target) ensures verus_common::rel::flat(rows.map_values(mapper)).contains(item) <==> exists|position: int| 0 <= position < rows.len() && #[trigger] mapper(rows[position]).contains(item), { verus_common::rel::lemma_flat_contains(rows.map_values(mapper), item); if verus_common::rel::flat(rows.map_values(mapper)).contains(item) { let position = choose|position: int| 0 <= position < rows.map_values(mapper).len() && (#[trigger] rows.map_values(mapper)[position]).contains(item); assert(rows.map_values(mapper)[position] == mapper(rows[position])); assert(mapper(rows[position]).contains(item)); } if exists|position: int| 0 <= position < rows.len() && #[trigger] mapper(rows[position]).contains(item) { let position = choose|position: int| 0 <= position < rows.len() && #[trigger] mapper(rows[position]).contains(item); assert(rows.map_values(mapper)[position].contains(item)); assert(exists|index: int| 0 <= index < rows.map_values(mapper).len() && (#[trigger] rows.map_values(mapper)[index]).contains(item)); } }
pub proof fn lemma_adjacent_int_order<Value>(rows: Seq<Value>, measure: spec_fn(Value) -> int, left: int, right: int) requires 0 <= left < right < rows.len(), forall|index: int| #![trigger rows[index]] 0 < index < rows.len() ==> measure(rows[index - 1]) < measure(rows[index]), ensures measure(rows[left]) < measure(rows[right]), decreases right - left, { if left + 1 < right { lemma_adjacent_int_order(rows, measure, left, right - 1); assert(measure(rows[right - 1]) < measure(rows[right])); } }
pub proof fn lemma_prefix_cutoff(values: Seq<int>, keep: spec_fn(int) -> bool, cutoff: int, bound: int) requires 0 <= cutoff <= values.len(), 0 <= bound <= values.len(), forall|index: int| #![trigger keep(index)] 0 <= index < values.len() ==> keep(index) == (index < cutoff), ensures sum(rng(bound), |index: int| if keep(index) { values[index] - if index == 0 { 0 } else { values[index - 1] } } else { 0 }) == if bound == 0 || cutoff == 0 { 0 } else { values[if bound <= cutoff { bound - 1 } else { cutoff - 1 }] }, decreases bound, { if bound > 0 { lemma_prefix_cutoff(values, keep, cutoff, bound - 1); lemma_range_step(bound - 1, |index: int| if keep(index) { values[index] - if index == 0 { 0 } else { values[index - 1] } } else { 0 }); assert(keep(bound - 1) == (bound - 1 < cutoff)); } }
/// Use to move an output projection through a flattened sequence of cache buckets.
pub proof fn lemma_map_flat<Source, Target>(pages: Seq<Seq<Source>>, mapper: spec_fn(Source) -> Target) ensures verus_common::rel::flat(pages).map_values(mapper) == verus_common::rel::flat(pages.map_values(|page: Seq<Source>| page.map_values(mapper))), decreases pages.len(), { if pages.len() > 0 { lemma_map_flat(pages.drop_last(), mapper); assert(pages.map_values(|page: Seq<Source>| page.map_values(mapper)).drop_last() =~= pages.drop_last().map_values(|page: Seq<Source>| page.map_values(mapper))); assert((verus_common::rel::flat(pages.drop_last()) + pages.last()).map_values(mapper) =~= verus_common::rel::flat(pages.drop_last()).map_values(mapper) + pages.last().map_values(mapper)); } else { assert(pages.map_values(|page: Seq<Source>| page.map_values(mapper)) =~= Seq::<Seq<Target>>::empty()); assert(verus_common::rel::flat(pages).map_values(mapper) =~= Seq::<Target>::empty()); } }
/// Use for a loop that appends one entire mapped bucket at a time.
pub proof fn lemma_flat_map_push<Source, Target>(rows: Seq<Source>, row: Source, mapper: spec_fn(Source) -> Seq<Target>) ensures verus_common::rel::flat(rows.push(row).map_values(mapper)) == verus_common::rel::flat(rows.map_values(mapper)) + mapper(row), { assert(rows.push(row).map_values(mapper).drop_last() =~= rows.map_values(mapper)); assert(rows.push(row).map_values(mapper).last() == mapper(row)); }
/// Use when every branch of a generated join produces an empty bag.
pub proof fn lemma_flat_map_empty<A, B>(rows: Seq<A>, branch: spec_fn(A) -> Seq<B>)
    requires forall|row: A| #[trigger] branch(row).len() == 0,
    ensures flat(rows.map_values(branch)).len() == 0,
    decreases rows.len(),
{
    if rows.len() > 0 {
        lemma_flat_map_empty(rows.drop_last(), branch);
        assert(rows.map_values(branch).drop_last() =~= rows.drop_last().map_values(branch));
        assert(rows.map_values(branch).last() == branch(rows.last()));
    }
}
/// Use for fact-table maintenance with overlapping post-state rescans and old-weight captures.
pub proof fn lemma_partitioned_bag_delta<A>(
    before: Seq<A>, after: Seq<A>, deltas: Seq<(A, int)>, first: Seq<A>, second: Seq<A>,
    old_weight: spec_fn(A) -> int, new_weight: spec_fn(A) -> int,
    in_first: spec_fn(A) -> bool, in_second: spec_fn(A) -> bool,
)
    requires bag_step(before, after, deltas),
        forall|row: A| !#[trigger] in_first(row) && !#[trigger] in_second(row) ==> new_weight(row) == old_weight(row),
        sum(first, |row: A| if in_first(row) { new_weight(row) - old_weight(row) } else { 0 }) ==
            sum(after, |row: A| if in_first(row) { new_weight(row) - old_weight(row) } else { 0 }),
        sum(second, |row: A| if !in_first(row) && in_second(row) { new_weight(row) - old_weight(row) } else { 0 }) ==
            sum(after, |row: A| if !in_first(row) && in_second(row) { new_weight(row) - old_weight(row) } else { 0 }),
    ensures sum(after, new_weight) == sum(before, old_weight) +
        sum(first, |row: A| if in_first(row) { new_weight(row) - old_weight(row) } else { 0 }) +
        sum(second, |row: A| if !in_first(row) && in_second(row) { new_weight(row) - old_weight(row) } else { 0 }) +
        sum(deltas, |delta: (A, int)| delta.1 * old_weight(delta.0)),
{
    let difference = |row: A| new_weight(row) - old_weight(row);
    let first_weight = |row: A| if in_first(row) { new_weight(row) - old_weight(row) } else { 0 };
    let second_weight = |row: A| if !in_first(row) && in_second(row) { new_weight(row) - old_weight(row) } else { 0 };
    assert forall|row: A| #[trigger] difference(row) == first_weight(row) + second_weight(row) by {}
    lemma_sum_add(after, first_weight, second_weight, difference);
    lemma_sum_sub(after, new_weight, old_weight, difference);
    assert(sum(after, old_weight) == sum(before, old_weight) + sum(deltas, |delta: (A, int)| delta.1 * old_weight(delta.0)));
    assert(sum(first, first_weight) == sum(after, first_weight));
    assert(sum(second, second_weight) == sum(after, second_weight));
}
/// Use to preserve strict positivity through checked multiplicity products.
pub proof fn lemma_positive_product(left: int, right: int)
    requires left > 0, right > 0,
    ensures left * right > 0,
{
    assert(left * right > 0) by (nonlinear_arith) requires left > 0, right > 0;
}
/// Use for support-list publication and processed-prefix membership invariants.
pub proof fn lemma_contains_push<Value>(rows: Seq<Value>, value: Value)
    ensures forall|other: Value| #[trigger] rows.push(value).contains(other) <==> rows.contains(other) || other == value,
{
    lemma_push_membership(rows, value);
}
/// Use universal scan sums to transfer representability or other row properties.
pub proof fn lemma_scan_property<A>(scanned: Seq<A>, database: Seq<A>, property: spec_fn(A) -> bool)
    requires forall|weight: spec_fn(A) -> int| #[trigger] sum(scanned, weight) == sum(database, weight),
        forall|index: int| 0 <= index < scanned.len() ==> property(#[trigger] scanned[index]),
    ensures forall|index: int| 0 <= index < database.len() ==> property(#[trigger] database[index]),
{
    let bad = |row: A| if property(row) { 0int } else { 1int };
    lemma_sum_zero(scanned, bad);
    assert(sum(database, bad) == 0);
    assert forall|index: int| 0 <= index < database.len() implies property(#[trigger] database[index]) by {
        if !property(database[index]) {
            lemma_sum_pos(database, bad, index);
        }
    }
}
/// The distinct key domain needed for weighted projection proofs.
pub open spec fn distinct_support<Key>(keys: Seq<Key>) -> Seq<Key> { dedup(keys) }
/// Use to establish a distinct domain and complete support for a projection.
pub proof fn lemma_distinct_support<Key>(keys: Seq<Key>)
    ensures distinct(distinct_support(keys)),
        forall|key: Key| #[trigger] distinct_support(keys).contains(key) <==> keys.contains(key),
{
    lemma_dedup(keys);
    assert forall|first: int, second: int| 0 <= first < distinct_support(keys).len() &&
        0 <= second < distinct_support(keys).len() && first != second implies
        (#[trigger] distinct_support(keys)[first]) != (#[trigger] distinct_support(keys)[second]) by {
        if second < first { assert(distinct_support(keys)[second] != distinct_support(keys)[first]); }
    }
}
/// Signed multiplicity belonging to one projected equivalence class.
pub open spec fn class_total<Row, Key>(rows: Seq<Row>, project: spec_fn(Row) -> Key, multiplicity: spec_fn(Row) -> int, key: Key) -> int {
    sum(rows, |row: Row| if project(row) == key { multiplicity(row) } else { 0 })
}
/// Use to factor an arbitrary class-dependent weight out of each grouped sum.
pub proof fn lemma_weighted_classes<Row, Key>(rows: Seq<Row>, keys: Seq<Key>, project: spec_fn(Row) -> Key,
    multiplicity: spec_fn(Row) -> int, weight: spec_fn(Key) -> int)
    requires distinct(keys),
        forall|index: int| 0 <= index < rows.len() ==> keys.contains(project(#[trigger] rows[index])),
    ensures sum(rows, |row: Row| multiplicity(row) * weight(project(row))) ==
        sum(keys, |key: Key| class_total(rows, project, multiplicity, key) * weight(key)),
{
    let row_weight = |row: Row| multiplicity(row) * weight(project(row));
    let per = |key: Key| sum(rows, |row: Row| if project(row) == key { row_weight(row) } else { 0 });
    lemma_sum_group(rows, keys, project, row_weight, per);
    let factored = |key: Key| class_total(rows, project, multiplicity, key) * weight(key);
    assert forall|key: Key| #[trigger] per(key) == factored(key) by {
        let raw = |row: Row| if project(row) == key { multiplicity(row) } else { 0 };
        let scaled = |row: Row| if project(row) == key { row_weight(row) } else { 0 };
        assert forall|row: Row| #[trigger] scaled(row) == weight(key) * raw(row) by {
            if project(row) == key {
                assert(multiplicity(row) * weight(key) == weight(key) * multiplicity(row)) by (nonlinear_arith);
            }
        }
        lemma_sum_scale(rows, raw, scaled, weight(key));
        lemma_sum_ext_all(rows, raw, |row: Row| if project(row) == key { multiplicity(row) } else { 0 });
        lemma_sum_ext_all(rows, scaled, |row: Row| if project(row) == key { row_weight(row) } else { 0 });
        assert(sum(rows, raw) == class_total(rows, project, multiplicity, key));
        assert(sum(rows, scaled) == per(key));
        assert(weight(key) * class_total(rows, project, multiplicity, key) == class_total(rows, project, multiplicity, key) * weight(key)) by (nonlinear_arith);
    }
    lemma_sum_ext_all(keys, per, factored);
}
/// Use equal projected multiplicities to transfer arbitrary weighted sums between representations.
pub proof fn lemma_equal_classes<Left, Right, Key>(left: Seq<Left>, right: Seq<Right>,
    left_project: spec_fn(Left) -> Key, right_project: spec_fn(Right) -> Key,
    left_multiplicity: spec_fn(Left) -> int, right_multiplicity: spec_fn(Right) -> int, weight: spec_fn(Key) -> int)
    requires forall|key: Key| #[trigger] class_total(left, left_project, left_multiplicity, key) ==
        class_total(right, right_project, right_multiplicity, key),
    ensures sum(left, |row: Left| left_multiplicity(row) * weight(left_project(row))) ==
        sum(right, |row: Right| right_multiplicity(row) * weight(right_project(row))),
{
    let joined = left.map_values(left_project) + right.map_values(right_project);
    let keys = distinct_support(joined);
    lemma_distinct_support(joined);
    assert forall|index: int| 0 <= index < left.len() implies keys.contains(left_project(#[trigger] left[index])) by {
        assert(left.map_values(left_project)[index] == left_project(left[index]));
        assert(joined[index] == left_project(left[index]));
        assert(joined.contains(left_project(left[index])));
    }
    assert forall|index: int| 0 <= index < right.len() implies keys.contains(right_project(#[trigger] right[index])) by {
        assert(right.map_values(right_project)[index] == right_project(right[index]));
        assert(joined[left.len() + index] == right_project(right[index]));
        assert(joined.contains(right_project(right[index])));
    }
    lemma_weighted_classes(left, keys, left_project, left_multiplicity, weight);
    lemma_weighted_classes(right, keys, right_project, right_multiplicity, weight);
    lemma_sum_ext_all(keys, |key: Key| class_total(left, left_project, left_multiplicity, key) * weight(key),
        |key: Key| class_total(right, right_project, right_multiplicity, key) * weight(key));
}

/// Use when projected supports differ but their weighted class contributions agree.
pub proof fn lemma_class_total_weight<Row, Key>(rows: Seq<Row>, project: spec_fn(Row) -> Key,
    multiplicity: spec_fn(Row) -> int, key: Key, weight: spec_fn(Row) -> int)
    requires forall|index: int| #![trigger rows[index]] 0 <= index < rows.len() ==>
        weight(rows[index]) == if project(rows[index]) == key { multiplicity(rows[index]) } else { 0int },
    ensures class_total(rows, project, multiplicity, key) == sum(rows, weight),
{
    lemma_sum_ext(rows, |row: Row| if project(row) == key { multiplicity(row) } else { 0int }, weight);
}

/// Use when projected supports differ but their weighted class contributions agree.
pub proof fn lemma_equal_weighted_classes<Left, Right, Key>(left: Seq<Left>, right: Seq<Right>,
    left_project: spec_fn(Left) -> Key, right_project: spec_fn(Right) -> Key,
    left_multiplicity: spec_fn(Left) -> int, right_multiplicity: spec_fn(Right) -> int,
    left_weight: spec_fn(Key) -> int, right_weight: spec_fn(Key) -> int)
    requires forall|key: Key| #![trigger class_total(left, left_project, left_multiplicity, key)]
        class_total(left, left_project, left_multiplicity, key) * left_weight(key) ==
        class_total(right, right_project, right_multiplicity, key) * right_weight(key),
    ensures sum(left, |row: Left| left_multiplicity(row) * left_weight(left_project(row))) ==
        sum(right, |row: Right| right_multiplicity(row) * right_weight(right_project(row))),
{
    let joined = left.map_values(left_project) + right.map_values(right_project);
    let keys = distinct_support(joined);
    lemma_distinct_support(joined);
    assert forall|index: int| 0 <= index < left.len() implies keys.contains(left_project(#[trigger] left[index])) by {
        assert(joined[index] == left_project(left[index]));
        assert(joined.contains(left_project(left[index])));
    }
    assert forall|index: int| 0 <= index < right.len() implies keys.contains(right_project(#[trigger] right[index])) by {
        assert(joined[left.len() + index] == right_project(right[index]));
        assert(joined.contains(right_project(right[index])));
    }
    lemma_weighted_classes(left, keys, left_project, left_multiplicity, left_weight);
    lemma_weighted_classes(right, keys, right_project, right_multiplicity, right_weight);
    lemma_sum_ext_all(keys, |key: Key| class_total(left, left_project, left_multiplicity, key) * left_weight(key),
        |key: Key| class_total(right, right_project, right_multiplicity, key) * right_weight(key));
}

/// Use to replace a projected class count by a support indicator on unique rows.
pub proof fn lemma_unique_class_total<Row, Key>(rows: Seq<Row>, project: spec_fn(Row) -> Key, key: Key)
    requires rows.map_values(project).no_duplicates(),
    ensures class_total(rows, project, |row: Row| 1int, key) ==
        if rows.map_values(project).contains(key) { 1int } else { 0int },
{
    let keys = rows.map_values(project);
    let weight = |row: Row| if project(row) == key { 1int } else { 0int };
    if keys.contains(key) {
        let position = choose|position: int| #![trigger keys[position]]
            0 <= position < keys.len() && keys[position] == key;
        assert forall|index: int| 0 <= index < rows.len() && index != position implies
            weight(#[trigger] rows[index]) == 0 by {
            assert(keys[index] == project(rows[index]));
        }
        lemma_sum_single_match(rows, weight, position);
    } else {
        assert forall|index: int| 0 <= index < rows.len() implies weight(#[trigger] rows[index]) == 0 by {
            assert(keys[index] == project(rows[index]));
        }
        lemma_sum_zero(rows, weight);
    }
    lemma_sum_ext_all(rows, weight, |row: Row| if project(row) == key { (|row: Row| 1int)(row) } else { 0int });
}

/// Use to turn an exact, duplicate-free position index into a filtered bag sum.
pub proof fn lemma_sum_selected<Row>(rows: Seq<Row>, positions: Seq<usize>, keep: spec_fn(Row) -> bool, weight: spec_fn(Row) -> int)
    requires positions.no_duplicates(),
        rows.len() <= usize::MAX,
        forall|position: usize| #[trigger] positions.contains(position) <==> position < rows.len() && keep(rows[position as int]),
    ensures sum(positions, |position: usize| weight(rows[position as int])) == sum(rows, |row: Row| if keep(row) { weight(row) } else { 0 }),
{
    let domain = rng(rows.len() as int);
    let left_project = |position: usize| position;
    let right_project = |position: int| position as usize;
    let left_multiplicity = |position: usize| 1int;
    let right_multiplicity = |position: int| if keep(rows[position]) { 1int } else { 0int };
    let class_weight = |position: usize| weight(rows[position as int]);
    assert forall|position: int| 0 <= position < positions.len() implies (#[trigger] positions[position]) < rows.len() by {
        assert(positions.contains(positions[position]));
    }
    assert forall|key: usize| #[trigger] class_total(positions, left_project, left_multiplicity, key) ==
        class_total(domain, right_project, right_multiplicity, key) by {
        let left = |position: usize| if position == key { 1int } else { 0int };
        let right = |position: int| if position as usize == key { right_multiplicity(position) } else { 0int };
        if positions.contains(key) {
            let selected = choose|selected: int| 0 <= selected < positions.len() && (#[trigger] positions[selected]) == key;
            assert forall|other: int| 0 <= other < positions.len() && other != selected implies left(#[trigger] positions[other]) == 0 by {
                assert(positions[other] != positions[selected]);
            }
            lemma_sum_single_match(positions, left, selected);
        } else { lemma_sum_zero(positions, left); }
        if key < rows.len() {
            assert forall|other: int| 0 <= other < domain.len() && other != key implies right(#[trigger] domain[other]) == 0 by {
                assert(domain[other] == other);
                assert(0 <= other < rows.len() <= usize::MAX);
            }
            lemma_sum_single_match(domain, right, key as int);
        } else {
            assert forall|other: int| 0 <= other < domain.len() implies right(#[trigger] domain[other]) == 0 by {
                assert(domain[other] == other);
                assert(0 <= other < rows.len() <= usize::MAX);
            }
            lemma_sum_zero(domain, right);
        }
        lemma_sum_ext_all(positions, left, |position: usize| if left_project(position) == key { left_multiplicity(position) } else { 0 });
        lemma_sum_ext_all(domain, right, |position: int| if right_project(position) == key { right_multiplicity(position) } else { 0 });
    }
    lemma_equal_classes(positions, domain, left_project, right_project, left_multiplicity, right_multiplicity, class_weight);
    lemma_sum_ext_all(positions, |position: usize| left_multiplicity(position) * class_weight(left_project(position)), |position: usize| weight(rows[position as int]));
    lemma_sum_aligned(domain, rows, |position: int| right_multiplicity(position) * class_weight(right_project(position)), |row: Row| if keep(row) { weight(row) } else { 0 });
}
pub mod sequence_updates {
use vstd::prelude::*;
use verus_common::sums::*;
verus! {
use verus_common::sums::{sum, lemma_sum_push};
/// Use after Vec::push to transfer support membership, including keys different from the appended key.
pub proof fn lemma_push_membership<Value>(rows: Seq<Value>, value: Value)
    ensures forall|target: Value| #[trigger] rows.push(value).contains(target)
        <==> rows.contains(target) || target == value,
{
    assert forall|target: Value| #[trigger] rows.push(value).contains(target)
        <==> rows.contains(target) || target == value by {
        if rows.push(value).contains(target) {
            let offset = choose|offset: int| 0 <= offset < rows.push(value).len()
                && #[trigger] rows.push(value)[offset] == target;
            if offset < rows.len() { assert(rows[offset] == target); }
        } else if rows.contains(target) {
            let offset = choose|offset: int| 0 <= offset < rows.len() && #[trigger] rows[offset] == target;
            assert(rows.push(value)[offset] == target);
        } else if target == value {
            assert(rows.push(value)[rows.len() as int] == target);
        }
    }
}
/// Use for loop invariants where the accumulated value is a sum over a processed prefix.
pub proof fn lemma_prefix_push<Value>(rows: Seq<Value>, index: int, value: Value, weight: spec_fn(Value) -> int)
    requires 0 <= index < rows.len(), value == rows[index],
    ensures sum(rows.take(index + 1), weight) == sum(rows.take(index), weight) + weight(value),
{
    assert(rows.take(index + 1) =~= rows.take(index).push(value));
    lemma_sum_push(rows.take(index), value, weight);
}
/// Use for signed aggregate weights when an absent multiplicity makes a chained product zero.
/// Origin: checked-arithmetic accumulator proofs; pass the zero premise explicitly to nonlinear arithmetic.
pub proof fn lemma_mul_zero_left(a: int, b: int, c: int)
    requires a == 0 || b == 0,
    ensures a * b * c == 0,
{
    assert(a * b * c == 0) by (nonlinear_arith) requires a == 0 || b == 0;
}
/// Use when an absent map entry or a filtered row contributes a zero product.
/// Origin: checked-arithmetic accumulator proofs; pass the zero premise explicitly to nonlinear arithmetic.
pub proof fn lemma_mul_zero(a: int, b: int)
    requires a == 0 || b == 0,
    ensures a * b == 0,
{
    assert(a * b == 0) by (nonlinear_arith) requires a == 0 || b == 0;
}
/// Use to transfer element membership through a sort or any transformation preserving all weighted sums.
/// Origin: auxiliary-state result assembly, where sorted output must retain every qualifying group.
pub proof fn lemma_bag_contains<Value>(before: Seq<Value>, after: Seq<Value>, value: Value)
    requires forall|weight: spec_fn(Value) -> int| #[trigger] verus_common::sums::sum(after, weight) == verus_common::sums::sum(before, weight),
    ensures before.contains(value) <==> after.contains(value),
{
    let weight = verus_common::sums::eqv(value);
    if after.contains(value) {
        let target = choose|target: int| 0 <= target < after.len() && #[trigger] after[target] == value;
        verus_common::sums::lemma_sum_pos(after, weight, target);
        assert(verus_common::sums::sum(before, weight) > 0);
        let source = verus_common::sums::lemma_sum_nonzero_witness(before, weight);
        assert(before[source] == value);
    }
    if before.contains(value) {
        let source = choose|source: int| 0 <= source < before.len() && #[trigger] before[source] == value;
        verus_common::sums::lemma_sum_pos(before, weight, source);
        assert(verus_common::sums::sum(after, weight) > 0);
        let target = verus_common::sums::lemma_sum_nonzero_witness(after, weight);
        assert(after[target] == value);
    }
}
/// Use after a bag-preserving sort when result uniqueness is a required postcondition.
/// Origin: auxiliary-state result assembly; remove one repeated occurrence to contradict its original indicator sum.
pub proof fn lemma_bag_no_duplicates<Value>(before: Seq<Value>, after: Seq<Value>)
    requires before.no_duplicates(), forall|weight: spec_fn(Value) -> int| #[trigger] verus_common::sums::sum(after, weight) == verus_common::sums::sum(before, weight),
    ensures after.no_duplicates(),
{
    assert forall|left: int, right: int| 0 <= left < after.len() && 0 <= right < after.len() && left != right implies
        (#[trigger] after[left]) != (#[trigger] after[right]) by {
        if after[left] == after[right] {
            let value = after[left];
            let weight = verus_common::sums::eqv(value);
            lemma_bag_contains(before, after, value);
            assert(after.contains(value));
            assert(before.contains(value));
            let source = choose|source: int| 0 <= source < before.len() && #[trigger] before[source] == value;
            assert forall|offset: int| 0 <= offset < before.len() && offset != source implies weight(#[trigger] before[offset]) == 0 by {
                assert(before[offset] != before[source]);
            }
            verus_common::sums::lemma_sum_single_match(before, weight, source);
            let removed = after.remove(left);
            let target = if right < left { right } else { right - 1 };
            assert(removed[target] == value);
            verus_common::sums::lemma_sum_pos(removed, weight, target);
            verus_common::sums::lemma_sum_remove(after, left, weight);
            assert(verus_common::sums::sum(after, weight) > 1);
            assert(verus_common::sums::sum(before, weight) == 1);
        }
    }
}
}
}

/// Use to transfer intrinsic ordered-index validity and map/sequence models into local proofs.
pub proof fn lemma_ordered_model<Value>(index: &ordered::Index<Value>)
    requires index.wf(),
    ensures ordered::ordered_rows(index.entries()),
        forall|key: ordered::KeyView| #[trigger] index.at(key) ==
            if index.view().dom().contains(key) { Some(index.view()[key]) } else { None },
        forall|position: int| 0 <= position < index.entries().len() ==>
            #[trigger] index.at(index.entries()[position].0) == Some(index.entries()[position].1),
{
    index.map_model();
    index.model();
}

/// Use after sharing a persistent ordered index, to frame every lookup through map equality.
pub proof fn lemma_ordered_equal<Value>(left: &ordered::Index<Value>, right: &ordered::Index<Value>)
    requires left.wf(), right.wf(), left.view() == right.view(),
    ensures forall|key: ordered::KeyView| #[trigger] left.at(key) == #[trigger] right.at(key),
{
    left.map_model();
    right.map_model();
}

/// Use before widening and multiplying two signed i64 inputs in an i128 accumulator.
pub proof fn lemma_i64_product(left: i64, right: i64)
    ensures i128::MIN <= left as int * right as int <= i128::MAX,
{
    assert(i128::MIN <= left as int * right as int <= i128::MAX) by (nonlinear_arith)
        requires i64::MIN <= left as int <= i64::MAX, i64::MIN <= right as int <= i64::MAX;
}

/// Use to preserve row representability or a support property through a signed bag capture.
pub proof fn lemma_bag_step_property<Row>(before: Seq<Row>, after: Seq<Row>, changes: Seq<(Row, int)>, property: spec_fn(Row) -> bool)
    requires bag_step(before, after, changes),
        forall|position: int| 0 <= position < before.len() ==> property(#[trigger] before[position]),
        forall|position: int| 0 <= position < changes.len() ==> property((#[trigger] changes[position]).0),
    ensures forall|position: int| 0 <= position < after.len() ==> property(#[trigger] after[position]),
{
    let bad = |row: Row| if property(row) { 0int } else { 1int };
    lemma_sum_zero(before, bad);
    lemma_sum_zero(changes, |change: (Row, int)| change.1 * bad(change.0));
    assert(sum(after, bad) == 0);
    assert forall|position: int| 0 <= position < after.len() implies property(#[trigger] after[position]) by {
        if !property(after[position]) { lemma_sum_pos(after, bad, position); }
    }
}
pub proof fn lemma_sum_mapped_prefix<Row, Projected>(rows: Seq<Row>, index: int, projection: spec_fn(Row) -> Projected, weight: spec_fn(Projected) -> int) requires 0 <= index < rows.len(), ensures sum(rows.take(index + 1).map_values(projection), weight) == sum(rows.take(index).map_values(projection), weight) + weight(projection(rows[index])), { let prefix = rows.take(index).map_values(projection); assert(rows.take(index + 1).map_values(projection) =~= prefix.push(projection(rows[index]))); lemma_sum_push(prefix, projection(rows[index]), weight); }
pub proof fn lemma_sum_transpose<A, B>(left: Seq<A>, right: Seq<B>, weight: spec_fn(A, B) -> int) ensures sum(left, |left_row: A| sum(right, |right_row: B| weight(left_row, right_row))) == sum(right, |right_row: B| sum(left, |left_row: A| weight(left_row, right_row))), { lemma_sum_swap(left, right, weight, |left_row: A| sum(right, |right_row: B| weight(left_row, right_row)), |right_row: B| sum(left, |left_row: A| weight(left_row, right_row))); }
#[verifier::auto_ext_equal(assert, assert_by, ensures)]
pub proof fn lemma_sum_exchange_pairs<A, B, C, D>(first: Seq<A>, second: Seq<B>, third: Seq<C>, fourth: Seq<D>, weight: spec_fn(A, B, C, D) -> int)
    ensures sum(first, |first_row: A| sum(second, |second_row: B| sum(third, |third_row: C| sum(fourth, |fourth_row: D| weight(first_row, second_row, third_row, fourth_row))))) ==
        sum(third, |third_row: C| sum(fourth, |fourth_row: D| sum(first, |first_row: A| sum(second, |second_row: B| weight(first_row, second_row, third_row, fourth_row))))),
{ let original = |first_row: A| sum(second, |second_row: B| sum(third, |third_row: C| sum(fourth, |fourth_row: D| weight(first_row, second_row, third_row, fourth_row))));
    let transposed = |first_row: A| sum(third, |third_row: C| sum(fourth, |fourth_row: D| sum(second, |second_row: B| weight(first_row, second_row, third_row, fourth_row))));
    assert forall|first_row: A| #[trigger] original(first_row) == transposed(first_row) by {
        let inner_left = |third_row: C| sum(second, |second_row: B| sum(fourth, |fourth_row: D| weight(first_row, second_row, third_row, fourth_row)));
        let inner_right = |third_row: C| sum(fourth, |fourth_row: D| sum(second, |second_row: B| weight(first_row, second_row, third_row, fourth_row)));
        let middle_weight = |second_row: B, third_row: C| sum(fourth, |fourth_row: D| weight(first_row, second_row, third_row, fourth_row));
        let middle_left = |second_row: B| sum(third, |third_row: C| sum(fourth, |fourth_row: D| weight(first_row, second_row, third_row, fourth_row)));
        assert forall|second_row: B| #[trigger] middle_left(second_row) == sum(third, |third_row: C| middle_weight(second_row, third_row)) by {
            assert((|third_row: C| middle_weight(second_row, third_row)) =~= (|third_row: C| sum(fourth, |fourth_row: D| weight(first_row, second_row, third_row, fourth_row))));
        } // exchange middle left extensionality
        assert forall|third_row: C| #[trigger] inner_left(third_row) == sum(second, |second_row: B| middle_weight(second_row, third_row)) by {
            assert((|second_row: B| middle_weight(second_row, third_row)) =~= (|second_row: B| sum(fourth, |fourth_row: D| weight(first_row, second_row, third_row, fourth_row))));
        } // exchange middle right extensionality
        lemma_sum_swap(second, third, middle_weight, middle_left, inner_left);
        assert forall|third_row: C| #[trigger] inner_left(third_row) == inner_right(third_row) by {
            let last_weight = |second_row: B, fourth_row: D| weight(first_row, second_row, third_row, fourth_row);
            let last_left = |second_row: B| sum(fourth, |fourth_row: D| weight(first_row, second_row, third_row, fourth_row));
            let last_right = |fourth_row: D| sum(second, |second_row: B| weight(first_row, second_row, third_row, fourth_row));
            assert forall|second_row: B| #[trigger] last_left(second_row) == sum(fourth, |fourth_row: D| last_weight(second_row, fourth_row)) by {
                assert((|fourth_row: D| last_weight(second_row, fourth_row)) =~= (|fourth_row: D| weight(first_row, second_row, third_row, fourth_row)));
            } // exchange last left extensionality
            assert forall|fourth_row: D| #[trigger] last_right(fourth_row) == sum(second, |second_row: B| last_weight(second_row, fourth_row)) by {
                assert((|second_row: B| last_weight(second_row, fourth_row)) =~= (|second_row: B| weight(first_row, second_row, third_row, fourth_row)));
            } // exchange last right extensionality
            lemma_sum_swap(second, fourth, last_weight, last_left, last_right);
        } // pair exchange inner second/fourth
        lemma_sum_ext_all(third,
            |third_row: C| sum(second, |second_row: B| sum(fourth, |fourth_row: D| weight(first_row, second_row, third_row, fourth_row))),
            |third_row: C| sum(fourth, |fourth_row: D| sum(second, |second_row: B| weight(first_row, second_row, third_row, fourth_row))));
    } // pair exchange first-row equality
    lemma_sum_ext_all(first, original, transposed);
    let outer_left = |third_row: C| sum(first, |first_row: A| sum(fourth, |fourth_row: D| sum(second, |second_row: B| weight(first_row, second_row, third_row, fourth_row))));
    let outer_right = |third_row: C| sum(fourth, |fourth_row: D| sum(first, |first_row: A| sum(second, |second_row: B| weight(first_row, second_row, third_row, fourth_row))));
    let outer_weight = |first_row: A, third_row: C| sum(fourth, |fourth_row: D| sum(second, |second_row: B| weight(first_row, second_row, third_row, fourth_row)));
    assert forall|first_row: A| #[trigger] transposed(first_row) == sum(third, |third_row: C| outer_weight(first_row, third_row)) by {
        assert((|third_row: C| outer_weight(first_row, third_row)) =~= (|third_row: C| sum(fourth, |fourth_row: D| sum(second, |second_row: B| weight(first_row, second_row, third_row, fourth_row)))));
    } // exchange outer left extensionality
    assert forall|third_row: C| #[trigger] outer_left(third_row) == sum(first, |first_row: A| outer_weight(first_row, third_row)) by {
        assert((|first_row: A| outer_weight(first_row, third_row)) =~= (|first_row: A| sum(fourth, |fourth_row: D| sum(second, |second_row: B| weight(first_row, second_row, third_row, fourth_row)))));
    } // exchange outer right extensionality
    lemma_sum_swap(first, third, outer_weight, transposed, outer_left);
    assert forall|third_row: C| #[trigger] outer_left(third_row) == outer_right(third_row) by {
        let end_weight = |first_row: A, fourth_row: D| sum(second, |second_row: B| weight(first_row, second_row, third_row, fourth_row));
        let end_left = |first_row: A| sum(fourth, |fourth_row: D| sum(second, |second_row: B| weight(first_row, second_row, third_row, fourth_row)));
        let end_right = |fourth_row: D| sum(first, |first_row: A| sum(second, |second_row: B| weight(first_row, second_row, third_row, fourth_row)));
        assert forall|first_row: A| #[trigger] end_left(first_row) == sum(fourth, |fourth_row: D| end_weight(first_row, fourth_row)) by {
            assert((|fourth_row: D| end_weight(first_row, fourth_row)) =~= (|fourth_row: D| sum(second, |second_row: B| weight(first_row, second_row, third_row, fourth_row))));
        } // exchange end left extensionality
        assert forall|fourth_row: D| #[trigger] end_right(fourth_row) == sum(first, |first_row: A| end_weight(first_row, fourth_row)) by {
            assert((|first_row: A| end_weight(first_row, fourth_row)) =~= (|first_row: A| sum(second, |second_row: B| weight(first_row, second_row, third_row, fourth_row))));
        } // exchange end right extensionality
        lemma_sum_swap(first, fourth, end_weight, end_left, end_right);
    } // pair exchange first/fourth
    lemma_sum_ext_all(third,
        |third_row: C| sum(first, |first_row: A| sum(fourth, |fourth_row: D| sum(second, |second_row: B| weight(first_row, second_row, third_row, fourth_row)))),
        |third_row: C| sum(fourth, |fourth_row: D| sum(first, |first_row: A| sum(second, |second_row: B| weight(first_row, second_row, third_row, fourth_row)))));
} // lemma_sum_exchange_pairs
/// Use when only the pages in a finite input sequence, rather than every possible page, are empty.
pub proof fn lemma_flat_empty_at<Value>(pages: Seq<Seq<Value>>)
    requires forall|position: int| 0 <= position < pages.len() ==> (#[trigger] pages[position]).len() == 0,
    ensures verus_common::rel::flat(pages).len() == 0,
    decreases pages.len(),
{   if pages.len() > 0 {
        assert forall|position: int| 0 <= position < pages.drop_last().len() implies
            (#[trigger] pages.drop_last()[position]).len() == 0 by { assert(pages.drop_last()[position] == pages[position]); }
        lemma_flat_empty_at(pages.drop_last());
        assert(pages.last().len() == 0); } }
/// Fixed-width unsigned big-endian bytes for composite ordered-map keys.
pub open spec fn be_u64(value: u64) -> Seq<u8> {
    Seq::new(8, |position: int| (value >> (((7 - position) * 8) as u32)) as u8) }
/// Use to unfold a full-width byte key without symbolic sequence slicing or variable shifts.
pub proof fn lemma_be_u64_literal(value: u64)
    ensures be_u64(value) == seq![(value >> 56) as u8, (value >> 48) as u8,
        (value >> 40) as u8, (value >> 32) as u8, (value >> 24) as u8,
        (value >> 16) as u8, (value >> 8) as u8, value as u8],
{   let literal = seq![(value >> 56) as u8, (value >> 48) as u8,
        (value >> 40) as u8, (value >> 32) as u8, (value >> 24) as u8,
        (value >> 16) as u8, (value >> 8) as u8, value as u8];
    assert((value >> 0u32) == value) by (bit_vector);
    assert forall|position: int| 0 <= position < 8 implies #[trigger] be_u64(value)[position] == literal[position] by {
        if position == 0 {} else if position == 1 {} else if position == 2 {} else if position == 3 {}
        else if position == 4 {} else if position == 5 {} else if position == 6 {} else {} }
    assert(be_u64(value) =~= literal); }
/// Use to justify unsigned numeric order in a fixed-width byte component of an ordered key.
pub proof fn lemma_be_u64_order(left: u64, right: u64)
    ensures bytes_lt(be_u64(left), be_u64(right)) == (left < right),
        (be_u64(left) == be_u64(right)) == (left == right),
{   lemma_be_u64_literal(left);
    lemma_be_u64_literal(right);
    assert((left < right) == (
        ((left >> 56) as u8) < ((right >> 56) as u8) || (left >> 56) as u8 == (right >> 56) as u8 && (
        ((left >> 48) as u8) < ((right >> 48) as u8) || (left >> 48) as u8 == (right >> 48) as u8 && (
        ((left >> 40) as u8) < ((right >> 40) as u8) || (left >> 40) as u8 == (right >> 40) as u8 && (
        ((left >> 32) as u8) < ((right >> 32) as u8) || (left >> 32) as u8 == (right >> 32) as u8 && (
        ((left >> 24) as u8) < ((right >> 24) as u8) || (left >> 24) as u8 == (right >> 24) as u8 && (
        ((left >> 16) as u8) < ((right >> 16) as u8) || (left >> 16) as u8 == (right >> 16) as u8 && (
        ((left >> 8) as u8) < ((right >> 8) as u8) || (left >> 8) as u8 == (right >> 8) as u8 && (
        (left as u8) < (right as u8)))))))))) by (bit_vector);
    reveal_with_fuel(bytes_lt, 9);
    if be_u64(left) == be_u64(right) {
        assert(left == right) by (bit_vector)
            requires (left >> 56) as u8 == (right >> 56) as u8,
                (left >> 48) as u8 == (right >> 48) as u8,
                (left >> 40) as u8 == (right >> 40) as u8,
                (left >> 32) as u8 == (right >> 32) as u8,
                (left >> 24) as u8 == (right >> 24) as u8,
                (left >> 16) as u8 == (right >> 16) as u8,
                (left >> 8) as u8 == (right >> 8) as u8,
                left as u8 == right as u8; } }

/// Use to compare two fixed-width key prefixes followed by arbitrary tie-breaker bytes.
pub proof fn lemma_bytes_fixed_concat(left: Seq<u8>, right: Seq<u8>, left_tail: Seq<u8>, right_tail: Seq<u8>)
    requires left.len() == right.len(),
    ensures bytes_lt(left + left_tail, right + right_tail)
        == (bytes_lt(left, right) || (left == right && bytes_lt(left_tail, right_tail))),
    decreases left.len(),
{   reveal_with_fuel(bytes_lt, 2);
    if left.len() == 0 {
        assert(left =~= Seq::<u8>::empty());
        assert(right =~= Seq::<u8>::empty());
        assert(left + left_tail =~= left_tail);
        assert(right + right_tail =~= right_tail);
    } else if left[0] == right[0] {
        lemma_bytes_fixed_concat(left.drop_first(), right.drop_first(), left_tail, right_tail);
        assert((left + left_tail).drop_first() =~= left.drop_first() + left_tail);
        assert((right + right_tail).drop_first() =~= right.drop_first() + right_tail);
        assert(left =~= seq![left[0]] + left.drop_first());
        assert(right =~= seq![right[0]] + right.drop_first());
        assert((left == right) == (left.drop_first() == right.drop_first()));
    } else {
        assert((left + left_tail)[0] == left[0]);
        assert((right + right_tail)[0] == right[0]); } }

/// Full signed-i128 descending rank followed by a signed-i64 ascending tie-breaker.
pub open spec fn i128_rank(value: i128, tie: i64) -> ordered::KeyView {
    ((!((value >> 64) as i64)) as int, be_u64(!(value as u64)) + be_u64((tie as u64) ^ (1u64 << 63))) }

/// Use for value-descending full-width ordered indexes without negating i128::MIN.
pub proof fn lemma_i128_rank_order(left: i128, right: i128, left_tie: i64, right_tie: i64)
    ensures ordered::key_lt(i128_rank(left, left_tie), i128_rank(right, right_tie))
        == (left > right || (left == right && left_tie < right_tie)),
        (i128_rank(left, left_tie) == i128_rank(right, right_tie)) == (left == right && left_tie == right_tie),
{   let left_high = (left >> 64) as i64;
    let right_high = (right >> 64) as i64;
    let left_low = left as u64;
    let right_low = right as u64;
    let left_bias = (left_tie as u64) ^ (1u64 << 63);
    let right_bias = (right_tie as u64) ^ (1u64 << 63);
    assert((left > right) == (left_high > right_high || (left_high == right_high && left_low > right_low))) by (bit_vector)
        requires left_high == (left >> 64) as i64, right_high == (right >> 64) as i64,
            left_low == left as u64, right_low == right as u64;
    assert((left == right) == (left_high == right_high && left_low == right_low)) by (bit_vector)
        requires left_high == (left >> 64) as i64, right_high == (right >> 64) as i64,
            left_low == left as u64, right_low == right as u64;
    assert((!left_high < !right_high) == (left_high > right_high)) by (bit_vector);
    assert((!left_high == !right_high) == (left_high == right_high)) by (bit_vector);
    assert((!left_low < !right_low) == (left_low > right_low)) by (bit_vector);
    assert((!left_low == !right_low) == (left_low == right_low)) by (bit_vector);
    assert((left_bias < right_bias) == (left_tie < right_tie)) by (bit_vector)
        requires left_bias == (left_tie as u64) ^ (1u64 << 63), right_bias == (right_tie as u64) ^ (1u64 << 63);
    assert((left_bias == right_bias) == (left_tie == right_tie)) by (bit_vector)
        requires left_bias == (left_tie as u64) ^ (1u64 << 63), right_bias == (right_tie as u64) ^ (1u64 << 63);
    lemma_be_u64_order(!left_low, !right_low);
    lemma_be_u64_order(left_bias, right_bias);
    lemma_bytes_fixed_concat(be_u64(!left_low), be_u64(!right_low), be_u64(left_bias), be_u64(right_bias));
    if i128_rank(left, left_tie) == i128_rank(right, right_tie) {
        assert(be_u64(!left_low) == be_u64(!right_low)) by {
            assert((be_u64(!left_low) + be_u64(left_bias)).take(8) =~= be_u64(!left_low));
            assert((be_u64(!right_low) + be_u64(right_bias)).take(8) =~= be_u64(!right_low)); }
        assert(be_u64(left_bias) == be_u64(right_bias)) by {
            assert((be_u64(!left_low) + be_u64(left_bias)).skip(8) =~= be_u64(left_bias));
            assert((be_u64(!right_low) + be_u64(right_bias)).skip(8) =~= be_u64(right_bias)); } } }

/// Use for a final inclusive fence: every shorter byte key is below an all-255 sentinel.
pub proof fn lemma_bytes_max_sentinel(bytes: Seq<u8>, width: nat)
    requires bytes.len() < width,
    ensures bytes_lt(bytes, Seq::new(width, |position: int| 255u8)),
    decreases bytes.len(),
{   if bytes.len() > 0 && bytes[0] == 255 {
        lemma_bytes_max_sentinel(bytes.drop_first(), (width - 1) as nat);
        assert(Seq::new(width, |position: int| 255u8).drop_first()
            =~= Seq::new((width - 1) as nat, |position: int| 255u8)); } }

/// Use when seeking a bounded signed rank must always find the final sentinel page.
pub proof fn lemma_i128_rank_sentinel(value: i128, tie: i64)
    ensures i64::MIN <= i128_rank(value, tie).0 <= i64::MAX,
        i128_rank(value, tie).1.len() == 16,
        ordered::key_lt(i128_rank(value, tie), (i64::MAX as int, Seq::new(17, |position: int| 255u8))),
{   let number: i64 = !((value >> 64) as i64);
    assert(i128_rank(value, tie).0 == number as int);
    assert(i64::MIN <= number <= i64::MAX) by (bit_vector)
        requires number == !((value >> 64) as i64);
    lemma_bytes_max_sentinel(i128_rank(value, tie).1, 17); }
/// Use filtered, projected scan sums to transfer row properties; origin: Q20 representability.
pub proof fn lemma_projected_scan_property<Source, Target>(scanned: Seq<Target>, database: Seq<Source>, keep: spec_fn(Source) -> bool, project: spec_fn(Source) -> Target, property: spec_fn(Target) -> bool) requires forall|weight: spec_fn(Target) -> int| #[trigger] sum(scanned, weight) == sum(database, |row: Source| if keep(row) { weight(project(row)) } else { 0 }), forall|index: int| 0 <= index < scanned.len() ==> property(#[trigger] scanned[index]), ensures forall|index: int| 0 <= index < database.len() && keep(#[trigger] database[index]) ==> property(project(database[index])), { let bad = |row: Target| if property(row) { 0int } else { 1int }; let missing = |row: Source| if keep(row) { bad(project(row)) } else { 0int }; lemma_sum_zero(scanned, bad); assert(sum(database, missing) == 0); assert forall|index: int| 0 <= index < database.len() && keep(#[trigger] database[index]) implies property(project(database[index])) by { if !property(project(database[index])) { lemma_sum_pos(database, missing, index); } } }
pub proof fn lemma_flat_sorted<Value>(pages: Seq<Seq<Value>>, precedes: spec_fn(Value, Value) -> bool)
    requires forall|page: int, left: int, right: int| 0 <= page < pages.len() && 0 <= left < right < pages[page].len()
            ==> precedes(#[trigger] pages[page][left], #[trigger] pages[page][right]),
        forall|earlier: int, later: int, left: int, right: int| 0 <= earlier < later < pages.len()
            && 0 <= left < pages[earlier].len() && 0 <= right < pages[later].len()
            ==> precedes(#[trigger] pages[earlier][left], #[trigger] pages[later][right]),
    ensures verus_common::order::sorted_by(verus_common::rel::flat(pages), precedes),
    decreases pages.len(),
{   if pages.len() > 0 { let prefix = pages.drop_last();
        let last = pages.last();
        assert forall|page: int, left: int, right: int| 0 <= page < prefix.len() && 0 <= left < right < prefix[page].len()
            implies precedes(#[trigger] prefix[page][left], #[trigger] prefix[page][right]) by { assert(prefix[page] == pages[page]); }
        assert forall|earlier: int, later: int, left: int, right: int| 0 <= earlier < later < prefix.len()
            && 0 <= left < prefix[earlier].len() && 0 <= right < prefix[later].len()
            implies precedes(#[trigger] prefix[earlier][left], #[trigger] prefix[later][right]) by {
            assert(prefix[earlier] == pages[earlier]); assert(prefix[later] == pages[later]); }
        lemma_flat_sorted(prefix, precedes);
        let earlier_rows = verus_common::rel::flat(prefix);
        let all_rows = verus_common::rel::flat(pages);
        assert forall|left: int, right: int| 0 <= left < right < all_rows.len()
            implies precedes(#[trigger] all_rows[left], #[trigger] all_rows[right]) by {
            if right < earlier_rows.len() {
                assert(all_rows[left] == earlier_rows[left]); assert(all_rows[right] == earlier_rows[right]);
            } else if left >= earlier_rows.len() {
                assert(all_rows[left] == last[left - earlier_rows.len()]); assert(all_rows[right] == last[right - earlier_rows.len()]);
                assert(last == pages[pages.len() as int - 1]);
            } else { assert(earlier_rows.contains(earlier_rows[left]));
                verus_common::rel::lemma_flat_contains(prefix, earlier_rows[left]);
                let page = choose|page: int| 0 <= page < prefix.len() && (#[trigger] prefix[page]).contains(earlier_rows[left]);
                let position = choose|position: int| 0 <= position < prefix[page].len() && #[trigger] prefix[page][position] == earlier_rows[left];
                assert(pages[page][position] == all_rows[left]);
                assert(pages[pages.len() as int - 1][right - earlier_rows.len()] == all_rows[right]); } } } }
pub proof fn lemma_flat_take_step<Value>(pages: Seq<Seq<Value>>, position: int)
    requires 0 <= position < pages.len(),
    ensures verus_common::rel::flat(pages.take(position + 1)) == verus_common::rel::flat(pages.take(position)) + pages[position],
{   assert(pages.take(position + 1).drop_last() =~= pages.take(position));
    assert(pages.take(position + 1).last() == pages[position]); }
pub proof fn lemma_flat_take_prefix<Value>(pages: Seq<Seq<Value>>, position: int)
    requires 0 <= position <= pages.len(),
    ensures verus_common::rel::flat(pages.take(position)).len() <= verus_common::rel::flat(pages).len(),
        verus_common::rel::flat(pages).take(verus_common::rel::flat(pages.take(position)).len() as int)
            == verus_common::rel::flat(pages.take(position)),
    decreases pages.len() - position,
{   if position < pages.len() {
        lemma_flat_take_step(pages, position);
        lemma_flat_take_prefix(pages, position + 1);
        assert(verus_common::rel::flat(pages.take(position + 1)).take(verus_common::rel::flat(pages.take(position)).len() as int)
            =~= verus_common::rel::flat(pages.take(position)));
    } else { assert(pages.take(position) =~= pages); } }
/// Indicator weight shared by count and support proofs.
pub open spec fn predicate_weight<Row>(predicate: spec_fn(Row) -> bool) -> spec_fn(Row) -> int { |row: Row| if predicate(row) { 1int } else { 0int } }
/// Multiplicity-sensitive count of rows satisfying a predicate.
pub open spec fn predicate_count<Row>(rows: Seq<Row>, predicate: spec_fn(Row) -> bool) -> int { sum(rows, predicate_weight(predicate)) }
/// Use to transfer EXISTS through equality of filtered bag counts, without assuming row uniqueness.
pub proof fn lemma_predicate_support<Row>(rows: Seq<Row>, predicate: spec_fn(Row) -> bool) ensures predicate_count(rows, predicate) >= 0, (predicate_count(rows, predicate) > 0) == (exists|position: int| #![trigger rows[position]] 0 <= position < rows.len() && predicate(rows[position])), { let weight = predicate_weight(predicate); lemma_sum_nonneg(rows, weight); if exists|position: int| #![trigger rows[position]] 0 <= position < rows.len() && predicate(rows[position]) { let position = choose|position: int| #![trigger rows[position]] 0 <= position < rows.len() && predicate(rows[position]); lemma_sum_pos(rows, weight, position); } if predicate_count(rows, predicate) > 0 { let position = lemma_sum_nonzero_witness(rows, weight); assert(predicate(rows[position])); } }

/// Use to move a row fold outside a pair of independent dimension folds while retaining duplicate multiplicities.
#[verifier::auto_ext_equal(assert, assert_by, ensures)]
pub proof fn lemma_sum_rotate_three<A, B, C>(first: Seq<A>, second: Seq<B>, third: Seq<C>, weight: spec_fn(A, B, C) -> int) ensures sum(first, |first_row: A| sum(second, |second_row: B| sum(third, |third_row: C| weight(first_row, second_row, third_row)))) == sum(second, |second_row: B| sum(third, |third_row: C| sum(first, |first_row: A| weight(first_row, second_row, third_row)))), { let outer_weight = |first_row: A, second_row: B| sum(third, |third_row: C| weight(first_row, second_row, third_row)); let original = |first_row: A| sum(second, |second_row: B| sum(third, |third_row: C| weight(first_row, second_row, third_row))); let middle = |second_row: B| sum(first, |first_row: A| sum(third, |third_row: C| weight(first_row, second_row, third_row))); assert forall|first_row: A| #[trigger] original(first_row) == sum(second, |second_row: B| outer_weight(first_row, second_row)) by { assert((|second_row: B| outer_weight(first_row, second_row)) =~= (|second_row: B| sum(third, |third_row: C| weight(first_row, second_row, third_row)))); } assert forall|second_row: B| #[trigger] middle(second_row) == sum(first, |first_row: A| outer_weight(first_row, second_row)) by { assert((|first_row: A| outer_weight(first_row, second_row)) =~= (|first_row: A| sum(third, |third_row: C| weight(first_row, second_row, third_row)))); } lemma_sum_swap(first, second, outer_weight, original, middle); let result = |second_row: B| sum(third, |third_row: C| sum(first, |first_row: A| weight(first_row, second_row, third_row))); assert forall|second_row: B| #[trigger] middle(second_row) == result(second_row) by { let inner_weight = |first_row: A, third_row: C| weight(first_row, second_row, third_row); let inner_left = |first_row: A| sum(third, |third_row: C| weight(first_row, second_row, third_row)); let inner_right = |third_row: C| sum(first, |first_row: A| weight(first_row, second_row, third_row)); assert forall|first_row: A| #[trigger] inner_left(first_row) == sum(third, |third_row: C| inner_weight(first_row, third_row)) by { assert((|third_row: C| inner_weight(first_row, third_row)) =~= (|third_row: C| weight(first_row, second_row, third_row))); } assert forall|third_row: C| #[trigger] inner_right(third_row) == sum(first, |first_row: A| inner_weight(first_row, third_row)) by { assert((|first_row: A| inner_weight(first_row, third_row)) =~= (|first_row: A| weight(first_row, second_row, third_row))); } lemma_sum_swap(first, third, inner_weight, inner_left, inner_right); } lemma_sum_ext_all(second, middle, result); }
pub proof fn lemma_sequence_insert_members<Value>(items: Seq<Value>, position: int, value: Value)
    requires 0 <= position <= items.len(),
    ensures forall|query: Value| #[trigger] items.insert(position, value).contains(query) <==> items.contains(query) || query == value,
{   let inserted = items.insert(position, value);
    assert forall|query: Value| #[trigger] inserted.contains(query) <==> items.contains(query) || query == value by {
        if inserted.contains(query) {
            let offset = choose|offset: int| 0 <= offset < inserted.len() && #[trigger] inserted[offset] == query;
            if offset < position { assert(items[offset] == query); }
            else if offset > position { assert(items[offset - 1] == query); }
        } if items.contains(query) {
            let offset = choose|offset: int| 0 <= offset < items.len() && #[trigger] items[offset] == query;
            if offset < position { assert(inserted[offset] == query); }
            else { assert(inserted[offset + 1] == query); }
        } if query == value { assert(inserted[position] == query); } } }
pub proof fn lemma_sequence_remove_members<Value>(items: Seq<Value>, position: int)
    requires 0 <= position < items.len(), items.no_duplicates(),
    ensures forall|query: Value| #[trigger] items.remove(position).contains(query) <==> items.contains(query) && query != items[position],
{   let removed = items.remove(position);
    assert forall|query: Value| #[trigger] removed.contains(query) <==> items.contains(query) && query != items[position] by {
        if removed.contains(query) {
            let offset = choose|offset: int| 0 <= offset < removed.len() && #[trigger] removed[offset] == query;
            let source = if offset < position { offset } else { offset + 1 };
            assert(items[source] == query); assert(source != position);
        } if items.contains(query) && query != items[position] {
            let offset = choose|offset: int| 0 <= offset < items.len() && #[trigger] items[offset] == query;
            if offset < position { assert(removed[offset] == query); }
            else { assert(removed[offset - 1] == query); } } } }
/// Use to rotate three bag folds with named outer and inner weights, avoiding nested closure identity obligations at each query call site.
pub proof fn lemma_sum_rotate_named<A, B, C>(first: Seq<A>, second: Seq<B>, third: Seq<C>, weight: spec_fn(A, B, C) -> int, original: spec_fn(A) -> int, target: spec_fn(B, C) -> int) requires forall|first_row: A| #[trigger] original(first_row) == sum(second, |second_row: B| sum(third, |third_row: C| weight(first_row, second_row, third_row))), forall|second_row: B, third_row: C| #[trigger] target(second_row, third_row) == sum(first, |first_row: A| weight(first_row, second_row, third_row)), ensures sum(first, original) == sum(second, |second_row: B| sum(third, |third_row: C| target(second_row, third_row))), { let expanded = |first_row: A| sum(second, |second_row: B| sum(third, |third_row: C| weight(first_row, second_row, third_row))); lemma_sum_ext_all(first, original, expanded); lemma_sum_rotate_three(first, second, third, weight); let middle = |second_row: B| sum(third, |third_row: C| sum(first, |first_row: A| weight(first_row, second_row, third_row))); let result = |second_row: B| sum(third, |third_row: C| target(second_row, third_row)); assert forall|second_row: B| #[trigger] middle(second_row) == result(second_row) by { lemma_sum_ext_all(third, |third_row: C| sum(first, |first_row: A| weight(first_row, second_row, third_row)), |third_row: C| target(second_row, third_row)); } lemma_sum_ext_all(second, middle, result); }
pub open spec fn ordered_weight_at<Value>(index: ordered::Index<Value>, key: ordered::KeyView, weight: spec_fn((ordered::KeyView, Value)) -> int) -> int { match index.at(key) { Some(value) => weight((key, value)), None => 0 } }
pub open spec fn ordered_dot<Left, Right>(left: ordered::Index<Left>, right: ordered::Index<Right>, left_weight: spec_fn((ordered::KeyView, Left)) -> int, right_weight: spec_fn((ordered::KeyView, Right)) -> int) -> int { sum(left.entries(), |row: (ordered::KeyView, Left)| left_weight(row) * ordered_weight_at(right, row.0, right_weight)) }
pub proof fn lemma_ordered_selected_weight<Value>(index: &ordered::Index<Value>, key: ordered::KeyView, weight: spec_fn((ordered::KeyView, Value)) -> int, factor: int, selected: spec_fn((ordered::KeyView, Value)) -> int) requires index.wf(), forall|row: (ordered::KeyView, Value)| #[trigger] selected(row) == if row.0 == key { weight(row) * factor } else { 0 }, ensures sum(index.entries(), selected) == ordered_weight_at(*index, key, weight) * factor,
{ let scaled = |row: (ordered::KeyView, Value)| weight(row) * factor;
    let filtered = |row: (ordered::KeyView, Value)| if row.0 == key { scaled(row) } else { 0 };
    ordered::lemma_key_contribution(index, key, scaled);
    lemma_sum_ext_all(index.entries(), selected, filtered); reveal(ordered::contribution); }
pub proof fn lemma_ordered_dot_transpose<Left, Right>(left: &ordered::Index<Left>, right: &ordered::Index<Right>, left_weight: spec_fn((ordered::KeyView, Left)) -> int, right_weight: spec_fn((ordered::KeyView, Right)) -> int)
    requires left.wf(), right.wf(),
    ensures ordered_dot(*left, *right, left_weight, right_weight) == ordered_dot(*right, *left, right_weight, left_weight),
{ let pair = |first: (ordered::KeyView, Left), second: (ordered::KeyView, Right)| if first.0 == second.0 { left_weight(first) * right_weight(second) } else { 0int };
    let original = |first: (ordered::KeyView, Left)| left_weight(first) * ordered_weight_at(*right, first.0, right_weight);
    let expanded = |first: (ordered::KeyView, Left)| sum(right.entries(), |second: (ordered::KeyView, Right)| pair(first, second));
    assert forall|first: (ordered::KeyView, Left)| #[trigger] original(first) == expanded(first) by {
        let selected = |second: (ordered::KeyView, Right)| pair(first, second);
        assert forall|second: (ordered::KeyView, Right)| #[trigger] selected(second) == if second.0 == first.0 { right_weight(second) * left_weight(first) } else { 0 } by {
            assert(left_weight(first) * right_weight(second) == right_weight(second) * left_weight(first)) by (nonlinear_arith); }
        lemma_ordered_selected_weight(right, first.0, right_weight, left_weight(first), selected);
        assert(left_weight(first) * ordered_weight_at(*right, first.0, right_weight) == ordered_weight_at(*right, first.0, right_weight) * left_weight(first)) by (nonlinear_arith); }
    lemma_sum_ext_all(left.entries(), original, expanded);
    let transposed = |second: (ordered::KeyView, Right)| sum(left.entries(), |first: (ordered::KeyView, Left)| pair(first, second));
    lemma_sum_swap(left.entries(), right.entries(), pair, expanded, transposed);
    let target = |second: (ordered::KeyView, Right)| right_weight(second) * ordered_weight_at(*left, second.0, left_weight);
    assert forall|second: (ordered::KeyView, Right)| #[trigger] transposed(second) == target(second) by {
        let selected = |first: (ordered::KeyView, Left)| pair(first, second);
        lemma_ordered_selected_weight(left, second.0, left_weight, right_weight(second), selected);
        assert(ordered_weight_at(*left, second.0, left_weight) * right_weight(second) == right_weight(second) * ordered_weight_at(*left, second.0, left_weight)) by (nonlinear_arith); }
    lemma_sum_ext_all(right.entries(), transposed, target); }
pub proof fn lemma_sparse_partition_delta<Row, Key>(before: Seq<Row>, after: Seq<Row>, keys: Seq<Key>, key_of: spec_fn(Row) -> Key, before_weight: spec_fn(Row) -> int, after_weight: spec_fn(Row) -> int, before_per: spec_fn(Key) -> int, after_per: spec_fn(Key) -> int)
    requires keys.no_duplicates(), forall|key: Key| #[trigger] before_per(key) == sum(before, |row: Row| if key_of(row) == key { before_weight(row) } else { 0int }), forall|key: Key| #[trigger] after_per(key) == sum(after, |row: Row| if key_of(row) == key { after_weight(row) } else { 0int }), forall|key: Key| !keys.contains(key) ==> #[trigger] after_per(key) == before_per(key),
    ensures sum(after, after_weight) - sum(before, before_weight) == sum(keys, |key: Key| after_per(key) - before_per(key)),
{ broadcast use vstd::seq_lib::group_seq_properties;
    let source = before.map_values(key_of) + after.map_values(key_of) + keys;
    let domain = verus_common::rel::dedup(source); verus_common::rel::lemma_dedup(source);
    assert(domain.no_duplicates()); assert(distinct(domain));
    assert forall|index: int| 0 <= index < before.len() && before_weight(before[index]) != 0 implies domain.contains(key_of(#[trigger] before[index])) by { assert(before.map_values(key_of)[index] == key_of(before[index])); assert(source.contains(key_of(before[index]))); }
    assert forall|index: int| 0 <= index < after.len() && after_weight(after[index]) != 0 implies domain.contains(key_of(#[trigger] after[index])) by { assert(after.map_values(key_of)[index] == key_of(after[index])); assert(source.contains(key_of(after[index]))); }
    lemma_sum_group(before, domain, key_of, before_weight, before_per); lemma_sum_group(after, domain, key_of, after_weight, after_per);
    let difference = |key: Key| after_per(key) - before_per(key); lemma_sum_sub(domain, after_per, before_per, difference);
    assert forall|key: Key| #[trigger] difference(key) == sum(domain, |item: Key| if item == key { difference(item) } else { 0int }) by { if keys.contains(key) { assert(source.contains(key)); assert(domain.contains(key)); } let selected = |item: Key| if item == key { difference(key) } else { 0int }; lemma_sum_indicator_distinct(domain, key, difference(key), selected); lemma_sum_ext_all(domain, selected, |item: Key| if item == key { difference(item) } else { 0int }); }
    assert forall|index: int| 0 <= index < domain.len() && difference(domain[index]) != 0 implies keys.contains(#[trigger] domain[index]) by {}
    let identity = |key: Key| key; assert forall|key: Key| #[trigger] difference(key) == sum(domain, |item: Key| if identity(item) == key { difference(item) } else { 0int }) by { lemma_sum_ext_all(domain, |item: Key| if item == key { difference(item) } else { 0int }, |item: Key| if identity(item) == key { difference(item) } else { 0int }); }
    assert(distinct(keys)); lemma_sum_group(domain, keys, identity, difference, difference); }

} // verus!

verus! {

/// Width of an integer dyadic block; use for bounded canonical interval decompositions.
pub open spec fn dyadic_width(level: int) -> int
    decreases level,
{
    if level <= 0 { 1 } else { 2 * dyadic_width(level - 1) }
}

/// Use to bound levels in fixed-height dyadic covers and to relate a child width to its parent.
pub proof fn lemma_dyadic_width_bounds(level: int)
    requires 0 <= level <= 27,
    ensures
        1 <= dyadic_width(level) <= 134217728,
        level <= 26 ==> dyadic_width(level) <= 67108864,
        dyadic_width(26) == 67108864,
        dyadic_width(27) == 134217728,
        dyadic_width(level + 1) == 2 * dyadic_width(level),
{
    reveal_with_fuel(dyadic_width, 29);
    if level == 0 {} else if level == 1 {} else if level == 2 {} else if level == 3 {}
    else if level == 4 {} else if level == 5 {} else if level == 6 {} else if level == 7 {}
    else if level == 8 {} else if level == 9 {} else if level == 10 {} else if level == 11 {}
    else if level == 12 {} else if level == 13 {} else if level == 14 {} else if level == 15 {}
    else if level == 16 {} else if level == 17 {} else if level == 18 {} else if level == 19 {}
    else if level == 20 {} else if level == 21 {} else if level == 22 {} else if level == 23 {}
    else if level == 24 {} else if level == 25 {} else if level == 26 {} else {}
}

/// Use when deleting a counted cell from a strictly sorted fixed-width column.
pub proof fn lemma_sorted_remove_at<Value>(rows: Seq<Value>, position: int, less: spec_fn(Value, Value) -> bool)
    requires 0 <= position < rows.len(), verus_common::order::sorted_by(rows, less),
    ensures verus_common::order::sorted_by(rows.remove(position), less),
{
    let after = rows.remove(position);
    assert forall|left: int, right: int| #![trigger after[left], after[right]]
        0 <= left < right < after.len() implies less(after[left], after[right]) by {
        let old_left = if left < position { left } else { left + 1 };
        let old_right = if right < position { right } else { right + 1 };
        assert(after[left] == rows[old_left]);
        assert(after[right] == rows[old_right]);
        assert(less(rows[old_left], rows[old_right]));
    }
}

/// Use when appending the next source row to a filtered strictly sorted prefix.
pub proof fn lemma_sorted_filtered_append<Value>(source: Seq<Value>, prefix: Seq<Value>, position: int,
    less: spec_fn(Value, Value) -> bool)
    requires
        0 <= position < source.len(),
        verus_common::order::sorted_by(source, less),
        verus_common::order::sorted_by(prefix, less),
        forall|index: int| #![trigger prefix[index]] 0 <= index < prefix.len() ==>
            exists|origin: int| #![trigger source[origin]] 0 <= origin < position && prefix[index] == source[origin],
    ensures verus_common::order::sorted_by(prefix.push(source[position]), less),
{
    assert forall|index: int| #![trigger prefix[index]] 0 <= index < prefix.len() implies less(prefix[index], source[position]) by {
        let origin = choose|origin: int| #![trigger source[origin]] 0 <= origin < position && prefix[index] == source[origin];
        assert(less(source[origin], source[position]));
    }
    lemma_sorted_insert_at(prefix, prefix.len() as int, source[position], less);
    assert(prefix.push(source[position]) =~= prefix.insert(prefix.len() as int, source[position]));
}

/// Decode a fixed-width big-endian integer without allocation; use for persistent-index identities.
pub fn read_be_u64(source: &[u8], begin: usize) -> (result: u64)
    requires begin <= source.len(), source.len() - begin >= 8,
    ensures be_u64(result) == source@.subrange(begin as int, begin as int + 8),
{
    let byte0 = source[begin];
    let byte1 = source[begin + 1];
    let byte2 = source[begin + 2];
    let byte3 = source[begin + 3];
    let byte4 = source[begin + 4];
    let byte5 = source[begin + 5];
    let byte6 = source[begin + 6];
    let byte7 = source[begin + 7];
    let result = ((byte0 as u64) << 56) | ((byte1 as u64) << 48)
        | ((byte2 as u64) << 40) | ((byte3 as u64) << 32)
        | ((byte4 as u64) << 24) | ((byte5 as u64) << 16)
        | ((byte6 as u64) << 8) | byte7 as u64;
    proof {
        lemma_be_u64_literal(result);
        assert(((result >> 56) as u8) == byte0 && ((result >> 48) as u8) == byte1
            && ((result >> 40) as u8) == byte2 && ((result >> 32) as u8) == byte3
            && ((result >> 24) as u8) == byte4 && ((result >> 16) as u8) == byte5
            && ((result >> 8) as u8) == byte6 && (result as u8) == byte7) by (bit_vector)
            requires result == ((byte0 as u64) << 56) | ((byte1 as u64) << 48)
                | ((byte2 as u64) << 40) | ((byte3 as u64) << 32)
                | ((byte4 as u64) << 24) | ((byte5 as u64) << 16)
                | ((byte6 as u64) << 8) | byte7 as u64;
        assert(be_u64(result) =~= source@.subrange(begin as int, begin as int + 8));
    }
    result
}

/// Four-byte big-endian encoding, paired with read_be_u32 for DATE and fixed-width identifiers.
pub open spec fn be_u32(value: u32) -> Seq<u8> {
    Seq::new(4, |position: int| (value >> (((3 - position) * 8) as u32)) as u8)
}

/// Decode a four-byte big-endian integer; use for DATE fields in composite index identities.
pub fn read_be_u32(source: &[u8], begin: usize) -> (result: u32)
    requires begin <= source.len(), source.len() - begin >= 4,
    ensures be_u32(result) == source@.subrange(begin as int, begin as int + 4),
{
    let byte0 = source[begin];
    let byte1 = source[begin + 1];
    let byte2 = source[begin + 2];
    let byte3 = source[begin + 3];
    let result = ((byte0 as u32) << 24) | ((byte1 as u32) << 16) | ((byte2 as u32) << 8) | byte3 as u32;
    proof {
        assert(((result >> 24) as u8) == byte0 && ((result >> 16) as u8) == byte1
            && ((result >> 8) as u8) == byte2 && (result as u8) == byte3) by (bit_vector)
            requires result == ((byte0 as u32) << 24) | ((byte1 as u32) << 16) | ((byte2 as u32) << 8) | byte3 as u32;
        assert((result >> 0u32) == result) by (bit_vector);
        let literal = seq![byte0, byte1, byte2, byte3];
        assert forall|position: int| #![trigger be_u32(result)[position]] 0 <= position < 4 implies be_u32(result)[position] == literal[position] by {
            if position == 0 {} else if position == 1 {} else if position == 2 {} else {}
        }
        assert(be_u32(result) =~= literal);
        assert(literal =~= source@.subrange(begin as int, begin as int + 4));
    }
    result
}

/// Use in bottom-up dyadic covers: a contained tile with an excluded parent lies on an odd boundary.
pub proof fn lemma_dyadic_boundary(first: int, end: int, width: int, low: int, high: int, offset: int)
    requires
        width > 0,
        low * width >= first, (low - 1) * width < first,
        high * width <= end, (high + 1) * width > end,
        first <= offset * width, (offset + 1) * width <= end,
        !(first <= (offset / 2) * (2 * width) && (offset / 2 + 1) * (2 * width) <= end),
    ensures (offset == low && low % 2 == 1) || (offset + 1 == high && high % 2 == 1),
{
    assert(low <= offset < high) by (nonlinear_arith)
        requires width > 0, (low - 1) * width < first, first <= offset * width,
            (offset + 1) * width <= end, (high + 1) * width > end;
    let parent = offset / 2;
    if offset % 2 == 0 {
        assert(parent * 2 == offset);
        assert(parent * (2 * width) == offset * width) by (nonlinear_arith) requires parent * 2 == offset;
        assert((parent + 1) * (2 * width) == (offset + 2) * width) by (nonlinear_arith) requires parent * 2 == offset;
        assert(offset + 1 == high) by (nonlinear_arith)
            requires width > 0, offset < high, high * width <= end, (offset + 2) * width > end;
    } else {
        assert(parent * 2 == offset - 1);
        assert(parent * (2 * width) == (offset - 1) * width) by (nonlinear_arith) requires parent * 2 == offset - 1;
        assert((parent + 1) * (2 * width) == (offset + 1) * width) by (nonlinear_arith) requires parent * 2 == offset - 1;
        assert(offset == low) by (nonlinear_arith)
            requires width > 0, low <= offset, low * width >= first, (offset - 1) * width < first;
    }
}

/// Use when a bottom-up interval cover terminates: every higher tile is at least twice as wide.
pub proof fn lemma_dyadic_width_grows(lower: int, upper: int)
    requires 0 <= lower < upper <= 27,
    ensures dyadic_width(upper) >= 2 * dyadic_width(lower),
    decreases upper - lower,
{
    lemma_dyadic_width_bounds(lower);
    lemma_dyadic_width_bounds(upper - 1);
    if upper > lower + 1 {
        lemma_dyadic_width_grows(lower, upper - 1);
    }
}

/// Use when replacing a sorted cell's payload while preserving its position between neighbors.
pub proof fn lemma_sorted_update_at<Value>(rows: Seq<Value>, position: int, value: Value, less: spec_fn(Value, Value) -> bool)
    requires
        0 <= position < rows.len(), verus_common::order::sorted_by(rows, less),
        forall|index: int| #![trigger rows[index]] 0 <= index < position ==> less(rows[index], value),
        forall|index: int| #![trigger rows[index]] position < index < rows.len() ==> less(value, rows[index]),
    ensures verus_common::order::sorted_by(rows.update(position, value), less),
{
    let after = rows.update(position, value);
    assert forall|left: int, right: int| #![trigger after[left], after[right]]
        0 <= left < right < after.len() implies less(after[left], after[right]) by {
        if left == position {} else if right == position {}
        else { assert(less(rows[left], rows[right])); }
    }
}

/// Use for bounded top-k retention: membership or domination by the retained cutoff.
pub open spec fn prefix_dominates<Value>(rows: Seq<Value>, candidate: Value, limit: int, precedes: spec_fn(Value, Value) -> bool) -> bool {
    rows.contains(candidate) || (rows.len() == limit && limit > 0 && precedes(rows[limit - 1], candidate))
}

/// Use after sorted insertion and truncation to preserve all previously dominated candidates.
pub proof fn lemma_top_prefix_insert<Value>(original: Seq<Value>, result: Seq<Value>, value: Value, position: int,
    limit: int, precedes: spec_fn(Value, Value) -> bool)
    requires
        0 < limit, original.len() <= limit, 0 <= position < limit, position <= original.len(),
        verus_common::order::sorted_by(original, precedes),
        forall|left: Value, middle: Value, right: Value| #![trigger precedes(left, middle), precedes(middle, right)]
            precedes(left, middle) && precedes(middle, right) ==> precedes(left, right),
        forall|source: int| #![trigger original[source]] position <= source < original.len() ==> precedes(value, original[source]),
        result == if original.len() < limit { original.insert(position, value) } else { original.insert(position, value).take(limit) },
    ensures
        prefix_dominates(result, value, limit, precedes),
        forall|candidate: Value| #![trigger prefix_dominates(result, candidate, limit, precedes)]
            prefix_dominates(original, candidate, limit, precedes) ==> prefix_dominates(result, candidate, limit, precedes),
{
    assert(result[position] == value);
    assert(result.contains(value));
    if original.len() == limit {
        if position == limit - 1 { assert(result[limit - 1] == value); }
        else { assert(result[limit - 1] == original[limit - 2]); }
        assert(precedes(result[limit - 1], original[limit - 1]));
    }
    assert forall|candidate: Value| #![trigger prefix_dominates(result, candidate, limit, precedes)]
        prefix_dominates(original, candidate, limit, precedes) implies prefix_dominates(result, candidate, limit, precedes) by {
        if original.contains(candidate) {
            let source = choose|source: int| #![trigger original[source]] 0 <= source < original.len() && original[source] == candidate;
            let offset = if source < position { source } else { source + 1 };
            if offset < result.len() { assert(result[offset] == candidate); }
            else { assert(original.len() == limit && source == limit - 1 && result.len() == limit); }
        } else {
            assert(original.len() == limit && result.len() == limit);
            assert(precedes(original[limit - 1], candidate));
            assert(precedes(result[limit - 1], original[limit - 1]));
        }
    }
}

/// Use to find a canonical-cover block among a point's ancestors in a bounded dyadic interval.
pub proof fn lemma_dyadic_ancestor(first: int, end: int, point: int, level: int, height: int)
    requires 0 <= level <= height <= 26, 0 <= first <= point < end <= dyadic_width(height),
        first <= (point / dyadic_width(level)) * dyadic_width(level),
        (point / dyadic_width(level) + 1) * dyadic_width(level) <= end,
    ensures exists|ancestor: int| #![trigger dyadic_width(ancestor)] level <= ancestor <= height && {
        let width = dyadic_width(ancestor);
        let offset = point / width;
        first <= offset * width && (offset + 1) * width <= end
            && (ancestor == height || !(first <= (offset / 2) * (2 * width)
                && (offset / 2 + 1) * (2 * width) <= end))
    },
    decreases height - level,
{
    lemma_dyadic_width_bounds(level);
    let width = dyadic_width(level);
    let offset = point / width;
    if level < height && first <= (offset / 2) * (2 * width)
        && (offset / 2 + 1) * (2 * width) <= end {
        vstd::arithmetic::div_mod::lemma_div_denominator(point, width, 2);
        assert(point / dyadic_width(level + 1) == offset / 2);
        lemma_dyadic_ancestor(first, end, point, level + 1, height);
    } else {
        assert(exists|ancestor: int| #![trigger dyadic_width(ancestor)] level <= ancestor <= height && {
            let ancestor_width = dyadic_width(ancestor);
            let ancestor_offset = point / ancestor_width;
            first <= ancestor_offset * ancestor_width && (ancestor_offset + 1) * ancestor_width <= end
                && (ancestor == height || !(first <= (ancestor_offset / 2) * (2 * ancestor_width)
                    && (ancestor_offset / 2 + 1) * (2 * ancestor_width) <= end))
        });
    }
}

/// Use when filtering an already ordered full answer; the relation need not be transitive.
pub proof fn lemma_filter_ordered<Value>(rows: Seq<Value>, keep: spec_fn(Value) -> bool, relation: spec_fn(Value, Value) -> bool)
    requires verus_common::order::sorted_by(rows, relation),
    ensures verus_common::order::sorted_by(rows.filter(keep), relation),
        forall|value: Value| #![trigger rows.filter(keep).contains(value)]
            rows.filter(keep).contains(value) == (rows.contains(value) && keep(value)),
    decreases rows.len(),
{
    reveal_with_fuel(Seq::filter, 2);
    if rows.len() > 0 {
        let prefix = rows.drop_last();
        let selected = prefix.filter(keep);
        lemma_filter_ordered(prefix, keep, relation);
        if keep(rows.last()) {
            assert(rows.filter(keep) =~= selected.push(rows.last()));
            lemma_push_membership(selected, rows.last());
            assert forall|left: int, right: int| #![trigger rows.filter(keep)[left], rows.filter(keep)[right]]
                0 <= left < right < rows.filter(keep).len() implies relation(rows.filter(keep)[left], rows.filter(keep)[right]) by {
                if right < selected.len() {
                    assert(relation(selected[left], selected[right]));
                } else {
                    assert(selected.contains(selected[left]));
                    assert(prefix.contains(selected[left]));
                    let source = choose|source: int| #![trigger prefix[source]] 0 <= source < prefix.len() && prefix[source] == selected[left];
                    assert(relation(rows[source], rows[rows.len() - 1]));
                }
            }
        } else { assert(rows.filter(keep) =~= selected); }
        assert(rows =~= prefix.push(rows.last()));
        lemma_push_membership(prefix, rows.last());
    }
}

}
