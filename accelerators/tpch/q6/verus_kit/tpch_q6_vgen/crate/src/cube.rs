use vstd::prelude::*;
use std::collections::HashMap;
use verus_common::sums::*;
use verus_prooflib::wide::Wide;
use crate::gen_spec::*;
use crate::gen_obligations::*;

verus! {

broadcast use vstd::std_specs::hash::group_hash_axioms;

pub type Key = (i64, i32, i64);

pub open spec fn packed(key: Key) -> u128 {
    (((key.1 as int) - i32::MIN) * 18446744073709551616int + ((key.2 as int) - i64::MIN)) as u128
}

pub fn pack(key: Key) -> (result: u128)
    ensures result == packed(key),
{
    let day = (key.1 as i128 - i32::MIN as i128) as u128;
    let quantity = (key.2 as i128 - i64::MIN as i128) as u128;
    day * 18446744073709551616u128 + quantity
}

pub proof fn lemma_pack_injective(left: Key, right: Key)
    requires left.0 == right.0, packed(left) == packed(right),
    ensures left == right,
{
    assert(packed(left) / 18446744073709551616u128 == (left.1 as int) - i32::MIN);
    assert(packed(right) / 18446744073709551616u128 == (right.1 as int) - i32::MIN);
    assert(packed(left) % 18446744073709551616u128 == (left.2 as int) - i64::MIN);
    assert(packed(right) % 18446744073709551616u128 == (right.2 as int) - i64::MIN);
}

pub proof fn lemma_mul_add(weight: int, left: int, right: int)
    ensures weight * (left + right) == weight * left + weight * right,
{
    assert(weight * (left + right) == weight * left + weight * right) by(nonlinear_arith);
}

pub proof fn lemma_mul_assoc(first: int, second: int, last: int)
    ensures first * (second * last) == second * (first * last),
{
    assert(first * (second * last) == second * (first * last)) by(nonlinear_arith);
}

pub open spec fn position_for(positions: Map<i64, HashMap<u128, usize>>, key: Key) -> Option<usize> {
    if positions.contains_key(key.0) && positions[key.0]@.contains_key(packed(key)) {
        Some(positions[key.0]@[packed(key)])
    } else { None }
}

pub open spec fn position_valid(cells: Seq<Cell>, positions: Map<i64, HashMap<u128, usize>>, key: Key) -> bool {
    match position_for(positions, key) {
        Some(index) => index < cells.len() && cells[index as int].key == key,
        None => true,
    }
}

pub open spec fn measure(cell: Cell, revenue: bool) -> int {
    if revenue { cell.revenue@ } else { cell.count@ }
}

pub open spec fn weighted(cells: Seq<Cell>, weight: spec_fn(Key) -> int, revenue: bool) -> int {
    sum(cells, |cell: Cell| weight(cell.key) * measure(cell, revenue))
}

pub open spec fn row_measure(row: LineitemRow, revenue: bool) -> int {
    if revenue { row.l_extendedprice * row.l_discount } else { 1 }
}

pub open spec fn table_weight(rows: Seq<LineitemRow>, weight: spec_fn(Key) -> int, revenue: bool) -> int {
    sum(rows, |row: LineitemRow| weight(row_key(row)) * row_measure(row, revenue))
}

pub open spec fn input_weight(rows: Seq<LineitemInput>, weight: spec_fn(Key) -> int, revenue: bool) -> int {
    sum(rows, |row: LineitemInput| weight(row_key(lineitem_input_view(row))) * row_measure(lineitem_input_view(row), revenue))
}

pub open spec fn delta_weight(rows: Seq<LineitemDelta>, weight: spec_fn(Key) -> int, revenue: bool) -> int {
    sum(rows, |delta: LineitemDelta| weight(row_key(lineitem_input_view(delta.row))) *
        ((delta.sign as int) * row_measure(lineitem_input_view(delta.row), revenue)))
}

pub proof fn lemma_weight_push(cells: Seq<Cell>, cell: Cell)
    ensures forall|weight: spec_fn(Key) -> int, revenue: bool| #[trigger] weighted(cells.push(cell), weight, revenue) ==
        weighted(cells, weight, revenue) + weight(cell.key) * measure(cell, revenue),
{
    assert forall|weight: spec_fn(Key) -> int, revenue: bool| #[trigger] weighted(cells.push(cell), weight, revenue) ==
        weighted(cells, weight, revenue) + weight(cell.key) * measure(cell, revenue) by {
        lemma_sum_push(cells, cell, |cell: Cell| weight(cell.key) * measure(cell, revenue));
    }
}

pub proof fn lemma_weight_take(cells: Seq<Cell>, index: int)
    requires 0 <= index < cells.len(),
    ensures forall|weight: spec_fn(Key) -> int, revenue: bool| #[trigger] weighted(cells.take(index + 1), weight, revenue) ==
        weighted(cells.take(index), weight, revenue) + weight(cells[index].key) * measure(cells[index], revenue),
{
    assert forall|weight: spec_fn(Key) -> int, revenue: bool| #[trigger] weighted(cells.take(index + 1), weight, revenue) ==
        weighted(cells.take(index), weight, revenue) + weight(cells[index].key) * measure(cells[index], revenue) by {
        lemma_sum_take(cells, index, |cell: Cell| weight(cell.key) * measure(cell, revenue));
    }
}

pub proof fn lemma_weight_update(cells: Seq<Cell>, index: int, cell: Cell)
    requires 0 <= index < cells.len(), cell.key == cells[index].key,
    ensures forall|weight: spec_fn(Key) -> int, revenue: bool| #[trigger] weighted(cells.update(index, cell), weight, revenue) ==
        weighted(cells, weight, revenue) + weight(cell.key) * (measure(cell, revenue) - measure(cells[index], revenue)),
{
    assert forall|weight: spec_fn(Key) -> int, revenue: bool| #[trigger] weighted(cells.update(index, cell), weight, revenue) ==
        weighted(cells, weight, revenue) + weight(cell.key) * (measure(cell, revenue) - measure(cells[index], revenue)) by {
        lemma_sum_update(cells, index, cell, |cell: Cell| weight(cell.key) * measure(cell, revenue));
        assert(weight(cell.key) * measure(cell, revenue) - weight(cell.key) * measure(cells[index], revenue) ==
            weight(cell.key) * (measure(cell, revenue) - measure(cells[index], revenue))) by(nonlinear_arith);
    }
}

pub struct Cell {
    pub key: Key,
    pub count: Wide,
    pub revenue: Wide,
}

impl Cell {
    pub fn dup(&self) -> (result: Cell)
        ensures result == *self,
    {
        Cell { key: self.key, count: self.count.dup(), revenue: self.revenue.dup() }
    }
}

pub open spec fn row_key(row: LineitemRow) -> Key {
    (row.l_discount as i64, row.l_shipdate as i32, row.l_quantity as i64)
}

pub open spec fn representable(row: LineitemRow) -> bool {
    i64::MIN <= row.l_discount <= i64::MAX &&
    i32::MIN <= row.l_shipdate <= i32::MAX &&
    i64::MIN <= row.l_quantity <= i64::MAX &&
    i64::MIN <= row.l_extendedprice <= i64::MAX
}

pub open spec fn key_lt(left: Key, right: Key) -> bool {
    left.0 < right.0 || (left.0 == right.0 &&
        (left.1 < right.1 || (left.1 == right.1 && left.2 < right.2)))
}

pub fn less(left: Key, right: Key) -> (result: bool)
    ensures result == key_lt(left, right),
{
    left.0 < right.0 || (left.0 == right.0 &&
        (left.1 < right.1 || (left.1 == right.1 && left.2 < right.2)))
}

pub proof fn lemma_key_order(first: Key, middle: Key, last: Key)
    ensures
        !key_lt(first, first),
        !key_lt(first, middle) && !key_lt(middle, first) ==> first == middle,
        key_lt(first, middle) && key_lt(middle, last) ==> key_lt(first, last),
{}

pub proof fn lemma_sorted_push(cells: Seq<Cell>, cell: Cell)
    requires sorted(cells), forall|index: int| 0 <= index < cells.len() ==> #[trigger] key_lt(cells[index].key, cell.key),
    ensures sorted(cells.push(cell)),
{
    assert forall|first: int, second: int| 0 <= first < second < cells.push(cell).len() implies
        #[trigger] key_lt(cells.push(cell)[first].key, cells.push(cell)[second].key) by {
        if second < cells.len() { assert(key_lt(cells[first].key, cells[second].key)); }
    }
}

pub proof fn lemma_nonzero_push(cells: Seq<Cell>, cell: Cell)
    requires nonzero(cells), cell.count@ != 0 || cell.revenue@ != 0,
    ensures nonzero(cells.push(cell)),
{
    assert forall|index: int| 0 <= index < cells.push(cell).len() implies
        (#[trigger] cells.push(cell)[index]).count@ != 0 || cells.push(cell)[index].revenue@ != 0 by {
        if index < cells.len() { assert(cells.push(cell)[index] == cells[index]); }
    }
}

pub proof fn lemma_weight_concat(left: Seq<Cell>, right: Seq<Cell>)
    ensures forall|weight: spec_fn(Key) -> int, revenue: bool| #[trigger] weighted(left + right, weight, revenue) ==
        weighted(left, weight, revenue) + weighted(right, weight, revenue),
{
    assert forall|weight: spec_fn(Key) -> int, revenue: bool| #[trigger] weighted(left + right, weight, revenue) ==
        weighted(left, weight, revenue) + weighted(right, weight, revenue) by {
        lemma_sum_concat(left, right, |cell: Cell| weight(cell.key) * measure(cell, revenue));
    }
}

pub open spec fn cell_count(cells: Seq<Cell>, key: Key) -> int {
    sum(cells, |cell: Cell| if cell.key == key { cell.count@ } else { 0 })
}

pub open spec fn cell_revenue(cells: Seq<Cell>, key: Key) -> int {
    sum(cells, |cell: Cell| if cell.key == key { cell.revenue@ } else { 0 })
}

pub open spec fn db_count(db: Db, key: Key) -> int {
    sum(db.lineitem, |row: LineitemRow| if row_key(row) == key { 1 } else { 0 })
}

pub open spec fn db_revenue(db: Db, key: Key) -> int {
    sum(db.lineitem, |row: LineitemRow| if row_key(row) == key {
        row.l_extendedprice * row.l_discount
    } else { 0 })
}

pub open spec fn input_count(rows: Seq<LineitemInput>, key: Key) -> int {
    sum(rows, |row: LineitemInput| if row_key(lineitem_input_view(row)) == key { 1 } else { 0 })
}

pub open spec fn input_revenue(rows: Seq<LineitemInput>, key: Key) -> int {
    sum(rows, |row: LineitemInput| if row_key(lineitem_input_view(row)) == key {
        (row.l_extendedprice as int) * (row.l_discount as int)
    } else { 0 })
}

pub open spec fn delta_count(rows: Seq<LineitemDelta>, key: Key) -> int {
    sum(rows, |delta: LineitemDelta| if row_key(lineitem_input_view(delta.row)) == key {
        delta.sign as int
    } else { 0 })
}

pub open spec fn delta_revenue(rows: Seq<LineitemDelta>, key: Key) -> int {
    sum(rows, |delta: LineitemDelta| if row_key(lineitem_input_view(delta.row)) == key {
        (delta.sign as int) * (delta.row.l_extendedprice as int) * (delta.row.l_discount as int)
    } else { 0 })
}

pub open spec fn different_keys(left: Key, right: Key) -> bool {
    left != right
}

pub open spec fn unique(cells: Seq<Cell>) -> bool {
    forall|first: int, second: int| 0 <= first < second < cells.len() ==>
        #[trigger] different_keys(cells[first].key, cells[second].key)
}

pub open spec fn sorted(cells: Seq<Cell>) -> bool {
    forall|first: int, second: int| 0 <= first < second < cells.len() ==>
        #[trigger] key_lt(cells[first].key, cells[second].key)
}

pub open spec fn nonzero(cells: Seq<Cell>) -> bool {
    forall|index: int| 0 <= index < cells.len() ==>
        (#[trigger] cells[index]).count@ != 0 || cells[index].revenue@ != 0
}

pub struct Accumulator {
    pub cells: Vec<Cell>,
    pub positions: HashMap<i64, HashMap<u128, usize>>,
}

impl Accumulator {
    pub open spec fn wf(self) -> bool {
        &&& unique(self.cells@)
        &&& forall|key: Key| #[trigger] position_valid(self.cells@, self.positions@, key)
        &&& forall|index: int| 0 <= index < self.cells@.len() ==>
            position_for(self.positions@, (#[trigger] self.cells@[index]).key) == Some(index as usize)
    }

    pub fn new() -> (result: Self)
        ensures result.wf(), result.cells@.len() == 0,
    {
        Self { cells: Vec::new(), positions: HashMap::new() }
    }

    pub fn add(&mut self, row: LineitemInput, sign: i64)
        requires old(self).wf(),
        ensures
            final(self).wf(),
            forall|weight: spec_fn(Key) -> int, revenue: bool|
                #[trigger] weighted(final(self).cells@, weight, revenue) == weighted(old(self).cells@, weight, revenue) +
                weight(row_key(lineitem_input_view(row))) * ((sign as int) * row_measure(lineitem_input_view(row), revenue)),
    {
        let ghost original = self.cells@;
        let ghost original_positions = self.positions@;
        let key = (row.l_discount, row.l_shipdate, row.l_quantity);
        let packed_key = pack(key);
        proof {
            assert(i128::MIN <= (row.l_extendedprice as int) * (row.l_discount as int) <= i128::MAX) by(nonlinear_arith);
        }
        let product = (row.l_extendedprice as i128) * (row.l_discount as i128);
        let change_count = Wide::from_i128(sign as i128);
        let change_revenue = change_count.mul(&Wide::from_i128(product));
        let ghost row_view = lineitem_input_view(row);
        proof {
            assert(change_revenue@ == (sign as int) * row_measure(row_view, true));
            assert(change_count@ == (sign as int) * row_measure(row_view, false));
        }
        let position = match self.positions.get(&key.0) {
            Some(inner) => match inner.get(&packed_key) {
                Some(position) => Some(*position),
                None => None,
            },
            None => None,
        };
        match position {
            Some(position) => {
                proof {
                    assert(position_for(self.positions@, key) == Some(position));
                    assert(position_valid(original, original_positions, key));
                    assert(position < self.cells.len());
                }
                let ghost old_cell = original[position as int];
                let count = self.cells[position].count.add(&change_count);
                let revenue = self.cells[position].revenue.add(&change_revenue);
                self.cells.set(position, Cell { key, count, revenue });
                let ghost cell = self.cells@[position as int];
                proof {
                    assert(cell.key == key);
                    lemma_weight_update(original, position as int, cell);
                    assert forall|revenues: bool| #[trigger] measure(cell, revenues) - measure(old_cell, revenues) ==
                        (sign as int) * row_measure(row_view, revenues) by {};
                    assert forall|weight: spec_fn(Key) -> int, revenues: bool| #[trigger] weighted(self.cells@, weight, revenues) ==
                        weighted(original, weight, revenues) + weight(key) * ((sign as int) * row_measure(row_view, revenues)) by {};
                    assert(self.cells@ == original.update(position as int, cell));
                    assert forall|index: int| 0 <= index < self.cells@.len() implies
                        position_for(self.positions@, (#[trigger] self.cells@[index]).key) == Some(index as usize) by {
                        assert(self.cells@[index].key == original[index].key);
                    }
                    assert forall|other: Key| #[trigger] position_valid(self.cells@, self.positions@, other) by {
                        if let Some(index) = position_for(self.positions@, other) {
                            assert(position_valid(original, original_positions, other));
                            assert(self.cells@[index as int].key == original[index as int].key);
                        }
                    }
                }
            },
            None => {
                let position = self.cells.len();
                self.cells.push(Cell { key, count: change_count, revenue: change_revenue });
                let ghost cell = self.cells@[position as int];
                let mut inner = match self.positions.remove(&key.0) { Some(value) => value, None => HashMap::new() };
                inner.insert(packed_key, position);
                self.positions.insert(key.0, inner);
                proof {
                    assert(self.cells@ == original.push(cell));
                    lemma_weight_push(original, cell);
                    assert forall|weight: spec_fn(Key) -> int, revenues: bool| #[trigger] weighted(self.cells@, weight, revenues) ==
                        weighted(original, weight, revenues) + weight(key) * ((sign as int) * row_measure(row_view, revenues)) by {
                        assert(measure(cell, revenues) == (sign as int) * row_measure(row_view, revenues));
                    }
                    assert forall|index: int| 0 <= index < original.len() implies (#[trigger] original[index]).key != key by {
                        assert(position_for(original_positions, original[index].key) == Some(index as usize));
                    }
                    assert forall|index: int| 0 <= index < self.cells@.len() implies
                        position_for(self.positions@, (#[trigger] self.cells@[index]).key) == Some(index as usize) by {
                        if index < original.len() {
                            assert(self.cells@[index] == original[index]);
                            if original[index].key.0 == key.0 && packed(original[index].key) == packed(key) {
                                lemma_pack_injective(original[index].key, key);
                            }
                        }
                    }
                    assert forall|other: Key| #[trigger] position_valid(self.cells@, self.positions@, other) by {
                        if other.0 == key.0 && packed(other) == packed(key) { lemma_pack_injective(other, key); }
                        else {
                            assert(position_for(self.positions@, other) == position_for(original_positions, other));
                            assert(position_valid(original, original_positions, other));
                        }
                    }
                }
            },
        }
        proof {
            assert forall|first: int, second: int| 0 <= first < second < self.cells@.len() implies
                #[trigger] different_keys(self.cells@[first].key, self.cells@[second].key) by {
                assert(position_for(self.positions@, self.cells@[first].key) == Some(first as usize));
                assert(position_for(self.positions@, self.cells@[second].key) == Some(second as usize));
            }
        }
    }
}

}
