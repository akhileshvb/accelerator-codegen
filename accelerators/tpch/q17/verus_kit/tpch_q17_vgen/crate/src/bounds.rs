use vstd::prelude::*;
use verus_common::sums::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::aggregation::*;

verus! {

pub proof fn lemma_sum_le_len<A>(rows: Seq<A>, f: spec_fn(A) -> int)
    requires forall|row: A| 0 <= #[trigger] f(row) <= 1,
    ensures 0 <= sum(rows, f) <= rows.len(),
{
    let one = |row: A| 1int;
    verus_prooflib::lemma_sum_ones(rows);
    lemma_sum_nonneg(rows, f);
    lemma_sum_le(rows, f, one);
}

pub proof fn lemma_scan_len(scanned: Seq<LineitemRow>, database: Seq<LineitemRow>)
    requires forall|weight: spec_fn(LineitemRow) -> int| #[trigger] sum(scanned, weight) == sum(database, weight),
    ensures scanned.len() == database.len(),
{
    let one = |row: LineitemRow| 1int;
    verus_prooflib::lemma_sum_ones(scanned);
    verus_prooflib::lemma_sum_ones(database);
    assert(sum(scanned, one) == sum(database, one));
}

pub proof fn lemma_scan_parts_len(db: Db, rows: Seq<PartInput>)
    requires scan_parts_ok(db, rows), envelope(db),
    ensures rows.len() <= 9223372036854775808,
{
    let one = |row: PartRow| 1int;
    verus_prooflib::lemma_sum_ones(part_input_seq_view(rows));
    verus_prooflib::lemma_sum_ones(db.part);
    assert(sum(part_input_seq_view(rows), one) == sum(db.part, one));
}

pub open spec fn in_range(row: LineitemRow) -> bool {
    -value_bound() <= row.l_quantity <= value_bound() && -value_bound() <= row.l_extendedprice <= value_bound()
}

pub proof fn lemma_scan_bounded(db: Db, rows: Seq<LineInput>)
    requires scan_lines_ok(db, rows), envelope(db),
    ensures rows_bounded(rows),
{
    lemma_scan_len(line_input_seq_view(rows), db.lineitem);
    let bad = |row: LineitemRow| if in_range(row) { 0int } else { 1int };
    lemma_sum_zero(db.lineitem, bad);
    assert(sum(line_input_seq_view(rows), bad) == 0);
    assert forall|index: int| #![trigger rows[index]] 0 <= index < rows.len() implies
        -value_bound() <= rows[index].l_quantity <= value_bound() && -value_bound() <= rows[index].l_extendedprice <= value_bound() by {
        let view = line_input_seq_view(rows);
        assert(view[index] == line_input_view(rows[index]));
        if !in_range(view[index]) { lemma_sum_pos(view, bad, index); }
    }
}

pub proof fn lemma_rescan_bounded(db: Db, keys: Seq<i64>, rows: Seq<LineInput>)
    requires rescan_lines_for_parts_ok(db, keys, rows), envelope(db),
    ensures rows_bounded(rows),
{
    let view = line_input_seq_view(rows);
    let selected = |row: LineitemRow| if seq_view_i64(keys).contains(row.l_partkey) { 1int } else { 0int };
    let one = |row: LineitemRow| 1int;
    let picked = |row: LineitemRow| if seq_view_i64(keys).contains(row.l_partkey) { one(row) } else { 0int };
    verus_prooflib::lemma_sum_ones(view);
    assert(sum(view, one) == sum(db.lineitem, picked));
    assert(picked =~= selected);
    lemma_sum_le_len(db.lineitem, selected);
    let bad = |row: LineitemRow| if in_range(row) { 0int } else { 1int };
    let picked_bad = |row: LineitemRow| if seq_view_i64(keys).contains(row.l_partkey) { bad(row) } else { 0int };
    lemma_sum_zero(db.lineitem, picked_bad);
    assert(sum(view, bad) == sum(db.lineitem, picked_bad));
    assert forall|index: int| #![trigger rows[index]] 0 <= index < rows.len() implies
        -value_bound() <= rows[index].l_quantity <= value_bound() && -value_bound() <= rows[index].l_extendedprice <= value_bound() by {
        assert(view[index] == line_input_view(rows[index]));
        if !in_range(view[index]) { lemma_sum_pos(view, bad, index); }
    }
}

}
