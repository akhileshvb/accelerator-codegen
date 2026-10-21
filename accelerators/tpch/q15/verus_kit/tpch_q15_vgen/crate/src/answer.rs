#![verifier::loop_isolation(false)]
// The answer for a window from its per-supplier tally: the maximum revenue, its winners, and their joined rows.
use vstd::prelude::*;
use std::collections::HashMap;
use verus_common::sums::*;
use verus_common::rel::*;
use verus_prooflib::ordered::{Index, Key, KeyView};
use verus_prooflib::wide::Wide;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::model::*;
use crate::window::*;
use crate::query_proofs::*;
use crate::emission::*;
use crate::database_proofs::{lemma_empty_range, lemma_execute_ok};
use crate::result_proofs::*;
use crate::exact::*;

verus! {

broadcast use vstd::std_specs::hash::group_hash_axioms, vstd::seq_lib::group_seq_properties;

pub proof fn lemma_max_step_x(totals: Seq<(i64, int)>, maximum: Option<int>, winners: Seq<i64>, supplier: i64, revenue: int)
    requires crate::exact::max_ok(totals, maximum, winners),
    ensures crate::exact::max_ok(totals.push((supplier, revenue)),
        if maximum is None || revenue > maximum.unwrap() { Some(revenue) } else { maximum },
        if maximum is None || revenue > maximum.unwrap() { seq![supplier] }
        else if maximum == Some(revenue) { winners.push(supplier) } else { winners }),
{
    let after = totals.push((supplier, revenue));
    assert(after.drop_last() =~= totals);
    let larger = maximum is None || revenue > maximum.unwrap();
    let value = if larger { revenue } else { maximum.unwrap() };
    let new_winners = if larger { seq![supplier] } else if maximum == Some(revenue) { winners.push(supplier) } else { winners };
    assert forall|position: int| 0 <= position < after.len() implies (#[trigger] after[position]).1 <= value by {
        if position < totals.len() { assert(after[position] == totals[position]); }
    }
    assert(after[totals.len() as int].1 == revenue);
    if larger { assert(after[totals.len() as int].1 == value); }
    else {
        let witness = choose|position: int| 0 <= position < totals.len() && (#[trigger] totals[position]).1 == value;
        assert(after[witness].1 == value);
    }
    assert forall|other: int| #[trigger] has_winner(new_winners, other) <==>
        exists|position: int| 0 <= position < after.len() && (#[trigger] after[position]).0 as int == other && after[position].1 == value by {
        if larger {
            assert(has_winner(new_winners, other) == (other == supplier as int)) by {
                if other == supplier as int { assert(new_winners[0] as int == other); }
            }
            if exists|position: int| 0 <= position < after.len() && (#[trigger] after[position]).0 as int == other && after[position].1 == value {
                let position = choose|position: int| 0 <= position < after.len() && (#[trigger] after[position]).0 as int == other && after[position].1 == value;
                if position < totals.len() {
                    assert(after[position] == totals[position]);
                    assert(totals[position].1 <= maximum.unwrap());
                    assert(false);
                } else { assert(other == supplier as int); }
            }
            if other == supplier as int { assert(after[totals.len() as int].0 as int == other && after[totals.len() as int].1 == value); }
        } else if maximum == Some(revenue) {
            if has_winner(new_winners, other) {
                let position = choose|position: int| 0 <= position < new_winners.len() && (#[trigger] new_winners[position]) as int == other;
                if position < winners.len() {
                    assert(has_winner(winners, other));
                    let index = choose|index: int| 0 <= index < totals.len() && (#[trigger] totals[index]).0 as int == other && totals[index].1 == value;
                    assert(after[index] == totals[index]);
                } else { assert(other == supplier as int); assert(after[totals.len() as int].0 as int == other); }
            }
            if exists|position: int| 0 <= position < after.len() && (#[trigger] after[position]).0 as int == other && after[position].1 == value {
                let position = choose|position: int| 0 <= position < after.len() && (#[trigger] after[position]).0 as int == other && after[position].1 == value;
                if position < totals.len() {
                    assert(after[position] == totals[position]);
                    assert(has_winner(winners, other));
                    let found = choose|found: int| 0 <= found < winners.len() && (#[trigger] winners[found]) as int == other;
                    assert(new_winners[found] == winners[found]);
                } else { assert(new_winners[winners.len() as int] as int == other); }
            }
        } else {
            if has_winner(winners, other) {
                let index = choose|index: int| 0 <= index < totals.len() && (#[trigger] totals[index]).0 as int == other && totals[index].1 == value;
                assert(after[index] == totals[index]);
            }
            if exists|position: int| 0 <= position < after.len() && (#[trigger] after[position]).0 as int == other && after[position].1 == value {
                let position = choose|position: int| 0 <= position < after.len() && (#[trigger] after[position]).0 as int == other && after[position].1 == value;
                if position < totals.len() { assert(after[position] == totals[position]); }
                else { assert(revenue == value); assert(false); }
            }
        }
    }
}


pub proof fn lemma_group_push_x(totals: Seq<(i64, int)>, pair: (i64, int), supplier: int)
    ensures crate::exact::has_group_x(totals.push(pair), supplier) <==> crate::exact::has_group_x(totals, supplier) || supplier == pair.0 as int,
{
    let after = totals.push(pair);
    if crate::exact::has_group_x(after, supplier) {
        let position = choose|position: int| 0 <= position < after.len() && (#[trigger] after[position]).0 as int == supplier;
        if position < totals.len() { assert(totals[position] == after[position]); assert(crate::exact::has_group_x(totals, supplier)); }
        else { assert(position == totals.len()); }
    }
    if crate::exact::has_group_x(totals, supplier) {
        let position = choose|position: int| 0 <= position < totals.len() && (#[trigger] totals[position]).0 as int == supplier;
        assert(after[position] == totals[position]);
        assert(crate::exact::has_group_x(after, supplier));
    }
    if supplier == pair.0 as int { assert(after[totals.len() as int] == pair); assert(crate::exact::has_group_x(after, supplier)); }
}

pub open spec fn max_view(maximum: Option<Wide>) -> Option<int> {
    match maximum { Some(value) => Some(value@), None => None }
}

/// The suppliers with rows in the window, their exact revenues, and the maximum with its winners.
#[verifier::spinoff_prover]
#[verifier::rlimit(60)]
pub fn scan_totals(cells: &HashMap<i64, Cell>, keys: &Vec<i64>) -> (result: (Option<Wide>, Vec<i64>, Ghost<Seq<(i64, int)>>))
    requires cells@.dom() == keys@.to_set(), keys@.no_duplicates(),
    ensures ({ let (maximum, winners, totals) = result;
        &&& crate::exact::max_ok(totals@, max_view(maximum), winners@)
        &&& winners@.no_duplicates()
        &&& forall|position: int| 0 <= position < totals@.len() ==> {
            let pair = #[trigger] totals@[position];
            keys@.contains(pair.0) && cnt_at(cells@, pair.0) > 0 && pair.1 == rev_at(cells@, pair.0)
        }
        &&& forall|key: i64| #[trigger] crate::exact::has_group_x(totals@, key as int) <==> keys@.contains(key) && cnt_at(cells@, key) > 0 }),
{
    let mut maximum: Option<Wide> = None;
    let mut winners: Vec<i64> = Vec::new();
    let ghost mut totals = Seq::<(i64, int)>::empty();
    let mut index = 0usize;
    proof { keys@.to_set_ensures(); }
    while index < keys.len()
        invariant index <= keys.len(), cells@.dom() == keys@.to_set(), keys@.no_duplicates(),
            crate::exact::max_ok(totals, max_view(maximum), winners@), winners@.no_duplicates(),
            forall|position: int| 0 <= position < totals.len() ==> {
                let pair = #[trigger] totals[position];
                keys@.take(index as int).contains(pair.0) && cnt_at(cells@, pair.0) > 0 && pair.1 == rev_at(cells@, pair.0)
            },
            forall|key: i64| #[trigger] crate::exact::has_group_x(totals, key as int) <==> keys@.take(index as int).contains(key) && cnt_at(cells@, key) > 0,
            forall|position: int| 0 <= position < winners.len() ==> keys@.take(index as int).contains(#[trigger] winners@[position]),
        decreases keys.len() - index,
    {
        let key = keys[index];
        let ghost previous_totals = totals;
        let ghost previous_winners = winners@;
        proof { assert(keys@.contains(key)); assert(cells@.contains_key(key)); }
        let (count, revenue) = match cells.get(&key) { Some(cell) => (cell.0.dup(), cell.1.dup()), None => (Wide::zero(), Wide::zero()) };
        proof {
            assert(count@ == cnt_at(cells@, key));
            assert(revenue@ == rev_at(cells@, key));
            assert(keys@.take(index as int + 1) =~= keys@.take(index as int).push(key));
            verus_prooflib::lemma_push_membership(keys@.take(index as int), key);
        }
        if count.is_pos() {
            proof { lemma_max_step_x(totals, max_view(maximum), winners@, key, revenue@); }
            let larger = match &maximum { Some(value) => value.lt(&revenue), None => true };
            let mut equal = false;
            if larger {
                maximum = Some(revenue.dup());
                winners.clear();
                winners.push(key);
            } else if let Some(value) = &maximum {
                if revenue.eq(value) { winners.push(key); equal = true; }
            }
            proof {
                assert(winners@ == if larger { seq![key] } else if equal { previous_winners.push(key) } else { previous_winners });
                totals = previous_totals.push((key, revenue@));
                assert(!keys@.take(index as int).contains(key)) by {
                    let found = choose|found: int| 0 <= found < index && keys@[found] == key;
                }
            }
        }
        proof {
            assert forall|position: int| 0 <= position < winners.len() implies keys@.take(index as int + 1).contains(#[trigger] winners@[position]) by {
                if count@ > 0 {
                    assert(winners@[position] == key || previous_winners.contains(winners@[position]));
                    if winners@[position] != key { assert(keys@.take(index as int).contains(winners@[position])); }
                } else { assert(winners@ == previous_winners); }
            }
            assert forall|position: int| 0 <= position < totals.len() implies {
                let pair = #[trigger] totals[position];
                keys@.take(index as int + 1).contains(pair.0) && cnt_at(cells@, pair.0) > 0 && pair.1 == rev_at(cells@, pair.0)
            } by {
                if count@ > 0 && position == previous_totals.len() as int { assert(totals[position] == (key, revenue@)); }
                else if count@ > 0 { assert(totals[position] == previous_totals[position]); }
            }
            assert forall|other: i64| #[trigger] crate::exact::has_group_x(totals, other as int) <==> keys@.take(index as int + 1).contains(other) && cnt_at(cells@, other) > 0 by {
                if count@ > 0 {
                    assert(totals =~= previous_totals.push((key, revenue@)));
                    lemma_group_push_x(previous_totals, (key, revenue@), other as int);
                    assert(((other as int) == (key as int)) == (other == key));
                }
            }
        }
        index += 1;
    }
    proof { assert(keys@.take(keys@.len() as int) =~= keys@); }
    (maximum, winners, Ghost(totals))
}


/// The winners in ascending order.
#[verifier::spinoff_prover]
#[verifier::rlimit(40)]
pub fn sort_winners(winners: &Vec<i64>) -> (out: Vec<i64>)
    requires winners@.no_duplicates(),
    ensures forall|left: int, right: int| #![trigger out@[left], out@[right]] 0 <= left < right < out.len() ==> out@[left] < out@[right],
        forall|value: i64| #[trigger] out@.contains(value) <==> winners@.contains(value),
        out@.no_duplicates(),
{
    let mut wide: Vec<i128> = Vec::new();
    let mut index = 0usize;
    while index < winners.len()
        invariant index <= winners.len(), wide@.len() == index,
            forall|position: int| 0 <= position < index ==> wide@[position] == winners@[position] as i128,
        decreases winners.len() - index,
    {
        wide.push(winners[index] as i128);
        index += 1;
    }
    proof {
        assert(wide@.subrange(0, wide@.len() as int) =~= wide@);
        assert forall|left: int, right: int| 0 <= left < right < wide@.len() implies wide@[left] != wide@[right] by {
            assert(winners@[left] != winners@[right]);
        }
    }
    let ordered = crate::sorting::sort_quantities(&wide, 0, wide.len());
    let mut out: Vec<i64> = Vec::new();
    index = 0;
    proof { assert(wide@.subrange(0, wide@.len() as int) =~= wide@); }
    while index < ordered.len()
        invariant index <= ordered.len(), out@.len() == index,
            forall|value: i128| #[trigger] ordered@.contains(value) <==> wide@.contains(value),
            forall|position: int| 0 <= position < wide@.len() ==> i64::MIN <= #[trigger] wide@[position] <= i64::MAX,
            forall|position: int| 0 <= position < index ==> out@[position] as i128 == ordered@[position],
        decreases ordered.len() - index,
    {
        proof { assert(ordered@.contains(ordered@[index as int])); assert(wide@.contains(ordered@[index as int])); }
        out.push(ordered[index] as i64);
        index += 1;
    }
    proof {
        assert forall|left: int, right: int| #![trigger out@[left], out@[right]] 0 <= left < right < out.len() implies out@[left] < out@[right] by {
            assert(ordered@[left] <= ordered@[right]);
            assert(ordered@[left] != ordered@[right]) by { }
        }
        assert forall|value: i64| #[trigger] out@.contains(value) <==> winners@.contains(value) by {
            if out@.contains(value) {
                let position = choose|position: int| 0 <= position < out.len() && out@[position] == value;
                assert(ordered@.contains(ordered@[position]));
                assert(out@[position] as i128 == ordered@[position]);
                assert(ordered@[position] == value as i128);
                assert(wide@.contains(ordered@[position]));
                let found = choose|found: int| 0 <= found < wide@.len() && wide@[found] == value as i128;
                assert(winners@[found] == value);
            }
            if winners@.contains(value) {
                let found = choose|found: int| 0 <= found < winners.len() && winners@[found] == value;
                assert(wide@[found] == winners@[found] as i128);
                assert(wide@.contains(value as i128));
                assert(ordered@.contains(value as i128));
                let position = choose|position: int| 0 <= position < ordered.len() && ordered@[position] == value as i128;
                assert(out@[position] == value);
            }
        }
        assert forall|left: int, right: int| 0 <= left < right < out.len() implies out@[left] != out@[right] by {
            assert(out@[left] < out@[right]);
        }
    }
    out
}



pub fn variants_total(variants: &Index<Variant>) -> (total: Wide)
    requires variants.wf(),
    ensures total@ == sum(variants.entries(), |entry: (KeyView, Variant)| entry.1.count as int),
{
    let mut total = Wide::zero();
    let mut cursor = variants.cursor();
    let ghost entries = variants.entries();
    let ghost mut position = 0int;
    proof {
        assert(entries.take(0) =~= Seq::<(KeyView, Variant)>::empty());
        lemma_sum_empty(entries.take(0), |entry: (KeyView, Variant)| entry.1.count as int);
    }
    loop
        invariant 0 <= position <= entries.len(), cursor.remaining() == entries.skip(position), cursor.work() >= 0, entries == variants.entries(),
            total@ == sum(entries.take(position), |entry: (KeyView, Variant)| entry.1.count as int),
        decreases cursor.work(),
    {
        match cursor.next() {
            None => {
                proof { assert(position == entries.len()); assert(entries.take(position) =~= entries); }
                return total;
            },
            Some((key, variant)) => {
                proof { lemma_sum_take(entries, position, |entry: (KeyView, Variant)| entry.1.count as int); assert(entries[position] == (key.view(), *variant)); }
                total = total.add(&Wide::from_i128(variant.count as i128));
                proof { position = position + 1; }
            },
        }
    }
}

/// How many rows the winners emit in all.
#[verifier::spinoff_prover]
pub fn winners_total_exec(dimensions: &Dimensions, winners: &Vec<i64>) -> (total: Wide)
    requires dimensions_shape(*dimensions),
    ensures total@ == winners_total(*dimensions, winners@),
{
    let known = dimensions.share();
    let mut total = Wide::zero();
    let mut position = 0usize;
    proof {
        assert(winners@.take(0) =~= Seq::<i64>::empty());
        lemma_sum_empty(winners@.take(0), |supplier: i64| supplier_total(*dimensions, supplier as int));
    }
    while position < winners.len()
        invariant position <= winners.len(), dimensions.wf(), dimensions_shape(*dimensions),
            total@ == sum(winners@.take(position as int), |supplier: i64| supplier_total(*dimensions, supplier as int)),
        decreases winners.len() - position,
    {
        let supplier = winners[position];
        proof { lemma_sum_take(winners@, position as int, |supplier: i64| supplier_total(*dimensions, supplier as int)); }
        if let Some(variants) = dimensions.get(&Key::number(supplier)) {
            proof { assert(dimensions.at(number_key(supplier as int)) == Some(*variants)); }
            let part = variants_total(&*variants);
            total = total.add(&part);
        }
        position += 1;
    }
    proof { assert(winners@.take(winners@.len() as int) =~= winners@); }
    total
}

/// The answer for [lower, upper): tally the suppliers' revenues, take the maximum, join the winners.
#[verifier::spinoff_prover]
#[verifier::rlimit(300)]
pub fn execute(facts: &Facts, dimensions: &Dimensions, lower: i32, upper: i32, Ghost(db): Ghost<Db>) -> (result: Option<Vec<OutputRow>>)
    requires facts_match(*facts, db.lineitem), dimensions_match(*dimensions, db.supplier),
    ensures result matches Some(rows) ==> query_result(db, params(lower as int, upper as int), out_view(rows@)),
        answer_fits(db, params(lower as int, upper as int)) ==> result is Some,
{
    let ghost parameters = params(lower as int, upper as int);
    if lower >= upper {
        proof {
            lemma_empty_range(db, parameters);
            assert(out_view(Seq::<OutputRow>::empty()) =~= Seq::<OutRow>::empty());
        }
        return Some(Vec::new());
    }
    let known_facts = facts.share();
    let known_dimensions = dimensions.share();
    let (cells, keys) = tally(facts, lower, upper);
    let (maximum, winners, totals) = scan_totals(&cells, &keys);
    proof {
        keys@.to_set_ensures();
        assert forall|position: int| 0 <= position < totals@.len() implies {
            let pair = #[trigger] totals@[position];
            window_sum(db.lineitem, pair.0 as int, lower as int, upper as int, false) > 0 &&
            pair.1 == window_sum(db.lineitem, pair.0 as int, lower as int, upper as int, true)
        } by {
            let pair = totals@[position];
            lemma_facts_window(facts, db.lineitem, pair.0 as int, lower as int, upper as int, false);
            lemma_facts_window(facts, db.lineitem, pair.0 as int, lower as int, upper as int, true);
        }
        assert forall|supplier: int| #[trigger] has_group_x(totals@, supplier) <==> window_sum(db.lineitem, supplier, lower as int, upper as int, false) > 0 by {
            if i64::MIN <= supplier <= i64::MAX {
                let key = supplier as i64;
                assert(key as int == supplier);
                lemma_facts_window(facts, db.lineitem, supplier, lower as int, upper as int, false);
                if cnt_at(cells@, key) > 0 { assert(cells@.contains_key(key)); assert(keys@.contains(key)); }
            } else {
                lemma_facts_window_outside(facts, supplier, lower as int, upper as int, false);
                lemma_facts_window(facts, db.lineitem, supplier, lower as int, upper as int, false);
                assert(!has_group_x(totals@, supplier)) by {
                    if has_group_x(totals@, supplier) {
                        let position = choose|position: int| 0 <= position < totals@.len() && (#[trigger] totals@[position]).0 as int == supplier;
                    }
                }
            }
        }
        assert(groups_match_x(totals@, db, lower as int, upper as int));
    }
    let top = match maximum {
        Some(top) => top,
        None => {
            proof {
                assert forall|supplier: int| #[trigger] window_sum(db.lineitem, supplier, lower as int, upper as int, false) == 0 by {
                    assert(!has_group_x(totals@, supplier)) by { assert(totals@.len() == 0); }
                    let weight = window_weight(supplier, lower as int, upper as int, false);
                    assert forall|index: int| 0 <= index < db.lineitem.len() implies weight(#[trigger] db.lineitem[index]) >= 0 by {}
                    lemma_sum_nonneg(db.lineitem, weight);
                }
                lemma_no_groups(db, parameters);
                assert(out_view(Seq::<OutputRow>::empty()) =~= Seq::<OutRow>::empty());
            }
            return Some(Vec::new());
        }
    };
    let ghost revenue = top@;
    let ordered = sort_winners(&winners);
    proof {
        assert(max_ok(totals@, Some(revenue), ordered@)) by {
            assert forall|supplier: int| #[trigger] has_winner(ordered@, supplier) <==> has_winner(winners@, supplier) by {
                if has_winner(ordered@, supplier) {
                    let position = choose|position: int| 0 <= position < ordered.len() && (#[trigger] ordered@[position]) as int == supplier;
                    assert(ordered@.contains(ordered@[position]));
                    assert(winners@.contains(ordered@[position]));
                    let found = choose|found: int| 0 <= found < winners.len() && winners@[found] == ordered@[position];
                }
                if has_winner(winners@, supplier) {
                    let position = choose|position: int| 0 <= position < winners.len() && (#[trigger] winners@[position]) as int == supplier;
                    assert(winners@.contains(winners@[position]));
                    assert(ordered@.contains(winners@[position]));
                    let found = choose|found: int| 0 <= found < ordered.len() && ordered@[found] == winners@[position];
                }
            }
        }
        let model = emitted_rows(*dimensions, ordered@, revenue);
        lemma_emitted_ordered(*dimensions, ordered@, revenue);
        assert(model.to_multiset() =~= rows(db, parameters).to_multiset()) by {
            broadcast use vstd::multiset::group_multiset_axioms;
            assert forall|output: OutRow| #[trigger] model.to_multiset().count(output) == rows(db, parameters).to_multiset().count(output) by {
                lemma_emitted_count(dimensions, db.supplier, ordered@, revenue, output);
                lemma_sql_count_x(db, parameters, totals@, ordered@, revenue, output);
            }
        }
        lemma_execute_ok(db, parameters, model);
        lemma_emitted_len(*dimensions, ordered@, revenue);
    }
    let ghost model = emitted_rows(*dimensions, ordered@, revenue);
    let total = winners_total_exec(dimensions, &ordered);
    if Wide::from_i128(1099511627776).lt(&total) {
        proof {
            assert(model.len() == total@);
            if answer_fits(db, parameters) { assert(query_result(db, parameters, model)); assert(model.len() <= max_rows()); }
        }
        return None;
    }
    if total.is_zero() {
        proof {
            assert(model.len() == 0);
            assert(out_view(Seq::<OutputRow>::empty()) =~= model);
        }
        return Some(Vec::new());
    }
    let wide_revenue = match top.to_i128() {
        Some(value) => value,
        None => {
            proof {
                assert(model.len() == total@);
                lemma_emitted_keys(*dimensions, ordered@, revenue, 0);
                if answer_fits(db, parameters) { assert(query_result(db, parameters, model)); assert(out_fits(model[0])); }
            }
            return None;
        }
    };
    let rows = emit(dimensions, &ordered, wide_revenue);
    proof { assert(wide_revenue as int == revenue); }
    Some(rows)
}

}
