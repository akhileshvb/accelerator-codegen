use vstd::prelude::*;
use crate::index::*;
use crate::aggregates::*;
use crate::gen_spec::*;
use crate::gen_obligations::{params, out_view, out_fits, answer_fits};
use verus_prooflib::wide::Wide;
use crate::query_proofs::*;
use crate::output_proofs::*;
use verus_common::order::*;
use verus_common::sums::*;
use std::sync::Arc;

verus! {

struct Candidate {
    name: Key,
    count: Wide,
}

impl Candidate {
    spec fn row(self) -> OutRow { OutRow { s_name: self.name.view().1, numwait: self.count@ } }
}

spec fn candidate_rows(rows: Seq<Candidate>) -> Seq<OutRow> {
    rows.map_values(|row: Candidate| row.row())
}

pub open spec fn group_rows(rows: Seq<(KeyView, Wide)>, multiplier: int) -> Seq<OutRow> {
    rows.map_values(|row: (KeyView, Wide)| OutRow { s_name: row.0.1, numwait: row.1@ * multiplier })
}

fn better(left: &Candidate, right: &Candidate) -> (result: bool)
    ensures result == row_lt(left.row(), right.row()), result == ext_order()(left.row(), right.row()),
{
    right.count.lt(&left.count) || (left.count.eq(&right.count) && compare_bytes(&left.name.text, &right.name.text) < 0)
}

fn retain(selected: &mut Vec<Candidate>, name: Key, count: Wide)
    requires old(selected).len() <= 100, count@ > 0,
        strict_order(ext_order()),
        sorted_by(candidate_rows(old(selected)@), ext_order()),
        forall|index: int| 0 <= index < old(selected).len() ==> (#[trigger] old(selected)[index]).count@ > 0,
    ensures final(selected).len() <= 100,
        candidate_rows(final(selected)@) == limit_rows(insert_by(candidate_rows(old(selected)@), OutRow { s_name: name.view().1, numwait: count@ }, ext_order()), 100),
        forall|index: int| 0 <= index < final(selected).len() ==> (#[trigger] final(selected)[index]).count@ > 0,
{
    let ghost previous = selected@;
    let candidate = Candidate { name, count };
    let ghost row = candidate.row();
    proof {
        assert(previous == old(selected)@);
        assert(sorted_by(candidate_rows(previous), ext_order()));
    }
    if selected.len() == 100 && !better(&candidate, &selected[selected.len() - 1]) {
        proof {
            assert forall|index: int| 0 <= index < previous.len() implies !ext_order()(row, #[trigger] candidate_rows(previous)[index]) by {
                if index < previous.len() - 1 {
                    assert(ext_order()(candidate_rows(previous)[index], candidate_rows(previous)[previous.len() - 1]));
                    assert(!ext_order()(row, candidate_rows(previous)[index])) by {
                        if ext_order()(row, candidate_rows(previous)[index]) {
                            assert(ext_order()(row, candidate_rows(previous)[previous.len() - 1]));
                        }
                    }
                }
            }
            lemma_insert_position(candidate_rows(previous), row, previous.len() as int);
            assert(candidate_rows(selected@) =~= candidate_rows(previous));
        }
        return;
    }
    if selected.len() < 100 && (selected.is_empty() || !better(&candidate, &selected[selected.len() - 1])) {
        let position = selected.len();
        proof {
            assert forall|index: int| 0 <= index < position implies !ext_order()(row, #[trigger] candidate_rows(previous)[index]) by {
                if position > 0 {
                    if index < position - 1 {
                        assert(ext_order()(candidate_rows(previous)[index], candidate_rows(previous)[position - 1]));
                    }
                    assert(!ext_order()(row, candidate_rows(previous)[index])) by {
                        if ext_order()(row, candidate_rows(previous)[index]) {
                            assert(ext_order()(row, candidate_rows(previous)[position - 1]));
                        }
                    }
                }
            }
            lemma_insert_position(candidate_rows(previous), row, position as int);
        }
        selected.push(candidate);
        proof {
            assert(candidate_rows(selected@) =~= candidate_rows(previous).take(position as int) + seq![row] + candidate_rows(previous).skip(position as int));
            lemma_insert_length(candidate_rows(previous), row);
            assert(candidate_rows(selected@) =~= limit_rows(insert_by(candidate_rows(previous), row, ext_order()), 100));
        }
        return;
    }
    let mut low = 0usize;
    let mut high = selected.len();
    while low < high
        invariant
            low <= high, high <= selected.len(), selected@ == previous, selected.len() <= 100, row == candidate.row(),
            strict_order(ext_order()), sorted_by(candidate_rows(previous), ext_order()),
            forall|index: int| 0 <= index < low ==> !ext_order()(row, #[trigger] candidate_rows(previous)[index]),
            forall|index: int| high <= index < selected.len() ==> ext_order()(row, #[trigger] candidate_rows(previous)[index]),
        decreases high - low,
    {
        let middle = low + (high - low) / 2;
        proof {
            assert(middle < high);
            assert(selected@[middle as int].row() == candidate_rows(previous)[middle as int]);
        }
        if better(&candidate, &selected[middle]) {
            proof {
                assert forall|index: int| middle <= index < selected.len() implies ext_order()(row, #[trigger] candidate_rows(previous)[index]) by {
                    if index == middle {
                        assert(ext_order()(row, candidate_rows(previous)[middle as int]));
                    } else {
                        assert(ext_order()(candidate_rows(previous)[middle as int], candidate_rows(previous)[index]));
                        assert(ext_order()(row, candidate_rows(previous)[index])) by {
                            if ext_order()(row, candidate_rows(previous)[index]) {
                                assert(ext_order()(row, candidate_rows(previous)[middle as int]));
                            }
                        }
                    }
                }
            }
            high = middle;
        } else {
            proof {
                assert forall|index: int| 0 <= index <= middle implies !ext_order()(row, #[trigger] candidate_rows(previous)[index]) by {
                    if index == middle {
                        assert(!ext_order()(row, candidate_rows(previous)[middle as int]));
                    } else {
                        assert(ext_order()(candidate_rows(previous)[index], candidate_rows(previous)[middle as int]));
                        assert(!ext_order()(row, candidate_rows(previous)[index])) by {
                            if ext_order()(row, candidate_rows(previous)[index]) {
                                assert(ext_order()(row, candidate_rows(previous)[middle as int]));
                            }
                        }
                    }
                }
            }
            low = middle + 1;
        }
    }
    proof { lemma_insert_position(candidate_rows(previous), row, low as int); }
    selected.insert(low, candidate);
    proof {
        assert(candidate_rows(selected@) =~= candidate_rows(previous).take(low as int) + seq![row] + candidate_rows(previous).skip(low as int));
    }
    if selected.len() > 100 { selected.pop(); }
    proof {
        lemma_insert_length(candidate_rows(previous), row);
        assert(candidate_rows(selected@) =~= limit_rows(insert_by(candidate_rows(previous), row, ext_order()), 100));
    }
}

pub open spec fn rows_fit(rows: Seq<OutRow>) -> bool {
    forall|index: int| 0 <= index < rows.len() ==> out_fits(#[trigger] rows[index])
}

fn select(groups: &Counts, multiplier: &Wide) -> (result: Option<Vec<(Vec<u8>, i64)>>)
    requires groups.wf(), positive(*groups), multiplier@ > 0,
        forall|index: int| 0 <= index < groups.entries().len() ==> (#[trigger] groups.entries()[index]).0.0 == 0,
    ensures
        result matches Some(output) ==> out_view(output@) == limit_rows(sort_by(group_rows(groups.entries(), multiplier@), ext_order()), 100),
        result.is_none() ==> !rows_fit(limit_rows(sort_by(group_rows(groups.entries(), multiplier@), ext_order()), 100)),
{
    hide(Tree::at);
    hide(Tree::entries);
    hide(Tree::wf);
    let mut selected: Vec<Candidate> = Vec::new();
    let mut cursor = groups.cursor();
    let ghost all_rows = group_rows(groups.entries(), multiplier@);
    let ghost mut consumed = 0int;
    proof { lemma_row_order(); groups.model(); }
    let mut finished = false;
    while !finished
        invariant
            selected.len() <= 100, 0 <= consumed <= groups.entries().len(), groups.wf(), ordered_rows(groups.entries()),
            positive(*groups), multiplier@ > 0,
            forall|index: int| 0 <= index < groups.entries().len() ==> (#[trigger] groups.entries()[index]).0.0 == 0,
            cursor.remaining() == groups.entries().skip(consumed),
            all_rows == group_rows(groups.entries(), multiplier@),
            candidate_rows(selected@) == limit_rows(sort_by(all_rows.take(consumed), ext_order()), 100),
            forall|index: int| 0 <= index < selected.len() ==> (#[trigger] selected@[index]).count@ > 0,
            strict_order(ext_order()),
            forall|first: OutRow, second: OutRow| first != second ==> #[trigger] ext_order()(first, second) || ext_order()(second, first),
            finished ==> consumed == groups.entries().len(), cursor.work() >= 0,
        decreases if finished { 0int } else { cursor.work() + 1 },
    {
        let (name, value) = match cursor.next() { Some(row) => row, None => { finished = true; continue; } };
        let count = (*value).mul(multiplier);
        proof {
            assert(groups.entries()[consumed].1@ > 0);
            assert(count@ > 0) by(nonlinear_arith) requires count@ == value@ * multiplier@, value@ > 0, multiplier@ > 0;
        }
        let ghost row = OutRow { s_name: name.view().1, numwait: count@ };
        proof {
            assert(row == all_rows[consumed]);
            lemma_sort_by(all_rows.take(consumed), ext_order());
            assert(!all_rows.take(consumed).contains(row)) by {
                assert forall|index: int| 0 <= index < consumed implies all_rows[index] != row by {
                    assert(key_lt(groups.entries()[index].0, groups.entries()[consumed].0));
                    lemma_key_order(groups.entries()[index].0, groups.entries()[consumed].0);
                }
            }
            lemma_limit_insert(sort_by(all_rows.take(consumed), ext_order()), row, 100);
            assert(all_rows.take(consumed + 1) =~= all_rows.take(consumed).push(row));
            lemma_sort_push(all_rows.take(consumed), row);
            assert(sort_by(all_rows.take(consumed + 1), ext_order()) == insert_by(sort_by(all_rows.take(consumed), ext_order()), row, ext_order()));
        }
        retain(&mut selected, name, count);
        proof { consumed = consumed + 1; }
    }
    proof {
        assert(consumed == groups.entries().len());
        assert(all_rows.take(consumed) =~= all_rows);
    }
    let mut output = Vec::new();
    let mut position = 0usize;
    while position < selected.len()
        invariant
            position <= selected.len(), selected.len() <= 100,
            forall|index: int| 0 <= index < selected.len() ==> (#[trigger] selected@[index]).count@ > 0,
            all_rows == group_rows(groups.entries(), multiplier@),
            candidate_rows(selected@) == limit_rows(sort_by(all_rows, ext_order()), 100),
            out_view(output@) == candidate_rows(selected@).take(position as int),
        decreases selected.len() - position,
    {
        let candidate = &selected[position];
        let wide = candidate.count.to_i128();
        let small = match wide { Some(value) if value <= i64::MAX as i128 => value as i64, _ => {
            proof {
                assert(candidate.count@ > i64::MAX);
                assert(candidate_rows(selected@)[position as int] == candidate.row());
                assert(!out_fits(candidate.row()));
                assert(!rows_fit(candidate_rows(selected@)));
            }
            return None;
        } };
        let ghost previous_output = out_view(output@);
        output.push(((*candidate.name.text).clone(), small));
        proof {
            assert(small as int == candidate.count@);
            assert(out_view(output@) =~= previous_output.push(candidate.row()));
            assert(candidate_rows(selected@).take(position as int + 1) =~= candidate_rows(selected@).take(position as int).push(candidate.row()));
        }
        position += 1;
        proof { assert(out_view(output@) =~= candidate_rows(selected@).take(position as int)); }
    }
    proof { assert(candidate_rows(selected@).take(position as int) =~= candidate_rows(selected@)); }
    Some(output)
}

fn merge_groups(target: &mut Counts, source: &Counts, multiplier: &Wide)
    requires positive(*old(target)), positive(*source), multiplier@ > 0,
    ensures positive(*final(target)),
        forall|key: KeyView| #![trigger count(*final(target), key)] count(*final(target), key)
            == count(*old(target), key) + multiplier@ * count(*source, key),
{
    hide(Tree::at);
    hide(Tree::entries);
    hide(Tree::wf);
    let ghost previous = *target;
    let mut cursor = source.cursor();
    let ghost mut consumed = 0int;
    loop
        invariant positive(*target), positive(*source), 0 <= consumed <= source.entries().len(), multiplier@ > 0,
            previous == *old(target),
            cursor.remaining() == source.entries().skip(consumed),
            forall|key: KeyView| #![trigger count(*target, key)] count(*target, key) == count(previous, key)
                + multiplier@ * sum(source.entries().take(consumed), entry_weight(key)),
        decreases cursor.work(),
    {
        let (key, value) = match cursor.next() {
            Some(row) => row,
            None => {
                proof {
                    assert(consumed == source.entries().len());
                    assert(source.entries().take(consumed) =~= source.entries());
                    assert forall|key: KeyView| #![trigger count(*target, key)] count(*target, key) == count(previous, key) + multiplier@ * count(*source, key) by {
                        lemma_count_sum(source, key);
                    }
                }
                return;
            },
        };
        let ghost changed_key = key.view();
        let ghost changed_value = value@;
        proof { assert(source.entries()[consumed] == (changed_key, *value)); source.model(); assert(changed_value > 0); }
        let weighted = (*value).mul(multiplier);
        proof {
            assert(weighted@ == multiplier@ * changed_value) by(nonlinear_arith)
                requires weighted@ == changed_value * multiplier@;
            assert(weighted@ >= 0) by(nonlinear_arith) requires weighted@ == multiplier@ * changed_value, multiplier@ > 0, changed_value > 0;
        }
        add_nonneg(target, key, &weighted);
        proof {
            assert forall|key: KeyView| #![trigger count(*target, key)] count(*target, key) == count(previous, key)
                + multiplier@ * sum(source.entries().take(consumed + 1), entry_weight(key)) by {
                lemma_sum_take(source.entries(), consumed, entry_weight(key));
                let subtotal = sum(source.entries().take(consumed), entry_weight(key));
                let term = entry_weight(key)(source.entries()[consumed]);
                assert(multiplier@ * (subtotal + term) == multiplier@ * subtotal + multiplier@ * term) by(nonlinear_arith);
            }
            consumed = consumed + 1;
        }
    }
}

pub open spec fn merge_weight(buckets: Slices, key: KeyView) -> spec_fn((Key, Arc<Wide>)) -> int {
    |row: (Key, Arc<Wide>)| (*row.1)@ * nested_count(buckets, row.0.view(), key)
}

#[verifier::rlimit(20)]
pub fn answer(nations: &Slices, buckets: &Slices, parameter: &[u8], Ghost(db): Ghost<Db>) -> (result: Option<Vec<(Vec<u8>, i64)>>)
    requires nations_for(*nations, db), buckets_for(*buckets, db),
    ensures result matches Some(out) ==> query_result(db, params(parameter@), out_view(out@)),
        result.is_none() ==> !answer_fits(db, params(parameter@)),
{
    hide(Tree::at);
    hide(Tree::entries);
    hide(Tree::wf);
    let mut parameter_bytes = Vec::new();
    let mut offset = 0usize;
    while offset < parameter.len()
        invariant offset <= parameter.len(), parameter_bytes@ == parameter@.take(offset as int),
        decreases parameter.len() - offset,
    {
        parameter_bytes.push(parameter[offset]);
        offset += 1;
    }
    proof { assert(parameter_bytes@ =~= parameter@); }
    let parameter_key = Key::text(parameter_bytes);
    let matches = match nations.get(&parameter_key) {
        Some(matches) => matches,
        None => {
            let empty: Vec<(Vec<u8>, i64)> = Vec::new();
            proof {
                lemma_missing_nation(nations, db, params(parameter@));
                assert(out_view(empty@) =~= Seq::<OutRow>::empty());
            }
            return Some(empty);
        },
    };
    proof { lemma_matches_for(nations, &*matches, db, params(parameter@)); }
    let rows = matches.rows();
    if rows.len() == 1 {
        let (nation, multiplicity) = &rows[0];
        let groups = slice(buckets, nation);
        proof {
            matches.model();
            assert(positive(*matches));
            lemma_bucket_names(buckets, db, nation.view(), &groups);
            assert(matches.entries() =~= seq![(nation.view(), **multiplicity)]);
            assert(matches.entries()[0].1 == **multiplicity);
            assert((**multiplicity)@ > 0);
            assert forall|name: Seq<u8>| #![trigger group_count(db, params(parameter@), name)]
                group_count(db, params(parameter@), name) == (**multiplicity)@ * count(groups, (0int, name)) by {
                lemma_group_indexed(db, params(parameter@), &*matches, buckets, name);
                lemma_sum_singleton((nation.view(), **multiplicity), |row: (KeyView, Wide)| row.1@ * nested_count(*buckets, row.0, (0int, name)));
                assert(count(groups, (0int, name)) == nested_count(*buckets, nation.view(), (0int, name)));
            }
            lemma_groups_result(db, params(parameter@), &groups, (**multiplicity)@);
        }
        let selected = select(&groups, &**multiplicity);
        proof {
            if selected.is_none() {
                let model = limit_rows(sort_by(group_rows(groups.entries(), (**multiplicity)@), ext_order()), 100);
                assert(query_result(db, params(parameter@), model));
                assert(!answer_fits(db, params(parameter@)));
            }
        }
        return selected;
    }
    let mut merged = Counts::new();
    let mut position = 0usize;
    while position < rows.len()
        invariant position <= rows.len(), positive(merged), matches_for(*matches, db, params(parameter@)), buckets_for(*buckets, db),
            rows@.map_values(|row: (Key, Arc<Wide>)| (row.0.view(), *row.1)) == matches.entries(),
            forall|key: KeyView| #![trigger count(merged, key)] count(merged, key) == sum(rows@.take(position as int), merge_weight(*buckets, key)),
        decreases rows.len() - position,
    {
        let (nation, multiplicity) = &rows[position];
        let groups = slice(buckets, nation);
        proof {
            matches.model();
            assert(matches.entries()[position as int] == (nation.view(), **multiplicity));
            assert((**multiplicity)@ > 0);
        }
        merge_groups(&mut merged, &groups, &**multiplicity);
        proof {
            assert forall|key: KeyView| #![trigger count(merged, key)] count(merged, key) == sum(rows@.take(position as int + 1), merge_weight(*buckets, key)) by {
                lemma_sum_take(rows@, position as int, merge_weight(*buckets, key));
            }
        }
        position += 1;
    }
    proof {
        assert(rows@.take(position as int) =~= rows@);
        merged.model();
        assert forall|index: int| 0 <= index < merged.entries().len() implies (#[trigger] merged.entries()[index]).0.0 == 0 by {
            let key = merged.entries()[index].0;
            if key.0 != 0 {
                lemma_sum_zero(rows@, merge_weight(*buckets, key));
                assert(count(merged, key) == 0);
            }
        }
        assert forall|name: Seq<u8>| #![trigger group_count(db, params(parameter@), name)] group_count(db, params(parameter@), name) == 1int * count(merged, (0int, name)) by {
            lemma_group_indexed(db, params(parameter@), &*matches, buckets, name);
            lemma_sum_map(rows@, |row: (Key, Arc<Wide>)| (row.0.view(), *row.1),
                |row: (KeyView, Wide)| row.1@ * nested_count(*buckets, row.0, (0int, name)), merge_weight(*buckets, (0int, name)));
        }
        lemma_groups_result(db, params(parameter@), &merged, 1int);
    }
    let one = Wide::from_i128(1);
    let selected = select(&merged, &one);
    proof {
        if selected.is_none() {
            let model = limit_rows(sort_by(group_rows(merged.entries(), 1int), ext_order()), 100);
            assert(query_result(db, params(parameter@), model));
            assert(!answer_fits(db, params(parameter@)));
        }
    }
    selected
}

}
