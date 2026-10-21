#![verifier::loop_isolation(false)]
use vstd::prelude::*;
use std::sync::Arc;
use verus_common::bytes::*;
use verus_common::sums::*;
use crate::pages::*;
use verus_prooflib::wide::Wide;

verus! {

pub struct OrderVariant {
    pub customer: i64,
    pub date: i32,
    pub price: i64,
    pub count: Wide,
}

impl OrderVariant {
    pub fn dup(&self) -> (out: Self)
        ensures out == *self,
    { OrderVariant { customer: self.customer, date: self.date, price: self.price, count: self.count.dup() } }
}

pub struct OrderData {
    pub count: Wide,
    pub quantity: Wide,
    pub variants: Vec<OrderVariant>,
    pub slot: Option<usize>,
}

pub open spec fn variant_weight(customer: int, date: int, price: int) -> spec_fn(OrderVariant) -> int {
    |row: OrderVariant| if row.customer == customer && row.date == date && row.price == price { row.count@ } else { 0 }
}

impl OrderData {
    pub open spec fn bucket_quantity(self) -> i128 { self.quantity@ as i128 }

    pub fn bucket_key(&self) -> (out: i128)
        requires i128::MIN <= self.quantity@ <= i128::MAX,
        ensures out == self.bucket_quantity(), out as int == self.quantity@,
    { self.quantity.to_i128().unwrap() }

    pub proof fn variant_count(self, index: int)
        requires self.shape(), 0 <= index < self.variants.len(),
        ensures self.multiplicity(self.variants@[index].customer as int, self.variants@[index].date as int, self.variants@[index].price as int) == self.variants@[index].count@,
    {
        let variant = self.variants@[index];
        let weight = variant_weight(variant.customer as int, variant.date as int, variant.price as int);
        assert forall|position: int| 0 <= position < self.variants.len() && position != index implies weight(#[trigger] self.variants@[position]) == 0 by {}
        lemma_sum_single_match(self.variants@, weight, index);
    }
    pub open spec fn shape(self) -> bool {
        forall|left: int, right: int| 0 <= left < right < self.variants.len() ==>
            !(#[trigger] self.variants@[left].customer == #[trigger] self.variants@[right].customer &&
                self.variants@[left].date == self.variants@[right].date && self.variants@[left].price == self.variants@[right].price)
    }
    pub open spec fn multiplicity(self, customer: int, date: int, price: int) -> int {
        sum(self.variants@, variant_weight(customer, date, price))
    }

    pub open spec fn wf(self) -> bool {
        &&& self.count@ >= 0
        &&& (self.count@ == 0 ==> self.quantity@ == 0)
        &&& (self.count@ > 0 || self.variants.len() > 0)
        &&& forall|index: int| 0 <= index < self.variants.len() ==>
            #[trigger] self.variants@[index].count@ > 0
        &&& self.shape()
    }

    pub fn empty() -> (out: Self)
        ensures out.count@ == 0, out.quantity@ == 0, out.variants@ == Seq::<OrderVariant>::empty(), out.slot is None,
    { OrderData { count: Wide::zero(), quantity: Wide::zero(), variants: Vec::new(), slot: None } }

    pub fn copy(&self) -> (out: Self)
        ensures out.count == self.count, out.quantity == self.quantity,
            out.variants@ == self.variants@, out.slot == self.slot,
            out.shape() == self.shape(), out.wf() == self.wf(),
            forall|person: int, day: int, total: int| #[trigger] out.multiplicity(person, day, total) == self.multiplicity(person, day, total),
    {
        self.copy_room(0)
    }

    /// Copy with the variant vector sized exactly `len + extra` (Vec growth would
    /// otherwise allocate four 48-byte slots for the usual single variant).
    pub fn copy_room(&self, extra: usize) -> (out: Self)
        requires extra <= 1,
        ensures out.count == self.count, out.quantity == self.quantity,
            out.variants@ == self.variants@, out.slot == self.slot,
            out.shape() == self.shape(), out.wf() == self.wf(),
            forall|person: int, day: int, total: int| #[trigger] out.multiplicity(person, day, total) == self.multiplicity(person, day, total),
    {
        let room = if self.variants.len() < 1000000 { self.variants.len() + extra } else { self.variants.len() };
        let mut variants = Vec::with_capacity(room);
        let mut index = 0usize;
        while index < self.variants.len()
            invariant index <= self.variants.len(), variants@ == self.variants@.take(index as int),
            decreases self.variants.len() - index,
        {
            variants.push(self.variants[index].dup());
            index += 1;
        }
        proof { assert(self.variants@.take(self.variants.len() as int) =~= self.variants@); }
        OrderData { count: self.count.dup(), quantity: self.quantity.dup(), variants, slot: self.slot }
    }

    pub fn clean(&mut self)
        requires old(self).shape(), old(self).count@ >= 0, old(self).count@ == 0 ==> old(self).quantity@ == 0,
            forall|index: int| 0 <= index < old(self).variants.len() ==> #[trigger] old(self).variants@[index].count@ >= 0,
        ensures final(self).shape(), final(self).count == old(self).count,
            final(self).quantity == old(self).quantity, final(self).slot == old(self).slot,
            (final(self).count@ > 0 || final(self).variants.len() > 0) ==> final(self).wf(),
            final(self).count@ >= 0 && (final(self).count@ == 0 ==> final(self).quantity@ == 0),
            forall|person: int, day: int, total: int| #[trigger] final(self).multiplicity(person, day, total) ==
                old(self).multiplicity(person, day, total),
    {
        let mut index = 0usize;
        while index < self.variants.len()
            invariant index <= self.variants.len(), self.shape(),
                self.count == old(self).count, self.quantity == old(self).quantity, self.slot == old(self).slot,
                self.count@ >= 0, self.count@ == 0 ==> self.quantity@ == 0,
                forall|position: int| 0 <= position < index ==> #[trigger] self.variants@[position].count@ > 0,
                forall|position: int| index <= position < self.variants.len() ==> #[trigger] self.variants@[position].count@ >= 0,
                forall|person: int, day: int, total: int| #[trigger] self.multiplicity(person, day, total) ==
                    old(self).multiplicity(person, day, total),
            decreases self.variants.len() - index,
        {
            if self.variants[index].count.is_zero() {
                let ghost before = self.variants@;
                proof {
                    assert forall|person: int, day: int, total: int|
                        #[trigger] sum(self.variants@.remove(index as int), variant_weight(person, day, total)) ==
                        self.multiplicity(person, day, total) by {
                        lemma_sum_remove(self.variants@, index as int, variant_weight(person, day, total));
                    }
                }
                self.variants.remove(index);
                proof {
                    assert forall|person: int, day: int, total: int| #[trigger] self.multiplicity(person, day, total) ==
                        old(self).multiplicity(person, day, total) by {
                        lemma_sum_remove(before, index as int, variant_weight(person, day, total));
                    }
                }
            }
            else { index += 1; }
        }
    }
}

pub struct NameVariant { pub name: Arc<Vec<u8>>, pub count: Wide }
pub struct CustomerData { pub names: Vec<NameVariant> }

pub open spec fn name_weight(name: Seq<u8>) -> spec_fn(NameVariant) -> int {
    |row: NameVariant| if row.name@ == name { row.count@ } else { 0 }
}

impl CustomerData {
    pub proof fn name_count(self, index: int)
        requires self.shape(), 0 <= index < self.names.len(),
        ensures self.multiplicity(self.names@[index].name@) == self.names@[index].count@,
    {
        let weight = name_weight(self.names@[index].name@);
        assert forall|position: int| 0 <= position < self.names.len() && position != index implies weight(#[trigger] self.names@[position]) == 0 by {}
        lemma_sum_single_match(self.names@, weight, index);
    }
    pub open spec fn shape(self) -> bool {
        forall|left: int, right: int| 0 <= left < right < self.names.len() ==>
            #[trigger] self.names@[left].name@ != #[trigger] self.names@[right].name@
    }
    pub open spec fn multiplicity(self, name: Seq<u8>) -> int {
        sum(self.names@, name_weight(name))
    }

    pub open spec fn wf(self) -> bool {
        &&& self.names.len() > 0
        &&& forall|index: int| 0 <= index < self.names.len() ==> #[trigger] self.names@[index].count@ > 0
        &&& self.shape()
    }

    pub fn empty() -> (out: Self)
        ensures out.names@ == Seq::<NameVariant>::empty(),
    { CustomerData { names: Vec::with_capacity(1) } }

    pub fn copy(&self) -> (out: Self)
        ensures out.names@ == self.names@,
            out.shape() == self.shape(), out.wf() == self.wf(),
            forall|name: Seq<u8>| #[trigger] out.multiplicity(name) == self.multiplicity(name),
    {
        self.copy_room(0)
    }

    pub fn copy_room(&self, extra: usize) -> (out: Self)
        requires extra <= 1,
        ensures out.names@ == self.names@,
            out.shape() == self.shape(), out.wf() == self.wf(),
            forall|name: Seq<u8>| #[trigger] out.multiplicity(name) == self.multiplicity(name),
    {
        let room = if self.names.len() < 1000000 { self.names.len() + extra } else { self.names.len() };
        let mut names = Vec::with_capacity(room);
        let mut index = 0usize;
        while index < self.names.len()
            invariant index <= self.names.len(), names@ == self.names@.take(index as int),
            decreases self.names.len() - index,
        {
            names.push(NameVariant { name: Arc::clone(&self.names[index].name), count: self.names[index].count.dup() });
            index += 1;
        }
        proof { assert(self.names@.take(self.names.len() as int) =~= self.names@); }
        CustomerData { names }
    }

    pub fn clean(&mut self)
        requires old(self).shape(),
            forall|index: int| 0 <= index < old(self).names.len() ==> #[trigger] old(self).names@[index].count@ >= 0,
        ensures final(self).shape(), final(self).names.len() > 0 ==> final(self).wf(),
            forall|bytes: Seq<u8>| #[trigger] final(self).multiplicity(bytes) == old(self).multiplicity(bytes),
    {
        let mut index = 0usize;
        while index < self.names.len()
            invariant index <= self.names.len(), self.shape(),
                forall|position: int| 0 <= position < index ==> #[trigger] self.names@[position].count@ > 0,
                forall|position: int| index <= position < self.names.len() ==> #[trigger] self.names@[position].count@ >= 0,
                forall|bytes: Seq<u8>| #[trigger] self.multiplicity(bytes) == old(self).multiplicity(bytes),
            decreases self.names.len() - index,
        {
            if self.names[index].count.is_zero() {
                let ghost before = self.names@;
                proof {
                    assert forall|bytes: Seq<u8>|
                        #[trigger] sum(self.names@.remove(index as int), name_weight(bytes)) ==
                        self.multiplicity(bytes) by {
                        lemma_sum_remove(self.names@, index as int, name_weight(bytes));
                    }
                }
                self.names.remove(index);
                proof {
                    assert forall|bytes: Seq<u8>| #[trigger] self.multiplicity(bytes) == old(self).multiplicity(bytes) by {
                        lemma_sum_remove(before, index as int, name_weight(bytes));
                    }
                }
            }
            else { index += 1; }
        }
    }
}

pub fn copy_bytes(source: &Vec<u8>) -> (out: Vec<u8>)
    ensures out@ == source@,
{
    let mut out = Vec::new();
    let mut index = 0usize;
    while index < source.len()
        invariant index <= source.len(), out@ == source@.take(index as int),
        decreases source.len() - index,
    {
        out.push(source[index]);
        index += 1;
    }
    out
}

pub fn change_line(editor: &mut Editor<OrderData>, key: i64, quantity: i64, sign: i64)
    requires old(editor).wf(), forall|other: i128| #[trigger] order_shape(old(editor).at(other)),
    ensures final(editor).wf(),
        final(editor).at(key as i128) is Some,
        forall|other: i128| other != key ==> #[trigger] final(editor).at(other) == old(editor).at(other),
        sign > 0 && valid_orders(*old(editor)) ==> valid_orders(*final(editor)),
        forall|other: i128| #[trigger] order_shape(final(editor).at(other)),
        forall|other: i128| #[trigger] line_count(final(editor).at(other)) ==
            line_count(old(editor).at(other)) + if other == key { sign as int } else { 0 },
        forall|other: i128| #[trigger] line_sum(final(editor).at(other)) ==
            line_sum(old(editor).at(other)) + if other == key { quantity as int * sign as int } else { 0 },
        forall|other: i128, customer: int, date: int, price: int| #[trigger] order_count(final(editor).at(other), customer, date, price) ==
            order_count(old(editor).at(other), customer, date, price),
{
    proof { assert(order_shape(editor.at(key as i128))); }
    let mut value = match editor.get(key as i128) { Some(row) => row.copy(), None => OrderData::empty() };
    let change = Wide::from_i128(quantity as i128).mul(&Wide::from_i128(sign as i128));
    value.count = value.count.add(&Wide::from_i128(sign as i128));
    value.quantity = value.quantity.add(&change);
    editor.put(key as i128, Arc::new(value));
}

pub fn put_order_metadata(editor: &mut Editor<OrderData>, key: i128, value: OrderData)
    requires old(editor).wf(),
        forall|other: i128| #[trigger] order_shape(old(editor).at(other)),
        old(editor).at(key) matches Some(before) && value.count == before.count && value.quantity == before.quantity && value.variants@ == before.variants@,
    ensures final(editor).wf(),
        forall|other: i128| #[trigger] order_shape(final(editor).at(other)),
        valid_orders(*old(editor)) ==> valid_orders(*final(editor)),
        forall|other: i128| #[trigger] order_valid(final(editor).at(other), other) == order_valid(old(editor).at(other), other),
        final(editor).at(key) == Some(value),
        forall|other: i128| other != key ==> #[trigger] final(editor).at(other) == old(editor).at(other),
        forall|other: i128| #[trigger] line_count(final(editor).at(other)) == line_count(old(editor).at(other)),
        forall|other: i128| #[trigger] line_sum(final(editor).at(other)) == line_sum(old(editor).at(other)),
        forall|other: i128, customer: int, date: int, price: int| #[trigger] order_count(final(editor).at(other), customer, date, price) == order_count(old(editor).at(other), customer, date, price),
        forall|other: i128, quantity: i128| #[trigger] eligible_order(final(editor).at(other), quantity) == eligible_order(old(editor).at(other), quantity),
        forall|other: i128| #[trigger] order_fit(final(editor).at(other)) == order_fit(old(editor).at(other)),
{
    proof { assert(order_shape(editor.at(key))); }
    editor.put(key, Arc::new(value));
}

pub fn change_order(editor: &mut Editor<OrderData>, key: i64, customer: i64, date: i32, price: i64, sign: i64)
    requires old(editor).wf(), forall|other: i128| #[trigger] order_shape(old(editor).at(other)),
    ensures final(editor).wf(),
        final(editor).at(key as i128) is Some,
        forall|other: i128| other != key ==> #[trigger] final(editor).at(other) == old(editor).at(other),
        sign > 0 && valid_orders(*old(editor)) ==> valid_orders(*final(editor)),
        forall|other: i128| #[trigger] order_shape(final(editor).at(other)),
        forall|other: i128| #[trigger] line_count(final(editor).at(other)) == line_count(old(editor).at(other)),
        forall|other: i128| #[trigger] line_sum(final(editor).at(other)) == line_sum(old(editor).at(other)),
        forall|other: i128, person: int, day: int, total: int| #[trigger] order_count(final(editor).at(other), person, day, total) ==
            order_count(old(editor).at(other), person, day, total) +
                if other == key && person == customer && day == date && total == price { sign as int } else { 0 },
{
    proof { assert(order_shape(editor.at(key as i128))); }
    let mut value = match editor.get(key as i128) { Some(row) => row.copy_room(1), None => OrderData::empty() };
    proof {
        if valid_orders(*old(editor)) {
            if let Some(row) = old(editor).at(key as i128) { assert(row.wf()); }
        }
    }
    let ghost original = value.variants@;
    let mut index = 0usize;
    while index < value.variants.len()
        invariant index <= value.variants.len(), value.variants@ == original,
            value.shape(),
            valid_orders(*old(editor)) ==> value.count@ >= 0 && (value.count@ == 0 ==> value.quantity@ == 0),
            valid_orders(*old(editor)) ==> forall|position: int| 0 <= position < value.variants.len() ==>
                #[trigger] value.variants@[position].count@ > 0,
            forall|position: int| 0 <= position < index ==>
                !(#[trigger] value.variants@[position].customer == customer &&
                    value.variants@[position].date == date && value.variants@[position].price == price),
            *editor == *old(editor), editor.wf(),
            value.count@ == line_count(old(editor).at(key as i128)),
            value.quantity@ == line_sum(old(editor).at(key as i128)),
            forall|person: int, day: int, total: int| #[trigger] value.multiplicity(person, day, total) ==
                order_count(old(editor).at(key as i128), person, day, total),
        decreases value.variants.len() - index,
    {
        if value.variants[index].customer == customer && value.variants[index].date == date && value.variants[index].price == price {
            let count = value.variants[index].count.add(&Wide::from_i128(sign as i128));
            let updated = OrderVariant { customer, date, price, count };
            proof {
                assert forall|person: int, day: int, total: int| #[trigger] value.multiplicity(person, day, total) +
                    (if person == customer && day == date && total == price { sign as int } else { 0 }) ==
                    sum(original.update(index as int, updated), |variant: OrderVariant|
                        if variant.customer == person && variant.date == day && variant.price == total { variant.count@ } else { 0 }) by {
                    lemma_sum_update(original, index as int, updated, |variant: OrderVariant|
                        if variant.customer == person && variant.date == day && variant.price == total { variant.count@ } else { 0 });
                }
            }
            value.variants.set(index, updated);
            let ghost installed = value;
            editor.put(key as i128, Arc::new(value));
            proof {
                assert forall|other: i128, person: int, day: int, total: int| #[trigger] order_count(editor.at(other), person, day, total) ==
                    order_count(old(editor).at(other), person, day, total) +
                    (if other == key && person == customer && day == date && total == price { sign as int } else { 0 }) by {
                    if other == key {
                        lemma_sum_update(original, index as int, updated, |variant: OrderVariant|
                            if variant.customer == person && variant.date == day && variant.price == total { variant.count@ } else { 0 });
                        assert(editor.at(other) == Some(installed));
                    }
                }
            }
            return;
        }
        index += 1;
    }
    let added = OrderVariant { customer, date, price, count: Wide::from_i128(sign as i128) };
    proof {
        assert forall|person: int, day: int, total: int| #[trigger] value.multiplicity(person, day, total) +
            (if person == customer && day == date && total == price { sign as int } else { 0 }) ==
            sum(original.push(added), |variant: OrderVariant|
                if variant.customer == person && variant.date == day && variant.price == total { variant.count@ } else { 0 }) by {
            lemma_sum_push(original, added, |variant: OrderVariant|
                if variant.customer == person && variant.date == day && variant.price == total { variant.count@ } else { 0 });
        }
    }
    value.variants.push(added);
    let ghost installed = value;
    editor.put(key as i128, Arc::new(value));
    proof {
        assert forall|other: i128, person: int, day: int, total: int| #[trigger] order_count(editor.at(other), person, day, total) ==
            order_count(old(editor).at(other), person, day, total) +
            (if other == key && person == customer && day == date && total == price { sign as int } else { 0 }) by {
            if other == key {
                lemma_sum_push(original, added, |variant: OrderVariant|
                    if variant.customer == person && variant.date == day && variant.price == total { variant.count@ } else { 0 });
                assert(editor.at(other) == Some(installed));
            }
        }
    }
}

pub fn change_customer(editor: &mut Editor<CustomerData>, key: i64, name: &Vec<u8>, sign: i64)
    requires old(editor).wf(), forall|other: i128| #[trigger] customer_shape(old(editor).at(other)),
    ensures final(editor).wf(),
        final(editor).at(key as i128) is Some,
        forall|other: i128| other != key ==> #[trigger] final(editor).at(other) == old(editor).at(other),
        sign > 0 && valid_customers(*old(editor)) ==> valid_customers(*final(editor)),
        forall|other: i128| #[trigger] customer_shape(final(editor).at(other)),
        forall|other: i128, bytes: Seq<u8>| #[trigger] customer_count(final(editor).at(other), bytes) ==
            customer_count(old(editor).at(other), bytes) + if other == key && bytes == name@ { sign as int } else { 0 },
{
    proof { assert(customer_shape(editor.at(key as i128))); }
    let mut value = match editor.get(key as i128) { Some(row) => row.copy_room(1), None => CustomerData::empty() };
    proof {
        if valid_customers(*old(editor)) {
            if let Some(row) = old(editor).at(key as i128) { assert(row.wf()); }
        }
    }
    let ghost original = value.names@;
    let mut index = 0usize;
    while index < value.names.len()
        invariant index <= value.names.len(), value.names@ == original,
            value.shape(),
            valid_customers(*old(editor)) ==> forall|position: int| 0 <= position < value.names.len() ==>
                #[trigger] value.names@[position].count@ > 0,
            forall|position: int| 0 <= position < index ==> #[trigger] value.names@[position].name@ != name@,
            *editor == *old(editor), editor.wf(),
            forall|bytes: Seq<u8>| #[trigger] value.multiplicity(bytes) ==
                customer_count(old(editor).at(key as i128), bytes),
        decreases value.names.len() - index,
    {
        if bytes_eq_exec(&value.names[index].name, name) {
            let count = value.names[index].count.add(&Wide::from_i128(sign as i128));
            let shared_name = Arc::clone(&value.names[index].name);
            let updated = NameVariant { name: shared_name, count };
            proof {
                assert forall|bytes: Seq<u8>| #[trigger] value.multiplicity(bytes) +
                    (if bytes == name@ { sign as int } else { 0 }) ==
                    sum(original.update(index as int, updated), |variant: NameVariant|
                        if variant.name@ == bytes { variant.count@ } else { 0 }) by {
                    lemma_sum_update(original, index as int, updated, |variant: NameVariant|
                        if variant.name@ == bytes { variant.count@ } else { 0 });
                }
            }
            value.names.set(index, updated);
            let ghost installed = value;
            editor.put(key as i128, Arc::new(value));
            proof {
                assert forall|other: i128, bytes: Seq<u8>| #[trigger] customer_count(editor.at(other), bytes) ==
                    customer_count(old(editor).at(other), bytes) + (if other == key && bytes == name@ { sign as int } else { 0 }) by {
                    if other == key {
                        lemma_sum_update(original, index as int, updated, |variant: NameVariant|
                            if variant.name@ == bytes { variant.count@ } else { 0 });
                        assert(editor.at(other) == Some(installed));
                    }
                }
            }
            return;
        }
        index += 1;
    }
    let added = NameVariant { name: Arc::new(copy_bytes(name)), count: Wide::from_i128(sign as i128) };
    proof {
        assert forall|bytes: Seq<u8>| #[trigger] value.multiplicity(bytes) +
            (if bytes == name@ { sign as int } else { 0 }) ==
            sum(original.push(added), |variant: NameVariant| if variant.name@ == bytes { variant.count@ } else { 0 }) by {
            lemma_sum_push(original, added, |variant: NameVariant| if variant.name@ == bytes { variant.count@ } else { 0 });
        }
    }
    value.names.push(added);
    let ghost installed = value;
    editor.put(key as i128, Arc::new(value));
    proof {
        assert forall|other: i128, bytes: Seq<u8>| #[trigger] customer_count(editor.at(other), bytes) ==
            customer_count(old(editor).at(other), bytes) + (if other == key && bytes == name@ { sign as int } else { 0 }) by {
            if other == key {
                lemma_sum_push(original, added, |variant: NameVariant| if variant.name@ == bytes { variant.count@ } else { 0 });
                assert(editor.at(other) == Some(installed));
            }
        }
    }
}

pub open spec fn line_count(value: Option<OrderData>) -> int {
    match value { Some(row) => row.count@, None => 0 }
}

pub open spec fn eligible_order(value: Option<OrderData>, quantity: i128) -> bool {
    match value { Some(row) => row.count@ > 0 && row.variants.len() > 0 && row.quantity@ == quantity as int, None => false }
}

pub open spec fn order_fit(value: Option<OrderData>) -> bool {
    match value { Some(row) => row.count@ > 0 && row.variants.len() > 0 ==> i128::MIN <= row.quantity@ <= i128::MAX, None => true }
}

pub open spec fn order_shape(value: Option<OrderData>) -> bool {
    match value { Some(row) => row.shape(), None => true }
}

pub open spec fn valid_orders(editor: Editor<OrderData>) -> bool {
    forall|key: i128| #[trigger] editor.at(key) matches Some(row) ==> i64::MIN <= key <= i64::MAX && row.wf()
}

pub open spec fn order_valid(value: Option<OrderData>, key: i128) -> bool {
    match value { Some(row) => i64::MIN <= key <= i64::MAX && row.wf(), None => true }
}

pub open spec fn customer_valid(value: Option<CustomerData>, key: i128) -> bool {
    match value { Some(row) => i64::MIN <= key <= i64::MAX && row.wf(), None => true }
}

pub fn install_clean_order(editor: &mut Editor<OrderData>, key: i128, value: OrderData)
    requires old(editor).wf(), i64::MIN <= key <= i64::MAX,
        forall|other: i128| #[trigger] order_shape(old(editor).at(other)),
        old(editor).at(key) matches Some(before) && value.count == before.count && value.quantity == before.quantity,
        forall|person: int, day: int, total: int| #[trigger] value.multiplicity(person, day, total) == order_count(old(editor).at(key), person, day, total),
        value.shape(), value.count@ >= 0, value.count@ == 0 ==> value.quantity@ == 0,
        value.count@ > 0 || value.variants.len() > 0 ==> value.wf(),
    ensures final(editor).wf(), order_valid(final(editor).at(key), key),
        final(editor).at(key) == if value.count@ == 0 && value.variants.len() == 0 { None } else { Some(value) },
        forall|other: i128| #[trigger] order_shape(final(editor).at(other)),
        forall|other: i128| other != key ==> #[trigger] final(editor).at(other) == old(editor).at(other),
        forall|other: i128| #[trigger] line_count(final(editor).at(other)) == line_count(old(editor).at(other)),
        forall|other: i128| #[trigger] line_sum(final(editor).at(other)) == line_sum(old(editor).at(other)),
        forall|other: i128, person: int, day: int, total: int| #[trigger] order_count(final(editor).at(other), person, day, total) == order_count(old(editor).at(other), person, day, total),
{
    if value.count.is_zero() && value.variants.len() == 0 {
        proof {
            assert forall|person: int, day: int, total: int| #[trigger] value.multiplicity(person, day, total) == 0 by {
                lemma_sum_empty(value.variants@, variant_weight(person, day, total));
            }
        }
        editor.erase(key);
    } else { editor.put(key, Arc::new(value)); }
    proof {
        assert forall|other: i128, person: int, day: int, total: int| #[trigger] order_count(editor.at(other), person, day, total) == order_count(old(editor).at(other), person, day, total) by {
            if other == key {
                assert(value.multiplicity(person, day, total) == order_count(old(editor).at(key), person, day, total));
                if value.count@ == 0 && value.variants.len() == 0 { lemma_sum_empty(value.variants@, variant_weight(person, day, total)); }
            }
        }
    }
}

pub fn install_clean_customer(editor: &mut Editor<CustomerData>, key: i128, value: CustomerData)
    requires old(editor).wf(), i64::MIN <= key <= i64::MAX,
        forall|other: i128| #[trigger] customer_shape(old(editor).at(other)),
        forall|name: Seq<u8>| #[trigger] value.multiplicity(name) == customer_count(old(editor).at(key), name),
        value.shape(), value.names.len() > 0 ==> value.wf(),
    ensures final(editor).wf(), customer_valid(final(editor).at(key), key),
        final(editor).at(key) == if value.names.len() == 0 { None } else { Some(value) },
        forall|other: i128| #[trigger] customer_shape(final(editor).at(other)),
        forall|other: i128| other != key ==> #[trigger] final(editor).at(other) == old(editor).at(other),
        forall|other: i128, name: Seq<u8>| #[trigger] customer_count(final(editor).at(other), name) == customer_count(old(editor).at(other), name),
{
    if value.names.len() == 0 {
        proof {
            assert forall|name: Seq<u8>| #[trigger] value.multiplicity(name) == 0 by { lemma_sum_empty(value.names@, name_weight(name)); }
        }
        editor.erase(key);
    } else { editor.put(key, Arc::new(value)); }
    proof {
        assert forall|other: i128, name: Seq<u8>| #[trigger] customer_count(editor.at(other), name) == customer_count(old(editor).at(other), name) by {
            if other == key {
                assert(value.multiplicity(name) == customer_count(old(editor).at(key), name));
                if value.names.len() == 0 { lemma_sum_empty(value.names@, name_weight(name)); }
            }
        }
    }
}

pub open spec fn valid_customers(editor: Editor<CustomerData>) -> bool {
    forall|key: i128| #[trigger] editor.at(key) matches Some(row) ==> i64::MIN <= key <= i64::MAX && row.wf()
}

pub open spec fn customer_shape(value: Option<CustomerData>) -> bool {
    match value { Some(row) => row.shape(), None => true }
}

pub open spec fn line_sum(value: Option<OrderData>) -> int {
    match value { Some(row) => row.quantity@, None => 0 }
}

pub open spec fn order_count(value: Option<OrderData>, customer: int, date: int, price: int) -> int {
    match value { Some(row) => row.multiplicity(customer, date, price), None => 0 }
}

pub open spec fn customer_count(value: Option<CustomerData>, name: Seq<u8>) -> int {
    match value { Some(row) => row.multiplicity(name), None => 0 }
}

}
