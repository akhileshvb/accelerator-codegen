// A rank entry: a group's id under its clamped upper bound, ordered by bound descending, then by
// the group's id bytes.  Stored in a `Paged` set per (tree node, segment).
use vstd::prelude::*;
use verus_common::bytes::*;
use verus_prooflib::ordered::{Key, KeyView, key_lt, lemma_key_order};
use crate::paged::*;
use crate::values::*;

verus! {

#[derive(Clone, Copy)]
pub struct RankEnt {
    pub upper: i64,
    pub id: GroupId,
}

impl RankEnt {
    pub open spec fn id(self) -> GroupId {
        self.id
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
        Key::composite(-1i64 - self.upper, encode(&self.id))
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
        let left = self.id;
        let right = other.id;
        self.upper > other.upper || (self.upper == other.upper && ((left.key as u64) < (right.key as u64)
            || (left.key as u64 == right.key as u64 && ((left.date as u32) < (right.date as u32)
            || (left.date as u32 == right.date as u32 && (left.priority as u64) < (right.priority as u64))))))
    }
    fn same_key(&self, other: &Self) -> (result: bool) {
        proof { lemma_rank_order(*self, *other); }
        self.upper == other.upper && self.id.key == other.id.key && self.id.date == other.id.date && self.id.priority == other.id.priority
    }
    proof fn top_above(entry: Self) {
        lemma_group_bytes_len(entry.id());
        verus_prooflib::lemma_bytes_max_sentinel(group_bytes(entry.id()), 25);
        assert(key_lt(entry.kv(), Self::top()));
    }
}

pub fn rank_ent(bound: i64, id: GroupId) -> (result: RankEnt)
    requires bound >= 0,
    ensures result.id() == id,
        result.kv() == rank_of(bound as int, id),
        result.upper >= 0,
{
    RankEnt { upper: bound, id }
}

}
