use vstd::prelude::*;
use verus_common::rel::*;
use verus_common::sums::*;

verus! {

pub proof fn lemma_flat_take_step<Value>(parts: Seq<Seq<Value>>, index: int)
    requires 0 <= index < parts.len(),
    ensures flat(parts.take(index + 1)) == flat(parts.take(index)) + parts[index],
{
    assert(parts.take(index + 1).drop_last() =~= parts.take(index));
    assert(parts.take(index + 1).last() == parts[index]);
}

pub proof fn lemma_flat_empty<Value>(parts: Seq<Seq<Value>>)
    requires forall|index: int| 0 <= index < parts.len() ==> (#[trigger] parts[index]).len() == 0,
    ensures flat(parts).len() == 0,
    decreases parts.len(),
{
    if parts.len() > 0 { lemma_flat_empty(parts.drop_last()); }
}

pub proof fn lemma_flat_length<Value>(parts: Seq<Seq<Value>>)
    ensures flat(parts).len() == sum(parts, |part: Seq<Value>| part.len() as int),
    decreases parts.len(),
{
    if parts.len() > 0 { lemma_flat_length(parts.drop_last()); }
}

pub proof fn lemma_flat_index<Value>(parts: Seq<Seq<Value>>, index: int) -> (result: (int, int))
    requires 0 <= index < flat(parts).len(),
    ensures 0 <= result.0 < parts.len(), 0 <= result.1 < parts[result.0].len(),
        flat(parts)[index] == parts[result.0][result.1],
        index == sum(parts.take(result.0), |part: Seq<Value>| part.len() as int) + result.1,
    decreases parts.len(),
{
    let prefix = parts.drop_last();
    let length = flat(prefix).len();
    if index < length {
        let result = lemma_flat_index(prefix, index);
        assert(prefix.take(result.0) =~= parts.take(result.0));
        result
    } else {
        lemma_flat_length(prefix);
        assert(parts.take(parts.len() as int - 1) =~= prefix);
        (parts.len() as int - 1, index - length)
    }
}

pub proof fn lemma_push_contains<Value>(rows: Seq<Value>, value: Value)
    ensures forall|other: Value| #[trigger] rows.push(value).contains(other) <==> rows.contains(other) || other == value,
{
    assert forall|other: Value| #[trigger] rows.push(value).contains(other) <==> rows.contains(other) || other == value by {
        if rows.push(value).contains(other) {
            let index = choose|index: int| 0 <= index < rows.push(value).len() && #[trigger] rows.push(value)[index] == other;
            if index < rows.len() { assert(rows[index] == other); }
        }
        if rows.contains(other) {
            let index = choose|index: int| 0 <= index < rows.len() && #[trigger] rows[index] == other;
            assert(rows.push(value)[index] == other);
        }
        if other == value { assert(rows.push(value)[rows.len() as int] == other); }
    }
}

pub proof fn lemma_take_extend_contains<Value>(rows: Seq<Value>, index: int, value: Value)
    requires 0 <= index < rows.len(),
    ensures rows.take(index + 1).contains(value) <==> rows.take(index).contains(value) || value == rows[index],
{
    assert(rows.take(index + 1) =~= rows.take(index).push(rows[index]));
    lemma_push_contains(rows.take(index), rows[index]);
}

}
