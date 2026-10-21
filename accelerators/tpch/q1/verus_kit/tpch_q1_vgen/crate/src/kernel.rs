use vstd::prelude::*;
use std::collections::HashMap;
use std::sync::Arc;
use verus_common::bytes::*;
use verus_common::rat::*;
use verus_common::sums::*;
use verus_common::order::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::groups::*;
use crate::paged::*;
use crate::values::*;
use crate::semantics::*;

verus! {

/// The kernel keeps no state that grows with the changed rows.
pub open spec fn history(s: State) -> nat { 0 }

pub type Output = (Vec<u8>, Vec<u8>, i128, i128, i128, i128,
    (i128, i128), (i128, i128), (i128, i128), i64);

pub open spec fn ancestor(leaf: int, key: u64) -> bool
    decreases leaf,
{
    if leaf <= 0 { false }
    else if leaf == key as int { true }
    else if leaf < key as int { false }
    else { ancestor(leaf / 2, key) }
}

pub open spec fn covers(key: u64, day: int) -> bool {
    -2147483648 <= day <= 2147483647 && 1 <= key < 8589934592
        && ancestor(4294967296 + day + 2147483648, key)
}

pub open spec fn interval_sum(rows: Seq<LineitemRow>, key: u64,
    flag: Seq<u8>, status: Seq<u8>, component: int) -> int {
    sum(rows, interval_weight(key, flag, status, component))
}

pub open spec fn interval_weight(key: u64, flag: Seq<u8>, status: Seq<u8>, component: int) -> spec_fn(LineitemRow) -> int {
    |row: LineitemRow| if covers(key, row.l_shipdate)
        && row.l_returnflag == flag && row.l_linestatus == status { weight(row, component) } else { 0 }
}

pub open spec fn row_leaf(row: LineitemRow) -> int {
    4294967296 + row.l_shipdate + 2147483648
}

pub open spec fn leaf_row_weight(leaf: u64, flag: Seq<u8>, status: Seq<u8>, component: int)
    -> spec_fn(LineitemRow) -> int {
    |row: LineitemRow| if row_leaf(row) == leaf as int
        && row.l_returnflag == flag && row.l_linestatus == status { weight(row, component) } else { 0 }
}

pub open spec fn leaf_cross(leaf: u64, row: LineitemRow, key: u64,
    flag: Seq<u8>, status: Seq<u8>, component: int) -> int {
    if ancestor(leaf as int, key) {
        leaf_row_weight(leaf, flag, status, component)(row)
    } else { 0 }
}

pub open spec fn interval_change(delta: Seq<(LineitemRow, int)>, key: u64,
    flag: Seq<u8>, status: Seq<u8>, component: int) -> int {
    sum(delta, |item: (LineitemRow, int)| item.1 * interval_weight(key, flag, status, component)(item.0))
}

pub open spec fn leaf_delta_cross(leaf: u64, item: (LineitemRow, int), key: u64,
    flag: Seq<u8>, status: Seq<u8>, component: int) -> int {
    if ancestor(leaf as int, key) {
        item.1 * leaf_row_weight(leaf, flag, status, component)(item.0)
    } else { 0 }
}

pub open spec fn power2(exponent: int) -> int
{
    if exponent <= 0 { 1 } else if exponent == 1 { 2 } else if exponent == 2 { 4 } else if exponent == 3 { 8 } else if exponent == 4 { 16 } else if exponent == 5 { 32 } else if exponent == 6 { 64 } else if exponent == 7 { 128 } else if exponent == 8 { 256 } else if exponent == 9 { 512 } else if exponent == 10 { 1024 } else if exponent == 11 { 2048 } else if exponent == 12 { 4096 } else if exponent == 13 { 8192 } else if exponent == 14 { 16384 } else if exponent == 15 { 32768 } else if exponent == 16 { 65536 } else if exponent == 17 { 131072 } else if exponent == 18 { 262144 } else if exponent == 19 { 524288 } else if exponent == 20 { 1048576 } else if exponent == 21 { 2097152 } else if exponent == 22 { 4194304 } else if exponent == 23 { 8388608 } else if exponent == 24 { 16777216 } else if exponent == 25 { 33554432 } else if exponent == 26 { 67108864 } else if exponent == 27 { 134217728 } else if exponent == 28 { 268435456 } else if exponent == 29 { 536870912 } else if exponent == 30 { 1073741824 } else if exponent == 31 { 2147483648 } else { 4294967296 }
}

pub struct Tile {
    pub level: int,
    pub start: int,
    pub width: int,
    pub key: int,
}

pub open spec fn tile_ok(tile: Tile) -> bool {
    0 <= tile.level <= 32
        && tile.width == power2(tile.level)
        && 0 <= tile.start && tile.start + tile.width <= 4294967296
        && tile.start % tile.width == 0
        && tile.key == power2(32 - tile.level) + tile.start / tile.width
        && 1 <= tile.key < 8589934592
}

pub open spec fn tile_contains(tile: Tile, offset: int) -> bool {
    tile.start <= offset < tile.start + tile.width
}

pub open spec fn tile_prefix(tiles: Seq<Tile>) -> bool {
    &&& forall|index: int| 0 <= index < tiles.len() ==>
        #[trigger] tiles[index].start == sum(tiles.take(index), |tile: Tile| tile.width as int)
    &&& forall|index: int| 0 <= index < tiles.len() ==> tile_ok(#[trigger] tiles[index])
}

pub open spec fn tile_sum(tiles: Seq<Tile>) -> int {
    sum(tiles, |tile: Tile| tile.width as int)
}

pub open spec fn tile_row_weight(tile: Tile, flag: Seq<u8>, status: Seq<u8>, component: int)
    -> spec_fn(LineitemRow) -> int {
    |row: LineitemRow| if tile_contains(tile, row_leaf(row) - 4294967296)
        && row.l_returnflag == flag && row.l_linestatus == status { weight(row, component) } else { 0 }
}

pub open spec fn prefix_bound_weight(end: int, flag: Seq<u8>, status: Seq<u8>, component: int)
    -> spec_fn(LineitemRow) -> int {
    |row: LineitemRow| if 0 <= row_leaf(row) - 4294967296 < end
        && row.l_returnflag == flag && row.l_linestatus == status { weight(row, component) } else { 0 }
}

proof fn lemma_power2_double(exponent: int)
    requires 0 <= exponent < 32,
    ensures power2(exponent + 1) == 2 * power2(exponent),
{
    assert(exponent == 0 || exponent == 1 || exponent == 2 || exponent == 3 || exponent >= 4);
}

proof fn lemma_power2_add(left: int, right: int)
    requires 0 <= left <= 32, 0 <= right, left + right <= 32,
    ensures power2(left + right) == power2(left) * power2(right),
    decreases left,
{
    if left == 0 {
        assert(power2(right) == 1 * power2(right));
    } else {
        lemma_power2_add(left - 1, right);
        lemma_power2_double(left - 1);
        lemma_power2_double(left - 1 + right);
        assert(power2(left) == 2 * power2(left - 1));
        assert(power2(left + right) == 2 * power2(left - 1 + right));
    }
}

proof fn lemma_div_twice(node: int, divisor: int)
    requires node >= 0, divisor > 0,
    ensures (node / 2) / divisor == node / (2 * divisor),
{
    vstd::arithmetic::div_mod::lemma_div_denominator(node, 2, divisor);
}

proof fn lemma_half_alignment(start: int, width: int)
    requires width > 0, start >= 0, start % (2 * width) == 0,
    ensures start % width == 0, (start + width) % width == 0,
{
    let quotient = start / (2 * width);
    assert(start == quotient * (2 * width)) by(nonlinear_arith)
        requires width > 0, start % (2 * width) == 0,
            quotient == start / (2 * width);
    assert(start == (2 * quotient) * width) by(nonlinear_arith)
        requires start == quotient * (2 * width);
    assert(start + width == (2 * quotient + 1) * width) by(nonlinear_arith)
        requires start == (2 * quotient) * width;
    vstd::arithmetic::div_mod::lemma_mod_multiples_basic(2 * quotient, width);
    vstd::arithmetic::div_mod::lemma_mod_multiples_basic(2 * quotient + 1, width);
}

proof fn lemma_ancestor_depth(node: int, key: u64, base: int, depth: int)
    requires 0 <= depth <= 32, base > 0,
        base <= (key as int) < 2 * base,
        base * power2(depth) <= node < 2 * base * power2(depth),
    ensures ancestor(node, key) <==> node / power2(depth) == key as int,
    decreases depth,
{
    if depth == 0 {
        reveal_with_fuel(ancestor, 2);
        assert(node / 2 < base);
    } else {
        let half = power2(depth - 1);
        assert(power2(depth) == 2 * half);
        assert(base * half <= node / 2 < 2 * base * half) by(nonlinear_arith)
            requires base > 0, half > 0,
                base * (2 * half) <= node < 2 * base * (2 * half);
        lemma_ancestor_depth(node / 2, key, base, depth - 1);
        lemma_div_twice(node, half);
        assert(node >= 2 * base) by(nonlinear_arith)
            requires base > 0, half >= 1, node >= base * (2 * half);
        lemma_ancestor_step(node, key);
    }
}

proof fn lemma_aligned_range(offset: int, start: int, width: int, base: int)
    requires offset >= 0, start >= 0, width > 0, start % width == 0,
    ensures (base * width + offset) / width == base + start / width
        <==> start <= offset < start + width,
{
    assert(start == (start / width) * width) by(nonlinear_arith)
        requires start >= 0, width > 0, start % width == 0;
    assert((base * width + offset) / width == base + start / width
        <==> start <= offset < start + width) by(nonlinear_arith)
        requires width > 0, start == (start / width) * width;
}

proof fn lemma_tile_indicator(tiles: Seq<Tile>, offset: int)
    requires tile_prefix(tiles), 0 <= offset,
    ensures sum(tiles, |tile: Tile| if tile_contains(tile, offset) { 1int } else { 0int })
        == if offset < tile_sum(tiles) { 1int } else { 0int },
    decreases tiles.len(),
{
    if tiles.len() == 0 {
        lemma_sum_empty(tiles, |tile: Tile| if tile_contains(tile, offset) { 1 } else { 0 });
    } else {
        let prefix = tiles.drop_last();
        let last = tiles.last();
        assert forall|index: int| 0 <= index < prefix.len() implies
            #[trigger] prefix[index].start
                == sum(prefix.take(index), |tile: Tile| tile.width as int)
                && tile_ok(prefix[index]) by {
            assert(prefix[index] == tiles[index]);
            assert(prefix.take(index) =~= tiles.take(index));
        }
        assert(tiles[tiles.len() - 1] == last);
        assert(tiles.take(tiles.len() as int - 1) =~= prefix);
        assert(last.start as int == tile_sum(prefix));
        assert(tile_sum(tiles) == tile_sum(prefix) + last.width as int);
        lemma_tile_indicator(prefix, offset);
        let f = |tile: Tile| if tile_contains(tile, offset) { 1int } else { 0int };
        lemma_sum_push(prefix, last, f);
        if offset < tile_sum(prefix) {
            assert(!tile_contains(last, offset));
        } else if offset < tile_sum(tiles) {
            assert(tile_contains(last, offset));
        } else {
            assert(!tile_contains(last, offset));
        }
    }
}

proof fn lemma_tile_prefix_push(tiles: Seq<Tile>, tile: Tile)
    requires tile_prefix(tiles), tile_ok(tile), tile.start == tile_sum(tiles),
    ensures tile_prefix(tiles.push(tile)),
        tile_sum(tiles.push(tile)) == tile_sum(tiles) + tile.width,
{
    let extended = tiles.push(tile);
    assert forall|index: int| 0 <= index < extended.len() implies
        #[trigger] extended[index].start
            == sum(extended.take(index), |item: Tile| item.width as int)
            && tile_ok(extended[index]) by {
        if index < tiles.len() {
            assert(extended[index] == tiles[index]);
            assert(extended.take(index) =~= tiles.take(index));
            assert(tiles[index].start == sum(tiles.take(index), |item: Tile| item.width as int)
                && tile_ok(tiles[index]));
        } else {
            assert(index == tiles.len() as int);
            assert(extended[index] == tile);
            assert(extended.take(index) =~= tiles);
        }
    }
    lemma_sum_push(tiles, tile, |item: Tile| item.width as int);
}

proof fn lemma_tile_interval_weight(tile: Tile, row: LineitemRow,
    flag: Seq<u8>, status: Seq<u8>, component: int)
    requires tile_ok(tile), -2147483648 <= row.l_shipdate <= 2147483647,
    ensures interval_weight(tile.key as u64, flag, status, component)(row)
        == tile_row_weight(tile, flag, status, component)(row),
{
    let offset = row_leaf(row) - 4294967296;
    lemma_covers_ancestor(row.l_shipdate as int, tile.key as u64);
    let base = power2(32 - tile.level);
    lemma_power2_add(32 - tile.level, tile.level);
    assert(base * tile.width == 4294967296);
    assert(4294967296 <= row_leaf(row) < 8589934592);
    assert(base * power2(tile.level) <= row_leaf(row)
        < 2 * base * power2(tile.level)) by(nonlinear_arith)
        requires base * power2(tile.level) == 4294967296,
            4294967296 <= row_leaf(row) < 8589934592;
    assert(base <= tile.key < 2 * base) by(nonlinear_arith)
        requires base > 0, tile.width > 0, 0 <= tile.start,
            tile.start + tile.width <= 4294967296,
            tile.key == base + tile.start / tile.width,
            base * tile.width == 4294967296;
    lemma_ancestor_depth(row_leaf(row), tile.key as u64, base, tile.level);
    lemma_aligned_range(offset, tile.start, tile.width, base);
}

proof fn lemma_tile_prefix_at(tiles: Seq<Tile>, index: int)
    requires tile_prefix(tiles), 0 <= index < tiles.len(),
    ensures tiles[index].start == sum(tiles.take(index), |tile: Tile| tile.width as int), tile_ok(tiles[index]),
{
    assert(tiles[index].start == tiles[index].start);
}

proof fn lemma_tile_row_sum(tiles: Seq<Tile>, row: LineitemRow,
    flag: Seq<u8>, status: Seq<u8>, component: int)
    requires tile_prefix(tiles), -2147483648 <= row.l_shipdate <= 2147483647,
    ensures sum(tiles, |tile: Tile| interval_weight(tile.key as u64, flag, status, component)(row))
        == prefix_bound_weight(tile_sum(tiles), flag, status, component)(row),
{
    let tile_weight = |tile: Tile| tile_row_weight(tile, flag, status, component)(row);
    let interval = |tile: Tile| interval_weight(tile.key as u64, flag, status, component)(row);
    assert forall|index: int| 0 <= index < tiles.len() implies
        #[trigger] interval(tiles[index]) == tile_weight(tiles[index]) by {
        let tile = tiles[index];
        lemma_tile_prefix_at(tiles, index);
        lemma_tile_interval_weight(tile, row, flag, status, component);
    }
    lemma_sum_ext(tiles, interval, tile_weight);
    let wanted = if row.l_returnflag == flag && row.l_linestatus == status {
        weight(row, component)
    } else { 0 };
    let indicator = |tile: Tile| if tile_contains(tile, row_leaf(row) - 4294967296) { 1int } else { 0int };
    assert forall|tile: Tile| #[trigger] tile_weight(tile)
        == if tile_contains(tile, row_leaf(row) - 4294967296) { wanted } else { 0 } by {}
    lemma_sum_ext_all(tiles, tile_weight,
        |tile: Tile| if tile_contains(tile, row_leaf(row) - 4294967296) { wanted } else { 0 });
    lemma_tile_indicator(tiles, row_leaf(row) - 4294967296);
    assert forall|tile: Tile| #[trigger] tile_weight(tile) == wanted * indicator(tile) by {}
    lemma_sum_scale(tiles, indicator, tile_weight, wanted);
    assert(sum(tiles, |tile: Tile| if tile_contains(tile, row_leaf(row) - 4294967296) { wanted } else { 0 })
        == if row_leaf(row) - 4294967296 < tile_sum(tiles) { wanted } else { 0 });
}

proof fn lemma_selected_interval_sum(rows: Seq<LineitemRow>, tiles: Seq<Tile>,
    flag: Seq<u8>, status: Seq<u8>, component: int)
    requires dates_ok(rows), tile_prefix(tiles),
    ensures sum(tiles, |tile: Tile| interval_sum(rows, tile.key as u64, flag, status, component))
        == sum(rows, prefix_bound_weight(tile_sum(tiles), flag, status, component)),
{
    let cross = |tile: Tile, row: LineitemRow|
        interval_weight(tile.key as u64, flag, status, component)(row);
    let tile_sum_fn = |tile: Tile| interval_sum(rows, tile.key as u64, flag, status, component);
    let row_sum_fn = |row: LineitemRow| sum(tiles, |tile: Tile| cross(tile, row));
    assert forall|tile: Tile| #[trigger] tile_sum_fn(tile)
        == sum(rows, |row: LineitemRow| cross(tile, row)) by {
        lemma_sum_ext_all(rows, interval_weight(tile.key as u64, flag, status, component),
            |row: LineitemRow| cross(tile, row));
    }
    assert forall|row: LineitemRow| #[trigger] row_sum_fn(row)
        == sum(tiles, |tile: Tile| cross(tile, row)) by {}
    lemma_sum_swap(tiles, rows, cross, tile_sum_fn, row_sum_fn);
    assert forall|row: LineitemRow| rows.contains(row) implies
        #[trigger] row_sum_fn(row) == prefix_bound_weight(tile_sum(tiles), flag, status, component)(row) by {
        let index = choose|index: int| 0 <= index < rows.len() && #[trigger] rows[index] == row;
        lemma_tile_row_sum(tiles, row, flag, status, component);
        lemma_sum_ext_all(tiles, |tile: Tile| cross(tile, row),
            |tile: Tile| interval_weight(tile.key as u64, flag, status, component)(row));
    }
    lemma_sum_ext(rows, row_sum_fn,
        prefix_bound_weight(tile_sum(tiles), flag, status, component));
}

fn power2_exec(exponent: u8) -> (result: u64)
    requires exponent <= 32,
    ensures result as int == power2(exponent as int), result > 0,
{
    let mut result = 1u64;
    let mut index = 0u8;
    while index < exponent
        invariant index <= exponent, result as int == power2(index as int), result > 0,
        decreases exponent - index,
    {
        match index {
            0 => result = 2, 1 => result = 4, 2 => result = 8, 3 => result = 16, 4 => result = 32,
            5 => result = 64, 6 => result = 128, 7 => result = 256, 8 => result = 512, 9 => result = 1024,
            10 => result = 2048, 11 => result = 4096, 12 => result = 8192, 13 => result = 16384,
            14 => result = 32768, 15 => result = 65536, 16 => result = 131072, 17 => result = 262144,
            18 => result = 524288, 19 => result = 1048576, 20 => result = 2097152, 21 => result = 4194304,
            22 => result = 8388608, 23 => result = 16777216, 24 => result = 33554432, 25 => result = 67108864,
            26 => result = 134217728, 27 => result = 268435456, 28 => result = 536870912, 29 => result = 1073741824,
            30 => result = 2147483648, _ => result = 4294967296,
        }
        index += 1;
    }
    result
}

proof fn lemma_leaf(day: int, key: u64)
    requires -2147483648 <= day <= 2147483647, key >= 4294967296,
    ensures covers(key, day) <==> key as int == 4294967296 + day + 2147483648,
{
    reveal_with_fuel(ancestor, 2);
}

proof fn lemma_ancestor_bound(node: int, key: u64)
    requires node > 0, ancestor(node, key),
    ensures 0 < key as int <= node,
    decreases node,
{
    reveal_with_fuel(ancestor, 2);
    if node != key as int { lemma_ancestor_bound(node / 2, key); }
}

proof fn lemma_covers_ancestor(day: int, key: u64)
    requires -2147483648 <= day <= 2147483647,
    ensures covers(key, day) <==> ancestor(4294967296 + day + 2147483648, key),
{
    let node = 4294967296 + day + 2147483648;
    assert(node == 4294967296 + day + 2147483648);
    assert(4294967296 <= node);
    assert(node <= 8589934591);
    if ancestor(node, key) {
        lemma_ancestor_bound(node, key);
    }
    reveal_with_fuel(ancestor, 1);
}

proof fn lemma_leaf_row_weight(leaf: u64, row: LineitemRow, flag: Seq<u8>, status: Seq<u8>, component: int)
    requires leaf >= 4294967296, -2147483648 <= row.l_shipdate <= 2147483647,
    ensures interval_weight(leaf, flag, status, component)(row)
        == leaf_row_weight(leaf, flag, status, component)(row),
{
    lemma_leaf(row.l_shipdate as int, leaf);
}

proof fn lemma_leaf_interval_change(delta: Seq<(LineitemRow, int)>, leaf: u64,
    flag: Seq<u8>, status: Seq<u8>, component: int)
    requires leaf >= 4294967296, 0 <= component < 6,
        forall|index: int| 0 <= index < delta.len() ==>
            -2147483648 <= #[trigger] delta[index].0.l_shipdate <= 2147483647,
    ensures interval_change(delta, leaf, flag, status, component)
        == sum(delta, |item: (LineitemRow, int)|
            item.1 * leaf_row_weight(leaf, flag, status, component)(item.0)),
{
    let left = |item: (LineitemRow, int)| item.1 * interval_weight(leaf, flag, status, component)(item.0);
    let right = |item: (LineitemRow, int)| item.1 * leaf_row_weight(leaf, flag, status, component)(item.0);
    assert forall|index: int| 0 <= index < delta.len() implies #[trigger] left(delta[index]) == right(delta[index]) by {
        lemma_leaf_row_weight(leaf, delta[index].0, flag, status, component);
    }
    lemma_sum_ext(delta, left, right);
}

proof fn lemma_leaf_count_support(rows: Seq<LineitemRow>, index: int, leaf: u64,
    flag: Seq<u8>, status: Seq<u8>)
    requires 0 <= index < rows.len(), row_leaf(rows[index]) == leaf as int,
        -2147483648 <= rows[index].l_shipdate <= 2147483647,
        rows[index].l_returnflag == flag, rows[index].l_linestatus == status,
    ensures interval_sum(rows, leaf, flag, status, 0) >= 1,
{
    let f = interval_weight(leaf, flag, status, 0);
    lemma_leaf(rows[index].l_shipdate as int, leaf);
    assert(f(rows[index]) == 1);
    lemma_sum_ge_term(rows, f, index);
}

proof fn lemma_directory_interval_sum(rows: Seq<LineitemRow>, keys: Seq<u64>,
    groups: Map<u64, FlatGroups>, key: u64, flag: Seq<u8>, status: Seq<u8>, component: int)
    requires 0 <= component < 6,
        forall|index: int| 0 <= index < rows.len() ==>
            -2147483648 <= #[trigger] rows[index].l_shipdate <= 2147483647,
        forall|index: int| 0 <= index < rows.len() &&
            rows[index].l_returnflag == flag && rows[index].l_linestatus == status ==>
            #[trigger] keys.contains((row_leaf(rows[index])) as u64),
        forall|index: int| 0 <= index < keys.len() ==> #[trigger] keys[index] >= 4294967296,
        forall|first: int, second: int| 0 <= first < second < keys.len() ==>
            #[trigger] keys[first] != #[trigger] keys[second],
        forall|leaf: u64| #[trigger] groups.contains_key(leaf) <==> keys.contains(leaf),
        forall|leaf: u64| #[trigger] groups.contains_key(leaf) ==>
            #[trigger] groups[leaf].value(flag, status, component)
                == interval_sum(rows, leaf, flag, status, component),
    ensures sum(keys, |leaf: u64| if ancestor(leaf as int, key) {
            if groups.contains_key(leaf) { groups[leaf].value(flag, status, component) } else { 0 }
        } else { 0 }) == interval_sum(rows, key, flag, status, component),
{
    let leaf_sum = |leaf: u64| if ancestor(leaf as int, key) {
        if groups.contains_key(leaf) { groups[leaf].value(flag, status, component) } else { 0 }
    } else { 0 };
    let cross = |leaf: u64, row: LineitemRow| leaf_cross(leaf, row, key, flag, status, component);
    let row_weight = interval_weight(key, flag, status, component);
    assert forall|leaf: u64| #[trigger] leaf_sum(leaf) == sum(rows, |row: LineitemRow| cross(leaf, row)) by {
        if groups.contains_key(leaf) {
            assert(groups[leaf].value(flag, status, component)
                == interval_sum(rows, leaf, flag, status, component));
            if ancestor(leaf as int, key) {
                assert forall|index: int| 0 <= index < rows.len() implies
                    #[trigger] interval_weight(leaf, flag, status, component)(rows[index])
                        == leaf_row_weight(leaf, flag, status, component)(rows[index]) by {
                    lemma_leaf_row_weight(leaf, rows[index], flag, status, component);
                }
                lemma_sum_ext(rows, interval_weight(leaf, flag, status, component),
                    leaf_row_weight(leaf, flag, status, component));
                assert forall|row: LineitemRow| #[trigger] leaf_cross(leaf, row, key, flag, status, component)
                    == leaf_row_weight(leaf, flag, status, component)(row) by {}
                lemma_sum_ext_all(rows, |row: LineitemRow| cross(leaf, row),
                    leaf_row_weight(leaf, flag, status, component));
            } else {
                assert forall|index: int| 0 <= index < rows.len() implies
                    #[trigger] cross(leaf, rows[index]) == 0 by {}
                lemma_sum_ext(rows, |row: LineitemRow| cross(leaf, row), |row: LineitemRow| 0);
                lemma_sum_zero(rows, |row: LineitemRow| 0);
            }
        } else {
            assert forall|index: int| 0 <= index < rows.len() implies
                #[trigger] cross(leaf, rows[index]) == 0 by {
                if cross(leaf, rows[index]) != 0 {
                    assert(leaf_cross(leaf, rows[index], key, flag, status, component) != 0);
                    assert(row_leaf(rows[index]) == leaf as int);
                    assert(rows[index].l_returnflag == flag && rows[index].l_linestatus == status);
                    assert(4294967296 <= row_leaf(rows[index]) <= 8589934591) by (nonlinear_arith)
                        requires -2147483648 <= rows[index].l_shipdate <= 2147483647;
                    assert((row_leaf(rows[index]) as u64) as int == row_leaf(rows[index])) by (nonlinear_arith)
                        requires 4294967296 <= row_leaf(rows[index]) <= 8589934591;
                    assert((row_leaf(rows[index])) as u64 == leaf);
                    assert(keys.contains((row_leaf(rows[index])) as u64));
                    assert(groups.contains_key(leaf));
                }
            }
            lemma_sum_ext(rows, |row: LineitemRow| cross(leaf, row), |row: LineitemRow| 0);
            lemma_sum_zero(rows, |row: LineitemRow| 0);
        }
    }
    let row_sum = |row: LineitemRow| sum(keys, |leaf: u64| cross(leaf, row));
    assert forall|row: LineitemRow| rows.contains(row) implies
        #[trigger] row_sum(row) == row_weight(row) by {
            let index = choose|index: int| 0 <= index < rows.len() && #[trigger] rows[index] == row;
            let wanted = if row.l_returnflag == flag && row.l_linestatus == status {
                weight(row, component)
            } else { 0 };
            let indicator_value = if ancestor(row_leaf(row), key) { wanted } else { 0 };
            let indicator = |leaf: u64| if leaf as int == row_leaf(row) {
                indicator_value
            } else { 0 };
            assert(4294967296 <= row_leaf(row) <= 8589934591) by (nonlinear_arith)
                requires -2147483648 <= row.l_shipdate <= 2147483647;
            assert((row_leaf(row) as u64) as int == row_leaf(row)) by (nonlinear_arith)
                requires 4294967296 <= row_leaf(row) <= 8589934591;
            lemma_covers_ancestor(row.l_shipdate as int, key);
            assert(row_weight(row) == if ancestor(row_leaf(row), key) { wanted } else { 0 });
            if wanted != 0 {
                assert(keys.contains((row_leaf(row)) as u64));
            }
            lemma_sum_indicator_distinct(keys, (row_leaf(row)) as u64, indicator_value, indicator);
            assert forall|leaf: u64| #[trigger] cross(leaf, row) == indicator(leaf) by {
                if leaf as int == row_leaf(row) {
                    lemma_leaf_row_weight(leaf, row, flag, status, component);
                } else {
                    assert(leaf_cross(leaf, row, key, flag, status, component) == 0);
                }
            }
            lemma_sum_ext_all(keys, |leaf: u64| cross(leaf, row), indicator);
    }
    lemma_sum_swap(keys, rows, cross, leaf_sum, row_sum);
    lemma_sum_ext(rows, row_sum, row_weight);
}

proof fn lemma_directory_interval_change(delta: Seq<(LineitemRow, int)>, keys: Seq<u64>,
    groups: Map<u64, FlatGroups>, key: u64, flag: Seq<u8>, status: Seq<u8>, component: int)
    requires 0 <= component < 6,
        forall|index: int| 0 <= index < delta.len() ==>
            -2147483648 <= #[trigger] delta[index].0.l_shipdate <= 2147483647,
        forall|index: int| 0 <= index < delta.len() && delta[index].1 != 0 ==>
            #[trigger] keys.contains((row_leaf(delta[index].0)) as u64),
        forall|index: int| 0 <= index < keys.len() ==> #[trigger] keys[index] >= 4294967296,
        forall|first: int, second: int| 0 <= first < second < keys.len() ==>
            #[trigger] keys[first] != #[trigger] keys[second],
        forall|leaf: u64| #[trigger] groups.contains_key(leaf) <==> keys.contains(leaf),
        forall|leaf: u64| #[trigger] groups.contains_key(leaf) ==>
            #[trigger] groups[leaf].value(flag, status, component)
                == sum(delta, |item: (LineitemRow, int)|
                    item.1 * leaf_row_weight(leaf, flag, status, component)(item.0)),
    ensures sum(keys, |leaf: u64| if ancestor(leaf as int, key) {
            if groups.contains_key(leaf) { groups[leaf].value(flag, status, component) } else { 0 }
        } else { 0 }) == interval_change(delta, key, flag, status, component),
{
    let leaf_sum = |leaf: u64| if ancestor(leaf as int, key) {
        if groups.contains_key(leaf) { groups[leaf].value(flag, status, component) } else { 0 }
    } else { 0 };
    let cross = |leaf: u64, item: (LineitemRow, int)|
        leaf_delta_cross(leaf, item, key, flag, status, component);
    let row_sum = |item: (LineitemRow, int)| sum(keys, |leaf: u64| cross(leaf, item));
    assert forall|leaf: u64| #[trigger] leaf_sum(leaf) == sum(delta, |item: (LineitemRow, int)| cross(leaf, item)) by {
        if groups.contains_key(leaf) {
            assert(groups[leaf].value(flag, status, component)
                == sum(delta, |item: (LineitemRow, int)| item.1 * leaf_row_weight(leaf, flag, status, component)(item.0)));
            if ancestor(leaf as int, key) {
                assert forall|index: int| 0 <= index < delta.len() implies
                    #[trigger] cross(leaf, delta[index]) ==
                        delta[index].1 * leaf_row_weight(leaf, flag, status, component)(delta[index].0) by {}
                lemma_sum_ext(delta, |item: (LineitemRow, int)| cross(leaf, item),
                    |item: (LineitemRow, int)| item.1 * leaf_row_weight(leaf, flag, status, component)(item.0));
            } else {
                lemma_sum_zero(delta, |item: (LineitemRow, int)| cross(leaf, item));
            }
        } else {
            assert forall|index: int| 0 <= index < delta.len() implies
                #[trigger] cross(leaf, delta[index]) == 0 by {
                if cross(leaf, delta[index]) != 0 {
                    assert(delta[index].1 != 0);
                    assert(keys.contains((row_leaf(delta[index].0)) as u64));
                    assert(groups.contains_key(leaf));
                }
            }
            lemma_sum_zero(delta, |item: (LineitemRow, int)| cross(leaf, item));
        }
    }
    assert forall|item: (LineitemRow, int)| #[trigger] row_sum(item) == sum(keys, |leaf: u64| cross(leaf, item)) by {}
    lemma_sum_swap(keys, delta, cross, leaf_sum, row_sum);
    assert forall|item: (LineitemRow, int)| delta.contains(item) implies
        #[trigger] row_sum(item)
            == item.1 * interval_weight(key, flag, status, component)(item.0) by {
        let item_index = choose|index: int| 0 <= index < delta.len() && #[trigger] delta[index] == item;
        assert(-2147483648 <= item.0.l_shipdate <= 2147483647);
        let wanted = item.1 * leaf_row_weight((row_leaf(item.0)) as u64, flag, status, component)(item.0);
        let indicator = |leaf: u64| if leaf as int == row_leaf(item.0) {
            if ancestor(row_leaf(item.0), key) { wanted } else { 0 }
        } else { 0 };
        lemma_covers_ancestor(item.0.l_shipdate as int, key);
        if wanted != 0 {
            assert(item.1 != 0);
            assert(keys.contains((row_leaf(item.0)) as u64));
        }
        lemma_sum_indicator_distinct(keys, (row_leaf(item.0)) as u64,
            if ancestor(row_leaf(item.0), key) { wanted } else { 0 }, indicator);
        assert forall|leaf: u64| #[trigger] cross(leaf, item) == indicator(leaf) by {
            if leaf as int == row_leaf(item.0) {
                lemma_leaf_row_weight(leaf, item.0, flag, status, component);
            }
        }
        lemma_sum_ext_all(keys, |leaf: u64| cross(leaf, item), indicator);
    }
    lemma_sum_ext(delta, row_sum,
        |item: (LineitemRow, int)| item.1 * interval_weight(key, flag, status, component)(item.0));
}

proof fn lemma_scan_prefix(rows: Seq<LineitemInput>, index: int, key: u64, flag: Seq<u8>, status: Seq<u8>, component: int)
    requires 0 <= index < rows.len(),
    ensures interval_sum(lineitem_input_seq_view(rows.take(index + 1)), key, flag, status, component)
        == interval_sum(lineitem_input_seq_view(rows.take(index)), key, flag, status, component)
            + interval_weight(key, flag, status, component)(lineitem_input_view(rows[index])),
{
    assert(lineitem_input_seq_view(rows.take(index + 1)) =~=
        lineitem_input_seq_view(rows.take(index)).push(lineitem_input_view(rows[index])));
    lemma_sum_push(lineitem_input_seq_view(rows.take(index)), lineitem_input_view(rows[index]), interval_weight(key, flag, status, component));
}

proof fn lemma_delta_prefix(rows: Seq<LineitemDelta>, index: int, key: u64, flag: Seq<u8>, status: Seq<u8>, component: int)
    requires 0 <= index < rows.len(),
    ensures interval_change(lineitem_delta_seq_view(rows.take(index + 1)), key, flag, status, component)
        == interval_change(lineitem_delta_seq_view(rows.take(index)), key, flag, status, component)
            + rows[index].sign * interval_weight(key, flag, status, component)(lineitem_input_view(rows[index].row)),
{
    let projected = (lineitem_input_view(rows[index].row), rows[index].sign as int);
    assert(lineitem_delta_seq_view(rows.take(index + 1)) =~= lineitem_delta_seq_view(rows.take(index)).push(projected));
    lemma_sum_push(lineitem_delta_seq_view(rows.take(index)), projected,
        |item: (LineitemRow, int)| item.1 * interval_weight(key, flag, status, component)(item.0));
}

proof fn lemma_ancestor_step(node: int, key: u64)
    requires node > 0,
    ensures ancestor(node, key) <==> node == key as int || ancestor(node / 2, key),
{
    reveal_with_fuel(ancestor, 2);
}

proof fn lemma_ancestor_trans(node: int, middle: u64, key: u64)
    requires ancestor(node, middle), ancestor(middle as int, key),
    ensures ancestor(node, key),
    decreases node,
{
    if node != middle as int { lemma_ancestor_trans(node / 2, middle, key); }
}

pub struct Interval {
    key: u64,
    groups: Groups,
}

pub struct State {
    intervals: Paged<Interval>,
    database: Ghost<Db>,
}

closed spec fn at_value(intervals: Paged<Interval>, key: u64, flag: Seq<u8>, status: Seq<u8>, component: int) -> int {
    match intervals.at(key) { Some(interval) => interval.groups.value(flag, status, component), None => 0 }
}

closed spec fn directory_valid(intervals: Paged<Interval>) -> bool {
    &&& intervals.wf()
    &&& forall|key: u64| #[trigger] intervals.at(key) matches Some(interval) ==>
        interval.key == key && 1 <= key < 8589934592 && interval.groups.wf()
}

closed spec fn state_model(intervals: Paged<Interval>, db: Db) -> bool {
    &&& directory_valid(intervals)
    &&& dates_ok(db.lineitem)
    &&& forall|key: u64, flag: Seq<u8>, status: Seq<u8>, component: int|
        0 <= component < 6 ==> #[trigger] at_value(intervals, key, flag, status, component)
            == interval_sum(db.lineitem, key, flag, status, component)
}

proof fn lemma_tiles_query(intervals: Paged<Interval>, db: Db, tiles: Seq<Tile>, cutoff: int,
    flag: Seq<u8>, status: Seq<u8>, component: int)
    requires state_model(intervals, db), tile_prefix(tiles), 0 <= component < 6,
        cutoff >= -2147483648,
        tile_sum(tiles) == if cutoff >= 2147483647 { 4294967296int }
            else { cutoff + 2147483649 },
    ensures sum(tiles, |tile: Tile| at_value(intervals, tile.key as u64, flag, status, component))
        == prefix_sum(db, cutoff, flag, status, component),
{
    let lookup = |tile: Tile| at_value(intervals, tile.key as u64, flag, status, component);
    let expected = |tile: Tile| interval_sum(db.lineitem, tile.key as u64, flag, status, component);
    assert forall|tile: Tile| #[trigger] lookup(tile) == expected(tile) by {}
    lemma_sum_ext_all(tiles, lookup, expected);
    lemma_selected_interval_sum(db.lineitem, tiles, flag, status, component);
    let bounded = prefix_bound_weight(tile_sum(tiles), flag, status, component);
    let wanted = prefix_weight(cutoff, flag, status, component);
    assert forall|index: int| 0 <= index < db.lineitem.len() implies
        #[trigger] bounded(db.lineitem[index]) == wanted(db.lineitem[index]) by {
        let row = db.lineitem[index];
        assert(-2147483648 <= row.l_shipdate <= 2147483647);
    }
    lemma_sum_ext(db.lineitem, bounded, wanted);
}

proof fn lemma_build_invariant(intervals: Paged<Interval>, rows: Seq<LineitemInput>, db: Db)
    requires directory_valid(intervals), scan_lineitem_build_ok(db, rows),
        forall|key: u64, flag: Seq<u8>, status: Seq<u8>, component: int| 0 <= component < 6 ==>
            #[trigger] at_value(intervals, key, flag, status, component)
                == interval_sum(lineitem_input_seq_view(rows), key, flag, status, component),
    ensures state_model(intervals, db),
{
    lemma_scan_dates(rows, db);
    assert forall|key: u64, flag: Seq<u8>, status: Seq<u8>, component: int| 0 <= component < 6 implies
        #[trigger] at_value(intervals, key, flag, status, component) == interval_sum(db.lineitem, key, flag, status, component) by {
        let f = interval_weight(key, flag, status, component);
        assert(sum(lineitem_input_seq_view(rows), f) == sum(db.lineitem, f));
    }
}

proof fn lemma_apply_invariant(before: Paged<Interval>, after: Paged<Interval>, db: Db, db2: Db, delta: Seq<LineitemDelta>)
    requires state_model(before, db), directory_valid(after), capture_ok(db, db2, lineitem_delta_seq_view(delta)),
        forall|key: u64, flag: Seq<u8>, status: Seq<u8>, component: int| 0 <= component < 6 ==>
            #[trigger] at_value(after, key, flag, status, component)
                == at_value(before, key, flag, status, component)
                    + interval_change(lineitem_delta_seq_view(delta), key, flag, status, component),
    ensures state_model(after, db2),
{
    lemma_capture_dates(db, db2, delta);
    assert forall|key: u64, flag: Seq<u8>, status: Seq<u8>, component: int| 0 <= component < 6 implies
        #[trigger] at_value(after, key, flag, status, component) == interval_sum(db2.lineitem, key, flag, status, component) by {
        let f = interval_weight(key, flag, status, component);
        assert(sum(db2.lineitem, f) == sum(db.lineitem, f)
            + sum(lineitem_delta_seq_view(delta), |item: (LineitemRow, int)| item.1 * f(item.0)));
    }
}

impl State {
    pub closed spec fn db(self) -> Db { self.database@ }

    pub closed spec fn value(self, key: u64, flag: Seq<u8>, status: Seq<u8>, component: int) -> int {
        at_value(self.intervals, key, flag, status, component)
    }

    #[verifier::type_invariant]
    closed spec fn valid(self) -> bool {
        state_model(self.intervals, self.db())
    }
}

struct FlatIntervals {
    intervals: HashMap<u64, FlatGroups>,
    keys: Vec<u64>,
}

impl FlatIntervals {
    closed spec fn value(self, key: u64, flag: Seq<u8>, status: Seq<u8>, component: int) -> int {
        if self.intervals@.contains_key(key) { self.intervals@[key].value(flag, status, component) } else { 0 }
    }

    closed spec fn wf(self) -> bool {
        &&& forall|key: u64| #[trigger] self.intervals@.contains_key(key) <==> self.keys@.contains(key)
        &&& forall|index: int| 0 <= index < self.keys.len() ==>
            1 <= #[trigger] self.keys@[index] < 8589934592 && self.intervals@[self.keys@[index]].wf()
        &&& forall|first: int, second: int| 0 <= first < second < self.keys.len() ==>
            #[trigger] self.keys@[first] != #[trigger] self.keys@[second]
    }

    fn new() -> (result: Self)
        ensures result.wf(), result.keys.len() == 0,
    { Self { intervals: HashMap::new(), keys: Vec::new() } }

    fn add(&mut self, key: u64, flag: &Vec<u8>, status: &Vec<u8>, totals: &Totals)
        requires old(self).wf(), 1 <= key < 8589934592,
        ensures final(self).wf(),
            final(self).keys@ == if old(self).intervals@.contains_key(key) { old(self).keys@ } else { old(self).keys@.push(key) },
            forall|other: u64| other != key ==>
                (#[trigger] final(self).intervals@.contains_key(other) <==> old(self).intervals@.contains_key(other))
                    && (old(self).intervals@.contains_key(other) ==> final(self).intervals@[other] == old(self).intervals@[other]),
            forall|other: u64, other_flag: Seq<u8>, other_status: Seq<u8>, component: int|
                #[trigger] final(self).value(other, other_flag, other_status, component)
                == old(self).value(other, other_flag, other_status, component)
                    + if other == key && (other_flag, other_status) == (flag@, status@) { totals.at(component) } else { 0 },
    {
        let ghost original_map = self.intervals@;
        let ghost original_keys = self.keys@;
        proof {
            if original_map.contains_key(key) {
                let position = choose|position: int| 0 <= position < original_keys.len() && #[trigger] original_keys[position] == key;
                assert(original_map[key].wf());
            }
        }
        let mut groups = match self.intervals.remove(&key) {
            Some(groups) => groups,
            None => { self.keys.push(key); FlatGroups::new() },
        };
        let ghost initial_groups = groups;
        groups.add(flag, status, totals);
        self.intervals.insert(key, groups);
        proof {
            crate::seqfacts::lemma_push_contains(original_keys, key);
            assert forall|position: int| 0 <= position < self.keys.len() implies
                1 <= #[trigger] self.keys@[position] < 8589934592 && self.intervals@[self.keys@[position]].wf() by {
                if self.keys@[position] != key {
                    let old_position = choose|old_position: int| 0 <= old_position < original_keys.len()
                        && #[trigger] original_keys[old_position] == self.keys@[position];
                }
            }
            assert forall|other: u64, query_flag: Seq<u8>, query_status: Seq<u8>, component: int|
                #[trigger] self.value(other, query_flag, query_status, component) ==
                    (if original_map.contains_key(other) { original_map[other].value(query_flag, query_status, component) } else { 0 })
                        + if other == key && (query_flag, query_status) == (flag@, status@) { totals.at(component) } else { 0 } by {
                if other == key {
                    assert(initial_groups.value(query_flag, query_status, component)
                        == if original_map.contains_key(key) { original_map[key].value(query_flag, query_status, component) } else { 0 });
                    assert(self.intervals@[key].value(query_flag, query_status, component) == initial_groups.value(query_flag, query_status, component)
                        + if (query_flag, query_status) == (flag@, status@) { totals.at(component) } else { 0 });
                }
            }
        }
    }

    fn add_ancestors(&mut self, leaf: u64, group: &Group)
        requires old(self).wf(), 4294967296 <= leaf < 8589934592,
        ensures final(self).wf(),
            final(self).keys@.take(old(self).keys.len() as int) == old(self).keys@,
            old(self).keys.len() <= final(self).keys.len(),
            forall|key: u64| key >= 4294967296 ==>
                (#[trigger] final(self).intervals@.contains_key(key) <==> old(self).intervals@.contains_key(key))
                    && (old(self).intervals@.contains_key(key) ==> final(self).intervals@[key] == old(self).intervals@[key]),
            forall|key: u64, flag: Seq<u8>, status: Seq<u8>, component: int|
                #[trigger] final(self).value(key, flag, status, component) == old(self).value(key, flag, status, component)
                    + if key < 4294967296 && ancestor(leaf as int, key) && (flag, status) == group.key() { group.totals.at(component) } else { 0 },
    {
        let ghost initial = *self;
        let mut parent = leaf / 2;
        proof {
            assert forall|key: u64| key < 4294967296 implies
                #[trigger] ancestor(leaf as int, key) == ancestor(parent as int, key) by { lemma_ancestor_step(leaf as int, key); }
            assert(ancestor(leaf as int, parent));
        }
        while parent > 0
            invariant self.wf(), parent < 4294967296,
                parent > 0 ==> ancestor(leaf as int, parent),
                self.keys@.take(initial.keys.len() as int) == initial.keys@, initial.keys.len() <= self.keys.len(),
                forall|key: u64| key >= 4294967296 ==>
                    (#[trigger] self.intervals@.contains_key(key) <==> initial.intervals@.contains_key(key))
                        && (initial.intervals@.contains_key(key) ==> self.intervals@[key] == initial.intervals@[key]),
                forall|key: u64, flag: Seq<u8>, status: Seq<u8>, component: int|
                    #[trigger] self.value(key, flag, status, component) == initial.value(key, flag, status, component)
                        + if key < 4294967296 && ancestor(leaf as int, key) && !ancestor(parent as int, key)
                            && (flag, status) == group.key() { group.totals.at(component) } else { 0 },
            decreases parent,
        {
            self.add(parent, &group.flag, &group.status, &group.totals);
            proof {
                assert forall|key: u64, flag: Seq<u8>, status: Seq<u8>, component: int|
                    #[trigger] self.value(key, flag, status, component) == initial.value(key, flag, status, component)
                        + if key < 4294967296 && ancestor(leaf as int, key) && !ancestor((parent / 2) as int, key)
                            && (flag, status) == group.key() { group.totals.at(component) } else { 0 } by {
                    lemma_ancestor_step(parent as int, key);
                }
                if parent / 2 > 0 {
                    lemma_ancestor_step(parent as int, parent / 2);
                    lemma_ancestor_trans(leaf as int, parent, parent / 2);
                }
            }
            parent /= 2;
        }
    }

    fn hierarchy(&mut self)
        requires old(self).wf(),
            forall|index: int| 0 <= index < old(self).keys.len() ==> #[trigger] old(self).keys@[index] >= 4294967296,
        ensures final(self).wf(),
            final(self).keys@.take(old(self).keys.len() as int) == old(self).keys@,
            forall|leaf: u64| leaf >= 4294967296 ==>
                (#[trigger] final(self).intervals@.contains_key(leaf) <==>
                    old(self).intervals@.contains_key(leaf))
                    && (old(self).intervals@.contains_key(leaf) ==>
                        final(self).intervals@[leaf] == old(self).intervals@[leaf]),
            forall|key: u64, flag: Seq<u8>, status: Seq<u8>, component: int|
                #[trigger] final(self).value(key, flag, status, component)
                == sum(old(self).keys@, |leaf: u64| if ancestor(leaf as int, key) { old(self).value(leaf, flag, status, component) } else { 0 }),
    {
        let ghost initial = *self;
        let length = self.keys.len();
        let mut index = 0usize;
        while index < length
            invariant index <= length, length == initial.keys.len(), self.wf(), initial.wf(),
                self.keys@.take(length as int) == initial.keys@, length <= self.keys.len(),
                forall|position: int| 0 <= position < initial.keys.len() ==> #[trigger] initial.keys@[position] >= 4294967296,
                forall|key: u64| key >= 4294967296 ==>
                    (#[trigger] self.intervals@.contains_key(key) <==> initial.intervals@.contains_key(key))
                        && (initial.intervals@.contains_key(key) ==> self.intervals@[key] == initial.intervals@[key]),
                forall|key: u64, flag: Seq<u8>, status: Seq<u8>, component: int|
                    #[trigger] self.value(key, flag, status, component) == initial.value(key, flag, status, component)
                        + if key < 4294967296 { sum(initial.keys@.take(index as int), |leaf: u64|
                            if ancestor(leaf as int, key) { initial.value(leaf, flag, status, component) } else { 0 }) } else { 0 },
            decreases length - index,
        {
            let leaf = self.keys[index];
            proof {
                assert(leaf == initial.keys@[index as int]);
                assert(4294967296 <= leaf < 8589934592);
                assert(initial.keys@.contains(leaf));
                assert(initial.intervals@.contains_key(leaf));
                assert(self.intervals@.contains_key(leaf));
                assert(self.intervals@[leaf] == initial.intervals@[leaf]);
            }
            let contributions = self.intervals.get(&leaf).unwrap().collect();
            let ghost before = *self;
            let mut group_index = 0usize;
            while group_index < contributions.len()
                invariant group_index <= contributions.len(), self.wf(), 4294967296 <= leaf < 8589934592,
                    index < length, length == initial.keys.len(), initial.wf(),
                    leaf == initial.keys@[index as int], initial.intervals@.contains_key(leaf),
                    contributions@ == initial.intervals@[leaf].entries(),
                    forall|flag: Seq<u8>, status: Seq<u8>, component: int|
                        #[trigger] aggregate(contributions@, flag, status, component) == initial.value(leaf, flag, status, component),
                    forall|key: u64, flag: Seq<u8>, status: Seq<u8>, component: int|
                        #[trigger] before.value(key, flag, status, component) == initial.value(key, flag, status, component)
                            + if key < 4294967296 { sum(initial.keys@.take(index as int), |leaf: u64|
                                if ancestor(leaf as int, key) { initial.value(leaf, flag, status, component) } else { 0 }) } else { 0 },
                    self.keys@.take(length as int) == initial.keys@, length <= self.keys.len(),
                    forall|key: u64| key >= 4294967296 ==>
                        (#[trigger] self.intervals@.contains_key(key) <==> initial.intervals@.contains_key(key))
                            && (initial.intervals@.contains_key(key) ==> self.intervals@[key] == initial.intervals@[key]),
                    forall|key: u64, flag: Seq<u8>, status: Seq<u8>, component: int|
                        #[trigger] self.value(key, flag, status, component) == before.value(key, flag, status, component)
                            + if key < 4294967296 && ancestor(leaf as int, key) {
                                aggregate(contributions@.take(group_index as int), flag, status, component)
                            } else { 0 },
                decreases contributions.len() - group_index,
            {
                let group = &contributions[group_index];
                self.add_ancestors(leaf, group);
                proof {
                    assert forall|key: u64, flag: Seq<u8>, status: Seq<u8>, component: int|
                        #[trigger] self.value(key, flag, status, component) == before.value(key, flag, status, component)
                            + if key < 4294967296 && ancestor(leaf as int, key) {
                                aggregate(contributions@.take(group_index as int + 1), flag, status, component)
                            } else { 0 } by {
                        lemma_sum_take(contributions@, group_index as int, group_weight(flag, status, component));
                    }
                }
                group_index += 1;
            }
            proof {
                assert forall|key: u64, flag: Seq<u8>, status: Seq<u8>, component: int|
                    #[trigger] self.value(key, flag, status, component) == initial.value(key, flag, status, component)
                        + if key < 4294967296 { sum(initial.keys@.take(index as int + 1), |leaf: u64|
                            if ancestor(leaf as int, key) { initial.value(leaf, flag, status, component) } else { 0 }) } else { 0 } by {
                    assert(contributions@.take(group_index as int) =~= contributions@);
                    lemma_sum_take_full(contributions@, group_weight(flag, status, component));
                    let f = |leaf: u64| if ancestor(leaf as int, key) { initial.value(leaf, flag, status, component) } else { 0 };
                    lemma_sum_take(initial.keys@, index as int, f);
                }
            }
            index += 1;
        }
        proof {
            assert forall|key: u64, flag: Seq<u8>, status: Seq<u8>, component: int|
                #[trigger] self.value(key, flag, status, component) == sum(initial.keys@, |leaf: u64|
                    if ancestor(leaf as int, key) { initial.value(leaf, flag, status, component) } else { 0 }) by {
                let f = |leaf: u64| if ancestor(leaf as int, key) { initial.value(leaf, flag, status, component) } else { 0 };
                if key >= 4294967296 {
                    assert forall|position: int| 0 <= position < initial.keys.len() && initial.keys@[position] != key implies #[trigger] f(initial.keys@[position]) == 0 by {
                        lemma_leaf(initial.keys@[position] as int - 6442450944, key);
                    }
                    if initial.intervals@.contains_key(key) {
                        let position = choose|position: int| 0 <= position < initial.keys.len() && #[trigger] initial.keys@[position] == key;
                        assert forall|other: int| 0 <= other < initial.keys.len() && other != position implies #[trigger] f(initial.keys@[other]) == 0 by {
                            if other < position { assert(initial.keys@[other] != key); } else { assert(initial.keys@[position] != initial.keys@[other]); }
                        }
                        lemma_sum_single_match(initial.keys@, f, position);
                    } else { lemma_sum_zero(initial.keys@, f); }
                } else {
                    assert(!initial.intervals@.contains_key(key));
                }
            }
        }
    }
}

pub fn build(lineitem_build: Vec<LineitemInput>, Ghost(db): Ghost<Db>) -> (result: Option<State>)
    requires scan_lineitem_build_ok(db, lineitem_build@),
    ensures result matches Some(state) ==> state.db() == db, result is Some,
{
    let mut daily = FlatIntervals::new();
    let mut index = 0usize;
    let mut processed = Ghost(0int);
    while index < lineitem_build.len()
        invariant index <= lineitem_build.len(), processed@ == index as int, daily.wf(),
            forall|position: int| 0 <= position < daily.keys.len() ==> #[trigger] daily.keys@[position] >= 4294967296,
            forall|key: u64, flag: Seq<u8>, status: Seq<u8>, component: int|
                key >= 4294967296 && 0 <= component < 6 ==> #[trigger] daily.value(key, flag, status, component)
                    == interval_sum(lineitem_input_seq_view(lineitem_build@.take(processed@)), key, flag, status, component),
        decreases lineitem_build.len() - index,
    {
        let row = &lineitem_build[index];
        let ghost before = daily;
        let totals = contribution(row);
        let key = 4294967296u64 + (row.l_shipdate as i64 + 2147483648i64) as u64;
        daily.add(key, &row.l_returnflag, &row.l_linestatus, &totals);
        proof {
            assert forall|other: u64, flag: Seq<u8>, status: Seq<u8>, component: int|
                other >= 4294967296 && 0 <= component < 6 implies #[trigger] daily.value(other, flag, status, component)
                    == interval_sum(lineitem_input_seq_view(lineitem_build@.take(index as int + 1)), other, flag, status, component) by {
                lemma_leaf(row.l_shipdate as int, other);
                lemma_scan_prefix(lineitem_build@, index as int, other, flag, status, component);
                assert(before.value(other, flag, status, component) == interval_sum(lineitem_input_seq_view(lineitem_build@.take(index as int)), other, flag, status, component));
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    let ghost initial_daily = daily;
    proof {
        assert(initial_daily.wf());
        assert forall|position: int| 0 <= position < initial_daily.keys.len() implies
            #[trigger] initial_daily.keys@[position] >= 4294967296 by {};
        assert forall|leaf: u64| #[trigger] initial_daily.intervals@.contains_key(leaf)
            <==> initial_daily.keys@.contains(leaf) by {};
        assert forall|leaf: u64, flag: Seq<u8>, status: Seq<u8>, component: int|
            leaf >= 4294967296 && 0 <= component < 6 implies
            #[trigger] initial_daily.value(leaf, flag, status, component)
                == interval_sum(lineitem_input_seq_view(lineitem_build@), leaf, flag, status, component) by {
            assert(lineitem_input_seq_view(lineitem_build@.take(processed@)) =~=
                lineitem_input_seq_view(lineitem_build@));
        }
        assert forall|index: int| 0 <= index < lineitem_build.len() implies
            #[trigger] initial_daily.keys@.contains(
                (row_leaf(lineitem_input_view(lineitem_build@[index]))) as u64) by {
            let row = lineitem_input_view(lineitem_build@[index]);
            let leaf = (row_leaf(row)) as u64;
            assert(4294967296 <= row_leaf(row) <= 8589934591) by (nonlinear_arith)
                requires -2147483648 <= row.l_shipdate <= 2147483647;
            assert((row_leaf(row) as u64) as int == row_leaf(row)) by (nonlinear_arith)
                requires 4294967296 <= row_leaf(row) <= 8589934591;
            if !initial_daily.keys@.contains(leaf) {
                assert(!initial_daily.intervals@.contains_key(leaf));
                lemma_leaf_count_support(lineitem_input_seq_view(lineitem_build@), index as int,
                    leaf, row.l_returnflag, row.l_linestatus);
                assert(initial_daily.value(leaf, row.l_returnflag, row.l_linestatus, 0) == 0);
                assert(initial_daily.value(leaf, row.l_returnflag, row.l_linestatus, 0) >= 1);
            }
        };
    }
    daily.hierarchy();
    let ghost leaf_keys = initial_daily.keys@;
    let ghost leaf_groups = initial_daily.intervals@;
    let ghost directory_keys = daily.keys@;
    let ghost directory_groups = daily.intervals@;
    let mut intervals = Paged::new();
    let mut key_index = 0usize;
    while key_index < daily.keys.len()
        invariant key_index <= daily.keys.len(), daily.wf(), intervals.wf(),
            leaf_keys == initial_daily.keys@, leaf_groups == initial_daily.intervals@,
            directory_keys == daily.keys@, directory_groups == daily.intervals@,
            initial_daily.wf(),
            forall|position: int| 0 <= position < leaf_keys.len() ==>
                #[trigger] leaf_keys[position] >= 4294967296,
            forall|leaf: u64| #[trigger] leaf_groups.contains_key(leaf) <==> leaf_keys.contains(leaf),
            forall|leaf: u64| leaf >= 4294967296 ==>
                (#[trigger] daily.intervals@.contains_key(leaf) <==> leaf_groups.contains_key(leaf))
                    && (leaf_groups.contains_key(leaf) ==>
                        daily.intervals@[leaf] == leaf_groups[leaf]),
            directory_valid(intervals),
            forall|key: u64, flag: Seq<u8>, status: Seq<u8>, component: int|
                #[trigger] at_value(intervals, key, flag, status, component)
                    == if directory_keys.take(key_index as int).contains(key) {
                        if directory_groups.contains_key(key) {
                            directory_groups[key].value(flag, status, component)
                        } else { 0 }
                    } else { 0 },
            decreases daily.keys.len() - key_index,
    {
        let key = daily.keys[key_index];
        let groups = daily.intervals.get(&key).unwrap().publish();
        let ghost before_intervals = intervals;
        let ghost published = groups;
        let empty = groups.is_empty();
        let ghost empty_ghost = empty;
        if !empty { intervals = intervals.replace(key, Some(Arc::new(Interval { key, groups }))); }
        proof {
            assert(directory_valid(intervals)) by {
                assert forall|query: u64| #[trigger] intervals.at(query) matches Some(interval) ==>
                    interval.key == query && 1 <= query < 8589934592 && interval.groups.wf() by {
                    if query == key {
                        if !empty_ghost {
                            assert(intervals.at(key) == Some(Interval { key, groups: published }));
                        } else {
                            assert(intervals.at(key) == before_intervals.at(key));
                        }
                    } else {
                        assert(intervals.at(query) == before_intervals.at(query));
                    }
                }
            }
            assert forall|query: u64, flag: Seq<u8>, status: Seq<u8>, component: int|
                #[trigger] at_value(intervals, query, flag, status, component)
                    == if directory_keys.take(key_index as int + 1).contains(query) {
                        if directory_groups.contains_key(query) {
                            directory_groups[query].value(flag, status, component)
                        } else { 0 }
                    } else { 0 } by {
                if query == key {
                    assert(directory_keys[key_index as int] == key);
                    assert(directory_keys.take(key_index as int + 1)[key_index as int] == key);
                    assert(directory_keys.take(key_index as int + 1).contains(key));
                    assert(directory_groups.contains_key(key));
                    if empty_ghost {
                        assert(published.value(flag, status, component) == 0);
                    } else {
                        assert(at_value(intervals, key, flag, status, component)
                            == published.value(flag, status, component));
                    }
                } else {
                    assert(at_value(intervals, query, flag, status, component)
                        == at_value(before_intervals, query, flag, status, component));
                }
            }
        }
        key_index += 1;
    }
    proof {
        assert(directory_keys.take(directory_keys.len() as int) =~= directory_keys);
        assert forall|key: u64, flag: Seq<u8>, status: Seq<u8>, component: int|
            0 <= component < 6 implies #[trigger] at_value(intervals, key, flag, status, component)
                == interval_sum(lineitem_input_seq_view(lineitem_build@), key, flag, status, component) by {
            assert forall|leaf: u64| #[trigger] leaf_groups.contains_key(leaf) implies
                #[trigger] leaf_groups[leaf].value(flag, status, component)
                    == interval_sum(lineitem_input_seq_view(lineitem_build@), leaf, flag, status, component) by {
                assert(leaf_keys.contains(leaf));
                assert(initial_daily.value(leaf, flag, status, component)
                    == interval_sum(lineitem_input_seq_view(lineitem_build@), leaf, flag, status, component));
                let position = choose|position: int| 0 <= position < leaf_keys.len()
                    && #[trigger] leaf_keys[position] == leaf;
                assert(leaf_keys[position] >= 4294967296);
                assert(daily.intervals@.contains_key(leaf));
                assert(daily.intervals@[leaf] == leaf_groups[leaf]);
                assert(initial_daily.intervals@[leaf] == leaf_groups[leaf]);
            }
            lemma_directory_interval_sum(lineitem_input_seq_view(lineitem_build@),
                leaf_keys, leaf_groups, key, flag, status, component);
            assert forall|leaf: u64| #[trigger] initial_daily.value(leaf, flag, status, component)
                == if leaf_groups.contains_key(leaf) {
                    leaf_groups[leaf].value(flag, status, component)
                } else { 0 } by {
                if leaf_groups.contains_key(leaf) {
                    assert(initial_daily.intervals@[leaf] == leaf_groups[leaf]);
                }
            }
            assert(daily.value(key, flag, status, component)
                == sum(leaf_keys, |leaf: u64| if ancestor(leaf as int, key) {
                    initial_daily.value(leaf, flag, status, component)
                } else { 0 }));
            let initial_weight = |leaf: u64| if ancestor(leaf as int, key) {
                initial_daily.value(leaf, flag, status, component)
            } else { 0 };
            let published_weight = |leaf: u64| if ancestor(leaf as int, key) {
                if leaf_groups.contains_key(leaf) { leaf_groups[leaf].value(flag, status, component) } else { 0 }
            } else { 0 };
            lemma_sum_ext_all(leaf_keys, initial_weight, published_weight);
            assert(at_value(intervals, key, flag, status, component)
                == daily.value(key, flag, status, component));
        }
    }
    proof { lemma_build_invariant(intervals, lineitem_build@, db); }
    Some(State { intervals, database: Ghost(db) })
}

pub fn apply(state: &State, lineitem_delta: &Vec<LineitemDelta>, Ghost(db2): Ghost<Db>) -> (result: Result<State, u32>)
    requires capture_ok(state.db(), db2, lineitem_delta_seq_view(lineitem_delta@)),
    ensures result matches Ok(next) ==> next.db() == db2, result is Ok,
{
    proof { use_type_invariant(state); }
    let mut changes = FlatIntervals::new();
    proof {
        assert forall|position: int| 0 <= position < 0 &&
            lineitem_delta@[position].sign != 0 implies
            #[trigger] changes.keys@.contains((row_leaf(lineitem_input_view(lineitem_delta@[position].row))) as u64) by {
            assert(changes.keys@.len() == 0);
        }
    }
    let mut index = 0usize;
    let mut processed = Ghost(0int);
    while index < lineitem_delta.len()
        invariant index <= lineitem_delta.len(), processed@ == index as int, changes.wf(),
            forall|position: int| 0 <= position < changes.keys.len() ==> #[trigger] changes.keys@[position] >= 4294967296,
            forall|position: int| 0 <= position < index &&
                lineitem_delta@[position].sign != 0 ==>
                #[trigger] changes.keys@.contains((row_leaf(lineitem_input_view(lineitem_delta@[position].row))) as u64),
            forall|key: u64, flag: Seq<u8>, status: Seq<u8>, component: int|
                key >= 4294967296 && 0 <= component < 6 ==> #[trigger] changes.value(key, flag, status, component)
                    == interval_change(lineitem_delta_seq_view(lineitem_delta@.take(processed@)), key, flag, status, component),
        decreases lineitem_delta.len() - index,
    {
        let delta = &lineitem_delta[index];
        let ghost before = changes;
        if delta.sign != 0 {
            let totals = contribution(&delta.row);
            let signed = totals.scaled(delta.sign);
            let key = 4294967296u64 + (delta.row.l_shipdate as i64 + 2147483648i64) as u64;
            changes.add(key, &delta.row.l_returnflag, &delta.row.l_linestatus, &signed);
            proof {
                assert(key as int == row_leaf(lineitem_input_view(delta.row)));
                assert((row_leaf(lineitem_input_view(delta.row))) as u64 == key);
                if before.keys@.contains(key as u64) {
                    assert(before.intervals@.contains_key(key as u64));
                    assert(changes.keys@ == before.keys@);
                    assert(changes.keys@.contains(key as u64));
                } else {
                    assert(changes.keys@ == before.keys@.push(key as u64));
                    crate::seqfacts::lemma_push_contains(before.keys@, key as u64);
                    assert(changes.keys@.contains(key as u64));
                }
                assert forall|other: u64, flag: Seq<u8>, status: Seq<u8>, component: int|
                    other >= 4294967296 && 0 <= component < 6 implies #[trigger] changes.value(other, flag, status, component)
                        == before.value(other, flag, status, component)
                            + if other == key && (flag, status) == (delta.row.l_returnflag@, delta.row.l_linestatus@) {
                                delta.sign * weight(lineitem_input_view(delta.row), component)
                            } else { 0 } by {
                    assert(signed.at(component) == totals.at(component) * delta.sign);
                    assert(totals.at(component) == weight(lineitem_input_view(delta.row), component));
                    assert(signed.at(component) == delta.sign * weight(lineitem_input_view(delta.row), component)) by (nonlinear_arith)
                        requires signed.at(component) == totals.at(component) * delta.sign,
                            totals.at(component) == weight(lineitem_input_view(delta.row), component);
                }
            }
        }
        proof {
        if delta.sign != 0 {
            let key = 4294967296u64 + (delta.row.l_shipdate as i64 + 2147483648i64) as u64;
            assert(key as int == row_leaf(lineitem_input_view(delta.row)));
            assert((row_leaf(lineitem_input_view(delta.row))) as u64 == key);
            assert(changes.keys@.contains(key as u64));
            assert(changes.keys@.contains((row_leaf(lineitem_input_view(delta.row))) as u64));
        }
        if delta.sign != 0 {
            assert forall|position: int| 0 <= position < index as int + 1 &&
                lineitem_delta@[position].sign != 0 implies
                #[trigger] changes.keys@.contains((row_leaf(lineitem_input_view(lineitem_delta@[position].row))) as u64) by {
                    if position == index as int {
                        assert(lineitem_delta@[position] == *delta);
                        assert(changes.keys@.contains((row_leaf(lineitem_input_view(delta.row))) as u64));
                    }
                }
        }
            assert forall|other: u64, flag: Seq<u8>, status: Seq<u8>, component: int|
                other >= 4294967296 && 0 <= component < 6 implies #[trigger] changes.value(other, flag, status, component)
                    == interval_change(lineitem_delta_seq_view(lineitem_delta@.take(index as int + 1)), other, flag, status, component) by {
                lemma_leaf(delta.row.l_shipdate as int, other);
                lemma_delta_prefix(lineitem_delta@, index as int, other, flag, status, component);
                assert(before.value(other, flag, status, component) == interval_change(lineitem_delta_seq_view(lineitem_delta@.take(index as int)), other, flag, status, component));
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    let ghost initial_changes = changes;
    proof {
        assert forall|leaf: u64, flag: Seq<u8>, status: Seq<u8>, component: int|
            leaf >= 4294967296 && 0 <= component < 6 implies
            #[trigger] initial_changes.value(leaf, flag, status, component)
                == interval_change(lineitem_delta_seq_view(lineitem_delta@), leaf, flag, status, component) by {
            assert(lineitem_delta_seq_view(lineitem_delta@.take(processed@)) =~=
                lineitem_delta_seq_view(lineitem_delta@));
        }
    }
    changes.hierarchy();
    let ghost leaf_keys = initial_changes.keys@;
    let ghost leaf_groups = initial_changes.intervals@;
    let mut intervals = state.intervals.share();
    let mut key_index = 0usize;
    while key_index < changes.keys.len()
        invariant key_index <= changes.keys.len(), changes.wf(), state.valid(), intervals.wf(),
            directory_valid(intervals),
            leaf_keys == initial_changes.keys@, leaf_groups == initial_changes.intervals@,
            forall|query: u64, flag: Seq<u8>, status: Seq<u8>, component: int|
                0 <= component < 6 ==> #[trigger] at_value(intervals, query, flag, status, component)
                    == at_value(state.intervals, query, flag, status, component)
                        + if changes.keys@.take(key_index as int).contains(query) {
                            changes.value(query, flag, status, component)
                        } else { 0 },
        decreases changes.keys.len() - key_index,
    {
        let key = changes.keys[key_index];
        let delta = changes.intervals.get(&key).unwrap();
        let old_groups = match state.intervals.get(key) { Some(interval) => interval.groups.share(), None => Groups::new() };
        let old_empty = old_groups.is_empty();
        let groups = old_groups.updated(delta);
        let empty = groups.is_empty();
        let replacement = if empty { None } else { Some(Arc::new(Interval { key, groups })) };
        let ghost before_intervals = intervals;
        intervals = intervals.replace(key, replacement);
        proof {
            assert(directory_valid(intervals)) by {
            assert forall|query: u64| #[trigger] intervals.at(query) matches Some(interval) ==>
                interval.key == query && 1 <= query < 8589934592 && interval.groups.wf() by {
                    if query == key {
                        if !empty {
                            assert(intervals.at(query) == Some(Interval { key, groups }));
                        }
                    } else {
                        assert(intervals.at(query) == before_intervals.at(query));
                    }
                }
            }
            assert forall|query: u64, flag: Seq<u8>, status: Seq<u8>, component: int|
                0 <= component < 6 implies #[trigger] at_value(intervals, query, flag, status, component)
                    == at_value(state.intervals, query, flag, status, component)
                        + if changes.keys@.take(key_index as int + 1).contains(query) {
                            changes.value(query, flag, status, component)
                        } else { 0 } by {
                crate::seqfacts::lemma_take_extend_contains(changes.keys@, key_index as int, query);
                if query == key {
                    assert(changes.keys@.take(key_index as int + 1)[key_index as int] == key);
                    assert(changes.keys@.take(key_index as int + 1).contains(key));
                    assert(changes.intervals@.contains_key(key));
                    assert(changes.value(key, flag, status, component) == delta.value(flag, status, component));
                    assert(groups.value(flag, status, component)
                        == old_groups.value(flag, status, component)
                            + delta.value(flag, status, component));
                    assert(old_groups.value(flag, status, component)
                        == at_value(state.intervals, key, flag, status, component));
                    if empty {
                        assert(groups.value(flag, status, component) == 0);
                        assert(at_value(intervals, key, flag, status, component) == 0);
                        assert(at_value(state.intervals, key, flag, status, component)
                            + changes.value(key, flag, status, component) == 0);
                    } else {
                        assert(at_value(intervals, key, flag, status, component)
                            == groups.value(flag, status, component));
                    }
                } else {
                    assert(at_value(intervals, query, flag, status, component)
                        == at_value(before_intervals, query, flag, status, component));
                    assert(changes.keys@.take(key_index as int + 1).contains(query)
                        <==> changes.keys@.take(key_index as int).contains(query));
                }
            }
        }
        key_index += 1;
    }
    proof {
        assert(changes.keys@.take(key_index as int) =~= changes.keys@);
        assert forall|leaf: u64, flag: Seq<u8>, status: Seq<u8>, component: int|
            0 <= component < 6 && #[trigger] leaf_groups.contains_key(leaf) implies
            #[trigger] leaf_groups[leaf].value(flag, status, component)
                == sum(lineitem_delta_seq_view(lineitem_delta@), |item: (LineitemRow, int)|
                    item.1 * leaf_row_weight(leaf, flag, status, component)(item.0)) by {
            assert(leaf_keys.contains(leaf));
            let leaf_position = choose|position: int| 0 <= position < leaf_keys.len() &&
                #[trigger] leaf_keys[position] == leaf;
            assert(leaf_keys[leaf_position] >= 4294967296);
            assert(lineitem_delta_seq_view(lineitem_delta@.take(processed@)) =~=
                lineitem_delta_seq_view(lineitem_delta@));
            lemma_leaf_interval_change(lineitem_delta_seq_view(lineitem_delta@), leaf, flag, status, component);
            assert(initial_changes.value(leaf, flag, status, component)
                == sum(lineitem_delta_seq_view(lineitem_delta@), |item: (LineitemRow, int)|
                    item.1 * leaf_row_weight(leaf, flag, status, component)(item.0)));
            assert(initial_changes.intervals@[leaf] == leaf_groups[leaf]);
        }
        assert forall|query: u64, flag: Seq<u8>, status: Seq<u8>, component: int|
            0 <= component < 6 implies #[trigger] at_value(intervals, query, flag, status, component)
                == at_value(state.intervals, query, flag, status, component)
                    + interval_change(lineitem_delta_seq_view(lineitem_delta@), query, flag, status, component) by {
            lemma_directory_interval_change(lineitem_delta_seq_view(lineitem_delta@),
                leaf_keys, leaf_groups, query, flag, status, component);
            assert(changes.value(query, flag, status, component)
                == sum(initial_changes.keys@, |leaf: u64| if ancestor(leaf as int, query) {
                    initial_changes.value(leaf, flag, status, component)
                } else { 0 }));
            assert forall|leaf: u64| #[trigger] initial_changes.value(leaf, flag, status, component)
                == if leaf_groups.contains_key(leaf) {
                    leaf_groups[leaf].value(flag, status, component)
                } else { 0 } by {}
            lemma_sum_ext_all(initial_changes.keys@,
                |leaf: u64| if ancestor(leaf as int, query) {
                    initial_changes.value(leaf, flag, status, component)
                } else { 0 },
                |leaf: u64| if ancestor(leaf as int, query) {
                    if leaf_groups.contains_key(leaf) {
                        leaf_groups[leaf].value(flag, status, component)
                    } else { 0 }
                } else { 0 });
            assert(changes.value(query, flag, status, component)
                == sum(leaf_keys, |leaf: u64| if ancestor(leaf as int, query) {
                    if leaf_groups.contains_key(leaf) {
                        leaf_groups[leaf].value(flag, status, component)
                    } else { 0 }
                } else { 0 }));
        }
    }
    proof { lemma_apply_invariant(state.intervals, intervals, state.db(), db2, lineitem_delta@); }
    Ok(State { intervals, database: Ghost(db2) })
}

fn group_before(left: &Group, right: &Group) -> (result: bool)
    ensures result == (bytes_lt(left.key().0, right.key().0)
        || (left.key().0 == right.key().0 && bytes_lt(left.key().1, right.key().1))),
{
    proof { lemma_bytes_lt_irrefl(left.key().0); }
    if bytes_eq_exec(&left.flag, &right.flag) { bytes_lt_exec(&left.status, &right.status) }
    else { bytes_lt_exec(&left.flag, &right.flag) }
}

proof fn lemma_group_irrefl(group: Group)
    ensures !group_lt(group, group),
{
    lemma_bytes_lt_irrefl(group.key().0);
    lemma_bytes_lt_irrefl(group.key().1);
}

proof fn lemma_group_trans(left: Group, middle: Group, right: Group)
    requires group_lt(left, middle), group_lt(middle, right),
    ensures group_lt(left, right),
{
    let lf = left.key().0;
    let ls = left.key().1;
    let mf = middle.key().0;
    let ms = middle.key().1;
    let rf = right.key().0;
    let rs = right.key().1;
    if lf != mf {
        if mf != rf {
            assert(bytes_lt(lf, mf));
            assert(bytes_lt(mf, rf));
            lemma_bytes_lt_trans(lf, mf, rf);
        } else {
            assert(bytes_lt(lf, mf));
            assert(bytes_lt(lf, rf));
        }
    } else {
        if mf != rf {
            assert(bytes_lt(mf, rf));
            assert(bytes_lt(lf, rf));
        } else {
            assert(!bytes_lt(lf, mf)) by { lemma_bytes_lt_irrefl(lf); }
            assert(bytes_lt(ls, ms)) by {
                if !bytes_lt(ls, ms) { assert(!group_lt(left, middle)); }
            }
            assert(bytes_lt(ms, rs)) by {
                if !bytes_lt(ms, rs) { assert(!group_lt(middle, right)); }
            }
            lemma_bytes_lt_trans(ls, ms, rs);
        }
    }
}

proof fn lemma_group_total(left: Group, right: Group)
    requires left.key() != right.key(),
    ensures group_lt(left, right) || group_lt(right, left),
{
    let lf = left.key().0;
    let ls = left.key().1;
    let rf = right.key().0;
    let rs = right.key().1;
    if lf != rf {
        lemma_bytes_lt_total(lf, rf);
    } else {
        lemma_bytes_lt_total(ls, rs);
    }
}

proof fn lemma_group_order()
    ensures strict_order(|left: Group, right: Group| group_lt(left, right)),
{
    assert forall|group: Group| #[trigger] group_lt(group, group) == false by {
        lemma_group_irrefl(group);
    }
    assert forall|left: Group, middle: Group, right: Group|
        #[trigger] group_lt(left, middle) && #[trigger] group_lt(middle, right) implies
        group_lt(left, right) by {
        lemma_group_trans(left, middle, right);
    }
}

proof fn lemma_empty_aggregate(groups: Seq<Group>, flag: Seq<u8>, status: Seq<u8>, component: int)
    requires groups.len() == 0,
    ensures aggregate(groups, flag, status, component) == 0,
{
    lemma_sum_empty(groups, group_weight(flag, status, component));
}

fn insert_group(groups: &mut Vec<Group>, value: Group)
    ensures final(groups)@ == insert_by(old(groups)@, value, |left: Group, right: Group| group_lt(left, right)),
{
    let ghost initial = old(groups)@;
    let ghost wanted = value;
    let mut index = 0usize;
    while index < groups.len() && !group_before(&value, &groups[index])
        invariant index <= groups.len(), groups@ == initial, value == wanted,
            forall|position: int| 0 <= position < index ==>
                !group_lt(wanted, #[trigger] initial[position]),
        decreases groups.len() - index,
    {
        index += 1;
    }
    groups.insert(index, value);
    proof {
        assert forall|position: int| 0 <= position < index implies
            !group_lt(wanted, #[trigger] initial[position]) by {}
        if index < initial.len() {
            assert(group_lt(wanted, initial[index as int]));
        }
        lemma_insert_scan(initial, wanted, index as int);
    }
}

proof fn lemma_insert_scan(groups: Seq<Group>, value: Group, index: int)
    requires 0 <= index <= groups.len(),
        forall|position: int| 0 <= position < index ==>
            !group_lt(value, #[trigger] groups[position]),
        index < groups.len() ==> group_lt(value, groups[index as int]),
    ensures groups.insert(index, value) == insert_by(groups, value, |left: Group, right: Group| group_lt(left, right)),
    decreases index,
{
    if index == 0 {
        if groups.len() == 0 {
            assert(groups.insert(0, value) =~= seq![value]);
        } else {
            assert(groups.insert(0, value) =~= seq![value] + groups);
        }
    } else {
        let tail = groups.drop_first();
        lemma_insert_scan(tail, value, index - 1);
        assert(groups.insert(index, value) =~= seq![groups[0]] + tail.insert(index - 1, value));
    }
}

fn insertion_sort(source: &Vec<Group>, index: usize) -> (result: Vec<Group>)
    requires index <= source.len(),
        group_keys_unique(source@),
    ensures result@ == sort_dups(source@.take(index as int), |left: Group, right: Group| group_lt(left, right)),
    decreases index,
{
    if index == 0 {
        proof {
            assert(source@.take(0) =~= Seq::empty());
            assert(sort_dups(Seq::<Group>::empty(),
                |left: Group, right: Group| group_lt(left, right)) =~= Seq::<Group>::empty());
        }
        return Vec::new();
    }
    let value = source[index - 1].share();
    let mut sorted = insertion_sort(source, index - 1);
    proof {
        assert(source@.take(index as int).drop_last() =~= source@.take(index as int - 1));
        assert(source@.take(index as int).last() == source@[index as int - 1]);
        assert(sort_dups(source@.take(index as int), |left: Group, right: Group| group_lt(left, right))
            == insert_by(sort_dups(source@.take(index as int - 1),
                |left: Group, right: Group| group_lt(left, right)),
                source@[index as int - 1], |left: Group, right: Group| group_lt(left, right)));
    }
    insert_group(&mut sorted, value);
    proof {
        assert(sort_dups(source@.take(index as int), |left: Group, right: Group| group_lt(left, right))
            == insert_by(sort_dups(source@.take(index as int - 1),
                |left: Group, right: Group| group_lt(left, right)),
                source@[index as int - 1], |left: Group, right: Group| group_lt(left, right)));
    }
    sorted
}

fn ordered(groups: Vec<Group>) -> (result: Vec<Group>)
    requires group_keys_unique(groups@),
    ensures result@ == sort_dups(groups@, |left: Group, right: Group| group_lt(left, right)),
        result@.to_multiset() =~= groups@.to_multiset(),
        forall|group: Group| #[trigger] result@.contains(group) <==> groups@.contains(group),
        group_keys_unique(result@),
        forall|first: int, second: int| 0 <= first < second < result.len() ==>
            !#[trigger] group_lt(result@[second], result@[first]),
{
    proof {
        lemma_group_order();
        assert forall|left: Group, right: Group| groups@.contains(left) && groups@.contains(right) && left != right implies
            #[trigger] group_lt(left, right) || #[trigger] group_lt(right, left) by {
            let left_index = choose|index: int| 0 <= index < groups.len() && #[trigger] groups@[index] == left;
            let right_index = choose|index: int| 0 <= index < groups.len() && #[trigger] groups@[index] == right;
            assert(groups@[left_index].key() != groups@[right_index].key());
            lemma_group_total(left, right);
        }
    }
    let sorted = insertion_sort(&groups, groups.len());
    proof {
        assert(groups@.take(groups.len() as int) =~= groups@);
        lemma_sort_dups(groups@, |left: Group, right: Group| group_lt(left, right));
        assert forall|group: Group| #[trigger] sorted@.contains(group) <==> groups@.contains(group) by {
            assert(sorted@ == sort_dups(groups@, |left: Group, right: Group| group_lt(left, right)));
        }
        assert forall|first: int, second: int| 0 <= first < second < sorted.len() implies
            #[trigger] sorted@[first].key() != #[trigger] sorted@[second].key() by {
            if sorted@[first].key() == sorted@[second].key() {
                assert(sort_dups(groups@, |left: Group, right: Group| group_lt(left, right))
                    .contains(sorted@[first]));
                assert(sort_dups(groups@, |left: Group, right: Group| group_lt(left, right))
                    .contains(sorted@[second]));
                assert(groups@.contains(sorted@[first]));
                assert(groups@.contains(sorted@[second]));
                let first_source = choose|index: int| 0 <= index < groups.len() && #[trigger] groups@[index] == sorted@[first];
                let second_source = choose|index: int| 0 <= index < groups.len() && #[trigger] groups@[index] == sorted@[second];
                assert(0 <= first_source < groups.len());
                assert(0 <= second_source < groups.len());
                assert(first_source != second_source) by {
                    if first_source == second_source {
                        assert(sorted@[first] == sorted@[second]);
                        assert forall|index: int| 0 <= index < groups.len() && index != first_source implies
                            #[trigger] groups@[index] != sorted@[first] by {
                            if groups@[index] == sorted@[first] {
                                assert(groups@[index].key() == groups@[first_source].key());
                                if index < first_source {
                                    assert(groups@[index].key() != groups@[first_source].key());
                                } else {
                                    assert(groups@[first_source].key() != groups@[index].key());
                                }
                            }
                        }
                        broadcast use vstd::seq_lib::group_to_multiset_ensures;
                        let f = eqv(sorted@[first]);
                        lemma_sum_single_match(groups@, f, first_source);
                        lemma_count_as_sum(groups@, sorted@[first]);
                        assert(groups@.to_multiset().count(sorted@[first]) == 1);
                        let rest = sorted@.remove(first);
                        let other = second - 1;
                        assert(rest[other] == sorted@[second]);
                        lemma_sum_remove(sorted@, first, f);
                        lemma_sum_ge_term(rest, f, other);
                        lemma_count_as_sum(sorted@, sorted@[first]);
                        assert(sorted@.to_multiset().count(sorted@[first]) > 1);
                    }
                }
                assert(groups@[first_source].key() != groups@[second_source].key());
            }
        }
    }
    sorted
}

/// The groups with rows; a group without rows has no sums (see lemma_prefix_empty).
fn positive_groups(source: &Vec<Group>, Ghost(db): Ghost<Db>, Ghost(cutoff): Ghost<int>) -> (result: Vec<Group>)
    requires group_keys_unique(source@),
        forall|flag: Seq<u8>, status: Seq<u8>, component: int| 0 <= component < 6 ==>
            #[trigger] aggregate(source@, flag, status, component) == prefix_sum(db, cutoff, flag, status, component),
    ensures group_directory(result@),
        forall|flag: Seq<u8>, status: Seq<u8>, component: int| 0 <= component < 6 ==>
            #[trigger] aggregate(result@, flag, status, component) == aggregate(source@, flag, status, component),
{
    let mut result: Vec<Group> = Vec::new();
    let mut index = 0usize;
    while index < source.len()
        invariant index <= source.len(), group_keys_unique(source@),
            forall|flag: Seq<u8>, status: Seq<u8>, component: int| 0 <= component < 6 ==>
                #[trigger] aggregate(source@, flag, status, component) == prefix_sum(db, cutoff, flag, status, component),
            group_directory(result@),
            forall|position: int| 0 <= position < result.len() ==> source@.take(index as int).contains(#[trigger] result@[position]),
            forall|flag: Seq<u8>, status: Seq<u8>, component: int| 0 <= component < 6 ==>
                #[trigger] aggregate(result@, flag, status, component) == aggregate(source@.take(index as int), flag, status, component),
        decreases source.len() - index,
    {
        let group = &source[index];
        let ghost before = result@;
        proof {
            assert forall|flag: Seq<u8>, status: Seq<u8>, component: int| 0 <= component < 6 implies
                #[trigger] aggregate(before, flag, status, component) == aggregate(source@.take(index as int), flag, status, component) by {}
            assert forall|flag: Seq<u8>, status: Seq<u8>, component: int| 0 <= component < 6 implies
                #[trigger] aggregate(source@.take(index as int + 1), flag, status, component)
                    == aggregate(source@.take(index as int), flag, status, component)
                        + group_weight(flag, status, component)(source@[index as int]) by {
                lemma_sum_take(source@, index as int, group_weight(flag, status, component));
            }
        }
        if group.totals.count.is_pos() {
            result.push(group.share());
            proof {
                assert(result@ == before.push(source@[index as int]));
                assert forall|position: int| 0 <= position < result.len() implies
                    source@.take(index as int + 1).contains(#[trigger] result@[position]) by {
                    if position < before.len() {
                        let earlier = source@.take(index as int);
                        assert(earlier.contains(before[position]));
                        let j = choose|j: int| 0 <= j < earlier.len() && earlier[j] == before[position];
                        assert(source@.take(index as int + 1)[j] == before[position]);
                    } else {
                        assert(source@.take(index as int + 1)[index as int] == result@[position]);
                    }
                }
                assert forall|first: int, second: int| 0 <= first < second < result@.len() implies
                    (#[trigger] result@[first]).key() != (#[trigger] result@[second]).key() by {
                    if second == before.len() {
                        let earlier = source@.take(index as int);
                        assert(earlier.contains(before[first]));
                        let j = choose|j: int| 0 <= j < earlier.len() && earlier[j] == before[first];
                        assert(source@[j] == before[first]);
                        assert(source@[j].key() != source@[index as int].key());
                    }
                }
                assert forall|flag: Seq<u8>, status: Seq<u8>, component: int| 0 <= component < 6 implies
                    #[trigger] aggregate(result@, flag, status, component)
                        == aggregate(source@.take(index as int + 1), flag, status, component) by {
                    lemma_sum_push(before, source@[index as int], group_weight(flag, status, component));
                    assert(aggregate(before, flag, status, component) == aggregate(source@.take(index as int), flag, status, component));
                    assert(aggregate(source@.take(index as int + 1), flag, status, component)
                        == aggregate(source@.take(index as int), flag, status, component)
                            + group_weight(flag, status, component)(source@[index as int]));
                    assert(aggregate(result@, flag, status, component)
                        == aggregate(before, flag, status, component) + group_weight(flag, status, component)(source@[index as int]));
                }
            }
        } else {
            proof {
                let key = source@[index as int].key();
                lemma_prefix_count_nonneg(db, cutoff, key.0, key.1);
                assert forall|component: int| 0 <= component < 6 implies #[trigger] source@[index as int].totals.at(component) == 0 by {
                    lemma_group_at(source@, index as int, 0);
                    lemma_group_at(source@, index as int, component);
                    lemma_prefix_empty(db, cutoff, key.0, key.1, component);
                }
                assert forall|flag: Seq<u8>, status: Seq<u8>, component: int| 0 <= component < 6 implies
                    #[trigger] aggregate(result@, flag, status, component)
                        == aggregate(source@.take(index as int + 1), flag, status, component) by {
                    assert(group_weight(flag, status, component)(source@[index as int]) == 0);
                }
                assert forall|position: int| 0 <= position < result.len() implies
                    source@.take(index as int + 1).contains(#[trigger] result@[position]) by {
                    let earlier = source@.take(index as int);
                    assert(earlier.contains(result@[position]));
                    let j = choose|j: int| 0 <= j < earlier.len() && earlier[j] == result@[position];
                    assert(source@.take(index as int + 1)[j] == result@[position]);
                }
            }
        }
        index += 1;
    }
    proof { assert(source@.take(index as int) =~= source@); }
    result
}

pub fn execute(state: &State, p1: i64) -> (result: Option<Vec<Output>>)
    ensures result matches Some(output) ==> query_result(state.db(), params(p1 as int), out_view(output@)),
        answer_fits(state.db(), params(p1 as int)) ==> result is Some,
{
    proof { use_type_invariant(state); }
    let cutoff = 10561i128 - p1 as i128;
    if cutoff < -2147483648i128 {
        let output = Vec::new();
        proof {
            assert forall|flag: Seq<u8>, status: Seq<u8>, component: int| 0 <= component < 6 implies
                #[trigger] aggregate(Seq::empty(), flag, status, component) == prefix_sum(state.db(), 10561 - p1, flag, status, component) by {
                lemma_sum_zero(state.db().lineitem, prefix_weight(10561 - p1, flag, status, component));
            }
            lemma_execute_result(state.db(), p1 as int, Seq::empty(), out_view(output@));
        }
        return Some(output);
    }
    let mut remaining = if cutoff >= 2147483647i128 { 4294967296u64 }
        else { (cutoff + 2147483648i128 + 1) as u64 };
    let ghost target = remaining as int;
    let mut start = 0u64;
    let mut level = 33u8;
    let mut accumulated = FlatGroups::new();
    let mut selected = Ghost(Seq::<Tile>::empty());
    while level > 0
        invariant level <= 33, start + remaining <= 4294967296,
            level == 33 ==> start == 0,
            start as int + remaining as int == target,
            (level == 33 && remaining <= 4294967296)
                || (level <= 32 && remaining < power2(level as int)),
            tile_prefix(selected@), tile_sum(selected@) == start as int,
            accumulated.wf(), state.valid(),
            start as int % power2(level as int) == 0,
            forall|index: int| 0 <= index < selected@.len() ==>
                1 <= #[trigger] selected@[index].key < 8589934592,
            forall|flag: Seq<u8>, status: Seq<u8>, component: int| 0 <= component < 6 ==>
                #[trigger] accumulated.value(flag, status, component)
                    == sum(selected@, |tile: Tile| at_value(state.intervals, tile.key as u64, flag, status, component)),
        decreases level,
    {
        let ghost previous_level = level;
        level -= 1;
        let width = power2_exec(level);
        proof {
            if previous_level == 33 {
                assert(start == 0);
            } else {
                assert(power2(previous_level as int) == 2 * (width as int));
                lemma_half_alignment(start as int, width as int);
            }
            assert(start as int % width as int == 0);
        }
            if remaining >= width {
                proof {
                    assert(width == power2(level as int));
                }
            let offset = power2_exec(32 - level) + start / width;
            let key = offset;
            if let Some(interval) = state.intervals.get(key) {
                let groups = interval.groups.collect();
                let ghost before_accumulated = accumulated;
                proof {
                    accumulated.lemma_value_entries();
                    before_accumulated.lemma_value_entries();
                    assert forall|flag: Seq<u8>, status: Seq<u8>, component: int| 0 <= component < 6 implies
                        #[trigger] accumulated.value(flag, status, component)
                            == before_accumulated.value(flag, status, component)
                                + aggregate(groups@.take(0), flag, status, component) by {
                        lemma_empty_aggregate(groups@.take(0), flag, status, component);
                    }
                }
                let mut index = 0usize;
                while index < groups.len()
                    invariant index <= groups.len(), accumulated.wf(), groups@ == interval.groups.entries(),
                        forall|flag: Seq<u8>, status: Seq<u8>, component: int| 0 <= component < 6 ==>
                            #[trigger] accumulated.value(flag, status, component)
                                == before_accumulated.value(flag, status, component)
                                    + aggregate(groups@.take(index as int), flag, status, component),
                    decreases groups.len() - index,
                {
                    let group = &groups[index];
                    let ghost before_group = accumulated;
                    let flag = copy_bytes(&group.flag);
                    let status = copy_bytes(&group.status);
                    let totals = group.totals.copy();
                    accumulated.add(&flag, &status, &totals);
                    proof {
                        accumulated.lemma_value_entries();
                        before_group.lemma_value_entries();
                        before_accumulated.lemma_value_entries();
                        assert(groups@[index as int] == *group);
                        assert(group.totals == groups@[index as int].totals);
                        assert(flag@ == groups@[index as int].key().0);
                        assert(status@ == groups@[index as int].key().1);
                        assert(totals == groups@[index as int].totals);
                        assert forall|component: int| 0 <= component < 6 implies
                            #[trigger] accumulated.value(flag@, status@, component)
                            == before_group.value(flag@, status@, component)
                                + totals.at(component) by {}
                        assert forall|flag: Seq<u8>, status: Seq<u8>, component: int| 0 <= component < 6 implies
                            #[trigger] accumulated.value(flag, status, component)
                                == before_accumulated.value(flag, status, component)
                                    + aggregate(groups@.take(index as int + 1), flag, status, component) by {
                            lemma_sum_take(groups@, index as int, group_weight(flag, status, component));
                            assert(before_group.value(flag, status, component)
                                == before_accumulated.value(flag, status, component)
                                    + aggregate(groups@.take(index as int), flag, status, component));
                            assert(aggregate(groups@.take(index as int + 1), flag, status, component)
                                == aggregate(groups@.take(index as int), flag, status, component)
                                    + group_weight(flag, status, component)(groups@[index as int]));
                            assert(accumulated.value(flag, status, component)
                                == before_group.value(flag, status, component)
                                    + if (flag, status) == (groups@[index as int].key().0, groups@[index as int].key().1) {
                                        groups@[index as int].totals.at(component)
                                    } else { 0 });
                            assert(groups@[index as int].key() == (groups@[index as int].key().0, groups@[index as int].key().1));
                            assert(group_weight(flag, status, component)(groups@[index as int])
                                == if (flag, status) == groups@[index as int].key() {
                                    groups@[index as int].totals.at(component)
                                } else { 0 });
                        }
                    }
                    index += 1;
                }
                proof {
                    assert(groups@.take(groups.len() as int) =~= groups@);
                    assert forall|flag: Seq<u8>, status: Seq<u8>, component: int| 0 <= component < 6 implies
                        #[trigger] accumulated.value(flag, status, component)
                            == before_accumulated.value(flag, status, component)
                                + at_value(state.intervals, key, flag, status, component) by {
                        assert(aggregate(groups@, flag, status, component)
                            == interval.groups.value(flag, status, component));
                    }
                }
            }
            let ghost previous_selected = selected@;
            let ghost previous_start = start;
            selected = Ghost(selected@.push(Tile {
                level: level as int,
                start: start as int,
                width: width as int,
                key: key as int,
            }));
            proof {
                assert(width as int == power2(level as int));
                assert(start as int % width as int == 0);
                assert(key as int == power2(32 - level as int) + start as int / width as int);
                assert(0 <= level as int <= 32);
                assert(1 <= (key as int));
                assert((key as int) < 8589934592);
                let tile = Tile { level: level as int, start: start as int,
                    width: width as int, key: key as int };
                assert(tile_ok(tile));
                lemma_tile_prefix_push(previous_selected, tile);
                assert(tile_prefix(selected@));
                assert forall|flag: Seq<u8>, status: Seq<u8>, component: int| 0 <= component < 6 implies
                    #[trigger] accumulated.value(flag, status, component)
                        == sum(selected@, |tile: Tile| at_value(state.intervals, tile.key as u64, flag, status, component)) by {
                    assert(selected@ =~= previous_selected.push(Tile {
                        level: level as int,
                        start: start as int,
                        width: width as int,
                        key: key as int,
                    }));
                    lemma_sum_push(previous_selected, Tile {
                        level: level as int,
                        start: start as int,
                        width: width as int,
                        key: key as int,
                    }, |tile: Tile| at_value(state.intervals, tile.key as u64, flag, status, component));
                }
            }
            start += width;
            remaining -= width;
            proof {
                assert(selected@ =~= previous_selected.push(Tile {
                    level: level as int,
                    start: previous_start as int,
                    width: width as int,
                    key: key as int,
                }));
                lemma_sum_push(previous_selected,
                    Tile {
                        level: level as int,
                        start: previous_start as int,
                        width: width as int,
                        key: key as int,
                    },
                    |tile: Tile| tile.width as int);
                assert(tile_sum(selected@) == previous_start as int + width as int);
                assert(start as int == previous_start as int + width as int);
                let quotient = previous_start as int / width as int + 1;
                vstd::arithmetic::div_mod::lemma_fundamental_div_mod(previous_start as int, width as int);
                assert(start as int == quotient * width as int) by (nonlinear_arith)
                    requires previous_start as int == width as int * (previous_start as int / width as int) + previous_start as int % width as int,
                        previous_start as int % width as int == 0, start as int == previous_start as int + width as int,
                        quotient == previous_start as int / width as int + 1;
                vstd::arithmetic::div_mod::lemma_mod_multiples_basic(quotient, width as int);
                assert(width as int == power2(level as int));
            }
        }
    }
    let collected = accumulated.collect();
    let ghost collected_view = collected@;
    proof {
        assert(remaining == 0);
        assert(tile_sum(selected@) == target);
        assert(target == if cutoff >= 2147483647 { 4294967296int }
            else { cutoff + 2147483649 });
        assert forall|flag: Seq<u8>, status: Seq<u8>, component: int| 0 <= component < 6 implies
            #[trigger] aggregate(collected_view, flag, status, component)
                == prefix_sum(state.db(), 10561 - p1, flag, status, component) by {
            assert(aggregate(collected_view, flag, status, component)
                == accumulated.value(flag, status, component));
            lemma_tiles_query(state.intervals, state.db(), selected@, cutoff as int,
                flag, status, component);
        }
    }
    let kept = positive_groups(&collected, Ghost(state.db()), Ghost(10561 - p1 as int));
    let ghost kept_view = kept@;
    let groups = ordered(kept);
    proof {
        broadcast use vstd::seq_lib::group_to_multiset_ensures;
        assert forall|flag: Seq<u8>, status: Seq<u8>, component: int| 0 <= component < 6 implies
            #[trigger] aggregate(groups@, flag, status, component)
                == prefix_sum(state.db(), 10561 - p1, flag, status, component) by {
            lemma_sum_perm(groups@, kept_view, group_weight(flag, status, component));
            assert(aggregate(kept_view, flag, status, component) == aggregate(collected_view, flag, status, component));
        }
        assert forall|index: int| 0 <= index < groups.len() implies
            (#[trigger] groups@[index]).totals.count@ > 0 by {
            assert(groups@.contains(groups@[index]));
            assert(kept_view.contains(groups@[index]));
        }
    }
    let ghost answer = groups@.map_values(|group: Group| group_row(group));
    proof { lemma_execute_result(state.db(), p1 as int, groups@, answer); }
    let mut output = Vec::new();
    let mut index = 0usize;
    while index < groups.len()
        invariant index <= groups.len(), output.len() == index,
            answer == groups@.map_values(|group: Group| group_row(group)),
            query_result(state.db(), params(p1 as int), answer),
            forall|position: int| 0 <= position < groups.len() ==>
                0 < #[trigger] groups@[position].totals.count@,
            forall|position: int| 0 <= position < index ==>
                (#[trigger] out_view(output@)[position]) == answer[position],
        decreases groups.len() - index,
    {
        let group = &groups[index];
        let totals = &group.totals;
        let count = totals.count.to_i128();
        let quantity = totals.quantity.to_i128();
        let price = totals.price.to_i128();
        let discounted = totals.discounted.to_i128();
        let charge = totals.charge.to_i128();
        let discount = totals.discount.to_i128();
        if count.is_none() || quantity.is_none() || price.is_none() || discounted.is_none()
            || charge.is_none() || discount.is_none() || count.unwrap() > i64::MAX as i128 {
            proof {
                let row = answer[index as int];
                assert(row == group_row(groups@[index as int]));
                assert(!out_fits(row));
                assert(!answer_fits(state.db(), params(p1 as int))) by {
                    if answer_fits(state.db(), params(p1 as int)) { assert(out_fits(answer[index as int])); }
                }
            }
            return None;
        }
        let count = count.unwrap();
        let denominator = count * 100;
        let ghost previous_output = output@;
        let ghost previous_rows = out_view(previous_output);
        output.push((copy_bytes(&group.flag), copy_bytes(&group.status), quantity.unwrap(), price.unwrap(),
            discounted.unwrap(), charge.unwrap(), (quantity.unwrap(), denominator), (price.unwrap(), denominator),
            (discount.unwrap(), denominator), count as i64));
        proof {
            let row = output@.last();
            assert(output@ == previous_output.push(row));
            assert(out_view(output@) =~= previous_rows.push(group_row(*group)));
            assert(out_view(output@)[index as int] == answer[index as int]);
        }
        index += 1;
    }
    proof { assert(out_view(output@) =~= answer); }
    Some(output)
}

}
