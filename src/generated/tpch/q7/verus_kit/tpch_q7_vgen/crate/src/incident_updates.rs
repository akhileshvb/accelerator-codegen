use vstd::prelude::*;
use verus_prooflib::*;
use crate::storage::*;
use crate::values::*;
use crate::values::value;
use crate::model::*;
use crate::joins::*;
use crate::construction::*;
use crate::directories::*;
use std::sync::Arc;

verus! {
broadcast use vstd::seq_lib::group_seq_properties;

pub open spec fn group_has(groups: Layers<Groups>, first: i64, second: i64) -> bool {
    groups.at(first) matches Some(bucket) && cell_support(bucket.rows@, second)
}

pub open spec fn replacement_has(groups: Layers<Groups>, changes: Sparse<Groups>, first: i64, second: i64) -> bool {
    match changes.at(first) { Some(bucket) => cell_support(bucket.rows@, second), None => group_has(groups, first, second) }
}

pub proof fn lemma_group_has(groups: Layers<Groups>, first: i64, second: i64)
    requires groups.at(first) matches Some(bucket) ==> groups_valid(bucket),
    ensures group_has(groups, first, second) == (group_value(groups, first, second, 1995, false) > 0
        || group_value(groups, first, second, 1996, false) > 0),
{
    if let Some(bucket) = groups.at(first) { lemma_group_support(bucket, second); }
}

pub proof fn lemma_replacement_has(previous: Layers<Groups>, changes: Sparse<Groups>, next: Layers<Groups>, first: i64, second: i64)
    requires next.at(first) == if changes.at(first).is_some() { normalize(changes.at(first)) } else { previous.at(first) },
    ensures group_has(next, first, second) == replacement_has(previous, changes, first, second),
{
    if let Some(bucket) = changes.at(first) { if bucket.empty() { assert(!cell_support(bucket.rows@, second)); } }
}

pub fn clear_incident_first(previous: &Layers<Layers<i128>>, changes: &mut Sparse<Layers<i128>>, rows: &Groups, first: i64)
    -> (result: Option<()>)
    requires old(changes).valid(),
        forall|second: i64| #![trigger patched_member(*previous, *old(changes), second, first)]
            patched_member(*previous, *old(changes), second, first) ==> cell_support(rows.rows@, second),
        forall|second: i64| #![trigger old(changes).at(second)] old(changes).at(second) matches Some(bucket) ==> bucket.valid(),
    ensures final(changes).valid(),
        forall|second: i64| #![trigger final(changes).at(second)] final(changes).at(second) matches Some(bucket) ==> bucket.valid(),
        result.is_some() ==> forall|second: i64, key: i64| #![trigger patched_member(*previous, *final(changes), second, key)]
            patched_member(*previous, *final(changes), second, key) ==
                (key != first && patched_member(*previous, *old(changes), second, key)),
{
    let ghost initial = *changes;
    let mut offset = 0;
    while offset < rows.rows.len()
        invariant offset <= rows.rows@.len(), changes.valid(), initial == *old(changes),
            forall|second: i64| #![trigger changes.at(second)] changes.at(second) matches Some(bucket) ==> bucket.valid(),
            forall|second: i64, key: i64| #![trigger patched_member(*previous, *changes, second, key)]
                patched_member(*previous, *changes, second, key) == (patched_member(*previous, initial, second, key)
                    && !(key == first && cell_support(rows.rows@.take(offset as int), second))),
        decreases rows.rows@.len() - offset,
    {
        value!(membership_patch(previous, changes, rows.rows[offset].other, first, false));
        proof {
            assert forall|second: i64| #![trigger cell_support(rows.rows@.take(offset as int + 1), second)]
                cell_support(rows.rows@.take(offset as int + 1), second)
                    == (cell_support(rows.rows@.take(offset as int), second) || rows.rows@[offset as int].other == second) by {
                lemma_cell_support_take(rows.rows@, offset as int, second);
            }
        }
        offset += 1;
    }
    proof { assert(rows.rows@.take(rows.rows@.len() as int) == rows.rows@); }
    Some(())
}

pub fn update_incident(groups: &Layers<Groups>, previous: &Layers<Layers<i128>>, replacements: &Sparse<Groups>)
    -> (result: Option<Layers<Layers<i128>>>)
    requires replacements.valid(), previous.valid(),
        forall|second: i64| #![trigger previous.at(second)] previous.at(second) matches Some(bucket) ==> bucket.valid(),
        forall|second: i64, first: i64| #![trigger member_value(*previous, second, first)]
            member_value(*previous, second, first) == group_has(*groups, first, second),
    ensures result matches Some(next) ==> {
        &&& next.valid()
        &&& forall|second: i64| #![trigger next.at(second)] next.at(second) matches Some(bucket) ==> bucket.valid()
        &&& forall|second: i64, first: i64| #![trigger member_value(next, second, first)]
            member_value(next, second, first) == replacement_has(*groups, *replacements, first, second)
    },
{
    let mut changes: Sparse<Layers<i128>> = Sparse::new();
    proof { replacements.row_facts(); replacements.unique_keys(); }
    let ghost keys = replacements.view().map_values(|entry: (i64, Arc<Groups>)| entry.0);
    let mut index = 0;
    while index < replacements.rows().len()
        invariant index <= replacements.view().len(), replacements.valid(), changes.valid(), keys.no_duplicates(),
            keys == replacements.view().map_values(|entry: (i64, Arc<Groups>)| entry.0),
            forall|position: int| #![trigger replacements.view()[position]] 0 <= position < replacements.view().len() ==>
                replacements.at(replacements.view()[position].0) == Some(*replacements.view()[position].1),
            forall|second: i64| #![trigger changes.at(second)] changes.at(second) matches Some(bucket) ==> bucket.valid(),
            forall|second: i64, first: i64| #![trigger patched_member(*previous, changes, second, first)]
                patched_member(*previous, changes, second, first) == if keys.take(index as int).contains(first) {
                    replacement_has(*groups, *replacements, first, second) } else { group_has(*groups, first, second) },
        decreases replacements.view().len() - index,
    {
        let first = replacements.rows()[index].0;
        let ghost before = changes;
        proof { assert(keys[index as int] == first); lemma_prefix_fresh(keys, index as int); }
        if let Some(bucket) = groups.get(first) {
            proof {
                assert forall|second: i64| #![trigger patched_member(*previous, changes, second, first)]
                    patched_member(*previous, changes, second, first) ==> cell_support(bucket.rows@, second) by {}
            }
            value!(clear_incident_first(previous, &mut changes, bucket, first));
        }
        proof {
            assert forall|second: i64, key: i64| #![trigger patched_member(*previous, changes, second, key)]
                patched_member(*previous, changes, second, key) == (key != first && patched_member(*previous, before, second, key)) by {
                if key == first && groups.at(first).is_none() { assert(!group_has(*groups, first, second)); }
            }
        }
        let bucket = &*replacements.rows()[index].1;
        let mut offset = 0;
        while offset < bucket.rows.len()
            invariant offset <= bucket.rows@.len(), index < replacements.view().len(), changes.valid(),
                first == replacements.view()[index as int].0, *bucket == *replacements.view()[index as int].1,
                replacements.at(first) == Some(*bucket),
                forall|second: i64| #![trigger changes.at(second)] changes.at(second) matches Some(bucket) ==> bucket.valid(),
                forall|second: i64, key: i64| #![trigger patched_member(*previous, before, second, key)]
                    patched_member(*previous, before, second, key) == if keys.take(index as int).contains(key) {
                        replacement_has(*groups, *replacements, key, second) } else { group_has(*groups, key, second) },
                forall|second: i64, key: i64| #![trigger patched_member(*previous, changes, second, key)]
                    patched_member(*previous, changes, second, key) == if key == first {
                        cell_support(bucket.rows@.take(offset as int), second) } else { patched_member(*previous, before, second, key) },
            decreases bucket.rows@.len() - offset,
        {
            value!(membership_patch(previous, &mut changes, bucket.rows[offset].other, first, true));
            proof {
                assert forall|second: i64| #![trigger cell_support(bucket.rows@.take(offset as int + 1), second)]
                    cell_support(bucket.rows@.take(offset as int + 1), second)
                        == (cell_support(bucket.rows@.take(offset as int), second) || bucket.rows@[offset as int].other == second) by {
                    lemma_cell_support_take(bucket.rows@, offset as int, second);
                }
            }
            offset += 1;
        }
        proof {
            assert(bucket.rows@.take(bucket.rows@.len() as int) == bucket.rows@);
            assert(keys.take(index as int + 1) =~= keys.take(index as int).push(first));
            lemma_contains_push(keys.take(index as int), first);
        }
        index += 1;
    }
    let next = value!(previous.patch(changes));
    proof {
        assert(keys.take(keys.len() as int) == keys);
        assert forall|second: i64, first: i64| #![trigger member_value(next, second, first)]
            member_value(next, second, first) == replacement_has(*groups, *replacements, first, second) by {
            lemma_member_install(*previous, changes, next, second, first);
            if replacements.at(first).is_some() {
                let position = choose|position: int| #![trigger replacements.view()[position]]
                    0 <= position < replacements.view().len() && replacements.view()[position].0 == first
                        && replacements.at(first) == Some(*replacements.view()[position].1);
                assert(keys[position] == first); assert(keys.contains(first));
            }
        }
        assert forall|second: i64| #![trigger next.at(second)] next.at(second) matches Some(bucket) ==> bucket.valid() by {
            if changes.at(second).is_none() { assert(next.at(second) == previous.at(second)); }
        }
    }
    Some(next)
}

}
