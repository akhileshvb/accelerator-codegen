use vstd::prelude::*;
use verus_prooflib::wide::Wide;
use std::sync::Arc;
use verus_common::sums::*;
use verus_prooflib::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::storage::*;
use crate::values::*;
use crate::model::*;
use crate::directories::*;
use crate::incident_updates::*;
use crate::kernel::{cell_marked, mark_bucket};
use crate::kernel::find_cell;
use crate::joins::*;
use crate::expansion::*;

verus! {
broadcast use vstd::seq_lib::group_seq_properties;

pub open spec fn group_present(groups: Layers<Groups>, first: i64, second: i64, year: i64) -> bool {
    groups.at(first) matches Some(bucket) && group_keys(bucket.rows@).contains((second, year))
}

pub open spec fn group_shape(groups: Layers<Groups>) -> bool {
    forall|key: i64| #![trigger groups.at(key)] groups.at(key) matches Some(bucket) ==> groups_shape(bucket)
}

pub open spec fn marks_preserved(before: Sparse<Groups>, after: Sparse<Groups>) -> bool {
    forall|first: i64, second: i64, year: i64| #![trigger cell_marked(after, first, second, year)]
        cell_marked(before, first, second, year) ==> cell_marked(after, first, second, year)
}

pub fn mark_second(old_groups: &Layers<Groups>, new_groups: &Layers<Groups>,
    directory: &Layers<Layers<i128>>, affected: &mut Sparse<Groups>, second: i64)
    requires old(affected).valid(), sparse_groups_shape(*old(affected)),
        group_shape(*old_groups), group_shape(*new_groups),
    ensures final(affected).valid(), sparse_groups_shape(*final(affected)),
        marks_preserved(*old(affected), *final(affected)),
        forall|first: i64, year: i64| #![trigger cell_marked(*final(affected), first, second, year)]
            member_value(*directory, second, first) &&
                (group_present(*old_groups, first, second, year) || group_present(*new_groups, first, second, year)) ==>
                    cell_marked(*final(affected), first, second, year),
{
    let ghost initial = *affected;
    if let Some(bucket) = directory.get(second) {
        let first_keys = bucket.keys();
        let mut offset = 0;
        while offset < first_keys.len()
            invariant offset <= first_keys@.len(), affected.valid(), sparse_groups_shape(*affected),
                group_shape(*old_groups), group_shape(*new_groups),
                marks_preserved(initial, *affected), initial == *old(affected),
                directory.at(second) == Some(*bucket),
                forall|key: i64| #![trigger bucket.at(key)] bucket.at(key).is_some() <==> first_keys@.contains(key),
                forall|position: int, year: i64| #![trigger cell_marked(*affected, first_keys@[position], second, year)]
                    0 <= position < offset && member_value(*directory, second, first_keys@[position]) &&
                    (group_present(*old_groups, first_keys@[position], second, year) ||
                     group_present(*new_groups, first_keys@[position], second, year)) ==>
                        cell_marked(*affected, first_keys@[position], second, year),
            decreases first_keys@.len() - offset,
        {
            let first = first_keys[offset];
            if let Some(present) = bucket.get(first) {
                if *present > 0 {
                    mark_bucket(old_groups, affected, first, Some(second));
                    mark_bucket(new_groups, affected, first, Some(second));
                }
            }
            offset += 1;
        }
        proof {
            assert forall|first: i64, year: i64| #![trigger cell_marked(*affected, first, second, year)]
                member_value(*directory, second, first) &&
                    (group_present(*old_groups, first, second, year) || group_present(*new_groups, first, second, year)) implies
                        cell_marked(*affected, first, second, year) by {
                assert(bucket.at(first).is_some());
                let position = choose|position: int| #![trigger first_keys@[position]]
                    0 <= position < first_keys@.len() && first_keys@[position] == first;
            }
        }
    }
}

pub open spec fn nation_touched(deltas: Seq<NationDelta>, key: i64) -> bool {
    exists|position: int| #![trigger deltas[position]]
        0 <= position < deltas.len() && deltas[position].row.n_nationkey == key
}

pub open spec fn nation_marks(old_groups: Layers<Groups>, new_groups: Layers<Groups>,
    deltas: Seq<NationDelta>, affected: Sparse<Groups>) -> bool {
    forall|first: i64, second: i64, year: i64| #![trigger cell_marked(affected, first, second, year)]
        (nation_touched(deltas, first) || nation_touched(deltas, second)) &&
        (group_present(old_groups, first, second, year) || group_present(new_groups, first, second, year)) ==>
            cell_marked(affected, first, second, year)
}

pub proof fn lemma_present_incident(groups: Layers<Groups>, directory: Layers<Layers<i128>>,
    first: i64, second: i64, year: i64)
    requires group_present(groups, first, second, year),
        forall|key: i64| #![trigger groups.at(key)] groups.at(key) matches Some(bucket) ==> groups_valid(bucket),
        forall|other: i64, key: i64| #![trigger member_value(directory, other, key)]
            member_value(directory, other, key) == group_has(groups, key, other),
    ensures member_value(directory, second, first),
{
    let bucket = groups.at(first).unwrap();
    let position = choose|position: int| #![trigger group_keys(bucket.rows@)[position]]
        0 <= position < bucket.rows@.len() && group_keys(bucket.rows@)[position] == (second, year);
    assert(bucket.rows@[position].other == second);
    assert(cell_support(bucket.rows@, second));
}

pub fn mark_nations(old_groups: &Layers<Groups>, new_groups: &Layers<Groups>,
    old_incident: &Layers<Layers<i128>>, new_incident: &Layers<Layers<i128>>,
    deltas: &Vec<NationDelta>, affected: &mut Sparse<Groups>)
    requires old(affected).valid(), sparse_groups_shape(*old(affected)),
        forall|key: i64| #![trigger old_groups.at(key)] old_groups.at(key) matches Some(bucket) ==> groups_valid(bucket),
        forall|key: i64| #![trigger new_groups.at(key)] new_groups.at(key) matches Some(bucket) ==> groups_valid(bucket),
        forall|other: i64, key: i64| #![trigger member_value(*old_incident, other, key)]
            member_value(*old_incident, other, key) == group_has(*old_groups, key, other),
        forall|other: i64, key: i64| #![trigger member_value(*new_incident, other, key)]
            member_value(*new_incident, other, key) == group_has(*new_groups, key, other),
    ensures final(affected).valid(), sparse_groups_shape(*final(affected)),
        marks_preserved(*old(affected), *final(affected)),
        nation_marks(*old_groups, *new_groups, deltas@, *final(affected)),
{
    let ghost initial = *affected;
    let mut index = 0;
    while index < deltas.len()
        invariant index <= deltas@.len(), affected.valid(), sparse_groups_shape(*affected),
            group_shape(*old_groups), group_shape(*new_groups),
            forall|key: i64| #![trigger old_groups.at(key)] old_groups.at(key) matches Some(bucket) ==> groups_valid(bucket),
            forall|key: i64| #![trigger new_groups.at(key)] new_groups.at(key) matches Some(bucket) ==> groups_valid(bucket),
            forall|other: i64, key: i64| #![trigger member_value(*old_incident, other, key)]
                member_value(*old_incident, other, key) == group_has(*old_groups, key, other),
            forall|other: i64, key: i64| #![trigger member_value(*new_incident, other, key)]
                member_value(*new_incident, other, key) == group_has(*new_groups, key, other),
            marks_preserved(initial, *affected), initial == *old(affected),
            forall|position: int, first: i64, second: i64, year: i64|
                #![trigger deltas@[position], cell_marked(*affected, first, second, year)]
                0 <= position < index && (first == deltas@[position].row.n_nationkey || second == deltas@[position].row.n_nationkey) &&
                (group_present(*old_groups, first, second, year) || group_present(*new_groups, first, second, year)) ==>
                    cell_marked(*affected, first, second, year),
        decreases deltas@.len() - index,
    {
        let key = deltas[index].row.n_nationkey;
        mark_bucket(old_groups, affected, key, None);
        mark_second(old_groups, new_groups, old_incident, affected, key);
        mark_second(old_groups, new_groups, new_incident, affected, key);
        mark_bucket(new_groups, affected, key, None);
        proof {
            assert forall|first: i64, second: i64, year: i64| #![trigger cell_marked(*affected, first, second, year)]
                (first == key || second == key) &&
                (group_present(*old_groups, first, second, year) || group_present(*new_groups, first, second, year)) implies
                    cell_marked(*affected, first, second, year) by {
                if second == key {
                    if group_present(*old_groups, first, second, year) {
                        lemma_present_incident(*old_groups, *old_incident, first, second, year);
                    } else {
                        lemma_present_incident(*new_groups, *new_incident, first, second, year);
                    }
                }
            }
        }
        index += 1;
    }
    proof {
        assert forall|first: i64, second: i64, year: i64| #![trigger cell_marked(*affected, first, second, year)]
            (nation_touched(deltas@, first) || nation_touched(deltas@, second)) &&
            (group_present(*old_groups, first, second, year) || group_present(*new_groups, first, second, year)) implies
                cell_marked(*affected, first, second, year) by {
            let key = if nation_touched(deltas@, first) { first } else { second };
            let position = choose|position: int| #![trigger deltas@[position]]
                0 <= position < deltas@.len() && deltas@[position].row.n_nationkey == key;
        }
    }
}

pub open spec fn named_group(nations: Layers<Names>, groups: Layers<Groups>,
    first_key: i64, second_key: i64, cell_year: i64,
    first: Seq<u8>, second: Seq<u8>, year: i64, revenue: bool) -> int {
    if cell_year == year {
        nation_value(nations, first_key, first) *
            (nation_value(nations, second_key, second) * group_value(groups, first_key, second_key, year, revenue))
    } else { 0int }
}

pub open spec fn change_weight(old_nations: Layers<Names>, new_nations: Layers<Names>,
    old_groups: Layers<Groups>, new_groups: Layers<Groups>, first_key: i64,
    first: Seq<u8>, second: Seq<u8>, year: i64, revenue: bool) -> spec_fn(Cell) -> int {
    |cell: Cell| named_group(new_nations, new_groups, first_key, cell.other, cell.year, first, second, year, revenue)
        - named_group(old_nations, old_groups, first_key, cell.other, cell.year, first, second, year, revenue)
}

pub open spec fn bucket_change_weight(old_nations: Layers<Names>, new_nations: Layers<Names>,
    old_groups: Layers<Groups>, new_groups: Layers<Groups>,
    first: Seq<u8>, second: Seq<u8>, year: i64, revenue: bool) -> spec_fn((i64, Arc<Groups>)) -> int {
    |entry: (i64, Arc<Groups>)| sum(entry.1.rows@,
        change_weight(old_nations, new_nations, old_groups, new_groups, entry.0, first, second, year, revenue))
}

pub open spec fn answer_change(rows: Seq<(i64, Arc<Groups>)>, old_nations: Layers<Names>, new_nations: Layers<Names>,
    old_groups: Layers<Groups>, new_groups: Layers<Groups>,
    first: Seq<u8>, second: Seq<u8>, year: i64, revenue: bool) -> int {
    sum(rows, bucket_change_weight(old_nations, new_nations, old_groups, new_groups, first, second, year, revenue))
}

pub fn update_answers(old_nations: &Layers<Names>, new_nations: &Layers<Names>,
    old_groups: &Layers<Groups>, new_groups: &Layers<Groups>, previous: &Layers<Answers>, affected: &Sparse<Groups>)
    -> (answers: Layers<Answers>)
    requires affected.valid(), sparse_groups_shape(*affected), group_shape(*old_groups), group_shape(*new_groups),
        forall|key: i64| #![trigger previous.at(key)] previous.at(key) matches Some(bucket) ==> answers_valid(bucket, key),
        forall|first: Seq<u8>, second: Seq<u8>, year: i64|
            #![trigger answer_change(affected.view(), *old_nations, *new_nations, *old_groups, *new_groups, first, second, year, false)]
            answer_value(*previous, first, second, year, false)
                + answer_change(affected.view(), *old_nations, *new_nations, *old_groups, *new_groups, first, second, year, false) >= 0
            && (answer_value(*previous, first, second, year, false)
                + answer_change(affected.view(), *old_nations, *new_nations, *old_groups, *new_groups, first, second, year, false) == 0 ==>
                answer_value(*previous, first, second, year, true)
                    + answer_change(affected.view(), *old_nations, *new_nations, *old_groups, *new_groups, first, second, year, true) == 0),
    ensures {
        &&& answers.valid()
        &&& forall|key: i64| #![trigger answers.at(key)] answers.at(key) matches Some(bucket) ==> answers_valid(bucket, key)
        &&& forall|first: Seq<u8>, second: Seq<u8>, year: i64, revenue: bool|
            #![trigger answer_value(answers, first, second, year, revenue)]
            answer_value(answers, first, second, year, revenue) == answer_value(*previous, first, second, year, revenue)
                + answer_change(affected.view(), *old_nations, *new_nations, *old_groups, *new_groups, first, second, year, revenue)
    },
{
    let mut changes: Sparse<Answers> = Sparse::new();
    proof { affected.row_facts(); }
    let mut index = 0;
    while index < affected.rows().len()
        invariant index <= affected.view().len(), affected.valid(), sparse_groups_shape(*affected), changes.valid(),
            group_shape(*old_groups), group_shape(*new_groups), patched_answers_shape(*previous, changes),
            forall|key: i64| #![trigger previous.at(key)] previous.at(key) matches Some(bucket) ==> answers_valid(bucket, key),
            forall|position: int| #![trigger affected.view()[position]] 0 <= position < affected.view().len() ==>
                affected.at(affected.view()[position].0) == Some(*affected.view()[position].1),
            forall|first: Seq<u8>, second: Seq<u8>, year: i64, revenue: bool|
                #![trigger patched_answer(*previous, changes, first, second, year, revenue)]
                patched_answer(*previous, changes, first, second, year, revenue) == answer_value(*previous, first, second, year, revenue)
                    + answer_change(affected.view().take(index as int), *old_nations, *new_nations, *old_groups, *new_groups, first, second, year, revenue),
        decreases affected.view().len() - index,
    {
        let first_key = affected.rows()[index].0;
        let bucket = &*affected.rows()[index].1;
        let mut offset = 0;
        while offset < bucket.rows.len()
            invariant offset <= bucket.rows@.len(), changes.valid(), index < affected.view().len(),
                first_key == affected.view()[index as int].0, *bucket == *affected.view()[index as int].1,
                groups_shape(*bucket), group_shape(*old_groups), group_shape(*new_groups), patched_answers_shape(*previous, changes),
                forall|first: Seq<u8>, second: Seq<u8>, year: i64, revenue: bool|
                    #![trigger patched_answer(*previous, changes, first, second, year, revenue)]
                    patched_answer(*previous, changes, first, second, year, revenue) == answer_value(*previous, first, second, year, revenue)
                        + answer_change(affected.view().take(index as int), *old_nations, *new_nations, *old_groups, *new_groups, first, second, year, revenue)
                        + sum(bucket.rows@.take(offset as int), change_weight(*old_nations, *new_nations, *old_groups, *new_groups,
                            first_key, first, second, year, revenue)),
            decreases bucket.rows@.len() - offset,
        {
            let ghost before = changes;
            let key = bucket.rows[offset].dup();
            let old_cell = find_cell(old_groups, first_key, key.other, key.year);
            let new_cell = find_cell(new_groups, first_key, key.other, key.year);
            expand_cell(old_nations, previous, &mut changes, first_key, &old_cell, &Wide::from_i128(-1));
            expand_cell(new_nations, previous, &mut changes, first_key, &new_cell, &Wide::from_i128(1));
            proof {
                assert forall|first: Seq<u8>, second: Seq<u8>, year: i64, revenue: bool|
                    #![trigger patched_answer(*previous, changes, first, second, year, revenue)]
                    patched_answer(*previous, changes, first, second, year, revenue) == answer_value(*previous, first, second, year, revenue)
                        + answer_change(affected.view().take(index as int), *old_nations, *new_nations, *old_groups, *new_groups, first, second, year, revenue)
                        + sum(bucket.rows@.take(offset as int + 1), change_weight(*old_nations, *new_nations, *old_groups, *new_groups,
                            first_key, first, second, year, revenue)) by {
                    let old_value = named_group(*old_nations, *old_groups, first_key, key.other, key.year, first, second, year, revenue);
                    let left_count = nation_value(*old_nations, first_key, first);
                    let right_count = nation_value(*old_nations, key.other, second);
                    let measure = if revenue { old_cell.revenue@ } else { old_cell.count@ };
                    assert(measure == group_value(*old_groups, first_key, key.other, key.year, revenue));
                    assert(left_count * (right_count * (-measure)) == -(left_count * (right_count * measure))) by (nonlinear_arith);
                    assert(cell_expansion(*old_nations, first_key, old_cell, -1int, first, second, year, revenue) == -old_value);
                    lemma_sum_take(bucket.rows@, offset as int,
                        change_weight(*old_nations, *new_nations, *old_groups, *new_groups, first_key, first, second, year, revenue));
                }
            }
            offset += 1;
        }
        proof {
            assert(bucket.rows@.take(bucket.rows@.len() as int) == bucket.rows@);
            assert forall|first: Seq<u8>, second: Seq<u8>, year: i64, revenue: bool|
                #![trigger patched_answer(*previous, changes, first, second, year, revenue)]
                patched_answer(*previous, changes, first, second, year, revenue) == answer_value(*previous, first, second, year, revenue)
                    + answer_change(affected.view().take(index as int + 1), *old_nations, *new_nations, *old_groups, *new_groups, first, second, year, revenue) by {
                lemma_sum_take(affected.view(), index as int,
                    bucket_change_weight(*old_nations, *new_nations, *old_groups, *new_groups, first, second, year, revenue));
            }
        }
        index += 1;
    }
    let patched = previous.patch(changes);
    let answers = patched;
    proof {
        assert(affected.view().take(affected.view().len() as int) == affected.view());
        assert forall|first: Seq<u8>, second: Seq<u8>, year: i64, revenue: bool|
            #![trigger answer_value(answers, first, second, year, revenue)]
            answer_value(answers, first, second, year, revenue) == answer_value(*previous, first, second, year, revenue)
                + answer_change(affected.view(), *old_nations, *new_nations, *old_groups, *new_groups, first, second, year, revenue) by {
            lemma_answer_install(*previous, changes, answers, first, second, year, revenue);
        }
        assert forall|key: i64| #![trigger answers.at(key)] answers.at(key) matches Some(bucket) ==> answers_valid(bucket, key) by {
            if changes.at(key).is_some() {
                let target = |first: Seq<u8>, second: Seq<u8>, year: i64, revenue: bool| answer_value(*previous, first, second, year, revenue)
                    + answer_change(affected.view(), *old_nations, *new_nations, *old_groups, *new_groups, first, second, year, revenue);
                assert forall|first: Seq<u8>, second: Seq<u8>, year: i64, revenue: bool|
                    #![trigger patched_answer(*previous, changes, first, second, year, revenue)]
                    patched_answer(*previous, changes, first, second, year, revenue) == target(first, second, year, revenue) by {}
                lemma_answers_positive(*previous, changes, key, target);
                assert(changes.at(key) matches Some(bucket) && answers_shape(bucket, key) && answers_positive(bucket));
            } else { assert(answers.at(key) == previous.at(key)); }
        }
    }
    answers
}

}
