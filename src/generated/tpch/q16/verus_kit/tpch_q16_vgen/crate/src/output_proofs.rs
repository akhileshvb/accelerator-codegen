use vstd::prelude::*;
use verus_common::sums::*;
use verus_prooflib::ordered::{Index, KeyView};
use crate::output::*;

verus! {

pub proof fn lemma_frontier_remove(before: &Index<usize>, after: &Index<usize>, key: KeyView)
    requires before.wf(), after.wf(), before.at(key).is_some(), after.view() == before.view().remove(key),
    ensures after.entries().len() as int == before.entries().len() as int - 1,
{
    let weight = |row: (KeyView, usize)| 1int;
    verus_prooflib::ordered::lemma_sum_remove(before, after, key, weight);
    verus_prooflib::lemma_sum_ones(before.entries());
    verus_prooflib::lemma_sum_ones(after.entries());
}

pub proof fn lemma_frontier_put(before: &Index<usize>, after: &Index<usize>, key: KeyView, value: usize)
    requires before.wf(), after.wf(), after.view() == before.view().insert(key, value),
    ensures after.entries().len() <= before.entries().len() + 1,
{
    let weight = |row: (KeyView, usize)| 1int;
    verus_prooflib::ordered::lemma_sum_put(before, after, key, value, weight);
    verus_prooflib::lemma_sum_ones(before.entries());
    verus_prooflib::lemma_sum_ones(after.entries());
}

pub open spec fn remaining_work(slot: Vec<Stream>) -> int {
    if slot@.len() == 1 { slot@[0].cursor.remaining().len() as int } else { 0int }
}

pub proof fn lemma_small_stream_update(before: Seq<Vec<Stream>>, after: Seq<Vec<Stream>>, position: int)
    requires 0 <= position < before.len(), after == before.update(position, after[position]),
    ensures sum(after, |slot: Vec<Stream>| stream_work(slot))
        == sum(before, |slot: Vec<Stream>| stream_work(slot)) - stream_work(before[position]) + stream_work(after[position]),
{
    lemma_sum_update(before, position, after[position], |slot: Vec<Stream>| stream_work(slot));
}

pub proof fn lemma_large_stream_update(before: Seq<Vec<Stream>>, after: Seq<Vec<Stream>>, position: int)
    requires 0 <= position < before.len(), after == before.update(position, after[position]),
    ensures sum(after, |slot: Vec<Stream>| remaining_work(slot))
        == sum(before, |slot: Vec<Stream>| remaining_work(slot)) - remaining_work(before[position]) + remaining_work(after[position]),
{
    lemma_sum_update(before, position, after[position], |slot: Vec<Stream>| remaining_work(slot));
}

}
