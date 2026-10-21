// Counted prefixes: how a table that counts the rows seen so far grows by one row.
use vstd::prelude::*;
use verus_common::sums::*;

verus! {

/// Counting one more row of a view: the prefix sum grows by that row's weight.
pub proof fn lemma_mapped_prefix_step<R, V>(rows: Seq<R>, view: spec_fn(R) -> V, index: int, weight: spec_fn(V) -> int)
    requires 0 <= index < rows.len(),
    ensures
        sum(rows.take(index + 1).map_values(view), weight)
            == sum(rows.take(index).map_values(view), weight) + weight(view(rows[index])),
{
    let views = rows.map_values(view);
    assert(rows.take(index).map_values(view) =~= views.take(index));
    assert(rows.take(index + 1).map_values(view) =~= views.take(index + 1));
    lemma_sum_take(views, index, weight);
}

/// Counting one more row after the dictionary grew: the rows before it keep their weights (their
/// texts were known), so the prefix sum grows by the new row's weight alone.
pub proof fn lemma_grown_prefix_step<A>(prefix: Seq<A>, next: A, before_weight: spec_fn(A) -> int, weight: spec_fn(A) -> int)
    requires forall|i: int| 0 <= i < prefix.len() ==> before_weight(#[trigger] prefix[i]) == weight(prefix[i]),
    ensures sum(prefix.push(next), weight) == sum(prefix, before_weight) + weight(next),
{
    lemma_sum_ext(prefix, before_weight, weight);
    lemma_sum_push(prefix, next, weight);
}

}
