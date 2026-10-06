use vstd::prelude::*;
use verus_prooflib::ordered::{Key, KeyView, key_lt, lemma_key_order};
use verus_common::bytes::*;
use crate::paged::*;

verus! {

/// One (customer) cell of a slice: count of lines, revenue and its positive part.
#[derive(Clone, Copy)]
pub struct CellEnt {
    pub customer: i64,
    pub count: u32,
    pub revenue: i64,
    pub upper: i64,
}

impl Ent for CellEnt {
    fn dup(&self) -> (result: Self) { *self }
    open spec fn kv(self) -> KeyView { (self.customer as int, Seq::<u8>::empty()) }
    open spec fn top() -> KeyView { (i64::MAX as int, seq![0u8]) }
    fn key(&self) -> (result: Key) { Key::number(self.customer) }
    fn top_key() -> (result: Key) {
        let mut bytes = Vec::new();
        bytes.push(0u8);
        Key::composite(i64::MAX, bytes)
    }
    fn before(&self, other: &Self) -> (result: bool) {
        proof { lemma_bytes_lt_irrefl(Seq::<u8>::empty()); }
        self.customer < other.customer
    }
    fn same_key(&self, other: &Self) -> (result: bool) { self.customer == other.customer }
    proof fn top_above(entry: Self) {
        assert(bytes_lt(Seq::<u8>::empty(), seq![0u8]));
    }
}

/// One rank entry of a slice: customers ordered by upper bound descending, then customer ascending.
#[derive(Clone, Copy)]
pub struct RankEnt {
    pub upper: i64,
    pub customer: i64,
}

pub open spec fn rank_before(left: RankEnt, right: RankEnt) -> bool {
    left.upper > right.upper || (left.upper == right.upper && left.customer < right.customer)
}

pub open spec fn rank_bias(customer: i64) -> u64 { (customer as u64) ^ (1u64 << 63) }

pub proof fn lemma_bias_order(left: i64, right: i64)
    ensures (rank_bias(left) < rank_bias(right)) == (left < right),
        (rank_bias(left) == rank_bias(right)) == (left == right),
{
    let left_bias = rank_bias(left);
    let right_bias = rank_bias(right);
    assert((left_bias < right_bias) == (left < right)) by (bit_vector)
        requires left_bias == (left as u64) ^ (1u64 << 63), right_bias == (right as u64) ^ (1u64 << 63);
    assert((left_bias == right_bias) == (left == right)) by (bit_vector)
        requires left_bias == (left as u64) ^ (1u64 << 63), right_bias == (right as u64) ^ (1u64 << 63);
}

pub proof fn lemma_rank_order(left: RankEnt, right: RankEnt)
    ensures key_lt(left.kv(), right.kv()) == rank_before(left, right),
        (left.kv() == right.kv()) == (left.upper == right.upper && left.customer == right.customer),
{
    lemma_bias_order(left.customer, right.customer);
    verus_prooflib::lemma_be_u64_order(rank_bias(left.customer), rank_bias(right.customer));
}

pub fn append_word(bytes: &mut Vec<u8>, value: u64)
    ensures final(bytes)@ == old(bytes)@ + verus_prooflib::be_u64(value),
{
    let ghost original = bytes@;
    bytes.push((value >> 56) as u8);
    bytes.push((value >> 48) as u8);
    bytes.push((value >> 40) as u8);
    bytes.push((value >> 32) as u8);
    bytes.push((value >> 24) as u8);
    bytes.push((value >> 16) as u8);
    bytes.push((value >> 8) as u8);
    bytes.push(value as u8);
    proof {
        verus_prooflib::lemma_be_u64_literal(value);
        assert(bytes@ =~= original + verus_prooflib::be_u64(value));
    }
}

impl Ent for RankEnt {
    fn dup(&self) -> (result: Self) { *self }
    open spec fn kv(self) -> KeyView { (-1 - self.upper as int, verus_prooflib::be_u64(rank_bias(self.customer))) }
    open spec fn top() -> KeyView { (i64::MAX as int, Seq::new(9, |position: int| 255u8)) }
    fn key(&self) -> (result: Key) {
        let mut bytes = Vec::new();
        append_word(&mut bytes, (self.customer as u64) ^ (1u64 << 63));
        Key::composite(-1i64 - self.upper, bytes)
    }
    fn top_key() -> (result: Key) {
        let mut bytes = Vec::new();
        let mut index = 0usize;
        while index < 9
            invariant index <= 9, bytes@ == Seq::new(index as nat, |position: int| 255u8),
            decreases 9 - index,
        {
            bytes.push(255u8);
            proof { assert(bytes@ =~= Seq::new(index as nat + 1, |position: int| 255u8)); }
            index += 1;
        }
        Key::composite(i64::MAX, bytes)
    }
    fn before(&self, other: &Self) -> (result: bool) {
        proof { lemma_rank_order(*self, *other); }
        self.upper > other.upper || (self.upper == other.upper && self.customer < other.customer)
    }
    fn same_key(&self, other: &Self) -> (result: bool) {
        proof { lemma_rank_order(*self, *other); }
        self.upper == other.upper && self.customer == other.customer
    }
    proof fn top_above(entry: Self) {
        let bytes = verus_prooflib::be_u64(rank_bias(entry.customer));
        assert(bytes.len() == 8);
        verus_prooflib::lemma_bytes_max_sentinel(bytes, 9);
        assert(key_lt(entry.kv(), Self::top()));
    }
}

}
