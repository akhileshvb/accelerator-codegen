use vstd::prelude::*;
use std::sync::Arc;
use verus_common::sums::*;
use verus_prooflib::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::storage::*;
use crate::values::*;
use crate::model::*;
use crate::domains::*;
use crate::answer_semantics::*;
use crate::answer_updates::*;
use crate::kernel::cell_marked;
use crate::dimensions::*;

verus! {
broadcast use vstd::seq_lib::group_seq_properties;

pub open spec fn nation_keys(db: Db) -> Seq<i64> {
    db.nation.map_values(nation_project())
}

pub open spec fn nation_project() -> spec_fn(NationRow) -> i64 { |row: NationRow| row.n_nationkey as i64 }
pub open spec fn nation_multiplicity(name: Seq<u8>) -> spec_fn(NationRow) -> int {
    |row: NationRow| if row.n_name == name { 1int } else { 0int }
}

pub open spec fn named_key_weight(nations: Layers<Names>, groups: Layers<Groups>, first_key: i64,
    first: Seq<u8>, second: Seq<u8>, year: i64, revenue: bool) -> spec_fn(i64) -> int {
    |second_key: i64| named_group(nations, groups, first_key, second_key, year, first, second, year, revenue)
}

pub open spec fn named_key_sum(keys: Seq<i64>, nations: Layers<Names>, groups: Layers<Groups>,
    first: Seq<u8>, second: Seq<u8>, year: i64, revenue: bool) -> int {
    sum(keys, |first_key: i64| sum(keys, named_key_weight(nations, groups, first_key, first, second, year, revenue)))
}

pub open spec fn delta_key_weight(old_nations: Layers<Names>, new_nations: Layers<Names>,
    old_groups: Layers<Groups>, new_groups: Layers<Groups>, first_key: i64,
    first: Seq<u8>, second: Seq<u8>, year: i64, revenue: bool) -> spec_fn((i64, i64)) -> int {
    |key: (i64, i64)| named_group(new_nations, new_groups, first_key, key.0, key.1, first, second, year, revenue)
        - named_group(old_nations, old_groups, first_key, key.0, key.1, first, second, year, revenue)
}

pub proof fn lemma_named_zero(nations: Layers<Names>, groups: Layers<Groups>, first_key: i64, second_key: i64, cell_year: i64,
    first: Seq<u8>, second: Seq<u8>, year: i64, revenue: bool)
    requires cell_year != year || nation_value(nations, first_key, first) == 0 || nation_value(nations, second_key, second) == 0
        || group_value(groups, first_key, second_key, year, revenue) == 0,
    ensures named_group(nations, groups, first_key, second_key, cell_year, first, second, year, revenue) == 0,
{
    if cell_year == year {
        let left = nation_value(nations, first_key, first);
        let right = nation_value(nations, second_key, second);
        let measure = group_value(groups, first_key, second_key, year, revenue);
        assert(left == 0 || right == 0 || measure == 0);
        assert(left * (right * measure) == 0) by (nonlinear_arith)
            requires left == 0 || right == 0 || measure == 0;
    }
}

pub open spec fn delta_inner(keys: Seq<i64>, old_nations: Layers<Names>, new_nations: Layers<Names>,
    old_groups: Layers<Groups>, new_groups: Layers<Groups>, first_key: i64,
    first: Seq<u8>, second: Seq<u8>, year: i64, revenue: bool) -> int {
    sum(keys, |other: i64| delta_key_weight(old_nations, new_nations, old_groups, new_groups,
        first_key, first, second, year, revenue)((other, year)))
}

pub open spec fn delta_coverage(affected: Sparse<Groups>, old_nations: Layers<Names>, new_nations: Layers<Names>,
    old_groups: Layers<Groups>, new_groups: Layers<Groups>,
    first: Seq<u8>, second: Seq<u8>, year: i64, revenue: bool) -> bool {
    forall|left: i64, right: i64, cell_year: i64| #![trigger cell_marked(affected, left, right, cell_year)]
        !cell_marked(affected, left, right, cell_year) ==>
            delta_key_weight(old_nations, new_nations, old_groups, new_groups,
                left, first, second, year, revenue)((right, cell_year)) == 0
}

pub open spec fn key_support(keys: Seq<i64>, old_nations: Layers<Names>, new_nations: Layers<Names>, first: Seq<u8>, second: Seq<u8>) -> bool {
    forall|key: i64| #![trigger keys.contains(key)] !keys.contains(key) ==> {
        &&& nation_value(old_nations, key, first) == 0
        &&& nation_value(new_nations, key, first) == 0
        &&& nation_value(old_nations, key, second) == 0
        &&& nation_value(new_nations, key, second) == 0
    }
}

pub open spec fn numeric_marks(old_groups: Layers<Groups>, new_groups: Layers<Groups>, affected: Sparse<Groups>) -> bool {
    forall|first: i64, second: i64, year: i64, revenue: bool|
        #![trigger group_value(new_groups, first, second, year, revenue), cell_marked(affected, first, second, year)]
        !cell_marked(affected, first, second, year) ==>
            group_value(new_groups, first, second, year, revenue) == group_value(old_groups, first, second, year, revenue)
}

pub proof fn lemma_numeric_marks(old_groups: Layers<Groups>, new_groups: Layers<Groups>, delta: Sparse<Groups>, affected: Sparse<Groups>)
    requires delta.valid(),
        forall|first: i64, second: i64, year: i64, revenue: bool| #![trigger group_value(new_groups, first, second, year, revenue)]
            group_value(new_groups, first, second, year, revenue) == group_value(old_groups, first, second, year, revenue)
                + sparse_group(delta, first, second, year, revenue),
        forall|position: int, offset: int| #![trigger delta.view()[position].1.rows@[offset]]
            0 <= position < delta.view().len() && 0 <= offset < delta.view()[position].1.rows@.len() ==>
                cell_marked(affected, delta.view()[position].0, delta.view()[position].1.rows@[offset].other, delta.view()[position].1.rows@[offset].year),
    ensures numeric_marks(old_groups, new_groups, affected),
{
    delta.row_facts();
    assert forall|first: i64, second: i64, year: i64, revenue: bool|
        #![trigger group_value(new_groups, first, second, year, revenue), cell_marked(affected, first, second, year)]
        !cell_marked(affected, first, second, year) implies
            group_value(new_groups, first, second, year, revenue) == group_value(old_groups, first, second, year, revenue) by {
        if let Some(bucket) = delta.at(first) {
            let position = choose|position: int| #![trigger delta.view()[position]]
                0 <= position < delta.view().len() && delta.view()[position].0 == first && delta.at(first) == Some(*delta.view()[position].1);
            assert forall|offset: int| 0 <= offset < bucket.rows@.len() implies
                group_weight(second, year, revenue)(#[trigger] bucket.rows@[offset]) == 0 by {
                assert(cell_marked(affected, first, bucket.rows@[offset].other, bucket.rows@[offset].year));
            }
            lemma_sum_zero(bucket.rows@, group_weight(second, year, revenue));
        }
        assert(sparse_group(delta, first, second, year, revenue) == 0);
    }
}

pub proof fn lemma_group_absent(groups: Layers<Groups>, first: i64, second: i64, year: i64, revenue: bool)
    requires !group_present(groups, first, second, year),
    ensures group_value(groups, first, second, year, revenue) == 0,
{
    if let Some(bucket) = groups.at(first) {
        assert forall|position: int| 0 <= position < bucket.rows@.len() implies
            group_weight(second, year, revenue)(#[trigger] bucket.rows@[position]) == 0 by {
            assert(group_keys(bucket.rows@)[position] == (bucket.rows@[position].other, bucket.rows@[position].year));
            assert(group_keys(bucket.rows@).contains((bucket.rows@[position].other, bucket.rows@[position].year)));
        }
        lemma_sum_zero(bucket.rows@, group_weight(second, year, revenue));
    }
}

pub proof fn lemma_nations_unchanged(old_nations: Layers<Names>, new_nations: Layers<Names>, deltas: Seq<NationDelta>)
    requires forall|key: i64, name: Seq<u8>| #![trigger nation_value(new_nations, key, name)]
        nation_value(new_nations, key, name) == nation_value(old_nations, key, name) + nation_delta_count(deltas, key, name),
    ensures forall|key: i64, name: Seq<u8>| #![trigger nation_value(new_nations, key, name)]
        !nation_touched(deltas, key) ==> nation_value(new_nations, key, name) == nation_value(old_nations, key, name),
{
    assert forall|key: i64, name: Seq<u8>| #![trigger nation_value(new_nations, key, name)]
        !nation_touched(deltas, key) implies nation_value(new_nations, key, name) == nation_value(old_nations, key, name) by {
        assert forall|position: int| 0 <= position < deltas.len() implies nation_signed_weight(key, name)(#[trigger] deltas[position]) == 0 by {
            assert(deltas[position].row.n_nationkey != key);
        }
        lemma_sum_zero(deltas, nation_signed_weight(key, name));
    }
}

pub proof fn lemma_delta_coverage(affected: Sparse<Groups>, old_nations: Layers<Names>, new_nations: Layers<Names>,
    old_groups: Layers<Groups>, new_groups: Layers<Groups>, deltas: Seq<NationDelta>,
    first: Seq<u8>, second: Seq<u8>, year: i64, revenue: bool)
    requires numeric_marks(old_groups, new_groups, affected), nation_marks(old_groups, new_groups, deltas, affected),
        forall|key: i64, name: Seq<u8>| #![trigger nation_value(new_nations, key, name)]
            !nation_touched(deltas, key) ==> nation_value(new_nations, key, name) == nation_value(old_nations, key, name),
    ensures delta_coverage(affected, old_nations, new_nations, old_groups, new_groups, first, second, year, revenue),
{
    assert forall|left: i64, right: i64, cell_year: i64| #![trigger cell_marked(affected, left, right, cell_year)]
        !cell_marked(affected, left, right, cell_year) implies
            delta_key_weight(old_nations, new_nations, old_groups, new_groups, left, first, second, year, revenue)((right, cell_year)) == 0 by {
        if cell_year == year {
            assert(group_value(new_groups, left, right, year, revenue) == group_value(old_groups, left, right, year, revenue));
            if nation_touched(deltas, left) || nation_touched(deltas, right) {
                assert(!group_present(old_groups, left, right, year));
                assert(!group_present(new_groups, left, right, year));
                lemma_group_absent(old_groups, left, right, year, revenue);
                lemma_group_absent(new_groups, left, right, year, revenue);
                lemma_named_zero(old_nations, old_groups, left, right, cell_year, first, second, year, revenue);
                lemma_named_zero(new_nations, new_groups, left, right, cell_year, first, second, year, revenue);
            } else {
                assert(nation_value(new_nations, left, first) == nation_value(old_nations, left, first));
                assert(nation_value(new_nations, right, second) == nation_value(old_nations, right, second));
            }
        }
    }
}

pub proof fn lemma_nation_absent(db: Db, keys: Seq<i64>, key: i64, name: Seq<u8>)
    requires database_domain(db), !keys.contains(key),
        forall|position: int| #![trigger nation_keys(db)[position]] 0 <= position < nation_keys(db).len() ==>
            keys.contains(nation_keys(db)[position]),
    ensures nation_count(db, key, name) == 0,
{
    reveal(nation_count);
    let weight = |row: NationRow| if row.n_nationkey == key && row.n_name == name { 1int } else { 0int };
    assert(nation_count(db, key, name) == sum(db.nation, weight));
    assert forall|position: int| 0 <= position < db.nation.len() implies weight(#[trigger] db.nation[position]) == 0 by {
        assert(nation_keys(db)[position] == db.nation[position].n_nationkey as i64);
        assert(keys.contains(nation_keys(db)[position]));
        assert(db.nation[position].n_nationkey as i64 as int == db.nation[position].n_nationkey);
    }
    lemma_sum_zero(db.nation, weight);
}

pub open spec fn answer_facts(db: Db, nations: Layers<Names>, groups: Layers<Groups>) -> bool {
    &&& database_domain(db)
    &&& forall|key: i64, name: Seq<u8>| #![trigger nation_value(nations, key, name)]
        nation_value(nations, key, name) == nation_count(db, key, name)
    &&& forall|left: i64, right: i64, year: i64, revenue: bool| #![trigger group_value(groups, left, right, year, revenue)]
        group_value(groups, left, right, year, revenue) == numeric_group(db, left, right, year, revenue)
}

pub proof fn lemma_answer_transaction(db: Db, db2: Db, affected: Sparse<Groups>,
    old_nations: Layers<Names>, new_nations: Layers<Names>, old_groups: Layers<Groups>, new_groups: Layers<Groups>, deltas: Seq<NationDelta>,
    first: Seq<u8>, second: Seq<u8>, year: i64, revenue: bool)
    requires answer_facts(db, old_nations, old_groups), answer_facts(db2, new_nations, new_groups),
        affected.valid(), sparse_groups_shape(affected),
        numeric_marks(old_groups, new_groups, affected), nation_marks(old_groups, new_groups, deltas, affected),
        forall|key: i64, name: Seq<u8>| #![trigger nation_value(new_nations, key, name)]
            !nation_touched(deltas, key) ==> nation_value(new_nations, key, name) == nation_value(old_nations, key, name),
    ensures answer_change(affected.view(), old_nations, new_nations, old_groups, new_groups, first, second, year, revenue)
        == named_numeric(db2, first, second, year, revenue) - named_numeric(db, first, second, year, revenue),
{
    let joined = nation_keys(db) + nation_keys(db2);
    let keys = distinct_support(joined);
    lemma_distinct_support(joined);
    assert(keys.no_duplicates());
    assert forall|position: int| #![trigger nation_keys(db)[position]] 0 <= position < nation_keys(db).len() implies
        keys.contains(nation_keys(db)[position]) by {
        assert(joined[position] == nation_keys(db)[position]);
        assert(joined.contains(nation_keys(db)[position]));
    }
    assert forall|position: int| #![trigger nation_keys(db2)[position]] 0 <= position < nation_keys(db2).len() implies
        keys.contains(nation_keys(db2)[position]) by {
        assert(joined[nation_keys(db).len() + position] == nation_keys(db2)[position]);
        assert(joined.contains(nation_keys(db2)[position]));
    }
    assert forall|key: i64| #![trigger keys.contains(key)] !keys.contains(key) implies {
        &&& nation_value(old_nations, key, first) == 0
        &&& nation_value(new_nations, key, first) == 0
        &&& nation_value(old_nations, key, second) == 0
        &&& nation_value(new_nations, key, second) == 0
    } by {
        lemma_nation_absent(db, keys, key, first); lemma_nation_absent(db, keys, key, second);
        lemma_nation_absent(db2, keys, key, first); lemma_nation_absent(db2, keys, key, second);
    }
    lemma_delta_coverage(affected, old_nations, new_nations, old_groups, new_groups, deltas, first, second, year, revenue);
    lemma_sparse_change(keys, affected, old_nations, new_nations, old_groups, new_groups, first, second, year, revenue);
    lemma_named_keys(db, old_nations, old_groups, keys, first, second, year, revenue);
    lemma_named_keys(db2, new_nations, new_groups, keys, first, second, year, revenue);
}

#[verifier::auto_ext_equal(assert, assert_by, ensures)]
pub proof fn lemma_sparse_change_row(keys: Seq<i64>, affected: Sparse<Groups>, old_nations: Layers<Names>, new_nations: Layers<Names>,
    old_groups: Layers<Groups>, new_groups: Layers<Groups>, first_key: i64,
    first: Seq<u8>, second: Seq<u8>, year: i64, revenue: bool)
    requires keys.no_duplicates(), affected.valid(), sparse_groups_shape(affected),
        delta_coverage(affected, old_nations, new_nations, old_groups, new_groups, first, second, year, revenue),
        key_support(keys, old_nations, new_nations, first, second),
    ensures delta_inner(keys, old_nations, new_nations, old_groups, new_groups, first_key, first, second, year, revenue) ==
        match affected.at(first_key) {
            Some(bucket) => sum(bucket.rows@, change_weight(old_nations, new_nations, old_groups, new_groups, first_key, first, second, year, revenue)),
            None => 0int,
        },
{
    let rows = match affected.at(first_key) { Some(bucket) => bucket.rows@, None => Seq::empty() };
    let left_project = |other: i64| (other, year);
    let right_project = |cell: Cell| (cell.other, cell.year);
    let left_one = |other: i64| 1int;
    let right_one = |cell: Cell| 1int;
    let weight = delta_key_weight(old_nations, new_nations, old_groups, new_groups, first_key, first, second, year, revenue);
    let left_keys = keys.map_values(left_project);
    let right_keys = rows.map_values(right_project);
    assert(left_keys.no_duplicates()) by {
        assert forall|left_index: int, right_index: int| 0 <= left_index < left_keys.len() && 0 <= right_index < left_keys.len() && left_index != right_index implies
            (#[trigger] left_keys[left_index]) != (#[trigger] left_keys[right_index]) by {
            assert(keys[left_index] != keys[right_index]);
        }
    }
    assert(right_keys.no_duplicates());
    assert forall|key: (i64, i64)| #[trigger] left_keys.contains(key) == (keys.contains(key.0) && key.1 == year) by {
        if left_keys.contains(key) {
            let position = choose|position: int| #![trigger left_keys[position]] 0 <= position < left_keys.len() && left_keys[position] == key;
            assert(keys.contains(key.0));
        }
        if keys.contains(key.0) && key.1 == year {
            let position = choose|position: int| #![trigger keys[position]] 0 <= position < keys.len() && keys[position] == key.0;
            assert(left_keys[position] == key);
            assert(left_keys.contains(key));
        }
    }
    assert forall|key: (i64, i64)| #[trigger] class_total(keys, left_project, left_one, key) * weight(key) ==
        class_total(rows, right_project, right_one, key) * weight(key) by {
        lemma_unique_class_total(keys, left_project, key);
        lemma_unique_class_total(rows, right_project, key);
        assert(right_keys.contains(key) == cell_marked(affected, first_key, key.0, key.1));
        if left_keys.contains(key) != right_keys.contains(key) {
            if !right_keys.contains(key) {
                assert(!cell_marked(affected, first_key, key.0, key.1));
                assert(delta_key_weight(old_nations, new_nations, old_groups, new_groups,
                    first_key, first, second, year, revenue)((key.0, key.1)) == 0);
                assert(weight(key) == 0);
            } else if key.1 != year {
                assert(weight(key) == 0);
            } else {
                assert(!keys.contains(key.0));
                assert(nation_value(old_nations, key.0, second) == 0);
                assert(nation_value(new_nations, key.0, second) == 0);
                lemma_named_zero(old_nations, old_groups, first_key, key.0, key.1, first, second, year, revenue);
                lemma_named_zero(new_nations, new_groups, first_key, key.0, key.1, first, second, year, revenue);
                assert(weight(key) == 0);
            }
        }
    }
    lemma_equal_weighted_classes(keys, rows, left_project, right_project, left_one, right_one, weight, weight);
    lemma_sum_ext_all(keys, |other: i64| left_one(other) * weight(left_project(other)),
        |other: i64| delta_key_weight(old_nations, new_nations, old_groups, new_groups, first_key, first, second, year, revenue)((other, year)));
    lemma_sum_ext_all(rows, |cell: Cell| right_one(cell) * weight(right_project(cell)),
        change_weight(old_nations, new_nations, old_groups, new_groups, first_key, first, second, year, revenue));
    if affected.at(first_key).is_none() {
        lemma_sum_empty(rows, change_weight(old_nations, new_nations, old_groups, new_groups, first_key, first, second, year, revenue));
    }
}

#[verifier::auto_ext_equal(assert, assert_by, ensures)]
pub proof fn lemma_sparse_change(keys: Seq<i64>, affected: Sparse<Groups>, old_nations: Layers<Names>, new_nations: Layers<Names>,
    old_groups: Layers<Groups>, new_groups: Layers<Groups>, first: Seq<u8>, second: Seq<u8>, year: i64, revenue: bool)
    requires keys.no_duplicates(), affected.valid(), sparse_groups_shape(affected),
        delta_coverage(affected, old_nations, new_nations, old_groups, new_groups, first, second, year, revenue),
        key_support(keys, old_nations, new_nations, first, second),
    ensures answer_change(affected.view(), old_nations, new_nations, old_groups, new_groups, first, second, year, revenue)
        == named_key_sum(keys, new_nations, new_groups, first, second, year, revenue)
            - named_key_sum(keys, old_nations, old_groups, first, second, year, revenue),
{
    affected.row_facts(); affected.unique_keys();
    let left_project = |key: i64| key;
    let right_project = |entry: (i64, Arc<Groups>)| entry.0;
    let left_one = |key: i64| 1int;
    let right_one = |entry: (i64, Arc<Groups>)| 1int;
    let left_weight = |key: i64| delta_inner(keys, old_nations, new_nations, old_groups, new_groups, key, first, second, year, revenue);
    let right_weight = |key: i64| match affected.at(key) {
        Some(bucket) => sum(bucket.rows@, change_weight(old_nations, new_nations, old_groups, new_groups, key, first, second, year, revenue)),
        None => 0int,
    };
    let row_keys = affected.view().map_values(right_project);
    assert(keys.map_values(left_project) =~= keys);
    assert(row_keys.no_duplicates());
    assert forall|key: i64| #[trigger] row_keys.contains(key) == affected.at(key).is_some() by {
        if row_keys.contains(key) {
            let position = choose|position: int| #![trigger row_keys[position]] 0 <= position < row_keys.len() && row_keys[position] == key;
            assert(affected.at(key) == Some(*affected.view()[position].1));
        }
        if affected.at(key).is_some() {
            let position = choose|position: int| #![trigger affected.view()[position]]
                0 <= position < affected.view().len() && affected.view()[position].0 == key;
            assert(row_keys[position] == key);
            assert(row_keys.contains(key));
        }
    }
    assert forall|key: i64| #[trigger] class_total(keys, left_project, left_one, key) * left_weight(key) ==
        class_total(affected.view(), right_project, right_one, key) * right_weight(key) by {
        lemma_unique_class_total(keys, left_project, key);
        lemma_unique_class_total(affected.view(), right_project, key);
        lemma_sparse_change_row(keys, affected, old_nations, new_nations, old_groups, new_groups, key, first, second, year, revenue);
        if !keys.contains(key) {
            lemma_sum_zero(keys, |other: i64| delta_key_weight(old_nations, new_nations, old_groups, new_groups, key, first, second, year, revenue)((other, year)));
            assert(left_weight(key) == 0);
        }
        if !row_keys.contains(key) { assert(right_weight(key) == 0); }
    }
    lemma_equal_weighted_classes(keys, affected.view(), left_project, right_project, left_one, right_one, left_weight, right_weight);
    lemma_sum_ext_all(keys, |key: i64| left_one(key) * left_weight(left_project(key)), left_weight);
    lemma_sum_ext(affected.view(), |entry: (i64, Arc<Groups>)| right_one(entry) * right_weight(right_project(entry)),
        bucket_change_weight(old_nations, new_nations, old_groups, new_groups, first, second, year, revenue));
    let old_weight = |key: i64| sum(keys, named_key_weight(old_nations, old_groups, key, first, second, year, revenue));
    let new_weight = |key: i64| sum(keys, named_key_weight(new_nations, new_groups, key, first, second, year, revenue));
    assert forall|key: i64| #[trigger] left_weight(key) == new_weight(key) - old_weight(key) by {
        lemma_sum_sub(keys, named_key_weight(new_nations, new_groups, key, first, second, year, revenue),
            named_key_weight(old_nations, old_groups, key, first, second, year, revenue),
            |other: i64| delta_key_weight(old_nations, new_nations, old_groups, new_groups, key, first, second, year, revenue)((other, year)));
    }
    lemma_sum_sub(keys, new_weight, old_weight, left_weight);
}

#[verifier::auto_ext_equal(assert, assert_by, ensures)]
pub proof fn lemma_nation_class(db: Db, key: i64, name: Seq<u8>)
    requires database_domain(db),
    ensures class_total(db.nation, nation_project(), nation_multiplicity(name), key) == nation_count(db, key, name),
{
    reveal(nation_count);
    let project = nation_project();
    let multiplicity = nation_multiplicity(name);
    let raw = |row: NationRow| if project(row) == key { multiplicity(row) } else { 0int };
    let target = |row: NationRow| if row.n_nationkey == key && row.n_name == name { 1int } else { 0int };
    assert(nation_count(db, key, name) == sum(db.nation, target));
    lemma_class_total_weight(db.nation, project, multiplicity, key, target);
    lemma_sum_ext(db.nation,
        raw,
        |row: NationRow| if row.n_nationkey == key && row.n_name == name { 1int } else { 0int });
}

#[verifier::auto_ext_equal(assert, assert_by, ensures)]
pub proof fn lemma_named_keys(db: Db, nations: Layers<Names>, groups: Layers<Groups>, keys: Seq<i64>,
    first: Seq<u8>, second: Seq<u8>, year: i64, revenue: bool)
    requires database_domain(db), distinct(keys),
        forall|position: int| #![trigger nation_keys(db)[position]] 0 <= position < nation_keys(db).len() ==>
            keys.contains(nation_keys(db)[position]),
        forall|key: i64, name: Seq<u8>| #![trigger nation_value(nations, key, name)]
            nation_value(nations, key, name) == nation_count(db, key, name),
        forall|left: i64, right: i64, cell_year: i64, measure: bool| #![trigger group_value(groups, left, right, cell_year, measure)]
            group_value(groups, left, right, cell_year, measure) == numeric_group(db, left, right, cell_year, measure),
    ensures named_key_sum(keys, nations, groups, first, second, year, revenue) == named_numeric(db, first, second, year, revenue),
{
    let project = nation_project();
    let right_multiplicity = nation_multiplicity(second);
    let left_multiplicity = nation_multiplicity(first);
    assert forall|position: int| 0 <= position < db.nation.len() implies keys.contains(project(#[trigger] db.nation[position])) by {
        assert(nation_keys(db)[position] == project(db.nation[position]));
    }
    let inner = |left_key: i64| sum(keys, |right_key: i64|
        nation_value(nations, right_key, second) * group_value(groups, left_key, right_key, year, revenue));
    let raw_outer = |left: NationRow| if left.n_name == first {
        sum(db.nation, |right: NationRow| if right.n_name == second {
            numeric_group(db, left.n_nationkey as i64, right.n_nationkey as i64, year, revenue) } else { 0int })
    } else { 0int };
    assert forall|left: NationRow| #[trigger] raw_outer(left) == left_multiplicity(left) * inner(project(left)) by {
        let weight = |right_key: i64| group_value(groups, project(left), right_key, year, revenue);
        lemma_weighted_classes(db.nation, keys, project, right_multiplicity, weight);
        assert forall|key: i64| #[trigger] class_total(db.nation, project, right_multiplicity, key) == nation_value(nations, key, second) by {
            lemma_nation_class(db, key, second);
        }
        lemma_sum_ext_all(keys, |key: i64| class_total(db.nation, project, right_multiplicity, key) * weight(key),
            |key: i64| nation_value(nations, key, second) * group_value(groups, project(left), key, year, revenue));
        lemma_sum_ext_all(db.nation, |right: NationRow| right_multiplicity(right) * weight(project(right)),
            |right: NationRow| if right.n_name == second {
                numeric_group(db, left.n_nationkey as i64, right.n_nationkey as i64, year, revenue) } else { 0int });
    }
    lemma_sum_ext_all(db.nation, raw_outer, |left: NationRow| left_multiplicity(left) * inner(project(left)));
    lemma_weighted_classes(db.nation, keys, project, left_multiplicity, inner);
    assert forall|key: i64| #[trigger] class_total(db.nation, project, left_multiplicity, key) == nation_value(nations, key, first) by {
        lemma_nation_class(db, key, first);
    }
    assert forall|key: i64| #[trigger] named_key_weight(nations, groups, key, first, second, year, revenue) ==
        (|other: i64| nation_value(nations, key, first) *
            (nation_value(nations, other, second) * group_value(groups, key, other, year, revenue))) by {}
    assert forall|key: i64| #[trigger] inner(key) * nation_value(nations, key, first) ==
        sum(keys, named_key_weight(nations, groups, key, first, second, year, revenue)) by {
        lemma_sum_scale(keys, |other: i64| nation_value(nations, other, second) * group_value(groups, key, other, year, revenue),
            named_key_weight(nations, groups, key, first, second, year, revenue), nation_value(nations, key, first));
        assert(inner(key) * nation_value(nations, key, first) == nation_value(nations, key, first) * inner(key)) by (nonlinear_arith);
    }
    assert forall|key: i64| #[trigger] class_total(db.nation, project, left_multiplicity, key) * inner(key) ==
        sum(keys, named_key_weight(nations, groups, key, first, second, year, revenue)) by {
        assert(class_total(db.nation, project, left_multiplicity, key) == nation_value(nations, key, first));
        assert(nation_value(nations, key, first) * inner(key) == inner(key) * nation_value(nations, key, first)) by (nonlinear_arith);
    }
    lemma_sum_ext_all(keys, |key: i64| class_total(db.nation, project, left_multiplicity, key) * inner(key),
        |key: i64| sum(keys, named_key_weight(nations, groups, key, first, second, year, revenue)));
    let sql_outer = |left: NationRow| sum(db.nation, |right: NationRow|
        if left.n_name == first && right.n_name == second {
            numeric_group(db, left.n_nationkey as i64, right.n_nationkey as i64, year, revenue) } else { 0int });
    assert forall|left: NationRow| #[trigger] sql_outer(left) == raw_outer(left) by {
        if left.n_name != first {
            lemma_sum_zero(db.nation, |right: NationRow| if left.n_name == first && right.n_name == second {
                numeric_group(db, left.n_nationkey as i64, right.n_nationkey as i64, year, revenue) } else { 0int });
        } else {
            lemma_sum_ext_all(db.nation, |right: NationRow| if left.n_name == first && right.n_name == second {
                numeric_group(db, left.n_nationkey as i64, right.n_nationkey as i64, year, revenue) } else { 0int },
                |right: NationRow| if right.n_name == second {
                numeric_group(db, left.n_nationkey as i64, right.n_nationkey as i64, year, revenue) } else { 0int });
        }
    }
    lemma_sum_ext_all(db.nation, sql_outer, raw_outer);
}

}
