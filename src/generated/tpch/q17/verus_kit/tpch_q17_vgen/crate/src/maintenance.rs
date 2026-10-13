use vstd::prelude::*;
use std::collections::HashMap;
use std::sync::Arc;
use crate::aggregation::*;
use crate::gen_obligations::*;
use crate::gen_spec::*;
use crate::model::*;
use crate::storage::*;
use verus_common::sums::*;
use crate::changes::*;
use crate::variants::lemma_variant_prefix;
use verus_prooflib::lemma_count_weight_add;

verus! {

broadcast use vstd::seq_lib::group_seq_properties;

pub struct Change { pub identity: Arc<Identity>, pub delta: i128 }

pub struct Changes {
    pub rows: HashMap<i64, Vec<Change>>,
    pub keys: Vec<i64>,
    pub identities: HashMap<i64, Vec<Arc<Identity>>>,
}

#[verifier::opaque]
pub open spec fn change_at(changes: Changes, key: i64, brand: Seq<u8>, container: Seq<u8>) -> int {
    if changes.rows@.contains_key(key) {
        change_total(changes.rows@[key]@, brand, container)
    } else { 0 }
}

#[verifier::opaque]
pub open spec fn changes_wf(changes: Changes) -> bool {
    &&& changes.keys@.no_duplicates()
    &&& changes.rows@.dom() == changes.keys@.to_set()
    &&& forall|key: i64| #![trigger changes.rows@[key]] changes.rows@.contains_key(key) ==> change_bucket_wf(changes.rows@[key]@)
    &&& identity_cache_wf(changes.identities@)
}

#[verifier::opaque]
pub open spec fn identity_cache_wf(cache: Map<i64, Vec<Arc<Identity>>>) -> bool {
    forall|hash: i64| #![trigger cache[hash]] cache.contains_key(hash) ==> {
        let bucket = cache[hash]@;
        forall|index: int| #![trigger bucket[index]] 0 <= index < bucket.len() ==>
            bucket[index].fingerprint == hash && hash as int == pair_hash(bucket[index].brand@, bucket[index].container@)
    }
}

pub proof fn lemma_changes_fields(changes: Changes)
    requires changes_wf(changes),
    ensures changes.keys@.no_duplicates(), changes.rows@.dom() == changes.keys@.to_set(), identity_cache_wf(changes.identities@),
        forall|key: i64| #![trigger changes.rows@[key]] changes.rows@.contains_key(key) ==> change_bucket_wf(changes.rows@[key]@),
{ reveal(changes_wf); }

pub proof fn lemma_changes_support(before: Changes, after: Changes, key: i64)
    requires changes_wf(before), changes_wf(after), after.rows@.dom() == before.rows@.dom().insert(key),
    ensures forall|other: i64| #[trigger] after.keys@.contains(other) == (before.keys@.contains(other) || other == key),
{
    lemma_changes_fields(before); lemma_changes_fields(after);
    before.keys@.to_set_ensures(); after.keys@.to_set_ensures();
    assert forall|other: i64| #[trigger] after.keys@.contains(other) == (before.keys@.contains(other) || other == key) by {
        assert(after.keys@.to_set().contains(other) == after.keys@.contains(other));
        assert(before.keys@.to_set().contains(other) == before.keys@.contains(other));
        assert(after.rows@.dom().contains(other) == (before.rows@.dom().contains(other) || other == key));
    }
}

pub proof fn lemma_part_prefix(rows: Seq<PartInput>, index: int, key: i64, brand: Seq<u8>, container: Seq<u8>)
    requires 0 <= index < rows.len(),
    ensures part_count(part_input_seq_view(rows.take(index + 1)), key as int, brand, container)
        == part_count(part_input_seq_view(rows.take(index)), key as int, brand, container)
            + ind(rows[index].p_partkey == key && rows[index].p_brand@ == brand && rows[index].p_container@ == container),
{
    assert(part_input_seq_view(rows.take(index + 1)) =~= part_input_seq_view(rows.take(index)).push(part_input_view(rows[index])));
    reveal(part_count);
    lemma_sum_push(part_input_seq_view(rows.take(index)), part_input_view(rows[index]), |row: PartRow|
        ind(row.p_partkey == key as int && row.p_brand == brand && row.p_container == container));
}

#[verifier::opaque]
pub open spec fn delta_part_count(rows: Seq<PartDelta>, key: i64, brand: Seq<u8>, container: Seq<u8>) -> int {
    sum(part_delta_seq_view(rows), |entry: (PartRow, int)| entry.1 * ind(entry.0.p_partkey == key as int && entry.0.p_brand == brand && entry.0.p_container == container))
}

pub proof fn lemma_delta_part_prefix(rows: Seq<PartDelta>, index: int, key: i64, brand: Seq<u8>, container: Seq<u8>)
    requires 0 <= index < rows.len(),
    ensures delta_part_count(rows.take(index + 1), key, brand, container) == delta_part_count(rows.take(index), key, brand, container)
        + rows[index].sign as int * ind(rows[index].row.p_partkey == key && rows[index].row.p_brand@ == brand && rows[index].row.p_container@ == container),
{
    reveal(delta_part_count);
    assert(part_delta_seq_view(rows.take(index + 1)) =~= part_delta_seq_view(rows.take(index)).push((part_input_view(rows[index].row), rows[index].sign as int)));
    lemma_sum_push(part_delta_seq_view(rows.take(index)), (part_input_view(rows[index].row), rows[index].sign as int),
        |entry: (PartRow, int)| entry.1 * ind(entry.0.p_partkey == key as int && entry.0.p_brand == brand && entry.0.p_container == container));
}

pub fn new_changes() -> (result: Changes)
    ensures changes_wf(result), result.keys@.len() == 0,
{
    let result = Changes { rows: HashMap::new(), keys: Vec::new(), identities: HashMap::new() };
    proof {
        result.keys@.to_set_ensures(); reveal(identity_cache_wf); reveal(changes_wf);
        assert(result.keys@.to_set() =~= Set::empty());
        assert(result.rows@.dom() =~= Set::empty());
    }
    result
}

#[verifier::rlimit(4)]
pub fn resolve_identity(groups: &Store<Vec<Group>>, pending: &mut HashMap<i64, Vec<Arc<Identity>>>, brand: &[u8], container: &[u8]) -> (result: Arc<Identity>)
    requires group_store_wf(*groups), identity_cache_wf(old(pending)@),
    ensures same(*result, brand@, container@), result.fingerprint as int == pair_hash(brand@, container@),
        identity_cache_wf(final(pending)@),
{
    let hash = fingerprint(brand, container);
    if let Some(bucket) = groups.get(hash) {
        proof { assert(groups_wf(groups_at(*groups, hash), hash)); }
        let mut index = 0;
        while index < bucket.len()
            invariant index <= bucket.len(), groups_wf(bucket@, hash), identity_cache_wf(pending@),
                hash as int == pair_hash(brand@, container@),
            decreases bucket.len() - index,
        {
            if identity_matches(&bucket[index].identity, brand, container) {
                proof { lemma_group_entry(bucket@, hash, index as int); }
                return bucket[index].identity.clone();
            }
            index += 1;
        }
    }
    if let Some(bucket) = pending.get(&hash) {
        proof { reveal(identity_cache_wf); }
        let mut index = 0;
        while index < bucket.len()
            invariant index <= bucket.len(), identity_cache_wf(pending@),
                hash as int == pair_hash(brand@, container@),
                forall|position: int| #![trigger bucket@[position]] 0 <= position < bucket.len() ==>
                    bucket@[position].fingerprint == hash && hash as int == pair_hash(bucket@[position].brand@, bucket@[position].container@),
            decreases bucket.len() - index,
        {
            if identity_matches(&bucket[index], brand, container) { return bucket[index].clone(); }
            index += 1;
        }
    }
    let identity = Arc::new(Identity { brand: copy_bytes(brand), container: copy_bytes(container), fingerprint: hash });
    let before = Ghost(pending@);
    let mut bucket: Vec<Arc<Identity>> = Vec::new();
    if let Some(previous) = pending.get(&hash) {
        let mut index = 0;
        while index < previous.len()
            invariant index <= previous.len(), bucket@ == previous@.take(index as int),
                identity_cache_wf(pending@), pending@ == before@,
                before@.contains_key(hash), previous@ == before@[hash]@,
            decreases previous.len() - index,
        {
            bucket.push(previous[index].clone());
            index += 1;
        }
    }
    proof {
        reveal(identity_cache_wf);
        assert(bucket@ == if before@.contains_key(hash) { before@[hash]@ } else { Seq::empty() });
        assert forall|position: int| #![trigger bucket@[position]] 0 <= position < bucket.len() ==>
            bucket@[position].fingerprint == hash && hash as int == pair_hash(bucket@[position].brand@, bucket@[position].container@) by {
            if before@.contains_key(hash) { assert(bucket@[position] == before@[hash]@[position]); }
            else { assert(bucket@.len() == 0); }
        }
    }
    bucket.push(identity.clone());
    pending.insert(hash, bucket);
    proof {
        reveal(identity_cache_wf);
        assert forall|other: i64| #![trigger pending@[other]] pending@.contains_key(other) implies {
            let entries = pending@[other]@;
            forall|position: int| #![trigger entries[position]] 0 <= position < entries.len() ==>
                entries[position].fingerprint == other && other as int == pair_hash(entries[position].brand@, entries[position].container@)
        } by {
            if other != hash { assert(pending@[other] == before@[other]); }
        }
    }
    identity
}

#[verifier::rlimit(1)]
pub fn add_change(changes: &mut Changes, groups: &Store<Vec<Group>>, row: &PartInput, sign: i64) -> (result: bool)
    requires group_store_wf(*groups), changes_wf(*old(changes)),
    ensures result ==> final(changes).rows@.dom() == old(changes).rows@.dom().insert(row.p_partkey),
        result ==> changes_wf(*final(changes))
        && forall|key: i64, brand: Seq<u8>, container: Seq<u8>| #[trigger] change_at(*final(changes), key, brand, container)
            == change_at(*old(changes), key, brand, container) + if key == row.p_partkey && brand == row.p_brand@ && container == row.p_container@ {
                sign as int
            } else { 0 },
{
    let before = Ghost(*changes);
    proof { reveal(changes_wf); }
    let identity = resolve_identity(groups, &mut changes.identities, &row.p_brand, &row.p_container);
    let mut bucket = match changes.rows.remove(&row.p_partkey) {
        Some(value) => value,
        None => {
            proof { changes.keys@.to_set_ensures(); verus_prooflib::lemma_unique_push(changes.keys@, row.p_partkey); }
            changes.keys.push(row.p_partkey);
            proof { reveal(change_bucket_wf); }
            Vec::new()
        },
    };
    let before_bucket = Ghost(bucket@);
    proof {
        reveal(change_at); reveal(change_total);
        assert forall|brand: Seq<u8>, container: Seq<u8>| #[trigger] change_total(before_bucket@, brand, container)
            == change_at(before@, row.p_partkey, brand, container) by {}
    }
    if !merge_change(&mut bucket, identity, sign) { return false; }
    let after_bucket = Ghost(bucket@);
    changes.rows.insert(row.p_partkey, bucket);
    proof {
        changes.keys@.to_set_ensures();
        assert(changes.rows@.dom() =~= changes.keys@.to_set());
        reveal(changes_wf);
        assert forall|key: i64| #![trigger changes.rows@[key]] changes.rows@.contains_key(key) implies change_bucket_wf(changes.rows@[key]@) by {
            if key == row.p_partkey { assert(changes.rows@[key]@ == after_bucket@); }
            else { assert(changes.rows@[key] == before@.rows@[key]); }
        }
        assert forall|key: i64, brand: Seq<u8>, container: Seq<u8>| #[trigger] change_at(*changes, key, brand, container)
            == change_at(before@, key, brand, container) + if key == row.p_partkey && brand == row.p_brand@ && container == row.p_container@ { sign as int } else { 0 } by {
            reveal(change_at);
            if key == row.p_partkey {
                assert(change_total(before_bucket@, brand, container) == change_at(before@, key, brand, container));
                assert(change_total(after_bucket@, brand, container) == change_total(before_bucket@, brand, container)
                    + if same(*identity, brand, container) { sign as int } else { 0 });
            }
        }
    }
    true
}

#[verifier::rlimit(4)]
pub fn build_changes(parts: &Vec<PartInput>, groups: &Store<Vec<Group>>) -> (result: Option<Changes>)
    requires group_store_wf(*groups),
    ensures result matches Some(changes) ==> changes_wf(changes),
        result matches Some(changes) ==> forall|position: int| #![trigger parts@[position]] 0 <= position < parts.len() ==> changes.keys@.contains(parts@[position].p_partkey),
        result matches Some(changes) ==> forall|key: i64, brand: Seq<u8>, container: Seq<u8>|
        #[trigger] change_at(changes, key, brand, container) == part_count(part_input_seq_view(parts@), key as int, brand, container),
{
    let mut changes = new_changes();
    proof {
        lemma_changes_fields(changes);
        reveal(change_at); reveal(change_total); reveal(part_count);
        assert(part_input_seq_view(parts@.take(0)) =~= Seq::<PartRow>::empty());
    }
    let mut index = 0;
    let mut processed = Ghost(0int);
    while index < parts.len()
        invariant index <= parts.len(), processed@ == index as int, changes_wf(changes), group_store_wf(*groups),
            forall|position: int| #![trigger parts@[position]] 0 <= position < index ==> changes.keys@.contains(parts@[position].p_partkey),
            forall|key: i64, brand: Seq<u8>, container: Seq<u8>| #[trigger] change_at(changes, key, brand, container)
                == part_count(part_input_seq_view(parts@.take(processed@)), key as int, brand, container),
        decreases parts.len() - index,
    {
        let before = Ghost(changes);
        if !add_change(&mut changes, groups, &parts[index], 1) { return None; }
        proof {
            assert forall|key: i64, brand: Seq<u8>, container: Seq<u8>| #[trigger] change_at(changes, key, brand, container)
                == part_count(part_input_seq_view(parts@.take(index as int + 1)), key as int, brand, container) by {
                assert(change_at(before@, key, brand, container) == part_count(part_input_seq_view(parts@.take(index as int)), key as int, brand, container));
                lemma_part_prefix(parts@, index as int, key, brand, container);
            }
            lemma_changes_fields(changes); lemma_changes_fields(before@);
            changes.keys@.to_set_ensures(); before@.keys@.to_set_ensures();
            lemma_changes_support(before@, changes, parts@[index as int].p_partkey);
        }
        index += 1;
        processed = Ghost(index as int);
    }
    proof { assert(parts@.take(parts@.len() as int) =~= parts@); }
    Some(changes)
}

#[verifier::rlimit(1)]
pub fn delta_changes(parts: &Vec<PartDelta>, groups: &Store<Vec<Group>>) -> (result: Option<Changes>)
    requires group_store_wf(*groups),
    ensures result matches Some(changes) ==> changes_wf(changes),
        result matches Some(changes) ==> forall|position: int| #![trigger parts@[position]] 0 <= position < parts.len() ==> changes.keys@.contains(parts@[position].row.p_partkey),
        result matches Some(changes) ==> forall|key: i64, brand: Seq<u8>, container: Seq<u8>|
        #[trigger] change_at(changes, key, brand, container) == sum(part_delta_seq_view(parts@), |entry: (PartRow, int)|
            entry.1 * ind(entry.0.p_partkey == key as int && entry.0.p_brand == brand && entry.0.p_container == container)),
{
    let mut changes = new_changes();
    proof {
        lemma_changes_fields(changes);
        reveal(change_at); reveal(change_total); reveal(delta_part_count);
        assert(part_delta_seq_view(parts@.take(0)) =~= Seq::<(PartRow, int)>::empty());
    }
    let mut index = 0;
    let mut processed = Ghost(0int);
    while index < parts.len()
        invariant index <= parts.len(), processed@ == index as int, changes_wf(changes), group_store_wf(*groups),
            forall|position: int| #![trigger parts@[position]] 0 <= position < index ==> changes.keys@.contains(parts@[position].row.p_partkey),
            forall|key: i64, brand: Seq<u8>, container: Seq<u8>| #[trigger] change_at(changes, key, brand, container)
                == delta_part_count(parts@.take(processed@), key, brand, container),
        decreases parts.len() - index,
    {
        let before = Ghost(changes);
        if !add_change(&mut changes, groups, &parts[index].row, parts[index].sign) { return None; }
        proof {
            assert forall|key: i64, brand: Seq<u8>, container: Seq<u8>| #[trigger] change_at(changes, key, brand, container)
                == delta_part_count(parts@.take(index as int + 1), key, brand, container) by {
                assert(change_at(before@, key, brand, container) == delta_part_count(parts@.take(index as int), key, brand, container));
                lemma_delta_part_prefix(parts@, index as int, key, brand, container);
            }
            lemma_changes_fields(changes); lemma_changes_fields(before@);
            changes.keys@.to_set_ensures(); before@.keys@.to_set_ensures();
            lemma_changes_support(before@, changes, parts@[index as int].row.p_partkey);
        }
        index += 1;
        processed = Ghost(index as int);
    }
    proof { assert(parts@.take(parts@.len() as int) =~= parts@); reveal(delta_part_count); }
    Some(changes)
}

pub fn read_moment(lines: &Store<Moment>, key: i64) -> (result: Moment)
    requires lines.wf(),
    ensures result == moment_at(*lines, key),
{ match lines.get(key) { Some(value) => *value, None => zero() } }

pub fn adjust_group(groups: &mut Store<Vec<Group>>, identity: &Arc<Identity>, references_delta: i128, count_delta: i128, price_delta: i128) -> (result: bool)
    requires group_store_wf(*old(groups)), identity.fingerprint as int == pair_hash(identity.brand@, identity.container@),
    ensures group_store_wf(*final(groups)),
        result ==> forall|brand: Seq<u8>, container: Seq<u8>| #[trigger] group_count_at(*final(groups), brand, container)
            == group_count_at(*old(groups), brand, container) + if same(**identity, brand, container) { count_delta as int } else { 0 },
        result ==> forall|brand: Seq<u8>, container: Seq<u8>| #[trigger] group_price_at(*final(groups), brand, container)
            == group_price_at(*old(groups), brand, container) + if same(**identity, brand, container) { price_delta as int } else { 0 },
        result ==> forall|brand: Seq<u8>, container: Seq<u8>| #[trigger] group_refs_at(*final(groups), brand, container)
            == group_refs_at(*old(groups), brand, container) + if same(**identity, brand, container) { references_delta as int } else { 0 },
{
    let result = crate::group_updates::adjust_group(groups, identity, references_delta, count_delta, price_delta);
    proof { reveal(group_count_at); reveal(group_price_at); reveal(group_refs_at); }
    result
}

#[verifier::rlimit(1)]
pub fn replace_lines(lines: &mut Store<Moment>, parts: &Store<Vec<Variant>>, groups: &mut Store<Vec<Group>>, key: i64, new: Moment) -> (result: bool)
    requires old(lines).wf(), part_store_wf(*parts), group_store_wf(*old(groups)), new.count >= 0,
        new.count == 0 ==> new.price == 0,
    ensures final(lines).wf(), group_store_wf(*final(groups)),
        result ==> forall|other: i64| #[trigger] final(lines).lookup(other) == if other == key {
            if new.count == 0 { None } else { Some(new) }
        } else { old(lines).lookup(other) },
        result ==> forall|other: i64| #[trigger] moment_at(*final(lines), other)
            == if other == key { new } else { moment_at(*old(lines), other) },
        result ==> forall|brand: Seq<u8>, container: Seq<u8>| #[trigger] group_count_at(*final(groups), brand, container)
            == group_count_at(*old(groups), brand, container) + multiplicity(*parts, key, brand, container)
                * (new.count as int - moment_at(*old(lines), key).count as int),
        result ==> forall|brand: Seq<u8>, container: Seq<u8>| #[trigger] group_price_at(*final(groups), brand, container)
            == group_price_at(*old(groups), brand, container) + multiplicity(*parts, key, brand, container)
                * (new.price as int - moment_at(*old(lines), key).price as int),
        result ==> forall|brand: Seq<u8>, container: Seq<u8>| #[trigger] group_refs_at(*final(groups), brand, container)
            == group_refs_at(*old(groups), brand, container),
{
    let before_lines = Ghost(*lines);
    let before_groups = Ghost(*groups);
    let previous = read_moment(lines, key);
    let count_delta = match new.count.checked_sub(previous.count) { Some(value) => value, None => return false };
    let price_delta = match new.price.checked_sub(previous.price) { Some(value) => value, None => return false };
    if count_delta != 0 || price_delta != 0 {
        if let Some(variants) = parts.get(key) {
            proof {
                assert(variants_wf(variants_at(*parts, key)));
                assert(variants_wf(variants@));
                assert(variants@.take(0) =~= Seq::<Variant>::empty());
                reveal(variant_total);
            }
            let mut index = 0;
            let mut processed = Ghost(0int);
            while index < variants.len()
                invariant index <= variants.len(), processed@ == index as int,
                    group_store_wf(*groups), lines.wf(), *lines == before_lines@,
                    previous == moment_at(before_lines@, key),
                    count_delta as int == new.count as int - previous.count as int,
                    price_delta as int == new.price as int - previous.price as int,
                    variants_wf(variants@), variants@ == variants_at(*parts, key),
                    forall|brand: Seq<u8>, container: Seq<u8>| #[trigger] group_count_at(*groups, brand, container)
                        == group_count_at(before_groups@, brand, container) + variant_total(variants@.take(processed@), brand, container) * count_delta as int,
                    forall|brand: Seq<u8>, container: Seq<u8>| #[trigger] group_price_at(*groups, brand, container)
                        == group_price_at(before_groups@, brand, container) + variant_total(variants@.take(processed@), brand, container) * price_delta as int,
                    forall|brand: Seq<u8>, container: Seq<u8>| #[trigger] group_refs_at(*groups, brand, container) == group_refs_at(before_groups@, brand, container),
                decreases variants.len() - index,
            {
                let variant = &variants[index];
                let old_groups = Ghost(*groups);
                proof { lemma_variant_entry(variants@, index as int); }
                let count = match variant.multiplicity.checked_mul(count_delta) { Some(value) => value, None => return false };
                let price = match variant.multiplicity.checked_mul(price_delta) { Some(value) => value, None => return false };
                if !adjust_group(groups, &variant.identity, 0, count, price) { return false; }
                proof {
                    assert forall|brand: Seq<u8>, container: Seq<u8>| #[trigger] group_count_at(*groups, brand, container)
                        == group_count_at(before_groups@, brand, container) + variant_total(variants@.take(index as int + 1), brand, container) * count_delta as int by {
                        assert(group_count_at(old_groups@, brand, container) == group_count_at(before_groups@, brand, container)
                            + variant_total(variants@.take(index as int), brand, container) * count_delta as int);
                        lemma_variant_prefix(variants@, index as int, brand, container);
                        lemma_count_weight_add(variant_total(variants@.take(index as int), brand, container),
                            if same(*variant.identity, brand, container) { variant.multiplicity as int } else { 0int }, count_delta as int);
                    }
                    assert forall|brand: Seq<u8>, container: Seq<u8>| #[trigger] group_price_at(*groups, brand, container)
                        == group_price_at(before_groups@, brand, container) + variant_total(variants@.take(index as int + 1), brand, container) * price_delta as int by {
                        assert(group_price_at(old_groups@, brand, container) == group_price_at(before_groups@, brand, container)
                            + variant_total(variants@.take(index as int), brand, container) * price_delta as int);
                        lemma_variant_prefix(variants@, index as int, brand, container);
                        lemma_count_weight_add(variant_total(variants@.take(index as int), brand, container),
                            if same(*variant.identity, brand, container) { variant.multiplicity as int } else { 0int }, price_delta as int);
                    }
                }
                index += 1;
                processed = Ghost(index as int);
            }
            proof { assert(variants@.take(variants@.len() as int) =~= variants@); reveal(multiplicity); }
        } else {
            proof { reveal(multiplicity); reveal(variant_total); }
        }
    }
    if new.count == 0 { lines.put(key, None); } else { lines.put(key, Some(Arc::new(new))); }
    true
}

#[verifier::rlimit(1)]
pub fn apply_changes(parts: &mut Store<Vec<Variant>>, lines: &Store<Moment>, groups: &mut Store<Vec<Group>>, changes: &Changes) -> (result: bool)
    requires part_store_wf(*old(parts)), lines.wf(), group_store_wf(*old(groups)), changes_wf(*changes),
    ensures part_store_wf(*final(parts)), group_store_wf(*final(groups)),
        result ==> forall|key: i64, brand: Seq<u8>, container: Seq<u8>| #[trigger] multiplicity(*final(parts), key, brand, container)
            == multiplicity(*old(parts), key, brand, container) + change_at(*changes, key, brand, container),
        result ==> forall|brand: Seq<u8>, container: Seq<u8>| #[trigger] group_count_at(*final(groups), brand, container)
            == group_count_at(*old(groups), brand, container) + sum(changes.keys@, |key: i64|
                change_at(*changes, key, brand, container) * moment_at(*lines, key).count as int),
        result ==> forall|brand: Seq<u8>, container: Seq<u8>| #[trigger] group_price_at(*final(groups), brand, container)
            == group_price_at(*old(groups), brand, container) + sum(changes.keys@, |key: i64|
                change_at(*changes, key, brand, container) * moment_at(*lines, key).price as int),
        result ==> forall|brand: Seq<u8>, container: Seq<u8>| #[trigger] group_refs_at(*final(groups), brand, container)
            == group_refs_at(*old(groups), brand, container) + sum(changes.keys@, |key: i64| change_at(*changes, key, brand, container)),
{
    proof { lemma_changes_fields(*changes); }
    let mut key_index = 0;
    let mut processed = Ghost(0int);
    let before_parts = Ghost(*parts);
    let before_groups = Ghost(*groups);
    while key_index < changes.keys.len()
        invariant key_index <= changes.keys.len(), processed@ == key_index as int,
            part_store_wf(*parts), group_store_wf(*groups), lines.wf(), changes_wf(*changes),
            changes.keys@.no_duplicates(), changes.rows@.dom() == changes.keys@.to_set(),
            forall|key: i64, brand: Seq<u8>, container: Seq<u8>| #[trigger] multiplicity(*parts, key, brand, container)
                == multiplicity(before_parts@, key, brand, container)
                    + if changes.keys@.take(processed@).contains(key) { change_at(*changes, key, brand, container) } else { 0 },
            forall|brand: Seq<u8>, container: Seq<u8>| #[trigger] group_count_at(*groups, brand, container)
                == group_count_at(before_groups@, brand, container) + sum(changes.keys@.take(processed@), |key: i64|
                    change_at(*changes, key, brand, container) * moment_at(*lines, key).count as int),
            forall|brand: Seq<u8>, container: Seq<u8>| #[trigger] group_price_at(*groups, brand, container)
                == group_price_at(before_groups@, brand, container) + sum(changes.keys@.take(processed@), |key: i64|
                    change_at(*changes, key, brand, container) * moment_at(*lines, key).price as int),
            forall|brand: Seq<u8>, container: Seq<u8>| #[trigger] group_refs_at(*groups, brand, container)
                == group_refs_at(before_groups@, brand, container) + sum(changes.keys@.take(processed@), |key: i64|
                    change_at(*changes, key, brand, container)),
        decreases changes.keys.len() - key_index,
    {
        let key = changes.keys[key_index];
        let old_parts = Ghost(*parts);
        let old_groups = Ghost(*groups);
        proof {
            lemma_changes_fields(*changes);
            changes.keys@.to_set_ensures();
            verus_prooflib::lemma_prefix_fresh(changes.keys@, key_index as int);
        }
        let patch = match changes.rows.get(&key) { Some(value) => value, None => return false };
        if !crate::patches::apply_key_patch(parts, lines, groups, key, patch) { return false; }
        proof {
            reveal(change_at);
            assert forall|other: i64, brand: Seq<u8>, container: Seq<u8>| #[trigger] multiplicity(*parts, other, brand, container)
                == multiplicity(before_parts@, other, brand, container) + if changes.keys@.take(key_index as int + 1).contains(other) {
                    change_at(*changes, other, brand, container)
                } else { 0 } by {
                assert(multiplicity(old_parts@, other, brand, container) == multiplicity(before_parts@, other, brand, container)
                    + if changes.keys@.take(key_index as int).contains(other) { change_at(*changes, other, brand, container) } else { 0 });
                assert(changes.keys@.take(key_index as int + 1) =~= changes.keys@.take(key_index as int).push(key));
            }
            assert forall|brand: Seq<u8>, container: Seq<u8>| #[trigger] group_count_at(*groups, brand, container)
                == group_count_at(before_groups@, brand, container) + sum(changes.keys@.take(key_index as int + 1), |key: i64|
                    change_at(*changes, key, brand, container) * moment_at(*lines, key).count as int) by {
                assert(group_count_at(old_groups@, brand, container) == group_count_at(before_groups@, brand, container)
                    + sum(changes.keys@.take(key_index as int), |key: i64| change_at(*changes, key, brand, container) * moment_at(*lines, key).count as int));
                lemma_sum_take(changes.keys@, key_index as int, |key: i64| change_at(*changes, key, brand, container) * moment_at(*lines, key).count as int);
            }
            assert forall|brand: Seq<u8>, container: Seq<u8>| #[trigger] group_price_at(*groups, brand, container)
                == group_price_at(before_groups@, brand, container) + sum(changes.keys@.take(key_index as int + 1), |key: i64|
                    change_at(*changes, key, brand, container) * moment_at(*lines, key).price as int) by {
                assert(group_price_at(old_groups@, brand, container) == group_price_at(before_groups@, brand, container)
                    + sum(changes.keys@.take(key_index as int), |key: i64| change_at(*changes, key, brand, container) * moment_at(*lines, key).price as int));
                lemma_sum_take(changes.keys@, key_index as int, |key: i64| change_at(*changes, key, brand, container) * moment_at(*lines, key).price as int);
            }
            assert forall|brand: Seq<u8>, container: Seq<u8>| #[trigger] group_refs_at(*groups, brand, container)
                == group_refs_at(before_groups@, brand, container) + sum(changes.keys@.take(key_index as int + 1), |key: i64| change_at(*changes, key, brand, container)) by {
                assert(group_refs_at(old_groups@, brand, container) == group_refs_at(before_groups@, brand, container)
                    + sum(changes.keys@.take(key_index as int), |key: i64| change_at(*changes, key, brand, container)));
                lemma_sum_take(changes.keys@, key_index as int, |key: i64| change_at(*changes, key, brand, container));
            }
        }
        key_index += 1;
        processed = Ghost(key_index as int);
    }
    proof { assert(changes.keys@.take(changes.keys@.len() as int) =~= changes.keys@); reveal(change_at); changes.keys@.to_set_ensures(); }
    true
}

pub fn touched_keys(delta: &[LineKeyDelta]) -> (result: Vec<i64>)
    ensures result@.no_duplicates(), forall|index: int| #![trigger delta@[index]]
        0 <= index < delta@.len() ==> result@.contains(delta@[index].l_partkey),
{
    let mut seen: HashMap<i64, bool> = HashMap::with_capacity(delta.len());
    let mut keys = Vec::with_capacity(delta.len());
    let mut index = 0;
    let mut processed = Ghost(0int);
    while index < delta.len()
        invariant index <= delta.len(), processed@ == index as int,
            keys@.no_duplicates(), seen@.dom() == keys@.to_set(),
            forall|position: int| #![trigger delta@[position]] 0 <= position < processed@ ==> keys@.contains(delta@[position].l_partkey),
        decreases delta.len() - index,
    {
        let key = delta[index].l_partkey;
        if !seen.contains_key(&key) {
            proof { keys@.to_set_ensures(); verus_prooflib::lemma_unique_push(keys@, key); }
            seen.insert(key, true);
            keys.push(key);
            proof { keys@.to_set_ensures(); }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    keys
}

}
