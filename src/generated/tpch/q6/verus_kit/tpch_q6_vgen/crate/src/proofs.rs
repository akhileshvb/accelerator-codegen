use vstd::prelude::*;
use verus_common::sums::*;
use verus_common::rel::*;
use crate::cube::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;

verus! {

pub open spec fn rows_valid(rows: Seq<LineitemRow>) -> bool {
    forall|index: int| 0 <= index < rows.len() ==> #[trigger] representable(rows[index])
}

pub proof fn lemma_valid_from_zero(rows: Seq<LineitemRow>)
    requires sum(rows, |row: LineitemRow| if representable(row) { 0int } else { 1int }) == 0,
    ensures rows_valid(rows),
{
    let invalid = |row: LineitemRow| if representable(row) { 0int } else { 1int };
    assert forall|index: int| 0 <= index < rows.len() implies #[trigger] representable(rows[index]) by {
        lemma_sum_ge_term(rows, invalid, index);
    }
}

pub proof fn lemma_build_transfer(db: Db, rows: Seq<LineitemInput>)
    requires scan_lineitem_cube_ok(db, rows),
    ensures
        rows_valid(db.lineitem),
        forall|weight: spec_fn(Key) -> int, revenue: bool| #[trigger] table_weight(db.lineitem, weight, revenue) ==
            input_weight(rows, weight, revenue),
{
    let invalid = |row: LineitemRow| if representable(row) { 0int } else { 1int };
    lemma_sum_map(rows, |row: LineitemInput| lineitem_input_view(row), invalid,
        |row: LineitemInput| invalid(lineitem_input_view(row)));
    lemma_sum_zero(rows, |row: LineitemInput| invalid(lineitem_input_view(row)));
    assert(lineitem_input_seq_view(rows) == rows.map_values(|row: LineitemInput| lineitem_input_view(row)));
    assert(sum(lineitem_input_seq_view(rows), invalid) == 0);
    assert(sum(lineitem_input_seq_view(rows), invalid) == sum(db.lineitem, invalid));
    assert(sum(db.lineitem, invalid) == 0);
    lemma_valid_from_zero(db.lineitem);
    assert forall|weight: spec_fn(Key) -> int, revenue: bool| #[trigger] table_weight(db.lineitem, weight, revenue) ==
        input_weight(rows, weight, revenue) by {
        let row_weight = |row: LineitemRow| weight(row_key(row)) * row_measure(row, revenue);
        let direct = |row: LineitemInput| weight(row_key(lineitem_input_view(row))) * row_measure(lineitem_input_view(row), revenue);
        lemma_sum_map(rows, |row: LineitemInput| lineitem_input_view(row), row_weight,
            direct);
        assert(sum(lineitem_input_seq_view(rows), row_weight) == input_weight(rows, weight, revenue));
        assert(sum(lineitem_input_seq_view(rows), row_weight) == sum(db.lineitem, row_weight));
    }
}

pub proof fn lemma_apply_transfer(db: Db, db2: Db, rows: Seq<LineitemDelta>)
    requires rows_valid(db.lineitem), capture_ok(db, db2, lineitem_delta_seq_view(rows)),
    ensures
        rows_valid(db2.lineitem),
        forall|weight: spec_fn(Key) -> int, revenue: bool| #[trigger] table_weight(db2.lineitem, weight, revenue) ==
            table_weight(db.lineitem, weight, revenue) + delta_weight(rows, weight, revenue),
{
    let invalid = |row: LineitemRow| if representable(row) { 0int } else { 1int };
    lemma_sum_zero(db.lineitem, invalid);
    let signed_invalid = |delta: (LineitemRow, int)| delta.1 * invalid(delta.0);
    lemma_sum_map(rows, |delta: LineitemDelta| (lineitem_input_view(delta.row), delta.sign as int), signed_invalid,
        |delta: LineitemDelta| signed_invalid((lineitem_input_view(delta.row), delta.sign as int)));
    lemma_sum_zero(rows, |delta: LineitemDelta| signed_invalid((lineitem_input_view(delta.row), delta.sign as int)));
    assert(sum(db2.lineitem, invalid) == 0);
    lemma_valid_from_zero(db2.lineitem);
    assert forall|weight: spec_fn(Key) -> int, revenue: bool| #[trigger] table_weight(db2.lineitem, weight, revenue) ==
        table_weight(db.lineitem, weight, revenue) + delta_weight(rows, weight, revenue) by {
        let row_weight = |row: LineitemRow| weight(row_key(row)) * row_measure(row, revenue);
        let signed = |delta: (LineitemRow, int)| delta.1 * row_weight(delta.0);
        let projected = |delta: LineitemDelta| signed((lineitem_input_view(delta.row), delta.sign as int));
        let direct = |delta: LineitemDelta| weight(row_key(lineitem_input_view(delta.row))) *
            ((delta.sign as int) * row_measure(lineitem_input_view(delta.row), revenue));
        lemma_sum_map(rows, |delta: LineitemDelta| (lineitem_input_view(delta.row), delta.sign as int), signed, projected);
        assert forall|delta: LineitemDelta| #[trigger] projected(delta) == direct(delta) by {
            lemma_mul_assoc(delta.sign as int, weight(row_key(lineitem_input_view(delta.row))), row_measure(lineitem_input_view(delta.row), revenue));
        }
        lemma_sum_ext_all(rows, projected, direct);
        assert(sum(db2.lineitem, row_weight) == sum(db.lineitem, row_weight) + sum(lineitem_delta_seq_view(rows), signed));
    }
}

pub open spec fn query_weight(parameters: Params) -> spec_fn(Key) -> int {
    |key: Key| if parameters.p1 <= key.1 < parameters.p4 && parameters.p2 - 1 <= key.0 <= parameters.p2 + 1 &&
        key.2 < parameters.p3 { 1int } else { 0int }
}

pub proof fn lemma_sum_one<A>(rows: Seq<A>)
    ensures sum(rows, |row: A| 1int) == rows.len(),
    decreases rows.len(),
{
    if rows.len() > 0 { lemma_sum_one(rows.drop_last()); }
}

pub proof fn lemma_query_transfer(db: Db, parameters: Params)
    requires rows_valid(db.lineitem),
    ensures
        group_count(db, parameters) == table_weight(db.lineitem, query_weight(parameters), false),
        group_sum(db, parameters, |tuple: Tuple| tuple.lineitem.l_extendedprice * tuple.lineitem.l_discount) ==
            table_weight(db.lineitem, query_weight(parameters), true),
{
    let extension = |row: LineitemRow| tuples_1(db, parameters, row);
    let count = |tuple: Tuple| 1int;
    let revenue = |tuple: Tuple| tuple.lineitem.l_extendedprice * tuple.lineitem.l_discount;
    lemma_sum_one(tuples(db, parameters));
    lemma_sum_flat_map(db.lineitem, extension, count);
    lemma_sum_flat_map(db.lineitem, extension, revenue);
    assert forall|index: int| 0 <= index < db.lineitem.len() implies
        (query_weight(parameters))(row_key(#[trigger] db.lineitem[index])) ==
        if where_clause(db, parameters, db.lineitem[index]) { 1int } else { 0int } by {
        assert(representable(db.lineitem[index]));
    }
    assert forall|index: int| 0 <= index < db.lineitem.len() implies
        sum(extension(#[trigger] db.lineitem[index]), count) ==
        (query_weight(parameters))(row_key(db.lineitem[index])) * row_measure(db.lineitem[index], false) by {
        lemma_sum_opt(where_clause(db, parameters, db.lineitem[index]), Tuple { lineitem: db.lineitem[index] }, count);
    }
    assert forall|index: int| 0 <= index < db.lineitem.len() implies
        sum(extension(#[trigger] db.lineitem[index]), revenue) ==
        (query_weight(parameters))(row_key(db.lineitem[index])) * row_measure(db.lineitem[index], true) by {
        lemma_sum_opt(where_clause(db, parameters, db.lineitem[index]), Tuple { lineitem: db.lineitem[index] }, revenue);
    }
    lemma_sum_ext(db.lineitem, |row: LineitemRow| sum(extension(row), count),
        |row: LineitemRow| (query_weight(parameters))(row_key(row)) * row_measure(row, false));
    lemma_sum_ext(db.lineitem, |row: LineitemRow| sum(extension(row), revenue),
        |row: LineitemRow| (query_weight(parameters))(row_key(row)) * row_measure(row, true));
}

}
