use vstd::prelude::*;
use verus_common::order::*;
use verus_common::sums::*;

verus! {

pub proof fn lemma_sum_insert<Element>(rows: Seq<Element>, position: int, item: Element, weight: spec_fn(Element) -> int)
    requires 0 <= position <= rows.len(),
    ensures sum(rows.insert(position, item), weight) == sum(rows, weight) + weight(item),
{
    let inserted = rows.insert(position, item);
    lemma_sum_remove(inserted, position, weight);
    assert(inserted.remove(position) =~= rows);
}

pub proof fn lemma_insert_by_position<Element>(rows: Seq<Element>, item: Element, less: spec_fn(Element, Element) -> bool, position: int)
    requires
        0 <= position <= rows.len(),
        forall|index: int| 0 <= index < position ==> !less(item, #[trigger] rows[index]),
        position < rows.len() ==> less(item, rows[position]),
    ensures insert_by(rows, item, less) == rows.insert(position, item),
    decreases position,
{
    if position == 0 {
        assert(rows.insert(0, item) =~= seq![item] + rows);
    } else {
        let tail = rows.drop_first();
        assert forall|index: int| 0 <= index < position - 1 implies !less(item, #[trigger] tail[index]) by {
            assert(tail[index] == rows[index + 1]);
        }
        lemma_insert_by_position(tail, item, less, position - 1);
        assert(rows.insert(position, item) =~= seq![rows[0]] + tail.insert(position - 1, item));
    }
}

pub proof fn lemma_insert_by_length<Element>(rows: Seq<Element>, item: Element, less: spec_fn(Element, Element) -> bool)
    ensures insert_by(rows, item, less).len() == rows.len() + 1,
    decreases rows.len(),
{
    if rows.len() > 0 && !less(item, rows[0]) {
        lemma_insert_by_length(rows.drop_first(), item, less);
    }
}

pub proof fn lemma_insert_by_prefix<Element>(rows: Seq<Element>, item: Element, less: spec_fn(Element, Element) -> bool, bound: int)
    requires 0 <= bound <= rows.len(),
    ensures insert_by(rows.take(bound), item, less).take(bound) == insert_by(rows, item, less).take(bound),
    decreases bound,
{
    if bound == 0 {
        assert(insert_by(rows.take(bound), item, less).take(bound) =~= Seq::<Element>::empty());
        assert(insert_by(rows, item, less).take(bound) =~= Seq::<Element>::empty());
    } else if less(item, rows[0]) {
        assert(rows.take(bound)[0] == rows[0]);
        assert((seq![item] + rows.take(bound)).take(bound) =~= (seq![item] + rows).take(bound));
    } else {
        let tail = rows.drop_first();
        assert(rows.take(bound)[0] == rows[0]);
        assert(rows.take(bound).drop_first() =~= tail.take(bound - 1));
        lemma_insert_by_prefix(tail, item, less, bound - 1);
        lemma_insert_by_length(tail, item, less);
        lemma_insert_by_length(tail.take(bound - 1), item, less);
        assert(insert_by(rows.take(bound), item, less).take(bound) =~=
            seq![rows[0]] + insert_by(tail.take(bound - 1), item, less).take(bound - 1));
        assert(insert_by(rows, item, less).take(bound) =~=
            seq![rows[0]] + insert_by(tail, item, less).take(bound - 1));
    }
}

}
