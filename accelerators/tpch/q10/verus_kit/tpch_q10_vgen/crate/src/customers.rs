// The customer dimension: a persistent paged table with one entry per customer key.  An entry
// holds the distinct-or-equal row variants of that key (almost always exactly one) with their
// multiplicities.  The table is a bag: only sums over its variants are observable, so the
// invariants are stated for every weight function at once, like the scan and capture contracts.
use vstd::prelude::*;
use std::sync::Arc;
use verus_prooflib::ordered::{Key, KeyView, key_lt, compare_bytes};
use verus_common::bytes::*;
use verus_common::sums::sum;
use crate::gen_spec::CustomerRow;
use crate::gen_obligations::{CustomerInput, customer_input_view};
use crate::dimensions::{Customer, copy_customer};
use crate::paged::*;
use crate::dimension_query::lemma_customer_join_key;
use verus_prooflib::wide::Wide;

verus! {

pub struct CustEnt {
    pub custkey: i64,
    pub variants: Arc<Vec<Customer>>,
}

impl Ent for CustEnt {
    fn dup(&self) -> (result: Self) {
        CustEnt { custkey: self.custkey, variants: self.variants.clone() }
    }
    open spec fn kv(self) -> KeyView { (self.custkey as int, Seq::<u8>::empty()) }
    open spec fn top() -> KeyView { (i64::MAX as int, seq![0u8]) }
    fn key(&self) -> (result: Key) { Key::number(self.custkey) }
    fn top_key() -> (result: Key) {
        let mut bytes = Vec::new();
        bytes.push(0u8);
        Key::composite(i64::MAX, bytes)
    }
    fn before(&self, other: &Self) -> (result: bool) {
        proof { lemma_bytes_lt_irrefl(Seq::<u8>::empty()); }
        self.custkey < other.custkey
    }
    fn same_key(&self, other: &Self) -> (result: bool) { self.custkey == other.custkey }
    proof fn top_above(entry: Self) {
        assert(bytes_lt(Seq::<u8>::empty(), seq![0u8]));
    }
}

pub open spec fn variant_weight(weight: spec_fn(CustomerRow) -> int) -> spec_fn(Customer) -> int {
    |variant: Customer| variant.count@ * weight(customer_input_view(*variant.row))
}

pub open spec fn variants_sum(variants: Seq<Customer>, weight: spec_fn(CustomerRow) -> int) -> int {
    sum(variants, variant_weight(weight))
}

pub open spec fn entry_weight(weight: spec_fn(CustomerRow) -> int) -> spec_fn(CustEnt) -> int {
    |entry: CustEnt| variants_sum(entry.variants@, weight)
}

pub open spec fn table_sum(table: Paged<CustEnt>, weight: spec_fn(CustomerRow) -> int) -> int {
    sum(table.entries(), entry_weight(weight))
}

pub open spec fn variant_count() -> spec_fn(Customer) -> int {
    |variant: Customer| variant.count@
}

pub open spec fn variants_count(variants: Seq<Customer>) -> int {
    sum(variants, variant_count())
}

pub open spec fn variants_distinct(variants: Seq<Customer>) -> bool {
    forall|first: int, second: int| #![trigger variants[first], variants[second]] 0 <= first < second < variants.len() ==>
        customer_input_view(*variants[first].row) != customer_input_view(*variants[second].row)
}

pub open spec fn row_indicator(row: CustomerRow) -> spec_fn(CustomerRow) -> int {
    |other: CustomerRow| if other == row { 1int } else { 0int }
}

pub open spec fn entry_valid(entry: CustEnt) -> bool {
    &&& entry.variants@.len() > 0
    &&& variants_distinct(entry.variants@)
    &&& forall|index: int| #![trigger entry.variants@[index]] 0 <= index < entry.variants@.len() ==> {
        &&& entry.variants@[index].count@ > 0
        &&& customer_input_view(*entry.variants@[index].row).c_custkey == entry.custkey as int
    }
}

pub open spec fn table_valid(table: Paged<CustEnt>) -> bool {
    &&& table.wf()
    &&& forall|key: KeyView| #[trigger] table.at(key).is_some() ==> entry_valid(table.at(key).unwrap())
}

/// No customer key has more rows than `bound`.
pub open spec fn table_bounded(table: Paged<CustEnt>, bound: int) -> bool {
    &&& bound >= 0
    &&& forall|key: KeyView| #[trigger] table.at(key).is_some() ==> variants_count(table.at(key).unwrap().variants@) <= bound
}

pub proof fn lemma_entry_valid(table: Paged<CustEnt>, entry: CustEnt)
    requires table_valid(table), table.holds(entry),
    ensures entry_valid(entry), table.at(entry.kv()) == Some(entry),
{
    lemma_at(table, entry.kv(), entry);
    assert(table.at(entry.kv()).is_some());
}

pub open spec fn variants_at(table: Paged<CustEnt>, customer: int) -> Seq<Customer> {
    match table.at((customer, Seq::<u8>::empty())) {
        Some(entry) => entry.variants@,
        None => Seq::empty(),
    }
}

pub open spec fn no_customers(table: Paged<CustEnt>) -> bool {
    forall|key: KeyView| #[trigger] table.at(key).is_none()
}

pub fn same_row(left: &CustomerInput, right: &CustomerInput) -> (result: bool)
    ensures result == (customer_input_view(*left) == customer_input_view(*right)),
{
    left.c_custkey == right.c_custkey && left.c_nationkey == right.c_nationkey && left.c_acctbal == right.c_acctbal
        && compare_bytes(left.c_name.as_slice(), right.c_name.as_slice()) == 0
        && compare_bytes(left.c_address.as_slice(), right.c_address.as_slice()) == 0
        && compare_bytes(left.c_phone.as_slice(), right.c_phone.as_slice()) == 0
        && compare_bytes(left.c_comment.as_slice(), right.c_comment.as_slice()) == 0
}

pub fn find_row(variants: &Vec<Customer>, row: &CustomerInput) -> (result: Option<usize>)
    ensures result matches Some(index) ==> index < variants.len()
            && customer_input_view(*variants@[index as int].row) == customer_input_view(*row),
        result is None ==> forall|index: int| #![trigger variants@[index]] 0 <= index < variants.len()
            ==> customer_input_view(*variants@[index].row) != customer_input_view(*row),
{
    let mut index = 0usize;
    while index < variants.len()
        invariant index <= variants.len(),
            forall|other: int| #![trigger variants@[other]] 0 <= other < index
                ==> customer_input_view(*variants@[other].row) != customer_input_view(*row),
        decreases variants.len() - index,
    {
        if same_row(&*variants[index].row, row) { return Some(index); }
        index += 1;
    }
    None
}

pub fn dup_customer(variant: &Customer) -> (result: Customer)
    ensures result == *variant,
{
    Customer { row: variant.row.clone(), count: variant.count.dup() }
}

pub fn variants_recount(variants: &Vec<Customer>, position: usize, count: &Wide) -> (result: Vec<Customer>)
    requires position < variants.len(),
    ensures result@ == variants@.update(position as int, Customer { row: variants@[position as int].row, count: *count }),
{
    let mut result = Vec::with_capacity(variants.len());
    let mut index = 0usize;
    while index < variants.len()
        invariant index <= variants.len(), position < variants.len(),
            result@ == variants@.update(position as int, Customer { row: variants@[position as int].row, count: *count }).take(index as int),
        decreases variants.len() - index,
    {
        if index == position { result.push(Customer { row: variants[index].row.clone(), count: count.dup() }); }
        else { result.push(dup_customer(&variants[index])); }
        proof {
            let whole = variants@.update(position as int, Customer { row: variants@[position as int].row, count: *count });
            assert(whole.take(index as int + 1) =~= whole.take(index as int).push(whole[index as int]));
        }
        index += 1;
    }
    proof { assert(result@ =~= variants@.update(position as int, Customer { row: variants@[position as int].row, count: *count })); }
    result
}

pub fn variants_drop(variants: &Vec<Customer>, position: usize) -> (result: Vec<Customer>)
    requires position < variants.len(),
    ensures result@ == variants@.remove(position as int),
{
    let mut result = Vec::with_capacity(variants.len() - 1);
    let mut index = 0usize;
    while index < variants.len()
        invariant index <= variants.len(), position < variants.len(),
            result@ == variants@.remove(position as int).take(if index > position { index as int - 1 } else { index as int }),
        decreases variants.len() - index,
    {
        if index != position {
            result.push(dup_customer(&variants[index]));
        }
        proof {
            let whole = variants@.remove(position as int);
            if index < position {
                assert(whole.take(index as int + 1) =~= whole.take(index as int).push(whole[index as int]));
            } else if index > position {
                assert(whole.take(index as int) =~= whole.take(index as int - 1).push(whole[index as int - 1]));
            }
        }
        index += 1;
    }
    proof { assert(result@ =~= variants@.remove(position as int)); }
    result
}

pub fn variants_append(variants: &Vec<Customer>, variant: Customer) -> (result: Vec<Customer>)
    ensures result@ == variants@.push(variant),
{
    let mut result = Vec::new();
    let mut index = 0usize;
    while index < variants.len()
        invariant index <= variants.len(), result@ == variants@.take(index as int),
        decreases variants.len() - index,
    {
        result.push(dup_customer(&variants[index]));
        proof { assert(variants@.take(index as int + 1) =~= variants@.take(index as int).push(variants@[index as int])); }
        index += 1;
    }
    result.push(variant);
    proof { assert(variants@.take(variants.len() as int) =~= variants@); }
    result
}

// ---- sums over variant lists ------------------------------------------------------------------

pub proof fn lemma_recount_sum(old: Seq<Customer>, position: int, count: Wide, weight: spec_fn(CustomerRow) -> int)
    requires 0 <= position < old.len(),
    ensures
        variants_sum(old.update(position, Customer { row: old[position].row, count }), weight)
            == variants_sum(old, weight) + (count@ - old[position].count@) * weight(customer_input_view(*old[position].row)),
        variants_count(old.update(position, Customer { row: old[position].row, count }))
            == variants_count(old) + count@ - old[position].count@,
{
    let updated = Customer { row: old[position].row, count };
    verus_common::sums::lemma_sum_update(old, position, updated, variant_weight(weight));
    verus_common::sums::lemma_sum_update(old, position, updated, variant_count());
    let w = weight(customer_input_view(*old[position].row));
    assert(count@ * w - old[position].count@ * w == (count@ - old[position].count@) * w) by (nonlinear_arith);
}

pub proof fn lemma_drop_sum(old: Seq<Customer>, position: int, weight: spec_fn(CustomerRow) -> int)
    requires 0 <= position < old.len(),
    ensures
        variants_sum(old.remove(position), weight) == variants_sum(old, weight) - old[position].count@ * weight(customer_input_view(*old[position].row)),
        variants_count(old.remove(position)) == variants_count(old) - old[position].count@,
{
    verus_common::sums::lemma_sum_remove(old, position, variant_weight(weight));
    verus_common::sums::lemma_sum_remove(old, position, variant_count());
}

pub proof fn lemma_append_sum(old: Seq<Customer>, variant: Customer, weight: spec_fn(CustomerRow) -> int)
    ensures
        variants_sum(old.push(variant), weight) == variants_sum(old, weight) + variant.count@ * weight(customer_input_view(*variant.row)),
        variants_count(old.push(variant)) == variants_count(old) + variant.count@,
{
    verus_common::sums::lemma_sum_push(old, variant, variant_weight(weight));
    verus_common::sums::lemma_sum_push(old, variant, variant_count());
}

// ---- editing ------------------------------------------------------------------------------

/// After a put or remove of one entry: the bag sum changes by the entry's contribution.
proof fn lemma_table_put(old: Paged<CustEnt>, new: Paged<CustEnt>, entry: CustEnt, weight: spec_fn(CustomerRow) -> int)
    requires old.wf(), new.wf(),
        forall|other: CustEnt| #![trigger new.holds(other)] new.holds(other) <==> (other == entry || (old.holds(other) && other.kv() != entry.kv())),
    ensures table_sum(new, weight) == table_sum(old, weight) + variants_sum(entry.variants@, weight)
        - match old.at(entry.kv()) { Some(previous) => variants_sum(previous.variants@, weight), None => 0int },
{
    lemma_sum_put(old, new, entry, entry_weight(weight));
    match old.at(entry.kv()) { Some(previous) => {}, None => {} }
}

proof fn lemma_table_remove(old: Paged<CustEnt>, new: Paged<CustEnt>, key: KeyView, weight: spec_fn(CustomerRow) -> int)
    requires old.wf(), new.wf(),
        forall|other: CustEnt| #![trigger new.holds(other)] new.holds(other) <==> (old.holds(other) && other.kv() != key),
    ensures table_sum(new, weight) == table_sum(old, weight)
        - match old.at(key) { Some(previous) => variants_sum(previous.variants@, weight), None => 0int },
{
    lemma_sum_remove(old, new, key, entry_weight(weight));
    match old.at(key) { Some(previous) => {}, None => {} }
}

/// A non-empty variant list with positive counts of one key is a valid entry.
pub open spec fn variants_valid(variants: Seq<Customer>, custkey: int) -> bool {
    &&& variants.len() > 0
    &&& variants_distinct(variants)
    &&& forall|index: int| #![trigger variants[index]] 0 <= index < variants.len() ==> {
        &&& variants[index].count@ > 0
        &&& customer_input_view(*variants[index].row).c_custkey == custkey
    }
}

/// Install `variants` as the entry of `custkey` (removing the entry when empty), keeping the table valid.
fn store_variants(table: &mut Paged<CustEnt>, custkey: i64, variants: Vec<Customer>)
    requires table_valid(*old(table)), variants.len() == 0 || variants_valid(variants@, custkey as int),
    ensures table_valid(*final(table)),
        variants_at(*final(table), custkey as int) == variants@,
        forall|key: KeyView| key != (custkey as int, Seq::<u8>::empty()) ==> final(table).at(key) == old(table).at(key),
        forall|weight: spec_fn(CustomerRow) -> int| #[trigger] table_sum(*final(table), weight)
            == table_sum(*old(table), weight) + variants_sum(variants@, weight) - variants_sum(variants_at(*old(table), custkey as int), weight),
{
    let ghost before = *table;
    let ghost ckey = (custkey as int, Seq::<u8>::empty());
    let probe = CustEnt { custkey, variants: Arc::new(Vec::new()) };
    if variants.len() == 0 {
        table.remove(&probe);
        proof {
            assert forall|weight: spec_fn(CustomerRow) -> int| #[trigger] table_sum(*table, weight)
                == table_sum(before, weight) + variants_sum(variants@, weight) - variants_sum(variants_at(before, custkey as int), weight) by {
                lemma_table_remove(before, *table, ckey, weight);
                match before.at(ckey) { Some(previous) => {}, None => {} }
            }
            assert(variants_at(*table, custkey as int) == Seq::<Customer>::empty());
        }
    } else {
        let entry = CustEnt { custkey, variants: Arc::new(variants) };
        let ghost kept = entry;
        table.put(entry);
        proof {
            assert(kept.kv() == ckey);
            assert forall|weight: spec_fn(CustomerRow) -> int| #[trigger] table_sum(*table, weight)
                == table_sum(before, weight) + variants_sum(variants@, weight) - variants_sum(variants_at(before, custkey as int), weight) by {
                lemma_table_put(before, *table, kept, weight);
                match before.at(ckey) { Some(previous) => {}, None => {} }
            }
            assert(table.at(ckey) == Some(kept));
            assert(entry_valid(kept));
        }
    }
}

pub open spec fn variant_row_at(variants: Seq<Customer>, index: int) -> CustomerRow {
    customer_input_view(*variants[index].row)
}

pub proof fn lemma_row_indicator_sum(variants: Seq<Customer>, index: int)
    requires 0 <= index < variants.len(), variants_distinct(variants),
    ensures variants_sum(variants, row_indicator(customer_input_view(*variants[index].row))) == variants[index].count@,
{
    let row = variant_row_at(variants, index);
    let weight = variant_weight(row_indicator(row));
    assert forall|other: int| 0 <= other < variants.len() && other != index implies weight(#[trigger] variants[other]) == 0 by {
        if other < index { assert(customer_input_view(*variants[other].row) != customer_input_view(*variants[index].row)); }
        else { assert(customer_input_view(*variants[index].row) != customer_input_view(*variants[other].row)); }
    }
    verus_common::sums::lemma_sum_single_match(variants, weight, index);
    assert(weight(variants[index]) == variants[index].count@ * 1) by {}
}

pub proof fn lemma_row_indicator_absent(variants: Seq<Customer>, row: CustomerRow)
    requires forall|index: int| #![trigger variants[index]] 0 <= index < variants.len() ==> customer_input_view(*variants[index].row) != row,
    ensures variants_sum(variants, row_indicator(row)) == 0,
{
    let weight = variant_weight(row_indicator(row));
    assert forall|index: int| 0 <= index < variants.len() implies weight(#[trigger] variants[index]) == 0 by {}
    verus_common::sums::lemma_sum_zero(variants, weight);
}

pub fn edit_table(table: &mut Paged<CustEnt>, shared: &Arc<CustomerInput>, change: &Wide)
    requires table_valid(*old(table)),
        table_sum(*old(table), row_indicator(customer_input_view(**shared))) + change@ >= 0,
    ensures table_valid(*final(table)),
        forall|weight: spec_fn(CustomerRow) -> int| #[trigger] table_sum(*final(table), weight)
            == table_sum(*old(table), weight) + change@ * weight(customer_input_view(**shared)),
        forall|key: KeyView| key != (shared.c_custkey as int, Seq::<u8>::empty()) ==> final(table).at(key) == old(table).at(key),
{
    let row: &CustomerInput = &**shared;
    let ghost before = *table;
    let ghost ckey = (row.c_custkey as int, Seq::<u8>::empty());
    let probe = CustEnt { custkey: row.c_custkey, variants: Arc::new(Vec::new()) };
    proof {
        assert(probe.kv() == ckey);
        assert forall|other: CustomerRow| other.c_custkey != row.c_custkey as int implies #[trigger] row_indicator(customer_input_view(*row))(other) == 0 by {}
        lemma_customer_join_key(before, row.c_custkey as int, row_indicator(customer_input_view(*row)));
    }
    let existing = table.get(&probe);
    match existing {
        None => {
            proof {
                assert(variants_at(before, row.c_custkey as int) == Seq::<Customer>::empty());
                assert(table_sum(before, row_indicator(customer_input_view(*row))) == 0);
            }
            if change.is_zero() { return; }
            let mut variants = Vec::new();
            let added = Customer { row: shared.clone(), count: change.dup() };
            variants.push(added);
            proof {
                assert(variants@ =~= seq![added]);
            }
            store_variants(table, row.c_custkey, variants);
            proof {
                assert forall|weight: spec_fn(CustomerRow) -> int| #[trigger] table_sum(*table, weight)
                    == table_sum(before, weight) + change@ * weight(customer_input_view(*row)) by {
                    verus_common::sums::lemma_sum_singleton(added, variant_weight(weight));
                }
            }
        },
        Some(entry) => {
            let old_variants = &*entry.variants;
            proof {
                assert(entry_valid(entry));
                assert(variants_at(before, row.c_custkey as int) == old_variants@);
            }
            match find_row(old_variants, row) {
                Some(index) => {
                    let current = old_variants[index].count.dup();
                    let new_count = current.add(change);
                    proof {
                        lemma_row_indicator_sum(old_variants@, index as int);
                        assert(customer_input_view(*old_variants@[index as int].row) == customer_input_view(*row));
                        assert(new_count@ >= 0);
                    }
                    if new_count.is_zero() {
                        let rest = variants_drop(old_variants, index);
                        proof {
                            lemma_drop_sum(old_variants@, index as int, weight_unit());
                            assert(rest@.len() == 0 || variants_valid(rest@, row.c_custkey as int)) by {
                                if rest@.len() > 0 {
                                    assert forall|j: int| #![trigger rest@[j]] 0 <= j < rest@.len() implies {
                                        &&& rest@[j].count@ > 0
                                        &&& customer_input_view(*rest@[j].row).c_custkey == row.c_custkey as int
                                    } by {
                                        assert(rest@[j] == old_variants@.remove(index as int)[j]);
                                        if j < index as int { assert(old_variants@.remove(index as int)[j] == old_variants@[j]); }
                                        else { assert(old_variants@.remove(index as int)[j] == old_variants@[j + 1]); }
                                    }
                                    assert forall|first: int, second: int| #![trigger rest@[first], rest@[second]] 0 <= first < second < rest@.len() implies
                                        customer_input_view(*rest@[first].row) != customer_input_view(*rest@[second].row) by {
                                        let source_first = if first < index as int { first } else { first + 1 };
                                        let source_second = if second < index as int { second } else { second + 1 };
                                        assert(rest@[first] == old_variants@[source_first]);
                                        assert(rest@[second] == old_variants@[source_second]);
                                    }
                                }
                            }
                        }
                        store_variants(table, row.c_custkey, rest);
                        proof {
                            assert forall|weight: spec_fn(CustomerRow) -> int| #[trigger] table_sum(*table, weight)
                                == table_sum(before, weight) + change@ * weight(customer_input_view(*row)) by {
                                lemma_drop_sum(old_variants@, index as int, weight);
                                assert(customer_input_view(*old_variants@[index as int].row) == customer_input_view(*row));
                                let w = weight(customer_input_view(*row));
                                assert(current@ + change@ == 0);
                                assert(change@ * w == -(current@ * w)) by (nonlinear_arith)
                                    requires current@ + change@ == 0;
                            }
                        }
                    } else {
                        let updated = variants_recount(old_variants, index, &new_count);
                        proof {
                            assert(updated@.len() == old_variants@.len());
                            assert(variants_valid(updated@, row.c_custkey as int)) by {
                                assert forall|j: int| #![trigger updated@[j]] 0 <= j < updated@.len() implies {
                                    &&& updated@[j].count@ > 0
                                    &&& customer_input_view(*updated@[j].row).c_custkey == row.c_custkey as int
                                } by {
                                    if j == index as int {
                                        assert(updated@[j].row == old_variants@[j].row);
                                    } else {
                                        assert(updated@[j] == old_variants@[j]);
                                    }
                                }
                                assert forall|first: int, second: int| #![trigger updated@[first], updated@[second]] 0 <= first < second < updated@.len() implies
                                    customer_input_view(*updated@[first].row) != customer_input_view(*updated@[second].row) by {
                                    assert(updated@[first].row == old_variants@[first].row);
                                    assert(updated@[second].row == old_variants@[second].row);
                                }
                            }
                        }
                        store_variants(table, row.c_custkey, updated);
                        proof {
                            assert forall|weight: spec_fn(CustomerRow) -> int| #[trigger] table_sum(*table, weight)
                                == table_sum(before, weight) + change@ * weight(customer_input_view(*row)) by {
                                lemma_recount_sum(old_variants@, index as int, new_count, weight);
                                assert(customer_input_view(*old_variants@[index as int].row) == customer_input_view(*row));
                            }
                        }
                    }
                },
                None => {
                    proof {
                        lemma_row_indicator_absent(old_variants@, customer_input_view(*row));
                        assert(change@ >= 0);
                    }
                    if change.is_zero() { return; }
                    let added = Customer { row: shared.clone(), count: change.dup() };
                    let appended = variants_append(old_variants, added);
                    proof {
                        assert(variants_valid(appended@, row.c_custkey as int)) by {
                            assert forall|j: int| #![trigger appended@[j]] 0 <= j < appended@.len() implies {
                                &&& appended@[j].count@ > 0
                                &&& customer_input_view(*appended@[j].row).c_custkey == row.c_custkey as int
                            } by {
                                if j < old_variants@.len() as int { assert(appended@[j] == old_variants@[j]); }
                                else { assert(appended@[j] == added); }
                            }
                            assert forall|first: int, second: int| #![trigger appended@[first], appended@[second]] 0 <= first < second < appended@.len() implies
                                customer_input_view(*appended@[first].row) != customer_input_view(*appended@[second].row) by {
                                if second < old_variants@.len() as int { assert(appended@[first] == old_variants@[first]); assert(appended@[second] == old_variants@[second]); }
                                else { assert(appended@[second] == added); assert(appended@[first] == old_variants@[first]); }
                            }
                        }
                    }
                    store_variants(table, row.c_custkey, appended);
                    proof {
                        assert forall|weight: spec_fn(CustomerRow) -> int| #[trigger] table_sum(*table, weight)
                            == table_sum(before, weight) + change@ * weight(customer_input_view(*row)) by {
                            lemma_append_sum(old_variants@, added, weight);
                        }
                    }
                },
            }
        },
    }
}

pub open spec fn weight_unit() -> spec_fn(CustomerRow) -> int { |row: CustomerRow| 1int }

fn count_variants(variants: &Vec<Customer>) -> (result: Wide)
    requires forall|index: int| #![trigger variants@[index]] 0 <= index < variants.len() ==> variants@[index].count@ > 0,
    ensures result@ == variants_count(variants@) && result@ >= 0,
{
    let mut total = Wide::zero();
    let mut index = 0usize;
    while index < variants.len()
        invariant index <= variants.len(), total@ == variants_count(variants@.take(index as int)), total@ >= 0,
            forall|other: int| #![trigger variants@[other]] 0 <= other < variants.len() ==> variants@[other].count@ > 0,
        decreases variants.len() - index,
    {
        proof { verus_common::sums::lemma_sum_take(variants@, index as int, variant_count()); }
        total = total.add(&variants[index].count);
        index += 1;
    }
    proof { assert(variants@.take(variants.len() as int) =~= variants@); }
    total
}

pub fn custkey_total(table: &Paged<CustEnt>, custkey: i64) -> (result: Wide)
    requires table_valid(*table),
    ensures result@ == variants_count(variants_at(*table, custkey as int)) && result@ >= 0,
{
    let probe = CustEnt { custkey, variants: Arc::new(Vec::new()) };
    proof { assert(probe.kv() == (custkey as int, Seq::<u8>::empty())); }
    match table.get(&probe) {
        None => Wide::zero(),
        Some(entry) => {
            proof { assert(entry_valid(entry)); }
            count_variants(&*entry.variants)
        },
    }
}

}
