use vstd::prelude::*;
use verus_common::sums::*;
use crate::gen_spec::*;
use crate::storage::*;
use crate::values::*;
use crate::meaning::*;
use crate::maintenance::*;
use crate::part_updates::*;
use crate::construction::*;
use crate::dimensions::*;
use crate::aggregate::*;
use crate::kernel::State;

verus! {

pub open spec fn summary_amount(db: Db, key: i64, year: int, field: int, supplier: bool) -> int {
    if supplier { supplier_total(db, key as int, year, field) } else { nation_total(db, key as int, year, field) }
}
#[verifier::spinoff_prover]
pub fn update_nation_totals(old_totals: &Pages<Vec<Total>>, old_dimensions: &Pages<Vec<Name>>,
    dimensions: &Work<Vec<Name>>, changes: &mut Work<Vec<Total>>, Ghost(db): Ghost<Db>, Ghost(db2): Ghost<Db>)
    -> (result: Option<(Work<Vec<Total>>, Work<Vec<Group>>)>)
    requires database_keys_fit(db), database_keys_fit(db2), summary_pages_ready(*old_totals, db, false),
        nation_pages_ready(*old_dimensions, db), nation_overlay_ready(*old_dimensions, *dimensions, db2),
        summary_delta_ready(*old(changes), db, db2, false),
    ensures final(changes).valid(), result matches Some((totals, next_changes)) ==>
        summary_overlay_ready(*old_totals, totals, db2, false) && group_delta_ready(next_changes, db, db2),
{
    hide(nation_total); hide(nation_count); hide(group_database);
    broadcast use vstd::seq_lib::group_seq_properties;
    seed_total_keys(changes, dimensions);
    let ghost seeded = *changes;
    let mut totals: Work<Vec<Total>> = Work::new();
    let mut next_changes: Work<Vec<Group>> = Work::new();
    proof {
        assert(summary_delta_ready(seeded, db, db2, false));
        assert forall|target: Seq<u8>, year: int, field: int| 0 <= field <= 1 implies #[trigger] work_group(next_changes, target, year, field)
            == sum(seeded.keys@.take(0), nation_difference(db, db2, target, year, field)) by {
            lemma_sum_empty(seeded.keys@.take(0), nation_difference(db, db2, target, year, field));
        }
    }
    let mut index = 0;
    while index < changes.keys.len()
        invariant index <= changes.keys.len(), *changes == seeded, seeded.valid(), totals.valid(), groups_work_hashed(next_changes),
            database_keys_fit(db), database_keys_fit(db2), summary_delta_ready(seeded, db, db2, false),
            summary_pages_ready(*old_totals, db, false), nation_pages_ready(*old_dimensions, db),
            nation_overlay_ready(*old_dimensions, *dimensions, db2),
            forall|key: i64| #[trigger] dimensions.rows@.contains_key(key) ==> seeded.keys@.contains(key),
            forall|key: i64| #[trigger] totals.rows@.contains_key(key) <==> seeded.keys@.take(index as int).contains(key),
            forall|key: i64| #[trigger] totals.rows@.contains_key(key) ==> totals_valid(totals.rows@[key]@),
            forall|key: i64, year: int, field: int| 0 <= field <= 1 && totals.rows@.contains_key(key) ==>
                #[trigger] work_total(totals, key, year, field) == nation_total(db2, key as int, year, field),
            forall|target: Seq<u8>, year: int, field: int| 0 <= field <= 1 ==> #[trigger] work_group(next_changes, target, year, field)
                == sum(seeded.keys@.take(index as int), nation_difference(db, db2, target, year, field)),
        decreases changes.keys.len() - index,
    {
        let key = changes.keys[index];
        let before = match old_totals.get(key) { Some(rows) => copy_totals(&rows), None => Vec::new() };
        let mut after = copy_totals(&before);
        merge_totals(&mut after, changes.rows.get(&key)?)?;
        prune_totals(&mut after)?;
        proof {
            assert(totals_valid(after@));
            assert forall|year: int, field: int| 0 <= field <= 1 implies #[trigger] total_at(before@, year, field) == nation_total(db, key as int, year, field) by {
                assert(page_total(*old_totals, key, year, field) == nation_total(db, key as int, year, field));
                if old_totals.at(key).is_none() {
                    lemma_sum_empty(before@, |row: Total| if row.year == year { if field == 0 { row.count as int } else { row.profit as int } } else { 0int });
                }
            }
            assert forall|year: int, field: int| 0 <= field <= 1 implies #[trigger] total_at(after@, year, field) == nation_total(db2, key as int, year, field) by {
                assert(seeded.rows@.contains_key(key));
                assert(work_total(seeded, key, year, field) == total_at(seeded.rows@[key]@, year, field));
            }
        }
        let ghost previous = next_changes;
        if let Some(nations) = old_dimensions.get(key) {
            proof {
                assert forall|target: Seq<u8>| #[trigger] name_at(nations@, target) == nation_count(db, key as int, target) by {
                    assert(name_page(*old_dimensions, key, target) == nation_count(db, key as int, target));
                }
            }
            add_nation(&mut next_changes, &before, &nations, -1)?;
        }
        let ghost removed = next_changes;
        proof {
            assert forall|target: Seq<u8>, year: int, field: int| 0 <= field <= 1 implies #[trigger] work_group(removed, target, year, field)
                == work_group(previous, target, year, field) - nation_count(db, key as int, target) * nation_total(db, key as int, year, field) by {
                assert(name_page(*old_dimensions, key, target) == nation_count(db, key as int, target));
                if old_dimensions.at(key).is_none() { assert(nation_count(db, key as int, target) == 0); }
            }
        }
        let nations = match dimensions.rows.get(&key) {
            Some(nations) => copy_names(nations),
            None => match old_dimensions.get(key) { Some(nations) => copy_names(&nations), None => Vec::new() },
        };
        proof {
            assert forall|target: Seq<u8>| #[trigger] name_at(nations@, target) == nation_count(db2, key as int, target) by {
                assert(name_overlay(*old_dimensions, *dimensions, key, target) == nation_count(db2, key as int, target));
                if !dimensions.rows@.contains_key(key) && old_dimensions.at(key).is_none() {
                    lemma_sum_empty(nations@, |row: Name| if row.bytes@ == target { row.count as int } else { 0int });
                }
            }
        }
        add_nation(&mut next_changes, &after, &nations, 1)?;
        let ghost completed = after;
        let ghost prior_totals = totals;
        proof {
            assert forall|year: int, field: int| 0 <= field <= 1 implies #[trigger] total_at(completed@, year, field) == nation_total(db2, key as int, year, field) by {}
            assert forall|target: i64, year: int, field: int| 0 <= field <= 1 && prior_totals.rows@.contains_key(target) implies #[trigger] work_total(prior_totals, target, year, field) == nation_total(db2, target as int, year, field) by {}
        }
        totals.put(key, after);
        proof {
            assert forall|target: i64, year: int, field: int| 0 <= field <= 1 && totals.rows@.contains_key(target) implies #[trigger] work_total(totals, target, year, field) == nation_total(db2, target as int, year, field) by {
                if target != key { assert(work_total(totals, target, year, field) == work_total(prior_totals, target, year, field)); }
            }
            assert(seeded.keys@.take(index as int + 1) =~= seeded.keys@.take(index as int).push(key));
            verus_prooflib::lemma_push_membership(seeded.keys@.take(index as int), key);
            assert forall|target: Seq<u8>, year: int, field: int| 0 <= field <= 1 implies #[trigger] work_group(next_changes, target, year, field)
                == sum(seeded.keys@.take(index as int + 1), nation_difference(db, db2, target, year, field)) by {
                lemma_sum_take(seeded.keys@, index as int, nation_difference(db, db2, target, year, field));
                assert(total_at(completed@, year, field) == nation_total(db2, key as int, year, field));
            }
        }
        index += 1;
    }
    proof {
        assert(seeded.keys@.take(index as int) =~= seeded.keys@);
        assert forall|key: i64, year: int, field: int| !seeded.keys@.contains(key) && 0 <= field <= 1 implies
            #[trigger] nation_total(db2, key as int, year, field) == nation_total(db, key as int, year, field) by {
            assert(!seeded.rows@.contains_key(key));
            assert(work_total(seeded, key, year, field) == 0);
        }
        lemma_group_delta(*old_totals, *old_dimensions, *dimensions, next_changes, db, db2, seeded.keys@);
        assert forall|key: i64, year: int, field: int| 0 <= field <= 1 implies #[trigger] summary_overlay(*old_totals, totals, key, year, field) == nation_total(db2, key as int, year, field) by {
            if totals.rows@.contains_key(key) { assert(work_total(totals, key, year, field) == nation_total(db2, key as int, year, field)); }
            else { assert(page_total(*old_totals, key, year, field) == nation_total(db, key as int, year, field)); }
        }
        assert(summary_overlay_ready(*old_totals, totals, db2, false));
    }
    Some((totals, next_changes))
}


pub open spec fn summary_pages_ready(pages: Pages<Vec<Total>>, db: Db, supplier: bool) -> bool {
    pages.valid()
        && (forall|key: i64, year: int, field: int| 0 <= field <= 1 ==>
            #[trigger] page_total(pages, key, year, field) == summary_amount(db, key, year, field, supplier))
        && forall|key: i64| #[trigger] pages.at(key).is_some() ==> {
            let rows = pages.at(key).unwrap(); rows.len() > 0 && totals_valid(rows@)
        }
}

pub open spec fn summary_overlay(pages: Pages<Vec<Total>>, work: Work<Vec<Total>>, key: i64, year: int, field: int) -> int {
    if work.rows@.contains_key(key) { total_at(work.rows@[key]@, year, field) } else { page_total(pages, key, year, field) }
}

pub open spec fn summary_overlay_ready(pages: Pages<Vec<Total>>, work: Work<Vec<Total>>, db: Db, supplier: bool) -> bool {
    work.valid() && pages.valid()
        && (forall|key: i64| #[trigger] pages.at(key).is_some() ==> {
            let rows = pages.at(key).unwrap(); rows.len() > 0 && totals_valid(rows@)
        })
        && (forall|key: i64, year: int, field: int| 0 <= field <= 1 ==>
            #[trigger] summary_overlay(pages, work, key, year, field) == summary_amount(db, key, year, field, supplier))
        && forall|key: i64| #[trigger] work.rows@.contains_key(key) ==> totals_valid(work.rows@[key]@)
}

pub open spec fn summary_delta_ready(work: Work<Vec<Total>>, db: Db, db2: Db, supplier: bool) -> bool {
    work.valid() && forall|key: i64, year: int, field: int| 0 <= field <= 1 ==>
        #[trigger] work_total(work, key, year, field) == summary_amount(db2, key, year, field, supplier) - summary_amount(db, key, year, field, supplier)
}

#[verifier::spinoff_prover]
pub proof fn lemma_supplier_delta(state: State, parts: Work<Part>, changes: Work<Vec<Total>>, db: Db)
    requires state.valid(), parts_normalized(state, parts, db), database_keys_fit(db), changes.valid(),
        forall|supplier: i64, year: int, field: int| 0 <= field <= 1 ==>
            #[trigger] work_total(changes, supplier, year, field) == sum(parts.keys@, part_difference_weight(state.db(), db, supplier, year, field)),
    ensures summary_delta_ready(changes, state.db(), db, true),
{
    hide(green); hide(moments); hide(pair_count); hide(pair_cost);
    broadcast use vstd::seq_lib::group_seq_properties;
    state.lemma_base_facts();
    let keys = parts.keys@.map_values(|key: i64| key as int);
    assert(keys.no_duplicates());
    assert forall|supplier: i64, year: int, field: int| 0 <= field <= 1 implies
        #[trigger] work_total(changes, supplier, year, field) == supplier_total(db, supplier as int, year, field) - supplier_total(state.db(), supplier as int, year, field) by {
        let old_per = |key: int| part_database(state.db(), key, supplier as int, year, field);
        let new_per = |key: int| part_database(db, key, supplier as int, year, field);
        assert forall|key: int| #[trigger] old_per(key) == sum(state.db().partsupp, |row: PartsuppRow| if row.ps_partkey == key { supplier_source(state.db(), supplier as int, year, field)(row) } else { 0int }) by {
            lemma_supplier_per_part(state.db(), key, supplier as int, year, field);
        }
        assert forall|key: int| #[trigger] new_per(key) == sum(db.partsupp, |row: PartsuppRow| if row.ps_partkey == key { supplier_source(db, supplier as int, year, field)(row) } else { 0int }) by {
            lemma_supplier_per_part(db, key, supplier as int, year, field);
        }
        assert forall|key: int| !keys.contains(key) implies #[trigger] new_per(key) == old_per(key) by {
            if i64::MIN <= key <= i64::MAX {
                let narrow = key as i64;
                assert(narrow as int == key);
                assert(!parts.keys@.contains(narrow)) by {
                    if parts.keys@.contains(narrow) {
                        let position = choose|position: int| 0 <= position < parts.keys.len() && #[trigger] parts.keys[position] == narrow;
                        assert(keys[position] == key);
                    }
                }
                assert(!parts.rows@.contains_key(narrow));
                assert(overlay_green(state, parts, narrow) == cached_green(state, narrow));
                assert(cached_green(state, narrow) == green(state.db(), key));
                assert(green(db, key) == green(state.db(), key));
                if green(db, key) > 0 {
                    assert(state.part_model(narrow).is_some());
                    let part = state.part_model(narrow).unwrap();
                    assert(part_valid(part, state.db(), key));
                    assert(overlay_pair(state, parts, narrow, supplier as int, 0) == active_pair(db, narrow, supplier as int, 0));
                    assert(overlay_pair(state, parts, narrow, supplier as int, 1) == active_pair(db, narrow, supplier as int, 1));
                    assert(cached_pair(state, narrow, supplier as int, 0) == pair_count(state.db(), key, supplier as int));
                    assert(cached_pair(state, narrow, supplier as int, 1) == pair_cost(state.db(), key, supplier as int));
                    assert(pair_count(db, key, supplier as int) == pair_count(state.db(), key, supplier as int));
                    assert(pair_cost(db, key, supplier as int) == pair_cost(state.db(), key, supplier as int));
                    assert forall|component: int| 0 <= component <= 2 implies #[trigger] moments(db, key, supplier as int, year, component) == moments(state.db(), key, supplier as int, year, component) by {
                        assert(overlay_moment(state, parts, narrow, supplier as int, year, component) == cached_moment(state, narrow, supplier as int, year, component));
                    }
                }
            } else {
                crate::database::lemma_green_support(db, key);
                crate::database::lemma_green_support(state.db(), key);
                assert(green(db, key) == 0 && green(state.db(), key) == 0);
            }
        }
        let key_of = |row: PartsuppRow| row.ps_partkey;
        assert forall|key: int| #[trigger] old_per(key) == sum(state.db().partsupp, |row: PartsuppRow| if key_of(row) == key { supplier_source(state.db(), supplier as int, year, field)(row) } else { 0int }) by {
            lemma_sum_ext_all(state.db().partsupp, |row: PartsuppRow| if row.ps_partkey == key { supplier_source(state.db(), supplier as int, year, field)(row) } else { 0int }, |row: PartsuppRow| if key_of(row) == key { supplier_source(state.db(), supplier as int, year, field)(row) } else { 0int });
        }
        assert forall|key: int| #[trigger] new_per(key) == sum(db.partsupp, |row: PartsuppRow| if key_of(row) == key { supplier_source(db, supplier as int, year, field)(row) } else { 0int }) by {
            lemma_sum_ext_all(db.partsupp, |row: PartsuppRow| if row.ps_partkey == key { supplier_source(db, supplier as int, year, field)(row) } else { 0int }, |row: PartsuppRow| if key_of(row) == key { supplier_source(db, supplier as int, year, field)(row) } else { 0int });
        }
        verus_prooflib::lemma_sparse_partition_delta(state.db().partsupp, db.partsupp, keys, key_of,
            supplier_source(state.db(), supplier as int, year, field), supplier_source(db, supplier as int, year, field), old_per, new_per);
        lemma_sum_map(parts.keys@, |key: i64| key as int, |key: int| new_per(key) - old_per(key), part_difference_weight(state.db(), db, supplier, year, field));
        lemma_sum_ext_all(state.db().partsupp, supplier_source(state.db(), supplier as int, year, field),
            |pair: PartsuppRow| if pair.ps_suppkey == supplier as int { green(state.db(), pair.ps_partkey) * if field == 0 { moments(state.db(), pair.ps_partkey, supplier as int, year, 0) } else { moments(state.db(), pair.ps_partkey, supplier as int, year, 1) - pair.ps_supplycost * moments(state.db(), pair.ps_partkey, supplier as int, year, 2) } } else { 0int });
        lemma_sum_ext_all(db.partsupp, supplier_source(db, supplier as int, year, field),
            |pair: PartsuppRow| if pair.ps_suppkey == supplier as int { green(db, pair.ps_partkey) * if field == 0 { moments(db, pair.ps_partkey, supplier as int, year, 0) } else { moments(db, pair.ps_partkey, supplier as int, year, 1) - pair.ps_supplycost * moments(db, pair.ps_partkey, supplier as int, year, 2) } } else { 0int });
    }
}

pub open spec fn supplier_difference(db: Db, db2: Db, nation: i64, year: int, field: int) -> spec_fn(i64) -> int {
    |key: i64| supplier_count(db2, key as int, nation as int) * supplier_total(db2, key as int, year, field)
        - supplier_count(db, key as int, nation as int) * supplier_total(db, key as int, year, field)
}

pub open spec fn nation_difference(db: Db, db2: Db, name: Seq<u8>, year: int, field: int) -> spec_fn(i64) -> int {
    |key: i64| nation_count(db2, key as int, name) * nation_total(db2, key as int, year, field)
        - nation_count(db, key as int, name) * nation_total(db, key as int, year, field)
}

pub open spec fn group_delta_ready(work: Work<Vec<Group>>, db: Db, db2: Db) -> bool {
    groups_work_hashed(work) && forall|name: Seq<u8>, year: int, field: int| 0 <= field <= 1 ==>
        #[trigger] work_group(work, name, year, field) == group_database(db2, name, year, field) - group_database(db, name, year, field)
}

pub open spec fn groups_pages_ready(pages: Pages<Vec<Group>>, db: Db) -> bool {
    pages.valid()
        && (forall|name: Seq<u8>, year: int, field: int| 0 <= field <= 1 ==>
            #[trigger] page_group(pages, name, year, field) == group_database(db, name, year, field))
        && forall|key: i64| #[trigger] pages.at(key).is_some() ==> group_bucket_valid(pages.at(key).unwrap()@, key)
}

#[verifier::spinoff_prover]
pub fn update_groups(pages: &Pages<Vec<Group>>, changes: &Work<Vec<Group>>, Ghost(db): Ghost<Db>, Ghost(db2): Ghost<Db>) -> (result: Option<Pages<Vec<Group>>>)
    requires groups_pages_ready(*pages, db), group_delta_ready(*changes, db, db2),
    ensures result matches Some(next) ==> groups_pages_ready(next, db2),
{
    hide(group_database);
    broadcast use vstd::seq_lib::group_seq_properties;
    let mut next = pages.share();
    let mut index = 0;
    while index < changes.keys.len()
        invariant index <= changes.keys.len(), groups_pages_ready(*pages, db), group_delta_ready(*changes, db, db2), next.valid(),
            forall|key: i64| #[trigger] next.at(key).is_some() ==> group_bucket_valid(next.at(key).unwrap()@, key),
            forall|key: i64| !changes.keys@.take(index as int).contains(key) ==> #[trigger] next.at(key) == pages.at(key),
            forall|name: Seq<u8>, year: int, field: int| 0 <= field <= 1 ==>
                #[trigger] page_group(next, name, year, field) == page_group(*pages, name, year, field)
                    + if changes.keys@.take(index as int).contains(name_hash(name) as i64) { work_group(*changes, name, year, field) } else { 0int },
        decreases changes.keys.len() - index,
    {
        let key = changes.keys[index];
        let ghost previous = next;
        proof { verus_prooflib::lemma_prefix_fresh(changes.keys@, index as int); }
        let mut groups = match pages.get(key) { Some(groups) => copy_groups(&groups), None => Vec::new() };
        let ghost initial = groups@;
        proof {
            assert(group_entries_unique(initial));
            assert(group_hashes(initial, key));
            assert forall|name: Seq<u8>, year: int, field: int| name_hash(name) as i64 == key && 0 <= field <= 1 implies
                #[trigger] groups_at(initial, name, year, field) == page_group(*pages, name, year, field) by {
                if pages.at(key).is_none() { lemma_sum_empty(initial, group_weight(name, year, field)); }
            }
        }
        let delta = changes.rows.get(&key)?;
        proof { assert(group_hashes(delta@, key)); }
        merge_groups(&mut groups, delta)?;
        let ghost completed = groups;
        proof {
            assert(completed.len() > 0 ==> group_bucket_valid(completed@, key));
            assert forall|name: Seq<u8>, year: int, field: int| name_hash(name) as i64 == key && 0 <= field <= 1 implies
                #[trigger] groups_at(completed@, name, year, field) == page_group(*pages, name, year, field) + work_group(*changes, name, year, field) by {
                assert(work_group(*changes, name, year, field) == groups_at(delta@, name, year, field));
            }
            assert forall|name: Seq<u8>, year: int, field: int| 0 <= field <= 1 implies
                #[trigger] page_group(previous, name, year, field) == page_group(*pages, name, year, field)
                    + if changes.keys@.take(index as int).contains(name_hash(name) as i64) { work_group(*changes, name, year, field) } else { 0int } by {}
        }
        next.put(key, if groups.len() == 0 { None } else { Some(groups) });
        proof {
            assert(changes.keys@.take(index as int + 1) =~= changes.keys@.take(index as int).push(key));
            verus_prooflib::lemma_push_membership(changes.keys@.take(index as int), key);
            assert forall|name: Seq<u8>, year: int, field: int| 0 <= field <= 1 implies
                #[trigger] page_group(next, name, year, field) == page_group(*pages, name, year, field)
                    + if changes.keys@.take(index as int + 1).contains(name_hash(name) as i64) { work_group(*changes, name, year, field) } else { 0int } by {
                if name_hash(name) as i64 == key {
                    assert(next.at(key) == if completed.len() == 0 { None } else { Some(completed) });
                    assert(groups_at(completed@, name, year, field) == page_group(*pages, name, year, field) + work_group(*changes, name, year, field));
                    if completed.len() == 0 { lemma_sum_empty(completed@, group_weight(name, year, field)); }
                } else { assert(page_group(next, name, year, field) == page_group(previous, name, year, field)); }
            }
        }
        index += 1;
    }
    proof {
        assert(changes.keys@.take(index as int) =~= changes.keys@);
        assert forall|name: Seq<u8>, year: int, field: int| 0 <= field <= 1 implies
            #[trigger] page_group(next, name, year, field) == group_database(db2, name, year, field) by {
            assert(page_group(*pages, name, year, field) == group_database(db, name, year, field));
            assert(work_group(*changes, name, year, field) == group_database(db2, name, year, field) - group_database(db, name, year, field));
            if !changes.keys@.contains(name_hash(name) as i64) { assert(!changes.rows@.contains_key(name_hash(name) as i64)); }
        }
    }
    Some(next)
}

#[verifier::spinoff_prover]
pub proof fn lemma_group_delta(old_totals: Pages<Vec<Total>>, old_dimensions: Pages<Vec<Name>>, dimensions: Work<Vec<Name>>, changes: Work<Vec<Group>>, db: Db, db2: Db,
    keys: Seq<i64>)
    requires database_keys_fit(db), database_keys_fit(db2), keys.no_duplicates(), summary_pages_ready(old_totals, db, false),
        nation_pages_ready(old_dimensions, db), nation_overlay_ready(old_dimensions, dimensions, db2),
        forall|key: i64| !#[trigger] keys.contains(key) ==> !dimensions.rows@.contains_key(key),
        forall|key: i64, year: int, field: int| !keys.contains(key) && 0 <= field <= 1 ==>
            #[trigger] nation_total(db2, key as int, year, field) == nation_total(db, key as int, year, field),
        groups_work_hashed(changes),
        forall|name: Seq<u8>, year: int, field: int| 0 <= field <= 1 ==>
            #[trigger] work_group(changes, name, year, field) == sum(keys, nation_difference(db, db2, name, year, field)),
    ensures group_delta_ready(changes, db, db2),
{
    hide(nation_total); hide(nation_count);
    broadcast use vstd::seq_lib::group_seq_properties;
    let wide_keys = keys.map_values(|key: i64| key as int);
    assert(wide_keys.no_duplicates());
    assert forall|name: Seq<u8>, year: int, field: int| 0 <= field <= 1 implies #[trigger] work_group(changes, name, year, field)
        == group_database(db2, name, year, field) - group_database(db, name, year, field) by {
        let old_weight = |row: NationRow| if row.n_name == name { nation_total(db, row.n_nationkey, year, field) } else { 0int };
        let new_weight = |row: NationRow| if row.n_name == name { nation_total(db2, row.n_nationkey, year, field) } else { 0int };
        let old_per = |key: int| nation_count(db, key, name) * nation_total(db, key, year, field);
        let new_per = |key: int| nation_count(db2, key, name) * nation_total(db2, key, year, field);
        let key_of = |row: NationRow| row.n_nationkey;
        assert forall|key: int| #[trigger] old_per(key) == sum(db.nation, |row: NationRow| if key_of(row) == key { old_weight(row) } else { 0int }) by {
            lemma_group_per_nation(db, key, name, year, field);
            lemma_sum_ext_all(db.nation, |row: NationRow| if row.n_nationkey == key { if row.n_name == name { nation_total(db, row.n_nationkey, year, field) } else { 0int } } else { 0int }, |row: NationRow| if key_of(row) == key { old_weight(row) } else { 0int });
        }
        assert forall|key: int| #[trigger] new_per(key) == sum(db2.nation, |row: NationRow| if key_of(row) == key { new_weight(row) } else { 0int }) by {
            lemma_group_per_nation(db2, key, name, year, field);
            lemma_sum_ext_all(db2.nation, |row: NationRow| if row.n_nationkey == key { if row.n_name == name { nation_total(db2, row.n_nationkey, year, field) } else { 0int } } else { 0int }, |row: NationRow| if key_of(row) == key { new_weight(row) } else { 0int });
        }
        assert forall|key: int| !wide_keys.contains(key) implies #[trigger] new_per(key) == old_per(key) by {
            if i64::MIN <= key <= i64::MAX {
                let narrow = key as i64;
                assert(narrow as int == key);
                assert(!keys.contains(narrow)) by {
                    if keys.contains(narrow) {
                        let position = choose|position: int| 0 <= position < keys.len() && #[trigger] keys[position] == narrow;
                        assert(wide_keys[position] == key);
                    }
                }
                assert(name_overlay(old_dimensions, dimensions, narrow, name) == name_page(old_dimensions, narrow, name));
            } else {
                assert forall|index: int| 0 <= index < db.nation.len() implies (#[trigger] db.nation[index]).n_nationkey != key by {}
                assert forall|index: int| 0 <= index < db2.nation.len() implies (#[trigger] db2.nation[index]).n_nationkey != key by {}
                lemma_sum_zero(db.nation, |row: NationRow| if row.n_nationkey == key && row.n_name == name { 1int } else { 0int });
                lemma_sum_zero(db2.nation, |row: NationRow| if row.n_nationkey == key && row.n_name == name { 1int } else { 0int });
                reveal(nation_count);
            }
        }
        verus_prooflib::lemma_sparse_partition_delta(db.nation, db2.nation, wide_keys, key_of, old_weight, new_weight, old_per, new_per);
        lemma_sum_map(keys, |key: i64| key as int, |key: int| new_per(key) - old_per(key), nation_difference(db, db2, name, year, field));
    }
}

#[verifier::spinoff_prover]
pub proof fn lemma_nation_delta(old_totals: Pages<Vec<Total>>, old_dimensions: Pages<Vec<Number>>, dimensions: Work<Vec<Number>>, changes: Work<Vec<Total>>, db: Db, db2: Db,
    keys: Seq<i64>)
    requires database_keys_fit(db), database_keys_fit(db2), keys.no_duplicates(), summary_pages_ready(old_totals, db, true),
        supplier_pages_ready(old_dimensions, db), supplier_overlay_ready(old_dimensions, dimensions, db2),
        forall|key: i64| !#[trigger] keys.contains(key) ==> !dimensions.rows@.contains_key(key),
        forall|key: i64, year: int, field: int| !keys.contains(key) && 0 <= field <= 1 ==>
            #[trigger] supplier_total(db2, key as int, year, field) == supplier_total(db, key as int, year, field),
        changes.valid(),
        forall|nation: i64, year: int, field: int| 0 <= field <= 1 ==>
            #[trigger] work_total(changes, nation, year, field) == sum(keys, supplier_difference(db, db2, nation, year, field)),
    ensures summary_delta_ready(changes, db, db2, false),
{
    hide(supplier_total); hide(supplier_count);
    broadcast use vstd::seq_lib::group_seq_properties;
    let wide_keys = keys.map_values(|key: i64| key as int);
    assert(wide_keys.no_duplicates());
    assert forall|nation: i64, year: int, field: int| 0 <= field <= 1 implies #[trigger] work_total(changes, nation, year, field)
        == nation_total(db2, nation as int, year, field) - nation_total(db, nation as int, year, field) by {
        let old_weight = |row: SupplierRow| if row.s_nationkey == nation as int { supplier_total(db, row.s_suppkey, year, field) } else { 0int };
        let new_weight = |row: SupplierRow| if row.s_nationkey == nation as int { supplier_total(db2, row.s_suppkey, year, field) } else { 0int };
        let old_per = |key: int| supplier_count(db, key, nation as int) * supplier_total(db, key, year, field);
        let new_per = |key: int| supplier_count(db2, key, nation as int) * supplier_total(db2, key, year, field);
        assert forall|key: int| #[trigger] old_per(key) == sum(db.supplier, |row: SupplierRow| if row.s_suppkey == key { old_weight(row) } else { 0int }) by {
            lemma_nation_per_supplier(db, key, nation as int, year, field);
            lemma_sum_ext_all(db.supplier, |row: SupplierRow| if row.s_suppkey == key { if row.s_nationkey == nation as int { supplier_total(db, row.s_suppkey, year, field) } else { 0int } } else { 0int }, |row: SupplierRow| if row.s_suppkey == key { old_weight(row) } else { 0int });
        }
        assert forall|key: int| #[trigger] new_per(key) == sum(db2.supplier, |row: SupplierRow| if row.s_suppkey == key { new_weight(row) } else { 0int }) by {
            lemma_nation_per_supplier(db2, key, nation as int, year, field);
            lemma_sum_ext_all(db2.supplier, |row: SupplierRow| if row.s_suppkey == key { if row.s_nationkey == nation as int { supplier_total(db2, row.s_suppkey, year, field) } else { 0int } } else { 0int }, |row: SupplierRow| if row.s_suppkey == key { new_weight(row) } else { 0int });
        }
        assert forall|key: int| !wide_keys.contains(key) implies #[trigger] new_per(key) == old_per(key) by {
            if i64::MIN <= key <= i64::MAX {
                let narrow = key as i64;
                assert(narrow as int == key);
                assert(!keys.contains(narrow)) by {
                    if keys.contains(narrow) {
                        let position = choose|position: int| 0 <= position < keys.len() && #[trigger] keys[position] == narrow;
                        assert(wide_keys[position] == key);
                    }
                }
                assert(number_overlay(old_dimensions, dimensions, narrow, nation as int) == number_page(old_dimensions, narrow, nation as int));
            } else {
                assert forall|index: int| 0 <= index < db.supplier.len() implies (#[trigger] db.supplier[index]).s_suppkey != key by {}
                assert forall|index: int| 0 <= index < db2.supplier.len() implies (#[trigger] db2.supplier[index]).s_suppkey != key by {}
                lemma_sum_zero(db.supplier, |row: SupplierRow| if row.s_suppkey == key && row.s_nationkey == nation as int { 1int } else { 0int });
                lemma_sum_zero(db2.supplier, |row: SupplierRow| if row.s_suppkey == key && row.s_nationkey == nation as int { 1int } else { 0int });
                reveal(supplier_count);
            }
        }
        let key_of = |row: SupplierRow| row.s_suppkey;
        assert forall|key: int| #[trigger] old_per(key) == sum(db.supplier, |row: SupplierRow| if key_of(row) == key { old_weight(row) } else { 0int }) by {
            lemma_sum_ext_all(db.supplier, |row: SupplierRow| if row.s_suppkey == key { old_weight(row) } else { 0int }, |row: SupplierRow| if key_of(row) == key { old_weight(row) } else { 0int });
        }
        assert forall|key: int| #[trigger] new_per(key) == sum(db2.supplier, |row: SupplierRow| if key_of(row) == key { new_weight(row) } else { 0int }) by {
            lemma_sum_ext_all(db2.supplier, |row: SupplierRow| if row.s_suppkey == key { new_weight(row) } else { 0int }, |row: SupplierRow| if key_of(row) == key { new_weight(row) } else { 0int });
        }
        verus_prooflib::lemma_sparse_partition_delta(db.supplier, db2.supplier, wide_keys, key_of, old_weight, new_weight, old_per, new_per);
        lemma_sum_map(keys, |key: i64| key as int, |key: int| new_per(key) - old_per(key), supplier_difference(db, db2, nation, year, field));
    }
}

pub fn seed_total_keys<Value>(changes: &mut Work<Vec<Total>>, dimensions: &Work<Value>)
    requires old(changes).valid(), dimensions.valid(),
    ensures final(changes).valid(),
        forall|key: i64| #[trigger] dimensions.rows@.contains_key(key) ==> final(changes).keys@.contains(key),
        forall|key: i64, year: int, field: int| 0 <= field <= 1 ==>
            #[trigger] work_total(*final(changes), key, year, field) == work_total(*old(changes), key, year, field),
{
    broadcast use vstd::seq_lib::group_seq_properties;
    let ghost initial = *changes;
    let mut index = 0;
    while index < dimensions.keys.len()
        invariant index <= dimensions.keys.len(), dimensions.valid(), changes.valid(),
            forall|key: i64| dimensions.keys@.take(index as int).contains(key) ==> #[trigger] changes.keys@.contains(key),
            forall|key: i64, year: int, field: int| 0 <= field <= 1 ==> #[trigger] work_total(*changes, key, year, field) == work_total(initial, key, year, field),
        decreases dimensions.keys.len() - index,
    {
        let key = dimensions.keys[index];
        let ghost previous = *changes;
        if !changes.rows.contains_key(&key) {
            changes.put(key, Vec::new());
            proof {
                assert forall|target: i64, year: int, field: int| 0 <= field <= 1 implies #[trigger] work_total(*changes, target, year, field) == work_total(previous, target, year, field) by {
                    if target == key { lemma_sum_empty(changes.rows@[key]@, |row: Total| if row.year == year { if field == 0 { row.count as int } else { row.profit as int } } else { 0int }); }
                }
            }
        }
        proof {
            assert(changes.rows@.contains_key(key));
            changes.keys@.to_set_ensures();
            assert(changes.keys@.to_set().contains(key));
            assert(changes.keys@.contains(key));
            assert(dimensions.keys@.take(index as int + 1) =~= dimensions.keys@.take(index as int).push(key));
            verus_prooflib::lemma_push_membership(dimensions.keys@.take(index as int), key);
            assert forall|target: i64| dimensions.keys@.take(index as int + 1).contains(target) implies #[trigger] changes.keys@.contains(target) by {
                if target != key {
                    assert(previous.keys@.contains(target));
                    assert(changes.keys@ == previous.keys@ || changes.keys@ == previous.keys@.push(key));
                    verus_prooflib::lemma_push_membership(previous.keys@, key);
                }
            }
        }
        index += 1;
    }
    proof { assert(dimensions.keys@.take(index as int) =~= dimensions.keys@); }
}

#[verifier::spinoff_prover]
pub fn update_supplier_totals(old_totals: &Pages<Vec<Total>>, old_dimensions: &Pages<Vec<Number>>,
    dimensions: &Work<Vec<Number>>, changes: &mut Work<Vec<Total>>, Ghost(db): Ghost<Db>, Ghost(db2): Ghost<Db>)
    -> (result: Option<(Work<Vec<Total>>, Work<Vec<Total>>)>)
    requires database_keys_fit(db), database_keys_fit(db2), summary_pages_ready(*old_totals, db, true),
        supplier_pages_ready(*old_dimensions, db), supplier_overlay_ready(*old_dimensions, *dimensions, db2),
        summary_delta_ready(*old(changes), db, db2, true),
    ensures final(changes).valid(), result matches Some((totals, next_changes)) ==>
        summary_overlay_ready(*old_totals, totals, db2, true) && summary_delta_ready(next_changes, db, db2, false),
{
    hide(supplier_total); hide(supplier_count); hide(nation_total);
    broadcast use vstd::seq_lib::group_seq_properties;
    seed_total_keys(changes, dimensions);
    let ghost seeded = *changes;
    let mut totals: Work<Vec<Total>> = Work::new();
    let mut next_changes: Work<Vec<Total>> = Work::new();
    proof {
        assert(summary_delta_ready(seeded, db, db2, true));
        assert forall|nation: i64, year: int, field: int| 0 <= field <= 1 implies #[trigger] work_total(next_changes, nation, year, field)
            == sum(seeded.keys@.take(0), supplier_difference(db, db2, nation, year, field)) by {
            lemma_sum_empty(seeded.keys@.take(0), supplier_difference(db, db2, nation, year, field));
        }
    }
    let mut index = 0;
    while index < changes.keys.len()
        invariant index <= changes.keys.len(), *changes == seeded, seeded.valid(), totals.valid(), next_changes.valid(),
            database_keys_fit(db), database_keys_fit(db2), summary_delta_ready(seeded, db, db2, true),
            summary_pages_ready(*old_totals, db, true), supplier_pages_ready(*old_dimensions, db),
            supplier_overlay_ready(*old_dimensions, *dimensions, db2),
            forall|key: i64| #[trigger] dimensions.rows@.contains_key(key) ==> seeded.keys@.contains(key),
            forall|key: i64| #[trigger] totals.rows@.contains_key(key) <==> seeded.keys@.take(index as int).contains(key),
            forall|key: i64| #[trigger] totals.rows@.contains_key(key) ==> totals_valid(totals.rows@[key]@),
            forall|key: i64, year: int, field: int| 0 <= field <= 1 && totals.rows@.contains_key(key) ==>
                #[trigger] work_total(totals, key, year, field) == supplier_total(db2, key as int, year, field),
            forall|nation: i64, year: int, field: int| 0 <= field <= 1 ==> #[trigger] work_total(next_changes, nation, year, field)
                == sum(seeded.keys@.take(index as int), supplier_difference(db, db2, nation, year, field)),
        decreases changes.keys.len() - index,
    {
        let key = changes.keys[index];
        let before = match old_totals.get(key) { Some(rows) => copy_totals(&rows), None => Vec::new() };
        let mut after = copy_totals(&before);
        merge_totals(&mut after, changes.rows.get(&key)?)?;
        prune_totals(&mut after)?;
        proof {
            assert(totals_valid(after@));
            assert forall|year: int, field: int| 0 <= field <= 1 implies #[trigger] total_at(before@, year, field) == supplier_total(db, key as int, year, field) by {
                assert(page_total(*old_totals, key, year, field) == supplier_total(db, key as int, year, field));
                if old_totals.at(key).is_none() {
                    lemma_sum_empty(before@, |row: Total| if row.year == year { if field == 0 { row.count as int } else { row.profit as int } } else { 0int });
                }
            }
            assert forall|year: int, field: int| 0 <= field <= 1 implies #[trigger] total_at(after@, year, field) == supplier_total(db2, key as int, year, field) by {
                assert(seeded.rows@.contains_key(key));
                assert(work_total(seeded, key, year, field) == total_at(seeded.rows@[key]@, year, field));
            }
        }
        let ghost previous = next_changes;
        if let Some(nations) = old_dimensions.get(key) {
            proof {
                assert forall|nation: i64| #[trigger] number_at(nations@, nation as int) == supplier_count(db, key as int, nation as int) by {
                    assert(number_page(*old_dimensions, key, nation as int) == supplier_count(db, key as int, nation as int));
                }
            }
            add_supplier(&mut next_changes, &before, &nations, -1)?;
        }
        let ghost removed = next_changes;
        proof {
            assert forall|nation: i64, year: int, field: int| 0 <= field <= 1 implies #[trigger] work_total(removed, nation, year, field)
                == work_total(previous, nation, year, field) - supplier_count(db, key as int, nation as int) * supplier_total(db, key as int, year, field) by {
                assert(number_page(*old_dimensions, key, nation as int) == supplier_count(db, key as int, nation as int));
                if old_dimensions.at(key).is_none() { assert(supplier_count(db, key as int, nation as int) == 0); }
            }
        }
        let nations = match dimensions.rows.get(&key) {
            Some(nations) => copy_numbers(nations),
            None => match old_dimensions.get(key) { Some(nations) => copy_numbers(&nations), None => Vec::new() },
        };
        proof {
            assert forall|nation: i64| #[trigger] number_at(nations@, nation as int) == supplier_count(db2, key as int, nation as int) by {
                assert(number_overlay(*old_dimensions, *dimensions, key, nation as int) == supplier_count(db2, key as int, nation as int));
                if !dimensions.rows@.contains_key(key) && old_dimensions.at(key).is_none() {
                    lemma_sum_empty(nations@, |row: Number| if row.key == nation as int { row.count as int } else { 0int });
                }
            }
        }
        add_supplier(&mut next_changes, &after, &nations, 1)?;
        let ghost completed = after;
        let ghost prior_totals = totals;
        proof {
            assert forall|year: int, field: int| 0 <= field <= 1 implies #[trigger] total_at(completed@, year, field) == supplier_total(db2, key as int, year, field) by {}
            assert forall|target: i64, year: int, field: int| 0 <= field <= 1 && prior_totals.rows@.contains_key(target) implies #[trigger] work_total(prior_totals, target, year, field) == supplier_total(db2, target as int, year, field) by {}
        }
        totals.put(key, after);
        proof {
            assert forall|target: i64, year: int, field: int| 0 <= field <= 1 && totals.rows@.contains_key(target) implies #[trigger] work_total(totals, target, year, field) == supplier_total(db2, target as int, year, field) by {
                if target != key { assert(work_total(totals, target, year, field) == work_total(prior_totals, target, year, field)); }
            }
            assert(seeded.keys@.take(index as int + 1) =~= seeded.keys@.take(index as int).push(key));
            verus_prooflib::lemma_push_membership(seeded.keys@.take(index as int), key);
            assert forall|nation: i64, year: int, field: int| 0 <= field <= 1 implies #[trigger] work_total(next_changes, nation, year, field)
                == sum(seeded.keys@.take(index as int + 1), supplier_difference(db, db2, nation, year, field)) by {
                lemma_sum_take(seeded.keys@, index as int, supplier_difference(db, db2, nation, year, field));
                assert(total_at(completed@, year, field) == supplier_total(db2, key as int, year, field));
            }
        }
        index += 1;
    }
    proof {
        assert(seeded.keys@.take(index as int) =~= seeded.keys@);
        assert forall|key: i64, year: int, field: int| !seeded.keys@.contains(key) && 0 <= field <= 1 implies
            #[trigger] supplier_total(db2, key as int, year, field) == supplier_total(db, key as int, year, field) by {
            assert(!seeded.rows@.contains_key(key));
            assert(work_total(seeded, key, year, field) == 0);
        }
        lemma_nation_delta(*old_totals, *old_dimensions, *dimensions, next_changes, db, db2, seeded.keys@);
        assert forall|key: i64, year: int, field: int| 0 <= field <= 1 implies #[trigger] summary_overlay(*old_totals, totals, key, year, field) == supplier_total(db2, key as int, year, field) by {
            if totals.rows@.contains_key(key) { assert(work_total(totals, key, year, field) == supplier_total(db2, key as int, year, field)); }
            else { assert(page_total(*old_totals, key, year, field) == supplier_total(db, key as int, year, field)); }
        }
        assert(summary_overlay_ready(*old_totals, totals, db2, true));
    }
    Some((totals, next_changes))
}

#[verifier::spinoff_prover]
pub fn publish_totals(pages: &Pages<Vec<Total>>, work: &mut Work<Vec<Total>>, Ghost(db): Ghost<Db>, Ghost(supplier): Ghost<bool>) -> (result: Option<Pages<Vec<Total>>>)
    requires pages.valid(), summary_overlay_ready(*pages, *old(work), db, supplier),
    ensures final(work).valid(), result matches Some(result) ==> summary_pages_ready(result, db, supplier),
{
    hide(supplier_total); hide(nation_total);
    broadcast use vstd::seq_lib::group_seq_properties;
    let ghost initial = *work;
    let mut next = pages.share();
    let mut index = 0;
    while index < work.keys.len()
        invariant index <= work.keys.len(), work.valid(), next.valid(), pages.valid(),
            initial == *old(work), summary_overlay_ready(*pages, initial, db, supplier), work.keys@ == initial.keys@,
            forall|key: i64| #[trigger] work.rows@.contains_key(key) <==> initial.rows@.contains_key(key) && !initial.keys@.take(index as int).contains(key),
            forall|key: i64| #[trigger] work.rows@.contains_key(key) ==> work.rows@[key] == initial.rows@[key],
            forall|key: i64| #[trigger] next.at(key) == if initial.keys@.take(index as int).contains(key) && initial.rows@.contains_key(key) {
                let rows = initial.rows@[key]; if rows.len() == 0 { None } else { Some(rows) }
            } else { pages.at(key) },
        decreases work.keys.len() - index,
    {
        let key = work.keys[index];
        if let Some(rows) = work.take(key) {
            next.put(key, if rows.len() == 0 { None } else { Some(rows) });
        }
        proof {
            assert(initial.keys@.take(index as int + 1) =~= initial.keys@.take(index as int).push(key));
            verus_prooflib::lemma_push_membership(initial.keys@.take(index as int), key);
            assert forall|target: i64| #[trigger] next.at(target) == if initial.keys@.take(index as int + 1).contains(target) && initial.rows@.contains_key(target) {
                let rows = initial.rows@[target]; if rows.len() == 0 { None } else { Some(rows) }
            } else { pages.at(target) } by {}
        }
        index += 1;
    }
    proof {
        assert(initial.keys@.take(index as int) =~= initial.keys@);
        assert forall|key: i64, year: int, field: int| 0 <= field <= 1 implies #[trigger] page_total(next, key, year, field) == summary_amount(db, key, year, field, supplier) by {
            assert(summary_overlay(*pages, initial, key, year, field) == summary_amount(db, key, year, field, supplier));
            if initial.rows@.contains_key(key) { assert(initial.keys@.contains(key)); }
            if initial.rows@.contains_key(key) && initial.rows@[key].len() == 0 {
                lemma_sum_empty(initial.rows@[key]@, |row: Total| if row.year == year { if field == 0 { row.count as int } else { row.profit as int } } else { 0int });
            }
        }
    }
    Some(next)
}

}
