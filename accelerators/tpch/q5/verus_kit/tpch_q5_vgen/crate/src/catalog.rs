use vstd::prelude::*;
use std::sync::Arc;
use std::collections::HashMap;
use verus_common::sums::*;
use crate::gen_obligations::*;
use crate::gen_spec::*;

verus! {

pub struct Binding {
    pub key: i64,
    pub region: i64,
    pub name: Vec<u8>,
    pub count: i128,
}

#[verifier::opaque]
pub open spec fn name_hash(bytes: Seq<u8>) -> u64
    decreases bytes.len(),
{
    if bytes.len() == 0 { 0u64 }
    else { ((name_hash(bytes.drop_last()) as int * 257 + bytes.last() as int) % 65521) as u64 }
}

pub proof fn lemma_hash_push(bytes: Seq<u8>, byte: u8)
    ensures name_hash(bytes.push(byte)) == ((name_hash(bytes) as int * 257 + byte as int) % 65521) as u64,
{
    reveal(name_hash);
    assert(bytes.push(byte).drop_last() =~= bytes);
}

pub fn hash_name(bytes: &[u8]) -> (out: u64)
    ensures out == name_hash(bytes@),
{
    proof { assert(name_hash(Seq::empty()) == 0u64) by { reveal(name_hash); } }
    let mut out = 0u64;
    let mut index = 0;
    proof { assert(bytes@.take(0) =~= Seq::empty()); }
    while index < bytes.len()
        invariant index <= bytes.len(), out < 65521, out == name_hash(bytes@.take(index as int)),
        decreases bytes.len() - index,
    {
        proof {
            assert(bytes@.take(index as int + 1) =~= bytes@.take(index as int).push(bytes@[index as int]));
            lemma_hash_push(bytes@.take(index as int), bytes@[index as int]);
        }
        out = (out * 257 + bytes[index] as u64) % 65521;
        index += 1;
    }
    proof { assert(bytes@.take(bytes@.len() as int) =~= bytes@); }
    out
}

pub open spec fn bindings_indexed<Key>(rows: Seq<Binding>, index: Map<Key, Vec<usize>>, project: spec_fn(Binding) -> Key) -> bool {
    &&& forall|key: Key| #[trigger] index.contains_key(key) <==>
        exists|position: int| 0 <= position < rows.len() && project(#[trigger] rows[position]) == key
    &&& forall|key: Key, position: usize| index.contains_key(key) ==>
        (#[trigger] (index[key])@.contains(position) <==>
            position < rows.len() && project(rows[position as int]) == key)
    &&& forall|key: Key, first: int, second: int| index.contains_key(key) && 0 <= first < second < (index[key])@.len() ==>
        #[trigger] (index[key])@[first] != #[trigger] (index[key])@[second]
}

pub open spec fn regions_indexed(rows: Seq<Binding>, index: Map<i64, Vec<usize>>) -> bool {
    bindings_indexed(rows, index, |row: Binding| row.region)
}

pub open spec fn names_indexed(rows: Seq<Binding>, index: Map<u64, Vec<usize>>) -> bool {
    bindings_indexed(rows, index, |row: Binding| name_hash(row.name@))
}

pub proof fn lemma_bindings_index_step<Key>(rows: Seq<Binding>, index: Map<Key, Vec<usize>>,
    row: Binding, positions: Vec<usize>, project: spec_fn(Binding) -> Key)
    requires bindings_indexed(rows, index, project), rows.len() < usize::MAX,
        positions@ == (if index.contains_key(project(row)) { (index[project(row)])@ } else { Seq::empty() }).push(rows.len() as usize),
    ensures bindings_indexed(rows.push(row), index.insert(project(row), positions), project),
{
    let next = index.insert(project(row), positions);
    let previous = if index.contains_key(project(row)) { (index[project(row)])@ } else { Seq::empty() };
    verus_prooflib::lemma_push_membership(previous, rows.len() as usize);
    assert(!previous.contains(rows.len() as usize));
    assert forall|key: Key| #[trigger] next.contains_key(key) <==>
        exists|position: int| 0 <= position < rows.push(row).len() && project(#[trigger] rows.push(row)[position]) == key by {
        if key == project(row) {
            assert(project(rows.push(row)[rows.len() as int]) == key);
        } else {
            if next.contains_key(key) {
                let position = choose|position: int| 0 <= position < rows.len() && project(#[trigger] rows[position]) == key;
                assert(rows.push(row)[position] == rows[position]);
            }
            if exists|position: int| 0 <= position < rows.push(row).len() && project(#[trigger] rows.push(row)[position]) == key {
                let position = choose|position: int| 0 <= position < rows.push(row).len() && project(#[trigger] rows.push(row)[position]) == key;
                assert(position < rows.len());
                assert(rows[position] == rows.push(row)[position]);
                assert(index.contains_key(key));
            }
        }
    }
    assert forall|key: Key, position: usize| next.contains_key(key) implies
        (#[trigger] (next[key])@.contains(position) <==> position < rows.push(row).len() && project(rows.push(row)[position as int]) == key) by {
        if position == rows.len() {
            if key != project(row) { assert(!(index[key])@.contains(position)); }
        } else if position < rows.len() {
            assert(rows.push(row)[position as int] == rows[position as int]);
        } else {
            if index.contains_key(key) { assert(!(index[key])@.contains(position)); }
        }
    }
    assert forall|key: Key, first: int, second: int| next.contains_key(key) && 0 <= first < second < (next[key])@.len() implies
        #[trigger] (next[key])@[first] != #[trigger] (next[key])@[second] by {
        if key == project(row) && second == previous.len() {
            assert(previous.contains(previous[first]));
        }
    }
}

pub fn region_index(rows: &Vec<Binding>) -> (out: HashMap<i64, Vec<usize>>)
    ensures regions_indexed(rows@, out@),
{
    let mut out: HashMap<i64, Vec<usize>> = HashMap::new();
    let mut index = 0;
    let mut processed: Ghost<int> = Ghost(0int);
    while index < rows.len()
        invariant index <= rows.len(), processed@ == index, regions_indexed(rows@.take(index as int), out@),
        decreases rows.len() - index,
    {
        let key = rows[index].region;
        let ghost before = out@;
        let mut positions = match out.remove(&key) { Some(positions) => positions, None => Vec::new() };
        positions.push(index);
        let ghost inserted = positions;
        proof { lemma_bindings_index_step(rows@.take(index as int), before, rows@[index as int], positions, |row: Binding| row.region); }
        out.insert(key, positions);
        proof {
            assert(out@ =~= before.insert(key, inserted));
            assert(rows@.take(index as int + 1) =~= rows@.take(index as int).push(rows@[index as int]));
            assert(regions_indexed(rows@.take(index as int + 1), out@));
        }
        index += 1;
        processed = Ghost(index as int);
    }
    proof { assert(rows@.take(rows@.len() as int) =~= rows@); }
    out
}

pub fn name_index(rows: &Vec<Binding>) -> (out: HashMap<u64, Vec<usize>>)
    ensures names_indexed(rows@, out@),
{
    let mut out: HashMap<u64, Vec<usize>> = HashMap::new();
    let mut index = 0;
    let mut processed: Ghost<int> = Ghost(0int);
    while index < rows.len()
        invariant index <= rows.len(), processed@ == index, names_indexed(rows@.take(index as int), out@),
        decreases rows.len() - index,
    {
        let key = hash_name(&rows[index].name);
        let ghost before = out@;
        let mut positions = match out.remove(&key) { Some(positions) => positions, None => Vec::new() };
        positions.push(index);
        let ghost inserted = positions;
        proof { lemma_bindings_index_step(rows@.take(index as int), before, rows@[index as int], positions, |row: Binding| name_hash(row.name@)); }
        out.insert(key, positions);
        proof {
            assert(out@ =~= before.insert(key, inserted));
            assert(rows@.take(index as int + 1) =~= rows@.take(index as int).push(rows@[index as int]));
            assert(names_indexed(rows@.take(index as int + 1), out@));
        }
        index += 1;
        processed = Ghost(index as int);
    }
    proof { assert(rows@.take(rows@.len() as int) =~= rows@); }
    out
}

pub fn bytes_copy(bytes: &[u8]) -> (out: Vec<u8>)
    ensures out@ == bytes@,
{
    let mut out = Vec::new();
    let mut index = 0;
    while index < bytes.len()
        invariant index <= bytes.len(), out@ == bytes@.take(index as int),
        decreases bytes.len() - index,
    { out.push(bytes[index]); index += 1; }
    out
}

pub fn bytes_equal(left: &[u8], right: &[u8]) -> (out: bool)
    ensures out == (left@ == right@),
{
    if left.len() != right.len() { return false; }
    let mut index = 0;
    while index < left.len()
        invariant index <= left.len(), left.len() == right.len(),
            forall|position: int| 0 <= position < index ==> #[trigger] left@[position] == right@[position],
        decreases left.len() - index,
    {
        if left[index] != right[index] { return false; }
        index += 1;
    }
    proof { assert(left@ =~= right@); }
    true
}

pub open spec fn binding_count(rows: Seq<Binding>, key: i64, region: i64, name: Seq<u8>) -> int {
    sum(rows, |row: Binding| if row.key == key && row.region == region && row.name@ == name { row.count as int } else { 0 })
}

pub proof fn lemma_binding_weight(rows: Seq<Binding>, key: i64, region: i64, name: Seq<u8>, weight: spec_fn(Binding) -> int)
    requires forall|position: int| 0 <= position < rows.len() ==> weight(#[trigger] rows[position]) ==
        if rows[position].key == key && rows[position].region == region && rows[position].name@ == name { rows[position].count as int } else { 0 },
    ensures sum(rows, weight) == binding_count(rows, key, region, name),
    decreases rows.len(),
{
    if rows.len() > 0 {
        assert forall|position: int| 0 <= position < rows.drop_last().len() implies weight(#[trigger] rows.drop_last()[position]) ==
            if rows.drop_last()[position].key == key && rows.drop_last()[position].region == region && rows.drop_last()[position].name@ == name { rows.drop_last()[position].count as int } else { 0 } by {
            assert(rows.drop_last()[position] == rows[position]);
        }
        lemma_binding_weight(rows.drop_last(), key, region, name, weight);
        assert(rows.last() == rows[rows.len() - 1]);
    }
}

pub fn copy(rows: &Vec<Binding>) -> (out: Vec<Binding>)
    ensures out@.len() == rows@.len(),
        forall|key: i64, region: i64, name: Seq<u8>| #[trigger] binding_count(out@, key, region, name) == binding_count(rows@, key, region, name),
{
    let mut out: Vec<Binding> = Vec::new();
    let mut index = 0;
    while index < rows.len()
        invariant index <= rows.len(), out@.len() == index,
            forall|position: int| 0 <= position < index ==>
                (#[trigger] out@[position]).key == rows@[position].key
                && out@[position].region == rows@[position].region
                && out@[position].name@ == rows@[position].name@
                && out@[position].count == rows@[position].count,
        decreases rows.len() - index,
    {
        let row = &rows[index];
        out.push(Binding { key: row.key, region: row.region, name: bytes_copy(&row.name), count: row.count });
        index += 1;
    }
    proof {
        assert forall|key: i64, region: i64, name: Seq<u8>| #[trigger] binding_count(out@, key, region, name) == binding_count(rows@, key, region, name) by {
            let weight = |row: Binding| if row.key == key && row.region == region && row.name@ == name { row.count as int } else { 0 };
            verus_prooflib::lemma_sum_aligned(out@, rows@, weight, weight);
        }
    }
    out
}

pub fn add(rows: &mut Vec<Binding>, key: i64, region: i64, name: &[u8], sign: i128) -> (ok: bool)
    ensures ok ==> forall|other: i64, area: i64, text: Seq<u8>| #[trigger] binding_count(final(rows)@, other, area, text) ==
        binding_count(old(rows)@, other, area, text) + if other == key && area == region && text == name@ { sign as int } else { 0 },
        ok && sign > 0 && (forall|position: int| 0 <= position < old(rows)@.len() ==> (#[trigger] old(rows)@[position]).count > 0) ==>
            forall|position: int| 0 <= position < final(rows)@.len() ==> (#[trigger] final(rows)@[position]).count > 0,
{
    let ghost entry = rows@;
    let mut index = 0;
    while index < rows.len()
        invariant index <= rows.len(), rows@ == entry, entry == old(rows)@,
        decreases rows.len() - index,
    {
        let row = &rows[index];
        if row.key == key && row.region == region && bytes_equal(&row.name, name) {
            let count = match row.count.checked_add(sign) { Some(value) => value, None => return false };
            let replacement = Binding { key, region, name: bytes_copy(name), count };
            proof {
                assert forall|other: i64, area: i64, text: Seq<u8>| #[trigger] binding_count(entry.update(index as int, replacement), other, area, text) ==
                    binding_count(entry, other, area, text) + if other == key && area == region && text == name@ { sign as int } else { 0 } by {
                    lemma_sum_update(entry, index as int, replacement,
                        |item: Binding| if item.key == other && item.region == area && item.name@ == text { item.count as int } else { 0 });
                }
            }
            rows.set(index, replacement);
            return true;
        }
        index += 1;
    }
    if sign != 0 {
        let row = Binding { key, region, name: bytes_copy(name), count: sign };
        proof {
            assert forall|other: i64, area: i64, text: Seq<u8>| #[trigger] binding_count(entry.push(row), other, area, text) ==
                binding_count(entry, other, area, text) + if other == key && area == region && text == name@ { sign as int } else { 0 } by {
                lemma_sum_push(entry, row, |item: Binding| if item.key == other && item.region == area && item.name@ == text { item.count as int } else { 0 });
            }
        }
        rows.push(row);
    }
    true
}

pub fn compact(rows: &mut Vec<Binding>) -> (ok: bool)
    ensures ok ==> forall|index: int| 0 <= index < final(rows)@.len() ==> (#[trigger] final(rows)@[index]).count > 0,
        forall|key: i64, region: i64, name: Seq<u8>| #[trigger] binding_count(final(rows)@, key, region, name) == binding_count(old(rows)@, key, region, name),
{
    let ghost entry = rows@;
    let mut index = rows.len();
    while index > 0
        invariant index <= rows.len(), entry == old(rows)@,
            forall|position: int| index <= position < rows@.len() ==> (#[trigger] rows@[position]).count > 0,
            forall|key: i64, region: i64, name: Seq<u8>| #[trigger] binding_count(rows@, key, region, name) == binding_count(entry, key, region, name),
        decreases index,
    {
        index -= 1;
        if rows[index].count < 0 { return false; }
        if rows[index].count == 0 {
            proof {
                assert forall|key: i64, region: i64, name: Seq<u8>| #[trigger] binding_count(rows@.remove(index as int), key, region, name) == binding_count(rows@, key, region, name) by {
                    lemma_sum_remove(rows@, index as int, |item: Binding| if item.key == key && item.region == region && item.name@ == name { item.count as int } else { 0 });
                }
            }
            rows.remove(index);
        }
    }
    true
}

}
