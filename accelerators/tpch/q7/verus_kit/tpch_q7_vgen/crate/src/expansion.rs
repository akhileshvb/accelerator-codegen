use vstd::prelude::*;
use verus_prooflib::wide::Wide;
use std::sync::Arc;
use verus_common::sums::*;
use crate::storage::*;
use crate::values::*;
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

pub fn build_answers(groups: &Sparse<Groups>, nations: &Layers<Names>) -> (answers: Layers<Answers>)
    requires groups.valid(), forall|key: i64| #![trigger groups.at(key)]
        groups.at(key) matches Some(bucket) ==> groups_valid(bucket),
        forall|key: i64| #![trigger nations.at(key)] nations.at(key) matches Some(bucket) ==> names_positive(bucket),
    ensures {
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
            let cell = bucket.rows[offset].dup();
            expand_cell(nations, &empty, &mut changes, first_key, &cell, &Wide::from_i128(1));
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
    let built = Layers::new(changes);
    let answers = built;
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
                let target = |first: Seq<u8>, second: Seq<u8>, year: i64, revenue: bool| expansion(groups.view(), *nations, first, second, year, revenue);
                assert forall|first: Seq<u8>, second: Seq<u8>, year: i64| #![trigger target(first, second, year, false)]
                    target(first, second, year, false) >= 0 && (target(first, second, year, false) == 0 ==> target(first, second, year, true) == 0) by {
                    lemma_expansion_support(*groups, *nations, first, second, year);
                }
                lemma_answers_positive(empty, changes, key, target);
                assert(answers_positive(bucket));
            }
        }
    }
    answers
}


pub proof fn lemma_nation_value_nonneg(nations: Layers<Names>, key: i64, name: Seq<u8>)
    requires forall|other: i64| #![trigger nations.at(other)] nations.at(other) matches Some(bucket) ==> names_positive(bucket),
    ensures nation_value(nations, key, name) >= 0,
{
    if let Some(bucket) = nations.at(key) {
        assert(names_positive(bucket));
        lemma_name_at_nonneg(bucket.rows@, name);
    }
}

proof fn lemma_cell_expansion_support(nations: Layers<Names>, key: i64, cell: Cell, first: Seq<u8>, second: Seq<u8>, year: i64)
    requires cell.count@ > 0,
        forall|other: i64| #![trigger nations.at(other)] nations.at(other) matches Some(bucket) ==> names_positive(bucket),
    ensures cell_expansion(nations, key, cell, 1int, first, second, year, false) >= 0,
        cell_expansion(nations, key, cell, 1int, first, second, year, false) == 0 ==>
            cell_expansion(nations, key, cell, 1int, first, second, year, true) == 0,
{
    lemma_nation_value_nonneg(nations, key, first);
    lemma_nation_value_nonneg(nations, cell.other, second);
    let first_count = nation_value(nations, key, first);
    let second_count = nation_value(nations, cell.other, second);
    if cell.year == year {
        assert(first_count * (second_count * (1int * cell.count@)) >= 0) by (nonlinear_arith)
            requires first_count >= 0, second_count >= 0, cell.count@ > 0;
        if first_count * (second_count * (1int * cell.count@)) == 0 {
            assert(first_count == 0 || second_count == 0) by (nonlinear_arith)
                requires first_count >= 0, second_count >= 0, cell.count@ > 0, first_count * (second_count * (1int * cell.count@)) == 0;
            assert(first_count * (second_count * (1int * cell.revenue@)) == 0) by (nonlinear_arith)
                requires first_count == 0 || second_count == 0;
        }
    }
}

pub proof fn lemma_expansion_support(groups: Sparse<Groups>, nations: Layers<Names>, first: Seq<u8>, second: Seq<u8>, year: i64)
    requires groups.valid(), forall|key: i64| #![trigger groups.at(key)] groups.at(key) matches Some(bucket) ==> groups_valid(bucket),
        forall|other: i64| #![trigger nations.at(other)] nations.at(other) matches Some(bucket) ==> names_positive(bucket),
    ensures expansion(groups.view(), nations, first, second, year, false) >= 0,
        expansion(groups.view(), nations, first, second, year, false) == 0 ==>
            expansion(groups.view(), nations, first, second, year, true) == 0,
{
    groups.row_facts();
    let counts = bucket_expansion_weight(nations, first, second, year, false);
    let revenues = bucket_expansion_weight(nations, first, second, year, true);
    assert forall|index: int| 0 <= index < groups.view().len() implies
        counts(#[trigger] groups.view()[index]) >= 0
        && (counts(groups.view()[index]) == 0 ==> revenues(groups.view()[index]) == 0) by {
        let entry = groups.view()[index];
        let bucket = *entry.1;
        assert(groups.at(entry.0) == Some(bucket));
        assert(groups_valid(bucket));
        let cell_counts = expansion_weight(nations, entry.0, first, second, year, false);
        let cell_revenues = expansion_weight(nations, entry.0, first, second, year, true);
        assert forall|offset: int| 0 <= offset < bucket.rows@.len() implies
            cell_counts(#[trigger] bucket.rows@[offset]) >= 0
            && (cell_counts(bucket.rows@[offset]) == 0 ==> cell_revenues(bucket.rows@[offset]) == 0) by {
            assert(bucket.rows@[offset].count@ > 0);
            lemma_cell_expansion_support(nations, entry.0, bucket.rows@[offset], first, second, year);
        }
        lemma_sum_nonneg(bucket.rows@, cell_counts);
        if sum(bucket.rows@, cell_counts) == 0 {
            assert forall|offset: int| 0 <= offset < bucket.rows@.len() implies cell_revenues(#[trigger] bucket.rows@[offset]) == 0 by {
                if cell_counts(bucket.rows@[offset]) > 0 { lemma_sum_pos(bucket.rows@, cell_counts, offset); }
            }
            lemma_sum_zero(bucket.rows@, cell_revenues);
        }
    }
    lemma_sum_nonneg(groups.view(), counts);
    if sum(groups.view(), counts) == 0 {
        assert forall|index: int| 0 <= index < groups.view().len() implies revenues(#[trigger] groups.view()[index]) == 0 by {
            if counts(groups.view()[index]) > 0 { lemma_sum_pos(groups.view(), counts, index); }
        }
        lemma_sum_zero(groups.view(), revenues);
    }
}


pub proof fn lemma_answers_positive(previous: Layers<Answers>, changes: Sparse<Answers>, key: i64,
    target: spec_fn(Seq<u8>, Seq<u8>, i64, bool) -> int)
    requires changes.at(key) matches Some(bucket) ==> answers_shape(bucket, key),
        forall|first: Seq<u8>, second: Seq<u8>, year: i64, revenue: bool|
            #![trigger patched_answer(previous, changes, first, second, year, revenue)]
            patched_answer(previous, changes, first, second, year, revenue) == target(first, second, year, revenue),
        forall|first: Seq<u8>, second: Seq<u8>, year: i64| #![trigger target(first, second, year, false)]
            target(first, second, year, false) >= 0 && (target(first, second, year, false) == 0 ==> target(first, second, year, true) == 0),
    ensures changes.at(key) matches Some(bucket) ==> answers_positive(bucket),
{
    if let Some(bucket) = changes.at(key) {
        assert forall|offset: int| 0 <= offset < bucket.rows@.len() implies (#[trigger] bucket.rows@[offset]).count@ > 0 by {
            let row = bucket.rows@[offset];
            lemma_answer_entry(bucket.rows@, offset, false);
            lemma_answer_entry(bucket.rows@, offset, true);
            assert(pair_hash_spec(row.first@, row.second@) == key);
            assert(patched_answer(previous, changes, row.first@, row.second@, row.year, false) == answer_at(bucket.rows@, row.first@, row.second@, row.year, false));
            assert(patched_answer(previous, changes, row.first@, row.second@, row.year, true) == answer_at(bucket.rows@, row.first@, row.second@, row.year, true));
            assert(target(row.first@, row.second@, row.year, false) >= 0);
        }
    }
}

}
