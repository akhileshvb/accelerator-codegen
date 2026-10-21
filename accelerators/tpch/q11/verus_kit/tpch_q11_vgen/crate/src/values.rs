use vstd::prelude::*;
use verus_prooflib::ordered::{Key, KeyView};
use verus_prooflib::wide::Wide;

verus! {

/// The exact value of a moment (spec).
pub struct Moment {
    pub count: int,
    pub value: int,
}

pub open spec fn zero_moment() -> Moment { Moment { count: 0, value: 0 } }

/// A moment as stored: exact integers of any size.
pub struct Mom {
    pub count: Wide,
    pub value: Wide,
}

impl Mom {
    pub open spec fn view(self) -> Moment { Moment { count: self.count@, value: self.value@ } }

    pub fn dup(&self) -> (result: Mom)
        ensures result == *self,
    { Mom { count: self.count.dup(), value: self.value.dup() } }
}

pub fn zero() -> (result: Mom)
    ensures result@ == zero_moment(),
{
    Mom { count: Wide::zero(), value: Wide::zero() }
}

pub fn plus(left: &Mom, right: &Mom) -> (result: Mom)
    ensures result@.count == left@.count + right@.count && result@.value == left@.value + right@.value,
{
    Mom { count: left.count.add(&right.count), value: left.value.add(&right.value) }
}

pub fn scale(moment: &Mom, weight: &Wide) -> (result: Mom)
    ensures result@.count == moment@.count * weight@ && result@.value == moment@.value * weight@,
{
    Mom { count: moment.count.mul(weight), value: moment.value.mul(weight) }
}

pub fn copy_bytes(bytes: &[u8]) -> (result: Vec<u8>)
    ensures result@ == bytes@,
{
    let mut result = Vec::new();
    let mut index: usize = 0;
    while index < bytes.len()
        invariant index <= bytes.len(), result@ == bytes@.take(index as int),
        decreases bytes.len() - index,
    {
        result.push(bytes[index]);
        index += 1;
    }
    result
}

pub open spec fn rank_bytes(value: i128, part: i64) -> Seq<u8> {
    let descending: u64 = !(value as u64);
    let ascending: u64 = (part as u64) ^ (1u64 << 63);
    Seq::new(16, |index: int| if index < 8 {
        (descending >> (((7 - index) * 8) as u32)) as u8
    } else {
        (ascending >> (((15 - index) * 8) as u32)) as u8
    })
}

pub open spec fn rank_view(value: i128, part: i64) -> KeyView {
    ((!((value >> 64) as i64)) as int, rank_bytes(value, part))
}

pub proof fn lemma_rank_model(value: i128, part: i64)
    ensures rank_view(value, part) == verus_prooflib::i128_rank(value, part),
{
    let descending = !(value as u64);
    let ascending = (part as u64) ^ (1u64 << 63);
    let combined = verus_prooflib::be_u64(descending) + verus_prooflib::be_u64(ascending);
    assert forall|position: int| 0 <= position < 16 implies
        #[trigger] rank_bytes(value, part)[position] == combined[position] by {
        if position >= 8 {
            assert(7 - (position - 8) == 15 - position);
        }
    }
    assert(rank_bytes(value, part) =~= combined);
}

pub proof fn lemma_rank_order(left: i128, right: i128, left_part: i64, right_part: i64)
    ensures verus_prooflib::ordered::key_lt(rank_view(left, left_part), rank_view(right, right_part))
        == (left > right || (left == right && left_part < right_part)),
        (rank_view(left, left_part) == rank_view(right, right_part)) == (left == right && left_part == right_part),
{
    lemma_rank_model(left, left_part);
    lemma_rank_model(right, right_part);
    verus_prooflib::lemma_i128_rank_order(left, right, left_part, right_part);
}

pub proof fn lemma_rank_sentinel(value: i128, part: i64)
    ensures verus_prooflib::ordered::key_lt(rank_view(value, part), sentinel_view()),
        i64::MIN <= rank_view(value, part).0 <= i64::MAX,
        rank_view(value, part).1.len() == 16,
{
    lemma_rank_model(value, part);
    verus_prooflib::lemma_i128_rank_sentinel(value, part);
}

pub fn rank_key(value: i128, part: i64) -> (result: Key)
    ensures result.view() == rank_view(value, part),
{
    let number = !((value >> 64) as i64);
    let descending = !(value as u64);
    let ascending = (part as u64) ^ (1u64 << 63);
    let mut bytes = Vec::new();
    let mut index: usize = 0;
    while index < 8
        invariant index <= 8, bytes@ == rank_bytes(value, part).take(index as int),
            descending == !(value as u64), ascending == (part as u64) ^ (1u64 << 63),
            number == !((value >> 64) as i64),
        decreases 8 - index,
    {
        proof {
            assert(rank_bytes(value, part)[index as int] == (descending >> (((7 - index) * 8) as u32)) as u8);
            assert(rank_bytes(value, part).take(index as int + 1)
                =~= rank_bytes(value, part).take(index as int).push(rank_bytes(value, part)[index as int]));
        }
        bytes.push((descending >> (((7 - index) * 8) as u32)) as u8);
        index += 1;
    }
    while index < 16
        invariant 8 <= index <= 16, bytes@ == rank_bytes(value, part).take(index as int),
            descending == !(value as u64), ascending == (part as u64) ^ (1u64 << 63),
            number == !((value >> 64) as i64),
        decreases 16 - index,
    {
        proof {
            assert(rank_bytes(value, part)[index as int] == (ascending >> (((15 - index) * 8) as u32)) as u8);
            assert(rank_bytes(value, part).take(index as int + 1)
                =~= rank_bytes(value, part).take(index as int).push(rank_bytes(value, part)[index as int]));
        }
        bytes.push((ascending >> (((15 - index) * 8) as u32)) as u8);
        index += 1;
    }
    Key::composite(number, bytes)
}

pub open spec fn sentinel_view() -> KeyView {
    (i64::MAX as int, Seq::new(17, |index: int| 255u8))
}

pub fn sentinel_key() -> (result: Key)
    ensures result.view() == sentinel_view(),
{
    let mut bytes = Vec::new();
    let mut index: usize = 0;
    while index < 17
        invariant index <= 17, bytes@ == sentinel_view().1.take(index as int),
        decreases 17 - index,
    {
        bytes.push(255);
        index += 1;
    }
    Key::composite(i64::MAX, bytes)
}

}
