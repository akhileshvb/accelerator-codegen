use vstd::prelude::*;
use verus_common::sums::*;
use verus_prooflib::wide::Wide;
use crate::cube::*;

verus! {

pub fn merge(left: &Vec<Cell>, right: &Vec<Cell>) -> (cells: Vec<Cell>)
    requires sorted(left@), sorted(right@), nonzero(left@), nonzero(right@),
    ensures sorted(cells@) && nonzero(cells@) &&
        cells.len() <= left.len() + right.len() &&
        (forall|weight: spec_fn(Key) -> int, revenue: bool| #[trigger] weighted(cells@, weight, revenue) ==
            weighted(left@, weight, revenue) + weighted(right@, weight, revenue)),
{
    let mut cells = Vec::new();
    let mut first = 0usize;
    let mut second = 0usize;
    while first < left.len() || second < right.len()
        invariant
            first <= left.len(), second <= right.len(),
            sorted(left@), sorted(right@), nonzero(left@), nonzero(right@),
            sorted(cells@), nonzero(cells@),
            cells.len() <= first + second,
            forall|index: int| 0 <= index < cells.len() && first < left.len() ==>
                #[trigger] key_lt(cells@[index].key, left@[first as int].key),
            forall|index: int| 0 <= index < cells.len() && second < right.len() ==>
                #[trigger] key_lt(cells@[index].key, right@[second as int].key),
            forall|weight: spec_fn(Key) -> int, revenue: bool| #[trigger] weighted(cells@, weight, revenue) ==
                weighted(left@.take(first as int), weight, revenue) + weighted(right@.take(second as int), weight, revenue),
        decreases (left.len() - first) + (right.len() - second),
    {
        let ghost previous = cells@;
        let ghost old_first = first;
        let ghost old_second = second;
        proof {
            if first < left.len() { lemma_weight_take(left@, first as int); }
            if second < right.len() { lemma_weight_take(right@, second as int); }
        }
        if second == right.len() || (first < left.len() && less(left[first].key, right[second].key)) {
            cells.push(left[first].dup());
            proof {
                assert(cells@ == previous.push(left@[first as int]));
                lemma_sorted_push(previous, left@[first as int]);
                lemma_nonzero_push(previous, left@[first as int]);
                lemma_weight_push(previous, left@[first as int]);
            }
            first += 1;
        } else if first == left.len() || less(right[second].key, left[first].key) {
            cells.push(right[second].dup());
            proof {
                assert(cells@ == previous.push(right@[second as int]));
                lemma_sorted_push(previous, right@[second as int]);
                lemma_nonzero_push(previous, right@[second as int]);
                lemma_weight_push(previous, right@[second as int]);
            }
            second += 1;
        } else {
            let ghost left_cell = left@[first as int];
            let ghost right_cell = right@[second as int];
            proof { lemma_key_order(left_cell.key, right_cell.key, right_cell.key); }
            let count = left[first].count.add(&right[second].count);
            let revenue = left[first].revenue.add(&right[second].revenue);
            let keep = !count.is_zero() || !revenue.is_zero();
            let ghost total_count = count@;
            let ghost total_revenue = revenue@;
            if keep {
                cells.push(Cell { key: left[first].key, count, revenue });
                proof {
                    let cell = cells@[previous.len() as int];
                    assert(cells@ == previous.push(cell));
                    lemma_sorted_push(previous, cell);
                    lemma_nonzero_push(previous, cell);
                    lemma_weight_push(previous, cell);
                }
            }
            proof {
                assert forall|weight: spec_fn(Key) -> int, revenues: bool| #[trigger] weighted(cells@, weight, revenues) ==
                    weighted(previous, weight, revenues) + weight(left_cell.key) * measure(left_cell, revenues) + weight(right_cell.key) * measure(right_cell, revenues) by {
                    let total = if revenues { total_revenue } else { total_count };
                    if keep {
                        assert(weighted(cells@, weight, revenues) == weighted(previous, weight, revenues) + weight(left_cell.key) * total);
                    } else {
                        assert(cells@ == previous);
                        assert(total == 0);
                    }
                    assert(total == measure(left_cell, revenues) + measure(right_cell, revenues));
                    lemma_mul_add(weight(left_cell.key), measure(left_cell, revenues), measure(right_cell, revenues));
                }
            }
            first += 1;
            second += 1;
        }
        proof {
            assert forall|index: int| 0 <= index < cells.len() && first < left.len() implies
                #[trigger] key_lt(cells@[index].key, left@[first as int].key) by {
                if index < previous.len() {
                    if first != old_first {
                        assert(key_lt(previous[index].key, left@[old_first as int].key));
                        assert(key_lt(left@[old_first as int].key, left@[first as int].key));
                        lemma_key_order(previous[index].key, left@[old_first as int].key, left@[first as int].key);
                    }
                } else {
                    if first != old_first { assert(cells@[index].key == left@[old_first as int].key); }
                    else { assert(cells@[index].key == right@[old_second as int].key); }
                }
            }
            assert forall|index: int| 0 <= index < cells.len() && second < right.len() implies
                #[trigger] key_lt(cells@[index].key, right@[second as int].key) by {
                if index < previous.len() {
                    if second != old_second {
                        assert(key_lt(previous[index].key, right@[old_second as int].key));
                        assert(key_lt(right@[old_second as int].key, right@[second as int].key));
                        lemma_key_order(previous[index].key, right@[old_second as int].key, right@[second as int].key);
                    }
                } else {
                    if second != old_second { assert(cells@[index].key == right@[old_second as int].key); }
                    else { assert(cells@[index].key == left@[old_first as int].key); }
                }
            }
        }
    }
    proof {
        assert(left@.take(first as int) == left@);
        assert(right@.take(second as int) == right@);
    }
    cells
}

pub fn seal(cells: Vec<Cell>) -> (output: Vec<Cell>)
    ensures sorted(output@) && nonzero(output@) &&
        output.len() <= cells.len() &&
        (forall|weight: spec_fn(Key) -> int, revenue: bool| #[trigger] weighted(output@, weight, revenue) == weighted(cells@, weight, revenue)),
    decreases cells.len(),
{
    if cells.len() <= 1 {
        let mut output = Vec::new();
        if cells.len() == 1 && (!cells[0].count.is_zero() || !cells[0].revenue.is_zero()) {
            proof { lemma_weight_push(Seq::empty(), cells@[0]); }
            output.push(cells[0].dup());
        }
        proof {
            if cells.len() == 1 { assert(cells@ == seq![cells@[0]]); }
            assert forall|weight: spec_fn(Key) -> int, revenue: bool| #[trigger] weighted(output@, weight, revenue) == weighted(cells@, weight, revenue) by {
                if output.len() == 0 {
                    assert forall|index: int| 0 <= index < cells@.len() implies
                        (|cell: Cell| weight(cell.key) * measure(cell, revenue))(#[trigger] cells@[index]) == 0 by {
                        assert(measure(cells@[index], revenue) == 0);
                    }
                    lemma_sum_zero(cells@, |cell: Cell| weight(cell.key) * measure(cell, revenue));
                } else { lemma_sum_singleton(cells@[0], |cell: Cell| weight(cell.key) * measure(cell, revenue)); }
            }
        }
        return output;
    }
    let midpoint = cells.len() / 2;
    let mut left = Vec::new();
    let mut right = Vec::new();
    let mut position = 0usize;
    while position < cells.len()
        invariant
            position <= cells.len(),
            0 < midpoint < cells.len(),
            left@ == cells@.take(if position < midpoint { position as int } else { midpoint as int }),
            right@ == cells@.subrange(midpoint as int, if position < midpoint { midpoint as int } else { position as int }),
        decreases cells.len() - position,
    {
        if position < midpoint { left.push(cells[position].dup()); }
        else { right.push(cells[position].dup()); }
        position += 1;
    }
    proof {
        assert(cells@ == left@ + right@);
        lemma_weight_concat(left@, right@);
    }
    let left = seal(left);
    let right = seal(right);
    merge(&left, &right)
}

pub open spec fn before(cell: Cell, discount: i64, day: i32, quantity: i64, mode: u8) -> bool {
    cell.key.0 < discount || (cell.key.0 == discount &&
        (cell.key.1 < day || (cell.key.1 == day &&
            (mode == 1 || (mode == 2 && cell.key.2 < quantity)))))
}

pub open spec fn cut(cells: Seq<Cell>, limit: int, discount: i64, day: i32, quantity: i64, mode: u8) -> bool {
    &&& 0 <= limit <= cells.len()
    &&& forall|index: int| 0 <= index < limit ==> #[trigger] before(cells[index], discount, day, quantity, mode)
    &&& forall|index: int| limit <= index < cells.len() ==> !#[trigger] before(cells[index], discount, day, quantity, mode)
}

pub proof fn lemma_before_order(left: Cell, right: Cell, discount: i64, day: i32, quantity: i64, mode: u8)
    requires key_lt(left.key, right.key), mode <= 2,
    ensures before(right, discount, day, quantity, mode) ==> before(left, discount, day, quantity, mode),
{}

pub fn bound(cells: &Vec<Cell>, discount: i64, day: i32, quantity: i64, mode: u8) -> (result: usize)
    requires sorted(cells@), mode <= 2,
    ensures
        result <= cells.len(),
        cut(cells@, result as int, discount, day, quantity, mode),
        forall|index: int| 0 <= index < result ==> #[trigger] before(cells@[index], discount, day, quantity, mode),
        forall|index: int| result <= index < cells.len() ==> !#[trigger] before(cells@[index], discount, day, quantity, mode),
{
    let mut lower = 0usize;
    let mut upper = cells.len();
    while lower < upper
        invariant
            lower <= upper <= cells.len(),
            sorted(cells@), mode <= 2,
            forall|index: int| 0 <= index < lower ==> #[trigger] before(cells@[index], discount, day, quantity, mode),
            forall|index: int| upper <= index < cells.len() ==> !#[trigger] before(cells@[index], discount, day, quantity, mode),
        decreases upper - lower,
    {
        let middle = lower + (upper - lower) / 2;
        let key = cells[middle].key;
        let is_before = key.0 < discount || (key.0 == discount &&
            (key.1 < day || (key.1 == day &&
                (mode == 1 || (mode == 2 && key.2 < quantity)))));
        if is_before {
            proof {
                assert forall|index: int| 0 <= index <= middle implies #[trigger] before(cells@[index], discount, day, quantity, mode) by {
                    if index < middle {
                        assert(key_lt(cells@[index].key, cells@[middle as int].key));
                        lemma_before_order(cells@[index], cells@[middle as int], discount, day, quantity, mode);
                    }
                }
            }
            lower = middle + 1;
        } else {
            proof {
                assert forall|index: int| middle <= index < cells.len() implies !#[trigger] before(cells@[index], discount, day, quantity, mode) by {
                    if index > middle {
                        assert(key_lt(cells@[middle as int].key, cells@[index].key));
                        lemma_before_order(cells@[middle as int], cells@[index], discount, day, quantity, mode);
                    }
                }
            }
            upper = middle;
        }
    }
    lower
}

pub open spec fn selected(cell: Cell, discount: i64, start: i32, quantity: i64, end: i32) -> bool {
    cell.key.0 == discount && start <= cell.key.1 < end && cell.key.2 < quantity
}

pub open spec fn selected_count(cells: Seq<Cell>, discount: i64, start: i32, quantity: i64, end: i32) -> int {
    weighted(cells, selection_weight(discount as int, start as int, quantity as int, end as int), false)
}

pub open spec fn selected_revenue(cells: Seq<Cell>, discount: i64, start: i32, quantity: i64, end: i32) -> int {
    weighted(cells, selection_weight(discount as int, start as int, quantity as int, end as int), true)
}

pub open spec fn selection_weight(discount: int, start: int, quantity: int, end: int) -> spec_fn(Key) -> int {
    |key: Key| if key.0 == discount && start <= key.1 < end && key.2 < quantity { 1int } else { 0int }
}

pub proof fn lemma_sum_prefix_skip<A>(rows: Seq<A>, begin: int, end: int, weight: spec_fn(A) -> int)
    requires 0 <= begin <= end <= rows.len(),
        forall|index: int| begin <= index < end ==> weight(#[trigger] rows[index]) == 0,
    ensures sum(rows.take(begin), weight) == sum(rows.take(end), weight),
    decreases end - begin,
{
    if begin < end {
        lemma_sum_prefix_skip(rows, begin, end - 1, weight);
        lemma_sum_take(rows, end - 1, weight);
    }
}

pub open spec fn day_start(cells: Seq<Cell>, position: int, discount: i64) -> bool {
    position == cells.len() || (0 <= position < cells.len() &&
        forall|index: int| 0 <= index < position ==>
            #[trigger] before(cells[index], discount, cells[position].key.1, 0, 0))
}

pub fn query_run(cells: &Vec<Cell>, discount: i64, start: i32, quantity: i64, end: i32) -> (weights: (Wide, Wide))
    requires sorted(cells@), start < end,
    ensures
        weights.0@ == selected_count(cells@, discount, start, quantity, end),
        weights.1@ == selected_revenue(cells@, discount, start, quantity, end),
{
    let begin = bound(cells, discount, start, 0, 0);
    let finish = bound(cells, discount, end, 0, 0);
    let ghost weight = selection_weight(discount as int, start as int, quantity as int, end as int);
    proof {
        if begin > finish {
            assert(before(cells@[finish as int], discount, start, 0, 0));
            assert(!before(cells@[finish as int], discount, end, 0, 0));
            assert(false);
        }
        assert forall|index: int| begin <= index < finish implies
            (#[trigger] cells@[index]).key.0 == discount && start <= cells@[index].key.1 < end by {
            assert(!before(cells@[index], discount, start, 0, 0));
            assert(before(cells@[index], discount, end, 0, 0));
        }
        assert forall|index: int| 0 <= index < begin implies weight((#[trigger] cells@[index]).key) == 0 by {
            assert(before(cells@[index], discount, start, 0, 0));
        }
        lemma_sum_zero(cells@.take(begin as int), |cell: Cell| weight(cell.key) * measure(cell, false));
        lemma_sum_zero(cells@.take(begin as int), |cell: Cell| weight(cell.key) * measure(cell, true));
        if begin < finish {
            assert forall|index: int| 0 <= index < begin implies
                #[trigger] before(cells@[index], discount, cells@[begin as int].key.1, 0, 0) by {
                assert(before(cells@[index], discount, start, 0, 0));
            }
        }
    }
    let mut position = begin;
    let mut count = Wide::zero();
    let mut revenue = Wide::zero();
    while position < finish
        invariant
            begin <= position <= finish <= cells.len(),
            start < end, sorted(cells@),
            weight == selection_weight(discount as int, start as int, quantity as int, end as int),
            cut(cells@, begin as int, discount, start, 0, 0),
            cut(cells@, finish as int, discount, end, 0, 0),
            forall|index: int| begin <= index < finish ==> (#[trigger] cells@[index]).key.0 == discount && start <= cells@[index].key.1 < end,
            count@ == selected_count(cells@.take(position as int), discount, start, quantity, end),
            revenue@ == selected_revenue(cells@.take(position as int), discount, start, quantity, end),
        decreases finish - position,
    {
        let day = cells[position].key.1;
        let mut cursor = position;
        while cursor < finish && cells[cursor].key.1 == day
            invariant
                finish <= cells.len(), sorted(cells@),
                position <= cursor <= finish,
                weight == selection_weight(discount as int, start as int, quantity as int, end as int),
                forall|index: int| position <= index < finish ==>
                    (#[trigger] cells@[index]).key.0 == discount && start <= cells@[index].key.1 < end,
                forall|index: int| position <= index < cursor ==>
                    (#[trigger] cells@[index]).key.0 == discount && cells@[index].key.1 == day,
                count@ == selected_count(cells@.take(cursor as int), discount, start, quantity, end),
                revenue@ == selected_revenue(cells@.take(cursor as int), discount, start, quantity, end),
            decreases finish - cursor,
        {
            proof {
                lemma_weight_take(cells@, cursor as int);
                assert(cells@[cursor as int].key.0 == discount);
                assert(cells@[cursor as int].key.1 == day);
                assert(weight((#[trigger] cells@[cursor as int]).key) ==
                    if cells@[cursor as int].key.2 < quantity { 1int } else { 0int });
            }
            if cells[cursor].key.2 < quantity {
                count = count.add(&cells[cursor].count);
                revenue = revenue.add(&cells[cursor].revenue);
            }
            cursor += 1;
        }
        position = cursor;
    }
    proof {
        assert forall|index: int| finish <= index < cells.len() implies weight((#[trigger] cells@[index]).key) == 0 by {
            assert(!before(cells@[index], discount, end, 0, 0));
        }
        lemma_sum_prefix_skip(cells@, finish as int, cells@.len() as int, |cell: Cell| weight(cell.key) * measure(cell, false));
        lemma_sum_prefix_skip(cells@, finish as int, cells@.len() as int, |cell: Cell| weight(cell.key) * measure(cell, true));
        assert(cells@.take(cells@.len() as int) == cells@);
    }
    (count, revenue)
}

}
