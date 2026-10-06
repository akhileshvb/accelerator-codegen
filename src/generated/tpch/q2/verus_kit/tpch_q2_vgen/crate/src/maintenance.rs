use vstd::prelude::*;
use std::sync::Arc;
use crate::gen_obligations::*;
use crate::bags::*;
use crate::gen_spec::PartRow;
use crate::model::*;
use crate::storage::*;
use verus_common::sums::*;

verus! {

pub open spec fn index_amount<Row: BagRow>(directory: Directory<Bag<Row>>, key: i64, value: Row::V) -> int {
    match directory.lookup(key) { None => 0, Some(bag) => (*bag).amount(value) }
}

pub open spec fn index_weight<Row: BagRow>(directory: Directory<Bag<Row>>, key: i64, weight: spec_fn(Row::V) -> int) -> int {
    match directory.lookup(key) { None => 0, Some(bag) => (*bag).weight(weight) }
}

pub open spec fn index_unique<Row: BagRow>(directory: Directory<Bag<Row>>) -> bool {
    forall|key: i64| match #[trigger] directory.lookup(key) { None => true, Some(bag) => (*bag).unique() }
}

pub open spec fn index_positive<Row: BagRow>(directory: Directory<Bag<Row>>) -> bool {
    forall|key: i64| match #[trigger] directory.lookup(key) { None => true, Some(bag) => (*bag).valid() }
}

pub open spec fn index_nonempty<Row: BagRow>(directory: Directory<Bag<Row>>) -> bool {
    forall|key: i64| match #[trigger] directory.lookup(key) { None => true, Some(bag) => (*bag).entries@.len() > 0 }
}

pub open spec fn index_routed<Row: BagRow>(directory: Directory<Bag<Row>>, predicate: spec_fn(i64, Row::V) -> bool) -> bool {
    forall|key: i64| match #[trigger] directory.lookup(key) {
        None => true,
        Some(bag) => forall|index: int| 0 <= index < (*bag).entries@.len() ==>
            predicate(key, (*((#[trigger] (*bag).entries@[index]).row)).rv()),
    }
}

pub fn adjust_bag<Row: BagRow>(directory: &mut Directory<Bag<Row>>, key: i64, row: Arc<Row>, change: i128) -> (result: bool)
    requires old(directory).valid(),
    ensures result ==> {
        &&& final(directory).valid()
        &&& forall|probe: i64| probe != key ==> #[trigger] final(directory).lookup(probe) == old(directory).lookup(probe)
        &&& index_unique(*old(directory)) ==> index_unique(*final(directory))
        &&& index_positive(*old(directory)) && change > 0 ==> index_positive(*final(directory))
        &&& index_nonempty(*old(directory)) ==> index_nonempty(*final(directory))
        &&& forall|predicate: spec_fn(i64, Row::V) -> bool| #[trigger] index_routed(*old(directory), predicate) && predicate(key, (*row).rv()) ==>
            index_routed(*final(directory), predicate)
        &&& forall|probe: i64, value: Row::V| #[trigger] index_amount(*final(directory), probe, value) ==
            index_amount(*old(directory), probe, value) + if probe == key && (*row).rv() == value { change as int } else { 0 }
        &&& forall|probe: i64, weight: spec_fn(Row::V) -> int| #[trigger] index_weight(*final(directory), probe, weight) ==
            index_weight(*old(directory), probe, weight) + if probe == key { change as int * weight((*row).rv()) } else { 0 }
    },
{
    let empty = Bag::new();
    let old_bag = directory.get(key);
    let before = match &old_bag { Some(bag) => &**bag, None => &empty };
    let ghost original = *directory;
    let after = match before.adjust(Arc::clone(&row), change) { None => return false, Some(bag) => bag };
    let ghost after_bag = after;
    if after.entries.len() == 0 { directory.replace(key, None); }
    else { directory.replace(key, Some(Arc::new(after))); }
    proof {
        assert forall|predicate: spec_fn(i64, Row::V) -> bool| #[trigger] index_routed(original, predicate) && predicate(key, (*row).rv()) implies
            index_routed(*directory, predicate) by {
            assert forall|probe: i64| match #[trigger] directory.lookup(probe) {
                None => true,
                Some(bag) => forall|index: int| 0 <= index < (*bag).entries@.len() ==>
                    predicate(probe, (*((#[trigger] (*bag).entries@[index]).row)).rv()),
            } by {
                if probe == key && directory.lookup(probe) is Some {
                    assert forall|index: int| 0 <= index < after_bag.entries@.len() implies
                        predicate(probe, (*((#[trigger] after_bag.entries@[index]).row)).rv()) by {
                        if (*after_bag.entries@[index].row).rv() != (*row).rv() {
                            let source = choose|source: int| 0 <= source < before.entries@.len() &&
                                (*after_bag.entries@[index].row).rv() == (*(#[trigger] before.entries@[source]).row).rv();
                        }
                    }
                }
            }
        }
        assert forall|probe: i64, value: Row::V| #[trigger] index_amount(*directory, probe, value) ==
            index_amount(original, probe, value) + if probe == key && (*row).rv() == value { change as int } else { 0 } by {
            if probe == key {
                if after_bag.entries@.len() == 0 { assert(after_bag.amount(value) == 0); }
            }
        }
        assert forall|probe: i64, weight: spec_fn(Row::V) -> int| #[trigger] index_weight(*directory, probe, weight) ==
            index_weight(original, probe, weight) + if probe == key { change as int * weight((*row).rv()) } else { 0 } by {
            if probe == key {
                if after_bag.entries@.len() == 0 { assert(after_bag.weight(weight) == 0); }
            }
        }
    }
    true
}

pub proof fn lemma_index_indicator<Row: BagRow>(directory: Directory<Bag<Row>>, key: i64, value: Row::V)
    ensures index_amount(directory, key, value) == index_weight(directory, key, eqv(value)),
{
    if directory.lookup(key) is Some {
        let bag = directory.lookup(key)->0;
        lemma_sum_ext_all((*bag).entries@,
            |entry: Counted<Row>| if (*entry.row).rv() == value { entry.count as int } else { 0int },
            |entry: Counted<Row>| entry.count as int * eqv(value)((*entry.row).rv()));
    }
}

#[verifier::rlimit(20)]
pub fn finish_bags<Row: BagRow>(directory: &mut Directory<Bag<Row>>, changes: &Directory<Bag<Row>>) -> (result: bool)
    requires old(directory).valid(), changes.valid(),
    ensures result ==> {
        &&& final(directory).valid()
        &&& index_unique(*old(directory)) ==> index_unique(*final(directory))
        &&& index_positive(*old(directory)) ==> index_positive(*final(directory))
        &&& index_nonempty(*old(directory)) ==> index_nonempty(*final(directory))
        &&& forall|predicate: spec_fn(i64, Row::V) -> bool| #[trigger] index_routed(*old(directory), predicate) && index_routed(*changes, predicate) ==>
            index_routed(*final(directory), predicate)
        &&& forall|key: i64, weight: spec_fn(Row::V) -> int| #[trigger] index_weight(*final(directory), key, weight) ==
            index_weight(*old(directory), key, weight) + index_weight(*changes, key, weight)
        &&& forall|key: i64, value: Row::V| #[trigger] index_amount(*final(directory), key, value) ==
            index_amount(*old(directory), key, value) + index_amount(*changes, key, value)
        &&& forall|key: i64| (#[trigger] changes.lookup(key)) is None ==> final(directory).lookup(key) == old(directory).lookup(key)
    },
{
    let ghost original = *directory;
    let keys = changes.entries();
    let mut index = 0;
    while index < keys.len()
        invariant index <= keys.len(), directory.valid(), changes.valid(),
            entry_keys(keys@).no_duplicates(),
            forall|offset: int| 0 <= offset < keys@.len() ==> changes.lookup((#[trigger] keys@[offset]).0) == Some(keys@[offset].1),
            forall|key: i64| (#[trigger] changes.lookup(key) is Some) == entry_keys(keys@).contains(key),
            index_unique(original) ==> index_unique(*directory),
            index_positive(original) ==> index_positive(*directory),
            index_nonempty(original) ==> index_nonempty(*directory),
            forall|probe: i64| !entry_keys(keys@.take(index as int)).contains(probe) ==> #[trigger] directory.lookup(probe) == original.lookup(probe),
            forall|predicate: spec_fn(i64, Row::V) -> bool| #[trigger] index_routed(original, predicate) && index_routed(*changes, predicate) ==>
                index_routed(*directory, predicate),
            forall|key: i64, weight: spec_fn(Row::V) -> int| #[trigger] index_weight(*directory, key, weight) ==
                index_weight(original, key, weight) +
                if entry_keys(keys@.take(index as int)).contains(key) { index_weight(*changes, key, weight) } else { 0int },
        decreases keys.len() - index,
    {
        let key = keys[index].0;
        let rows = &keys[index].1.entries;
        let ghost previous_directory = *directory;
        proof { verus_prooflib::lemma_prefix_fresh(entry_keys(keys@), index as int); }
        proof {
            assert(changes.lookup(key) == Some(keys@[index as int].1));
            assert forall|predicate: spec_fn(i64, Row::V) -> bool| #[trigger] index_routed(*changes, predicate) implies
                forall|row_index: int| 0 <= row_index < rows@.len() ==> predicate(key, (*((#[trigger] rows@[row_index]).row)).rv()) by {
                let bag = (*changes).lookup(key)->0;
                assert((*bag).entries@ == rows@);
                assert forall|row_index: int| 0 <= row_index < rows@.len() implies predicate(key, (*((#[trigger] rows@[row_index]).row)).rv()) by {
                    assert(predicate(key, (*(*bag).entries@[row_index].row).rv()));
                }
            }
        }
        let mut offset = 0;
        while offset < rows.len()
            invariant offset <= rows.len(), directory.valid(),
                index_unique(previous_directory) ==> index_unique(*directory),
                index_nonempty(previous_directory) ==> index_nonempty(*directory),
                forall|predicate: spec_fn(i64, Row::V) -> bool| #[trigger] index_routed(previous_directory, predicate) && index_routed(*changes, predicate) ==>
                    index_routed(*directory, predicate),
                forall|predicate: spec_fn(i64, Row::V) -> bool| #[trigger] index_routed(*changes, predicate) ==>
                    forall|row_index: int| 0 <= row_index < rows@.len() ==> predicate(key, (*((#[trigger] rows@[row_index]).row)).rv()),
                forall|probe: i64| probe != key ==> #[trigger] directory.lookup(probe) == previous_directory.lookup(probe),
                forall|probe: i64, weight: spec_fn(Row::V) -> int| #[trigger] index_weight(*directory, probe, weight) ==
                    index_weight(previous_directory, probe, weight) +
                    if probe == key { weighted_entries(rows@.take(offset as int), weight) } else { 0int },
            decreases rows.len() - offset,
        {
            let ghost previous = *directory;
            proof {
                assert forall|predicate: spec_fn(i64, Row::V) -> bool| #[trigger] index_routed(previous_directory, predicate) && index_routed(*changes, predicate) implies
                    index_routed(previous, predicate) && predicate(key, (*rows@[offset as int].row).rv()) by {
                    assert(predicate(key, (*rows@[offset as int].row).rv()));
                }
            }
            if !adjust_bag(directory, key, Arc::clone(&rows[offset].row), rows[offset].count) { return false; }
            proof {
                assert forall|probe: i64, weight: spec_fn(Row::V) -> int| #[trigger] index_weight(*directory, probe, weight) ==
                    index_weight(previous_directory, probe, weight) +
                    if probe == key { weighted_entries(rows@.take(offset as int + 1), weight) } else { 0int } by {
                    lemma_sum_take(rows@, offset as int, |entry: Counted<Row>| entry.count as int * weight((*entry.row).rv()));
                    assert(index_weight(previous, probe, weight) == index_weight(previous_directory, probe, weight) +
                        if probe == key { weighted_entries(rows@.take(offset as int), weight) } else { 0int });
                }
            }
            offset += 1;
        }
        proof { assert(rows@.take(offset as int) =~= rows@); }
        if let Some(bag) = directory.get(key) {
            if !bag.positive() { return false; }
        }
        proof {
            assert(index_positive(original) ==> index_positive(*directory)) by {
                if index_positive(original) {
                    assert forall|probe: i64| match #[trigger] directory.lookup(probe) {
                        None => true, Some(bag) => (*bag).valid(),
                    } by {
                        if probe != key { assert(directory.lookup(probe) == previous_directory.lookup(probe)); }
                    }
                }
            }
            assert(entry_keys(keys@.take(index as int + 1)) =~= entry_keys(keys@.take(index as int)).push(key));
            assert forall|probe: i64| !entry_keys(keys@.take(index as int + 1)).contains(probe) implies directory.lookup(probe) == original.lookup(probe) by {
                broadcast use vstd::seq_lib::group_seq_properties;
                assert(probe != key);
                assert(!entry_keys(keys@.take(index as int)).contains(probe));
                assert(directory.lookup(probe) == previous_directory.lookup(probe));
            }
            assert(entry_keys(keys@.take(index as int)) =~= entry_keys(keys@).take(index as int));
            assert(!entry_keys(keys@.take(index as int)).contains(key));
            assert forall|probe: i64, weight: spec_fn(Row::V) -> int| #[trigger] index_weight(*directory, probe, weight) ==
                index_weight(original, probe, weight) +
                if entry_keys(keys@.take(index as int + 1)).contains(probe) { index_weight(*changes, probe, weight) } else { 0int } by {
                broadcast use vstd::seq_lib::group_seq_properties;
                assert(index_weight(*directory, probe, weight) == index_weight(previous_directory, probe, weight) +
                    if probe == key { weighted_entries(rows@, weight) } else { 0int });
                assert(index_weight(previous_directory, probe, weight) == index_weight(original, probe, weight) +
                    if entry_keys(keys@.take(index as int)).contains(probe) { index_weight(*changes, probe, weight) } else { 0int });
                if probe == key { assert(index_weight(*changes, key, weight) == weighted_entries(rows@, weight)); }
            }
        }
        index += 1;
    }
    proof {
        assert(keys@.take(index as int) =~= keys@);
        assert forall|probe: i64| (#[trigger] changes.lookup(probe)) is None implies directory.lookup(probe) == original.lookup(probe) by {
            assert(!entry_keys(keys@).contains(probe));
        }
        assert forall|probe: i64, weight: spec_fn(Row::V) -> int| #[trigger] index_weight(*directory, probe, weight) ==
            index_weight(original, probe, weight) + index_weight(*changes, probe, weight) by {
            if !entry_keys(keys@).contains(probe) { assert(changes.lookup(probe) is None); }
        }
        assert forall|probe: i64, value: Row::V| #[trigger] index_amount(*directory, probe, value) ==
            index_amount(original, probe, value) + index_amount(*changes, probe, value) by {
            lemma_index_indicator(*directory, probe, value);
            lemma_index_indicator(original, probe, value);
            lemma_index_indicator(*changes, probe, value);
            assert(index_weight(*directory, probe, eqv(value)) == index_weight(original, probe, eqv(value)) + index_weight(*changes, probe, eqv(value)));
        }
    }
    true
}

pub fn adjust_region(regions: &mut Directory<RegionBucket>, name: &[u8], key: i64, change: i128) -> (result: bool)
    requires regions_valid(*old(regions)),
    ensures result ==> {
        &&& regions_valid(*final(regions))
        &&& regions_positive(*old(regions)) && change > 0 ==> regions_positive(*final(regions))
        &&& regions_nonzero(*old(regions)) ==> regions_nonzero(*final(regions))
        &&& forall|probe: Seq<u8>, region: i64| #[trigger] region_amount(*final(regions), probe, region) ==
            region_amount(*old(regions), probe, region) + if probe == name@ && region == key { change as int } else { 0int }
    },
{
    let hash = hash_bytes(name);
    let ghost original = *regions;
    let old_bucket = regions.get(hash);
    let mut names: Vec<RegionName> = Vec::new();
    let mut found = false;
    if let Some(bucket) = old_bucket {
        let mut offset = 0;
        while offset < bucket.names.len()
            invariant
                offset <= bucket.names.len(), region_bucket_valid(*bucket, hash), names.len() == offset,
                original.lookup(hash) == Some(bucket),
                regions_positive(original) && change > 0 ==> region_bucket_positive(*bucket),
                regions_positive(original) && change > 0 ==> forall|index: int| 0 <= index < names@.len() ==>
                    counts_positive((#[trigger] names@[index]).keys),
                regions_nonzero(original) ==> region_bucket_nonzero(*bucket),
                regions_nonzero(original) ==> forall|index: int| 0 <= index < names@.len() ==>
                    counts_nonzero((#[trigger] names@[index]).keys),
                forall|index: int| 0 <= index < names@.len() ==>
                    (*(#[trigger] names@[index]).name)@ == (*bucket.names@[index].name)@ && names@[index].keys.valid(),
                found == bucket.names@.take(offset as int).map_values(|entry: RegionName| (*entry.name)@).contains(name@),
                forall|index: int, region: i64| 0 <= index < names@.len() ==>
                    #[trigger] count_at(names@[index].keys, region) == count_at(bucket.names@[index].keys, region) +
                        if (*bucket.names@[index].name)@ == name@ && region == key { change as int } else { 0int },
                forall|probe: Seq<u8>, region: i64| #[trigger] region_names_count(names@, probe, region) ==
                    region_names_count(bucket.names@.take(offset as int), probe, region) +
                    if found && probe == name@ && region == key { change as int } else { 0int },
            decreases bucket.names.len() - offset,
        {
            let entry = &bucket.names[offset];
            let ghost previous = names@;
            let ghost previously_found = found;
            let mut keys = entry.keys.share();
            if compare_bytes(&entry.name, name) == 0 {
                found = true;
                let before = match keys.get(key) { None => 0, Some(count) => count.value };
                let after = match before.checked_add(change) { None => return false, Some(value) => value };
                proof { assert(regions_positive(original) && change > 0 ==> after > 0); }
                if after == 0 { keys.replace(key, None); }
                else { keys.replace(key, Some(Arc::new(Count { value: after }))); }
            }
            proof { assert(regions_positive(original) && change > 0 ==> counts_positive(keys)); }
            proof { assert(regions_nonzero(original) ==> counts_nonzero(keys)); }
            names.push(RegionName { name: Arc::clone(&entry.name), keys });
            proof {
                broadcast use vstd::seq_lib::group_seq_properties;
                assert(bucket.names@.take(offset as int + 1).map_values(|entry: RegionName| (*entry.name)@) =~=
                    bucket.names@.take(offset as int).map_values(|entry: RegionName| (*entry.name)@).push((*entry.name)@));
                assert forall|probe: Seq<u8>, region: i64| #[trigger] region_names_count(names@, probe, region) ==
                    region_names_count(bucket.names@.take(offset as int + 1), probe, region) +
                    if found && probe == name@ && region == key { change as int } else { 0int } by {
                    let weight = |entry: RegionName| region_name_weight(entry, probe, region);
                    lemma_sum_push(previous, names@.last(), weight);
                    lemma_sum_take(bucket.names@, offset as int, weight);
                    assert(region_names_count(previous, probe, region) == region_names_count(bucket.names@.take(offset as int), probe, region) +
                        if previously_found && probe == name@ && region == key { change as int } else { 0int });
                    if (*entry.name)@ == name@ {
                        assert(!previously_found) by {
                            assert forall|index: int| 0 <= index < offset implies (*(#[trigger] bucket.names@[index]).name)@ != name@ by {}
                        }
                    }
                }
            }
            offset += 1;
        }
        proof { assert(bucket.names@.take(offset as int) =~= bucket.names@); }
    }
    let ghost previous = names@;
    proof {
        assert forall|probe: Seq<u8>, region: i64| text_hash(probe) as i64 == hash implies #[trigger] region_names_count(previous, probe, region) ==
            region_amount(original, probe, region) + if found && probe == name@ && region == key { change as int } else { 0int } by {}
        assert(found == match original.lookup(hash) {
            None => false,
            Some(bucket) => (*bucket).names@.map_values(|entry: RegionName| (*entry.name)@).contains(name@),
        });
    }
    if !found && change != 0 {
        let mut keys = Directory::new();
        keys.replace(key, Some(Arc::new(Count { value: change })));
        names.push(RegionName { name: Arc::new(copy_bytes(name)), keys });
    }
    proof {
        assert forall|probe: Seq<u8>, region: i64| text_hash(probe) as i64 == hash implies #[trigger] region_names_count(names@, probe, region) ==
            region_amount(original, probe, region) + if probe == name@ && region == key { change as int } else { 0int }
        by {
            let weight = |entry: RegionName| region_name_weight(entry, probe, region);
            assert(region_names_count(previous, probe, region) == region_amount(original, probe, region) +
                if found && probe == name@ && region == key { change as int } else { 0int });
            if !found && change != 0 {
                lemma_sum_push(previous, names@.last(), weight);
            }
        }
        assert(region_bucket_valid(RegionBucket { names }, hash)) by {
            assert forall|index: int| 0 <= index < names@.len() implies
                text_hash((*((#[trigger] names@[index]).name))@) == hash as int && names@[index].keys.valid() by {
                if index < previous.len() {
                    let bucket = original.lookup(hash)->0;
                    assert((*names@[index].name)@ == (*(*bucket).names@[index].name)@);
                }
            }
            assert forall|first: int, second: int| 0 <= first < second < names@.len() implies
                (*((#[trigger] names@[first]).name))@ != (*((#[trigger] names@[second]).name))@ by {
                let bucket = original.lookup(hash)->0;
                if second < previous.len() {
                    assert((*names@[first].name)@ == (*(*bucket).names@[first].name)@);
                    assert((*names@[second].name)@ == (*(*bucket).names@[second].name)@);
                } else {
                    broadcast use vstd::seq_lib::group_seq_properties;
                    assert(!found);
                    assert((*names@[first].name)@ == (*(*bucket).names@[first].name)@);
                    assert(!(*bucket).names@.map_values(|entry: RegionName| (*entry.name)@).contains(name@));
                    assert(previous.len() == (*bucket).names@.len());
                    assert((*bucket).names@.map_values(|entry: RegionName| (*entry.name)@)[first] == (*(*bucket).names@[first].name)@);
                    assert((*(*bucket).names@[first].name)@ != name@);
                }
            }
        }
    }
    let ghost after_names = names@;
    regions.replace(hash, Some(Arc::new(RegionBucket { names })));
    proof {
        assert(regions_positive(original) && change > 0 ==> regions_positive(*regions));
        assert forall|probe: Seq<u8>, region: i64| #[trigger] region_amount(*regions, probe, region) ==
            region_amount(original, probe, region) + if probe == name@ && region == key { change as int } else { 0int } by {
            if text_hash(probe) as i64 == hash {
                assert(region_names_count(after_names, probe, region) == region_amount(original, probe, region) +
                    if probe == name@ && region == key { change as int } else { 0int });
            }
        }
    }
    true
}

pub fn region_slice(regions: &Directory<RegionBucket>, name: &[u8]) -> (result: Option<Directory<Count>>)
    requires regions_valid(*regions),
    ensures
        result matches Some(slice) ==> slice.valid() && forall|key: i64|
            #[trigger] count_at(slice, key) == region_amount(*regions, name@, key),
        result is None ==> forall|key: i64| #[trigger] region_amount(*regions, name@, key) == 0,
{
    if let Some(bucket) = regions.get(hash_bytes(name)) {
        let mut offset = 0;
        while offset < bucket.names.len()
            invariant
                offset <= bucket.names.len(), region_bucket_valid(*bucket, text_hash(name@) as i64),
                regions.lookup(text_hash(name@) as i64) == Some(bucket),
                forall|index: int| 0 <= index < offset ==> (*(#[trigger] bucket.names@[index]).name)@ != name@,
            decreases bucket.names.len() - offset,
        {
            if compare_bytes(&bucket.names[offset].name, name) == 0 {
                proof {
                    assert forall|key: i64| #[trigger] count_at(bucket.names@[offset as int].keys, key) == region_amount(*regions, name@, key) by {
                        broadcast use vstd::seq_lib::group_seq_properties;
                        let weight = |entry: RegionName| region_name_weight(entry, name@, key);
                        assert forall|index: int| 0 <= index < bucket.names@.len() && index != offset implies weight(#[trigger] bucket.names@[index]) == 0 by {
                            if index < offset { assert((*bucket.names@[index].name)@ != name@); }
                            else { assert((*bucket.names@[index].name)@ != (*bucket.names@[offset as int].name)@); }
                        }
                        lemma_sum_single_match(bucket.names@, weight, offset as int);
                    }
                }
                return Some(bucket.names[offset].keys.share());
            }
            offset += 1;
        }
        proof {
            assert(bucket.names@.take(offset as int) =~= bucket.names@);
            assert forall|key: i64| #[trigger] region_amount(*regions, name@, key) == 0 by {
                lemma_sum_zero(bucket.names@, |entry: RegionName| region_name_weight(entry, name@, key));
            }
        }
    }
    None
}

#[verifier::rlimit(20)]
pub fn compact_region(regions: &mut Directory<RegionBucket>, hash: i64)
    requires regions_valid(*old(regions)),
    ensures
        regions_valid(*final(regions)),
        regions_nonzero(*old(regions)) ==> regions_nonzero(*final(regions)),
        forall|name: Seq<u8>, key: i64| #[trigger] region_amount(*final(regions), name, key) == region_amount(*old(regions), name, key),
{
    let ghost original = *regions;
    if let Some(bucket) = regions.get(hash) {
        let mut names: Vec<RegionName> = Vec::new();
        let mut index = 0;
        while index < bucket.names.len()
            invariant index <= bucket.names.len(), region_bucket_valid(*bucket, hash),
                regions_nonzero(original) ==> region_bucket_nonzero(*bucket),
                regions_nonzero(original) ==> forall|offset: int| 0 <= offset < names@.len() ==>
                    counts_nonzero((#[trigger] names@[offset]).keys),
                regions.lookup(hash) == Some(bucket), regions_valid(*regions),
                forall|offset: int| 0 <= offset < names@.len() ==> {
                    let entry = #[trigger] names@[offset];
                    &&& text_hash((*entry.name)@) == hash as int
                    &&& entry.keys.valid()
                    &&& exists|source: int| 0 <= source < index &&
                        (*(#[trigger] bucket.names@[source]).name)@ == (*entry.name)@ && bucket.names@[source].keys == entry.keys
                },
                forall|first: int, second: int| 0 <= first < second < names@.len() ==>
                    (*((#[trigger] names@[first]).name))@ != (*((#[trigger] names@[second]).name))@,
                forall|name: Seq<u8>, key: i64| #[trigger] region_names_count(names@, name, key) ==
                    region_names_count(bucket.names@.take(index as int), name, key),
            decreases bucket.names.len() - index,
        {
            let ghost previous = names@;
            if !bucket.names[index].keys.is_empty() {
                names.push(RegionName { name: Arc::clone(&bucket.names[index].name), keys: bucket.names[index].keys.share() });
            }
            proof {
                assert forall|offset: int| 0 <= offset < names@.len() implies {
                    let entry = #[trigger] names@[offset];
                    &&& text_hash((*entry.name)@) == hash as int
                    &&& entry.keys.valid()
                    &&& exists|source: int| 0 <= source < index + 1 &&
                        (*(#[trigger] bucket.names@[source]).name)@ == (*entry.name)@ && bucket.names@[source].keys == entry.keys
                } by {
                    if offset == previous.len() { assert((*names@[offset].name)@ == (*bucket.names@[index as int].name)@); }
                    else {
                        let source = choose|source: int| 0 <= source < index &&
                            (*(#[trigger] bucket.names@[source]).name)@ == (*previous[offset].name)@ && bucket.names@[source].keys == previous[offset].keys;
                    }
                }
                assert forall|first: int, second: int| 0 <= first < second < names@.len() implies
                    (*((#[trigger] names@[first]).name))@ != (*((#[trigger] names@[second]).name))@ by {
                    if second == previous.len() {
                        let source = choose|source: int| 0 <= source < index &&
                            (*(#[trigger] bucket.names@[source]).name)@ == (*previous[first].name)@ && bucket.names@[source].keys == previous[first].keys;
                        assert((*bucket.names@[source].name)@ != (*bucket.names@[index as int].name)@);
                    }
                }
                assert forall|name: Seq<u8>, key: i64| #[trigger] region_names_count(names@, name, key) ==
                    region_names_count(bucket.names@.take(index as int + 1), name, key) by {
                    let weight = |entry: RegionName| region_name_weight(entry, name, key);
                    assert(region_names_count(previous, name, key) == region_names_count(bucket.names@.take(index as int), name, key));
                    lemma_sum_take(bucket.names@, index as int, weight);
                    if names@.len() > previous.len() {
                        lemma_sum_push(previous, names@.last(), weight);
                        assert(names@.last().keys == bucket.names@[index as int].keys);
                        assert((*names@.last().name)@ == (*bucket.names@[index as int].name)@);
                    }
                    else { assert(bucket.names@[index as int].keys.lookup(key) is None); }
                }
            }
            index += 1;
        }
        let ghost final_names = names@;
        proof { assert(bucket.names@.take(index as int) =~= bucket.names@); }
        if names.len() == 0 { regions.replace(hash, None); }
        else { regions.replace(hash, Some(Arc::new(RegionBucket { names }))); }
        proof {
            assert forall|name: Seq<u8>, key: i64| #[trigger] region_amount(*regions, name, key) == region_amount(original, name, key) by {
                if text_hash(name) as i64 == hash {
                    assert(region_names_count(final_names, name, key) == region_names_count(bucket.names@, name, key));
                }
            }
        }
    }
}

pub fn part_keys(deltas: &[PartKeyDelta]) -> (result: Vec<i64>)
    ensures
        result@.no_duplicates(),
        forall|key: i64| #[trigger] result@.contains(key) ==
            deltas@.map_values(|delta: PartKeyDelta| delta.p_partkey).contains(key),
{
    let mut seen: Directory<Count> = Directory::new();
    let mut result = Vec::new();
    let mut index = 0;
    let mut processed: Ghost<int> = Ghost(0);
    while index < deltas.len()
        invariant
            index <= deltas.len(), processed@ == index as int, seen.valid(), result@.no_duplicates(),
            forall|key: i64| #[trigger] result@.contains(key) ==
                deltas@.take(processed@).map_values(|delta: PartKeyDelta| delta.p_partkey).contains(key),
            forall|key: i64| (#[trigger] seen.lookup(key) is Some) == result@.contains(key),
        decreases deltas.len() - index,
    {
        let key = deltas[index].p_partkey;
        let ghost previous = result@;
        let ghost previous_seen = seen;
        if seen.get(key).is_none() {
            proof { verus_prooflib::lemma_unique_push(result@, key); }
            seen.replace(key, Some(Arc::new(Count { value: 1 })));
            result.push(key);
        }
        proof {
            assert(deltas@.take(index as int + 1).map_values(|delta: PartKeyDelta| delta.p_partkey) =~=
                deltas@.take(index as int).map_values(|delta: PartKeyDelta| delta.p_partkey).push(key));
            assert forall|probe: i64| (#[trigger] seen.lookup(probe) is Some) == result@.contains(probe) by {
                broadcast use vstd::seq_lib::group_seq_properties;
                assert((previous_seen.lookup(probe) is Some) == previous.contains(probe));
                if result@.len() > previous.len() { assert(result@ =~= previous.push(key)); }
                else { assert(result@ == previous); }
            }
            assert forall|probe: i64| #[trigger] result@.contains(probe) ==
                deltas@.take(index as int + 1).map_values(|delta: PartKeyDelta| delta.p_partkey).contains(probe) by {
                broadcast use vstd::seq_lib::group_seq_properties;
                assert(previous.contains(probe) == deltas@.take(index as int).map_values(|delta: PartKeyDelta| delta.p_partkey).contains(probe));
                if result@.len() > previous.len() { assert(result@ =~= previous.push(key)); }
                else { assert(result@ == previous); }
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    proof { assert(deltas@.take(processed@) =~= deltas@); }
    result
}

pub fn contains_key(keys: &[i64], key: i64) -> (result: bool)
    ensures result == keys@.contains(key),
{
    let mut index = 0;
    while index < keys.len()
        invariant index <= keys.len(),
            forall|offset: int| 0 <= offset < index ==> #[trigger] keys@[offset] != key,
        decreases keys.len() - index,
    {
        if keys[index] == key { return true; }
        index += 1;
    }
    false
}

}
