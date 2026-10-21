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
    ensures result as int == (date as int - i32::MIN as int) / 128, 0 <= result < 67108864,
{
    (date as i64 - i32::MIN as i64) / 128
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

/// One shipment's price * (100 - discount); below 2^127 in magnitude for any i64 inputs.
pub fn revenue(price: i64, discount: i64) -> (result: i128)
    ensures result as int == price as int * (100 - discount as int),
        -0x8000_0000_0000_0000_0000_0000_0000_0000int < (result as int) < 0x8000_0000_0000_0000_0000_0000_0000_0000int,
{
    let factor = 100i128 - discount as i128;
    proof {
        let (p, q) = (price as int, factor as int);
        let half = 0x8000_0000_0000_0000int;
        assert(-half <= p <= half && -(half + 100) <= q <= half + 100);
        assert(-0x8000_0000_0000_0000_0000_0000_0000_0000int < p * q < 0x8000_0000_0000_0000_0000_0000_0000_0000int) by (nonlinear_arith)
            requires -half <= p <= half, -(half + 100) <= q <= half + 100, half == 0x8000_0000_0000_0000int;
    }
    price as i128 * factor
}

#[derive(Clone, Copy)]
pub struct GroupId {
    pub key: i64,
    pub date: i32,
    pub priority: i64,
}

pub open spec fn group_bytes(group: GroupId) -> Seq<u8> {
    verus_prooflib::be_u64(group.key as u64)
        + verus_prooflib::be_u64(group.date as u32 as u64)
        + verus_prooflib::be_u64(group.priority as u64)
}

pub fn encode(group: &GroupId) -> (result: Vec<u8>)
    ensures result@ == group_bytes(*group),
{
    let mut result = Vec::with_capacity(24);
    let key = group.key as u64;
    let date = group.date as u32 as u64;
    let priority = group.priority as u64;
    result.push((key >> 56) as u8);
    result.push((key >> 48) as u8);
    result.push((key >> 40) as u8);
    result.push((key >> 32) as u8);
    result.push((key >> 24) as u8);
    result.push((key >> 16) as u8);
    result.push((key >> 8) as u8);
    result.push(key as u8);
    result.push((date >> 56) as u8);
    result.push((date >> 48) as u8);
    result.push((date >> 40) as u8);
    result.push((date >> 32) as u8);
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
        verus_prooflib::lemma_be_u64_literal(date);
        verus_prooflib::lemma_be_u64_literal(priority);
        assert(result@ =~= group_bytes(*group));
    }
    result
}

pub proof fn lemma_group_bytes_len(group: GroupId)
    ensures group_bytes(group).len() == 24,
{}

/// The three words of a group's bytes decide equality and order.
pub proof fn lemma_group_bytes_order(left: GroupId, right: GroupId)
    ensures
        bytes_lt(group_bytes(left), group_bytes(right)) == ((left.key as u64) < (right.key as u64)
            || (left.key as u64 == right.key as u64 && ((left.date as u32) < (right.date as u32)
            || (left.date as u32 == right.date as u32 && (left.priority as u64) < (right.priority as u64))))),
        (group_bytes(left) == group_bytes(right)) == (left.key == right.key && left.date == right.date && left.priority == right.priority),
{
    let lk = left.key as u64; let rk = right.key as u64;
    let ld = left.date as u32 as u64; let rd = right.date as u32 as u64;
    let lp = left.priority as u64; let rp = right.priority as u64;
    verus_prooflib::lemma_be_u64_order(lk, rk);
    verus_prooflib::lemma_be_u64_order(ld, rd);
    verus_prooflib::lemma_be_u64_order(lp, rp);
    let tail_left = verus_prooflib::be_u64(ld) + verus_prooflib::be_u64(lp);
    let tail_right = verus_prooflib::be_u64(rd) + verus_prooflib::be_u64(rp);
    assert(group_bytes(left) =~= verus_prooflib::be_u64(lk) + tail_left);
    assert(group_bytes(right) =~= verus_prooflib::be_u64(rk) + tail_right);
    verus_prooflib::lemma_bytes_fixed_concat(verus_prooflib::be_u64(lk), verus_prooflib::be_u64(rk), tail_left, tail_right);
    verus_prooflib::lemma_bytes_fixed_concat(verus_prooflib::be_u64(ld), verus_prooflib::be_u64(rd), verus_prooflib::be_u64(lp), verus_prooflib::be_u64(rp));
    let ldate = left.date; let rdate = right.date;
    assert(((ldate as u32 as u64) < (rdate as u32 as u64)) == ((ldate as u32) < (rdate as u32))) by (bit_vector);
    assert(((ldate as u32 as u64) == (rdate as u32 as u64)) == ((ldate as u32) == (rdate as u32))) by (bit_vector);
    if group_bytes(left) == group_bytes(right) {
        assert(group_bytes(left).take(8) =~= verus_prooflib::be_u64(lk));
        assert(group_bytes(right).take(8) =~= verus_prooflib::be_u64(rk));
        assert(group_bytes(left).subrange(8, 16) =~= verus_prooflib::be_u64(ld));
        assert(group_bytes(right).subrange(8, 16) =~= verus_prooflib::be_u64(rd));
        assert(group_bytes(left).subrange(16, 24) =~= verus_prooflib::be_u64(lp));
        assert(group_bytes(right).subrange(16, 24) =~= verus_prooflib::be_u64(rp));
        assert(lk == rk && ld == rd && lp == rp);
        let lkey = left.key; let rkey = right.key; let lprio = left.priority; let rprio = right.priority;
        assert(lkey == rkey) by (bit_vector) requires lkey as u64 == rkey as u64;
        assert(ldate == rdate) by (bit_vector) requires ldate as u32 as u64 == rdate as u32 as u64;
        assert(lprio == rprio) by (bit_vector) requires lprio as u64 == rprio as u64;
    }
    if left.key == right.key && left.date == right.date && left.priority == right.priority {
        assert(group_bytes(left) =~= group_bytes(right));
    }
}

pub proof fn lemma_group_identity(left: GroupId, right: GroupId)
    requires group_bytes(left) == group_bytes(right),
    ensures left.key == right.key, left.date == right.date, left.priority == right.priority,
{
    lemma_group_bytes_order(left, right);
}

}
