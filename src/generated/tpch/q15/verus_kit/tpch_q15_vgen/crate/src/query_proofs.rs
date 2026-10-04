use vstd::prelude::*;
use verus_common::sums::*;
use verus_common::rel::*;
use verus_prooflib::ordered::{Index, KeyView, lemma_key_order};
use crate::gen_spec::*;
use crate::model::*;
use crate::output::{selected_count, selected_revenue, pending_weight};
use crate::output::{maximum_matches, winning_keys};
use verus_common::order::{map_where, lemma_map_where};

verus! {
pub proof fn lemma_component_cancel(left: Seq<u8>, right: Seq<u8>, left_tail: Seq<u8>, right_tail: Seq<u8>)
    requires encode_part(left) + left_tail == encode_part(right) + right_tail,
    ensures left == right, left_tail == right_tail,
{
    let encoded_left = encode_part(left);
    let encoded_right = encode_part(right);
    let combined = encoded_left + left_tail;
    if left.len() < right.len() {
        let position = 2 * left.len() as int;
        assert(combined[position] == 0u8);
        assert((encoded_right + right_tail)[position] == 1u8);
        assert(false);
    }
    if right.len() < left.len() {
        let position = 2 * right.len() as int;
        assert(combined[position] == 1u8);
        assert((encoded_right + right_tail)[position] == 0u8);
        assert(false);
    }
    assert(left.len() == right.len());
    assert forall|position: int| 0 <= position < left.len() implies (#[trigger] left[position]) == right[position] by {
        let encoded_position = 2 * position + 1;
        assert(combined[encoded_position] == left[position]);
        assert((encoded_right + right_tail)[encoded_position] == right[position]);
    }
    assert(left =~= right);
    assert(combined.subrange(encoded_left.len() as int, combined.len() as int) =~= left_tail);
    assert((encoded_right + right_tail).subrange(encoded_right.len() as int, combined.len() as int) =~= right_tail);
    assert(left_tail =~= right_tail);
}
pub proof fn lemma_variant_injective(left_name: Seq<u8>, left_address: Seq<u8>, left_phone: Seq<u8>, right_name: Seq<u8>, right_address: Seq<u8>, right_phone: Seq<u8>)
    requires variant_key(left_name, left_address, left_phone) == variant_key(right_name, right_address, right_phone),
    ensures left_name == right_name, left_address == right_address, left_phone == right_phone,
{
    assert(variant_key(left_name, left_address, left_phone).1 == (encode_part(left_name) + encode_part(left_address)) + encode_part(left_phone));
    assert(variant_key(right_name, right_address, right_phone).1 == (encode_part(right_name) + encode_part(right_address)) + encode_part(right_phone));
    assert((encode_part(left_name) + encode_part(left_address)) + encode_part(left_phone) =~= encode_part(left_name) + (encode_part(left_address) + encode_part(left_phone)));
    assert((encode_part(right_name) + encode_part(right_address)) + encode_part(right_phone) =~= encode_part(right_name) + (encode_part(right_address) + encode_part(right_phone)));
    assert(encode_part(left_name) + (encode_part(left_address) + encode_part(left_phone)) =~=
        encode_part(right_name) + (encode_part(right_address) + encode_part(right_phone)));
    lemma_component_cancel(left_name, right_name, encode_part(left_address) + encode_part(left_phone), encode_part(right_address) + encode_part(right_phone));
    lemma_component_cancel(left_address, right_address, encode_part(left_phone), encode_part(right_phone));
    assert(encode_part(left_phone) + Seq::<u8>::empty() =~= encode_part(right_phone) + Seq::<u8>::empty());
    lemma_component_cancel(left_phone, right_phone, Seq::<u8>::empty(), Seq::<u8>::empty());
}
pub open spec fn window_weight(supplier: int, lower: int, upper: int, revenue: bool) -> spec_fn(LineitemRow) -> int {
    |row: LineitemRow| if row.l_suppkey == supplier && lower <= row.l_shipdate < upper {
        if revenue { weight(row) } else { 1int }
    } else { 0int }
}
pub open spec fn window_sum(rows: Seq<LineitemRow>, supplier: int, lower: int, upper: int, revenue: bool) -> int {
    sum(rows, window_weight(supplier, lower, upper, revenue))
}
pub open spec fn has_group(totals: Seq<(i64, i128)>, supplier: int) -> bool {
    exists|position: int| 0 <= position < totals.len() && (#[trigger] totals[position]).0 as int == supplier
}
pub open spec fn groups_match(totals: Seq<(i64, i128)>, db: Db, lower: int, upper: int) -> bool {
    (forall|position: int| 0 <= position < totals.len() ==> {
        let pair = #[trigger] totals[position];
        window_sum(db.lineitem, pair.0 as int, lower, upper, false) > 0 &&
        pair.1 as int == window_sum(db.lineitem, pair.0 as int, lower, upper, true)
    }) &&
    (forall|supplier: int| #[trigger] has_group(totals, supplier) <==> window_sum(db.lineitem, supplier, lower, upper, false) > 0)
}
pub proof fn lemma_group_push(totals: Seq<(i64, i128)>, pair: (i64, i128), supplier: int)
    ensures has_group(totals.push(pair), supplier) <==> has_group(totals, supplier) || supplier == pair.0 as int,
{
    let after = totals.push(pair);
    if has_group(after, supplier) {
        let position = choose|position: int| 0 <= position < after.len() && (#[trigger] after[position]).0 as int == supplier;
        if position < totals.len() { assert(totals[position] == after[position]); assert(has_group(totals, supplier)); }
        else { assert(position == totals.len()); }
    }
    if has_group(totals, supplier) {
        let position = choose|position: int| 0 <= position < totals.len() && (#[trigger] totals[position]).0 as int == supplier;
        assert(after[position] == totals[position]);
        assert(has_group(after, supplier));
    }
    if supplier == pair.0 as int { assert(after[totals.len() as int] == pair); assert(has_group(after, supplier)); }
}
pub proof fn lemma_no_groups(db: Db, parameters: Params)
    requires forall|supplier: int| #[trigger] window_sum(db.lineitem, supplier, parameters.p1, parameters.p2, false) == 0,
    ensures query_result(db, parameters, Seq::<OutRow>::empty()),
{
    let tuples = revenue0_tuples(db, parameters);
    if tuples.len() > 0 {
        let supplier = tuples[0].lineitem.l_suppkey;
        lemma_range_group(db, parameters, supplier);
        let selected = |tuple: Revenue0Tuple| if tuple.lineitem.l_suppkey == supplier { 1int } else { 0int };
        lemma_sum_pos(tuples, selected, 0);
        assert(revenue0_group_count(db, parameters, supplier) == sum(tuples, selected));
        assert(false);
    }
    assert(rel_revenue0(db, parameters).len() == 0) by { reveal(verus_common::rel::dedup); reveal(verus_common::order::map_where); }
    let query_bags = db.supplier.map_values(|supplier: SupplierRow| tuples_1(db, parameters, supplier));
    assert forall|position: int| 0 <= position < query_bags.len() implies (#[trigger] query_bags[position]).len() == 0 by {
        assert(query_bags[position] == tuples_1(db, parameters, db.supplier[position]));
    }
    crate::database_proofs::lemma_empty_query_bags(query_bags);
    assert(rows(db, parameters).len() == 0);
    assert(rows(db, parameters) =~= Seq::<OutRow>::empty());
}
pub open spec fn revenue0_keys(db: Db, parameters: Params) -> Seq<int> {
    dedup(map_where(revenue0_tuples(db, parameters), |tuple: Revenue0Tuple| revenue0_having(db, parameters, tuple.lineitem.l_suppkey), |tuple: Revenue0Tuple| tuple.lineitem.l_suppkey))
}
pub open spec fn revenue1_keys(db: Db, parameters: Params) -> Seq<int> {
    dedup(map_where(revenue1_tuples(db, parameters), |tuple: Revenue1Tuple| revenue1_having(db, parameters, tuple.lineitem_2.l_suppkey), |tuple: Revenue1Tuple| tuple.lineitem_2.l_suppkey))
}
pub proof fn lemma_support0(db: Db, parameters: Params, supplier: int)
    ensures no_dups(revenue0_keys(db, parameters)),
        revenue0_keys(db, parameters).contains(supplier) <==> window_sum(db.lineitem, supplier, parameters.p1, parameters.p2, false) > 0,
{
    let tuples = revenue0_tuples(db, parameters);
    let keep = |tuple: Revenue0Tuple| revenue0_having(db, parameters, tuple.lineitem.l_suppkey);
    let key = |tuple: Revenue0Tuple| tuple.lineitem.l_suppkey;
    let projected = map_where(tuples, keep, key);
    let keys = revenue0_keys(db, parameters);
    lemma_dedup(projected);
    lemma_map_where(tuples, keep, key);
    lemma_range_group(db, parameters, supplier);
    let count = |tuple: Revenue0Tuple| if tuple.lineitem.l_suppkey == supplier { 1int } else { 0int };
    assert(sum(tuples, count) == window_sum(db.lineitem, supplier, parameters.p1, parameters.p2, false));
    if keys.contains(supplier) {
        assert(projected.contains(supplier));
        let position = choose|position: int| 0 <= position < tuples.len() && keep(tuples[position]) && (#[trigger] key(tuples[position])) == supplier;
        lemma_sum_pos(tuples, count, position);
    }
    if sum(tuples, count) > 0 {
        let position = lemma_sum_nonzero_witness(tuples, count);
        assert(key(tuples[position]) == supplier);
        assert(projected.contains(supplier));
        assert(keys.contains(supplier));
    }
}
pub proof fn lemma_support1(db: Db, parameters: Params, supplier: int)
    ensures no_dups(revenue1_keys(db, parameters)),
        revenue1_keys(db, parameters).contains(supplier) <==> window_sum(db.lineitem, supplier, parameters.p1, parameters.p2, false) > 0,
{
    let tuples = revenue1_tuples(db, parameters);
    let keep = |tuple: Revenue1Tuple| revenue1_having(db, parameters, tuple.lineitem_2.l_suppkey);
    let key = |tuple: Revenue1Tuple| tuple.lineitem_2.l_suppkey;
    let projected = map_where(tuples, keep, key);
    let keys = revenue1_keys(db, parameters);
    lemma_dedup(projected);
    lemma_map_where(tuples, keep, key);
    lemma_range_group(db, parameters, supplier);
    let count = |tuple: Revenue1Tuple| if tuple.lineitem_2.l_suppkey == supplier { 1int } else { 0int };
    assert(sum(tuples, count) == window_sum(db.lineitem, supplier, parameters.p1, parameters.p2, false));
    if keys.contains(supplier) {
        assert(projected.contains(supplier));
        let position = choose|position: int| 0 <= position < tuples.len() && keep(tuples[position]) && (#[trigger] key(tuples[position])) == supplier;
        lemma_sum_pos(tuples, count, position);
    }
    if sum(tuples, count) > 0 {
        let position = lemma_sum_nonzero_witness(tuples, count);
        assert(key(tuples[position]) == supplier);
        assert(projected.contains(supplier));
        assert(keys.contains(supplier));
    }
}
pub open spec fn has_winner(winners: Seq<i64>, supplier: int) -> bool {
    exists|position: int| 0 <= position < winners.len() && (#[trigger] winners[position]) as int == supplier
}
pub proof fn lemma_winner_membership(totals: Seq<(i64, i128)>, value: i128)
    ensures forall|supplier: int| #[trigger] has_winner(winning_keys(totals, value), supplier) <==>
        exists|position: int| 0 <= position < totals.len() && (#[trigger] totals[position]).0 as int == supplier && totals[position].1 == value,
    decreases totals.len(),
{
    reveal_with_fuel(Seq::filter, 2);
    if totals.len() > 0 {
        let prefix = totals.drop_last();
        lemma_winner_membership(prefix, value);
        let before = winning_keys(prefix, value);
        let after = winning_keys(totals, value);
        let last = totals.last();
        if last.1 == value { assert(after =~= before.push(last.0)); }
        else { assert(after =~= before); }
        assert forall|supplier: int| #[trigger] has_winner(after, supplier) <==>
            exists|position: int| 0 <= position < totals.len() && (#[trigger] totals[position]).0 as int == supplier && totals[position].1 == value by {
            if has_winner(after, supplier) {
                let position = choose|position: int| 0 <= position < after.len() && (#[trigger] after[position]) as int == supplier;
                if position < before.len() {
                    assert(has_winner(before, supplier));
                    let index = choose|index: int| 0 <= index < prefix.len() && (#[trigger] prefix[index]).0 as int == supplier && prefix[index].1 == value;
                    assert(totals[index] == prefix[index]);
                } else { assert(last.0 as int == supplier); assert(totals[totals.len() as int - 1] == last); }
            }
            if exists|position: int| 0 <= position < totals.len() && (#[trigger] totals[position]).0 as int == supplier && totals[position].1 == value {
                let index = choose|index: int| 0 <= index < totals.len() && (#[trigger] totals[index]).0 as int == supplier && totals[index].1 == value;
                if index < prefix.len() {
                    assert(prefix[index] == totals[index]);
                    assert(has_winner(before, supplier));
                    let position = choose|position: int| 0 <= position < before.len() && (#[trigger] before[position]) as int == supplier;
                    assert(after[position] == before[position]);
                    assert(has_winner(after, supplier));
                } else { assert(after[after.len() as int - 1] == last.0); assert(has_winner(after, supplier)); }
            }
        }
    }
}
pub proof fn lemma_scalar_values(rows: Seq<Revenue1Row>, db: Db, parameters: Params)
    ensures flat(rows.map_values(|row: Revenue1Row| scalar_1_tuples_1(db, parameters, row))).len() == rows.len(),
        flat(rows.map_values(|row: Revenue1Row| scalar_1_tuples_1(db, parameters, row))).map_values(|tuple: Scalar1Tuple| tuple.revenue1.total_revenue)
            == rows.map_values(|row: Revenue1Row| row.total_revenue),
    decreases rows.len(),
{
    if rows.len() > 0 {
        let prefix = rows.drop_last();
        lemma_scalar_values(prefix, db, parameters);
        let mapped = rows.map_values(|row: Revenue1Row| scalar_1_tuples_1(db, parameters, row));
        let earlier = prefix.map_values(|row: Revenue1Row| scalar_1_tuples_1(db, parameters, row));
        assert(mapped.drop_last() =~= earlier);
        assert(mapped.last() == seq![Scalar1Tuple { revenue1: rows.last() }]);
        assert(flat(mapped) =~= flat(earlier).push(Scalar1Tuple { revenue1: rows.last() }));
        assert(flat(earlier).map_values(|tuple: Scalar1Tuple| tuple.revenue1.total_revenue) == prefix.map_values(|row: Revenue1Row| row.total_revenue));
        assert(flat(mapped).map_values(|tuple: Scalar1Tuple| tuple.revenue1.total_revenue) =~= flat(earlier).map_values(|tuple: Scalar1Tuple| tuple.revenue1.total_revenue).push(rows.last().total_revenue));
        assert(rows.map_values(|row: Revenue1Row| row.total_revenue) =~= prefix.map_values(|row: Revenue1Row| row.total_revenue).push(rows.last().total_revenue));
        assert(flat(mapped).map_values(|tuple: Scalar1Tuple| tuple.revenue1.total_revenue) =~= rows.map_values(|row: Revenue1Row| row.total_revenue));
    }
}
pub proof fn lemma_revenue1_max(rows: Seq<Revenue1Row>)
    requires rows.len() > 0,
    ensures forall|position: int| 0 <= position < rows.len() ==> (#[trigger] rows[position]).total_revenue <= max_of(rows.map_values(|row: Revenue1Row| row.total_revenue)),
        exists|position: int| 0 <= position < rows.len() && (#[trigger] rows[position]).total_revenue == max_of(rows.map_values(|row: Revenue1Row| row.total_revenue)),
    decreases rows.len(),
{
    let values = rows.map_values(|row: Revenue1Row| row.total_revenue);
    if rows.len() == 1 { assert(rows[0].total_revenue == max_of(values)); }
    else {
        let prefix = rows.drop_last();
        lemma_revenue1_max(prefix);
        let earlier = prefix.map_values(|row: Revenue1Row| row.total_revenue);
        assert(values.drop_last() =~= earlier);
        if rows.last().total_revenue > max_of(earlier) { assert(rows[rows.len() as int - 1].total_revenue == max_of(values)); }
        else {
            let position = choose|position: int| 0 <= position < prefix.len() && (#[trigger] prefix[position]).total_revenue == max_of(earlier);
            assert(rows[position].total_revenue == max_of(values));
        }
        assert forall|position: int| 0 <= position < rows.len() implies (#[trigger] rows[position]).total_revenue <= max_of(values) by {
            if position < prefix.len() { assert(rows[position] == prefix[position]); }
        }
    }
}
pub proof fn lemma_maximum_sql(db: Db, parameters: Params, totals: Seq<(i64, i128)>, winners: Seq<i64>, value: i128)
    requires groups_match(totals, db, parameters.p1, parameters.p2), maximum_matches(totals, Some(value), winners),
    ensures scalar_1_value(db, parameters) == Some(value as int),
{
    let rows = rel_revenue1(db, parameters);
    let keys = revenue1_keys(db, parameters);
    assert(rows == keys.map_values(|key: int| revenue1_row_of(db, parameters, key)));
    assert forall|position: int| 0 <= position < rows.len() implies (#[trigger] rows[position]).total_revenue <= value as int by {
        let key = keys[position];
        assert(keys.contains(key));
        lemma_support1(db, parameters, key);
        lemma_range_group(db, parameters, key);
        assert(has_group(totals, key));
        let index = choose|index: int| 0 <= index < totals.len() && (#[trigger] totals[index]).0 as int == key;
        assert(rows[position].total_revenue == totals[index].1 as int);
    }
    let witness = choose|position: int| 0 <= position < totals.len() && (#[trigger] totals[position]).1 == value;
    let key = totals[witness].0 as int;
    lemma_support1(db, parameters, key);
    lemma_range_group(db, parameters, key);
    assert(keys.contains(key));
    let position = choose|position: int| 0 <= position < keys.len() && (#[trigger] keys[position]) == key;
    assert(rows[position].total_revenue == value as int);
    assert(rows.len() > 0);
    lemma_revenue1_max(rows);
    let maximum_position = choose|position: int| 0 <= position < rows.len() && (#[trigger] rows[position]).total_revenue == max_of(rows.map_values(|row: Revenue1Row| row.total_revenue));
    assert(max_of(rows.map_values(|row: Revenue1Row| row.total_revenue)) == value as int);
    lemma_scalar_values(rows, db, parameters);
    assert(scalar_1_group_count(db, parameters) == rows.len());
    assert(scalar_1_group_vals(db, parameters, |tuple: Scalar1Tuple| tuple.revenue1.total_revenue) == rows.map_values(|row: Revenue1Row| row.total_revenue));
}
pub open spec fn profile_value(profile: BucketProfile, lower: int, upper: int, revenue: bool) -> int {
    if revenue { selected_revenue(profile, lower, upper) } else { selected_count(profile, lower, upper) }
}
pub open spec fn facts_window_weight(supplier: int, lower: int, upper: int, revenue: bool) -> spec_fn((KeyView, Index<BucketProfile>)) -> int {
    |entry: (KeyView, Index<BucketProfile>)| sum(entry.1.entries(), pending_weight(supplier, lower, upper, revenue))
}
pub open spec fn facts_window(facts: Facts, supplier: int, lower: int, upper: int, revenue: bool) -> int {
    sum(facts.entries(), facts_window_weight(supplier, lower, upper, revenue))
}
pub proof fn lemma_profile_lookup(profiles: &Index<BucketProfile>, supplier: int, lower: int, upper: int, revenue: bool)
    requires profiles.wf(),
    ensures sum(profiles.entries(), pending_weight(supplier, lower, upper, revenue)) ==
        match profiles.at(number_key(supplier)) { Some(profile) => profile_value(profile, lower, upper, revenue), None => 0int },
{
    profiles.model();
    let key = number_key(supplier);
    let rows = profiles.entries();
    let selected = pending_weight(supplier, lower, upper, revenue);
    if profiles.at(key) is Some {
        let position = choose|position: int| 0 <= position < rows.len() && (#[trigger] rows[position]).0 == key;
        assert(rows[position].1 == profiles.at(key).unwrap());
        assert forall|other: int| 0 <= other < rows.len() && other != position implies selected(#[trigger] rows[other]) == 0 by {
            lemma_key_order(key, key);
            if other < position { assert(verus_prooflib::ordered::key_lt(rows[other].0, rows[position].0)); }
            else { assert(verus_prooflib::ordered::key_lt(rows[position].0, rows[other].0)); }
        }
        lemma_sum_single_match(rows, selected, position);
    } else {
        assert forall|position: int| 0 <= position < rows.len() implies selected(#[trigger] rows[position]) == 0 by {
            assert(profiles.at(rows[position].0) is Some);
        }
        lemma_sum_zero(rows, selected);
    }
}
pub proof fn lemma_range_group(db: Db, parameters: Params, supplier: int)
    ensures revenue0_group_count(db, parameters, supplier) == window_sum(db.lineitem, supplier, parameters.p1, parameters.p2, false),
        revenue0_row_of(db, parameters, supplier).total_revenue == window_sum(db.lineitem, supplier, parameters.p1, parameters.p2, true),
        revenue1_group_count(db, parameters, supplier) == window_sum(db.lineitem, supplier, parameters.p1, parameters.p2, false),
        revenue1_row_of(db, parameters, supplier).total_revenue == window_sum(db.lineitem, supplier, parameters.p1, parameters.p2, true),
{
    let count0 = |tuple: Revenue0Tuple| if tuple.lineitem.l_suppkey == supplier { 1int } else { 0int };
    let revenue0 = |tuple: Revenue0Tuple| if tuple.lineitem.l_suppkey == supplier { weight(tuple.lineitem) } else { 0int };
    let mapper0 = |row: LineitemRow| revenue0_tuples_1(db, parameters, row);
    lemma_sum_flat_map(db.lineitem, mapper0, count0);
    lemma_sum_flat_map(db.lineitem, mapper0, revenue0);
    assert forall|row: LineitemRow| #[trigger] sum(mapper0(row), count0) == window_weight(supplier, parameters.p1, parameters.p2, false)(row) by {
        lemma_sum_opt(revenue0_where_clause(db, parameters, row), Revenue0Tuple { lineitem: row }, count0);
    }
    assert forall|row: LineitemRow| #[trigger] sum(mapper0(row), revenue0) == window_weight(supplier, parameters.p1, parameters.p2, true)(row) by {
        lemma_sum_opt(revenue0_where_clause(db, parameters, row), Revenue0Tuple { lineitem: row }, revenue0);
    }
    lemma_sum_ext_all(db.lineitem, |row: LineitemRow| sum(mapper0(row), count0), window_weight(supplier, parameters.p1, parameters.p2, false));
    lemma_sum_ext_all(db.lineitem, |row: LineitemRow| sum(mapper0(row), revenue0), window_weight(supplier, parameters.p1, parameters.p2, true));
    lemma_sum_ext_all(revenue0_tuples(db, parameters), revenue0,
        |tuple: Revenue0Tuple| if tuple.lineitem.l_suppkey == supplier { tuple.lineitem.l_extendedprice * (100 - tuple.lineitem.l_discount) } else { 0int });
    let aggregate0 = |tuple: Revenue0Tuple| tuple.lineitem.l_extendedprice * (100 - tuple.lineitem.l_discount);
    lemma_sum_ext_all(revenue0_tuples(db, parameters), revenue0,
        |tuple: Revenue0Tuple| if tuple.lineitem.l_suppkey == supplier { aggregate0(tuple) } else { 0int });
    assert(revenue0_group_sum(db, parameters, supplier, aggregate0) == sum(revenue0_tuples(db, parameters), revenue0));
    assert(revenue0_row_of(db, parameters, supplier).total_revenue == revenue0_group_sum(db, parameters, supplier, aggregate0));
    let count1 = |tuple: Revenue1Tuple| if tuple.lineitem_2.l_suppkey == supplier { 1int } else { 0int };
    let revenue1 = |tuple: Revenue1Tuple| if tuple.lineitem_2.l_suppkey == supplier { weight(tuple.lineitem_2) } else { 0int };
    let mapper1 = |row: LineitemRow| revenue1_tuples_1(db, parameters, row);
    lemma_sum_flat_map(db.lineitem, mapper1, count1);
    lemma_sum_flat_map(db.lineitem, mapper1, revenue1);
    assert forall|row: LineitemRow| #[trigger] sum(mapper1(row), count1) == window_weight(supplier, parameters.p1, parameters.p2, false)(row) by {
        lemma_sum_opt(revenue1_where_clause(db, parameters, row), Revenue1Tuple { lineitem_2: row }, count1);
    }
    assert forall|row: LineitemRow| #[trigger] sum(mapper1(row), revenue1) == window_weight(supplier, parameters.p1, parameters.p2, true)(row) by {
        lemma_sum_opt(revenue1_where_clause(db, parameters, row), Revenue1Tuple { lineitem_2: row }, revenue1);
    }
    lemma_sum_ext_all(db.lineitem, |row: LineitemRow| sum(mapper1(row), count1), window_weight(supplier, parameters.p1, parameters.p2, false));
    lemma_sum_ext_all(db.lineitem, |row: LineitemRow| sum(mapper1(row), revenue1), window_weight(supplier, parameters.p1, parameters.p2, true));
    lemma_sum_ext_all(revenue1_tuples(db, parameters), revenue1,
        |tuple: Revenue1Tuple| if tuple.lineitem_2.l_suppkey == supplier { tuple.lineitem_2.l_extendedprice * (100 - tuple.lineitem_2.l_discount) } else { 0int });
    let aggregate1 = |tuple: Revenue1Tuple| tuple.lineitem_2.l_extendedprice * (100 - tuple.lineitem_2.l_discount);
    lemma_sum_ext_all(revenue1_tuples(db, parameters), revenue1,
        |tuple: Revenue1Tuple| if tuple.lineitem_2.l_suppkey == supplier { aggregate1(tuple) } else { 0int });
    assert(revenue1_group_sum(db, parameters, supplier, aggregate1) == sum(revenue1_tuples(db, parameters), revenue1));
    assert(revenue1_row_of(db, parameters, supplier).total_revenue == revenue1_group_sum(db, parameters, supplier, aggregate1));
}
pub open spec fn profile_day_value(profile: BucketProfile, date: int, revenue: bool) -> int {
    if revenue { days_revenue(profile.days@, date) } else { days_count(profile.days@, date) }
}
pub open spec fn database_day_value(rows: Seq<LineitemRow>, supplier: int, date: int, revenue: bool) -> int {
    if revenue { day_revenue(rows, supplier, date) } else { day_count(rows, supplier, date) }
}
pub open spec fn stored_day_value(facts: Facts, supplier: int, date: int, revenue: bool) -> int {
    if revenue { stored_day_revenue(facts, supplier, date) } else { stored_day_count(facts, supplier, date) }
}
pub open spec fn bucket_day_weight(supplier: int, date: int, revenue: bool) -> spec_fn((KeyView, Index<BucketProfile>)) -> int {
    |entry: (KeyView, Index<BucketProfile>)| match entry.1.at(number_key(supplier)) {
        Some(profile) => profile_day_value(profile, date, revenue), None => 0int,
    }
}
pub proof fn lemma_day_outside(profile: BucketProfile, bin: int, date: int, revenue: bool)
    requires profile_shape(profile, bin), bucket(date) != bin,
    ensures profile_day_value(profile, date, revenue) == 0,
{
    if revenue {
        assert forall|position: int| 0 <= position < profile.days.len() implies (#[trigger] profile.days@[position]).shipdate as int != date by { }
        lemma_sum_zero(profile.days@, |day: Day| if day.shipdate as int == date { day.revenue as int } else { 0int });
    } else {
        assert forall|position: int| 0 <= position < profile.days.len() implies (#[trigger] profile.days@[position]).shipdate as int != date by { }
        lemma_sum_zero(profile.days@, |day: Day| if day.shipdate as int == date { day.count as int } else { 0int });
    }
}
pub proof fn lemma_bucket_day_sum(facts: &Facts, supplier: int, date: int, revenue: bool)
    requires facts.wf(), facts_shape(*facts),
    ensures sum(facts.entries(), bucket_day_weight(supplier, date, revenue)) == stored_day_value(*facts, supplier, date, revenue),
{
    facts.model();
    let rows = facts.entries();
    let key = number_key(bucket(date));
    let selected = bucket_day_weight(supplier, date, revenue);
    assert forall|position: int| 0 <= position < rows.len() && rows[position].0 != key implies selected(#[trigger] rows[position]) == 0 by {
        let entry = rows[position];
        assert(facts.at(entry.0) == Some(entry.1));
        assert(entry.0 == number_key(entry.0.0));
        assert(entry.0.0 != bucket(date));
        if entry.1.at(number_key(supplier)) is Some {
            let profile = entry.1.at(number_key(supplier)).unwrap();
            assert(profile_at(*facts, entry.0.0, supplier) == Some(profile));
            lemma_day_outside(profile, entry.0.0, date, revenue);
        }
    }
    if facts.at(key) is Some {
        let position = choose|position: int| 0 <= position < rows.len() && (#[trigger] rows[position]).0 == key;
        assert(rows[position].1 == facts.at(key).unwrap());
        assert forall|other: int| 0 <= other < rows.len() && other != position implies selected(#[trigger] rows[other]) == 0 by {
            lemma_key_order(key, key);
            if other < position { assert(verus_prooflib::ordered::key_lt(rows[other].0, rows[position].0)); }
            else { assert(verus_prooflib::ordered::key_lt(rows[position].0, rows[other].0)); }
        }
        lemma_sum_single_match(rows, selected, position);
    } else {
        assert forall|position: int| 0 <= position < rows.len() implies selected(#[trigger] rows[position]) == 0 by {
            assert(facts.at(rows[position].0) is Some);
            assert(rows[position].0 != key);
        }
        lemma_sum_zero(rows, selected);
    }
}
pub proof fn lemma_profile_partition(profile: BucketProfile, lower: int, upper: int, revenue: bool)
    requires lower < upper,
    ensures profile_value(profile, lower, upper, revenue) ==
        sum(rng(upper - lower), |offset: int| profile_day_value(profile, lower + offset, revenue)),
{
    let dates = rng(upper - lower);
    let rows = profile.days@;
    let paired = |day: Day, offset: int| if day.shipdate as int == lower + offset {
        if revenue { day.revenue as int } else { day.count as int }
    } else { 0int };
    let per_row = |day: Day| if lower <= day.shipdate as int && (day.shipdate as int) < upper {
        if revenue { day.revenue as int } else { day.count as int }
    } else { 0int };
    let per_date = |offset: int| profile_day_value(profile, lower + offset, revenue);
    assert forall|day: Day| #[trigger] per_row(day) == sum(dates, |offset: int| paired(day, offset)) by {
        let date = day.shipdate as int;
        let selected = |offset: int| paired(day, offset);
        if lower <= date < upper {
            let position = date - lower;
            assert(dates[position] == position);
            assert forall|other: int| 0 <= other < dates.len() && other != position implies selected(#[trigger] dates[other]) == 0 by { assert(dates[other] == other); }
            lemma_sum_single_match(dates, selected, position);
        } else {
            assert forall|other: int| 0 <= other < dates.len() implies selected(#[trigger] dates[other]) == 0 by { assert(dates[other] == other); }
            lemma_sum_zero(dates, selected);
        }
    }
    assert forall|offset: int| #[trigger] per_date(offset) == sum(rows, |day: Day| paired(day, offset)) by {
        if revenue { lemma_sum_ext_all(rows, |day: Day| paired(day, offset), |day: Day| if day.shipdate as int == lower + offset { day.revenue as int } else { 0int }); }
        else { lemma_sum_ext_all(rows, |day: Day| paired(day, offset), |day: Day| if day.shipdate as int == lower + offset { day.count as int } else { 0int }); }
    }
    lemma_sum_swap(rows, dates, paired, per_row, per_date);
    if revenue { lemma_sum_ext_all(rows, per_row, |day: Day| if lower <= day.shipdate as int && (day.shipdate as int) < upper { day.revenue as int } else { 0int }); }
    else { lemma_sum_ext_all(rows, per_row, |day: Day| if lower <= day.shipdate as int && (day.shipdate as int) < upper { day.count as int } else { 0int }); }
}
pub proof fn lemma_database_partition(rows: Seq<LineitemRow>, supplier: int, lower: int, upper: int, revenue: bool)
    requires lower < upper,
    ensures window_sum(rows, supplier, lower, upper, revenue) ==
        sum(rng(upper - lower), |offset: int| database_day_value(rows, supplier, lower + offset, revenue)),
{
    let dates = rng(upper - lower);
    let paired = |row: LineitemRow, offset: int| if row.l_suppkey == supplier && row.l_shipdate == lower + offset {
        if revenue { weight(row) } else { 1int }
    } else { 0int };
    let per_row = window_weight(supplier, lower, upper, revenue);
    let per_date = |offset: int| database_day_value(rows, supplier, lower + offset, revenue);
    assert forall|row: LineitemRow| #[trigger] per_row(row) == sum(dates, |offset: int| paired(row, offset)) by {
        let date = row.l_shipdate;
        let selected = |offset: int| paired(row, offset);
        if lower <= date < upper {
            let position = date - lower;
            assert(dates[position] == position);
            assert forall|other: int| 0 <= other < dates.len() && other != position implies selected(#[trigger] dates[other]) == 0 by { assert(dates[other] == other); }
            lemma_sum_single_match(dates, selected, position);
        } else {
            assert forall|other: int| 0 <= other < dates.len() implies selected(#[trigger] dates[other]) == 0 by { assert(dates[other] == other); }
            lemma_sum_zero(dates, selected);
        }
    }
    assert forall|offset: int| #[trigger] per_date(offset) == sum(rows, |row: LineitemRow| paired(row, offset)) by {
        if revenue { lemma_sum_ext_all(rows, |row: LineitemRow| paired(row, offset), |row: LineitemRow| if row.l_shipdate == lower + offset && row.l_suppkey == supplier { weight(row) } else { 0int }); }
        else { lemma_sum_ext_all(rows, |row: LineitemRow| paired(row, offset), |row: LineitemRow| if row.l_shipdate == lower + offset && row.l_suppkey == supplier { 1int } else { 0int }); }
    }
    lemma_sum_swap(rows, dates, paired, per_row, per_date);
}
pub proof fn lemma_facts_window(facts: &Facts, rows: Seq<LineitemRow>, supplier: int, lower: int, upper: int, revenue: bool)
    requires facts.wf(), facts_match(*facts, rows), lower < upper,
    ensures facts_window(*facts, supplier, lower, upper, revenue) == window_sum(rows, supplier, lower, upper, revenue),
{
    facts.model();
    let entries = facts.entries();
    let dates = rng(upper - lower);
    let paired = |entry: (KeyView, Index<BucketProfile>), offset: int| bucket_day_weight(supplier, lower + offset, revenue)(entry);
    let per_entry = facts_window_weight(supplier, lower, upper, revenue);
    let per_date = |offset: int| stored_day_value(*facts, supplier, lower + offset, revenue);
    assert forall|position: int| 0 <= position < entries.len() implies (#[trigger] entries[position]).1.wf() by {
        assert(facts.at(entries[position].0) == Some(entries[position].1));
    }
    assert forall|entry: (KeyView, Index<BucketProfile>)| #[trigger] per_entry(entry) == sum(dates, |offset: int| paired(entry, offset)) || !entries.contains(entry) by {
        if entries.contains(entry) {
            let position = choose|position: int| 0 <= position < entries.len() && (#[trigger] entries[position]) == entry;
            lemma_profile_lookup(&entry.1, supplier, lower, upper, revenue);
            if entry.1.at(number_key(supplier)) is Some {
                let profile = entry.1.at(number_key(supplier)).unwrap();
                lemma_profile_partition(profile, lower, upper, revenue);
                lemma_sum_ext_all(dates, |offset: int| paired(entry, offset), |offset: int| profile_day_value(profile, lower + offset, revenue));
            } else { lemma_sum_zero(dates, |offset: int| paired(entry, offset)); }
        }
    }
    let restricted_per_entry = |entry: (KeyView, Index<BucketProfile>)| sum(dates, |offset: int| paired(entry, offset));
    lemma_sum_ext(entries, per_entry, restricted_per_entry);
    assert forall|offset: int| #[trigger] per_date(offset) == sum(entries, |entry: (KeyView, Index<BucketProfile>)| paired(entry, offset)) by {
        lemma_bucket_day_sum(facts, supplier, lower + offset, revenue);
        lemma_sum_ext_all(entries, |entry: (KeyView, Index<BucketProfile>)| paired(entry, offset), bucket_day_weight(supplier, lower + offset, revenue));
    }
    lemma_sum_swap(entries, dates, paired, restricted_per_entry, per_date);
    lemma_database_partition(rows, supplier, lower, upper, revenue);
    lemma_sum_ext_all(dates, per_date, |offset: int| database_day_value(rows, supplier, lower + offset, revenue));
}
pub proof fn lemma_profile_range_zero(profile: BucketProfile, bin: int, lower: int, upper: int)
    requires profile_shape(profile, bin), lower < upper, bin < bucket(lower) || bin > bucket(upper - 1),
    ensures selected_count(profile, lower, upper) == 0, selected_revenue(profile, lower, upper) == 0,
{
    assert forall|position: int| 0 <= position < profile.days.len() implies
        !(lower <= (#[trigger] profile.days@[position]).shipdate as int && (profile.days@[position].shipdate as int) < upper) by {
        let date = profile.days@[position].shipdate as int;
        if lower <= date < upper {
            assert(lower / 256 <= date / 256 <= (upper - 1) / 256);
            assert(date / 256 == bin);
        }
    }
    lemma_sum_zero(profile.days@, |day: Day| if lower <= day.shipdate as int && (day.shipdate as int) < upper { day.count as int } else { 0int });
    lemma_sum_zero(profile.days@, |day: Day| if lower <= day.shipdate as int && (day.shipdate as int) < upper { day.revenue as int } else { 0int });
}
pub proof fn lemma_bucket_range_zero(facts: &Facts, entry: (KeyView, Index<BucketProfile>), supplier: int, lower: int, upper: int, revenue: bool)
    requires facts_shape(*facts), facts.at(entry.0) == Some(entry.1), lower < upper,
        entry.0.0 < bucket(lower) || entry.0.0 > bucket(upper - 1),
    ensures facts_window_weight(supplier, lower, upper, revenue)(entry) == 0,
{
    lemma_profile_lookup(&entry.1, supplier, lower, upper, revenue);
    if entry.1.at(number_key(supplier)) is Some {
        let profile = entry.1.at(number_key(supplier)).unwrap();
        assert(entry.0 == number_key(entry.0.0));
        assert(profile_at(*facts, entry.0.0, supplier) == Some(profile));
        lemma_profile_range_zero(profile, entry.0.0, lower, upper);
    }
}
}
