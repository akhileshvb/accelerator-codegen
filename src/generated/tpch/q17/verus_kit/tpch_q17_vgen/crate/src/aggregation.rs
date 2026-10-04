use vstd::prelude::*;
use std::collections::HashMap;
use verus_common::sums::*;
use crate::gen_obligations::*;
use crate::gen_spec::*;
use crate::model::*;

verus! {

broadcast use vstd::seq_lib::group_seq_properties;

#[derive(Clone, Copy)]
pub struct Total { pub count: i128, pub quantity: i128 }

pub open spec fn total_at(totals: Map<i64, Total>, key: i64) -> Total {
    if totals.contains_key(key) { totals[key] } else { Total { count: 0, quantity: 0 } }
}

pub open spec fn accumulated(moments: Map<i64, Moment>, key: i64) -> Moment {
    if moments.contains_key(key) { moments[key] } else { Moment { count: 0, price: 0 } }
}

#[verifier::opaque]
pub open spec fn prefix_count(rows: Seq<LineInput>, prefix: int, key: i64) -> int {
    sum(line_input_seq_view(rows.take(prefix)), |row: LineitemRow|
        ind(row.l_partkey == key as int && qualifies(line_input_seq_view(rows), row)))
}

#[verifier::opaque]
pub open spec fn prefix_price(rows: Seq<LineInput>, prefix: int, key: i64) -> int {
    sum(line_input_seq_view(rows.take(prefix)), |row: LineitemRow|
        if row.l_partkey == key as int && qualifies(line_input_seq_view(rows), row) { row.l_extendedprice } else { 0 })
}

pub proof fn lemma_line_prefix(rows: Seq<LineInput>, index: int, key: i64)
    requires 0 <= index < rows.len(),
    ensures number(line_input_seq_view(rows.take(index + 1)), key as int)
            == number(line_input_seq_view(rows.take(index)), key as int) + ind(rows[index].l_partkey == key),
        quantity(line_input_seq_view(rows.take(index + 1)), key as int)
            == quantity(line_input_seq_view(rows.take(index)), key as int) + if rows[index].l_partkey == key { rows[index].l_quantity as int } else { 0 },
        prefix_count(rows, index + 1, key) == prefix_count(rows, index, key)
            + ind(rows[index].l_partkey == key && qualifies(line_input_seq_view(rows), line_input_view(rows[index]))),
        prefix_price(rows, index + 1, key) == prefix_price(rows, index, key)
            + if rows[index].l_partkey == key && qualifies(line_input_seq_view(rows), line_input_view(rows[index])) { rows[index].l_extendedprice as int } else { 0 },
{
    let prefix = line_input_seq_view(rows.take(index));
    let row = line_input_view(rows[index]);
    assert(line_input_seq_view(rows.take(index + 1)) =~= prefix.push(row));
    reveal(number);
    reveal(quantity);
    reveal(prefix_count);
    reveal(prefix_price);
    lemma_sum_push(prefix, row, |row: LineitemRow| ind(row.l_partkey == key as int));
    lemma_sum_push(prefix, row, |row: LineitemRow| if row.l_partkey == key as int { row.l_quantity } else { 0int });
    lemma_sum_push(prefix, row, |row: LineitemRow| ind(row.l_partkey == key as int && qualifies(line_input_seq_view(rows), row)));
    lemma_sum_push(prefix, row, |row: LineitemRow| if row.l_partkey == key as int && qualifies(line_input_seq_view(rows), row) { row.l_extendedprice } else { 0int });
}

pub proof fn lemma_line_empty(rows: Seq<LineInput>, key: i64)
    ensures number(line_input_seq_view(rows.take(0)), key as int) == 0,
        quantity(line_input_seq_view(rows.take(0)), key as int) == 0,
        prefix_count(rows, 0, key) == 0, prefix_price(rows, 0, key) == 0,
{
    assert(line_input_seq_view(rows.take(0)) =~= Seq::<LineitemRow>::empty());
    reveal(number);
    reveal(quantity);
    reveal(prefix_count);
    reveal(prefix_price);
}

pub fn equal_bytes(left: &[u8], right: &[u8]) -> (result: bool)
    ensures result == (left@ == right@),
{
    if left.len() != right.len() { return false; }
    let mut index = 0;
    while index < left.len()
        invariant index <= left.len(), left.len() == right.len(),
            forall|position: int| #![trigger left@[position]] 0 <= position < index ==> left@[position] == right@[position],
        decreases left.len() - index,
    {
        if left[index] != right[index] { return false; }
        index += 1;
    }
    proof { assert(left@ =~= right@); }
    true
}

pub fn copy_bytes(bytes: &[u8]) -> (result: Vec<u8>)
    ensures result@ == bytes@,
{
    let mut result = Vec::new();
    let mut index = 0;
    while index < bytes.len()
        invariant index <= bytes.len(), result@ == bytes@.take(index as int),
        decreases bytes.len() - index,
    {
        result.push(bytes[index]);
        index += 1;
    }
    result
}

pub fn hash_sequence(bytes: &[u8], seed: i64) -> (result: i64)
    requires 0 <= seed < 1000000007,
    ensures result as int == hash_bytes(bytes@, seed as int), 0 <= result < 1000000007,
{
    proof { reveal(hash_bytes); assert(bytes@.take(0) =~= Seq::<u8>::empty()); }
    let mut value = seed;
    let mut index = 0;
    while index < bytes.len()
        invariant index <= bytes.len(), 0 <= value < 1000000007,
            value as int == hash_bytes(bytes@.take(index as int), seed as int),
        decreases bytes.len() - index,
    {
        proof {
            reveal(hash_bytes);
            assert(bytes@.take(index as int + 1).drop_last() =~= bytes@.take(index as int));
            assert(bytes@.take(index as int + 1).last() == bytes@[index as int]);
        }
        value = (value * 257 + bytes[index] as i64 + 1) % 1000000007;
        index += 1;
    }
    proof { assert(bytes@.take(bytes@.len() as int) =~= bytes@); }
    value
}

pub fn fingerprint(brand: &[u8], container: &[u8]) -> (result: i64)
    ensures result as int == pair_hash(brand@, container@),
{
    let first = hash_sequence(brand, 0);
    hash_sequence(container, (first * 257 + 256) % 1000000007)
}

pub fn identity_matches(identity: &Identity, brand: &[u8], container: &[u8]) -> (result: bool)
    ensures result == same(*identity, brand@, container@),
{
    equal_bytes(&identity.brand, brand) && equal_bytes(&identity.container, container)
}

pub fn zero() -> (result: Moment)
    ensures result.count == 0, result.price == 0,
{ Moment { count: 0, price: 0 } }

pub fn qualified(quantity: i64, total: Total) -> (result: Option<bool>)
    requires total.count > 0,
    ensures result matches Some(value) ==> value == (5 * (total.count as int) * (quantity as int) < total.quantity as int),
{
    let factor = match total.count.checked_mul(5) { Some(value) => value, None => return None };
    let left = match factor.checked_mul(quantity as i128) { Some(value) => value, None => return None };
    Some(left < total.quantity)
}

pub fn summarize(rows: &[LineInput]) -> (result: Option<(HashMap<i64, Moment>, Vec<i64>)>)
    ensures result matches Some((moments, keys)) ==> {
        &&& keys@.no_duplicates()
        &&& moments@.dom().subset_of(keys@.to_set())
        &&& forall|index: int| #![trigger rows@[index]] 0 <= index < rows@.len() ==> keys@.contains(rows@[index].l_partkey)
        &&& forall|key: i64| #![trigger moments@[key]] moments@.contains_key(key) ==> moments@[key].count > 0
        &&& forall|key: i64| #[trigger] accumulated(moments@, key).count as int == line_count(line_input_seq_view(rows@), key as int)
        &&& forall|key: i64| #[trigger] accumulated(moments@, key).price as int == line_price(line_input_seq_view(rows@), key as int)
    },
{
    let mut totals: HashMap<i64, Total> = HashMap::new();
    let mut keys = Vec::new();
    let mut index = 0;
    let mut processed = Ghost(0int);
    proof {
        assert forall|key: i64| #[trigger] total_at(totals@, key).count as int == number(line_input_seq_view(rows@.take(0)), key as int) by { lemma_line_empty(rows@, key); }
        assert forall|key: i64| #[trigger] total_at(totals@, key).quantity as int == quantity(line_input_seq_view(rows@.take(0)), key as int) by { lemma_line_empty(rows@, key); }
    }
    while index < rows.len()
        invariant index <= rows.len(), processed@ == index as int,
            keys@.no_duplicates(), totals@.dom() == keys@.to_set(),
            forall|position: int| #![trigger rows@[position]] 0 <= position < index ==> totals@.contains_key(rows@[position].l_partkey),
            forall|key: i64| #![trigger totals@[key]] totals@.contains_key(key) ==> totals@[key].count > 0,
            forall|key: i64| #[trigger] total_at(totals@, key).count as int == number(line_input_seq_view(rows@.take(processed@)), key as int),
            forall|key: i64| #[trigger] total_at(totals@, key).quantity as int == quantity(line_input_seq_view(rows@.take(processed@)), key as int),
        decreases rows.len() - index,
    {
        let row = &rows[index];
        let before = Ghost(totals@);
        let previous = match totals.get(&row.l_partkey) {
            Some(value) => *value,
            None => {
                proof { keys@.to_set_ensures(); verus_prooflib::lemma_unique_push(keys@, row.l_partkey); }
                keys.push(row.l_partkey);
                Total { count: 0, quantity: 0 }
            },
        };
        proof { assert(previous == total_at(before@, row.l_partkey)); }
        let count = match previous.count.checked_add(1) { Some(value) => value, None => return None };
        let quantity_sum = match previous.quantity.checked_add(row.l_quantity as i128) { Some(value) => value, None => return None };
        totals.insert(row.l_partkey, Total { count, quantity: quantity_sum });
        proof {
            keys@.to_set_ensures();
            assert forall|key: i64| #[trigger] total_at(totals@, key).count as int
                == number(line_input_seq_view(rows@.take(index as int + 1)), key as int) by {
                assert(total_at(before@, key).count as int == number(line_input_seq_view(rows@.take(index as int)), key as int));
                lemma_line_prefix(rows@, index as int, key);
            }
            assert forall|key: i64| #[trigger] total_at(totals@, key).quantity as int
                == quantity(line_input_seq_view(rows@.take(index as int + 1)), key as int) by {
                assert(total_at(before@, key).quantity as int == quantity(line_input_seq_view(rows@.take(index as int)), key as int));
                lemma_line_prefix(rows@, index as int, key);
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    let mut moments: HashMap<i64, Moment> = HashMap::new();
    proof {
        assert(rows@.take(rows@.len() as int) =~= rows@);
        assert forall|key: i64| #[trigger] accumulated(moments@, key).count as int == prefix_count(rows@, 0, key) by { lemma_line_empty(rows@, key); }
        assert forall|key: i64| #[trigger] accumulated(moments@, key).price as int == prefix_price(rows@, 0, key) by { lemma_line_empty(rows@, key); }
    }
    index = 0;
    processed = Ghost(0int);
    while index < rows.len()
        invariant index <= rows.len(), processed@ == index as int,
            keys@.no_duplicates(), totals@.dom() == keys@.to_set(),
            moments@.dom().subset_of(keys@.to_set()),
            forall|key: i64| #[trigger] total_at(totals@, key).count as int == number(line_input_seq_view(rows@), key as int),
            forall|key: i64| #[trigger] total_at(totals@, key).quantity as int == quantity(line_input_seq_view(rows@), key as int),
            forall|position: int| #![trigger rows@[position]] 0 <= position < rows.len() ==> totals@.contains_key(rows@[position].l_partkey),
            forall|key: i64| #![trigger totals@[key]] totals@.contains_key(key) ==> totals@[key].count > 0,
            forall|key: i64| #![trigger moments@[key]] moments@.contains_key(key) ==> moments@[key].count > 0,
            forall|key: i64| #[trigger] accumulated(moments@, key).count as int == prefix_count(rows@, processed@, key),
            forall|key: i64| #[trigger] accumulated(moments@, key).price as int == prefix_price(rows@, processed@, key),
        decreases rows.len() - index,
    {
        let row = &rows[index];
        let before = Ghost(moments@);
        let total = match totals.get(&row.l_partkey) { Some(value) => *value, None => return None };
        proof {
            assert(total == total_at(totals@, row.l_partkey));
            assert(total.count as int == number(line_input_seq_view(rows@), row.l_partkey as int));
            assert(total.quantity as int == quantity(line_input_seq_view(rows@), row.l_partkey as int));
        }
        let accepts = match qualified(row.l_quantity, total) { Some(value) => value, None => return None };
        proof { reveal(qualifies); assert(accepts == qualifies(line_input_seq_view(rows@), line_input_view(*row))); }
        if accepts {
            let previous = match moments.get(&row.l_partkey) { Some(value) => *value, None => zero() };
            let count = match previous.count.checked_add(1) { Some(value) => value, None => return None };
            let price = match previous.price.checked_add(row.l_extendedprice as i128) { Some(value) => value, None => return None };
            moments.insert(row.l_partkey, Moment { count, price });
        }
        proof {
            assert forall|key: i64| #[trigger] accumulated(moments@, key).count as int == prefix_count(rows@, index as int + 1, key) by {
                assert(accumulated(before@, key).count as int == prefix_count(rows@, index as int, key));
                lemma_line_prefix(rows@, index as int, key);
            }
            assert forall|key: i64| #[trigger] accumulated(moments@, key).price as int == prefix_price(rows@, index as int + 1, key) by {
                assert(accumulated(before@, key).price as int == prefix_price(rows@, index as int, key));
                lemma_line_prefix(rows@, index as int, key);
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    proof {
        reveal(prefix_count); reveal(prefix_price); reveal(line_count); reveal(line_price);
        keys@.to_set_ensures();
    }
    Some((moments, keys))
}

}
