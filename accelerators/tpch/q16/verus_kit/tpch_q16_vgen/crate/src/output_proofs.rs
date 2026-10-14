use vstd::prelude::*;
use verus_common::sums::*;
use verus_prooflib::ordered::{Index, KeyView};
use crate::output::*;

verus! {

pub proof fn lemma_small_stream_update(before: Seq<Vec<Stream>>, after: Seq<Vec<Stream>>, position: int)
    requires 0 <= position < before.len(), after == before.update(position, after[position]),
    ensures sum(after, |slot: Vec<Stream>| stream_work(slot))
        == sum(before, |slot: Vec<Stream>| stream_work(slot)) - stream_work(before[position]) + stream_work(after[position]),
{
    lemma_sum_update(before, position, after[position], |slot: Vec<Stream>| stream_work(slot));
}

}
