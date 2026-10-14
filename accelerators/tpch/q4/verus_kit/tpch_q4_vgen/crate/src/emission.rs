use vstd::prelude::*;
use verus_common::sums::*;
use verus_common::bytes::*;
use crate::rollup::*;

verus! {

pub proof fn lemma_bucket_absent(bucket: Seq<(Vec<u8>, i128)>, priority: Seq<u8>)
    requires forall|index: int| #![trigger bucket[index]] 0 <= index < bucket.len() ==> bucket[index].0@ != priority,
    ensures bucket_count(bucket, priority) == 0,
    decreases bucket.len(),
{
    if bucket.len() > 0 {
        lemma_bucket_absent(bucket.drop_last(), priority);
        assert(bucket.last().0@ != priority);
    }
}

pub proof fn lemma_bucket_entry(bucket: Seq<(Vec<u8>, i128)>, index: int)
    requires bucket_unique(bucket), 0 <= index < bucket.len(),
    ensures bucket_count(bucket, bucket[index].0@) == bucket[index].1 as int,
    decreases bucket.len(),
{
    let priority = bucket[index].0@;
    let prefix = bucket.drop_last();
    assert(bucket_count(bucket, priority) == bucket_count(prefix, priority)
        + if bucket.last().0@ == priority { bucket.last().1 as int } else { 0 });
    if index == bucket.len() - 1 {
        assert forall|other: int| #![trigger prefix[other]] 0 <= other < prefix.len() implies prefix[other].0@ != priority by {
            assert(bucket[other].0@ != bucket[index].0@);
        }
        lemma_bucket_absent(prefix, priority);
    } else {
        lemma_bucket_entry(prefix, index);
        assert(bucket[index].0@ != bucket[bucket.len() as int - 1].0@);
    }
}

pub proof fn lemma_bucket_positive(bucket: Seq<(Vec<u8>, i128)>, priority: Seq<u8>) -> (index: int)
    requires bucket_unique(bucket), bucket_count(bucket, priority) > 0,
    ensures 0 <= index < bucket.len(), bucket[index].0@ == priority, bucket[index].1 > 0,
    decreases bucket.len(),
{
    assert(bucket.len() > 0);
    if bucket.last().0@ == priority {
        lemma_bucket_entry(bucket, bucket.len() as int - 1);
        bucket.len() as int - 1
    } else {
        lemma_bucket_positive(bucket.drop_last(), priority)
    }
}

pub open spec fn output_rows_ok(out: Seq<(Vec<u8>, i64)>, bucket: Seq<(Vec<u8>, i128)>, processed: int) -> bool {
    forall|index: int| #![trigger out[index]] 0 <= index < out.len() ==>
        out[index].1 > 0 && out[index].1 as int == bucket_count(bucket, out[index].0@)
            && exists|source: int| #![trigger bucket[source]] 0 <= source < processed
                && bucket[source].0@ == out[index].0@ && bucket[source].1 as int == out[index].1 as int
}

pub open spec fn output_covered(out: Seq<(Vec<u8>, i64)>, bucket: Seq<(Vec<u8>, i128)>, processed: int) -> bool {
    forall|source: int| #![trigger bucket[source]] 0 <= source < processed && bucket[source].1 > 0 ==>
        exists|index: int| #![trigger out[index]] 0 <= index < out.len() && out[index].0@ == bucket[source].0@
}

pub open spec fn output_less(left: (Vec<u8>, i64), right: (Vec<u8>, i64)) -> bool { bytes_lt(left.0@, right.0@) }

pub open spec fn output_prefix_ok(out: Seq<(Vec<u8>, i64)>, bucket: Seq<(Vec<u8>, i128)>, processed: int) -> bool {
    &&& output_rows_ok(out, bucket, processed)
    &&& output_covered(out, bucket, processed)
    &&& verus_common::order::sorted_by(out, |left: (Vec<u8>, i64), right: (Vec<u8>, i64)| output_less(left, right))
}

#[verifier::rlimit(30)]
pub fn insertion_position(out: &Vec<(Vec<u8>, i64)>, bucket: &Bucket, index: usize) -> (position: usize)
    requires index < bucket.len(), bucket_unique(bucket@), output_prefix_ok(out@, bucket@, index as int),
    ensures position <= out.len(),
        forall|before: int| #![trigger out@[before]] 0 <= before < position ==> bytes_lt(out@[before].0@, bucket@[index as int].0@),
        forall|after: int| #![trigger out@[after]] position <= after < out@.len() ==> bytes_lt(bucket@[index as int].0@, out@[after].0@),
{
    hide(bucket_count);
    hide(sum);
    hide(bytes_lt);
    let mut position = 0;
    while position < out.len()
        invariant position <= out.len(), index < bucket.len(), bucket_unique(bucket@), output_prefix_ok(out@, bucket@, index as int),
            forall|before: int| #![trigger out@[before]] 0 <= before < position ==> bytes_lt(out@[before].0@, bucket@[index as int].0@),
        decreases out.len() - position,
    {
        if bytes_lt_exec(&bucket[index].0, &out[position].0) {
            proof {
                assert forall|after: int| #![trigger out@[after]] position <= after < out@.len()
                    implies bytes_lt(bucket@[index as int].0@, out@[after].0@) by {
                    if after > position { lemma_bytes_lt_trans(bucket@[index as int].0@, out@[position as int].0@, out@[after].0@); }
                }
            }
            return position;
        }
        proof {
            let source = choose|source: int| #![trigger bucket@[source]] 0 <= source < index
                && bucket@[source].0@ == out@[position as int].0@ && bucket@[source].1 as int == out@[position as int].1 as int;
            assert(bucket@[source].0@ != bucket@[index as int].0@);
            lemma_bytes_lt_total(bucket@[index as int].0@, out@[position as int].0@);
        }
        position += 1;
    }
    position
}

pub proof fn lemma_output_empty(bucket: Seq<(Vec<u8>, i128)>)
    ensures output_prefix_ok(Seq::empty(), bucket, 0),
{
}

pub proof fn lemma_output_skip(out: Seq<(Vec<u8>, i64)>, bucket: Seq<(Vec<u8>, i128)>, index: int)
    requires 0 <= index < bucket.len(), output_prefix_ok(out, bucket, index), bucket[index].1 <= 0,
    ensures output_prefix_ok(out, bucket, index + 1),
{
    hide(bucket_count);
    hide(sum);
    hide(bytes_lt);
}

pub proof fn lemma_insert_rows(previous: Seq<(Vec<u8>, i64)>, bucket: Seq<(Vec<u8>, i128)>, index: int, position: int, cell: (Vec<u8>, i64))
    requires 0 <= index < bucket.len(), bucket_unique(bucket), output_rows_ok(previous, bucket, index),
        0 <= position <= previous.len(), cell.0@ == bucket[index].0@, cell.1 as int == bucket[index].1 as int, cell.1 > 0,
    ensures output_rows_ok(previous.insert(position, cell), bucket, index + 1),
{
    hide(bucket_count);
    hide(sum);
    hide(bytes_lt);
    let out = previous.insert(position, cell);
    lemma_bucket_entry(bucket, index);
    assert forall|target: int| #![trigger out[target]] 0 <= target < out.len() implies
        out[target].1 > 0 && out[target].1 as int == bucket_count(bucket, out[target].0@)
            && exists|source: int| #![trigger bucket[source]] 0 <= source < index + 1
                && bucket[source].0@ == out[target].0@ && bucket[source].1 as int == out[target].1 as int by {
        if target == position { assert(out[target].0@ == bucket[index].0@); }
        else {
            let old_target = if target < position { target } else { target - 1 };
            assert(out[target] == previous[old_target]);
        }
    }
}

pub proof fn lemma_insert_coverage(previous: Seq<(Vec<u8>, i64)>, bucket: Seq<(Vec<u8>, i128)>, index: int, position: int, cell: (Vec<u8>, i64))
    requires 0 <= index < bucket.len(), output_covered(previous, bucket, index),
        0 <= position <= previous.len(), cell.0@ == bucket[index].0@,
    ensures output_covered(previous.insert(position, cell), bucket, index + 1),
{
    let out = previous.insert(position, cell);
    assert forall|source: int| #![trigger bucket[source]] 0 <= source < index + 1 && bucket[source].1 > 0 implies
        exists|target: int| #![trigger out[target]] 0 <= target < out.len() && out[target].0@ == bucket[source].0@ by {
        if source == index { assert(out[position].0@ == bucket[source].0@); }
        else {
            let old_target = choose|target: int| #![trigger previous[target]] 0 <= target < previous.len() && previous[target].0@ == bucket[source].0@;
            let target = if old_target < position { old_target } else { old_target + 1 };
            assert(out[target] == previous[old_target]);
        }
    }
}

pub proof fn lemma_output_insert(previous: Seq<(Vec<u8>, i64)>, bucket: Seq<(Vec<u8>, i128)>, index: int, position: int, cell: (Vec<u8>, i64))
    requires 0 <= index < bucket.len(), bucket_unique(bucket), output_prefix_ok(previous, bucket, index),
        0 <= position <= previous.len(), cell.0@ == bucket[index].0@, cell.1 as int == bucket[index].1 as int, cell.1 > 0,
        forall|before: int| #![trigger previous[before]] 0 <= before < position ==> bytes_lt(previous[before].0@, cell.0@),
        forall|after: int| #![trigger previous[after]] position <= after < previous.len() ==> bytes_lt(cell.0@, previous[after].0@),
    ensures output_prefix_ok(previous.insert(position, cell), bucket, index + 1),
{
    hide(bucket_count);
    hide(sum);
    hide(bytes_lt);
    hide(output_rows_ok);
    hide(output_covered);
    lemma_insert_rows(previous, bucket, index, position, cell);
    lemma_insert_coverage(previous, bucket, index, position, cell);
    verus_prooflib::lemma_sorted_insert_at(previous, position, cell,
        |left: (Vec<u8>, i64), right: (Vec<u8>, i64)| output_less(left, right));
}

pub proof fn lemma_output_finish(out: Seq<(Vec<u8>, i64)>, bucket: Seq<(Vec<u8>, i128)>)
    requires bucket_unique(bucket), output_prefix_ok(out, bucket, bucket.len() as int),
    ensures
        forall|index: int| #![trigger out[index]] 0 <= index < out.len() ==>
            out[index].1 > 0 && out[index].1 as int == bucket_count(bucket, out[index].0@),
        forall|priority: Seq<u8>| #[trigger] bucket_count(bucket, priority) > 0 ==>
            exists|index: int| #![trigger out[index]] 0 <= index < out.len() && out[index].0@ == priority,
        forall|left: int, right: int| #![trigger out[left], out[right]] 0 <= left < right < out.len() ==> bytes_lt(out[left].0@, out[right].0@),
{
    hide(bucket_count);
    hide(sum);
    hide(bytes_lt);
    assert forall|priority: Seq<u8>| #[trigger] bucket_count(bucket, priority) > 0 implies
        exists|index: int| #![trigger out[index]] 0 <= index < out.len() && out[index].0@ == priority by {
        let source = lemma_bucket_positive(bucket, priority);
    }
}

pub fn sorted_output(bucket: Bucket) -> (result: Option<Vec<(Vec<u8>, i64)>>)
    requires bucket_unique(bucket@),
    ensures result matches Some(out) ==>
        (forall|index: int| #![trigger out@[index]] 0 <= index < out@.len() ==>
            out@[index].1 > 0 && out@[index].1 as int == bucket_count(bucket@, out@[index].0@))
        && (forall|priority: Seq<u8>| #[trigger] bucket_count(bucket@, priority) > 0 ==>
            exists|index: int| #![trigger out@[index]] 0 <= index < out@.len() && out@[index].0@ == priority)
        && (forall|left: int, right: int| #![trigger out@[left], out@[right]] 0 <= left < right < out@.len() ==>
            verus_common::bytes::bytes_lt(out@[left].0@, out@[right].0@)),
{
    hide(bucket_count);
    hide(sum);
    hide(bytes_lt);
    hide(output_prefix_ok);
    let mut out: Vec<(Vec<u8>, i64)> = Vec::new();
    proof { lemma_output_empty(bucket@); }
    let mut index = 0;
    while index < bucket.len()
        invariant index <= bucket.len(), bucket_unique(bucket@), output_prefix_ok(out@, bucket@, index as int),
        decreases bucket.len() - index,
    {
        let count = bucket[index].1;
        if count < 0 || count > i64::MAX as i128 { return None; }
        if count > 0 {
            let previous: Ghost<Vec<(Vec<u8>, i64)>> = Ghost(out);
            let position = insertion_position(&out, &bucket, index);
            out.insert(position, (copy_bytes(&bucket[index].0), count as i64));
            proof {
                lemma_output_insert(previous@@, bucket@, index as int, position as int, out@[position as int]);
            }
        } else {
            proof { lemma_output_skip(out@, bucket@, index as int); }
        }
        index += 1;
    }
    proof { lemma_output_finish(out@, bucket@); }
    Some(out)
}

}
