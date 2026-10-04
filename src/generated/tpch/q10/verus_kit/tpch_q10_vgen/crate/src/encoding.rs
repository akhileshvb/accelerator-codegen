    use vstd::prelude::*;
    use vstd::arithmetic::div_mod::lemma_fundamental_div_mod;
    verus! {

    pub open spec fn radix_power(width: nat) -> nat
        decreases width,
    {
        if width == 0 { 1nat } else { 256 * radix_power((width - 1) as nat) }
    }

    pub open spec fn number(value: nat, width: nat) -> Seq<u8>
        decreases width,
    {
        if width == 0 { Seq::empty() }
        else { number(value / 256, (width - 1) as nat).push((value % 256) as u8) }
    }

    pub open spec fn decode(bytes: Seq<u8>) -> nat
        decreases bytes.len(),
    {
        if bytes.len() == 0 { 0nat }
        else { decode(bytes.drop_last()) * 256 + bytes.last() as nat }
    }

    pub open spec fn payload(text: Seq<u8>) -> Seq<u8>
        decreases text.len(),
    {
        if text.len() == 0 { Seq::empty() }
        else { payload(text.drop_last()).push(1u8).push(text.last()) }
    }

    pub open spec fn text(text: Seq<u8>) -> Seq<u8> { payload(text).push(0u8) }

    /// Use for fixed-width composite keys and overflow-free decoder arithmetic.
    pub proof fn lemma_number_shape(value: nat, width: nat)
        ensures number(value, width).len() == width, radix_power(width) > 0,
        decreases width,
    {
        if width > 0 { lemma_number_shape(value / 256, (width - 1) as nat); }
    }

    /// Use to recover the bounded integer represented by a big-endian key prefix.
    pub proof fn lemma_decode_number(value: nat, width: nat)
        requires value < radix_power(width),
        ensures decode(number(value, width)) == value,
        decreases width,
    {
        lemma_number_shape(value, width);
        if width > 0 {
            lemma_number_shape(value / 256, (width - 1) as nat);
            lemma_fundamental_div_mod(value as int, 256);
            assert(value / 256 < radix_power((width - 1) as nat)) by (nonlinear_arith)
                requires value < 256 * radix_power((width - 1) as nat),
                    value == (value / 256) * 256 + value % 256, 0 <= value % 256 < 256;
            lemma_decode_number(value / 256, (width - 1) as nat);
            assert(number(value, width).drop_last() =~= number(value / 256, (width - 1) as nat));
            assert((value % 256) as u8 as nat == value % 256);
        }
    }

    /// Use to split a fixed-width integer component from arbitrary following tuple components.
    pub proof fn lemma_number_frame(left: nat, right: nat, width: nat, left_tail: Seq<u8>, right_tail: Seq<u8>)
        requires left < radix_power(width), right < radix_power(width),
            number(left, width) + left_tail == number(right, width) + right_tail,
        ensures left == right, left_tail == right_tail,
    {
        lemma_number_shape(left, width);
        lemma_number_shape(right, width);
        assert((number(left, width) + left_tail).take(width as int) =~= number(left, width));
        assert((number(right, width) + right_tail).take(width as int) =~= number(right, width));
        lemma_decode_number(left, width);
        lemma_decode_number(right, width);
        assert((number(left, width) + left_tail).skip(width as int) =~= left_tail);
        assert((number(right, width) + right_tail).skip(width as int) =~= right_tail);
    }

    /// Use to move a text payload's processed-prefix boundary without changing its encoding.
    pub proof fn lemma_payload_concat(left: Seq<u8>, right: Seq<u8>)
        ensures payload(left + right) == payload(left) + payload(right),
        decreases right.len(),
    {
        if right.len() == 0 {
            assert(left + right =~= left);
            assert(payload(left) + payload(right) =~= payload(left));
        } else {
            assert((left + right).drop_last() =~= left + right.drop_last());
            assert((left + right).last() == right.last());
            lemma_payload_concat(left, right.drop_last());
            assert(payload(left + right) =~= payload(left) + payload(right));
        }
    }

    /// Use for decoding the first component of a terminator-framed byte tuple.
    pub proof fn lemma_text_cons(value: Seq<u8>)
        requires value.len() > 0,
        ensures text(value) == seq![1u8, value[0]] + text(value.skip(1)),
    {
        assert(value =~= seq![value[0]] + value.skip(1));
        lemma_payload_concat(seq![value[0]], value.skip(1));
        assert(seq![value[0]].drop_last() =~= Seq::<u8>::empty());
        assert(seq![value[0]].last() == value[0]);
        assert(payload(Seq::<u8>::empty()) == Seq::<u8>::empty());
        assert(payload(seq![value[0]]) =~= seq![1u8, value[0]]);
        assert(text(value) =~= seq![1u8, value[0]] + text(value.skip(1)));
    }

    /// Use to prove exact composite-key equality even when byte values include NUL or UTF-8.
    pub proof fn lemma_text_frame(left: Seq<u8>, right: Seq<u8>, left_tail: Seq<u8>, right_tail: Seq<u8>)
        requires text(left) + left_tail == text(right) + right_tail,
        ensures left == right, left_tail == right_tail,
        decreases left.len() + right.len(),
    {
        if left.len() == 0 {
            if right.len() > 0 {
                lemma_text_cons(right);
                assert((text(left) + left_tail)[0] == 0);
                assert((text(right) + right_tail)[0] == 1);
                assert(false);
            }
            assert((text(left) + left_tail).skip(1) =~= left_tail);
            assert((text(right) + right_tail).skip(1) =~= right_tail);
            assert(left =~= right);
        } else if right.len() == 0 {
            lemma_text_cons(left);
            assert((text(left) + left_tail)[0] == 1);
            assert((text(right) + right_tail)[0] == 0);
            assert(false);
        } else {
            lemma_text_cons(left);
            lemma_text_cons(right);
            assert((text(left) + left_tail)[1] == left[0]);
            assert((text(right) + right_tail)[1] == right[0]);
            assert(left[0] == right[0]);
            assert((text(left) + left_tail).skip(2) =~= text(left.skip(1)) + left_tail);
            assert((text(right) + right_tail).skip(2) =~= text(right.skip(1)) + right_tail);
            lemma_text_frame(left.skip(1), right.skip(1), left_tail, right_tail);
            assert(left =~= seq![left[0]] + left.skip(1));
            assert(right =~= seq![right[0]] + right.skip(1));
            assert(left =~= right);
        }
    }
    }
