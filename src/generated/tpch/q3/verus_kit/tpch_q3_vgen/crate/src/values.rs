use vstd::prelude::*;
use verus_common::bytes::*;
use verus_prooflib::ordered::Key;

verus! {

pub fn copy_bytes(source: &[u8]) -> (result: Vec<u8>)
    ensures result@ == source@,
{
    let mut result = Vec::new();
    let mut index = 0;
    while index < source.len()
        invariant index <= source.len(), result@ == source@.take(index as int),
        decreases source.len() - index,
    {
        result.push(source[index]);
        index += 1;
    }
    result
}

pub fn page_id(key: i64) -> (result: i64)
    ensures result as int == (key as int) / 256,
{
    let quotient = key / 256;
    if key % 256 < 0 { quotient - 1 } else { quotient }
}

pub fn leaf(date: i32) -> (result: i64)
    ensures result as int == (date as int - i32::MIN as int) / 64, 0 <= result < 67108864,
{
    (date as i64 - i32::MIN as i64) / 64
}

pub fn add(left: i128, right: i128) -> (result: Option<i128>)
    ensures result matches Some(value) ==> value as int == left as int + right as int,
{
    left.checked_add(right)
}

pub fn mul(left: i128, right: i128) -> (result: Option<i128>)
    ensures result matches Some(value) ==> value as int == left as int * right as int,
{
    left.checked_mul(right)
}

pub fn revenue(price: i64, discount: i64) -> (result: Option<i128>)
    ensures result matches Some(value) ==> value as int == price as int * (100 - discount as int),
{
    mul(price as i128, 100i128 - discount as i128)
}

#[derive(Clone, Copy)]
pub struct GroupId {
    pub key: i64,
    pub date: i32,
    pub priority: i64,
}

pub open spec fn group_bytes(group: GroupId) -> Seq<u8> {
    verus_prooflib::be_u64(group.key as u64)
        + Seq::new(4, |position: int| ((group.date as u32) >> (((3 - position) * 8) as u32)) as u8)
        + verus_prooflib::be_u64(group.priority as u64)
}

pub fn encode(group: GroupId) -> (result: Vec<u8>)
    ensures result@ == group_bytes(group),
{
    let mut result = Vec::new();
    let key = group.key as u64;
    let date = group.date as u32;
    let priority = group.priority as u64;
    result.push((key >> 56) as u8);
    result.push((key >> 48) as u8);
    result.push((key >> 40) as u8);
    result.push((key >> 32) as u8);
    result.push((key >> 24) as u8);
    result.push((key >> 16) as u8);
    result.push((key >> 8) as u8);
    result.push(key as u8);
    result.push((date >> 24) as u8);
    result.push((date >> 16) as u8);
    result.push((date >> 8) as u8);
    result.push(date as u8);
    result.push((priority >> 56) as u8);
    result.push((priority >> 48) as u8);
    result.push((priority >> 40) as u8);
    result.push((priority >> 32) as u8);
    result.push((priority >> 24) as u8);
    result.push((priority >> 16) as u8);
    result.push((priority >> 8) as u8);
    result.push(priority as u8);
    proof {
        verus_prooflib::lemma_be_u64_literal(key);
        verus_prooflib::lemma_be_u64_literal(priority);
        let date_bytes = Seq::new(4, |position: int| (date >> (((3 - position) * 8) as u32)) as u8);
        let literal = seq![(date >> 24) as u8, (date >> 16) as u8, (date >> 8) as u8, date as u8];
        assert((date >> 0u32) == date) by (bit_vector);
        assert forall|position: int| #![trigger date_bytes[position]] 0 <= position < 4 implies date_bytes[position] == literal[position] by {
            if position == 0 {} else if position == 1 {} else if position == 2 {} else {}
        }
        assert(date_bytes =~= literal);
        assert(result@ =~= group_bytes(group));
    }
    result
}

pub fn decode(bytes: &[u8]) -> (result: Option<GroupId>)
    ensures result.is_some() == (bytes@.len() == 20),
        result matches Some(group) ==> group_bytes(group) == bytes@,
{
    if bytes.len() != 20 { return None; }
    let key = verus_prooflib::read_be_u64(bytes, 0);
    let date = verus_prooflib::read_be_u32(bytes, 8);
    let priority = verus_prooflib::read_be_u64(bytes, 12);
    let group = GroupId { key: key as i64, date: date as i32, priority: priority as i64 };
    proof {
        assert((key as i64) as u64 == key) by (bit_vector);
        assert((date as i32) as u32 == date) by (bit_vector);
        assert((priority as i64) as u64 == priority) by (bit_vector);
        assert(group_bytes(group) =~= bytes@);
    }
    Some(group)
}

pub proof fn lemma_group_identity(left: GroupId, right: GroupId)
    requires group_bytes(left) == group_bytes(right),
    ensures left.key == right.key, left.date == right.date, left.priority == right.priority,
{
    let bytes = group_bytes(left);
    assert(bytes.subrange(0, 8) =~= verus_prooflib::be_u64(left.key as u64));
    assert(bytes.subrange(0, 8) =~= verus_prooflib::be_u64(right.key as u64));
    assert(bytes.subrange(8, 12) =~= verus_prooflib::be_u32(left.date as u32));
    assert(bytes.subrange(8, 12) =~= verus_prooflib::be_u32(right.date as u32));
    assert(bytes.subrange(12, 20) =~= verus_prooflib::be_u64(left.priority as u64));
    assert(bytes.subrange(12, 20) =~= verus_prooflib::be_u64(right.priority as u64));
    verus_prooflib::lemma_be_u64_order(left.key as u64, right.key as u64);
    verus_prooflib::lemma_be_u64_order(left.priority as u64, right.priority as u64);
    let first = left.date as u32;
    let second = right.date as u32;
    assert(verus_prooflib::be_u32(first)[0] == verus_prooflib::be_u32(second)[0]);
    assert(verus_prooflib::be_u32(first)[1] == verus_prooflib::be_u32(second)[1]);
    assert(verus_prooflib::be_u32(first)[2] == verus_prooflib::be_u32(second)[2]);
    assert(verus_prooflib::be_u32(first)[3] == verus_prooflib::be_u32(second)[3]);
    assert((first >> 0u32) == first && (second >> 0u32) == second) by (bit_vector);
    assert(first == second) by (bit_vector)
        requires ((first >> 24) as u8) == ((second >> 24) as u8),
            ((first >> 16) as u8) == ((second >> 16) as u8),
            ((first >> 8) as u8) == ((second >> 8) as u8), (first as u8) == (second as u8);
    let left_key = left.key;
    let right_key = right.key;
    let left_priority = left.priority;
    let right_priority = right.priority;
    let left_date = left.date;
    let right_date = right.date;
    assert(left_key == right_key) by (bit_vector) requires (left_key as u64) == (right_key as u64);
    assert(left_priority == right_priority) by (bit_vector) requires (left_priority as u64) == (right_priority as u64);
    assert(left_date == right_date) by (bit_vector) requires (left_date as u32) == (right_date as u32);
}

pub fn rank_key(bound: i128, group: GroupId) -> (result: Key)
    requires bound >= 0,
    ensures result.view() == (-(if bound >= i64::MAX { i64::MAX as int } else { bound as int }), group_bytes(group)),
{
    let capped = if bound >= i64::MAX as i128 { i64::MAX } else { bound as i64 };
    Key::composite(-capped, encode(group))
}

}
