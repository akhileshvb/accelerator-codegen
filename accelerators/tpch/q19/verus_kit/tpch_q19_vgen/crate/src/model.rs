use vstd::prelude::*;
use std::sync::Arc;
use verus_common::sums::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::storage::*;
use verus_prooflib::wide::Wide;

verus! {

pub struct PartCell {
    pub class: u8,
    pub brand: Arc<Vec<u8>>,
    pub count: Wide,
}

pub struct HistCell {
    pub class: u8,
    pub brand: Arc<Vec<u8>>,
    pub quantity: i64,
    pub count: Wide,
    pub revenue: Wide,
}

pub open spec fn class_matches(part: PartRow, class: u8) -> bool {
    &&& part.p_size >= 1
    &&& if class == 0 {
        part.p_size <= 5 && (part.p_container == seq![b'S', b'M', b' ', b'C', b'A', b'S', b'E']
            || part.p_container == seq![b'S', b'M', b' ', b'B', b'O', b'X']
            || part.p_container == seq![b'S', b'M', b' ', b'P', b'A', b'C', b'K']
            || part.p_container == seq![b'S', b'M', b' ', b'P', b'K', b'G'])
    } else if class == 1 {
        part.p_size <= 10 && (part.p_container == seq![b'M', b'E', b'D', b' ', b'B', b'A', b'G']
            || part.p_container == seq![b'M', b'E', b'D', b' ', b'B', b'O', b'X']
            || part.p_container == seq![b'M', b'E', b'D', b' ', b'P', b'K', b'G']
            || part.p_container == seq![b'M', b'E', b'D', b' ', b'P', b'A', b'C', b'K'])
    } else if class == 2 {
        part.p_size <= 15 && (part.p_container == seq![b'L', b'G', b' ', b'C', b'A', b'S', b'E']
            || part.p_container == seq![b'L', b'G', b' ', b'B', b'O', b'X']
            || part.p_container == seq![b'L', b'G', b' ', b'P', b'A', b'C', b'K']
            || part.p_container == seq![b'L', b'G', b' ', b'P', b'K', b'G'])
    } else { false }
}

pub open spec fn classified(part: PartRow) -> bool {
    class_matches(part, 0) || class_matches(part, 1) || class_matches(part, 2)
}

pub open spec fn eligible(line: LineitemRow) -> bool {
    (line.l_shipmode == seq![b'A', b'I', b'R'] || line.l_shipmode == seq![b'A', b'I', b'R', b' ', b'R', b'E', b'G'])
        && line.l_shipinstruct == seq![b'D', b'E', b'L', b'I', b'V', b'E', b'R', b' ', b'I', b'N', b' ', b'P', b'E', b'R', b'S', b'O', b'N']
}

#[verifier::opaque]
pub open spec fn part_sum(db: Db, key: int, class: u8, brand: Seq<u8>) -> int {
    sum(db.part, |part: PartRow| ind(part.p_partkey == key && class_matches(part, class) && part.p_brand == brand))
}

#[verifier::opaque]
pub open spec fn hist_sum(db: Db, class: u8, brand: Seq<u8>, quantity: int, revenue: bool) -> int {
    sum(db.part, |part: PartRow| sum(db.lineitem, |line: LineitemRow| cell_weight(part, line, class, brand, quantity, revenue)))
}

pub open spec fn line_weight(line: LineitemRow, revenue: bool) -> int {
    if revenue { line.l_extendedprice * (100 - line.l_discount) } else { 1 }
}

pub open spec fn cell_weight(part: PartRow, line: LineitemRow, class: u8, brand: Seq<u8>, quantity: int, revenue: bool) -> int {
    if class_matches(part, class) && part.p_brand == brand && part.p_partkey == line.l_partkey && eligible(line) && line.l_quantity == quantity {
        line_weight(line, revenue)
    } else { 0 }
}

pub open spec fn join_weight(class: u8, brand: Seq<u8>, quantity: int, revenue: bool) -> spec_fn(LineitemRow, PartRow) -> int {
    |line: LineitemRow, part: PartRow| cell_weight(part, line, class, brand, quantity, revenue)
}

#[verifier::opaque]
pub open spec fn fingerprint(brand: Seq<u8>) -> u64
    decreases brand.len(),
{
    if brand.len() == 0 { 0 } else {
        ((fingerprint(brand.drop_last()) as int * 257 + brand.last() as int + 1) % 1000000007) as u64
    }
}

pub open spec fn hist_key(class: u8, brand: Seq<u8>, quantity: i64) -> u64 {
    ((fingerprint(brand) as int * 3 + class as int) * 1000000007
        + (quantity as u64) as int % 1000000007) as u64
}

pub open spec fn part_value(store: Store<PartCell>, key: int, class: u8, brand: Seq<u8>) -> int {
    if i64::MIN <= key <= i64::MAX {
        part_cells_value(store.lookup((key as i64) as u64), class, brand)
    } else { 0 }
}

pub open spec fn hist_value(store: Store<HistCell>, class: u8, brand: Seq<u8>, quantity: int, revenue: bool) -> int {
    if i64::MIN <= quantity <= i64::MAX {
        hist_cells_value(store.lookup(hist_key(class, brand, quantity as i64)), class, brand, quantity, revenue)
    } else { 0 }
}

pub open spec fn part_work_value(work: Work<PartCell>, key: int, class: u8, brand: Seq<u8>) -> int {
    if i64::MIN <= key <= i64::MAX {
        part_cells_value(work.lookup((key as i64) as u64), class, brand)
    } else { 0 }
}

pub open spec fn hist_work_value(work: Work<HistCell>, class: u8, brand: Seq<u8>, quantity: int, revenue: bool) -> int {
    if i64::MIN <= quantity <= i64::MAX {
        hist_cells_value(work.lookup(hist_key(class, brand, quantity as i64)), class, brand, quantity, revenue)
    } else { 0 }
}

pub open spec fn part_scan_prefix(rows: Seq<PartInput>, end: int, key: int, class: u8, brand: Seq<u8>) -> int {
    sum(rows.take(end), |row: PartInput| part_scan_weight(row, key, class, brand))
}

pub open spec fn part_scan_weight(row: PartInput, key: int, class: u8, brand: Seq<u8>) -> int {
    ind(row.p_partkey == key && row.p_brand@ == brand && class_matches(part_input_view(row), class))
}

pub open spec fn part_delta_prefix(rows: Seq<PartDelta>, end: int, key: int, class: u8, brand: Seq<u8>) -> int {
    sum(rows.take(end), |row: PartDelta| part_delta_weight(row, key, class, brand))
}

pub open spec fn part_delta_weight(row: PartDelta, key: int, class: u8, brand: Seq<u8>) -> int {
    if row.p_partkey == key && row.p_brand@ == brand
        && ((class == 0 && row.small != 0) || (class == 1 && row.medium != 0) || (class == 2 && row.large != 0)) {
        row.sign as int
    } else { 0 }
}

pub open spec fn line_scan_prefix(parts: Store<PartCell>, rows: Seq<LineInput>, end: int,
    class: u8, brand: Seq<u8>, quantity: int, revenue: bool) -> int
{
    sum(rows.take(end), |row: LineInput| line_scan_weight(parts, row, class, brand, quantity, revenue))
}

pub open spec fn line_scan_weight(parts: Store<PartCell>, row: LineInput, class: u8, brand: Seq<u8>, quantity: int, revenue: bool) -> int {
    if row.l_quantity == quantity {
        part_value(parts, row.l_partkey as int, class, brand)
            * if revenue { row.l_extendedprice as int * (100 - row.l_discount as int) } else { 1 }
    } else { 0 }
}

pub open spec fn line_delta_prefix(parts: Store<PartCell>, rows: Seq<LineDelta>, end: int,
    class: u8, brand: Seq<u8>, quantity: int, revenue: bool) -> int
{
    sum(rows.take(end), |row: LineDelta| line_delta_weight(parts, row, class, brand, quantity, revenue))
}

pub open spec fn line_delta_weight(parts: Store<PartCell>, row: LineDelta, class: u8, brand: Seq<u8>, quantity: int, revenue: bool) -> int {
    if row.eligible != 0 && row.sign != 0 && row.l_quantity == quantity {
        part_value(parts, row.l_partkey as int, class, brand) * row.sign as int
            * if revenue { row.l_extendedprice as int * (100 - row.l_discount as int) } else { 1 }
    } else { 0 }
}

pub open spec fn rescan_prefix(changes: Work<PartCell>, rows: Seq<LineInput>, end: int,
    class: u8, brand: Seq<u8>, quantity: int, revenue: bool) -> int
{
    sum(rows.take(end), |row: LineInput| rescan_weight(changes, row, class, brand, quantity, revenue))
}

pub open spec fn rescan_weight(changes: Work<PartCell>, row: LineInput, class: u8, brand: Seq<u8>, quantity: int, revenue: bool) -> int {
    if row.l_quantity == quantity {
        part_work_value(changes, row.l_partkey as int, class, brand)
            * if revenue { row.l_extendedprice as int * (100 - row.l_discount as int) } else { 1 }
    } else { 0 }
}

pub open spec fn window_prefix(store: Store<HistCell>, class: u8, brand: Seq<u8>, parameter: int,
    end: int, revenue: bool) -> int
{
    sum(rng(end), |offset: int| hist_value(store, class, brand, parameter + offset, revenue))
}

pub open spec fn part_cell_weight(cell: PartCell, class: u8, brand: Seq<u8>) -> int {
    if cell.class == class && (*cell.brand)@ == brand { cell.count@ } else { 0 }
}

pub open spec fn hist_cell_weight(cell: HistCell, class: u8, brand: Seq<u8>, quantity: int, revenue: bool) -> int {
    if cell.class == class && (*cell.brand)@ == brand && cell.quantity == quantity {
        if revenue { cell.revenue@ } else { cell.count@ }
    } else { 0 }
}

pub open spec fn part_cells_value(cells: Seq<PartCell>, class: u8, brand: Seq<u8>) -> int {
    sum(cells, |cell: PartCell| part_cell_weight(cell, class, brand))
}

pub open spec fn hist_cells_value(cells: Seq<HistCell>, class: u8, brand: Seq<u8>, quantity: int, revenue: bool) -> int {
    sum(cells, |cell: HistCell| hist_cell_weight(cell, class, brand, quantity, revenue))
}

pub open spec fn join_cell_weight(cell: PartCell, class: u8, brand: Seq<u8>, quantity: int,
    input_quantity: int, sign: int, value: int, revenue: bool) -> int
{
    if quantity == input_quantity && cell.class == class && (*cell.brand)@ == brand {
        cell.count@ * sign * if revenue { value } else { 1 }
    } else { 0 }
}

pub proof fn lemma_join_cell_factor(cells: Seq<PartCell>, class: u8, brand: Seq<u8>, quantity: int,
    input_quantity: int, sign: int, value: int, revenue: bool)
    ensures sum(cells, |cell: PartCell| join_cell_weight(cell, class, brand, quantity, input_quantity, sign, value, revenue))
        == if quantity == input_quantity { part_cells_value(cells, class, brand) * sign * if revenue { value } else { 1 } } else { 0 },
{
    let weight = |cell: PartCell| join_cell_weight(cell, class, brand, quantity, input_quantity, sign, value, revenue);
    if quantity == input_quantity {
        let factor = if revenue { value } else { 1 };
        let scalar = sign * factor;
        assert forall|cell: PartCell| #[trigger] weight(cell) == scalar * part_cell_weight(cell, class, brand) by {
            if cell.class == class && (*cell.brand)@ == brand {
                let count = cell.count@;
                assert(count * sign * factor == (sign * factor) * count) by (nonlinear_arith);
            }
        }
        lemma_sum_scale(cells, |cell: PartCell| part_cell_weight(cell, class, brand), weight, scalar);
        let total = part_cells_value(cells, class, brand);
        assert((sign * factor) * total == total * sign * factor) by (nonlinear_arith);
    } else { lemma_sum_zero(cells, weight); }
}

pub open spec fn part_cells_valid(cells: Seq<PartCell>, positive: bool) -> bool {
    forall|index: int| #![trigger cells[index]]
        0 <= index < cells.len() ==> cells[index].class < 3 && (positive ==> cells[index].count@ > 0)
}

pub open spec fn hist_cells_valid(cells: Seq<HistCell>, key: u64, positive: bool) -> bool {
    &&& forall|index: int| #![trigger cells[index]]
        0 <= index < cells.len() ==> {
            let cell = cells[index];
            cell.class < 3 && (positive ==> cell.count@ > 0) && hist_key(cell.class, (*cell.brand)@, cell.quantity) == key
        }
}

pub open spec fn part_patch_valid(work: Work<PartCell>) -> bool {
    work.ready() && forall|key: u64| #![trigger work.lookup(key)] part_cells_valid(work.lookup(key), false)
}

pub open spec fn changes_for(work: Work<PartCell>, rows: Seq<PartDelta>) -> bool {
    part_patch_valid(work) && forall|key: int, class: u8, brand: Seq<u8>| #![trigger part_work_value(work, key, class, brand)]
        part_work_value(work, key, class, brand) == part_delta_prefix(rows, rows.len() as int, key, class, brand)
}

pub open spec fn keys_u64(keys: Seq<i64>) -> Seq<u64> { keys.map_values(|key: i64| key as u64) }

pub open spec fn coverage(work: Work<PartCell>, keys: Seq<i64>) -> bool {
    forall|key: int, class: u8, brand: Seq<u8>| #![trigger part_work_value(work, key, class, brand)]
        !seq_view_i64(keys).contains(key) ==> part_work_value(work, key, class, brand) == 0
}

pub proof fn lemma_signed_keys(keys: Seq<i64>, key: i64)
    ensures keys_u64(keys).contains(key as u64) == seq_view_i64(keys).contains(key as int),
{
    if keys_u64(keys).contains(key as u64) {
        let index = choose|index: int| #![trigger keys_u64(keys)[index]]
            0 <= index < keys_u64(keys).len() && keys_u64(keys)[index] == key as u64;
        let actual = keys[index];
        assert(actual as u64 == key as u64);
        assert(actual == key) by (bit_vector) requires actual as u64 == key as u64;
        assert(seq_view_i64(keys)[index] == key as int);
        assert(seq_view_i64(keys).contains(key as int));
    }
    if seq_view_i64(keys).contains(key as int) {
        let index = choose|index: int| #![trigger seq_view_i64(keys)[index]]
            0 <= index < seq_view_i64(keys).len() && seq_view_i64(keys)[index] == key as int;
        assert(keys[index] == key);
        assert(keys_u64(keys)[index] == key as u64);
        assert(keys_u64(keys).contains(key as u64));
    }
}

pub open spec fn part_touches(row: PartDelta) -> bool {
    row.small != 0 || row.medium != 0 || row.large != 0
}

/// The part keys whose lineitems are rescanned: each key of a class-touching delta row, once, in order.
pub open spec fn wanted_keys(rows: Seq<PartDelta>) -> Seq<i64>
    decreases rows.len(),
{
    if rows.len() == 0 { Seq::empty() } else {
        let before = wanted_keys(rows.drop_last());
        let row = rows.last();
        if part_touches(row) && !before.contains(row.p_partkey) { before.push(row.p_partkey) } else { before }
    }
}

pub proof fn lemma_wanted_covers(rows: Seq<PartDelta>, key: int, class: u8, brand: Seq<u8>)
    ensures !seq_view_i64(wanted_keys(rows)).contains(key)
        ==> sum(rows, |row: PartDelta| part_delta_weight(row, key, class, brand)) == 0,
    decreases rows.len(),
{
    if rows.len() > 0 {
        let init = rows.drop_last();
        lemma_wanted_covers(init, key, class, brand);
        let row = rows.last();
        let before = wanted_keys(init);
        if !seq_view_i64(wanted_keys(rows)).contains(key) {
            if part_touches(row) && !before.contains(row.p_partkey) {
                assert(seq_view_i64(before.push(row.p_partkey)) =~= seq_view_i64(before).push(row.p_partkey as int));
                assert(seq_view_i64(before.push(row.p_partkey)).last() == row.p_partkey as int);
                assert(!seq_view_i64(before).contains(key)) by {
                    if seq_view_i64(before).contains(key) {
                        let i = choose|i: int| 0 <= i < seq_view_i64(before).len() && seq_view_i64(before)[i] == key;
                        assert(seq_view_i64(before.push(row.p_partkey))[i] == key);
                    }
                }
            }
            assert(!seq_view_i64(before).contains(key));
            assert(part_delta_weight(row, key, class, brand) == 0) by {
                if part_delta_weight(row, key, class, brand) != 0 {
                    assert(row.p_partkey as int == key && part_touches(row));
                    if before.contains(row.p_partkey) {
                        let i = choose|i: int| 0 <= i < before.len() && before[i] == row.p_partkey;
                        assert(seq_view_i64(before)[i] == key);
                    } else {
                        assert(seq_view_i64(before.push(row.p_partkey)) =~= seq_view_i64(before).push(row.p_partkey as int));
                        assert(seq_view_i64(wanted_keys(rows)).contains(key));
                    }
                }
            }
        }
    }
}

pub open spec fn hist_patch_valid(work: Work<HistCell>) -> bool {
    work.ready() && forall|key: u64| #![trigger work.lookup(key)] hist_cells_valid(work.lookup(key), key, false)
}

pub open spec fn parts_valid(store: Store<PartCell>) -> bool {
    store.valid() && forall|key: u64| #![trigger store.lookup(key)] part_cells_valid(store.lookup(key), false)
}

pub open spec fn histogram_valid(store: Store<HistCell>) -> bool {
    store.valid() && forall|key: u64| #![trigger store.lookup(key)] hist_cells_valid(store.lookup(key), key, false)
}

}
