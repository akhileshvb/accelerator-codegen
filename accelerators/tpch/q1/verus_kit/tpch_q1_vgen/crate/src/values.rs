use vstd::prelude::*;
use std::sync::Arc;
use crate::gen_obligations::*;
use crate::gen_spec::*;

verus! {

pub struct Totals {
    pub count: i128,
    pub quantity: i128,
    pub price: i128,
    pub discounted: i128,
    pub charge: i128,
    pub discount: i128,
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
        if component == 0 { self.count as int }
        else if component == 1 { self.quantity as int }
        else if component == 2 { self.price as int }
        else if component == 3 { self.discounted as int }
        else if component == 4 { self.charge as int }
        else { self.discount as int }
    }

    pub fn zero() -> (result: Self)
        ensures forall|component: int| #[trigger] result.at(component) == 0,
    {
        Self { count: 0, quantity: 0, price: 0, discounted: 0, charge: 0, discount: 0 }
    }

    pub fn copy(&self) -> (result: Self)
        ensures result == *self,
    {
        Self { count: self.count, quantity: self.quantity, price: self.price,
            discounted: self.discounted, charge: self.charge, discount: self.discount }
    }

    pub fn add(&self, other: &Self) -> (result: Option<Self>)
        ensures result matches Some(value) ==> forall|component: int|
            #[trigger] value.at(component) == self.at(component) + other.at(component),
    {
        let count = match self.count.checked_add(other.count) { Some(value) => value, None => return None };
        let quantity = match self.quantity.checked_add(other.quantity) { Some(value) => value, None => return None };
        let price = match self.price.checked_add(other.price) { Some(value) => value, None => return None };
        let discounted = match self.discounted.checked_add(other.discounted) { Some(value) => value, None => return None };
        let charge = match self.charge.checked_add(other.charge) { Some(value) => value, None => return None };
        let discount = match self.discount.checked_add(other.discount) { Some(value) => value, None => return None };
        Some(Self { count, quantity, price, discounted, charge, discount })
    }

    pub fn scaled(&self, sign: i64) -> (result: Option<Self>)
        ensures result matches Some(value) ==> forall|component: int|
            #[trigger] value.at(component) == self.at(component) * sign,
    {
        let factor = sign as i128;
        let count = match self.count.checked_mul(factor) { Some(value) => value, None => return None };
        let quantity = match self.quantity.checked_mul(factor) { Some(value) => value, None => return None };
        let price = match self.price.checked_mul(factor) { Some(value) => value, None => return None };
        let discounted = match self.discounted.checked_mul(factor) { Some(value) => value, None => return None };
        let charge = match self.charge.checked_mul(factor) { Some(value) => value, None => return None };
        let discount = match self.discount.checked_mul(factor) { Some(value) => value, None => return None };
        Some(Self { count, quantity, price, discounted, charge, discount })
    }

    pub fn is_zero(&self) -> (result: bool)
        ensures result == (forall|component: int| #[trigger] self.at(component) == 0),
    {
        let result = self.count == 0 && self.quantity == 0 && self.price == 0
            && self.discounted == 0 && self.charge == 0 && self.discount == 0;
        proof {
            if !result {
                if self.count != 0 { assert(self.at(0) != 0); }
                else if self.quantity != 0 { assert(self.at(1) != 0); }
                else if self.price != 0 { assert(self.at(2) != 0); }
                else if self.discounted != 0 { assert(self.at(3) != 0); }
                else if self.charge != 0 { assert(self.at(4) != 0); }
                else { assert(self.at(5) != 0); }
            }
        }
        result
    }
}

pub fn contribution(row: &LineitemInput) -> (result: Option<Totals>)
    ensures result matches Some(value) ==> forall|component: int|
        #[trigger] value.at(component) == weight(lineitem_input_view(*row), component),
{
    let discounted = match (row.l_extendedprice as i128).checked_mul(100i128 - row.l_discount as i128) {
        Some(value) => value, None => return None,
    };
    let charge = match discounted.checked_mul(100i128 + row.l_tax as i128) {
        Some(value) => value, None => return None,
    };
    Some(Totals { count: 1, quantity: row.l_quantity as i128, price: row.l_extendedprice as i128,
        discounted, charge, discount: row.l_discount as i128 })
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
