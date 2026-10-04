use vstd::prelude::*;
use verus_common::bytes::bytes_lt;

verus! {

pub open spec fn hash_bytes(bytes: Seq<u8>) -> u64
    decreases bytes.len(),
{
    if bytes.len() == 0 { 0 } else {
        ((hash_bytes(bytes.drop_last()) as int * 257 + bytes.last() as int + 1)
            % 4294967291) as u64
    }
}

pub fn hash(bytes: &[u8]) -> (out: u64)
    ensures out == hash_bytes(bytes@),
{
    let mut result: u64 = 0;
    let mut index = 0;
    while index < bytes.len()
        invariant index <= bytes.len(), result < 4294967291,
            result == hash_bytes(bytes@.take(index as int)),
        decreases bytes.len() - index,
    {
        proof {
            assert(bytes@.take(index as int + 1).drop_last() =~= bytes@.take(index as int));
            assert(bytes@.take(index as int + 1).last() == bytes@[index as int]);
        }
        result = (result * 257 + bytes[index] as u64 + 1) % 4294967291;
        index += 1;
    }
    proof { assert(bytes@.take(index as int) =~= bytes@); }
    result
}

pub fn equal(left: &[u8], right: &[u8]) -> (out: bool)
    ensures out == (left@ == right@),
{
    verus_common::bytes::bytes_eq_exec(left, right)
}

pub fn less(left: &[u8], right: &[u8]) -> (out: bool)
    ensures out == bytes_lt(left@, right@),
{
    verus_common::bytes::bytes_lt_exec(left, right)
}

pub fn copy(bytes: &[u8]) -> (out: Vec<u8>)
    ensures out@ == bytes@,
{
    let mut out = Vec::new();
    let mut index = 0;
    while index < bytes.len()
        invariant index <= bytes.len(), out@ == bytes@.take(index as int),
        decreases bytes.len() - index,
    {
        out.push(bytes[index]);
        index += 1;
    }
    out
}

}
