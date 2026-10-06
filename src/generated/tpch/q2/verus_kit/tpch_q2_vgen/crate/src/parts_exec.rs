use vstd::prelude::*;
use std::sync::Arc;
use verus_common::sums::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::model::{retained, fits_i64};
use crate::dict::*;
use crate::parts::*;
use crate::semantics::{part_scan_amount, part_scan_weight};
use crate::table::{Table, Cell, Major, Work, rows_valid, minor_of, cell_sum};

verus! {

pub open spec fn row_weight(key: i64, size: i64, mfgr: Seq<u8>, ty: Seq<u8>) -> spec_fn(BrassPartScan) -> int {
    |row: BrassPartScan| if row.p_partkey == key && row.p_size == size && row.p_mfgr@ == mfgr && row.p_type@ == ty { 1int } else { 0int }
}

pub open spec fn tp_expected(d: Dict, rows: Seq<BrassPartScan>, key: i64, size: i64, mid: i64, tid: i64) -> int {
    if 0 <= mid < d.len() && 0 <= tid < d.len() { sum(rows, row_weight(key, size, d.text(mid as int), d.text(tid as int))) } else { 0int }
}

pub open spec fn row_covered(d: Dict, row: BrassPartScan) -> bool {
    &&& exists|id: int| 0 <= id < d.len() && d.text(id) == row.p_mfgr@
    &&& exists|id: int| 0 <= id < d.len() && d.text(id) == row.p_type@
}

/// Interns the manufacturer and type of every row.
pub fn intern_rows(rows: &[BrassPartScan]) -> (result: Option<Dict>)
    ensures result matches Some(dict) ==> dict.valid() && dict.len() <= 1000000 &&
        forall|j: int| 0 <= j < rows@.len() ==> row_covered(dict, #[trigger] rows@[j]),
{
    let mut dict = Dict::new();
    let mut index = 0;
    while index < rows.len()
        invariant index <= rows.len(), dict.valid(), dict.len() <= 1000000,
            forall|j: int| 0 <= j < index ==> row_covered(dict, #[trigger] rows@[j]),
        decreases rows.len() - index,
    {
        if dict.strings.len() >= 999990 { return None; }
        let ghost before = dict;
        assert(before.len() < 999990);
        let first = dict.intern(&rows[index].p_mfgr);
        let ghost mid = dict;
        let second = dict.intern(&rows[index].p_type);
        proof {
            assert forall|j: int| 0 <= j < index + 1 implies row_covered(dict, #[trigger] rows@[j]) by {
                if j < index as int {
                    let a = choose|id: int| 0 <= id < before.len() && before.text(id) == rows@[j].p_mfgr@;
                    let b = choose|id: int| 0 <= id < before.len() && before.text(id) == rows@[j].p_type@;
                    assert(mid.strings@[a] == before.strings@[a]);
                    assert(mid.strings@[b] == before.strings@[b]);
                    assert(dict.strings@[a] == mid.strings@[a]);
                    assert(dict.strings@[b] == mid.strings@[b]);
                    assert(dict.len() >= before.len());
                    assert(dict.text(a) == rows@[j].p_mfgr@);
                    assert(dict.text(b) == rows@[j].p_type@);
                } else {
                    assert(dict.text(first as int) == rows@[j].p_mfgr@) by { assert(dict.strings@[first as int] == mid.strings@[first as int]); }
                    assert(dict.text(second as int) == rows@[j].p_type@);
                }
            }
        }
        index += 1;
    }
    assert(index == rows@.len());
    Some(dict)
}


/// Cells of a table whose at-values vanish outside the dictionary and whose volumes vanish.
pub proof fn lemma_tp_structure(table: Table, dl: int)
    requires table.valid(),
        forall|key: i64, size: i64, mid: i64, tid: i64| !(0 <= mid < dl && 0 <= tid < dl) ==>
            #[trigger] table.at((key, 0, 0), (size, mid, tid), false) == 0,
        forall|key: Major, minor: Major| #[trigger] table.at(key, minor, true) == 0,
    ensures forall|key: i64, index: int| 0 <= index < table.rows((key, 0, 0)).len() ==>
        0 <= (#[trigger] table.rows((key, 0, 0))[index]).second < dl && 0 <= table.rows((key, 0, 0))[index].third < dl,
{
    table.lemma_shaped_valid();
    assert forall|key: i64, index: int| 0 <= index < table.rows((key, 0, 0)).len() implies
        0 <= (#[trigger] table.rows((key, 0, 0))[index]).second < dl && 0 <= table.rows((key, 0, 0))[index].third < dl by {
        let rows = table.rows((key, 0, 0));
        let cell = rows[index];
        let minor = minor_of(cell);
        crate::table::lemma_cell_lookup(rows, minor, index);
        assert(table.at((key, 0, 0), minor, true) == cell_sum(rows, minor, true)) by { reveal(Table::at); }
        assert(table.at((key, 0, 0), minor, false) == cell_sum(rows, minor, false)) by { reveal(Table::at); }
        assert(cell.volume == 0);
        assert(cell.count != 0);
        if !(0 <= cell.second < dl && 0 <= cell.third < dl) {
            assert(table.at((key, 0, 0), (cell.first, cell.second, cell.third), false) == 0);
            assert(minor == (cell.first, cell.second, cell.third));
        }
    }
}

/// The scan amount of a part equals the cell count of its strings' ids.
pub proof fn lemma_scan_amount_cells(d: Dict, rows: Seq<BrassPartScan>, key: i64, size: i64, target: PartRow, mid: int, tid: int)
    requires d.valid(), 0 <= mid < d.len(), 0 <= tid < d.len(), d.text(mid) == target.p_mfgr, d.text(tid) == target.p_type,
        target.p_partkey == key as int, target.p_size == size as int,
    ensures part_scan_amount(rows, key, size, target) == sum(rows, row_weight(key, size, d.text(mid), d.text(tid))),
{
    lemma_sum_ext_all(rows, |row: BrassPartScan| part_scan_weight(row, key, size, target), row_weight(key, size, d.text(mid), d.text(tid)));
}

pub proof fn lemma_dict_has_eq(ps: Parts, d: Dict, text: Seq<u8>)
    requires *ps.dict == d,
    ensures ps.dict_has(text) == (exists|id: int| 0 <= id < d.len() && d.text(id) == text),
{
    if exists|id: int| 0 <= id < d.len() && d.text(id) == text {
        let id = choose|id: int| 0 <= id < d.len() && d.text(id) == text;
        assert(0 <= id < ps.dl() && ps.dtext(id) == text);
    }
    if ps.dict_has(text) {
        let id = choose|id: int| 0 <= id < ps.dl() && ps.dtext(id) == text;
        assert(0 <= id < d.len() && d.text(id) == text);
    }
}

pub proof fn lemma_scan_amount_zero(d: Dict, rows: Seq<BrassPartScan>, key: i64, size: i64, target: PartRow)
    requires forall|j: int| 0 <= j < rows.len() ==> row_covered(d, #[trigger] rows[j]),
        !(target.p_partkey == key as int && target.p_size == size as int
            && (exists|id: int| 0 <= id < d.len() && d.text(id) == target.p_mfgr)
            && (exists|id: int| 0 <= id < d.len() && d.text(id) == target.p_type)),
    ensures part_scan_amount(rows, key, size, target) == 0,
{
    assert forall|j: int| 0 <= j < rows.len() implies (|row: BrassPartScan| part_scan_weight(row, key, size, target))(#[trigger] rows[j]) == 0 by {
        assert(row_covered(d, rows[j]));
    }
    lemma_sum_zero(rows, |row: BrassPartScan| part_scan_weight(row, key, size, target));
}

#[verifier::rlimit(60)]
pub fn build_parts(rows: &[BrassPartScan]) -> (result: Option<Parts>)
    ensures result matches Some(ps) ==> {
        &&& ps.structure_ok()
        &&& forall|key: i64, size: i64, target: PartRow| #[trigger] ps.amount(key, size, target) == part_scan_amount(rows@, key, size, target)
        &&& forall|j: int| 0 <= j < rows@.len() ==> ps.dict_has((#[trigger] rows@[j]).p_mfgr@) && ps.dict_has(rows@[j].p_type@)
    },
{
    let dict = match intern_rows(rows) { None => return None, Some(dict) => dict };
    let tp0 = Table::empty(rows.len());
    let tl0 = Table::empty(8192);
    let mut tp = Work::new(&tp0);
    let mut tl = Work::new(&tl0);
    let mut index = 0;
    while index < rows.len()
        invariant index <= rows.len(), dict.valid(), dict.len() <= 1000000, tp.valid(), tl.valid(),
            forall|j: int| 0 <= j < rows@.len() ==> row_covered(dict, #[trigger] rows@[j]),
            forall|key: i64, size: i64, mid: i64, tid: i64| #[trigger] tp.at((key, 0, 0), (size, mid, tid), false) ==
                tp_expected(dict, rows@.take(index as int), key, size, mid, tid),
            forall|key: Major, minor: Major| #[trigger] tp.at(key, minor, true) == 0,
            forall|key: Major, minor: Major| #[trigger] tl.at(key, minor, true) == 0,
            forall|size: i64, tid: i64, key: i64, mid: i64, third: i64| #[trigger] tl.at((size, tid, 0), (key, mid, third), false) ==
                if third == 0 { tp.at((key, 0, 0), (size, mid, tid), false) } else { 0int },
        decreases rows.len() - index,
    {
        let row = &rows[index];
        let mid = match dict.find(&row.p_mfgr) { Some(id) => id, None => { proof { assert(row_covered(dict, rows@[index as int])); } return None; } };
        let tid = match dict.find(&row.p_type) { Some(id) => id, None => { proof { assert(row_covered(dict, rows@[index as int])); } return None; } };
        proof { assert(mid < 1000000 && tid < 1000000); }
        let ghost tp_before = tp;
        let ghost tl_before = tl;
        if tp.add((row.p_partkey, 0, 0), (row.p_size, mid as i64, tid as i64), 1, 0).is_none() { return None; }
        let ghost tp_mid = tp;
        if tl.add((row.p_size, tid as i64, 0), (row.p_partkey, mid as i64, 0), 1, 0).is_none() { return None; }
        proof {
            let weight_at = |key: i64, size: i64, m: i64, t: i64| row_weight(key, size, dict.text(m as int), dict.text(t as int));
            assert forall|key: i64, size: i64, m: i64, t: i64| #[trigger] tp.at((key, 0, 0), (size, m, t), false) ==
                tp_expected(dict, rows@.take(index as int + 1), key, size, m, t) by {
                assert(tp.at((key, 0, 0), (size, m, t), false) == tp_mid.at((key, 0, 0), (size, m, t), false));
                assert(tp_mid.at((key, 0, 0), (size, m, t), false) == tp_before.at((key, 0, 0), (size, m, t), false)
                    + if (key, 0i64, 0i64) == (row.p_partkey, 0i64, 0i64) && (size, m, t) == (row.p_size, mid as i64, tid as i64) { 1int } else { 0int });
                if 0 <= m < dict.len() && 0 <= t < dict.len() {
                    lemma_sum_take(rows@, index as int, row_weight(key, size, dict.text(m as int), dict.text(t as int)));
                    if row.p_partkey == key && row.p_size == size && row.p_mfgr@ == dict.text(m as int) && row.p_type@ == dict.text(t as int) {
                        assert(dict.text(mid as int) == row.p_mfgr@);
                        lemma_dict_ids_eq(dict, mid as int, m as int);
                        lemma_dict_ids_eq(dict, tid as int, t as int);
                    }
                    if (size, m, t) == (row.p_size, mid as i64, tid as i64) && key == row.p_partkey {
                        assert(dict.text(m as int) == row.p_mfgr@);
                        assert(dict.text(t as int) == row.p_type@);
                    }
                } else {
                    assert((size, m, t) != (row.p_size, mid as i64, tid as i64));
                }
            }
            assert forall|key: Major, minor: Major| #[trigger] tp.at(key, minor, true) == 0 by {
                assert(tp.at(key, minor, true) == tp_before.at(key, minor, true) + 0);
            }
            assert forall|key: Major, minor: Major| #[trigger] tl.at(key, minor, true) == 0 by {
                assert(tl.at(key, minor, true) == tl_before.at(key, minor, true) + 0);
            }
            assert forall|size: i64, t: i64, key: i64, m: i64, third: i64| #[trigger] tl.at((size, t, 0), (key, m, third), false) ==
                if third == 0 { tp.at((key, 0, 0), (size, m, t), false) } else { 0int } by {
                assert(tl.at((size, t, 0), (key, m, third), false) == tl_before.at((size, t, 0), (key, m, third), false)
                    + if (size, t, 0i64) == (row.p_size, tid as i64, 0i64) && (key, m, third) == (row.p_partkey, mid as i64, 0i64) { 1int } else { 0int });
                assert(tp.at((key, 0, 0), (size, m, t), false) == tp_before.at((key, 0, 0), (size, m, t), false)
                    + if (key, 0i64, 0i64) == (row.p_partkey, 0i64, 0i64) && (size, m, t) == (row.p_size, mid as i64, tid as i64) { 1int } else { 0int });
            }
        }
        index += 1;
    }
    proof { assert(index == rows@.len()); assert(rows@.take(index as int) =~= rows@); }
    let tpt = tp.finish();
    let tlt = tl.finish();
    let ps = Parts { dict: Arc::new(dict), tp: tpt, tl: tlt };
    proof {
        assert(ps.tp.valid() && ps.tl.valid());
        assert forall|key: i64, size: i64, mid: i64, tid: i64| !(0 <= mid < dict.len() && 0 <= tid < dict.len()) implies
            #[trigger] ps.tp.at((key, 0, 0), (size, mid, tid), false) == 0 by {
            assert(tp.at((key, 0, 0), (size, mid, tid), false) == tp_expected(dict, rows@, key, size, mid, tid));
        }
        assert forall|key: Major, minor: Major| #[trigger] ps.tp.at(key, minor, true) == 0 by {}
        assert forall|key: Major, minor: Major| #[trigger] ps.tl.at(key, minor, true) == 0 by {}
        lemma_tp_structure(ps.tp, dict.len());
        assert(ps.dl() == dict.len());
        assert(ps.structure_ok());
        assert forall|key: i64, size: i64, target: PartRow| #[trigger] ps.amount(key, size, target) == part_scan_amount(rows@, key, size, target) by {
            if target.p_partkey == key as int && target.p_size == size as int && ps.dict_has(target.p_mfgr) && ps.dict_has(target.p_type) {
                let m = ps.dict_id(target.p_mfgr);
                let t = ps.dict_id(target.p_type);
                assert(0 <= m < dict.len() && 0 <= t < dict.len());
                assert(ps.tp.at((key, 0, 0), (size, m as i64, t as i64), false) == tp_expected(dict, rows@, key, size, m as i64, t as i64));
                lemma_scan_amount_cells(dict, rows@, key, size, target, m, t);
            } else {
                lemma_dict_has_eq(ps, dict, target.p_mfgr);
                lemma_dict_has_eq(ps, dict, target.p_type);
                lemma_scan_amount_zero(dict, rows@, key, size, target);
            }
        }
        assert forall|j: int| 0 <= j < rows@.len() implies ps.dict_has((#[trigger] rows@[j]).p_mfgr@) && ps.dict_has(rows@[j].p_type@) by {
            assert(row_covered(dict, rows@[j]));
            lemma_dict_has_eq(ps, dict, rows@[j].p_mfgr@);
            lemma_dict_has_eq(ps, dict, rows@[j].p_type@);
        }
    }
    Some(ps)
}


pub open spec fn cell_minor_weight(minor: Major) -> spec_fn(Cell) -> int {
    |cell: Cell| crate::table::cell_weight(cell, minor, false)
}

pub open spec fn cells_at(cells: Seq<Cell>, minor: Major) -> int {
    sum(cells, cell_minor_weight(minor))
}

pub proof fn lemma_cells_at_sum(cells: Seq<Cell>, minor: Major)
    ensures cell_sum(cells, minor, false) == cells_at(cells, minor),
{
    reveal(cell_sum);
    lemma_sum_ext_all(cells, |cell: Cell| crate::table::cell_weight(cell, minor, false), cell_minor_weight(minor));
}

/// Takes every cell of part key `key` out of both tables.
#[verifier::rlimit(60)]
pub fn remove_key(tp: &mut Work, tl: &mut Work, before: &Parts, key: i64) -> (result: Option<()>)
    requires old(tp).valid(), old(tl).valid(), before.structure_ok(),
        forall|minor: Major| #[trigger] old(tp).at((key, 0, 0), minor, false) == before.tp.at((key, 0, 0), minor, false),
        forall|size: i64, tid: i64, mid: i64, third: i64| #[trigger] old(tl).at((size, tid, 0), (key, mid, third), false) ==
            if third == 0 { before.tp.at((key, 0, 0), (size, mid, tid), false) } else { 0int },
    ensures result is Some ==> {
        &&& final(tp).valid() && final(tl).valid()
        &&& forall|k: Major, m: Major| #[trigger] final(tp).at(k, m, false) == if k == (key, 0i64, 0i64) { 0int } else { old(tp).at(k, m, false) }
        &&& forall|k: Major, m: Major| #[trigger] final(tp).at(k, m, true) == old(tp).at(k, m, true)
        &&& forall|k: Major, m: Major| #[trigger] final(tl).at(k, m, false) == if k.2 == 0 && m.0 == key { 0int } else { old(tl).at(k, m, false) }
        &&& forall|k: Major, m: Major| #[trigger] final(tl).at(k, m, true) == old(tl).at(k, m, true)
    },
{
    proof { before.tp.lemma_shaped_valid(); }
    let cells = before.tp.get((key, 0, 0));
    let ghost rows = (*cells)@;
    let ghost tp_start = *tp;
    let ghost tl_start = *tl;
    let mut j = 0;
    while j < cells.len()
        invariant j <= cells.len(), tp.valid(), tl.valid(), (*cells)@ == rows, rows == before.tp.rows((key, 0, 0)), rows_valid(rows),
            forall|k: Major, m: Major| #[trigger] tp.at(k, m, false) == tp_start.at(k, m, false) - (if k == (key, 0i64, 0i64) { cells_at(rows.take(j as int), m) } else { 0int }),
            forall|k: Major, m: Major| #[trigger] tp.at(k, m, true) == tp_start.at(k, m, true),
            forall|k: Major, m: Major| #[trigger] tl.at(k, m, false) == tl_start.at(k, m, false) -
                (if k.2 == 0 && m.0 == key && m.2 == 0 { cells_at(rows.take(j as int), (k.0, m.1, k.1)) } else { 0int }),
            forall|k: Major, m: Major| #[trigger] tl.at(k, m, true) == tl_start.at(k, m, true),
        decreases cells.len() - j,
    {
        let cell = cells[j];
        if cell.count == i128::MIN { return None; }
        let ghost tp_before = *tp;
        let ghost tl_before = *tl;
        if tp.add((key, 0, 0), (cell.first, cell.second, cell.third), -cell.count, 0).is_none() { return None; }
        if tl.add((cell.first, cell.third, 0), (key, cell.second, 0), -cell.count, 0).is_none() { return None; }
        proof {
            assert(rows.take(j as int + 1).drop_last() =~= rows.take(j as int));
            lemma_sum_take(rows, j as int, cell_minor_weight(minor_of(cell)));
            assert forall|k: Major, m: Major| #[trigger] tp.at(k, m, false) == tp_start.at(k, m, false) - (if k == (key, 0i64, 0i64) { cells_at(rows.take(j as int + 1), m) } else { 0int }) by {
                lemma_sum_take(rows, j as int, cell_minor_weight(m));
                assert(tp.at(k, m, false) == tp_before.at(k, m, false) + if k == (key, 0i64, 0i64) && m == minor_of(cell) { -(cell.count as int) } else { 0int });
                assert(cells_at(rows.take(j as int + 1), m) == cells_at(rows.take(j as int), m) + crate::table::cell_weight(cell, m, false));
            }
            assert forall|k: Major, m: Major| #[trigger] tl.at(k, m, false) == tl_start.at(k, m, false) -
                (if k.2 == 0 && m.0 == key && m.2 == 0 { cells_at(rows.take(j as int + 1), (k.0, m.1, k.1)) } else { 0int }) by {
                lemma_sum_take(rows, j as int, cell_minor_weight((k.0, m.1, k.1)));
                assert(tl.at(k, m, false) == tl_before.at(k, m, false) + if k == (cell.first, cell.third, 0i64) && m == (key, cell.second, 0i64) { -(cell.count as int) } else { 0int });
                assert(cells_at(rows.take(j as int + 1), (k.0, m.1, k.1)) == cells_at(rows.take(j as int), (k.0, m.1, k.1)) + crate::table::cell_weight(cell, (k.0, m.1, k.1), false));
            }
        }
        j += 1;
    }
    proof {
        assert(j == rows.len());
        assert(rows.take(j as int) =~= rows);
        assert forall|k: Major, m: Major| #[trigger] tp.at(k, m, false) == if k == (key, 0i64, 0i64) { 0int } else { tp_start.at(k, m, false) } by {
            if k == (key, 0i64, 0i64) {
                lemma_cells_at_sum(rows, m);
                assert(before.tp.at((key, 0, 0), m, false) == cell_sum(rows, m, false)) by { reveal(Table::at); }
            }
        }
        assert forall|k: Major, m: Major| #[trigger] tl.at(k, m, false) == if k.2 == 0 && m.0 == key { 0int } else { tl_start.at(k, m, false) } by {
            if k.2 == 0 && m.0 == key {
                if m.2 == 0 {
                    lemma_cells_at_sum(rows, (k.0, m.1, k.1));
                    assert(before.tp.at((key, 0, 0), (k.0, m.1, k.1), false) == cell_sum(rows, (k.0, m.1, k.1), false)) by { reveal(Table::at); }
                    assert(tl_start.at((k.0, k.1, 0), (key, m.1, 0), false) == before.tp.at((key, 0, 0), (k.0, m.1, k.1), false));
                    assert(k == (k.0, k.1, 0i64) && m == (key, m.1, 0i64));
                } else {
                    assert(tl_start.at((k.0, k.1, 0), (key, m.1, m.2), false) == 0);
                    assert(k == (k.0, k.1, 0i64) && m == (key, m.1, m.2));
                }
            }
        }
    }
    Some(())
}


/// Interns the strings of the rows into an existing dictionary (ids of existing strings stay).
pub fn intern_into(start: Dict, rows: &[BrassPartScan]) -> (result: Option<Dict>)
    requires start.valid(), start.len() <= 1000000,
    ensures result matches Some(dict) ==> dict.valid() && dict.len() <= 1000000 && dict.len() >= start.len() &&
        (forall|id: int| 0 <= id < start.len() ==> dict.strings@[id] == start.strings@[id]) &&
        forall|j: int| 0 <= j < rows@.len() ==> row_covered(dict, #[trigger] rows@[j]),
{
    let mut dict = start;
    let mut index = 0;
    while index < rows.len()
        invariant index <= rows.len(), dict.valid(), dict.len() <= 1000000, dict.len() >= start.len(),
            forall|id: int| 0 <= id < start.len() ==> dict.strings@[id] == start.strings@[id],
            forall|j: int| 0 <= j < index ==> row_covered(dict, #[trigger] rows@[j]),
        decreases rows.len() - index,
    {
        if dict.strings.len() >= 999990 { return None; }
        let ghost before = dict;
        assert(before.len() < 999990);
        let first = dict.intern(&rows[index].p_mfgr);
        let ghost mid = dict;
        let second = dict.intern(&rows[index].p_type);
        proof {
            assert forall|id: int| 0 <= id < start.len() implies dict.strings@[id] == start.strings@[id] by {
                assert(mid.strings@[id] == before.strings@[id]);
                assert(dict.strings@[id] == mid.strings@[id]);
            }
            assert forall|j: int| 0 <= j < index + 1 implies row_covered(dict, #[trigger] rows@[j]) by {
                if j < index as int {
                    let a = choose|id: int| 0 <= id < before.len() && before.text(id) == rows@[j].p_mfgr@;
                    let b = choose|id: int| 0 <= id < before.len() && before.text(id) == rows@[j].p_type@;
                    assert(mid.strings@[a] == before.strings@[a]);
                    assert(mid.strings@[b] == before.strings@[b]);
                    assert(dict.strings@[a] == mid.strings@[a]);
                    assert(dict.strings@[b] == mid.strings@[b]);
                    assert(dict.len() >= before.len());
                    assert(dict.text(a) == rows@[j].p_mfgr@);
                    assert(dict.text(b) == rows@[j].p_type@);
                } else {
                    assert(dict.text(first as int) == rows@[j].p_mfgr@) by { assert(dict.strings@[first as int] == mid.strings@[first as int]); }
                    assert(dict.text(second as int) == rows@[j].p_type@);
                }
            }
        }
        index += 1;
    }
    assert(index == rows@.len());
    Some(dict)
}


#[verifier::rlimit(80)]
/// The facts about the rebuilt tables give the post-condition of `apply_parts`.
#[verifier::rlimit(60)]
pub proof fn lemma_apply_parts_post(before: Parts, ps: Parts, dict: Dict, touched: Seq<i64>, rows: Seq<BrassPartScan>)
    requires before.structure_ok(), *ps.dict == dict, dict.valid(), dict.len() <= 1000000, dict.len() >= before.dl(),
        forall|id: int| 0 <= id < before.dl() ==> dict.strings@[id] == (*before.dict).strings@[id],
        ps.tp.valid(), ps.tl.valid(),
        forall|j: int| 0 <= j < rows.len() ==> row_covered(dict, #[trigger] rows[j]),
        forall|key: i64, size: i64, mid: i64, tid: i64| #[trigger] ps.tp.at((key, 0, 0), (size, mid, tid), false) ==
            if touched.contains(key) { tp_expected(dict, rows, key, size, mid, tid) } else { before.tp.at((key, 0, 0), (size, mid, tid), false) },
        forall|k: Major, m: Major| #[trigger] ps.tp.at(k, m, true) == 0,
        forall|k: Major, m: Major| #[trigger] ps.tl.at(k, m, true) == 0,
        forall|size: i64, tid: i64, key: i64, mid: i64, third: i64| #[trigger] ps.tl.at((size, tid, 0), (key, mid, third), false) ==
            if third == 0 { ps.tp.at((key, 0, 0), (size, mid, tid), false) } else { 0int },
    ensures
        ps.structure_ok(),
        forall|key: i64, size: i64, target: PartRow| #[trigger] ps.amount(key, size, target) ==
            if touched.contains(key) { part_scan_amount(rows, key, size, target) } else { before.amount(key, size, target) },
        forall|j: int| 0 <= j < rows.len() ==> ps.dict_has((#[trigger] rows[j]).p_mfgr@) && ps.dict_has(rows[j].p_type@),
        forall|text: Seq<u8>| before.dict_has(text) ==> #[trigger] ps.dict_has(text),
{
    assert(ps.dl() == dict.len());
    assert forall|key: i64, size: i64, mid: i64, tid: i64| !(0 <= mid < dict.len() && 0 <= tid < dict.len()) implies
        #[trigger] ps.tp.at((key, 0, 0), (size, mid, tid), false) == 0 by {
        if !touched.contains(key) {
            lemma_out_of_range_zero(before, key, size, mid, tid);
        }
    }
    lemma_tp_structure(ps.tp, dict.len());
    assert(ps.structure_ok());
    assert forall|text: Seq<u8>| before.dict_has(text) implies #[trigger] ps.dict_has(text) by {
        let id = choose|id: int| 0 <= id < before.dl() && before.dtext(id) == text;
        assert(dict.strings@[id] == (*before.dict).strings@[id]);
        assert(0 <= id < ps.dl() && ps.dtext(id) == text);
    }
    assert forall|j: int| 0 <= j < rows.len() implies ps.dict_has((#[trigger] rows[j]).p_mfgr@) && ps.dict_has(rows[j].p_type@) by {
        assert(row_covered(dict, rows[j]));
        lemma_dict_has_eq(ps, dict, rows[j].p_mfgr@);
        lemma_dict_has_eq(ps, dict, rows[j].p_type@);
    }
    assert forall|key: i64, size: i64, target: PartRow| #[trigger] ps.amount(key, size, target) ==
        if touched.contains(key) { part_scan_amount(rows, key, size, target) } else { before.amount(key, size, target) } by {
        if touched.contains(key) {
            if target.p_partkey == key as int && target.p_size == size as int && ps.dict_has(target.p_mfgr) && ps.dict_has(target.p_type) {
                let m = ps.dict_id(target.p_mfgr);
                let t = ps.dict_id(target.p_type);
                assert(0 <= m < dict.len() && 0 <= t < dict.len());
                assert(ps.tp.at((key, 0, 0), (size, m as i64, t as i64), false) == tp_expected(dict, rows, key, size, m as i64, t as i64));
                lemma_scan_amount_cells(dict, rows, key, size, target, m, t);
            } else {
                lemma_dict_has_eq(ps, dict, target.p_mfgr);
                lemma_dict_has_eq(ps, dict, target.p_type);
                lemma_scan_amount_zero(dict, rows, key, size, target);
            }
        } else {
            if target.p_partkey == key as int && target.p_size == size as int && ps.dict_has(target.p_mfgr) && ps.dict_has(target.p_type) {
                let m = ps.dict_id(target.p_mfgr);
                let t = ps.dict_id(target.p_type);
                assert(ps.amount(key, size, target) == ps.tp.at((key, 0, 0), (size, m as i64, t as i64), false));
                assert(ps.tp.at((key, 0, 0), (size, m as i64, t as i64), false) == before.tp.at((key, 0, 0), (size, m as i64, t as i64), false));
                if before.dict_has(target.p_mfgr) && before.dict_has(target.p_type) {
                    let m1 = before.dict_id(target.p_mfgr);
                    let t1 = before.dict_id(target.p_type);
                    assert(dict.strings@[m1] == (*before.dict).strings@[m1]);
                    assert(dict.strings@[t1] == (*before.dict).strings@[t1]);
                    assert(0 <= m1 < dict.len() && dict.text(m1) == target.p_mfgr);
                    assert(0 <= t1 < dict.len() && dict.text(t1) == target.p_type);
                    lemma_dict_ids_eq(dict, m1, m);
                    lemma_dict_ids_eq(dict, t1, t);
                    assert(before.amount(key, size, target) == before.tp.at((key, 0, 0), (size, m1 as i64, t1 as i64), false));
                } else {
                    assert(before.amount(key, size, target) == 0);
                    // one of the ids is beyond the old dictionary
                    if before.dl() > m && before.dl() > t {
                        assert(dict.strings@[m] == (*before.dict).strings@[m]);
                        assert(dict.strings@[t] == (*before.dict).strings@[t]);
                        assert(ps.dtext(m) == target.p_mfgr && ps.dtext(t) == target.p_type);
                        assert(before.dtext(m) == target.p_mfgr && before.dtext(t) == target.p_type);
                        assert(0 <= m < before.dl() && 0 <= t < before.dl());
                        assert(before.dict_has(target.p_mfgr));
                        assert(before.dict_has(target.p_type));
                    }
                    lemma_out_of_range_zero(before, key, size, m as i64, t as i64);
                }
            }
        }
    }
}

pub fn apply_parts(before: &Parts, touched: &[i64], rows: &[BrassPartScan]) -> (result: Option<Parts>)
    requires before.structure_ok(), touched@.no_duplicates(),
    ensures result matches Some(ps) ==> {
        &&& ps.structure_ok()
        &&& forall|key: i64, size: i64, target: PartRow| #[trigger] ps.amount(key, size, target) ==
            if touched@.contains(key) { part_scan_amount(rows@, key, size, target) } else { before.amount(key, size, target) }
        &&& forall|j: int| 0 <= j < rows@.len() ==> ps.dict_has((#[trigger] rows@[j]).p_mfgr@) && ps.dict_has(rows@[j].p_type@)
        &&& forall|text: Seq<u8>| before.dict_has(text) ==> #[trigger] ps.dict_has(text)
    },
{
    let start = (*before.dict).share();
    let dict = match intern_into(start, rows) { None => return None, Some(dict) => dict };
    let mut tp = Work::new(&before.tp);
    let mut tl = Work::new(&before.tl);
    // 1. take every touched key out of both tables
    let mut i = 0;
    while i < touched.len()
        invariant i <= touched.len(), tp.valid(), tl.valid(), before.structure_ok(), touched@.no_duplicates(),
            forall|key: i64, minor: Major| #[trigger] tp.at((key, 0, 0), minor, false) ==
                if touched@.take(i as int).contains(key) { 0int } else { before.tp.at((key, 0, 0), minor, false) },
            forall|k: Major, m: Major| #[trigger] tp.at(k, m, true) == 0,
            forall|k: Major, m: Major| #[trigger] tl.at(k, m, true) == 0,
            forall|size: i64, tid: i64, key: i64, mid: i64, third: i64| #[trigger] tl.at((size, tid, 0), (key, mid, third), false) ==
                if touched@.take(i as int).contains(key) { 0int } else { before.tl.at((size, tid, 0), (key, mid, third), false) },
        decreases touched.len() - i,
    {
        let key = touched[i];
        let ghost tp_before = tp;
        let ghost tl_before = tl;
        proof {
            assert(!touched@.take(i as int).contains(key)) by {
                if touched@.take(i as int).contains(key) {
                    let p = choose|p: int| 0 <= p < i && touched@.take(i as int)[p] == key;
                    assert(touched@[p] == key);
                }
            }
            assert forall|size: i64, tid: i64, mid: i64, third: i64| #[trigger] tl.at((size, tid, 0), (key, mid, third), false) ==
                if third == 0 { before.tp.at((key, 0, 0), (size, mid, tid), false) } else { 0int } by {
                assert(before.tl.at((size, tid, 0), (key, mid, third), false) ==
                    if third == 0 { before.tp.at((key, 0, 0), (size, mid, tid), false) } else { 0int });
            }
        }
        if remove_key(&mut tp, &mut tl, before, key).is_none() { return None; }
        proof {
            assert(touched@.take(i as int + 1) =~= touched@.take(i as int).push(key));
            assert forall|k: i64, minor: Major| #[trigger] tp.at((k, 0, 0), minor, false) ==
                if touched@.take(i as int + 1).contains(k) { 0int } else { before.tp.at((k, 0, 0), minor, false) } by {
                broadcast use vstd::seq_lib::group_seq_properties;
                assert(tp.at((k, 0, 0), minor, false) == if (k, 0i64, 0i64) == (key, 0i64, 0i64) { 0int } else { tp_before.at((k, 0, 0), minor, false) });
            }
            assert forall|size: i64, tid: i64, k: i64, mid: i64, third: i64| #[trigger] tl.at((size, tid, 0), (k, mid, third), false) ==
                if touched@.take(i as int + 1).contains(k) { 0int } else { before.tl.at((size, tid, 0), (k, mid, third), false) } by {
                broadcast use vstd::seq_lib::group_seq_properties;
                assert(tl.at((size, tid, 0), (k, mid, third), false) ==
                    if (0i64 == 0 && k == key) { 0int } else { tl_before.at((size, tid, 0), (k, mid, third), false) });
            }
        }
        i += 1;
    }
    proof { assert(touched@.take(i as int) =~= touched@); }
    // 2. add the rescanned rows of the touched keys
    let mut r = 0;
    while r < rows.len()
        invariant r <= rows.len(), tp.valid(), tl.valid(), dict.valid(), dict.len() <= 1000000, before.structure_ok(),
            forall|j: int| 0 <= j < rows@.len() ==> row_covered(dict, #[trigger] rows@[j]),
            forall|key: i64, size: i64, mid: i64, tid: i64| #[trigger] tp.at((key, 0, 0), (size, mid, tid), false) ==
                if touched@.contains(key) { tp_expected(dict, rows@.take(r as int), key, size, mid, tid) }
                else { before.tp.at((key, 0, 0), (size, mid, tid), false) },
            forall|k: Major, m: Major| #[trigger] tp.at(k, m, true) == 0,
            forall|k: Major, m: Major| #[trigger] tl.at(k, m, true) == 0,
            forall|size: i64, tid: i64, key: i64, mid: i64, third: i64| #[trigger] tl.at((size, tid, 0), (key, mid, third), false) ==
                if third == 0 { tp.at((key, 0, 0), (size, mid, tid), false) } else { 0int },
        decreases rows.len() - r,
    {
        let row = &rows[r];
        if crate::maintenance::contains_key(touched, row.p_partkey) {
            let mid = match dict.find(&row.p_mfgr) { Some(id) => id, None => { proof { assert(row_covered(dict, rows@[r as int])); } return None; } };
            let tid = match dict.find(&row.p_type) { Some(id) => id, None => { proof { assert(row_covered(dict, rows@[r as int])); } return None; } };
            proof { assert(mid < 1000000 && tid < 1000000); }
            let ghost tp_before = tp;
            let ghost tl_before = tl;
            if tp.add((row.p_partkey, 0, 0), (row.p_size, mid as i64, tid as i64), 1, 0).is_none() { return None; }
            if tl.add((row.p_size, tid as i64, 0), (row.p_partkey, mid as i64, 0), 1, 0).is_none() { return None; }
            proof {
                assert forall|key: i64, size: i64, m: i64, t: i64| #[trigger] tp.at((key, 0, 0), (size, m, t), false) ==
                    if touched@.contains(key) { tp_expected(dict, rows@.take(r as int + 1), key, size, m, t) }
                    else { before.tp.at((key, 0, 0), (size, m, t), false) } by {
                    assert(tp.at((key, 0, 0), (size, m, t), false) == tp_before.at((key, 0, 0), (size, m, t), false)
                        + if (key, 0i64, 0i64) == (row.p_partkey, 0i64, 0i64) && (size, m, t) == (row.p_size, mid as i64, tid as i64) { 1int } else { 0int });
                    if touched@.contains(key) {
                        if 0 <= m < dict.len() && 0 <= t < dict.len() {
                            lemma_sum_take(rows@, r as int, row_weight(key, size, dict.text(m as int), dict.text(t as int)));
                            if row.p_partkey == key && row.p_size == size && row.p_mfgr@ == dict.text(m as int) && row.p_type@ == dict.text(t as int) {
                                assert(dict.text(mid as int) == row.p_mfgr@);
                                lemma_dict_ids_eq(dict, mid as int, m as int);
                                lemma_dict_ids_eq(dict, tid as int, t as int);
                            }
                            if (size, m, t) == (row.p_size, mid as i64, tid as i64) && key == row.p_partkey {
                                assert(dict.text(m as int) == row.p_mfgr@);
                                assert(dict.text(t as int) == row.p_type@);
                            }
                        } else {
                            assert((size, m, t) != (row.p_size, mid as i64, tid as i64));
                        }
                    } else {
                        assert(key != row.p_partkey);
                    }
                }
                assert forall|k: Major, m: Major| #[trigger] tp.at(k, m, true) == 0 by { assert(tp.at(k, m, true) == tp_before.at(k, m, true) + 0); }
                assert forall|k: Major, m: Major| #[trigger] tl.at(k, m, true) == 0 by { assert(tl.at(k, m, true) == tl_before.at(k, m, true) + 0); }
                assert forall|size: i64, t: i64, key: i64, m: i64, third: i64| #[trigger] tl.at((size, t, 0), (key, m, third), false) ==
                    if third == 0 { tp.at((key, 0, 0), (size, m, t), false) } else { 0int } by {
                    assert(tl.at((size, t, 0), (key, m, third), false) == tl_before.at((size, t, 0), (key, m, third), false)
                        + if (size, t, 0i64) == (row.p_size, tid as i64, 0i64) && (key, m, third) == (row.p_partkey, mid as i64, 0i64) { 1int } else { 0int });
                    assert(tp.at((key, 0, 0), (size, m, t), false) == tp_before.at((key, 0, 0), (size, m, t), false)
                        + if (key, 0i64, 0i64) == (row.p_partkey, 0i64, 0i64) && (size, m, t) == (row.p_size, mid as i64, tid as i64) { 1int } else { 0int });
                }
            }
        } else {
            proof {
                assert forall|key: i64, size: i64, m: i64, t: i64| #[trigger] tp.at((key, 0, 0), (size, m, t), false) ==
                    if touched@.contains(key) { tp_expected(dict, rows@.take(r as int + 1), key, size, m, t) }
                    else { before.tp.at((key, 0, 0), (size, m, t), false) } by {
                    if touched@.contains(key) && 0 <= m < dict.len() && 0 <= t < dict.len() {
                        lemma_sum_take(rows@, r as int, row_weight(key, size, dict.text(m as int), dict.text(t as int)));
                        assert(key != row.p_partkey);
                    }
                }
            }
        }
        r += 1;
    }
    proof { assert(r == rows@.len()); assert(rows@.take(r as int) =~= rows@); }
    let ghost tpg = tp;
    let ghost tlg = tl;
    let tpt = tp.finish();
    let tlt = tl.finish();
    let ps = Parts { dict: Arc::new(dict), tp: tpt, tl: tlt };
    proof {
        assert forall|key: i64, size: i64, mid: i64, tid: i64| #[trigger] ps.tp.at((key, 0, 0), (size, mid, tid), false) ==
            if touched@.contains(key) { tp_expected(dict, rows@, key, size, mid, tid) } else { before.tp.at((key, 0, 0), (size, mid, tid), false) } by {
            assert(ps.tp.at((key, 0, 0), (size, mid, tid), false) == tpg.at((key, 0, 0), (size, mid, tid), false));
        }
        assert forall|k: Major, m: Major| #[trigger] ps.tp.at(k, m, true) == 0 by { assert(ps.tp.at(k, m, true) == tpg.at(k, m, true)); }
        assert forall|k: Major, m: Major| #[trigger] ps.tl.at(k, m, true) == 0 by { assert(ps.tl.at(k, m, true) == tlg.at(k, m, true)); }
        assert forall|size: i64, tid: i64, key: i64, mid: i64, third: i64| #[trigger] ps.tl.at((size, tid, 0), (key, mid, third), false) ==
            if third == 0 { ps.tp.at((key, 0, 0), (size, mid, tid), false) } else { 0int } by {
            assert(ps.tl.at((size, tid, 0), (key, mid, third), false) == tlg.at((size, tid, 0), (key, mid, third), false));
            assert(ps.tp.at((key, 0, 0), (size, mid, tid), false) == tpg.at((key, 0, 0), (size, mid, tid), false));
        }
        lemma_apply_parts_post(*before, ps, dict, touched@, rows@);
    }
    Some(ps)
}


}
