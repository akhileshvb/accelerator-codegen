use vstd::prelude::*;
use std::sync::Arc;
use verus_common::sums::*;
use crate::storage::*;
use crate::values::*;
use crate::values::value;
use crate::model::*;
use crate::joins::*;

verus! {
broadcast use vstd::seq_lib::group_seq_properties;

pub open spec fn expansion_weight(nations: Layers<Names>, key: i64, first: Seq<u8>, second: Seq<u8>, year: i64, revenue: bool)
    -> spec_fn(Cell) -> int
{
    |cell: Cell| cell_expansion(nations, key, cell, 1int, first, second, year, revenue)
}

pub open spec fn bucket_expansion_weight(nations: Layers<Names>, first: Seq<u8>, second: Seq<u8>, year: i64, revenue: bool)
    -> spec_fn((i64, Arc<Groups>)) -> int
{
    |entry: (i64, Arc<Groups>)| sum(entry.1.rows@, expansion_weight(nations, entry.0, first, second, year, revenue))
}

pub open spec fn expansion(rows: Seq<(i64, Arc<Groups>)>, nations: Layers<Names>, first: Seq<u8>, second: Seq<u8>, year: i64, revenue: bool) -> int {
    sum(rows, bucket_expansion_weight(nations, first, second, year, revenue))
}

pub proof fn lemma_answer_install(previous: Layers<Answers>, changes: Sparse<Answers>, next: Layers<Answers>,
    first: Seq<u8>, second: Seq<u8>, year: i64, revenue: bool)
    requires next.at(pair_hash_spec(first, second) as i64) ==
        if changes.at(pair_hash_spec(first, second) as i64).is_some() {
            normalize(changes.at(pair_hash_spec(first, second) as i64))
        } else { previous.at(pair_hash_spec(first, second) as i64) },
    ensures answer_value(next, first, second, year, revenue) == patched_answer(previous, changes, first, second, year, revenue),
{
    if let Some(bucket) = changes.at(pair_hash_spec(first, second) as i64) {
        if bucket.empty() { lemma_sum_empty(bucket.rows@, answer_weight(first, second, year, revenue)); }
    }
}

pub fn build_answers(groups: &Sparse<Groups>, nations: &Layers<Names>) -> (result: Option<Layers<Answers>>)
    requires groups.valid(), forall|key: i64| #![trigger groups.at(key)]
        groups.at(key) matches Some(bucket) ==> groups_valid(bucket),
    ensures result matches Some(answers) ==> {
        &&& answers.valid()
        &&& forall|first: Seq<u8>, second: Seq<u8>, year: i64, revenue: bool|
            #![trigger answer_value(answers, first, second, year, revenue)]
            answer_value(answers, first, second, year, revenue)
                == expansion(groups.view(), *nations, first, second, year, revenue)
        &&& forall|key: i64| #![trigger answers.at(key)]
            answers.at(key) matches Some(bucket) ==> answers_valid(bucket, key)
    },
{
    let empty = Layers::new(Sparse::new());
    let mut changes: Sparse<Answers> = Sparse::new();
    proof { groups.row_facts(); }
    let mut index = 0;
    while index < groups.rows().len()
        invariant index <= groups.view().len(), groups.valid(), changes.valid(),
            forall|key: i64| #![trigger groups.at(key)] groups.at(key) matches Some(bucket) ==> groups_valid(bucket),
            forall|position: int| #![trigger groups.view()[position]] 0 <= position < groups.view().len() ==>
                groups.at(groups.view()[position].0) == Some(*groups.view()[position].1),
            forall|key: i64| #![trigger empty.at(key)] empty.at(key).is_none(),
            patched_answers_shape(empty, changes),
            forall|first: Seq<u8>, second: Seq<u8>, year: i64, revenue: bool|
                #![trigger patched_answer(empty, changes, first, second, year, revenue)]
                patched_answer(empty, changes, first, second, year, revenue)
                    == expansion(groups.view().take(index as int), *nations, first, second, year, revenue),
        decreases groups.view().len() - index,
    {
        let first_key = groups.rows()[index].0;
        let bucket = &*groups.rows()[index].1;
        proof { assert(groups_valid(*bucket)); }
        let mut offset = 0;
        while offset < bucket.rows.len()
            invariant offset <= bucket.rows@.len(), changes.valid(), index < groups.view().len(),
                first_key == groups.view()[index as int].0, *bucket == *groups.view()[index as int].1,
                groups_valid(*bucket),
                forall|key: i64| #![trigger empty.at(key)] empty.at(key).is_none(),
                patched_answers_shape(empty, changes),
                forall|first: Seq<u8>, second: Seq<u8>, year: i64, revenue: bool|
                    #![trigger patched_answer(empty, changes, first, second, year, revenue)]
                    patched_answer(empty, changes, first, second, year, revenue)
                        == expansion(groups.view().take(index as int), *nations, first, second, year, revenue)
                            + sum(bucket.rows@.take(offset as int), expansion_weight(*nations, first_key, first, second, year, revenue)),
            decreases bucket.rows@.len() - offset,
        {
            let ghost before = changes;
            let cell = bucket.rows[offset];
            value!(expand_cell(nations, &empty, &mut changes, first_key, cell, 1));
            proof {
                assert forall|first: Seq<u8>, second: Seq<u8>, year: i64, revenue: bool|
                    #![trigger patched_answer(empty, changes, first, second, year, revenue)]
                    patched_answer(empty, changes, first, second, year, revenue)
                        == expansion(groups.view().take(index as int), *nations, first, second, year, revenue)
                            + sum(bucket.rows@.take(offset as int + 1), expansion_weight(*nations, first_key, first, second, year, revenue)) by {
                    assert(patched_answer(empty, changes, first, second, year, revenue)
                        == patched_answer(empty, before, first, second, year, revenue)
                            + cell_expansion(*nations, first_key, cell, 1int, first, second, year, revenue));
                    lemma_sum_take(bucket.rows@, offset as int, expansion_weight(*nations, first_key, first, second, year, revenue));
                }
            }
            offset += 1;
        }
        proof {
            assert(bucket.rows@.take(bucket.rows@.len() as int) == bucket.rows@);
            assert forall|first: Seq<u8>, second: Seq<u8>, year: i64, revenue: bool|
                #![trigger patched_answer(empty, changes, first, second, year, revenue)]
                patched_answer(empty, changes, first, second, year, revenue)
                    == expansion(groups.view().take(index as int + 1), *nations, first, second, year, revenue) by {
                lemma_sum_take(groups.view(), index as int, bucket_expansion_weight(*nations, first, second, year, revenue));
            }
        }
        index += 1;
    }
    if !positive_answers(&changes) { return None; }
    let answers = Layers::new(changes);
    proof {
        assert(groups.view().take(groups.view().len() as int) == groups.view());
        assert forall|first: Seq<u8>, second: Seq<u8>, year: i64, revenue: bool|
            #![trigger answer_value(answers, first, second, year, revenue)]
            answer_value(answers, first, second, year, revenue)
                == expansion(groups.view(), *nations, first, second, year, revenue) by {
            lemma_answer_install(empty, changes, answers, first, second, year, revenue);
        }
        assert forall|key: i64| #![trigger answers.at(key)]
            answers.at(key) matches Some(bucket) ==> answers_valid(bucket, key) by {
            assert(empty.at(key).is_none());
            if let Some(bucket) = changes.at(key) {
                assert(answers_shape(bucket, key));
                assert(answers_positive(bucket));
            }
        }
    }
    Some(answers)
}

}
