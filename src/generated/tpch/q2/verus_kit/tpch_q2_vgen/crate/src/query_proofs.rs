use vstd::prelude::*;
use std::sync::Arc;
use verus_common::sums::*;
use verus_prooflib::{lemma_flat_map_weight, lemma_weighted_bag_equal};
use crate::gen_spec::*;
use crate::model::*;
use crate::storage::*;
use crate::enumeration::*;
use crate::result_proofs::*;
use crate::key_proofs::*;

verus! {

pub(crate) proof fn lemma_size_weight(state: State, size: i64, key: i64, raw: spec_fn(PartRow) -> int)
    requires state.meaning(), state.db() == state.database@,
        forall|part: PartRow| #[trigger] raw(part) == if brass(part) && part.p_partkey == key as int && part.p_size == size as int { 1int } else { 0int },
    ensures size_count(state.sizes, size, key) == sum(state.db().part, raw),
{
    lemma_sum_ext_all(state.db().part, |part: PartRow| if brass(part) && part.p_partkey == key as int && part.p_size == size as int { 1int } else { 0int }, raw);
}

pub(crate) proof fn lemma_key_query_raw(state: State, size: i64, name: Seq<u8>, key: i64, weight: spec_fn(OutRow) -> int, raw: spec_fn(PartRow) -> int)
    requires forall|part: PartRow| #[trigger] raw(part) == if part.p_partkey == key as int {
        part_query_weight(state.db(), Params { p1: size as int, p2: name }, part, weight)
    } else { 0int },
    ensures key_query_weight(state, size, name, key, weight) == sum(state.db().part, raw),
{
    assert((|part: PartRow| if part.p_partkey == key as int { part_query_weight(state.db(), Params { p1: size as int, p2: name }, part, weight) } else { 0int }) =~= raw);
}

#[verifier::auto_ext_equal(assert, assert_by, ensures)]
pub(crate) proof fn lemma_index_output_weight(state: State, size: i64, name: Seq<u8>, keys: Seq<(i64, Arc<Count>)>, regions: Directory<Count>, weight: spec_fn(OutRow) -> int)
    requires state.meaning(), state.db() == state.database@, entry_keys(keys).no_duplicates(),
        forall|key: i64| #[trigger] size_count(state.sizes, size, key) != 0 ==> entry_keys(keys).contains(key),
        forall|region: i64| #[trigger] count_at(regions, region) == region_amount(state.regions, name, region),
    ensures sum(index_outputs(keys, state, size, regions), weight) == sum(rows(state.db(), Params { p1: size as int, p2: name }), weight),
{
    let db = state.db();
    let p = Params { p1: size as int, p2: name };
    let f = |part: PartRow| part_query_weight(db, p, part, weight);
    let per = |key: i64| key_query_weight(state, size, name, key, weight);
    let project = |part: PartRow| part.p_partkey as i64;
    let domain = entry_keys(keys);
    assert(distinct(domain)) by {
        assert forall|first: int, second: int| 0 <= first < domain.len() && 0 <= second < domain.len() && first != second implies
            (#[trigger] domain[first]) != (#[trigger] domain[second]) by {}
    }
    assert forall|index: int| 0 <= index < db.part.len() && f(db.part[index]) != 0 implies domain.contains(project(#[trigger] db.part[index])) by {
        let part = db.part[index];
        assert(part_selected(part, p));
        assert(fits_i64(part.p_partkey));
        let key = part.p_partkey as i64;
        let indicator = |row: PartRow| if brass(row) && row.p_partkey == key as int && row.p_size == size as int { 1int } else { 0int };
        lemma_size_weight(state, size, key, indicator);
        lemma_sum_pos(db.part, indicator, index);
        assert(size_count(state.sizes, size, key) > 0);
    }
    assert forall|key: i64| #[trigger] per(key) == sum(db.part, |part: PartRow| if project(part) == key { f(part) } else { 0int }) by {
        let left = |part: PartRow| if part.p_partkey == key as int { f(part) } else { 0int };
        let right = |part: PartRow| if project(part) == key { f(part) } else { 0int };
        lemma_key_query_raw(state, size, name, key, weight, left);
        assert forall|index: int| 0 <= index < db.part.len() implies left(#[trigger] db.part[index]) == right(db.part[index]) by {
            if part_selected(db.part[index], p) { assert(fits_i64(db.part[index].p_partkey)); }
        }
        lemma_sum_ext(db.part, left, right);
    }
    lemma_sum_group(db.part, domain, project, f, per);
    lemma_result_weight(db, p, weight);
    let raw = |entry: (i64, Arc<Count>)| per(entry.0);
    assert forall|entry: (i64, Arc<Count>)| #[trigger] raw(entry) == sum(key_output(state, entry.0, size, regions), weight) by {
        lemma_key_query_weight(state, size, name, entry.0, regions, weight);
    }
    lemma_flat_map_weight(keys, |entry: (i64, Arc<Count>)| key_output(state, entry.0, size, regions), weight, raw);
    lemma_sum_map(keys, |entry: (i64, Arc<Count>)| entry.0, per, raw);
}

pub(crate) proof fn lemma_index_output_bag(state: State, size: i64, name: Seq<u8>, keys: Seq<(i64, Arc<Count>)>, regions: Directory<Count>)
    requires state.meaning(), state.db() == state.database@, entry_keys(keys).no_duplicates(),
        forall|key: i64| #[trigger] size_count(state.sizes, size, key) != 0 ==> entry_keys(keys).contains(key),
        forall|region: i64| #[trigger] count_at(regions, region) == region_amount(state.regions, name, region),
    ensures index_outputs(keys, state, size, regions).to_multiset() == rows(state.db(), Params { p1: size as int, p2: name }).to_multiset(),
{
    let left = index_outputs(keys, state, size, regions);
    let right = rows(state.db(), Params { p1: size as int, p2: name });
    assert forall|weight: spec_fn(OutRow) -> int| #[trigger] sum(left, weight) == sum(right, weight) by {
        lemma_index_output_weight(state, size, name, keys, regions, weight);
    }
    lemma_weighted_bag_equal(left, right);
}

}
