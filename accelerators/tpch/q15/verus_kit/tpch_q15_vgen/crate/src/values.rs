use vstd::prelude::*;
use crate::model::*;
use verus_prooflib::ordered::Key;

verus! {
pub fn bin(date: i32) -> (result: i64)
    ensures result as int == bucket(date as int),
{
    let wide = date as i64;
    if wide < 0 { (wide - 255) / 256 } else { wide / 256 }
}
pub fn copy_bytes(bytes: &Vec<u8>) -> (result: Vec<u8>)
    ensures result@ == bytes@,
{
    let mut result = Vec::new();
    let mut position = 0;
    while position < bytes.len()
        invariant position <= bytes.len(), result@ == bytes@.take(position as int),
        decreases bytes.len() - position,
    {
        result.push(bytes[position]);
        position += 1;
    }
    result
}
pub fn append_part(result: &mut Vec<u8>, bytes: &Vec<u8>)
    ensures final(result)@ == old(result)@ + encode_part(bytes@),
{
    let mut position = 0;
    while position < bytes.len()
        invariant position <= bytes.len(),
            result@ == old(result)@ + encode_part(bytes@.take(position as int)).drop_last(),
        decreases bytes.len() - position,
    {
        proof {
            let prefix = bytes@.take(position as int);
            assert(bytes@.take(position as int + 1) =~= prefix.push(bytes@[position as int]));
            lemma_component_push(prefix, bytes@[position as int]);
        }
        result.push(1);
        result.push(bytes[position]);
        position += 1;
    }
    proof {
        assert(encode_part(bytes@).last() == 0u8);
        assert(encode_part(bytes@).drop_last().push(0u8) =~= encode_part(bytes@));
        assert((result@ + seq![0u8]) =~= old(result)@ + encode_part(bytes@));
    }
    result.push(0);
}
pub fn text_key(name: &Vec<u8>, address: &Vec<u8>, phone: &Vec<u8>) -> (result: Key)
    ensures result.view() == variant_key(name@, address@, phone@),
{
    let mut bytes = Vec::new();
    append_part(&mut bytes, name);
    append_part(&mut bytes, address);
    append_part(&mut bytes, phone);
    Key::text(bytes)
}
pub fn add(left: i128, right: i128) -> (result: Option<i128>)
    ensures result matches Some(value) ==> value as int == left as int + right as int,
{
    if right > 0 && left > i128::MAX - right { return None; }
    if right < 0 && left < i128::MIN - right { return None; }
    Some(left + right)
}
pub fn multiply(left: i128, right: i128) -> (result: Option<i128>)
    ensures result matches Some(value) ==> value as int == left as int * right as int,
{
    left.checked_mul(right)
}
pub proof fn lemma_component_push(bytes: Seq<u8>, byte: u8)
    ensures encode_part(bytes.push(byte)).drop_last() == encode_part(bytes).drop_last() + seq![1u8, byte],
{
    let left = encode_part(bytes.push(byte)).drop_last();
    let right = encode_part(bytes).drop_last() + seq![1u8, byte];
    assert(left =~= right) by {
        assert forall|position: int| 0 <= position < left.len() implies
            (#[trigger] left[position]) == right[position] by {
            if position < 2 * bytes.len() {
                assert(bytes.push(byte)[position / 2] == bytes[position / 2]);
            } else if position == 2 * bytes.len() {
                assert(position % 2 == 0) by (nonlinear_arith) requires position == 2 * bytes.len();
            } else {
                assert(position == 2 * bytes.len() + 1);
                assert(position % 2 == 1 && position / 2 == bytes.len()) by (nonlinear_arith)
                    requires position == 2 * bytes.len() + 1, bytes.len() >= 0;
            }
        }
    }
}
}
