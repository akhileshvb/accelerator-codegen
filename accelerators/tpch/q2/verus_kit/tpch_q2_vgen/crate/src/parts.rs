use vstd::prelude::*;
use std::sync::Arc;
use verus_common::sums::*;
use verus_prooflib::{class_total, lemma_equal_classes};
use crate::gen_spec::*;
use crate::model::{retained, fits_i64};
use crate::dict::*;
use crate::table::{Table, Cell, Major, rows_valid, minor_of, cell_sum};

verus! {

/// The parts of the database: strings interned in `dict`; `tp` keyed (partkey, 0, 0) holds one
/// cell (size, mfgr id, type id) per distinct part row (count = copies); `tl` keyed
/// (size, type id, 0) holds one cell (partkey, mfgr id, 0) per distinct part row of that size and type.
pub struct Parts {
    pub dict: Arc<Dict>,
    pub tp: Table,
    pub tl: Table,
}

/// The number of database rows equal to `target` with part key `key` and size `size`.
pub open spec fn amount_weight(key: i64, size: i64, target: PartRow) -> spec_fn(PartRow) -> int {
    |row: PartRow| if retained(row) && row.p_partkey == key as int && row.p_size == size as int && row == target { 1int } else { 0int }
}

pub open spec fn db_amount(db: Db, key: i64, size: i64, target: PartRow) -> int {
    sum(db.part, amount_weight(key, size, target))
}

impl Parts {
    pub open spec fn dl(self) -> int { (*self.dict).strings@.len() as int }

    pub open spec fn dtext(self, id: int) -> Seq<u8> { (*(*self.dict).strings@[id])@ }

    pub open spec fn dict_has(self, s: Seq<u8>) -> bool {
        exists|id: int| 0 <= id < self.dl() && self.dtext(id) == s
    }

    pub open spec fn dict_id(self, s: Seq<u8>) -> int {
        choose|id: int| 0 <= id < self.dl() && self.dtext(id) == s
    }

    pub open spec fn tp_rows(self, key: i64) -> Seq<Cell> { self.tp.rows((key, 0, 0)) }

    pub open spec fn cell_part(self, key: i64, cell: Cell) -> PartRow {
        PartRow { p_partkey: key as int, p_mfgr: self.dtext(dec(cell.second)), p_type: self.dtext(dec(cell.third)), p_size: cell.first as int }
    }

    /// The number of rows equal to `row` with part key `key` and size `size`.
    pub open spec fn amount(self, key: i64, size: i64, row: PartRow) -> int {
        if row.p_partkey == key as int && row.p_size == size as int && self.dict_has(row.p_mfgr) && self.dict_has(row.p_type) {
            self.tp.at((key, 0, 0), (size, enc(self.dict_id(row.p_mfgr)), enc(self.dict_id(row.p_type))), false)
        } else { 0 }
    }

    pub open spec fn structure_ok(self) -> bool {
        &&& (*self.dict).valid()
        &&& self.tp.valid()
        &&& self.tl.valid()
        &&& forall|key: i64, index: int| 0 <= index < self.tp_rows(key).len() ==>
            dec((#[trigger] self.tp_rows(key)[index]).second) < self.dl() && dec(self.tp_rows(key)[index].third) < self.dl()
        &&& forall|key: Major, minor: Major| #[trigger] self.tp.at(key, minor, true) == 0
        &&& forall|key: Major, minor: Major| #[trigger] self.tl.at(key, minor, true) == 0
        &&& forall|size: i64, tid: i64, key: i64, mid: i64, third: i64| #[trigger] self.tl.at((size, tid, 0), (key, mid, third), false) ==
            if third == 0 { self.tp.at((key, 0, 0), (size, mid, tid), false) } else { 0int }
    }

    /// Every cell of the part table counts at least one row.
    pub open spec fn cells_positive(self) -> bool {
        forall|key: i64, index: int| 0 <= index < self.tp_rows(key).len() ==> (#[trigger] self.tp_rows(key)[index]).count > 0
    }

    pub open spec fn meaning(self, db: Db) -> bool {
        &&& self.structure_ok()
        &&& forall|key: i64, size: i64, target: PartRow| #[trigger] self.amount(key, size, target) == db_amount(db, key, size, target)
        &&& forall|index: int| 0 <= index < db.part.len() ==>
            self.dict_has((#[trigger] db.part[index]).p_mfgr) && self.dict_has(db.part[index].p_type)
    }
}

/// A dictionary id and its table key stand for each other.
pub proof fn lemma_id_key(ps: Parts, id: int)
    requires (*ps.dict).valid(), 0 <= id < ps.dl(),
    ensures dec(enc(id)) == id,
{
    lemma_enc_dec(id);
}

/// A dictionary id outside the dictionary has no cells.
pub proof fn lemma_out_of_range_zero(ps: Parts, key: i64, size: i64, mid: i64, tid: i64)
    requires ps.structure_ok(), !(dec(mid) < ps.dl() && dec(tid) < ps.dl()),
    ensures ps.tp.at((key, 0, 0), (size, mid, tid), false) == 0,
{
    ps.tp.lemma_shaped_valid();
    let rows = ps.tp_rows(key);
    let minor = (size, mid, tid);
    assert(ps.tp.at((key, 0, 0), minor, false) == cell_sum(rows, minor, false)) by { reveal(Table::at); }
    reveal(cell_sum);
    assert forall|index: int| 0 <= index < rows.len() implies
        (|cell: Cell| crate::table::cell_weight(cell, minor, false))(#[trigger] rows[index]) == 0 by {
        assert(dec(rows[index].second) < ps.dl() && dec(rows[index].third) < ps.dl());
        assert(minor_of(rows[index]) != minor);
    }
    lemma_sum_zero(rows, |cell: Cell| crate::table::cell_weight(cell, minor, false));
}

pub proof fn lemma_dict_id(ps: Parts, id: int)
    requires (*ps.dict).valid(), 0 <= id < ps.dl(),
    ensures ps.dict_has(ps.dtext(id)), ps.dict_id(ps.dtext(id)) == id,
{
    assert(ps.dict_has(ps.dtext(id)));
    let chosen = ps.dict_id(ps.dtext(id));
    if chosen != id {
        if chosen < id { assert((*(*ps.dict).strings@[chosen])@ != (*(*ps.dict).strings@[id])@); }
        else { assert((*(*ps.dict).strings@[id])@ != (*(*ps.dict).strings@[chosen])@); }
    }
}

/// Texts with the same id are equal; equal texts have the same id.
pub proof fn lemma_dict_text_eq(ps: Parts, first: int, second: int)
    requires (*ps.dict).valid(), 0 <= first < ps.dl(), 0 <= second < ps.dl(), ps.dtext(first) == ps.dtext(second),
    ensures first == second,
{
    if first < second { assert((*(*ps.dict).strings@[first])@ != (*(*ps.dict).strings@[second])@); }
    if second < first { assert((*(*ps.dict).strings@[second])@ != (*(*ps.dict).strings@[first])@); }
}

/// No cell of a part key stands for a part that is not in the dictionary or has another key.
pub proof fn lemma_cell_part_facts(ps: Parts, key: i64, cell: Cell)
    requires (*ps.dict).valid(), dec(cell.second) < ps.dl(), dec(cell.third) < ps.dl(),
    ensures ps.cell_part(key, cell).p_partkey == key as int, ps.dict_has(ps.cell_part(key, cell).p_mfgr),
        ps.dict_has(ps.cell_part(key, cell).p_type), fits_i64(ps.cell_part(key, cell).p_size),
{
    assert(ps.dict_has(ps.dtext(dec(cell.second))));
    assert(ps.dict_has(ps.dtext(dec(cell.third))));
}

#[verifier::auto_ext_equal(assert, assert_by, ensures)]
pub proof fn lemma_part_class(db: Db, ps: Parts, key: i64, x: PartRow,
    left_project: spec_fn(PartRow) -> PartRow, right_project: spec_fn(Cell) -> PartRow,
    left_count: spec_fn(PartRow) -> int, right_count: spec_fn(Cell) -> int)
    requires ps.meaning(db), forall|index: int| 0 <= index < db.part.len() ==> fits_i64((#[trigger] db.part[index]).p_size),
        forall|row: PartRow| #[trigger] left_project(row) == row,
        forall|row: PartRow| #[trigger] left_count(row) == if row.p_partkey == key as int { 1int } else { 0int },
        forall|cell: Cell| #[trigger] right_project(cell) == ps.cell_part(key, cell),
        forall|cell: Cell| #[trigger] right_count(cell) == cell.count as int,
    ensures
        class_total(db.part, left_project, left_count, x) == class_total(ps.tp_rows(key), right_project, right_count, x),
{
    let rows = ps.tp_rows(key);
    assert(rows_valid(rows)) by { ps.tp.lemma_shaped_valid(); }
    let source = |row: PartRow| if row == x { left_count(row) } else { 0int };
    let right_source = |cell: Cell| if right_project(cell) == x { right_count(cell) } else { 0int };
    verus_prooflib::lemma_class_total_weight(db.part, left_project, left_count, x, source);
    verus_prooflib::lemma_class_total_weight(rows, right_project, right_count, x, right_source);
    if x.p_partkey != key as int {
        lemma_sum_zero(db.part, source);
        assert forall|index: int| 0 <= index < rows.len() implies right_source(#[trigger] rows[index]) == 0 by {
            lemma_cell_part_facts(ps, key, rows[index]);
        }
        lemma_sum_zero(rows, right_source);
    } else if ps.dict_has(x.p_mfgr) && ps.dict_has(x.p_type) && fits_i64(x.p_size) {
        let size = x.p_size as i64;
        let mid = ps.dict_id(x.p_mfgr);
        let tid = ps.dict_id(x.p_type);
        assert(ps.amount(key, size, x) == ps.tp.at((key, 0, 0), (size, enc(mid), enc(tid)), false));
        lemma_sum_ext_all(db.part, source,
            |row: PartRow| if retained(row) && row.p_partkey == key as int && row.p_size == size as int && row == x { 1int } else { 0int });
        let minor = (size, enc(mid), enc(tid));
        assert forall|index: int| 0 <= index < rows.len() implies
            right_source(#[trigger] rows[index]) == crate::table::cell_weight(rows[index], minor, false) by {
            let cell = rows[index];
            assert(dec(cell.second) < ps.dl() && dec(cell.third) < ps.dl());
            assert(0 <= mid < ps.dl() && 0 <= tid < ps.dl());
            assert(ps.dtext(mid) == x.p_mfgr && ps.dtext(tid) == x.p_type);
            if right_project(cell) == x {
                lemma_dict_text_eq(ps, dec(cell.second), mid);
                lemma_dict_text_eq(ps, dec(cell.third), tid);
                assert(cell.first == size);
                assert(minor_of(cell) == minor);
            } else if minor_of(cell) == minor {
                lemma_id_key(ps, mid);
                lemma_id_key(ps, tid);
                assert(dec(cell.second) == mid && dec(cell.third) == tid);
                assert(right_project(cell) == x);
            }
        }
        lemma_sum_ext(rows, right_source, |cell: Cell| crate::table::cell_weight(cell, minor, false));
        assert(ps.tp.at((key, 0, 0), minor, false) == cell_sum(rows, minor, false)) by { reveal(Table::at); }
        assert(cell_sum(rows, minor, false) == sum(rows, |cell: Cell| crate::table::cell_weight(cell, minor, false))) by { reveal(cell_sum); }
        assert(sum(rows, right_source) == ps.tp.at((key, 0, 0), minor, false));
        assert(ps.amount(key, size, x) == db_amount(db, key, size, x));
        lemma_sum_ext_all(db.part, source, amount_weight(key, size, x));
        assert(sum(db.part, source) == ps.amount(key, size, x));
    } else {
        assert forall|index: int| 0 <= index < db.part.len() implies source(#[trigger] db.part[index]) == 0 by {
            assert(ps.dict_has(db.part[index].p_mfgr) && ps.dict_has(db.part[index].p_type));
            assert(fits_i64(db.part[index].p_size));
        }
        lemma_sum_zero(db.part, source);
        assert forall|index: int| 0 <= index < rows.len() implies right_source(#[trigger] rows[index]) == 0 by {
            lemma_cell_part_facts(ps, key, rows[index]);
        }
        lemma_sum_zero(rows, right_source);
    }
}

/// The sum over the cells of a part key equals the sum over the database rows with that key.
#[verifier::auto_ext_equal(assert, assert_by, ensures)]
pub proof fn lemma_cells_weight(db: Db, ps: Parts, key: i64, weight: spec_fn(PartRow) -> int)
    requires ps.meaning(db), forall|index: int| 0 <= index < db.part.len() ==> fits_i64((#[trigger] db.part[index]).p_size),
    ensures sum(ps.tp_rows(key), |cell: Cell| cell.count as int * weight(ps.cell_part(key, cell))) ==
        sum(db.part, |row: PartRow| if row.p_partkey == key as int { weight(row) } else { 0int }),
{
    let rows = ps.tp_rows(key);
    let left_project = |row: PartRow| row;
    let right_project = |cell: Cell| ps.cell_part(key, cell);
    let left_count = |row: PartRow| if row.p_partkey == key as int { 1int } else { 0int };
    let right_count = |cell: Cell| cell.count as int;
    assert forall|x: PartRow| #[trigger] class_total(db.part, left_project, left_count, x) == class_total(rows, right_project, right_count, x) by {
        lemma_part_class(db, ps, key, x, left_project, right_project, left_count, right_count);
    }
    lemma_equal_classes(db.part, rows, left_project, right_project, left_count, right_count, weight);
    lemma_sum_ext_all(db.part, |row: PartRow| left_count(row) * weight(left_project(row)),
        |row: PartRow| if row.p_partkey == key as int { weight(row) } else { 0int });
    lemma_sum_ext_all(rows, |cell: Cell| right_count(cell) * weight(right_project(cell)),
        |cell: Cell| cell.count as int * weight(ps.cell_part(key, cell)));
}


/// Every cell of a part key has a positive count.
pub proof fn lemma_tp_cell_positive(db: Db, ps: Parts, key: i64, index: int)
    requires ps.meaning(db), 0 <= index < ps.tp_rows(key).len(),
    ensures ps.tp_rows(key)[index].count > 0,
{
    ps.tp.lemma_shaped_valid();
    let rows = ps.tp_rows(key);
    let cell = rows[index];
    let minor = minor_of(cell);
    crate::table::lemma_cell_lookup(rows, minor, index);
    assert(ps.tp.at((key, 0, 0), minor, true) == cell_sum(rows, minor, true)) by { reveal(Table::at); }
    assert(ps.tp.at((key, 0, 0), minor, false) == cell_sum(rows, minor, false)) by { reveal(Table::at); }
    assert(cell.volume == 0);
    assert(cell.count != 0);
    assert(dec(cell.second) < ps.dl() && dec(cell.third) < ps.dl());
    lemma_dict_id(ps, dec(cell.second));
    lemma_dict_id(ps, dec(cell.third));
    let row = ps.cell_part(key, cell);
    assert(row.p_mfgr == ps.dtext(dec(cell.second)));
    assert(ps.amount(key, cell.first, row) == ps.tp.at((key, 0, 0), (cell.first, cell.second, cell.third), false));
    assert(minor == (cell.first, cell.second, cell.third));
    assert(ps.amount(key, cell.first, row) == db_amount(db, key, cell.first, row));
    lemma_sum_nonneg(db.part, amount_weight(key, cell.first, row));
    assert(db_amount(db, key, cell.first, row) >= 0);
}

/// The database has a part with this key exactly when the key has cells.
#[verifier::auto_ext_equal(assert, assert_by, ensures)]
pub proof fn lemma_eligible_cells(db: Db, ps: Parts, key: i64)
    requires ps.meaning(db), forall|index: int| 0 <= index < db.part.len() ==> fits_i64((#[trigger] db.part[index]).p_size),
    ensures (crate::model::eligible(db, key) > 0) == (ps.tp_rows(key).len() > 0), crate::model::eligible(db, key) >= 0,
{
    let rows = ps.tp_rows(key);
    let one = |row: PartRow| 1int;
    lemma_cells_weight(db, ps, key, one);
    assert forall|index: int| 0 <= index < rows.len() implies (|cell: Cell| cell.count as int * one(ps.cell_part(key, cell)))(#[trigger] rows[index]) >= 0 by {
        lemma_tp_cell_positive(db, ps, key, index);
    }
    lemma_sum_ext_all(db.part, |row: PartRow| if row.p_partkey == key as int { one(row) } else { 0int },
        |row: PartRow| if row.p_partkey == key as int && retained(row) { 1int } else { 0int });
    lemma_sum_nonneg(rows, |cell: Cell| cell.count as int * one(ps.cell_part(key, cell)));
    if rows.len() == 0 {
        lemma_sum_zero(rows, |cell: Cell| cell.count as int * one(ps.cell_part(key, cell)));
    } else {
        lemma_tp_cell_positive(db, ps, key, 0);
        lemma_sum_pos(rows, |cell: Cell| cell.count as int * one(ps.cell_part(key, cell)), 0);
    }
}

/// The meaning of a database gives positive cells.
pub proof fn lemma_meaning_positive(db: Db, ps: Parts)
    requires ps.meaning(db),
    ensures ps.cells_positive(),
{
    assert forall|key: i64, index: int| 0 <= index < ps.tp_rows(key).len() implies (#[trigger] ps.tp_rows(key)[index]).count > 0 by {
        lemma_tp_cell_positive(db, ps, key, index);
    }
}

}
