// The query proofs with exact integer revenues: the totals pair a supplier with its revenue as an int.
use vstd::prelude::*;
use verus_common::sums::*;
use verus_common::rel::*;
use verus_common::order::*;
use crate::gen_spec::*;
use crate::model::*;
use crate::query_proofs::*;
use crate::result_proofs::*;

verus! {

pub open spec fn has_group_x(totals: Seq<(i64, int)>, supplier: int) -> bool {
    exists|position: int| 0 <= position < totals.len() && (#[trigger] totals[position]).0 as int == supplier
}

pub open spec fn groups_match_x(totals: Seq<(i64, int)>, db: Db, lower: int, upper: int) -> bool {
    (forall|position: int| 0 <= position < totals.len() ==> {
        let pair = #[trigger] totals[position];
        window_sum(db.lineitem, pair.0 as int, lower, upper, false) > 0 &&
        pair.1 == window_sum(db.lineitem, pair.0 as int, lower, upper, true)
    }) &&
    (forall|supplier: int| #[trigger] has_group_x(totals, supplier) <==> window_sum(db.lineitem, supplier, lower, upper, false) > 0)
}

/// The maximum of the totals and the suppliers that reach it.
pub open spec fn max_ok(totals: Seq<(i64, int)>, maximum: Option<int>, winners: Seq<i64>) -> bool {
    match maximum {
        None => totals.len() == 0 && winners.len() == 0,
        Some(value) => {
            &&& totals.len() > 0
            &&& forall|position: int| 0 <= position < totals.len() ==> (#[trigger] totals[position]).1 <= value
            &&& exists|position: int| 0 <= position < totals.len() && (#[trigger] totals[position]).1 == value
            &&& forall|supplier: int| #[trigger] has_winner(winners, supplier) <==>
                exists|position: int| 0 <= position < totals.len() && (#[trigger] totals[position]).0 as int == supplier && totals[position].1 == value
        },
    }
}

pub proof fn lemma_maximum_sql_x(db: Db, parameters: Params, totals: Seq<(i64, int)>, winners: Seq<i64>, value: int)
    requires groups_match_x(totals, db, parameters.p1, parameters.p2), max_ok(totals, Some(value), winners),
    ensures scalar_1_value(db, parameters) == Some(value),
{
    let rows = rel_revenue1(db, parameters);
    let keys = revenue1_keys(db, parameters);
    assert(rows == keys.map_values(|key: int| revenue1_row_of(db, parameters, key)));
    assert forall|position: int| 0 <= position < rows.len() implies (#[trigger] rows[position]).total_revenue <= value by {
        let key = keys[position];
        assert(keys.contains(key));
        lemma_support1(db, parameters, key);
        lemma_range_group(db, parameters, key);
        assert(has_group_x(totals, key));
        let index = choose|index: int| 0 <= index < totals.len() && (#[trigger] totals[index]).0 as int == key;
        assert(rows[position].total_revenue == totals[index].1);
    }
    let witness = choose|position: int| 0 <= position < totals.len() && (#[trigger] totals[position]).1 == value;
    let key = totals[witness].0 as int;
    lemma_support1(db, parameters, key);
    lemma_range_group(db, parameters, key);
    assert(keys.contains(key));
    let position = choose|position: int| 0 <= position < keys.len() && (#[trigger] keys[position]) == key;
    assert(rows[position].total_revenue == value);
    assert(rows.len() > 0);
    lemma_revenue1_max(rows);
    let maximum_position = choose|position: int| 0 <= position < rows.len() && (#[trigger] rows[position]).total_revenue == max_of(rows.map_values(|row: Revenue1Row| row.total_revenue));
    assert(max_of(rows.map_values(|row: Revenue1Row| row.total_revenue)) == value);
    lemma_scalar_values(rows, db, parameters);
    assert(scalar_1_group_count(db, parameters) == rows.len());
    assert(scalar_1_group_vals(db, parameters, |tuple: Scalar1Tuple| tuple.revenue1.total_revenue) == rows.map_values(|row: Revenue1Row| row.total_revenue));
}

pub proof fn lemma_winner_qualifies_x(db: Db, parameters: Params, totals: Seq<(i64, int)>, winners: Seq<i64>, value: int, supplier: int)
    requires groups_match_x(totals, db, parameters.p1, parameters.p2), max_ok(totals, Some(value), winners),
    ensures has_winner(winners, supplier) <==> window_sum(db.lineitem, supplier, parameters.p1, parameters.p2, false) > 0 &&
        window_sum(db.lineitem, supplier, parameters.p1, parameters.p2, true) == value,
{
    if has_winner(winners, supplier) {
        let position = choose|position: int| 0 <= position < totals.len() && (#[trigger] totals[position]).0 as int == supplier && totals[position].1 == value;
    }
    if window_sum(db.lineitem, supplier, parameters.p1, parameters.p2, false) > 0 && window_sum(db.lineitem, supplier, parameters.p1, parameters.p2, true) == value {
        assert(has_group_x(totals, supplier));
        let position = choose|position: int| 0 <= position < totals.len() && (#[trigger] totals[position]).0 as int == supplier;
        assert(totals[position].1 == value);
        assert(has_winner(winners, supplier));
    }
}

pub proof fn lemma_supplier_join_x(db: Db, parameters: Params, totals: Seq<(i64, int)>, winners: Seq<i64>, value: int, supplier: SupplierRow, output: OutRow)
    requires groups_match_x(totals, db, parameters.p1, parameters.p2), max_ok(totals, Some(value), winners), scalar_1_value(db, parameters) == Some(value),
    ensures sum(tuples_1(db, parameters, supplier), tuple_weight(output)) == supplier_output_weight(winners, value, output)(supplier),
{
    let keys = revenue0_keys(db, parameters);
    let rows0 = rel_revenue0(db, parameters);
    let weight = tuple_weight(output);
    let mapper = |row: Revenue0Row| tuples_2(db, parameters, supplier, row);
    let per_row = |row: Revenue0Row| sum(mapper(row), weight);
    let projection = |key: int| revenue0_row_of(db, parameters, key);
    let per_key = |key: int| per_row(projection(key));
    assert(rows0 == keys.map_values(projection));
    lemma_sum_flat_map(rows0, mapper, weight);
    lemma_sum_map(keys, projection, per_row, per_key);
    assert(sum(tuples_1(db, parameters, supplier), weight) == sum(keys, per_key));
    lemma_support0(db, parameters, supplier.s_suppkey);
    lemma_range_group(db, parameters, supplier.s_suppkey);
    lemma_winner_qualifies_x(db, parameters, totals, winners, value, supplier.s_suppkey);
    if keys.contains(supplier.s_suppkey) {
        let position = choose|position: int| 0 <= position < keys.len() && (#[trigger] keys[position]) == supplier.s_suppkey;
        assert forall|other: int| 0 <= other < keys.len() && other != position implies per_key(#[trigger] keys[other]) == 0 by {
            if other < position { assert(keys[other] != keys[position]); }
            else { assert(keys[position] != keys[other]); }
            let row = projection(keys[other]);
            assert(!where_clause(db, parameters, supplier, row));
            lemma_sum_opt(false, Tuple { supplier, revenue0: row }, weight);
        }
        lemma_sum_single_match(keys, per_key, position);
        let row = projection(supplier.s_suppkey);
        lemma_sum_opt(where_clause(db, parameters, supplier, row), Tuple { supplier, revenue0: row }, weight);
        if where_clause(db, parameters, supplier, row) { assert(tuple_output(Tuple { supplier, revenue0: row }) == supplier_output(supplier, value)); }
        assert(per_key(supplier.s_suppkey) == supplier_output_weight(winners, value, output)(supplier));
    } else {
        assert forall|position: int| 0 <= position < keys.len() implies per_key(#[trigger] keys[position]) == 0 by {
            assert(keys[position] != supplier.s_suppkey);
            let row = projection(keys[position]);
            assert(!where_clause(db, parameters, supplier, row));
            lemma_sum_opt(false, Tuple { supplier, revenue0: row }, weight);
        }
        lemma_sum_zero(keys, per_key);
        assert(!has_winner(winners, supplier.s_suppkey));
    }
}

pub proof fn lemma_sql_count_x(db: Db, parameters: Params, totals: Seq<(i64, int)>, winners: Seq<i64>, value: int, output: OutRow)
    requires groups_match_x(totals, db, parameters.p1, parameters.p2), max_ok(totals, Some(value), winners),
    ensures rows(db, parameters).to_multiset().count(output) as int == sum(db.supplier, supplier_output_weight(winners, value, output)),
{
    lemma_maximum_sql_x(db, parameters, totals, winners, value);
    let tuples = tuples(db, parameters);
    let rows = rows(db, parameters);
    let projection = |tuple: Tuple| tuple_output(tuple);
    let weight = tuple_weight(output);
    assert(rows =~= tuples.map_values(projection));
    lemma_count_as_sum(rows, output);
    lemma_sum_map(tuples, projection, eqv(output), weight);
    let mapper = |supplier: SupplierRow| tuples_1(db, parameters, supplier);
    lemma_sum_flat_map(db.supplier, mapper, weight);
    let nested = |supplier: SupplierRow| sum(mapper(supplier), weight);
    let expected = supplier_output_weight(winners, value, output);
    assert forall|supplier: SupplierRow| #[trigger] nested(supplier) == expected(supplier) by {
        lemma_supplier_join_x(db, parameters, totals, winners, value, supplier, output);
    }
    lemma_sum_ext_all(db.supplier, nested, expected);
}

}
