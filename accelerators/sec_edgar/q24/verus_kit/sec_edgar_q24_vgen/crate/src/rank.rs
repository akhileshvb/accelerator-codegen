// Ordered-map keys that sort groups by count, largest first, then by group key.
#![allow(unused_imports)]
use vstd::prelude::*;
use verus_common::bytes::*;
use verus_prooflib::*;
use verus_prooflib::ordered::{Key, KeyView, key_lt};
use crate::esc::*;

verus! {

/// The key of a group with count ci whose group key is ik.
pub open spec fn rkey(ci: i128, ik: KeyView) -> KeyView {
    let rk = i128_rank(ci, 0);
    (rk.0, rk.1 + ik.1)
}

pub proof fn lemma_rkey_order(c1: i128, i1: KeyView, c2: i128, i2: KeyView)
    ensures
        key_lt(rkey(c1, i1), rkey(c2, i2)) == (c1 > c2 || (c1 == c2 && bytes_lt(i1.1, i2.1))),
        (rkey(c1, i1) == rkey(c2, i2)) == (c1 == c2 && i1.1 == i2.1),
{
    lemma_i128_rank_order(c1, c2, 0, 0);
    lemma_i128_rank_sentinel(c1, 0);
    lemma_i128_rank_sentinel(c2, 0);
    let (r1, r2) = (i128_rank(c1, 0), i128_rank(c2, 0));
    lemma_bytes_fixed_concat(r1.1, r2.1, i1.1, i2.1);
    if rkey(c1, i1) == rkey(c2, i2) {
        assert(r1.0 == r2.0);
        assert((r1.1 + i1.1).take(16) =~= r1.1);
        assert((r2.1 + i2.1).take(16) =~= r2.1);
        assert((r1.1 + i1.1).skip(16) =~= i1.1);
        assert((r2.1 + i2.1).skip(16) =~= i2.1);
    }
}

pub fn push_be_u64(out: &mut Vec<u8>, v: u64)
    ensures final(out)@ == old(out)@ + be_u64(v),
{
    proof { lemma_be_u64_literal(v); }
    out.push((v >> 56) as u8);
    out.push((v >> 48) as u8);
    out.push((v >> 40) as u8);
    out.push((v >> 32) as u8);
    out.push((v >> 24) as u8);
    out.push((v >> 16) as u8);
    out.push((v >> 8) as u8);
    out.push(v as u8);
    proof { assert(out@ =~= old(out)@ + be_u64(v)); }
}

pub fn rank_key(ci: i128, ik: &Key) -> (k: Key)
    ensures k.view() == rkey(ci, ik.view()),
{
    let mut text: Vec<u8> = Vec::new();
    push_be_u64(&mut text, !(ci as u64));
    push_be_u64(&mut text, 0u64 ^ (1u64 << 63));
    let mut i = 0;
    while i < ik.text.len()
        invariant
            i <= ik.text.len(),
            text@ == i128_rank(ci, 0).1 + (*ik.text)@.take(i as int),
        decreases ik.text.len() - i,
    {
        text.push(ik.text[i]);
        proof { assert((*ik.text)@.take(i as int + 1) =~= (*ik.text)@.take(i as int).push((*ik.text)@[i as int])); }
        i += 1;
    }
    proof { assert((*ik.text)@.take(i as int) =~= (*ik.text)@); }
    let number: i64 = !((ci >> 64) as i64);
    proof {
        assert(i128_rank(ci, 0).0 == number as int);
        assert((0i64 as u64) ^ (1u64 << 63) == 0u64 ^ (1u64 << 63)) by (bit_vector);
    }
    Key::composite(number, text)
}

} // verus!
