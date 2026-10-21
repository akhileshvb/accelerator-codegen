use vstd::prelude::*;
use std::sync::Arc;
use std::collections::HashMap;
use crate::profiles::*;
use crate::storage::*;
use crate::gen_obligations::*;
use verus_common::sums::*;
use verus_prooflib::{IntegerTriple, IntegerTripleMap};
use verus_prooflib::wide::Wide;

verus! {
broadcast use vstd::std_specs::hash::group_hash_axioms;
pub struct SupplierVariant { pub name: Vec<u8>, pub address: Vec<u8>, pub count: Wide }
pub struct NationVariant { pub name: Vec<u8>, pub key: i64, pub count: Wide }
pub type Suppliers = Table<Table<Vec<SupplierVariant>>>;
pub type Nations = Table<Vec<NationVariant>>;
/// Part names: keyed by name_slot(part key, name); a variant is (name, part key, count).
pub type PartNames = Table<Vec<NationVariant>>;
pub struct SupplierWork {
    pub rows: IntegerTripleMap<Vec<SupplierVariant>>,
    pub keys: Vec<IntegerTriple>,
}
pub struct NationWork { pub rows: HashMap<i64, Vec<NationVariant>>, pub keys: Vec<i64> }
pub struct NameWork { pub rows: HashMap<i64, Vec<NationVariant>>, pub keys: Vec<i64> }

pub open spec fn supplier_measure(rows: Seq<SupplierVariant>, name: Seq<u8>, address: Seq<u8>) -> int {
    sum(rows, |row: SupplierVariant| if row.name@ == name && row.address@ == address { row.count@ } else { 0 })
}
pub open spec fn nation_measure(rows: Seq<NationVariant>, name: Seq<u8>, key: i64) -> int {
    sum(rows, |row: NationVariant| if row.name@ == name && row.key == key { row.count@ } else { 0 })
}
pub proof fn lemma_supplier_measure_weight(rows: Seq<SupplierVariant>, name: Seq<u8>, address: Seq<u8>, term: spec_fn(SupplierVariant) -> int)
    requires forall|row: SupplierVariant| #[trigger] term(row) == if row.name@ == name && row.address@ == address { row.count@ } else { 0 },
    ensures supplier_measure(rows, name, address) == sum(rows, term),
    decreases rows.len(),
{
    if rows.len() > 0 {
        lemma_supplier_measure_weight(rows.drop_last(), name, address, term);
        lemma_sum_push(rows.drop_last(), rows.last(), term);
        assert(term(rows.last()) == if rows.last().name@ == name && rows.last().address@ == address { rows.last().count@ } else { 0 });
        let weight = |row: SupplierVariant| if row.name@ == name && row.address@ == address { row.count@ } else { 0 };
        lemma_sum_push(rows.drop_last(), rows.last(), weight);
    }
}
pub proof fn lemma_nation_measure_weight(rows: Seq<NationVariant>, name: Seq<u8>, key: i64, term: spec_fn(NationVariant) -> int)
    requires forall|row: NationVariant| #[trigger] term(row) == if row.name@ == name && row.key == key { row.count@ } else { 0 },
    ensures nation_measure(rows, name, key) == sum(rows, term),
    decreases rows.len(),
{
    if rows.len() > 0 {
        lemma_nation_measure_weight(rows.drop_last(), name, key, term);
        lemma_sum_push(rows.drop_last(), rows.last(), term);
        assert(term(rows.last()) == if rows.last().name@ == name && rows.last().key == key { rows.last().count@ } else { 0 });
    }
}
pub open spec fn suppliers_positive(state: Suppliers) -> bool {
    forall|nation: i64| #![trigger state.lookup(nation)] state.lookup(nation) matches Some(bucket) ==>
        bucket.well_formed() && forall|supplier: i64| #![trigger bucket.lookup(supplier)] bucket.lookup(supplier) matches Some(rows) ==>
            forall|index: int| #![trigger rows@[index]] 0 <= index < rows@.len() ==> rows@[index].count@ > 0
}
pub open spec fn byte_or_zero(s: Seq<u8>, i: int) -> int { if 0 <= i < s.len() { s[i] as int } else { 0 } }
/// The first three bytes of a name, packed (missing bytes read as 0).
pub open spec fn name_code(s: Seq<u8>) -> int { byte_or_zero(s, 0) * 65536 + byte_or_zero(s, 1) * 256 + byte_or_zero(s, 2) }
pub open spec fn key_low(key: i64) -> int { ((key as u64) % 1024) as int }
/// Table slot of a (part key, name) row: first-three-bytes code, then the low 10 bits of the part key.
pub open spec fn name_slot(key: i64, name: Seq<u8>) -> i64 { (name_code(name) * 1024 + key_low(key)) as i64 }
pub fn name_slot_exec(key: i64, name: &[u8]) -> (result: i64)
    ensures result == name_slot(key, name@),
{
    let b0: i64 = if name.len() > 0 { name[0] as i64 } else { 0 };
    let b1: i64 = if name.len() > 1 { name[1] as i64 } else { 0 };
    let b2: i64 = if name.len() > 2 { name[2] as i64 } else { 0 };
    let code: i64 = b0 * 65536 + b1 * 256 + b2;
    let low: i64 = (key as u64 % 1024) as i64;
    code * 1024 + low
}
pub open spec fn names_positive(state: PartNames) -> bool {
    forall|slot: i64| #![trigger state.lookup(slot)] state.lookup(slot) matches Some(rows) ==>
        forall|index: int| #![trigger rows@[index]] 0 <= index < rows@.len() ==>
            rows@[index].count@ > 0 && name_slot(rows@[index].key, rows@[index].name@) == slot
}
pub proof fn lemma_names_row(names: PartNames, slot: i64, rows: Vec<NationVariant>, index: int)
    requires names_positive(names), names.lookup(slot) == Some(rows), 0 <= index < rows@.len(),
    ensures rows@[index].count@ > 0, name_slot(rows@[index].key, rows@[index].name@) == slot,
{
    assert(names.lookup(slot) matches Some(r) && r == rows);
    assert(forall|i: int| #![trigger rows@[i]] 0 <= i < rows@.len() ==> rows@[i].count@ > 0 && name_slot(rows@[i].key, rows@[i].name@) == slot);
}
pub open spec fn nations_positive(state: Nations) -> bool {
    forall|hash: i64| #![trigger state.lookup(hash)] state.lookup(hash) matches Some(rows) ==>
        forall|index: int| #![trigger rows@[index]] 0 <= index < rows@.len() ==>
            rows@[index].count@ > 0 && name_hash(rows@[index].name@) == hash
}
impl SupplierWork {
    pub open spec fn shape(self) -> bool {
        self.keys@.no_duplicates() && self.rows@.dom() == self.keys@.to_set().to_iset()
            && forall|index: int| #![trigger self.keys@[index]] 0 <= index < self.keys@.len() ==> self.keys@[index].2 == 0
    }
    pub open spec fn count(self, state: Suppliers, nation: i64, supplier: i64, name: Seq<u8>, address: Seq<u8>) -> int {
        if self.rows@.contains_key((nation, supplier, 0)) { supplier_measure(self.rows@[(nation, supplier, 0)]@, name, address) }
        else { crate::model::supplier_count(state, nation, supplier, name, address) }
    }
    /// Every supplier variant has a non-negative multiplicity, as in a real table.
    pub open spec fn ready(self, state: Suppliers) -> bool {
        forall|nation: i64, supplier: i64, name: Seq<u8>, address: Seq<u8>|
            #[trigger] self.count(state, nation, supplier, name, address) >= 0
    }
}
impl NationWork {
    pub open spec fn shape(self) -> bool {
        self.keys@.no_duplicates() && self.rows@.dom() == self.keys@.to_set()
    }
    pub open spec fn hashes(self) -> bool {
        forall|hash: i64| #![trigger self.rows@[hash]] self.rows@.contains_key(hash) ==>
            forall|index: int| #![trigger self.rows@[hash]@[index]] 0 <= index < self.rows@[hash]@.len() ==>
                name_hash(self.rows@[hash]@[index].name@) == hash
    }
    pub open spec fn count(self, state: Nations, name: Seq<u8>, key: i64) -> int {
        if self.rows@.contains_key(name_hash(name) as i64) { nation_measure(self.rows@[name_hash(name) as i64]@, name, key) }
        else { crate::model::nation_count(state, name, key) }
    }
    pub open spec fn ready(self, state: Nations) -> bool {
        forall|name: Seq<u8>, key: i64| #[trigger] self.count(state, name, key) >= 0
    }
}

pub fn copy_suppliers(source: &Vec<SupplierVariant>) -> (result: Vec<SupplierVariant>)
    ensures result@.len() == source@.len(),
        forall|index: int| #![trigger result@[index]] 0 <= index < result@.len() ==>
            result@[index].name@ == source@[index].name@ && result@[index].address@ == source@[index].address@
                && result@[index].count == source@[index].count,
        forall|name: Seq<u8>, address: Seq<u8>| #[trigger] supplier_measure(result@, name, address) == supplier_measure(source@, name, address),
{
    let mut rows: Vec<SupplierVariant> = Vec::new(); let mut index = 0;
    while index < source.len()
        invariant index <= source.len(), rows@.len() == index,
            forall|offset: int| #![trigger rows@[offset]] 0 <= offset < index ==>
                rows@[offset].name@ == source@[offset].name@ && rows@[offset].address@ == source@[offset].address@
                    && rows@[offset].count == source@[offset].count,
            forall|name: Seq<u8>, address: Seq<u8>| #[trigger] supplier_measure(rows@, name, address) == supplier_measure(source@.take(index as int), name, address),
        decreases source.len() - index,
    {
        let ghost before = rows@;
        rows.push(SupplierVariant { name: copy_bytes(&source[index].name), address: copy_bytes(&source[index].address), count: source[index].count.dup() });
        proof {
            assert forall|name: Seq<u8>, address: Seq<u8>| #[trigger] supplier_measure(rows@, name, address) == supplier_measure(source@.take(index as int + 1), name, address) by {
                let weight = |row: SupplierVariant| if row.name@ == name && row.address@ == address { row.count@ } else { 0 };
                assert(supplier_measure(before, name, address) == supplier_measure(source@.take(index as int), name, address));
                assert(rows@ == before.push(rows@[index as int]));
                lemma_sum_push(before, rows@[index as int], weight);
                lemma_sum_take(source@, index as int, weight);
            }
        }
        index += 1;
    }
    proof { assert(source@.take(source.len() as int) =~= source@); }
    rows
}
pub fn copy_nations(source: &Vec<NationVariant>) -> (result: Vec<NationVariant>)
    ensures result@.len() == source@.len(),
        forall|index: int| #![trigger result@[index]] 0 <= index < result@.len() ==>
            result@[index].name@ == source@[index].name@ && result@[index].key == source@[index].key && result@[index].count == source@[index].count,
        forall|name: Seq<u8>, key: i64| #[trigger] nation_measure(result@, name, key) == nation_measure(source@, name, key),
{
    let mut rows: Vec<NationVariant> = Vec::new(); let mut index = 0;
    while index < source.len()
        invariant index <= source.len(), rows@.len() == index,
            forall|offset: int| #![trigger rows@[offset]] 0 <= offset < index ==>
                rows@[offset].name@ == source@[offset].name@ && rows@[offset].key == source@[offset].key && rows@[offset].count == source@[offset].count,
            forall|name: Seq<u8>, key: i64| #[trigger] nation_measure(rows@, name, key) == nation_measure(source@.take(index as int), name, key),
        decreases source.len() - index,
    {
        let ghost before = rows@;
        rows.push(NationVariant { name: copy_bytes(&source[index].name), key: source[index].key, count: source[index].count.dup() });
        proof {
            assert forall|name: Seq<u8>, key: i64| #[trigger] nation_measure(rows@, name, key) == nation_measure(source@.take(index as int + 1), name, key) by {
                let weight = |row: NationVariant| if row.name@ == name && row.key == key { row.count@ } else { 0 };
                assert(nation_measure(before, name, key) == nation_measure(source@.take(index as int), name, key));
                assert(rows@ == before.push(rows@[index as int]));
                lemma_sum_push(before, rows@[index as int], weight);
                lemma_sum_take(source@, index as int, weight);
            }
        }
        index += 1;
    }
    proof { assert(source@.take(source.len() as int) =~= source@); }
    rows
}
pub open spec fn unique_suppliers(rows: Seq<SupplierVariant>) -> bool {
    forall|left: int, right: int| #![trigger rows[left], rows[right]] 0 <= left < right < rows.len() ==>
        !(rows[left].name@ == rows[right].name@ && rows[left].address@ == rows[right].address@)
}
pub open spec fn unique_nations(rows: Seq<NationVariant>) -> bool {
    forall|left: int, right: int| #![trigger rows[left], rows[right]] 0 <= left < right < rows.len() ==>
        !(rows[left].name@ == rows[right].name@ && rows[left].key == rows[right].key)
}
pub proof fn lemma_unique_supplier_count(rows: Seq<SupplierVariant>, index: int)
    requires unique_suppliers(rows), 0 <= index < rows.len(),
    ensures supplier_measure(rows, rows[index].name@, rows[index].address@) == rows[index].count@,
{
    let name = rows[index].name@;
    let address = rows[index].address@;
    let weight = |row: SupplierVariant| if row.name@ == name && row.address@ == address { row.count@ } else { 0 };
    assert forall|other: int| #![trigger weight(rows[other])] 0 <= other < rows.len() && other != index implies weight(rows[other]) == 0 by {
        if other < index { assert(!(rows[other].name@ == rows[index].name@ && rows[other].address@ == rows[index].address@)); }
        else { assert(!(rows[index].name@ == rows[other].name@ && rows[index].address@ == rows[other].address@)); }
    }
    lemma_sum_single_match(rows, weight, index);
    lemma_supplier_measure_weight(rows, name, address, weight);
}
pub proof fn lemma_unique_nation_count(rows: Seq<NationVariant>, index: int)
    requires unique_nations(rows), 0 <= index < rows.len(),
    ensures nation_measure(rows, rows[index].name@, rows[index].key) == rows[index].count@,
{
    let name = rows[index].name@;
    let key = rows[index].key;
    let weight = |row: NationVariant| if row.name@ == name && row.key == key { row.count@ } else { 0 };
    assert forall|other: int| #![trigger weight(rows[other])] 0 <= other < rows.len() && other != index implies weight(rows[other]) == 0 by {
        if other < index { assert(!(rows[other].name@ == rows[index].name@ && rows[other].key == rows[index].key)); }
        else { assert(!(rows[index].name@ == rows[other].name@ && rows[index].key == rows[other].key)); }
    }
    lemma_sum_single_match(rows, weight, index);
    lemma_nation_measure_weight(rows, name, key, weight);
}
/// Adds `delta` to the variant with this name and address, creating it when absent.
pub fn add_supplier(rows: &mut Vec<SupplierVariant>, name: &Vec<u8>, address: &Vec<u8>, delta: &Wide)
    ensures
        forall|probe_name: Seq<u8>, probe_address: Seq<u8>| #[trigger] supplier_measure(final(rows)@, probe_name, probe_address)
            == supplier_measure(old(rows)@, probe_name, probe_address)
                + if probe_name == name@ && probe_address == address@ { delta@ } else { 0 },
        old(rows)@.len() <= final(rows)@.len(),
        forall|index: int| #![trigger final(rows)@[index]] 0 <= index < old(rows)@.len() ==>
            final(rows)@[index].name@ == old(rows)@[index].name@ && final(rows)@[index].address@ == old(rows)@[index].address@,
        forall|index: int| #![trigger final(rows)@[index]] old(rows)@.len() <= index < final(rows)@.len() ==>
            final(rows)@[index].name@ == name@ && final(rows)@[index].address@ == address@,
        unique_suppliers(old(rows)@) ==> unique_suppliers(final(rows)@),
{
    let ghost before = rows@;
    let mut index = 0;
    while index < rows.len() && !(compare(&rows[index].name, name) == 0 && compare(&rows[index].address, address) == 0)
        invariant index <= rows.len(), rows@ == before,
            forall|offset: int| #![trigger before[offset]] 0 <= offset < index ==>
                !(before[offset].name@ == name@ && before[offset].address@ == address@),
        decreases rows.len() - index,
    {
        index += 1;
    }
    if index < rows.len() {
        let count = rows[index].count.add(delta);
        let mut item = rows.remove(index); item.count = count; rows.insert(index, item);
        proof {
            let replacement = rows@[index as int];
            assert(rows@ =~= before.update(index as int, replacement));
            assert forall|probe_name: Seq<u8>, probe_address: Seq<u8>| #[trigger] supplier_measure(rows@, probe_name, probe_address)
                == supplier_measure(before, probe_name, probe_address)
                    + if probe_name == name@ && probe_address == address@ { delta@ } else { 0 } by {
                lemma_sum_update(before, index as int, replacement, |row: SupplierVariant| if row.name@ == probe_name && row.address@ == probe_address { row.count@ } else { 0 });
            }
            assert forall|offset: int| #![trigger rows@[offset]] 0 <= offset < before.len() implies
                rows@[offset].name@ == before[offset].name@ && rows@[offset].address@ == before[offset].address@ by {
                if offset != index { assert(rows@[offset] == before[offset]); }
            }
        }
    } else {
        rows.push(SupplierVariant { name: copy_bytes(name), address: copy_bytes(address), count: delta.dup() });
        proof {
            assert forall|probe_name: Seq<u8>, probe_address: Seq<u8>| #[trigger] supplier_measure(rows@, probe_name, probe_address)
                == supplier_measure(before, probe_name, probe_address)
                    + if probe_name == name@ && probe_address == address@ { delta@ } else { 0 } by {
                lemma_sum_push(before, rows@.last(), |row: SupplierVariant| if row.name@ == probe_name && row.address@ == probe_address { row.count@ } else { 0 });
            }
            assert(rows@ =~= before.push(rows@.last()));
            assert forall|offset: int| #![trigger rows@[offset]] 0 <= offset < before.len() implies
                rows@[offset].name@ == before[offset].name@ && rows@[offset].address@ == before[offset].address@ by {}
        }
    }
}
/// Adds `delta` to the variant with this name and key, creating it when absent.
pub fn add_nation(rows: &mut Vec<NationVariant>, name: &Vec<u8>, key: i64, delta: &Wide)
    ensures
        forall|probe_name: Seq<u8>, probe_key: i64| #[trigger] nation_measure(final(rows)@, probe_name, probe_key)
            == nation_measure(old(rows)@, probe_name, probe_key)
                + if probe_name == name@ && probe_key == key { delta@ } else { 0 },
        old(rows)@.len() <= final(rows)@.len(),
        forall|index: int| #![trigger final(rows)@[index]] 0 <= index < old(rows)@.len() ==>
            final(rows)@[index].name@ == old(rows)@[index].name@ && final(rows)@[index].key == old(rows)@[index].key,
        forall|index: int| #![trigger final(rows)@[index]] old(rows)@.len() <= index < final(rows)@.len() ==>
            final(rows)@[index].name@ == name@ && final(rows)@[index].key == key,
        unique_nations(old(rows)@) ==> unique_nations(final(rows)@),
{
    let ghost before = rows@;
    let mut index = 0;
    while index < rows.len() && !(rows[index].key == key && compare(&rows[index].name, name) == 0)
        invariant index <= rows.len(), rows@ == before,
            forall|offset: int| #![trigger before[offset]] 0 <= offset < index ==>
                !(before[offset].name@ == name@ && before[offset].key == key),
        decreases rows.len() - index,
    {
        index += 1;
    }
    if index < rows.len() {
        let count = rows[index].count.add(delta);
        let mut item = rows.remove(index); item.count = count; rows.insert(index, item);
        proof {
            let replacement = rows@[index as int];
            assert(rows@ =~= before.update(index as int, replacement));
            assert forall|probe_name: Seq<u8>, probe_key: i64| #[trigger] nation_measure(rows@, probe_name, probe_key)
                == nation_measure(before, probe_name, probe_key)
                    + if probe_name == name@ && probe_key == key { delta@ } else { 0 } by {
                lemma_sum_update(before, index as int, replacement, |row: NationVariant| if row.name@ == probe_name && row.key == probe_key { row.count@ } else { 0 });
            }
            assert forall|offset: int| #![trigger rows@[offset]] 0 <= offset < before.len() implies
                rows@[offset].name@ == before[offset].name@ && rows@[offset].key == before[offset].key by {
                if offset != index { assert(rows@[offset] == before[offset]); }
            }
        }
    } else {
        rows.push(NationVariant { name: copy_bytes(name), key, count: delta.dup() });
        proof {
            assert forall|probe_name: Seq<u8>, probe_key: i64| #[trigger] nation_measure(rows@, probe_name, probe_key)
                == nation_measure(before, probe_name, probe_key)
                    + if probe_name == name@ && probe_key == key { delta@ } else { 0 } by {
                lemma_sum_push(before, rows@.last(), |row: NationVariant| if row.name@ == probe_name && row.key == probe_key { row.count@ } else { 0 });
            }
            assert(rows@ =~= before.push(rows@.last()));
            assert forall|offset: int| #![trigger rows@[offset]] 0 <= offset < before.len() implies
                rows@[offset].name@ == before[offset].name@ && rows@[offset].key == before[offset].key by {}
        }
    }
}
impl SupplierWork {
    pub fn new() -> (result: Self)
        ensures result.shape(), result.rows@.is_empty(),
    {
        let result = Self { rows: IntegerTripleMap::new(), keys: Vec::new() };
        proof { result.keys@.to_set_ensures(); assert(result.keys@.to_set().to_iset() =~= result.rows@.dom()); }
        result
    }
    pub fn change(&mut self, state: &Suppliers, row: &SupplierInput, sign: i128)
        requires old(self).shape(),
        ensures final(self).shape(),
            forall|nation: i64, supplier: i64, name: Seq<u8>, address: Seq<u8>|
                #[trigger] final(self).count(*state, nation, supplier, name, address) == old(self).count(*state, nation, supplier, name, address)
                    + if nation == row.s_nationkey && supplier == row.s_suppkey && name == row.s_name@ && address == row.s_address@ { sign as int } else { 0 },
    {
        let ghost previous = *self;
        proof { assert(previous.shape()); }
        let key = (row.s_nationkey, row.s_suppkey, 0);
        let mut rows = match self.rows.remove(&key) {
            Some(value) => value,
            None => {
                proof { assert(!previous.rows@.contains_key(key)); previous.keys@.to_set_ensures(); assert(!previous.keys@.contains(key)); verus_prooflib::lemma_unique_push(self.keys@, key); verus_prooflib::lemma_push_to_set(self.keys@, key); }
                self.keys.push(key);
                match state.get(key.0) {
                    Some(bucket) => match bucket.get(key.1) { Some(value) => copy_suppliers(&value), None => Vec::new() },
                    None => Vec::new(),
                }
            },
        };
        let ghost before = rows@;
        proof {
            assert forall|name: Seq<u8>, address: Seq<u8>| #[trigger] supplier_measure(before, name, address) == previous.count(*state, key.0, key.1, name, address) by {
                if !previous.rows@.contains_key(key) {
                    if let Some(bucket) = state.lookup(key.0) {
                        if let Some(source) = bucket.lookup(key.1) {
                            assert(supplier_measure(before, name, address) == supplier_measure(source@, name, address));
                        }
                    }
                }
            }
        }
        add_supplier(&mut rows, &row.s_name, &row.s_address, &Wide::from_i128(sign));
        self.rows.insert(key, rows);
        proof { assert(self.rows@.dom() =~= self.keys@.to_set().to_iset()); }
    }
    pub fn publish(self, state: &mut Suppliers) -> (result: bool)
        requires self.shape(),
        ensures result && suppliers_positive(*old(state)) ==> suppliers_positive(*final(state)),
            result ==> forall|nation: i64, supplier: i64, name: Seq<u8>, address: Seq<u8>|
                #[trigger] crate::model::supplier_count(*final(state), nation, supplier, name, address)
                    == self.count(*old(state), nation, supplier, name, address),
            self.ready(*old(state)) ==> result,
    {
        let ghost initial = *state;
        let mut work = self; let mut index = 0;
        while index < work.keys.len()
            invariant index <= work.keys.len(), work.keys@ == self.keys@, self.shape(), initial == *old(state),
                work.rows@ == self.rows@.remove_keys(self.keys@.take(index as int).to_set().to_iset()),
                suppliers_positive(initial) ==> suppliers_positive(*state),
                forall|nation: i64, supplier: i64, name: Seq<u8>, address: Seq<u8>|
                    #[trigger] crate::model::supplier_count(*state, nation, supplier, name, address) ==
                        if self.keys@.take(index as int).contains((nation, supplier, 0)) { self.count(initial, nation, supplier, name, address) }
                        else { crate::model::supplier_count(initial, nation, supplier, name, address) },
            decreases work.keys.len() - index,
        {
            let key = work.keys[index];
            proof { verus_prooflib::lemma_prefix_fresh(self.keys@, index as int); self.keys@.to_set_ensures(); }
            let ghost before_work = work.rows@;
            proof {
                self.keys@.take(index as int).to_set_ensures();
                assert(self.keys@.contains(key));
                assert(!self.keys@.take(index as int).contains(key));
                assert(self.rows@.contains_key(key));
                assert(work.rows@.contains_key(key));
            }
            let mut raw = match work.rows.remove(&key) { Some(value) => value, None => { proof { assert(false); } return false; } };
            let ghost source = raw@;
            let ghost remaining = work.rows@;
            proof { assert(source == self.rows@[key]@); assert(remaining == before_work.remove(key)); }
            let mut merged: Vec<SupplierVariant> = Vec::new();
            while raw.len() > 0
                invariant unique_suppliers(merged@),
                    forall|name: Seq<u8>, address: Seq<u8>| #[trigger] supplier_measure(merged@, name, address) + supplier_measure(raw@, name, address) == supplier_measure(source, name, address),
                decreases raw.len(),
            {
                let ghost before = raw@;
                let ghost accepted = merged@;
                let row = match raw.pop() { Some(value) => value, None => { proof { assert(false); } return false; } };
                add_supplier(&mut merged, &row.name, &row.address, &row.count);
                proof {
                    assert(before == before.drop_last().push(row));
                    assert(raw@ == before.drop_last());
                    assert forall|name: Seq<u8>, address: Seq<u8>| #[trigger] supplier_measure(merged@, name, address) + supplier_measure(raw@, name, address) == supplier_measure(source, name, address) by {
                        let weight = |row: SupplierVariant| if row.name@ == name && row.address@ == address { row.count@ } else { 0 };
                        assert(supplier_measure(accepted, name, address) + supplier_measure(before, name, address) == supplier_measure(source, name, address));
                        lemma_sum_push(before.drop_last(), row, weight);
                    }
                }
            }
            let ghost merged_all = merged@;
            proof {
                assert(self.keys@[index as int].2 == 0); assert(key == (key.0, key.1, 0i64));
                if self.ready(initial) {
                    assert forall|offset: int| #![trigger merged@[offset]] 0 <= offset < merged@.len() implies merged@[offset].count@ >= 0 by {
                        lemma_unique_supplier_count(merged@, offset);
                        assert(self.count(initial, key.0, key.1, merged@[offset].name@, merged@[offset].address@) >= 0);
                        assert(supplier_measure(raw@, merged@[offset].name@, merged@[offset].address@) == 0);
                    }
                }
            }
            let mut rows: Vec<SupplierVariant> = Vec::new();
            while merged.len() > 0
                invariant initial == *old(state), forall|offset: int| #![trigger rows@[offset]] 0 <= offset < rows@.len() ==> rows@[offset].count@ > 0,
                    self.ready(initial) ==> forall|offset: int| #![trigger merged@[offset]] 0 <= offset < merged@.len() ==> merged@[offset].count@ >= 0,
                    forall|name: Seq<u8>, address: Seq<u8>| #[trigger] supplier_measure(rows@, name, address) + supplier_measure(merged@, name, address) == supplier_measure(merged_all, name, address),
                decreases merged.len(),
            {
                let ghost before = merged@;
                let ghost accepted = rows@;
                let row = match merged.pop() { Some(value) => value, None => { proof { assert(false); } return false; } };
                if row.count.is_neg() { proof { assert(!self.ready(initial)); assert(before.last() == row); } return false; }
                if row.count.is_pos() { rows.push(row); }
                proof {
                    assert forall|name: Seq<u8>, address: Seq<u8>| #[trigger] supplier_measure(rows@, name, address) + supplier_measure(merged@, name, address) == supplier_measure(merged_all, name, address) by {
                        let weight = |row: SupplierVariant| if row.name@ == name && row.address@ == address { row.count@ } else { 0 };
                        assert(supplier_measure(accepted, name, address) + supplier_measure(before, name, address) == supplier_measure(merged_all, name, address));
                        assert(before == before.drop_last().push(row));
                        assert(merged@ == before.drop_last());
                        lemma_sum_push(before.drop_last(), row, weight);
                        if row.count@ > 0 { lemma_sum_push(accepted, row, weight); }
                    }
                    if self.ready(initial) {
                        assert forall|offset: int| #![trigger merged@[offset]] 0 <= offset < merged@.len() implies merged@[offset].count@ >= 0 by {
                            assert(merged@[offset] == before[offset]);
                        }
                    }
                }
            }
            let ghost previous_state = *state;
            let ghost published_rows = rows@;
            proof {
                assert forall|name: Seq<u8>, address: Seq<u8>| #[trigger] supplier_measure(published_rows, name, address) == supplier_measure(source, name, address) by {
                    assert(supplier_measure(published_rows, name, address) + supplier_measure(merged@, name, address) == supplier_measure(merged_all, name, address));
                    assert(supplier_measure(merged_all, name, address) + supplier_measure(raw@, name, address) == supplier_measure(source, name, address));
                }
            }
            let mut bucket = match state.get(key.0) { Some(value) => value.share(), None => Table::new() };
            if rows.len() == 0 { bucket.put(key.1, None); } else { bucket.put(key.1, Some(Arc::new(rows))); }
            state.put(key.0, Some(Arc::new(bucket)));
            proof {
                assert(work.rows@ == remaining);
                assert(self.keys@.take(index as int + 1) =~= self.keys@.take(index as int).push(key));
                verus_prooflib::lemma_push_to_set(self.keys@.take(index as int), key);
                verus_prooflib::lemma_push_membership(self.keys@.take(index as int), key);
                assert forall|other: IntegerTriple| #[trigger] work.rows@.contains_key(other) <==> self.rows@.contains_key(other) && !self.keys@.take(index as int + 1).contains(other) by {
                    assert(work.rows@.contains_key(other) == (before_work.contains_key(other) && other != key));
                    self.keys@.take(index as int).to_set_ensures();
                    self.keys@.take(index as int + 1).to_set_ensures();
                }
                assert(work.rows@ =~= self.rows@.remove_keys(self.keys@.take(index as int + 1).to_set().to_iset()));
                assert forall|nation: i64, supplier: i64, name: Seq<u8>, address: Seq<u8>|
                    #[trigger] crate::model::supplier_count(*state, nation, supplier, name, address) ==
                        if self.keys@.take(index as int + 1).contains((nation, supplier, 0)) { self.count(initial, nation, supplier, name, address) }
                        else { crate::model::supplier_count(initial, nation, supplier, name, address) } by {
                    if (nation, supplier, 0) == key {
                        assert(supplier_measure(source, name, address) == self.count(initial, nation, supplier, name, address));
                        assert(supplier_measure(published_rows, name, address) == supplier_measure(source, name, address));
                        assert(crate::model::supplier_count(*state, nation, supplier, name, address) == supplier_measure(published_rows, name, address));
                    } else {
                        assert(crate::model::supplier_count(*state, nation, supplier, name, address) == crate::model::supplier_count(previous_state, nation, supplier, name, address));
                        assert(crate::model::supplier_count(previous_state, nation, supplier, name, address) ==
                            if self.keys@.take(index as int).contains((nation, supplier, 0)) { self.count(initial, nation, supplier, name, address) }
                            else { crate::model::supplier_count(initial, nation, supplier, name, address) });
                    }
                }
            }
            index += 1;
        }
        proof { assert(self.keys@.take(self.keys@.len() as int) =~= self.keys@); self.keys@.to_set_ensures(); }
        true
    }
}
impl NationWork {
    pub fn new() -> (result: Self)
        ensures result.shape(), result.hashes(), result.rows@.is_empty(),
    {
        let result = Self { rows: HashMap::new(), keys: Vec::new() };
        proof { result.keys@.to_set_ensures(); assert(result.keys@.to_set() =~= result.rows@.dom()); }
        result
    }
    pub fn change(&mut self, state: &Nations, row: &NationInput, sign: i128)
        requires old(self).shape(),
        ensures final(self).shape(),
            old(self).hashes() && nations_positive(*state) ==> final(self).hashes(),
            forall|name: Seq<u8>, nation: i64| #[trigger] final(self).count(*state, name, nation)
                == old(self).count(*state, name, nation)
                    + if name == row.n_name@ && nation == row.n_nationkey { sign as int } else { 0 },
    {
        let ghost previous = *self;
        let key = hash(&row.n_name);
        let mut rows = match self.rows.remove(&key) {
            Some(value) => value,
            None => {
                proof { previous.keys@.to_set_ensures(); assert(!previous.keys@.contains(key)); verus_prooflib::lemma_unique_push(self.keys@, key); verus_prooflib::lemma_push_to_set(self.keys@, key); }
                self.keys.push(key); match state.get(key) { Some(value) => copy_nations(&value), None => Vec::new() }
            },
        };
        let ghost before = rows@;
        proof {
            assert forall|name: Seq<u8>, nation: i64| #![trigger nation_measure(before, name, nation)] name_hash(name) == key implies
                nation_measure(before, name, nation) == previous.count(*state, name, nation) by {
                if !previous.rows@.contains_key(key) {
                    if let Some(source) = state.lookup(key) { assert(nation_measure(before, name, nation) == nation_measure(source@, name, nation)); }
                }
            }
            if previous.hashes() && nations_positive(*state) {
                assert forall|offset: int| #![trigger rows@[offset]] 0 <= offset < rows@.len() implies name_hash(rows@[offset].name@) == key by {
                    if previous.rows@.contains_key(key) { assert(rows@[offset] == previous.rows@[key]@[offset]); }
                    else if let Some(source) = state.lookup(key) { assert(rows@[offset].name@ == source@[offset].name@); }
                }
            }
        }
        add_nation(&mut rows, &row.n_name, row.n_nationkey, &Wide::from_i128(sign));
        proof {
            if previous.hashes() && nations_positive(*state) {
                assert forall|offset: int| #![trigger rows@[offset]] 0 <= offset < rows@.len() implies name_hash(rows@[offset].name@) == key by {
                    if offset >= before.len() { assert(rows@[offset].name@ == row.n_name@); }
                }
            }
        }
        self.rows.insert(key, rows);
        proof {
            assert(self.rows@.dom() =~= self.keys@.to_set());
            assert forall|name: Seq<u8>, nation: i64| #[trigger] self.count(*state, name, nation) == previous.count(*state, name, nation)
                + if name == row.n_name@ && nation == row.n_nationkey { sign as int } else { 0 } by {
                if name_hash(name) == key { assert(nation_measure(before, name, nation) == previous.count(*state, name, nation)); }
                else { assert(name != row.n_name@); }
            }
        }
    }
    pub fn publish(self, state: &mut Nations) -> (result: bool)
        requires self.shape(),
        ensures result && self.hashes() && nations_positive(*old(state)) ==> nations_positive(*final(state)),
            result ==> forall|name: Seq<u8>, nation: i64| #[trigger] crate::model::nation_count(*final(state), name, nation) == self.count(*old(state), name, nation),
            self.ready(*old(state)) && self.hashes() ==> result,
    {
        let ghost initial = *state;
        let mut work = self; let mut index = 0;
        while index < work.keys.len()
            invariant index <= work.keys.len(), work.keys@ == self.keys@, self.shape(), initial == *old(state),
                work.rows@ == self.rows@.remove_keys(self.keys@.take(index as int).to_set()),
                self.hashes() && nations_positive(initial) ==> nations_positive(*state),
                forall|name: Seq<u8>, nation: i64| #[trigger] crate::model::nation_count(*state, name, nation) ==
                    if self.keys@.take(index as int).contains(name_hash(name) as i64) { self.count(initial, name, nation) }
                    else { crate::model::nation_count(initial, name, nation) },
            decreases work.keys.len() - index,
        {
            let key = work.keys[index];
            proof { verus_prooflib::lemma_prefix_fresh(self.keys@, index as int); self.keys@.to_set_ensures(); }
            let ghost before_work = work.rows@;
            proof {
                self.keys@.take(index as int).to_set_ensures();
                assert(self.keys@.contains(key));
                assert(!self.keys@.take(index as int).contains(key));
                assert(self.rows@.contains_key(key));
                assert(work.rows@.contains_key(key));
            }
            let mut raw = match work.rows.remove(&key) { Some(value) => value, None => { proof { assert(false); } return false; } };
            let ghost source = raw@;
            let ghost remaining = work.rows@;
            proof { assert(source == self.rows@[key]@); assert(remaining == before_work.remove(key)); }
            let mut merged: Vec<NationVariant> = Vec::new();
            while raw.len() > 0
                invariant unique_nations(merged@),
                    self.hashes() ==> forall|offset: int| #![trigger merged@[offset]] 0 <= offset < merged@.len() ==> name_hash(merged@[offset].name@) == key,
                    self.hashes() ==> forall|offset: int| #![trigger raw@[offset]] 0 <= offset < raw@.len() ==> name_hash(raw@[offset].name@) == key,
                    forall|name: Seq<u8>, nation: i64| #[trigger] nation_measure(merged@, name, nation) + nation_measure(raw@, name, nation) == nation_measure(source, name, nation),
                decreases raw.len(),
            {
                let ghost before = raw@;
                let ghost accepted = merged@;
                let row = match raw.pop() { Some(value) => value, None => { proof { assert(false); } return false; } };
                add_nation(&mut merged, &row.name, row.key, &row.count);
                proof {
                    assert(before == before.drop_last().push(row));
                    assert(raw@ == before.drop_last());
                    assert(before.last() == row);
                    assert forall|name: Seq<u8>, nation: i64| #[trigger] nation_measure(merged@, name, nation) + nation_measure(raw@, name, nation) == nation_measure(source, name, nation) by {
                        let weight = |row: NationVariant| if row.name@ == name && row.key == nation { row.count@ } else { 0 };
                        assert(nation_measure(accepted, name, nation) + nation_measure(before, name, nation) == nation_measure(source, name, nation));
                        lemma_sum_push(before.drop_last(), row, weight);
                    }
                    if self.hashes() {
                        assert forall|offset: int| #![trigger merged@[offset]] 0 <= offset < merged@.len() implies name_hash(merged@[offset].name@) == key by {
                            if offset >= accepted.len() { assert(merged@[offset].name@ == row.name@); }
                        }
                    }
                }
            }
            let ghost merged_all = merged@;
            proof {
                if self.ready(initial) && self.hashes() {
                    assert forall|offset: int| #![trigger merged@[offset]] 0 <= offset < merged@.len() implies merged@[offset].count@ >= 0 by {
                        lemma_unique_nation_count(merged@, offset);
                        let name = merged@[offset].name@;
                        assert(name_hash(name) == key);
                        assert(self.count(initial, name, merged@[offset].key) >= 0);
                        assert(nation_measure(raw@, name, merged@[offset].key) == 0);
                    }
                }
            }
            let mut rows: Vec<NationVariant> = Vec::new();
            while merged.len() > 0
                invariant initial == *old(state), forall|offset: int| #![trigger rows@[offset]] 0 <= offset < rows@.len() ==>
                        rows@[offset].count@ > 0 && (self.hashes() ==> name_hash(rows@[offset].name@) == key),
                    self.hashes() ==> forall|offset: int| #![trigger merged@[offset]] 0 <= offset < merged@.len() ==> name_hash(merged@[offset].name@) == key,
                    self.ready(initial) && self.hashes() ==> forall|offset: int| #![trigger merged@[offset]] 0 <= offset < merged@.len() ==> merged@[offset].count@ >= 0,
                    forall|name: Seq<u8>, nation: i64| #[trigger] nation_measure(rows@, name, nation) + nation_measure(merged@, name, nation) == nation_measure(merged_all, name, nation),
                decreases merged.len(),
            {
                let ghost before = merged@;
                let ghost accepted = rows@;
                let row = match merged.pop() { Some(value) => value, None => { proof { assert(false); } return false; } };
                if row.count.is_neg() { proof { assert(!(self.ready(initial) && self.hashes())); assert(before.last() == row); } return false; }
                if row.count.is_pos() { rows.push(row); }
                proof {
                    if self.ready(initial) && self.hashes() {
                        assert forall|offset: int| #![trigger merged@[offset]] 0 <= offset < merged@.len() implies merged@[offset].count@ >= 0 by {
                            assert(merged@[offset] == before[offset]);
                        }
                    }
                    assert forall|name: Seq<u8>, nation: i64| #[trigger] nation_measure(rows@, name, nation) + nation_measure(merged@, name, nation) == nation_measure(merged_all, name, nation) by {
                        let weight = |row: NationVariant| if row.name@ == name && row.key == nation { row.count@ } else { 0 };
                        assert(nation_measure(accepted, name, nation) + nation_measure(before, name, nation) == nation_measure(merged_all, name, nation));
                        assert(before == before.drop_last().push(row));
                        assert(merged@ == before.drop_last());
                        lemma_sum_push(before.drop_last(), row, weight);
                        if row.count@ > 0 { lemma_sum_push(accepted, row, weight); }
                    }
                    assert(merged@ == before.drop_last());
                    assert(before.last() == row);
                    assert forall|offset: int| #![trigger rows@[offset]] 0 <= offset < rows@.len() implies
                        rows@[offset].count@ > 0 && (self.hashes() ==> name_hash(rows@[offset].name@) == key) by {
                        if offset < accepted.len() { assert(rows@[offset] == accepted[offset]); } else { assert(rows@[offset] == row); }
                    }
                }
            }
            let ghost previous_state = *state;
            let ghost published_rows = rows@;
            proof {
                assert forall|name: Seq<u8>, nation: i64| #[trigger] nation_measure(published_rows, name, nation) == nation_measure(source, name, nation) by {
                    assert(nation_measure(published_rows, name, nation) + nation_measure(merged@, name, nation) == nation_measure(merged_all, name, nation));
                    assert(nation_measure(merged_all, name, nation) + nation_measure(raw@, name, nation) == nation_measure(source, name, nation));
                }
            }
            if rows.len() == 0 { state.put(key, None); } else { state.put(key, Some(Arc::new(rows))); }
            proof {
                assert(work.rows@ == remaining);
                assert(self.keys@.take(index as int + 1) =~= self.keys@.take(index as int).push(key));
                verus_prooflib::lemma_push_membership(self.keys@.take(index as int), key);
                verus_prooflib::lemma_push_to_set(self.keys@.take(index as int), key);
                assert(work.rows@ =~= self.rows@.remove_keys(self.keys@.take(index as int + 1).to_set()));
                assert forall|name: Seq<u8>, nation: i64| #[trigger] crate::model::nation_count(*state, name, nation) ==
                    if self.keys@.take(index as int + 1).contains(name_hash(name) as i64) { self.count(initial, name, nation) }
                    else { crate::model::nation_count(initial, name, nation) } by {
                    if name_hash(name) == key {
                        assert(nation_measure(source, name, nation) == self.count(initial, name, nation));
                        assert(nation_measure(published_rows, name, nation) == nation_measure(source, name, nation));
                        assert(crate::model::nation_count(*state, name, nation) == nation_measure(published_rows, name, nation));
                    } else { assert(crate::model::nation_count(*state, name, nation) == crate::model::nation_count(previous_state, name, nation)); }
                }
            }
            index += 1;
        }
        proof { assert(self.keys@.take(self.keys@.len() as int) =~= self.keys@); self.keys@.to_set_ensures(); }
        true
    }
}
impl NameWork {
    pub open spec fn shape(self) -> bool {
        &&& self.keys@.no_duplicates() && self.rows@.dom() == self.keys@.to_set()
        &&& forall|slot: i64, index: int| #![trigger self.rows@[slot]@[index]]
            self.rows@.contains_key(slot) && 0 <= index < self.rows@[slot]@.len() ==>
                name_slot(self.rows@[slot]@[index].key, self.rows@[slot]@[index].name@) == slot
    }
    pub open spec fn count(self, state: PartNames, key: i64, name: Seq<u8>) -> int {
        if self.rows@.contains_key(name_slot(key, name)) { nation_measure(self.rows@[name_slot(key, name)]@, name, key) }
        else { crate::model::name_count(state, key, name) }
    }
    pub open spec fn ready(self, state: PartNames) -> bool {
        forall|key: i64, name: Seq<u8>| #[trigger] self.count(state, key, name) >= 0
    }
    pub fn new() -> (result: Self)
        ensures result.shape(), result.rows@.is_empty(),
    {
        let result = Self { rows: HashMap::new(), keys: Vec::new() };
        proof { result.keys@.to_set_ensures(); assert(result.keys@.to_set() =~= result.rows@.dom()); }
        result
    }
    #[verifier::rlimit(100)]
    pub fn change(&mut self, state: &PartNames, row: &PartInput, sign: i128)
        requires old(self).shape(), names_positive(*state),
        ensures final(self).shape(),
            forall|key: i64, name: Seq<u8>| #[trigger] final(self).count(*state, key, name)
                == old(self).count(*state, key, name)
                    + if key == row.p_partkey && name == row.p_name@ { sign as int } else { 0 },
    {
        let ghost previous = *self;
        let key = row.p_partkey;
        let slot = name_slot_exec(key, row.p_name.as_slice());
        let mut rows = match self.rows.remove(&slot) {
            Some(value) => value,
            None => {
                proof { previous.keys@.to_set_ensures(); assert(!previous.keys@.contains(slot)); verus_prooflib::lemma_unique_push(self.keys@, slot); verus_prooflib::lemma_push_to_set(self.keys@, slot); }
                self.keys.push(slot); match state.get(slot) { Some(value) => copy_nations(&value), None => Vec::new() }
            },
        };
        let ghost before = rows@;
        proof {
            assert forall|other: i64, name: Seq<u8>| #![trigger previous.count(*state, other, name)] name_slot(other, name) == slot implies
                nation_measure(before, name, other) == previous.count(*state, other, name) by {
                if !previous.rows@.contains_key(slot) {
                    if let Some(source) = state.lookup(slot) { assert(nation_measure(before, name, other) == nation_measure(source@, name, other)); }
                }
            }
            assert forall|index: int| #![trigger before[index]] 0 <= index < before.len() implies
                name_slot(before[index].key, before[index].name@) == slot by {
                if previous.rows@.contains_key(slot) {
                    assert(before == previous.rows@[slot]@);
                    assert(name_slot(previous.rows@[slot]@[index].key, previous.rows@[slot]@[index].name@) == slot);
                } else {
                    if let Some(source) = state.lookup(slot) { lemma_names_row(*state, slot, source, index); }
                }
            }
        }
        add_nation(&mut rows, &row.p_name, key, &Wide::from_i128(sign));
        let ghost updated = rows@;
        proof {
            assert(name_slot(key, row.p_name@) == slot);
            assert forall|offset: int| #![trigger updated[offset]] 0 <= offset < updated.len() implies
                name_slot(updated[offset].key, updated[offset].name@) == slot by {
                if offset < before.len() {} else { assert(updated[offset].key == key && updated[offset].name@ == row.p_name@); }
            }
        }
        self.rows.insert(slot, rows);
        proof {
            assert(self.rows@.dom() =~= self.keys@.to_set());
            assert forall|other: i64, name: Seq<u8>| #[trigger] self.count(*state, other, name) == previous.count(*state, other, name)
                + if other == key && name == row.p_name@ { sign as int } else { 0 } by {
                if name_slot(other, name) == slot {
                    assert(nation_measure(before, name, other) == previous.count(*state, other, name));
                    assert(self.rows@.contains_key(slot) && self.rows@[slot]@ == updated);
                }
            }
            assert forall|s: i64, offset: int| #![trigger self.rows@[s]@[offset]] self.rows@.contains_key(s) && 0 <= offset < self.rows@[s]@.len() implies
                name_slot(self.rows@[s]@[offset].key, self.rows@[s]@[offset].name@) == s by {
                if s != slot { assert(previous.rows@.contains_key(s)); }
            }
        }
    }
    pub fn publish(self, state: &mut PartNames) -> (result: bool)
        requires self.shape(),
        ensures result && names_positive(*old(state)) ==> names_positive(*final(state)),
            result ==> forall|key: i64, name: Seq<u8>| #[trigger] crate::model::name_count(*final(state), key, name) == self.count(*old(state), key, name),
            self.ready(*old(state)) ==> result,
    {
        let ghost initial = *state;
        let mut work = self; let mut index = 0;
        while index < work.keys.len()
            invariant index <= work.keys.len(), work.keys@ == self.keys@, self.shape(), initial == *old(state),
                work.rows@ == self.rows@.remove_keys(self.keys@.take(index as int).to_set()),
                names_positive(initial) ==> names_positive(*state),
                forall|other: i64, name: Seq<u8>| #[trigger] crate::model::name_count(*state, other, name) ==
                    if self.keys@.take(index as int).contains(name_slot(other, name)) { self.count(initial, other, name) }
                    else { crate::model::name_count(initial, other, name) },
            decreases work.keys.len() - index,
        {
            let slot = work.keys[index];
            proof { verus_prooflib::lemma_prefix_fresh(self.keys@, index as int); self.keys@.to_set_ensures(); }
            let ghost before_work = work.rows@;
            proof {
                self.keys@.take(index as int).to_set_ensures();
                assert(self.keys@.contains(slot));
                assert(!self.keys@.take(index as int).contains(slot));
                assert(self.rows@.contains_key(slot));
                assert(work.rows@.contains_key(slot));
            }
            let mut raw = match work.rows.remove(&slot) { Some(value) => value, None => { proof { assert(false); } return false; } };
            let ghost source = raw@;
            let ghost remaining = work.rows@;
            proof { assert(source == self.rows@[slot]@); assert(remaining == before_work.remove(slot)); }
            let mut merged: Vec<NationVariant> = Vec::new();
            while raw.len() > 0
                invariant unique_nations(merged@),
                    forall|offset: int| #![trigger merged@[offset]] 0 <= offset < merged@.len() ==> name_slot(merged@[offset].key, merged@[offset].name@) == slot,
                    forall|offset: int| #![trigger raw@[offset]] 0 <= offset < raw@.len() ==> name_slot(raw@[offset].key, raw@[offset].name@) == slot,
                    forall|name: Seq<u8>, other: i64| #[trigger] nation_measure(merged@, name, other) + nation_measure(raw@, name, other) == nation_measure(source, name, other),
                decreases raw.len(),
            {
                let ghost before = raw@;
                let ghost accepted = merged@;
                let row = match raw.pop() { Some(value) => value, None => { proof { assert(false); } return false; } };
                add_nation(&mut merged, &row.name, row.key, &row.count);
                proof {
                    assert(before == before.drop_last().push(row));
                    assert(raw@ == before.drop_last());
                    assert(before.last() == row);
                    assert forall|offset: int| #![trigger raw@[offset]] 0 <= offset < raw@.len() implies name_slot(raw@[offset].key, raw@[offset].name@) == slot by {
                        assert(raw@[offset] == before[offset]);
                    }
                    assert forall|name: Seq<u8>, other: i64| #[trigger] nation_measure(merged@, name, other) + nation_measure(raw@, name, other) == nation_measure(source, name, other) by {
                        let weight = |row: NationVariant| if row.name@ == name && row.key == other { row.count@ } else { 0 };
                        assert(nation_measure(accepted, name, other) + nation_measure(before, name, other) == nation_measure(source, name, other));
                        lemma_sum_push(before.drop_last(), row, weight);
                    }
                    assert forall|offset: int| #![trigger merged@[offset]] 0 <= offset < merged@.len() implies name_slot(merged@[offset].key, merged@[offset].name@) == slot by {
                        if offset >= accepted.len() { assert(merged@[offset].name@ == row.name@ && merged@[offset].key == row.key); }
                    }
                }
            }
            let ghost merged_all = merged@;
            proof {
                if self.ready(initial) {
                    assert forall|offset: int| #![trigger merged@[offset]] 0 <= offset < merged@.len() implies merged@[offset].count@ >= 0 by {
                        lemma_unique_nation_count(merged@, offset);
                        let name = merged@[offset].name@;
                        let other = merged@[offset].key;
                        assert(name_slot(other, name) == slot);
                        assert(self.count(initial, other, name) >= 0);
                        assert(nation_measure(raw@, name, other) == 0);
                    }
                }
            }
            let mut rows: Vec<NationVariant> = Vec::new();
            while merged.len() > 0
                invariant initial == *old(state), forall|offset: int| #![trigger rows@[offset]] 0 <= offset < rows@.len() ==> rows@[offset].count@ > 0 && name_slot(rows@[offset].key, rows@[offset].name@) == slot,
                    forall|offset: int| #![trigger merged@[offset]] 0 <= offset < merged@.len() ==> name_slot(merged@[offset].key, merged@[offset].name@) == slot,
                    self.ready(initial) ==> forall|offset: int| #![trigger merged@[offset]] 0 <= offset < merged@.len() ==> merged@[offset].count@ >= 0,
                    forall|name: Seq<u8>, other: i64| #[trigger] nation_measure(rows@, name, other) + nation_measure(merged@, name, other) == nation_measure(merged_all, name, other),
                decreases merged.len(),
            {
                let ghost before = merged@;
                let ghost accepted = rows@;
                let row = match merged.pop() { Some(value) => value, None => { proof { assert(false); } return false; } };
                if row.count.is_neg() { proof { assert(!self.ready(initial)); assert(before.last() == row); } return false; }
                if row.count.is_pos() { rows.push(row); }
                proof {
                    if self.ready(initial) {
                        assert forall|offset: int| #![trigger merged@[offset]] 0 <= offset < merged@.len() implies merged@[offset].count@ >= 0 by {
                            assert(merged@[offset] == before[offset]);
                        }
                    }
                    assert(before == before.drop_last().push(row));
                    assert(merged@ == before.drop_last());
                    assert(before.last() == row);
                    assert forall|name: Seq<u8>, other: i64| #[trigger] nation_measure(rows@, name, other) + nation_measure(merged@, name, other) == nation_measure(merged_all, name, other) by {
                        let weight = |row: NationVariant| if row.name@ == name && row.key == other { row.count@ } else { 0 };
                        assert(nation_measure(accepted, name, other) + nation_measure(before, name, other) == nation_measure(merged_all, name, other));
                        lemma_sum_push(before.drop_last(), row, weight);
                        if row.count@ > 0 { lemma_sum_push(accepted, row, weight); }
                    }
                    assert forall|offset: int| #![trigger merged@[offset]] 0 <= offset < merged@.len() implies name_slot(merged@[offset].key, merged@[offset].name@) == slot by {
                        assert(merged@[offset] == before[offset]);
                    }
                    assert forall|offset: int| #![trigger rows@[offset]] 0 <= offset < rows@.len() implies rows@[offset].count@ > 0 && name_slot(rows@[offset].key, rows@[offset].name@) == slot by {
                        if offset < accepted.len() { assert(rows@[offset] == accepted[offset]); } else { assert(rows@[offset] == row); }
                    }
                }
            }
            let ghost previous_state = *state;
            let ghost published_rows = rows@;
            proof {
                assert forall|name: Seq<u8>, other: i64| #[trigger] nation_measure(published_rows, name, other) == nation_measure(source, name, other) by {
                    assert(nation_measure(published_rows, name, other) + nation_measure(merged@, name, other) == nation_measure(merged_all, name, other));
                    assert(nation_measure(merged_all, name, other) + nation_measure(raw@, name, other) == nation_measure(source, name, other));
                }
            }
            if rows.len() == 0 { state.put(slot, None); } else { state.put(slot, Some(Arc::new(rows))); }
            proof {
                assert(work.rows@ == remaining);
                assert(self.keys@.take(index as int + 1) =~= self.keys@.take(index as int).push(slot));
                verus_prooflib::lemma_push_membership(self.keys@.take(index as int), slot);
                verus_prooflib::lemma_push_to_set(self.keys@.take(index as int), slot);
                assert(work.rows@ =~= self.rows@.remove_keys(self.keys@.take(index as int + 1).to_set()));
                assert forall|other: i64, name: Seq<u8>| #[trigger] crate::model::name_count(*state, other, name) ==
                    if self.keys@.take(index as int + 1).contains(name_slot(other, name)) { self.count(initial, other, name) }
                    else { crate::model::name_count(initial, other, name) } by {
                    if name_slot(other, name) == slot {
                        assert(nation_measure(source, name, other) == self.count(initial, other, name));
                        assert(nation_measure(published_rows, name, other) == nation_measure(source, name, other));
                        assert(crate::model::name_count(*state, other, name) == nation_measure(published_rows, name, other));
                    } else { assert(crate::model::name_count(*state, other, name) == crate::model::name_count(previous_state, other, name)); }
                }
            }
            index += 1;
        }
        proof { assert(self.keys@.take(self.keys@.len() as int) =~= self.keys@); self.keys@.to_set_ensures(); }
        true
    }
}
}
