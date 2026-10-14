use vstd::prelude::*;
use std::sync::Arc;
use verus_prooflib::ordered::{Key, Index};
use verus_common::sums::sum;
pub use crate::encoding::{number as encode_nat, payload as encode_payload, text as encode_text};

verus! {

#[derive(Clone, Copy)]
pub struct Moments {
    pub count: i128,
    pub revenue: i128,
    pub upper: i128,
}

impl Moments {
    pub open spec fn component(self, component: int) -> int {
        if component == 0 { self.count as int }
        else if component == 1 { self.revenue as int } else { self.upper as int }
    }

    pub fn zero() -> (result: Self)
        ensures result.count == 0, result.revenue == 0, result.upper == 0,
    { Self { count: 0, revenue: 0, upper: 0 } }

    pub fn is_zero(&self) -> (result: bool)
        ensures result == (self.count == 0 && self.revenue == 0 && self.upper == 0),
    { self.count == 0 && self.revenue == 0 && self.upper == 0 }

    pub fn add(&self, other: Self) -> (result: Option<Self>)
        ensures result matches Some(value) ==> forall|component: int|
            #[trigger] value.component(component) == self.component(component) + other.component(component),
    {
        let count = match self.count.checked_add(other.count) { Some(value) => value, None => return None };
        let revenue = match self.revenue.checked_add(other.revenue) { Some(value) => value, None => return None };
        let upper = match self.upper.checked_add(other.upper) { Some(value) => value, None => return None };
        Some(Self { count, revenue, upper })
    }

    pub fn scale(&self, factor: i128) -> (result: Option<Self>)
        ensures result matches Some(value) ==> forall|component: int|
            #[trigger] value.component(component) == self.component(component) * factor,
    {
        let count = match self.count.checked_mul(factor) { Some(value) => value, None => return None };
        let revenue = match self.revenue.checked_mul(factor) { Some(value) => value, None => return None };
        let upper = match self.upper.checked_mul(factor) { Some(value) => value, None => return None };
        Some(Self { count, revenue, upper })
    }
}

pub fn line_moments(price: i64, discount: i64) -> (result: Moments)
    ensures result.count == 1,
        result.revenue == price as int * (100 - discount as int),
        result.upper == if result.revenue > 0 { result.revenue as int } else { 0int },
{
    proof {
        assert(i128::MIN <= price as int * (100 - discount as int) <= i128::MAX) by (nonlinear_arith);
    }
    let revenue = (price as i128) * (100i128 - discount as i128);
    Moments { count: 1, revenue, upper: if revenue > 0 { revenue } else { 0 } }
}

pub open spec fn moment_at(index: Index<Moments>, key: int) -> Moments {
    match index.at((key, Seq::empty())) {
        Some(value) => value,
        None => Moments { count: 0, revenue: 0, upper: 0 },
    }
}

pub open spec fn moment_index_valid(index: Index<Moments>) -> bool {
    forall|key: (int, Seq<u8>)| #[trigger] index.at(key).is_some() ==> {
        let value = index.at(key).unwrap();
        &&& key.1 == Seq::<u8>::empty()
        &&& i64::MIN <= key.0 <= i64::MAX
        &&& value.count != 0 || value.revenue != 0 || value.upper != 0
    }
}

pub fn get_moments(index: &Index<Moments>, key: i64) -> (result: Moments)
    ensures result == moment_at(*index, key as int),
{
    match index.get(&Key::number(key)) { Some(value) => *value, None => Moments::zero() }
}

pub fn add_moments(index: &mut Index<Moments>, key: i64, change: Moments) -> (result: bool)
    ensures result ==> final(index).wf(),
        result && moment_index_valid(*old(index)) ==> moment_index_valid(*final(index)),
        result ==> forall|query: int, component: int| #[trigger] moment_at(*final(index), query).component(component)
        == moment_at(*old(index), query).component(component) + if query == key { change.component(component) } else { 0 },
{
    let next = match get_moments(index, key).add(change) { Some(value) => value, None => return false };
    if next.is_zero() { index.remove(&Key::number(key)); }
    else { index.put(Key::number(key), Arc::new(next)); }
    true
}

pub fn append_number(bytes: &mut Vec<u8>, value: u128, width: usize)
    requires width <= 16,
    ensures final(bytes)@ == old(bytes)@ + encode_nat(value as nat, width as nat),
    decreases width,
{
    let ghost before = bytes@;
    if width > 0 {
        append_number(bytes, value / 256, width - 1);
        bytes.push((value % 256) as u8);
        proof {
            assert(bytes@ =~= before + encode_nat(value as nat, width as nat));
        }
    } else {
        proof { assert(bytes@ =~= before + Seq::<u8>::empty()); }
    }
}

pub fn append_i64(bytes: &mut Vec<u8>, value: i64)
    ensures final(bytes)@ == old(bytes)@ + encode_nat((value as int - i64::MIN as int) as nat, 8),
{
    append_number(bytes, (value as i128 - i64::MIN as i128) as u128, 8);
}

pub fn append_text(bytes: &mut Vec<u8>, text: &[u8])
    ensures final(bytes)@ == old(bytes)@ + encode_text(text@),
{
    let ghost before = bytes@;
    let mut index = 0usize;
    while index < text.len()
        invariant index <= text.len(), bytes@ == before + encode_payload(text@.take(index as int)),
        decreases text.len() - index,
    {
        let ghost processed = index as int;
        bytes.push(1);
        bytes.push(text[index]);
        proof {
            assert(text@.take(index as int + 1).drop_last() =~= text@.take(index as int));
            assert(text@.take(index as int + 1).last() == text@[index as int]);
            assert(bytes@ =~= before + encode_payload(text@.take(index as int + 1)));
        }
        index += 1;
    }
    bytes.push(0);
    proof {
        assert(text@.take(text@.len() as int) =~= text@);
        assert(bytes@ =~= before + encode_text(text@));
    }
}

pub fn copy_bytes(bytes: &[u8]) -> (result: Vec<u8>)
    ensures result@ == bytes@,
{
    let mut result = Vec::new();
    let mut index = 0usize;
    while index < bytes.len()
        invariant index <= bytes.len(), result@ == bytes@.take(index as int),
        decreases bytes.len() - index,
    {
        let ghost processed = index as int;
        result.push(bytes[index]);
        index += 1;
    }
    result
}

pub open spec fn rank_view(upper: int, customer: int) -> (int, Seq<u8>) {
    verus_prooflib::i128_rank(upper as i128, customer as i64)
}

pub fn append_rank_word(bytes: &mut Vec<u8>, value: u64)
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

pub fn rank_key(upper: i128, customer: i64) -> (result: Key)
    requires upper >= 0,
    ensures result.view() == rank_view(upper as int, customer as int),
{
    let mut bytes = Vec::new();
    append_rank_word(&mut bytes, !(upper as u64));
    append_rank_word(&mut bytes, (customer as u64) ^ (1u64 << 63));
    Key::composite(!((upper >> 64) as i64), bytes)
}

}
