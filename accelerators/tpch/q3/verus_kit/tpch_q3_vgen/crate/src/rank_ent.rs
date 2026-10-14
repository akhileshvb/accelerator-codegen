// A rank entry: a group's id under its clamped upper bound, ordered by bound descending, then by
// the group's id bytes.  Stored in a `Paged` set per (tree node, segment).
use vstd::prelude::*;
use verus_common::bytes::*;
use verus_prooflib::ordered::{Key, KeyView, key_lt, lemma_key_order};
use crate::paged::*;
use crate::values::*;

verus! {

pub open spec fn id_fits(id: GroupId) -> bool {
    0 <= id.key < 1099511627776 && 0 <= id.date <= 65535 && -128 <= id.priority <= 127
}

pub open spec fn decode_id(word: u64) -> GroupId {
    GroupId { key: (word >> 24) as i64, date: ((word >> 8) & 0xFFFF) as i32, priority: ((word & 0xFF) as u8) as i8 as i64 }
}

/// A group id of 40 key bits, 16 date bits and 8 priority bits in one word.
pub fn encode_id(id: GroupId) -> (word: u64)
    requires id_fits(id),
    ensures decode_id(word) == id,
{
    let key = id.key;
    let date = id.date;
    let priority = id.priority;
    let word = ((key as u64) << 24) | ((date as u64) << 8) | ((priority as i8 as u8) as u64);
    proof {
        assert((word >> 24) as i64 == key) by (bit_vector)
            requires word == ((key as u64) << 24) | ((date as u64) << 8) | ((priority as i8 as u8) as u64),
                0 <= key < 1099511627776i64, 0 <= date <= 65535i32, -128 <= priority <= 127i64;
        assert(((word >> 8) & 0xFFFF) as i32 == date) by (bit_vector)
            requires word == ((key as u64) << 24) | ((date as u64) << 8) | ((priority as i8 as u8) as u64),
                0 <= key < 1099511627776i64, 0 <= date <= 65535i32, -128 <= priority <= 127i64;
        assert((((word & 0xFF) as u8) as i8 as i64) == priority) by (bit_vector)
            requires word == ((key as u64) << 24) | ((date as u64) << 8) | ((priority as i8 as u8) as u64),
                0 <= key < 1099511627776i64, 0 <= date <= 65535i32, -128 <= priority <= 127i64;
    }
    word
}

pub proof fn lemma_decode_injective(left: u64, right: u64)
    ensures decode_id(left) == decode_id(right) ==> left == right,
{
    if decode_id(left) == decode_id(right) {
        assert(left == right) by (bit_vector)
            requires (left >> 24) as i64 == (right >> 24) as i64,
                ((left >> 8) & 0xFFFF) as i32 == ((right >> 8) & 0xFFFF) as i32,
                ((left & 0xFF) as u8) as i8 as i64 == ((right & 0xFF) as u8) as i8 as i64;
    }
}

pub fn decode_word(word: u64) -> (id: GroupId)
    ensures id == decode_id(word),
{
    GroupId { key: (word >> 24) as i64, date: ((word >> 8) & 0xFFFF) as i32, priority: ((word & 0xFF) as u8) as i8 as i64 }
}

#[derive(Clone, Copy)]
pub struct RankEnt {
    pub upper: i64,
    pub word: u64,
}

impl RankEnt {
    pub open spec fn id(self) -> GroupId {
        decode_id(self.word)
    }
}

/// The ordered-map key of a rank: larger bounds first, ties by the group's id bytes.
pub open spec fn rank_of(upper: int, id: GroupId) -> KeyView {
    (-1 - upper, group_bytes(id))
}

pub open spec fn rank_before(left: RankEnt, right: RankEnt) -> bool {
    left.upper > right.upper || (left.upper == right.upper && ((left.id().key as u64) < (right.id().key as u64)
        || (left.id().key as u64 == right.id().key as u64 && ((left.id().date as u32) < (right.id().date as u32)
        || (left.id().date as u32 == right.id().date as u32 && (left.id().priority as u64) < (right.id().priority as u64))))))
}

pub proof fn lemma_rank_order(left: RankEnt, right: RankEnt)
    ensures key_lt(left.kv(), right.kv()) == rank_before(left, right),
        (left.kv() == right.kv()) == (left.upper == right.upper && left.id() == right.id()),
{
    lemma_group_bytes_order(left.id(), right.id());
}

impl Ent for RankEnt {
    fn dup(&self) -> (result: Self) { *self }
    open spec fn kv(self) -> KeyView { rank_of(self.upper as int, self.id()) }
    open spec fn top() -> KeyView { (i64::MAX as int, Seq::new(25, |position: int| 255u8)) }
    fn key(&self) -> (result: Key) {
        Key::composite(-1i64 - self.upper, encode(decode_word(self.word)))
    }
    fn top_key() -> (result: Key) {
        let mut bytes = Vec::new();
        let mut index = 0usize;
        while index < 25
            invariant index <= 25, bytes@ == Seq::new(index as nat, |position: int| 255u8),
            decreases 25 - index,
        {
            bytes.push(255u8);
            proof { assert(bytes@ =~= Seq::new(index as nat + 1, |position: int| 255u8)); }
            index += 1;
        }
        Key::composite(i64::MAX, bytes)
    }
    fn before(&self, other: &Self) -> (result: bool) {
        proof { lemma_rank_order(*self, *other); }
        let left = decode_word(self.word);
        let right = decode_word(other.word);
        self.upper > other.upper || (self.upper == other.upper && ((left.key as u64) < (right.key as u64)
            || (left.key as u64 == right.key as u64 && ((left.date as u32) < (right.date as u32)
            || (left.date as u32 == right.date as u32 && (left.priority as u64) < (right.priority as u64))))))
    }
    fn same_key(&self, other: &Self) -> (result: bool) {
        proof { lemma_rank_order(*self, *other); lemma_decode_injective(self.word, other.word); }
        self.upper == other.upper && self.word == other.word
    }
    proof fn top_above(entry: Self) {
        lemma_group_bytes_len(entry.id());
        verus_prooflib::lemma_bytes_max_sentinel(group_bytes(entry.id()), 25);
        assert(key_lt(entry.kv(), Self::top()));
    }
}

pub fn rank_ent(bound: i128, id: GroupId) -> (result: RankEnt)
    requires bound >= 0, id_fits(id),
    ensures result.id() == id,
        result.kv() == rank_of(if bound >= i64::MAX { i64::MAX as int } else { bound as int }, id),
        result.upper >= 0,
{
    let capped = if bound >= i64::MAX as i128 { i64::MAX } else { bound as i64 };
    RankEnt { upper: capped, word: encode_id(id) }
}

}
