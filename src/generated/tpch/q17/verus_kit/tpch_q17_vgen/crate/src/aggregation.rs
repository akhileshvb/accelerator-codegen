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

pub open spec fn vtotal_at(index_of: Map<i64, usize>, totals: Seq<Total>, key: i64) -> Total {
    if index_of.contains_key(key) && index_of[key] < totals.len() { totals[index_of[key] as int] } else { Total { count: 0, quantity: 0 } }
}

pub open spec fn vaccum_at(index_of: Map<i64, usize>, moms: Seq<Moment>, key: i64) -> Moment {
    if index_of.contains_key(key) && index_of[key] < moms.len() { moms[index_of[key] as int] } else { Moment { count: 0, price: 0 } }
}

/// Aggregates the rows per partkey with one hash lookup per row: pass one
/// finds (or creates) each row's slot in dense vectors, pass two reuses the
/// recorded slot, pass three exports the nonzero moments.
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
    let expected = rows.len() / 16 + 1;
    let mut index_of: HashMap<i64, usize> = HashMap::with_capacity(expected);
    let mut totals: Vec<Total> = Vec::with_capacity(expected);
    let mut keys: Vec<i64> = Vec::with_capacity(expected);
    let mut positions: Vec<usize> = Vec::with_capacity(rows.len());
    let mut index = 0;
    let mut processed = Ghost(0int);
    proof {
        assert forall|key: i64| #[trigger] vtotal_at(index_of@, totals@, key).count as int == number(line_input_seq_view(rows@.take(0)), key as int) by { lemma_line_empty(rows@, key); }
        assert forall|key: i64| #[trigger] vtotal_at(index_of@, totals@, key).quantity as int == quantity(line_input_seq_view(rows@.take(0)), key as int) by { lemma_line_empty(rows@, key); }
    }
    while index < rows.len()
        invariant index <= rows.len(), processed@ == index as int,
            keys@.no_duplicates(), totals@.len() == keys@.len(), positions@.len() == index,
            index_of@.dom() == keys@.to_set(),
            forall|key: i64| #![trigger index_of@[key]] index_of@.contains_key(key) ==> index_of@[key] < keys@.len() && keys@[index_of@[key] as int] == key,
            forall|position: int| #![trigger totals@[position]] 0 <= position < totals@.len() ==> totals@[position].count > 0,
            forall|row: int| #![trigger positions@[row]] 0 <= row < index ==> positions@[row] < keys@.len() && keys@[positions@[row] as int] == rows@[row].l_partkey,
            forall|key: i64| #[trigger] vtotal_at(index_of@, totals@, key).count as int == number(line_input_seq_view(rows@.take(processed@)), key as int),
            forall|key: i64| #[trigger] vtotal_at(index_of@, totals@, key).quantity as int == quantity(line_input_seq_view(rows@.take(processed@)), key as int),
        decreases rows.len() - index,
    {
        let row = &rows[index];
        let ghost before_index = index_of@;
        let ghost before_totals = totals@;
        let ghost before_keys = keys@;
        let ghost before_positions = positions@;
        let found = match index_of.get(&row.l_partkey) { Some(value) => Some(*value), None => None };
        match found {
            Some(slot) => {
                let previous = totals[slot];
                proof {
                    assert(previous == vtotal_at(before_index, before_totals, row.l_partkey));
                }
                let count = match previous.count.checked_add(1) { Some(value) => value, None => return None };
                let quantity_sum = match previous.quantity.checked_add(row.l_quantity as i128) { Some(value) => value, None => return None };
                totals.set(slot, Total { count, quantity: quantity_sum });
                positions.push(slot);
                proof {
                    keys@.to_set_ensures();
                    let rk = row.l_partkey;
                    assert(index_of@ == before_index);
                    assert(before_index.contains_key(rk) && before_index[rk] == slot);
                    assert(before_keys[slot as int] == rk);
                    assert(totals@[slot as int].count == previous.count + 1);
                    assert(totals@[slot as int].quantity == previous.quantity + row.l_quantity as i128);
                    assert forall|key: i64| #[trigger] vtotal_at(index_of@, totals@, key).count as int
                        == number(line_input_seq_view(rows@.take(index as int + 1)), key as int) by {
                        lemma_line_prefix(rows@, index as int, key);
                        assert(vtotal_at(before_index, before_totals, key).count as int == number(line_input_seq_view(rows@.take(index as int)), key as int));
                        assert(vtotal_at(before_index, before_totals, key).quantity as int == quantity(line_input_seq_view(rows@.take(index as int)), key as int));
                        if index_of@.contains_key(key) && key != rk { assert(before_keys[before_index[key] as int] == key); }
                    }
                    assert forall|key: i64| #[trigger] vtotal_at(index_of@, totals@, key).quantity as int
                        == quantity(line_input_seq_view(rows@.take(index as int + 1)), key as int) by {
                        lemma_line_prefix(rows@, index as int, key);
                        assert(vtotal_at(before_index, before_totals, key).count as int == number(line_input_seq_view(rows@.take(index as int)), key as int));
                        assert(vtotal_at(before_index, before_totals, key).quantity as int == quantity(line_input_seq_view(rows@.take(index as int)), key as int));
                        if index_of@.contains_key(key) && key != rk { assert(before_keys[before_index[key] as int] == key); }
                    }
                }
            },
            None => {
                let slot = keys.len();
                proof { keys@.to_set_ensures(); verus_prooflib::lemma_unique_push(keys@, row.l_partkey); }
                keys.push(row.l_partkey);
                totals.push(Total { count: 1, quantity: row.l_quantity as i128 });
                index_of.insert(row.l_partkey, slot);
                positions.push(slot);
                proof {
                    keys@.to_set_ensures();
                    before_keys.to_set_ensures();
                    let rk = row.l_partkey;
                    assert(!before_index.contains_key(rk));
                    assert(index_of@ == before_index.insert(rk, before_keys.len() as usize));
                    assert(totals@ == before_totals.push(Total { count: 1, quantity: row.l_quantity as i128 }));
                    assert forall|key: i64| #[trigger] vtotal_at(index_of@, totals@, key).count as int
                        == number(line_input_seq_view(rows@.take(index as int + 1)), key as int) by {
                        lemma_line_prefix(rows@, index as int, key);
                        assert(vtotal_at(before_index, before_totals, key).count as int == number(line_input_seq_view(rows@.take(index as int)), key as int));
                        assert(vtotal_at(before_index, before_totals, key).quantity as int == quantity(line_input_seq_view(rows@.take(index as int)), key as int));
                        if key == rk {
                            assert(vtotal_at(before_index, before_totals, key).count == 0);
                            assert(totals@[slot as int].count == 1);
                        } else if before_index.contains_key(key) {
                            assert(before_index[key] < before_totals.len());
                        }
                    }
                    assert forall|key: i64| #[trigger] vtotal_at(index_of@, totals@, key).quantity as int
                        == quantity(line_input_seq_view(rows@.take(index as int + 1)), key as int) by {
                        lemma_line_prefix(rows@, index as int, key);
                        assert(vtotal_at(before_index, before_totals, key).count as int == number(line_input_seq_view(rows@.take(index as int)), key as int));
                        assert(vtotal_at(before_index, before_totals, key).quantity as int == quantity(line_input_seq_view(rows@.take(index as int)), key as int));
                        if key == rk {
                            assert(vtotal_at(before_index, before_totals, key).quantity == 0);
                        } else if before_index.contains_key(key) {
                            assert(before_index[key] < before_totals.len());
                        }
                    }
                }
            },
        }
        index += 1;
        processed = Ghost(index as int);
    }
    // pass two: per-slot moments, driven by the recorded slots
    let mut moms: Vec<Moment> = Vec::with_capacity(keys.len());
    let mut slot_index = 0;
    while slot_index < keys.len()
        invariant slot_index <= keys.len(), moms@.len() == slot_index,
            forall|position: int| #![trigger moms@[position]] 0 <= position < slot_index ==> moms@[position].count == 0 && moms@[position].price == 0,
        decreases keys.len() - slot_index,
    {
        moms.push(Moment { count: 0, price: 0 });
        slot_index += 1;
    }
    proof {
        assert(rows@.take(rows@.len() as int) =~= rows@);
        assert forall|key: i64| #[trigger] vaccum_at(index_of@, moms@, key).count as int == prefix_count(rows@, 0, key) by { lemma_line_empty(rows@, key); }
        assert forall|key: i64| #[trigger] vaccum_at(index_of@, moms@, key).price as int == prefix_price(rows@, 0, key) by { lemma_line_empty(rows@, key); }
    }
    index = 0;
    processed = Ghost(0int);
    while index < rows.len()
        invariant index <= rows.len(), processed@ == index as int,
            keys@.no_duplicates(), totals@.len() == keys@.len(), positions@.len() == rows@.len(), moms@.len() == keys@.len(),
            index_of@.dom() == keys@.to_set(),
            forall|key: i64| #![trigger index_of@[key]] index_of@.contains_key(key) ==> index_of@[key] < keys@.len() && keys@[index_of@[key] as int] == key,
            forall|position: int| #![trigger totals@[position]] 0 <= position < totals@.len() ==> totals@[position].count > 0,
            forall|row: int| #![trigger positions@[row]] 0 <= row < rows@.len() ==> positions@[row] < keys@.len() && keys@[positions@[row] as int] == rows@[row].l_partkey,
            forall|key: i64| #[trigger] vtotal_at(index_of@, totals@, key).count as int == number(line_input_seq_view(rows@), key as int),
            forall|key: i64| #[trigger] vtotal_at(index_of@, totals@, key).quantity as int == quantity(line_input_seq_view(rows@), key as int),
            forall|position: int| #![trigger moms@[position]] 0 <= position < moms@.len() ==> moms@[position].count >= 0 && (moms@[position].count == 0 ==> moms@[position].price == 0),
            forall|key: i64| #[trigger] vaccum_at(index_of@, moms@, key).count as int == prefix_count(rows@, processed@, key),
            forall|key: i64| #[trigger] vaccum_at(index_of@, moms@, key).price as int == prefix_price(rows@, processed@, key),
        decreases rows.len() - index,
    {
        let row = &rows[index];
        let ghost before_moms = moms@;
        let slot = positions[index];
        proof {
            // the recorded slot is the key's slot
            keys@.to_set_ensures();
            assert(keys@.contains(row.l_partkey));
            assert(index_of@.contains_key(row.l_partkey));
            assert(keys@[index_of@[row.l_partkey] as int] == row.l_partkey);
            assert(index_of@[row.l_partkey] == slot);
        }
        let total = totals[slot];
        proof {
            assert(total == vtotal_at(index_of@, totals@, row.l_partkey));
            assert(total.count as int == number(line_input_seq_view(rows@), row.l_partkey as int));
            assert(total.quantity as int == quantity(line_input_seq_view(rows@), row.l_partkey as int));
        }
        let accepts = match qualified(row.l_quantity, total) { Some(value) => value, None => return None };
        proof { reveal(qualifies); assert(accepts == qualifies(line_input_seq_view(rows@), line_input_view(*row))); }
        if accepts {
            let previous = moms[slot];
            let count = match previous.count.checked_add(1) { Some(value) => value, None => return None };
            let price = match previous.price.checked_add(row.l_extendedprice as i128) { Some(value) => value, None => return None };
            moms.set(slot, Moment { count, price });
        }
        proof {
            let rk = row.l_partkey;
            assert(vaccum_at(index_of@, before_moms, rk) == before_moms[slot as int]);
            assert forall|key: i64| #[trigger] vaccum_at(index_of@, moms@, key).count as int == prefix_count(rows@, index as int + 1, key) by {
                lemma_line_prefix(rows@, index as int, key);
                assert(vaccum_at(index_of@, before_moms, key).count as int == prefix_count(rows@, index as int, key));
                assert(vaccum_at(index_of@, before_moms, key).price as int == prefix_price(rows@, index as int, key));
                if index_of@.contains_key(key) && key != rk { assert(keys@[index_of@[key] as int] == key); }
                if accepts { assert(moms@ == before_moms.update(slot as int, moms@[slot as int])); }
                else { assert(moms@ == before_moms); }
            }
            assert forall|key: i64| #[trigger] vaccum_at(index_of@, moms@, key).price as int == prefix_price(rows@, index as int + 1, key) by {
                lemma_line_prefix(rows@, index as int, key);
                assert(vaccum_at(index_of@, before_moms, key).count as int == prefix_count(rows@, index as int, key));
                assert(vaccum_at(index_of@, before_moms, key).price as int == prefix_price(rows@, index as int, key));
                if index_of@.contains_key(key) && key != rk { assert(keys@[index_of@[key] as int] == key); }
                if accepts { assert(moms@ == before_moms.update(slot as int, moms@[slot as int])); }
                else { assert(moms@ == before_moms); }
            }
            assert forall|position: int| #![trigger moms@[position]] 0 <= position < moms@.len() implies moms@[position].count >= 0 && (moms@[position].count == 0 ==> moms@[position].price == 0) by {
                if accepts && position == slot as int { assert(moms@[position].count == before_moms[position].count + 1); }
                else if accepts { assert(moms@[position] == before_moms[position]); }
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    // pass three: export the nonzero moments
    let mut moments: HashMap<i64, Moment> = HashMap::new();
    slot_index = 0;
    while slot_index < keys.len()
        invariant slot_index <= keys.len(), moms@.len() == keys@.len(),
            keys@.no_duplicates(), positions@.len() == rows@.len(),
            forall|row: int| #![trigger positions@[row]] 0 <= row < rows@.len() ==> positions@[row] < keys@.len() && keys@[positions@[row] as int] == rows@[row].l_partkey,
            index_of@.dom() == keys@.to_set(),
            forall|key: i64| #![trigger index_of@[key]] index_of@.contains_key(key) ==> index_of@[key] < keys@.len() && keys@[index_of@[key] as int] == key,
            forall|position: int| #![trigger moms@[position]] 0 <= position < moms@.len() ==> moms@[position].count >= 0 && (moms@[position].count == 0 ==> moms@[position].price == 0),
            forall|key: i64| #![trigger moments@[key]] moments@.contains_key(key) ==> moments@[key].count > 0,
            forall|key: i64| #[trigger] moments@.contains_key(key) ==> index_of@.contains_key(key) && index_of@[key] < slot_index,
            forall|key: i64| #[trigger] accumulated(moments@, key) == if index_of@.contains_key(key) && index_of@[key] < slot_index { moms@[index_of@[key] as int] } else { Moment { count: 0, price: 0 } },
        decreases keys.len() - slot_index,
    {
        let key = keys[slot_index];
        let moment = moms[slot_index];
        proof {
            keys@.to_set_ensures();
            assert(index_of@.contains_key(key));
            assert(keys@[index_of@[key] as int] == key);
            assert(index_of@[key] == slot_index);
        }
        let ghost before_moments = moments@;
        if moment.count > 0 {
            moments.insert(key, moment);
        }
        proof {
            assert forall|other: i64| #[trigger] accumulated(moments@, other)
                == if index_of@.contains_key(other) && index_of@[other] < slot_index + 1 { moms@[index_of@[other] as int] } else { Moment { count: 0, price: 0 } } by {
                if other == key {
                    assert(index_of@[other] == slot_index);
                    if moment.count > 0 {
                        assert(moments@[key] == moment);
                    } else {
                        assert(moment.price == 0);
                        assert(!moments@.contains_key(key));
                    }
                } else {
                    assert(accumulated(before_moments, other) == if index_of@.contains_key(other) && index_of@[other] < slot_index { moms@[index_of@[other] as int] } else { Moment { count: 0, price: 0 } });
                    if index_of@.contains_key(other) {
                        assert(keys@[index_of@[other] as int] == other);
                        assert(index_of@[other] != slot_index);
                    }
                }
            }
        }
        slot_index += 1;
    }
    proof {
        reveal(prefix_count); reveal(prefix_price); reveal(line_count); reveal(line_price);
        keys@.to_set_ensures();
        assert(rows@.take(rows@.len() as int) =~= rows@);
        assert forall|index: int| #![trigger rows@[index]] 0 <= index < rows@.len() implies keys@.contains(rows@[index].l_partkey) by {
            assert(keys@[positions@[index] as int] == rows@[index].l_partkey);
        }
        assert forall|key: i64| moments@.dom().contains(key) implies keys@.to_set().contains(key) by { assert(index_of@.contains_key(key)); }
        assert forall|key: i64| #[trigger] accumulated(moments@, key).count as int == line_count(line_input_seq_view(rows@), key as int) by {
            if index_of@.contains_key(key) { assert(index_of@[key] < keys@.len()); }
            assert(vaccum_at(index_of@, moms@, key).count as int == prefix_count(rows@, rows@.len() as int, key));
        }
        assert forall|key: i64| #[trigger] accumulated(moments@, key).price as int == line_price(line_input_seq_view(rows@), key as int) by {
            if index_of@.contains_key(key) { assert(index_of@[key] < keys@.len()); }
            assert(vaccum_at(index_of@, moms@, key).price as int == prefix_price(rows@, rows@.len() as int, key));
        }
    }
    Some((moments, keys))
}

}
