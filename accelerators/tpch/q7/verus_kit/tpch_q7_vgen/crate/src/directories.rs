use vstd::prelude::*;
use std::sync::Arc;
use verus_common::sums::*;
use crate::storage::*;
use crate::values::*;
use crate::values::value;
use crate::model::*;
use crate::joins::*;
use crate::construction::*;

verus! {
broadcast use vstd::seq_lib::group_seq_properties;

pub open spec fn cell_support(rows: Seq<Cell>, other: i64) -> bool {
    exists|offset: int| #![trigger rows[offset]] 0 <= offset < rows.len() && rows[offset].other == other
}

pub open spec fn incident_entries(rows: Seq<(i64, Arc<Groups>)>, first: i64, second: i64) -> bool {
    exists|offset: int| #![trigger rows[offset]] 0 <= offset < rows.len()
        && rows[offset].0 == first && cell_support(rows[offset].1.rows@, second)
}

pub proof fn lemma_cell_support_take(rows: Seq<Cell>, offset: int, other: i64)
    requires 0 <= offset < rows.len(),
    ensures cell_support(rows.take(offset + 1), other)
        == (cell_support(rows.take(offset), other) || rows[offset].other == other),
{
    if cell_support(rows.take(offset + 1), other) {
        let position = choose|position: int| #![trigger rows.take(offset + 1)[position]]
            0 <= position < offset + 1 && rows.take(offset + 1)[position].other == other;
        if position < offset { assert(rows.take(offset)[position].other == other); }
    }
    if cell_support(rows.take(offset), other) {
        let position = choose|position: int| #![trigger rows.take(offset)[position]]
            0 <= position < offset && rows.take(offset)[position].other == other;
        assert(rows.take(offset + 1)[position].other == other);
    }
    if rows[offset].other == other { assert(rows.take(offset + 1)[offset].other == other); }
}

pub proof fn lemma_incident_entries_take(rows: Seq<(i64, Arc<Groups>)>, offset: int, first: i64, second: i64)
    requires 0 <= offset < rows.len(),
    ensures incident_entries(rows.take(offset + 1), first, second)
        == (incident_entries(rows.take(offset), first, second)
            || (rows[offset].0 == first && cell_support(rows[offset].1.rows@, second))),
{
    if incident_entries(rows.take(offset + 1), first, second) {
        let position = choose|position: int| #![trigger rows.take(offset + 1)[position]]
            0 <= position < offset + 1 && rows.take(offset + 1)[position].0 == first
                && cell_support(rows.take(offset + 1)[position].1.rows@, second);
        if position < offset { assert(rows.take(offset)[position] == rows[position]); }
    }
    if incident_entries(rows.take(offset), first, second) {
        let position = choose|position: int| #![trigger rows.take(offset)[position]]
            0 <= position < offset && rows.take(offset)[position].0 == first
                && cell_support(rows.take(offset)[position].1.rows@, second);
        assert(rows.take(offset + 1)[position] == rows[position]);
    }
    assert(rows.take(offset + 1)[offset] == rows[offset]);
}

pub proof fn lemma_group_support(bucket: Groups, other: i64)
    requires groups_valid(bucket),
    ensures cell_support(bucket.rows@, other)
        == (group_at(bucket.rows@, other, 1995, false) > 0 || group_at(bucket.rows@, other, 1996, false) > 0),
{
    let first_weight = group_weight(other, 1995, false);
    let second_weight = group_weight(other, 1996, false);
    if cell_support(bucket.rows@, other) {
        let position = choose|position: int| #![trigger bucket.rows@[position]]
            0 <= position < bucket.rows@.len() && bucket.rows@[position].other == other;
        if bucket.rows@[position].year == 1995 { lemma_sum_pos(bucket.rows@, first_weight, position); }
        else { lemma_sum_pos(bucket.rows@, second_weight, position); }
    }
    if group_at(bucket.rows@, other, 1995, false) > 0 {
        let position = lemma_sum_nonzero_witness(bucket.rows@, first_weight);
        assert(bucket.rows@[position].other == other);
    }
    if group_at(bucket.rows@, other, 1996, false) > 0 {
        let position = lemma_sum_nonzero_witness(bucket.rows@, second_weight);
        assert(bucket.rows@[position].other == other);
    }
}

pub proof fn lemma_sparse_incident(rows: Sparse<Groups>, first: i64, second: i64)
    requires rows.valid(), forall|key: i64| #![trigger rows.at(key)]
        rows.at(key) matches Some(bucket) ==> groups_valid(bucket),
    ensures incident_entries(rows.view(), first, second)
        == (sparse_group(rows, first, second, 1995, false) > 0
            || sparse_group(rows, first, second, 1996, false) > 0),
{
    rows.row_facts();
    if let Some(bucket) = rows.at(first) {
        lemma_group_support(bucket, second);
        let position = choose|position: int| #![trigger rows.view()[position]]
            0 <= position < rows.view().len() && rows.view()[position].0 == first
                && rows.at(first) == Some(*rows.view()[position].1);
        if cell_support(bucket.rows@, second) { assert(cell_support(rows.view()[position].1.rows@, second)); }
    }
    if incident_entries(rows.view(), first, second) {
        let position = choose|position: int| #![trigger rows.view()[position]]
            0 <= position < rows.view().len() && rows.view()[position].0 == first
                && cell_support(rows.view()[position].1.rows@, second);
        assert(rows.at(first) == Some(*rows.view()[position].1));
        let ghost bucket = *rows.view()[position].1;
        lemma_group_support(bucket, second);
    }
}

pub fn build_incident(groups: &Sparse<Groups>) -> (result: Option<Layers<Layers<i128>>>)
    requires groups.valid(), forall|key: i64| #![trigger groups.at(key)]
        groups.at(key) matches Some(bucket) ==> groups_valid(bucket),
    ensures result matches Some(directory) ==> {
        &&& directory.valid()
        &&& forall|second: i64, first: i64| #![trigger member_value(directory, second, first)]
            member_value(directory, second, first)
                == (sparse_group(*groups, first, second, 1995, false) > 0
                    || sparse_group(*groups, first, second, 1996, false) > 0)
        &&& forall|key: i64| #![trigger directory.at(key)]
            directory.at(key) matches Some(bucket) ==> bucket.valid()
    },
{
    let empty = Layers::new(Sparse::new());
    let mut changes: Sparse<Layers<i128>> = Sparse::new();
    let mut index = 0;
    while index < groups.rows().len()
        invariant index <= groups.view().len(), changes.valid(),
            forall|key: i64| #![trigger empty.at(key)] empty.at(key).is_none(),
            forall|key: i64| #![trigger changes.at(key)] changes.at(key) matches Some(bucket) ==> bucket.valid(),
            forall|second: i64, first: i64| #![trigger patched_member(empty, changes, second, first)]
                patched_member(empty, changes, second, first)
                    == incident_entries(groups.view().take(index as int), first, second),
        decreases groups.view().len() - index,
    {
        let first = groups.rows()[index].0;
        let bucket = &*groups.rows()[index].1;
        let mut offset = 0;
        while offset < bucket.rows.len()
            invariant offset <= bucket.rows@.len(), changes.valid(), index < groups.view().len(),
                first == groups.view()[index as int].0, *bucket == *groups.view()[index as int].1,
                forall|key: i64| #![trigger empty.at(key)] empty.at(key).is_none(),
                forall|key: i64| #![trigger changes.at(key)] changes.at(key) matches Some(bucket) ==> bucket.valid(),
                forall|second: i64, target: i64| #![trigger patched_member(empty, changes, second, target)]
                    patched_member(empty, changes, second, target)
                        == (incident_entries(groups.view().take(index as int), target, second)
                            || (first == target && cell_support(bucket.rows@.take(offset as int), second))),
            decreases bucket.rows@.len() - offset,
        {
            let ghost before = changes;
            value!(membership_patch(&empty, &mut changes, bucket.rows[offset].other, first, true));
            proof {
                assert forall|second: i64, target: i64| #![trigger patched_member(empty, changes, second, target)]
                    patched_member(empty, changes, second, target)
                        == (incident_entries(groups.view().take(index as int), target, second)
                            || (first == target && cell_support(bucket.rows@.take(offset as int + 1), second))) by {
                    lemma_cell_support_take(bucket.rows@, offset as int, second);
                    assert(patched_member(empty, changes, second, target)
                        == if second == bucket.rows@[offset as int].other && target == first { true }
                            else { patched_member(empty, before, second, target) });
                }
            }
            offset += 1;
        }
        proof {
            assert(bucket.rows@.take(bucket.rows@.len() as int) == bucket.rows@);
            assert forall|second: i64, target: i64| #![trigger patched_member(empty, changes, second, target)]
                patched_member(empty, changes, second, target)
                    == incident_entries(groups.view().take(index as int + 1), target, second) by {
                lemma_incident_entries_take(groups.view(), index as int, target, second);
            }
        }
        index += 1;
    }
    let directory = Layers::new(changes);
    proof {
        assert(groups.view().take(groups.view().len() as int) == groups.view());
        assert forall|second: i64, first: i64| #![trigger member_value(directory, second, first)]
            member_value(directory, second, first)
                == (sparse_group(*groups, first, second, 1995, false) > 0
                    || sparse_group(*groups, first, second, 1996, false) > 0) by {
            lemma_reverse_install(changes, directory, second, first);
            assert(sparse_member(changes, second, first) == patched_member(empty, changes, second, first));
            lemma_sparse_incident(*groups, first, second);
        }
    }
    Some(directory)
}

}
