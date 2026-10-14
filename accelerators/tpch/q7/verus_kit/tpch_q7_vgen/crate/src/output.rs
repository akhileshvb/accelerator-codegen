use vstd::prelude::*;
use verus_common::sums::*;
use crate::gen_spec::*;
use crate::gen_obligations::out_view;
use crate::model::*;
use crate::storage::*;
use crate::values::*;
use crate::query_proofs::*;
use crate::gen_obligations::params;
use verus_common::bytes::*;

verus! {
broadcast use vstd::seq_lib::group_seq_properties;

#[verifier::opaque]
pub open spec fn output_cell(answers: Layers<Answers>, first: Seq<u8>, second: Seq<u8>, year: i64) -> Seq<OutRow> {
    if answer_value(answers, first, second, year, false) > 0 {
        seq![OutRow { supp_nation: first, cust_nation: second, l_year: year as int,
            revenue: answer_value(answers, first, second, year, true) }]
    } else { Seq::empty() }
}

#[verifier::opaque]
pub open spec fn pair_output(answers: Layers<Answers>, first: Seq<u8>, second: Seq<u8>) -> Seq<OutRow> {
    output_cell(answers, first, second, 1995) + output_cell(answers, first, second, 1996)
}

pub fn read_answer(answers: &Layers<Answers>, first: &[u8], second: &[u8], year: i64) -> (result: (i128, i128))
    requires forall|key: i64| #![trigger answers.at(key)]
        answers.at(key) matches Some(bucket) ==> answers_valid(bucket, key),
    ensures result.0 as int == answer_value(*answers, first@, second@, year, false),
        result.1 as int == answer_value(*answers, first@, second@, year, true),
{
    let key = pair_hash(first, second);
    if let Some(bucket) = answers.get(key) {
        proof { assert(answers_valid(*bucket, key)); }
        let mut offset = 0;
        while offset < bucket.rows.len()
            invariant offset <= bucket.rows@.len(), answers.at(key) == Some(*bucket),
                key == pair_hash_spec(first@, second@), answers_valid(*bucket, key),
                forall|index: int| #![trigger bucket.rows@[index]] 0 <= index < offset ==>
                    bucket.rows@[index].first@ != first@ || bucket.rows@[index].second@ != second@
                        || bucket.rows@[index].year != year,
            decreases bucket.rows@.len() - offset,
        {
            let row = &bucket.rows[offset];
            if row.year == year && bytes_equal(&row.first, first) && bytes_equal(&row.second, second) {
                proof {
                    assert forall|measure: bool|
                        #![trigger answer_at(bucket.rows@, first@, second@, year, measure)]
                        answer_at(bucket.rows@, first@, second@, year, measure)
                            == if measure { row.revenue as int } else { row.count as int } by {
                        let weight = |entry: Answer| if entry.first@ == first@ && entry.second@ == second@
                            && entry.year == year { if measure { entry.revenue as int } else { entry.count as int } } else { 0int };
                        assert forall|index: int| #![trigger bucket.rows@[index]]
                            0 <= index < bucket.rows@.len() && index != offset implies weight(bucket.rows@[index]) == 0 by {
                            assert(answer_keys(bucket.rows@)[offset as int] == (row.first@, row.second@, row.year));
                            assert(answer_keys(bucket.rows@)[index]
                                == (bucket.rows@[index].first@, bucket.rows@[index].second@, bucket.rows@[index].year));
                            if index < offset {
                                assert((bucket.rows@[index].first@, bucket.rows@[index].second@, bucket.rows@[index].year)
                                    != (row.first@, row.second@, row.year));
                            } else {
                                assert((row.first@, row.second@, row.year)
                                    != (bucket.rows@[index].first@, bucket.rows@[index].second@, bucket.rows@[index].year));
                            }
                        }
                        lemma_sum_single_match(bucket.rows@, weight, offset as int);
                    }
                }
                return (row.count, row.revenue);
            }
            offset += 1;
        }
        proof {
            assert forall|measure: bool| #![trigger answer_at(bucket.rows@, first@, second@, year, measure)]
                answer_at(bucket.rows@, first@, second@, year, measure) == 0 by {
                lemma_sum_zero(bucket.rows@, |entry: Answer| if entry.first@ == first@ && entry.second@ == second@
                    && entry.year == year { if measure { entry.revenue as int } else { entry.count as int } } else { 0int });
            }
        }
        return (0, 0);
    }
    (0, 0)
}

pub fn emit_pair(answers: &Layers<Answers>, first: &[u8], second: &[u8], out: &mut Vec<(Vec<u8>, Vec<u8>, i64, i128)>)
    requires forall|key: i64| #![trigger answers.at(key)]
        answers.at(key) matches Some(bucket) ==> answers_valid(bucket, key),
    ensures out_view(final(out)@) == out_view(old(out)@) + pair_output(*answers, first@, second@),
{
    proof { reveal(output_cell); reveal(pair_output); }
    let ghost before = out_view(out@);
    let earlier = read_answer(answers, first, second, 1995);
    if earlier.0 > 0 { out.push((copy_bytes(first), copy_bytes(second), 1995, earlier.1)); }
    proof { assert(out_view(out@) == before + output_cell(*answers, first@, second@, 1995)); }
    let later = read_answer(answers, first, second, 1996);
    if later.0 > 0 { out.push((copy_bytes(first), copy_bytes(second), 1996, later.1)); }
}

#[verifier::opaque]
pub open spec fn ordered_output(answers: Layers<Answers>, first: Seq<u8>, second: Seq<u8>) -> Seq<OutRow> {
    if bytes_lt(second, first) {
        pair_output(answers, second, first) + pair_output(answers, first, second)
    } else if first == second {
        pair_output(answers, first, second)
    } else {
        pair_output(answers, first, second) + pair_output(answers, second, first)
    }
}

proof fn lemma_pair_output(answers: Layers<Answers>, first: Seq<u8>, second: Seq<u8>)
    ensures
        forall|index: int| #![trigger pair_output(answers, first, second)[index]]
            0 <= index < pair_output(answers, first, second).len() ==> {
                let row = pair_output(answers, first, second)[index];
                &&& row.supp_nation == first && row.cust_nation == second
                &&& row.l_year == 1995 || row.l_year == 1996
                &&& answer_value(answers, first, second, row.l_year as i64, false) > 0
                &&& row.revenue == answer_value(answers, first, second, row.l_year as i64, true)
            },
        forall|left: int, right: int| #![trigger pair_output(answers, first, second)[left], pair_output(answers, first, second)[right]]
            0 <= left < right < pair_output(answers, first, second).len() ==>
                pair_output(answers, first, second)[left].l_year < pair_output(answers, first, second)[right].l_year,
        forall|year: i64| #![trigger answer_value(answers, first, second, year, false)]
            (year == 1995 || year == 1996) && answer_value(answers, first, second, year, false) > 0 ==>
                exists|index: int| 0 <= index < pair_output(answers, first, second).len()
                    && (#[trigger] pair_output(answers, first, second)[index]).l_year == year as int,
{
    reveal(pair_output);
    reveal(output_cell);
    if answer_value(answers, first, second, 1995, false) > 0 {
        if answer_value(answers, first, second, 1996, false) > 0 {} else {}
    } else {
        if answer_value(answers, first, second, 1996, false) > 0 {} else {}
    }
    assert forall|year: i64| #![trigger answer_value(answers, first, second, year, false)]
        (year == 1995 || year == 1996) && answer_value(answers, first, second, year, false) > 0 implies
            exists|index: int| 0 <= index < pair_output(answers, first, second).len()
                && (#[trigger] pair_output(answers, first, second)[index]).l_year == year as int by {
        let position = if year == 1995 || answer_value(answers, first, second, 1995, false) <= 0 { 0int } else { 1int };
        assert(0 <= position < pair_output(answers, first, second).len());
        assert(pair_output(answers, first, second)[position].l_year == year as int);
    }
}

pub proof fn lemma_ordered_output(answers: Layers<Answers>, first: Seq<u8>, second: Seq<u8>)
    ensures
        forall|index: int| #![trigger ordered_output(answers, first, second)[index]]
            0 <= index < ordered_output(answers, first, second).len() ==> {
                let row = ordered_output(answers, first, second)[index];
                &&& selected(params(first, second), row.supp_nation, row.cust_nation)
                &&& row.l_year == 1995 || row.l_year == 1996
                &&& answer_value(answers, row.supp_nation, row.cust_nation, row.l_year as i64, false) > 0
                &&& row.revenue == answer_value(answers, row.supp_nation, row.cust_nation, row.l_year as i64, true)
            },
        forall|left: int, right: int| #![trigger ordered_output(answers, first, second)[left], ordered_output(answers, first, second)[right]]
            0 <= left < right < ordered_output(answers, first, second).len() ==>
                !same_group(ordered_output(answers, first, second)[left], ordered_output(answers, first, second)[right])
                    && !row_lt(ordered_output(answers, first, second)[right], ordered_output(answers, first, second)[left]),
        forall|left: Seq<u8>, right: Seq<u8>, year: i64| #![trigger answer_value(answers, left, right, year, false)]
            selected(params(first, second), left, right) && (year == 1995 || year == 1996)
                && answer_value(answers, left, right, year, false) > 0 ==>
            exists|index: int| 0 <= index < ordered_output(answers, first, second).len()
                && (#[trigger] ordered_output(answers, first, second)[index]).supp_nation == left
                && ordered_output(answers, first, second)[index].cust_nation == right
                && ordered_output(answers, first, second)[index].l_year == year as int,
{
    hide(answer_value);
    hide(bytes_lt);
    let out = ordered_output(answers, first, second);
    let p = params(first, second);
    assert forall|index: int| #![trigger out[index]] 0 <= index < out.len() implies {
        let row = out[index];
        &&& selected(p, row.supp_nation, row.cust_nation)
        &&& row.l_year == 1995 || row.l_year == 1996
        &&& answer_value(answers, row.supp_nation, row.cust_nation, row.l_year as i64, false) > 0
        &&& row.revenue == answer_value(answers, row.supp_nation, row.cust_nation, row.l_year as i64, true)
    } by {
        lemma_ordered_row(answers, first, second, index);
    }
    assert forall|left: int, right: int| #![trigger out[left], out[right]]
        0 <= left < right < out.len() implies !same_group(out[left], out[right]) && !row_lt(out[right], out[left]) by {
        lemma_ordered_positions(answers, first, second, left, right);
    }
    assert forall|left: Seq<u8>, right: Seq<u8>, year: i64|
        #![trigger answer_value(answers, left, right, year, false)]
        selected(p, left, right) && (year == 1995 || year == 1996)
            && answer_value(answers, left, right, year, false) > 0 implies
            exists|index: int| 0 <= index < out.len()
                && (#[trigger] out[index]).supp_nation == left && out[index].cust_nation == right && out[index].l_year == year as int by {
        lemma_ordered_member(answers, first, second, left, right, year);
    }
}

proof fn lemma_ordered_row(answers: Layers<Answers>, first: Seq<u8>, second: Seq<u8>, index: int)
    requires 0 <= index < ordered_output(answers, first, second).len(),
    ensures {
        let row = ordered_output(answers, first, second)[index];
        &&& selected(params(first, second), row.supp_nation, row.cust_nation)
        &&& row.l_year == 1995 || row.l_year == 1996
        &&& answer_value(answers, row.supp_nation, row.cust_nation, row.l_year as i64, false) > 0
        &&& row.revenue == answer_value(answers, row.supp_nation, row.cust_nation, row.l_year as i64, true)
    },
{
    hide(answer_value);
    hide(bytes_lt);
    reveal(ordered_output);
    lemma_pair_output(answers, first, second);
    lemma_pair_output(answers, second, first);
    let out = ordered_output(answers, first, second);
    if bytes_lt(second, first) {
        let prefix = pair_output(answers, second, first);
        if index < prefix.len() { assert(out[index] == prefix[index]); }
        else { assert(out[index] == pair_output(answers, first, second)[index - prefix.len()]); }
    } else if first != second {
        let prefix = pair_output(answers, first, second);
        if index < prefix.len() { assert(out[index] == prefix[index]); }
        else { assert(out[index] == pair_output(answers, second, first)[index - prefix.len()]); }
    }
}

proof fn lemma_ordered_positions(answers: Layers<Answers>, first: Seq<u8>, second: Seq<u8>, left: int, right: int)
    requires 0 <= left < right < ordered_output(answers, first, second).len(),
    ensures !same_group(ordered_output(answers, first, second)[left], ordered_output(answers, first, second)[right])
        && !row_lt(ordered_output(answers, first, second)[right], ordered_output(answers, first, second)[left]),
{
    hide(answer_value);
    hide(bytes_lt);
    reveal(ordered_output);
    lemma_pair_output(answers, first, second);
    lemma_pair_output(answers, second, first);
    let out = ordered_output(answers, first, second);
    lemma_bytes_lt_irrefl(first);
    lemma_bytes_lt_irrefl(second);
    if first != second { lemma_bytes_lt_total(first, second); }
    lemma_bytes_lt_asym(first, second);
    if bytes_lt(second, first) {
        let prefix = pair_output(answers, second, first);
        if right < prefix.len() {
            assert(out[left] == prefix[left] && out[right] == prefix[right]);
        } else if left >= prefix.len() {
            let suffix = pair_output(answers, first, second);
            assert(out[left] == suffix[left - prefix.len()] && out[right] == suffix[right - prefix.len()]);
        } else {
            assert(out[left].supp_nation == second && out[right].supp_nation == first);
        }
    } else if first != second {
        let prefix = pair_output(answers, first, second);
        if right < prefix.len() {
            assert(out[left] == prefix[left] && out[right] == prefix[right]);
        } else if left >= prefix.len() {
            let suffix = pair_output(answers, second, first);
            assert(out[left] == suffix[left - prefix.len()] && out[right] == suffix[right - prefix.len()]);
        } else {
            assert(out[left].supp_nation == first && out[right].supp_nation == second);
        }
    }
}

proof fn lemma_ordered_member(answers: Layers<Answers>, first: Seq<u8>, second: Seq<u8>, left: Seq<u8>, right: Seq<u8>, year: i64)
    requires selected(params(first, second), left, right), year == 1995 || year == 1996,
        answer_value(answers, left, right, year, false) > 0,
    ensures exists|index: int| 0 <= index < ordered_output(answers, first, second).len()
        && (#[trigger] ordered_output(answers, first, second)[index]).supp_nation == left
        && ordered_output(answers, first, second)[index].cust_nation == right
        && ordered_output(answers, first, second)[index].l_year == year as int,
{
    hide(answer_value);
    hide(bytes_lt);
    reveal(ordered_output);
    lemma_pair_output(answers, left, right);
    let pair = pair_output(answers, left, right);
    let position = choose|position: int| 0 <= position < pair.len() && (#[trigger] pair[position]).l_year == year as int;
    let target = if bytes_lt(second, first) {
        if left == second && right == first { position }
        else { pair_output(answers, second, first).len() as int + position }
    } else {
        if left == first && right == second { position }
        else { pair_output(answers, first, second).len() as int + position }
    };
    let out = ordered_output(answers, first, second);
    assert(0 <= target < out.len());
    assert(out[target] == pair[position]);
}

pub proof fn lemma_cached_query(db: Db, answers: Layers<Answers>, first: Seq<u8>, second: Seq<u8>)
    requires
        forall|left: Seq<u8>, right: Seq<u8>, year: i64|
            #![trigger answer_value(answers, left, right, year, false)]
            answer_value(answers, left, right, year, false) == group_count(db, params(left, right), left, right, year as int),
        forall|left: Seq<u8>, right: Seq<u8>, year: i64|
            #![trigger answer_value(answers, left, right, year, true)]
            answer_value(answers, left, right, year, true) == row_of(db, params(left, right), left, right, year as int).revenue,
    ensures query_result(db, params(first, second), ordered_output(answers, first, second)),
{
    hide(tuples);
    hide(shipping_tuples);
    hide(group_count);
    hide(group_sum);
    hide(row_of);
    hide(answer_value);
    lemma_ordered_output(answers, first, second);
    let out = ordered_output(answers, first, second);
    let p = params(first, second);
    assert forall|index: int| #![trigger out[index]] 0 <= index < out.len() implies row_ok(db, p, out[index]) by {
        let row = out[index];
        lemma_group_parameter(db, p, row.supp_nation, row.cust_nation, row.l_year);
        assert(answer_value(answers, row.supp_nation, row.cust_nation, row.l_year as i64, false) > 0);
        assert(row.revenue == answer_value(answers, row.supp_nation, row.cust_nation, row.l_year as i64, true));
        reveal(row_of);
    }
    assert forall|left: Seq<u8>, right: Seq<u8>, year: int| #![trigger group_count(db, p, left, right, year)]
        group_count(db, p, left, right, year) > 0 && having(db, p, left, right, year) implies
            exists|index: int| 0 <= index < out.len()
                && (#[trigger] out[index]).supp_nation == left && out[index].cust_nation == right && out[index].l_year == year by {
        lemma_group_support(db, p, left, right, year);
        lemma_group_parameter(db, p, left, right, year);
        assert(answer_value(answers, left, right, year as i64, false) > 0);
    }
}

}
