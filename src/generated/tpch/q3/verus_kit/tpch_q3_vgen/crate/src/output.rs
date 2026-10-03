use vstd::prelude::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use verus_common::order::*;
use crate::proof_support::*;

verus! {

pub type Row = (i64, i128, i32, i64);

pub open spec fn row_view(row: Row) -> OutRow {
    OutRow { l_orderkey: row.0 as int, revenue: row.1 as int, o_orderdate: row.2 as int, o_shippriority: row.3 as int }
}

pub open spec fn selection_lt(left: OutRow, right: OutRow) -> bool {
    left.revenue > right.revenue || (left.revenue == right.revenue &&
        (left.o_orderdate < right.o_orderdate || (left.o_orderdate == right.o_orderdate &&
            (left.l_orderkey > right.l_orderkey ||
                (left.l_orderkey == right.l_orderkey && left.o_shippriority > right.o_shippriority)))))
}

pub open spec fn insert_row(rows: Seq<Row>, row: Row) -> Seq<Row>
{
    insert_by(rows, row, selection_order())
}

pub open spec fn selection_order() -> spec_fn(Row, Row) -> bool {
    |left: Row, right: Row| selection_lt(row_view(left), row_view(right))
}

pub fn better(left: Row, right: Row) -> (result: bool)
    ensures
        result == selection_lt(row_view(left), row_view(right)),
        row_lt(row_view(left), row_view(right)) ==> result,
        result ==> !row_lt(row_view(right), row_view(left)),
{
    left.1 > right.1 || (left.1 == right.1 &&
        (left.2 < right.2 || (left.2 == right.2 &&
            (left.0 > right.0 || (left.0 == right.0 && left.3 > right.3)))))
}

pub fn insertion_position(rows: &Vec<Row>, row: Row) -> (result: usize)
    ensures
        result <= rows.len(),
        forall|index: int| 0 <= index < result ==> !selection_lt(row_view(row), row_view(#[trigger] rows@[index])),
        result < rows.len() ==> selection_lt(row_view(row), row_view(rows@[result as int])),
{
    let mut position = 0;
    while position < rows.len()
        invariant position <= rows.len(),
            forall|index: int| 0 <= index < position ==> !selection_lt(row_view(row), row_view(#[trigger] rows@[index])),
        decreases rows.len() - position,
    {
        if better(row, rows[position]) { return position; }
        position += 1;
    }
    position
}

pub fn select(rows: &mut Vec<Row>, row: Row)
    requires old(rows).len() <= 10,
    ensures
        final(rows).len() <= 10,
        final(rows)@ == insert_row(old(rows)@, row).take(
            if old(rows).len() < 10 { old(rows).len() as int + 1 } else { 10 }),
{
    let ghost before = rows@;
    let position = insertion_position(rows, row);
    proof {
        lemma_insert_by_position(before, row, |left: Row, right: Row| selection_lt(row_view(left), row_view(right)), position as int);
        lemma_insert_by_length(before, row, |left: Row, right: Row| selection_lt(row_view(left), row_view(right)));
    }
    if position < 10 {
        rows.insert(position, row);
        if rows.len() > 10 { rows.pop(); }
    }
    proof {
        if position == 10 {
            assert(before.len() == 10);
            assert(before.insert(10, row).take(10) =~= before);
        } else if before.len() == 10 {
            assert(before.insert(position as int, row).drop_last() =~= before.insert(position as int, row).take(10));
        } else {
            assert(before.insert(position as int, row).take(before.len() as int + 1) =~= before.insert(position as int, row));
        }
    }
}

}
