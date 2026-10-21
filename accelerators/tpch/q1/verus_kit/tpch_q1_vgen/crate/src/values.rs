use vstd::prelude::*;
use std::sync::Arc;
use verus_prooflib::wide::Wide;
use crate::gen_obligations::*;
use crate::gen_spec::*;

verus! {

pub struct Totals {
    pub count: Wide,
    pub quantity: Wide,
    pub price: Wide,
    pub discounted: Wide,
    pub charge: Wide,
    pub discount: Wide,
}

pub open spec fn weight(row: LineitemRow, component: int) -> int {
    if component == 0 { 1 }
    else if component == 1 { row.l_quantity }
    else if component == 2 { row.l_extendedprice }
    else if component == 3 { row.l_extendedprice * (100 - row.l_discount) }
    else if component == 4 { row.l_extendedprice * (100 - row.l_discount) * (100 + row.l_tax) }
    else { row.l_discount }
}

impl Totals {
    pub open spec fn at(self, component: int) -> int {
        if component == 0 { self.count@ }
        else if component == 1 { self.quantity@ }
        else if component == 2 { self.price@ }
        else if component == 3 { self.discounted@ }
        else if component == 4 { self.charge@ }
        else { self.discount@ }
    }

    pub open spec fn vanishes(self) -> bool {
        forall|component: int| #[trigger] self.at(component) == 0
    }

    pub fn zero() -> (result: Self)
        ensures forall|component: int| #[trigger] result.at(component) == 0,
    {
        Self { count: Wide::zero(), quantity: Wide::zero(), price: Wide::zero(),
            discounted: Wide::zero(), charge: Wide::zero(), discount: Wide::zero() }
    }

    pub fn copy(&self) -> (result: Self)
        ensures result == *self,
    {
        Self { count: self.count.dup(), quantity: self.quantity.dup(), price: self.price.dup(),
            discounted: self.discounted.dup(), charge: self.charge.dup(), discount: self.discount.dup() }
    }

    pub fn add(&self, other: &Self) -> (result: Self)
        ensures forall|component: int| #[trigger] result.at(component) == self.at(component) + other.at(component),
    {
        Self { count: self.count.add(&other.count), quantity: self.quantity.add(&other.quantity),
            price: self.price.add(&other.price), discounted: self.discounted.add(&other.discounted),
            charge: self.charge.add(&other.charge), discount: self.discount.add(&other.discount) }
    }

    pub fn scaled(&self, sign: i64) -> (result: Self)
        ensures forall|component: int| #[trigger] result.at(component) == self.at(component) * sign,
    {
        let factor = Wide::from_i128(sign as i128);
        Self { count: self.count.mul(&factor), quantity: self.quantity.mul(&factor),
            price: self.price.mul(&factor), discounted: self.discounted.mul(&factor),
            charge: self.charge.mul(&factor), discount: self.discount.mul(&factor) }
    }

    pub fn is_zero(&self) -> (result: bool)
        ensures result == self.vanishes(),
    {
        let result = self.count.is_zero() && self.quantity.is_zero() && self.price.is_zero()
            && self.discounted.is_zero() && self.charge.is_zero() && self.discount.is_zero();
        proof {
            if !result {
                if self.count@ != 0 { assert(self.at(0) != 0); }
                else if self.quantity@ != 0 { assert(self.at(1) != 0); }
                else if self.price@ != 0 { assert(self.at(2) != 0); }
                else if self.discounted@ != 0 { assert(self.at(3) != 0); }
                else if self.charge@ != 0 { assert(self.at(4) != 0); }
                else { assert(self.at(5) != 0); }
            }
        }
        result
    }
}

pub fn contribution(row: &LineitemInput) -> (result: Totals)
    ensures forall|component: int| #[trigger] result.at(component) == weight(lineitem_input_view(*row), component),
{
    let price = Wide::from_i128(row.l_extendedprice as i128);
    let discounted = price.mul(&Wide::from_i128(100i128 - row.l_discount as i128));
    let charge = discounted.mul(&Wide::from_i128(100i128 + row.l_tax as i128));
    Totals { count: Wide::from_i128(1), quantity: Wide::from_i128(row.l_quantity as i128), price,
        discounted, charge, discount: Wide::from_i128(row.l_discount as i128) }
}

pub fn copy_bytes(bytes: &Vec<u8>) -> (result: Vec<u8>)
    ensures result@ == bytes@,
{
    let mut result = Vec::new();
    let mut index = 0usize;
    while index < bytes.len()
        invariant index <= bytes.len(), result@ == bytes@.take(index as int),
        decreases bytes.len() - index,
    {
        result.push(bytes[index]);
        index += 1;
    }
    proof { assert(bytes@.take(index as int) =~= bytes@); }
    result
}

#[verifier::opaque]
pub open spec fn hash_bytes(bytes: Seq<u8>, seed: int) -> int
    decreases bytes.len(),
{
    if bytes.len() == 0 { seed }
    else { (hash_bytes(bytes.drop_last(), seed) * 257 + bytes.last() as int + 1) % 18446744073709551616 }
}

pub open spec fn group_hash(flag: Seq<u8>, status: Seq<u8>) -> u64 {
    hash_bytes(status, (hash_bytes(flag, 0) * 257) % 18446744073709551616) as u64
}

pub fn hash_part(bytes: &Vec<u8>, seed: u64) -> (result: u64)
    ensures result as int == hash_bytes(bytes@, seed as int),
{
    proof { reveal(hash_bytes); }
    let mut result = seed;
    let mut index = 0usize;
    while index < bytes.len()
        invariant index <= bytes.len(), result as int == hash_bytes(bytes@.take(index as int), seed as int),
        decreases bytes.len() - index,
    {
        proof {
            let next = bytes@.take(index as int + 1);
            assert(next.drop_last() =~= bytes@.take(index as int));
            assert(next.last() == bytes@[index as int]);
            reveal(hash_bytes);
        }
        result = ((result as u128 * 257 + bytes[index] as u128 + 1) % 18446744073709551616u128) as u64;
        index += 1;
    }
    proof { assert(bytes@.take(index as int) =~= bytes@); }
    result
}

pub fn hash_group(flag: &Vec<u8>, status: &Vec<u8>) -> (result: u64)
    ensures result == group_hash(flag@, status@),
{
    let first = hash_part(flag, 0);
    let separator = ((first as u128 * 257) % 18446744073709551616u128) as u64;
    hash_part(status, separator)
}

pub struct Group {
    pub flag: Arc<Vec<u8>>,
    pub status: Arc<Vec<u8>>,
    pub totals: Totals,
}

impl Group {
    pub open spec fn key(self) -> (Seq<u8>, Seq<u8>) { ((*self.flag)@, (*self.status)@) }

    pub fn share(&self) -> (result: Self)
        ensures result == *self, result.key() == self.key(), result.totals == self.totals,
    {
        Self { flag: self.flag.clone(), status: self.status.clone(), totals: self.totals.copy() }
    }
}

}
