use vstd::prelude::*;
use verus_common::sums::*;
use verus_prooflib::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::model::*;
use crate::storage::*;

verus! {

broadcast use vstd::seq_lib::group_seq_properties;

pub open spec fn delta_part_moment(full: Seq<(PartRow, int)>, key: int, class: u8, brand: Seq<u8>) -> int {
    sum(full, |change: (PartRow, int)| change.1 * ind(change.0.p_partkey == key && class_matches(change.0, class) && change.0.p_brand == brand))
}

pub proof fn lemma_part_projection(rows: Seq<PartDelta>, full: Seq<(PartRow, int)>, key: int, class: u8, brand: Seq<u8>)
    requires part_delta_ok(rows, full),
    ensures part_delta_prefix(rows, rows.len() as int, key, class, brand) == delta_part_moment(full, key, class, brand),
{
    let input = |row: PartDelta| part_delta_weight(row, key, class, brand);
    let weight = |change: (PartRow, int)| change.1 * ind(change.0.p_partkey == key && class_matches(change.0, class) && change.0.p_brand == brand);
    assert forall|index: int| #![trigger rows[index], full[index]] 0 <= index < rows.len() implies input(rows[index]) == weight(full[index]) by {
        if class == 0 {} else if class == 1 {} else if class == 2 {}
    }
    lemma_sum_aligned(rows, full, input, weight);
}

pub proof fn lemma_part_capture(db: Db, db2: Db, rows: Seq<PartDelta>, full: Seq<(PartRow, int)>, key: int, class: u8, brand: Seq<u8>)
    requires part_delta_ok(rows, full), bag_step(db.part, db2.part, full),
    ensures part_sum(db2, key, class, brand) == part_sum(db, key, class, brand) + part_delta_prefix(rows, rows.len() as int, key, class, brand),
{
    reveal(part_sum);
    let weight = |part: PartRow| ind(part.p_partkey == key && class_matches(part, class) && part.p_brand == brand);
    assert(sum(db2.part, weight) == sum(db.part, weight) + sum(full, |change: (PartRow, int)| change.1 * weight(change.0)));
    lemma_part_projection(rows, full, key, class, brand);
    lemma_sum_ext_all(full, |change: (PartRow, int)| change.1 * weight(change.0),
        |change: (PartRow, int)| change.1 * ind(change.0.p_partkey == key && class_matches(change.0, class) && change.0.p_brand == brand));
}

pub proof fn lemma_line_part(db: Db, line: LineitemRow, class: u8, brand: Seq<u8>, quantity: int, revenue: bool)
    ensures sum(db.part, |part: PartRow| cell_weight(part, line, class, brand, quantity, revenue))
        == if eligible(line) && line.l_quantity == quantity { part_sum(db, line.l_partkey, class, brand) * line_weight(line, revenue) } else { 0 },
{
    reveal(part_sum);
    let weight = |part: PartRow| cell_weight(part, line, class, brand, quantity, revenue);
    if eligible(line) && line.l_quantity == quantity {
        let count = |part: PartRow| ind(part.p_partkey == line.l_partkey && class_matches(part, class) && part.p_brand == brand);
        let value = line_weight(line, revenue);
        assert forall|part: PartRow| #[trigger] weight(part) == value * count(part) by {}
        lemma_sum_scale(db.part, count, weight, value);
        let total = part_sum(db, line.l_partkey, class, brand);
        assert(value * total == total * value) by (nonlinear_arith);
    } else { lemma_sum_zero(db.part, weight); }
}

pub proof fn lemma_hist_join(db: Db, class: u8, brand: Seq<u8>, quantity: int, revenue: bool)
    ensures hist_sum(db, class, brand, quantity, revenue) == join_sum(db.lineitem, db.part, join_weight(class, brand, quantity, revenue)),
{
    let matrix = join_weight(class, brand, quantity, revenue);
    let moment = |line: LineitemRow| if eligible(line) && line.l_quantity == quantity {
        part_sum(db, line.l_partkey, class, brand) * line_weight(line, revenue)
    } else { 0 };
    crate::database::lemma_hist_line(db, class, brand, quantity, revenue);
    assert forall|line: LineitemRow| #[trigger] moment(line) == sum(db.part, |part: PartRow| matrix(line, part)) by {
        lemma_line_part(db, line, class, brand, quantity, revenue);
        lemma_sum_ext_all(db.part, |part: PartRow| cell_weight(part, line, class, brand, quantity, revenue), |part: PartRow| matrix(line, part));
    }
    lemma_sum_ext_all(db.lineitem, moment, |line: LineitemRow| sum(db.part, |part: PartRow| matrix(line, part)));
}

pub proof fn lemma_line_capture(db: Db, parts: Store<PartCell>, rows: Seq<LineDelta>, full: Seq<(LineitemRow, int)>,
    class: u8, brand: Seq<u8>, quantity: int, revenue: bool)
    requires lineitem_delta_ok(rows, full),
        forall|key: int| #![trigger part_value(parts, key, class, brand)] part_value(parts, key, class, brand) == part_sum(db, key, class, brand),
    ensures line_delta_prefix(parts, rows, rows.len() as int, class, brand, quantity, revenue)
        == left_join_delta(full, db.part, join_weight(class, brand, quantity, revenue)),
{
    let matrix = join_weight(class, brand, quantity, revenue);
    let weight = |change: (LineitemRow, int)| change.1 * sum(db.part, |part: PartRow| matrix(change.0, part));
    let input = |row: LineDelta| line_delta_weight(parts, row, class, brand, quantity, revenue);
    assert forall|index: int| #![trigger rows[index], full[index]] 0 <= index < rows.len() implies input(rows[index]) == weight(full[index]) by {
        let line = full[index].0;
        let sign = full[index].1;
        lemma_line_part(db, line, class, brand, quantity, revenue);
        lemma_sum_ext_all(db.part, |part: PartRow| cell_weight(part, line, class, brand, quantity, revenue), |part: PartRow| matrix(line, part));
        let total = part_sum(db, line.l_partkey, class, brand);
        let amount = line_weight(line, revenue);
        assert(total * sign * amount == sign * (total * amount)) by (nonlinear_arith);
    }
    lemma_sum_aligned(rows, full, input, weight);
}

pub proof fn lemma_delta_line(full: Seq<(PartRow, int)>, line: LineitemRow, class: u8, brand: Seq<u8>, quantity: int, revenue: bool)
    ensures sum(full, |change: (PartRow, int)| change.1 * cell_weight(change.0, line, class, brand, quantity, revenue))
        == if eligible(line) && line.l_quantity == quantity { delta_part_moment(full, line.l_partkey, class, brand) * line_weight(line, revenue) } else { 0 },
{
    let weight = |change: (PartRow, int)| change.1 * cell_weight(change.0, line, class, brand, quantity, revenue);
    if eligible(line) && line.l_quantity == quantity {
        let count = |change: (PartRow, int)| change.1 * ind(change.0.p_partkey == line.l_partkey && class_matches(change.0, class) && change.0.p_brand == brand);
        let value = line_weight(line, revenue);
        assert forall|change: (PartRow, int)| #[trigger] weight(change) == value * count(change) by {
            let sign = change.1;
            if change.0.p_partkey == line.l_partkey && class_matches(change.0, class) && change.0.p_brand == brand {
                assert(count(change) == sign);
                assert(weight(change) == sign * value);
                assert(sign * value == value * sign) by (nonlinear_arith);
            } else {
                assert(count(change) == 0);
                assert(weight(change) == 0);
            }
        }
        lemma_sum_scale(full, count, weight, value);
        let total = delta_part_moment(full, line.l_partkey, class, brand);
        assert(value * total == total * value) by (nonlinear_arith);
    } else { lemma_sum_zero(full, weight); }
}

pub proof fn lemma_rescan(db: Db, changes: Work<PartCell>, rows: Seq<LineInput>, keys: Seq<i64>, part_rows: Seq<PartDelta>, full: Seq<(PartRow, int)>,
    class: u8, brand: Seq<u8>, quantity: int, revenue: bool)
    requires rescan_lines_for_parts_ok(db, keys, rows), changes_for(changes, part_rows), coverage(changes, keys), part_delta_ok(part_rows, full),
    ensures rescan_prefix(changes, rows, rows.len() as int, class, brand, quantity, revenue)
        == right_join_delta(db.lineitem, full, join_weight(class, brand, quantity, revenue)),
{
    let weight = |row: (int, int, int, int)| if row.1 == quantity {
        part_work_value(changes, row.0, class, brand) * if revenue { row.2 * (100 - row.3) } else { 1 }
    } else { 0 };
    let raw = |line: LineitemRow| if (line.l_shipmode == seq![b'A', b'I', b'R'] || line.l_shipmode == seq![b'A', b'I', b'R', b' ', b'R', b'E', b'G'])
        && line.l_shipinstruct == seq![b'D', b'E', b'L', b'I', b'V', b'E', b'R', b' ', b'I', b'N', b' ', b'P', b'E', b'R', b'S', b'O', b'N']
        && seq_view_i64(keys).contains(line.l_partkey) {
        weight((line.l_partkey, line.l_quantity, line.l_extendedprice, line.l_discount))
    } else { 0 };
    assert(sum(line_input_seq_view(rows), weight) == sum(db.lineitem, raw));
    lemma_sum_aligned(rows, line_input_seq_view(rows), |row: LineInput| rescan_weight(changes, row, class, brand, quantity, revenue), weight);
    let moment = |line: LineitemRow| if eligible(line) && line.l_quantity == quantity {
        delta_part_moment(full, line.l_partkey, class, brand) * line_weight(line, revenue)
    } else { 0 };
    assert forall|index: int| #![trigger db.lineitem[index]] 0 <= index < db.lineitem.len() implies raw(db.lineitem[index]) == moment(db.lineitem[index]) by {
        let line = db.lineitem[index];
        lemma_part_projection(part_rows, full, line.l_partkey, class, brand);
    }
    lemma_sum_ext(db.lineitem, raw, moment);
    let join = join_weight(class, brand, quantity, revenue);
    let matrix = |change: (PartRow, int), line: LineitemRow| change.1 * join(line, change.0);
    let by_part = |change: (PartRow, int)| change.1 * sum(db.lineitem, |line: LineitemRow| join(line, change.0));
    let by_line = |line: LineitemRow| sum(full, |change: (PartRow, int)| matrix(change, line));
    assert forall|change: (PartRow, int)| #[trigger] by_part(change) == sum(db.lineitem, |line: LineitemRow| matrix(change, line)) by {
        lemma_sum_scale(db.lineitem, |line: LineitemRow| join(line, change.0), |line: LineitemRow| matrix(change, line), change.1);
    }
    lemma_sum_swap(full, db.lineitem, matrix, by_part, by_line);
    assert forall|line: LineitemRow| #[trigger] by_line(line) == moment(line) by {
        lemma_delta_line(full, line, class, brand, quantity, revenue);
        lemma_sum_ext_all(full, |change: (PartRow, int)| change.1 * cell_weight(change.0, line, class, brand, quantity, revenue), |change: (PartRow, int)| matrix(change, line));
    }
    lemma_sum_ext_all(db.lineitem, by_line, moment);
}

pub proof fn lemma_hist_capture(db: Db, db2: Db, parts: Store<PartCell>, changes: Work<PartCell>, lines: Seq<LineDelta>, part_rows: Seq<PartDelta>,
    rows: Seq<LineInput>, keys: Seq<i64>, line_full: Seq<(LineitemRow, int)>, part_full: Seq<(PartRow, int)>, class: u8, brand: Seq<u8>, quantity: int, revenue: bool)
    requires capture_ok(db, db2, line_full, part_full), lineitem_delta_ok(lines, line_full), part_delta_ok(part_rows, part_full),
        changes_for(changes, part_rows), coverage(changes, keys), rescan_lines_for_parts_ok(db2, keys, rows),
        forall|key: int| #![trigger part_value(parts, key, class, brand)] part_value(parts, key, class, brand) == part_sum(db, key, class, brand),
    ensures hist_sum(db2, class, brand, quantity, revenue) == hist_sum(db, class, brand, quantity, revenue)
        + line_delta_prefix(parts, lines, lines.len() as int, class, brand, quantity, revenue)
        + rescan_prefix(changes, rows, rows.len() as int, class, brand, quantity, revenue),
{
    lemma_hist_join(db, class, brand, quantity, revenue);
    lemma_hist_join(db2, class, brand, quantity, revenue);
    lemma_line_capture(db, parts, lines, line_full, class, brand, quantity, revenue);
    lemma_rescan(db2, changes, rows, keys, part_rows, part_full, class, brand, quantity, revenue);
    lemma_join_bag_step(db.lineitem, db2.lineitem, line_full, db.part, db2.part, part_full, join_weight(class, brand, quantity, revenue));
}

}
