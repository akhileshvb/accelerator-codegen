use vstd::prelude::*;
use verus_common::sqlfns::*;
use crate::model::*;

verus! {

pub proof fn lemma_escape_head(bytes: Seq<u8>)
    ensures escape(bytes).len() > 0,
        bytes.len() == 0 ==> escape(bytes)[0] == 0u8,
        bytes.len() > 0 ==> escape(bytes).len() >= 3 && escape(bytes)[0] == 1u8 && escape(bytes)[1] == bytes[0],
{
    broadcast use vstd::seq_lib::group_seq_properties;
    reveal_with_fuel(escape, 2);
    if bytes.len() > 0 { lemma_escape_end(bytes.skip(1)); }
}

pub proof fn lemma_kind_identity(left: Seq<u8>, right: Seq<u8>)
    requires escape(left) == escape(right),
    ensures left == right,
    decreases left.len() + right.len(),
{
    broadcast use vstd::seq_lib::group_seq_properties;
    reveal_with_fuel(escape, 2);
    lemma_escape_head(left);
    lemma_escape_head(right);
    if left.len() == 0 || right.len() == 0 {
        assert(left.len() == 0 && right.len() == 0);
        assert(left =~= right);
    } else {
        assert(left[0] == right[0]);
        assert(escape(left).skip(2) =~= escape(left.skip(1)));
        assert(escape(right).skip(2) =~= escape(right.skip(1)));
        lemma_kind_identity(left.skip(1), right.skip(1));
        assert(left =~= seq![left[0]] + left.skip(1));
        assert(right =~= seq![right[0]] + right.skip(1));
    }
}

pub proof fn lemma_group_text_identity(left_brand: Seq<u8>, left_kind: Seq<u8>, right_brand: Seq<u8>, right_kind: Seq<u8>)
    requires escape(left_brand) + escape(left_kind) == escape(right_brand) + escape(right_kind),
    ensures left_brand == right_brand, left_kind == right_kind,
    decreases left_brand.len() + right_brand.len(),
{
    broadcast use vstd::seq_lib::group_seq_properties;
    reveal_with_fuel(escape, 2);
    lemma_escape_head(left_brand);
    lemma_escape_head(right_brand);
    let left_text = escape(left_brand) + escape(left_kind);
    let right_text = escape(right_brand) + escape(right_kind);
    assert(left_text[0] == escape(left_brand)[0]);
    assert(right_text[0] == escape(right_brand)[0]);
    if left_brand.len() == 0 || right_brand.len() == 0 {
        assert(left_brand.len() == 0 && right_brand.len() == 0);
        assert(left_brand =~= right_brand);
        assert((escape(left_brand) + escape(left_kind)).skip(1) =~= escape(left_kind));
        assert((escape(right_brand) + escape(right_kind)).skip(1) =~= escape(right_kind));
        lemma_kind_identity(left_kind, right_kind);
    } else {
        assert(left_text[1] == escape(left_brand)[1]);
        assert(right_text[1] == escape(right_brand)[1]);
        assert(left_brand[0] == right_brand[0]);
        assert((escape(left_brand) + escape(left_kind)).skip(2)
            =~= escape(left_brand.skip(1)) + escape(left_kind));
        assert((escape(right_brand) + escape(right_kind)).skip(2)
            =~= escape(right_brand.skip(1)) + escape(right_kind));
        lemma_group_text_identity(left_brand.skip(1), left_kind, right_brand.skip(1), right_kind);
        assert(left_brand =~= seq![left_brand[0]] + left_brand.skip(1));
        assert(right_brand =~= seq![right_brand[0]] + right_brand.skip(1));
    }
}

pub proof fn lemma_group_identity(left: &Group, right: &Group)
    requires left.key() == right.key(),
    ensures (*left.brand)@ == (*right.brand)@, (*left.kind)@ == (*right.kind)@,
        left.size == right.size,
{
    lemma_group_text_identity((*left.brand)@, (*left.kind)@, (*right.brand)@, (*right.kind)@);
}

pub proof fn lemma_group_order_identity(left: &Group, right: &Group)
    requires left.valid(), right.valid(), left.size == right.size,
        left.order.view().1 == right.order.view().1,
    ensures left.key() == right.key(),
{
    broadcast use vstd::seq_lib::group_seq_properties;
    let left_text = escape((*left.brand)@) + escape((*left.kind)@);
    let right_text = escape((*right.brand)@) + escape((*right.kind)@);
    assert(left.order.view().1 == left_text + size_bytes(left.size as int));
    assert(right.order.view().1 == right_text + size_bytes(right.size as int));
    assert(left.order.view().1.len() == left_text.len() + size_bytes(left.size as int).len());
    assert(right.order.view().1.len() == right_text.len() + size_bytes(right.size as int).len());
    assert(left_text.len() == right_text.len());
    assert((left_text + size_bytes(left.size as int)).take(left_text.len() as int) =~= left_text);
    assert((right_text + size_bytes(right.size as int)).take(right_text.len() as int) =~= right_text);
}

pub proof fn lemma_escape_push(bytes: Seq<u8>, byte: u8)
    ensures escape(bytes.push(byte)) == escape(bytes).drop_last() + seq![1u8, byte, 0u8],
    decreases bytes.len(),
{
    broadcast use vstd::seq_lib::group_seq_properties;
    reveal_with_fuel(escape, 2);
    if bytes.len() > 0 {
        lemma_escape_end(bytes.skip(1));
        lemma_escape_push(bytes.skip(1), byte);
        assert(bytes.push(byte).skip(1) =~= bytes.skip(1).push(byte));
        assert(escape(bytes).len() > 0);
        assert((seq![1u8, bytes[0]] + escape(bytes.skip(1))).drop_last()
            =~= seq![1u8, bytes[0]] + escape(bytes.skip(1)).drop_last());
        assert((seq![1u8, bytes[0]] + escape(bytes.skip(1)).drop_last()) + seq![1u8, byte, 0u8]
            =~= seq![1u8, bytes[0]] + (escape(bytes.skip(1)).drop_last() + seq![1u8, byte, 0u8]));
    }
}

pub proof fn lemma_escape_end(bytes: Seq<u8>)
    ensures escape(bytes).len() > 0, escape(bytes).last() == 0u8,
        escape(bytes) == escape(bytes).drop_last().push(0u8),
    decreases bytes.len(),
{
    if bytes.len() > 0 {
        lemma_escape_end(bytes.skip(1));
    }
    assert(escape(bytes) =~= escape(bytes).drop_last().push(0u8));
}

pub proof fn lemma_brushed_like(kind: Seq<u8>)
    ensures like_match(kind, seq![b'S', b'M', b'A', b'L', b'L', b' ', b'B', b'R', b'U', b'S', b'H', b'E', b'D', b'%'])
        == (kind.len() >= 13 && kind.take(13) == seq![b'S', b'M', b'A', b'L', b'L', b' ', b'B', b'R', b'U', b'S', b'H', b'E', b'D']),
{
    lemma_brushed_tail(kind, 0);
    assert(brushed_prefix().skip(0) =~= brushed_prefix());
    assert(brushed_prefix().skip(0) + seq![b'%']
        =~= seq![b'S', b'M', b'A', b'L', b'L', b' ', b'B', b'R', b'U', b'S', b'H', b'E', b'D', b'%']);
}

pub open spec fn brushed_prefix() -> Seq<u8> {
    seq![b'S', b'M', b'A', b'L', b'L', b' ', b'B', b'R', b'U', b'S', b'H', b'E', b'D']
}

pub proof fn lemma_brushed_tail(kind: Seq<u8>, offset: int)
    requires 0 <= offset <= 13,
    ensures like_match(kind, brushed_prefix().skip(offset) + seq![b'%'])
        == (kind.len() >= 13 - offset && kind.take(13 - offset) == brushed_prefix().skip(offset)),
    decreases 13 - offset,
{
    let pattern = brushed_prefix().skip(offset) + seq![b'%'];
    reveal_with_fuel(like_match, 2);
    if offset == 13 {
        assert(pattern =~= seq![b'%']);
        assert(kind.take(0) =~= Seq::<u8>::empty());
        assert(brushed_prefix().skip(13) =~= Seq::<u8>::empty());
        lemma_percent(kind);
    } else {
        assert(pattern[0] != b'%' && pattern[0] != b'_');
        assert(pattern[0] == brushed_prefix().skip(offset)[0]);
        assert(pattern.drop_first() =~= brushed_prefix().skip(offset + 1) + seq![b'%']);
        assert(like_match(kind, pattern) == (kind.len() > 0 && kind[0] == pattern[0]
            && like_match(kind.drop_first(), pattern.drop_first())));
        if kind.len() > 0 {
            lemma_brushed_tail(kind.drop_first(), offset + 1);
            if kind.len() >= 13 - offset {
                assert(kind.take(13 - offset).drop_first() =~= kind.drop_first().take(12 - offset));
                assert(brushed_prefix().skip(offset).drop_first() =~= brushed_prefix().skip(offset + 1));
                if kind.take(13 - offset) == brushed_prefix().skip(offset) {
                    assert(kind[0] == pattern[0]);
                    assert(kind.drop_first().take(12 - offset) == brushed_prefix().skip(offset + 1));
                }
                if kind[0] == pattern[0] && kind.drop_first().take(12 - offset) == brushed_prefix().skip(offset + 1) {
                    assert forall|position: int| 0 <= position < 13 - offset implies
                        (#[trigger] kind.take(13 - offset)[position]) == brushed_prefix().skip(offset)[position] by {
                        if position > 0 {
                            assert(kind.drop_first().take(12 - offset)[position - 1]
                                == brushed_prefix().skip(offset + 1)[position - 1]);
                        }
                    }
                    assert(kind.take(13 - offset) =~= brushed_prefix().skip(offset));
                }
            }
            if kind.len() < 13 - offset {
                assert(!like_match(kind.drop_first(), brushed_prefix().skip(offset + 1) + seq![b'%']));
            }
        }
    }
}

pub proof fn lemma_percent(kind: Seq<u8>)
    ensures like_match(kind, seq![b'%']),
    decreases kind.len(),
{
    reveal_with_fuel(like_match, 2);
    assert(seq![b'%'].drop_first() =~= Seq::<u8>::empty());
    if kind.len() > 0 { lemma_percent(kind.drop_first()); }
}

}
