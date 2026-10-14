use vstd::prelude::*;
use verus_common::sums::*;
use verus_prooflib::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::model::*;
use crate::storage::*;

verus! {

broadcast use vstd::seq_lib::group_seq_properties;

pub proof fn lemma_part_scan(db: Db, rows: Seq<PartInput>, key: int, class: u8, brand: Seq<u8>)
    requires scan_qualifying_parts_ok(db, rows),
    ensures part_scan_prefix(rows, rows.len() as int, key, class, brand) == part_sum(db, key, class, brand),
{
    reveal(part_sum);
    let weight = |part: PartRow| ind(part.p_partkey == key && class_matches(part, class) && part.p_brand == brand);
    let raw = |part: PartRow| if (((part.p_size >= 1 && part.p_size <= 5)
        && (((part.p_container == seq![b'S', b'M', b' ', b'C', b'A', b'S', b'E'] || part.p_container == seq![b'S', b'M', b' ', b'B', b'O', b'X'])
            || part.p_container == seq![b'S', b'M', b' ', b'P', b'A', b'C', b'K']) || part.p_container == seq![b'S', b'M', b' ', b'P', b'K', b'G']))
        || ((part.p_size >= 1 && part.p_size <= 10)
        && (((part.p_container == seq![b'M', b'E', b'D', b' ', b'B', b'A', b'G'] || part.p_container == seq![b'M', b'E', b'D', b' ', b'B', b'O', b'X'])
            || part.p_container == seq![b'M', b'E', b'D', b' ', b'P', b'K', b'G']) || part.p_container == seq![b'M', b'E', b'D', b' ', b'P', b'A', b'C', b'K'])))
        || ((part.p_size >= 1 && part.p_size <= 15)
        && (((part.p_container == seq![b'L', b'G', b' ', b'C', b'A', b'S', b'E'] || part.p_container == seq![b'L', b'G', b' ', b'B', b'O', b'X'])
            || part.p_container == seq![b'L', b'G', b' ', b'P', b'A', b'C', b'K']) || part.p_container == seq![b'L', b'G', b' ', b'P', b'K', b'G'])) {
        weight(part)
    } else { 0 };
    assert(sum(part_input_seq_view(rows), weight) == sum(db.part, raw));
    assert forall|part: PartRow| #[trigger] raw(part) == weight(part) by {
        if class == 0 {} else if class == 1 {} else if class == 2 {}
    }
    lemma_sum_ext_all(db.part, raw, weight);
    lemma_sum_aligned(rows, part_input_seq_view(rows), |row: PartInput| part_scan_weight(row, key, class, brand), weight);
}

pub proof fn lemma_hist_line(db: Db, class: u8, brand: Seq<u8>, quantity: int, revenue: bool)
    ensures hist_sum(db, class, brand, quantity, revenue) == sum(db.lineitem, |line: LineitemRow|
        if eligible(line) && line.l_quantity == quantity { part_sum(db, line.l_partkey, class, brand) * line_weight(line, revenue) } else { 0 }),
{
    reveal(hist_sum);
    reveal(part_sum);
    let matrix = |part: PartRow, line: LineitemRow| cell_weight(part, line, class, brand, quantity, revenue);
    let by_part = |part: PartRow| sum(db.lineitem, |line: LineitemRow| cell_weight(part, line, class, brand, quantity, revenue));
    let by_line = |line: LineitemRow| sum(db.part, |part: PartRow| matrix(part, line));
    assert forall|part: PartRow| #[trigger] by_part(part) == sum(db.lineitem, |line: LineitemRow| matrix(part, line)) by {
        lemma_sum_ext_all(db.lineitem, |line: LineitemRow| cell_weight(part, line, class, brand, quantity, revenue), |line: LineitemRow| matrix(part, line));
    }
    lemma_sum_swap(db.part, db.lineitem, matrix, by_part, by_line);
    let moment = |line: LineitemRow| if eligible(line) && line.l_quantity == quantity {
        part_sum(db, line.l_partkey, class, brand) * line_weight(line, revenue)
    } else { 0 };
    assert forall|line: LineitemRow| #[trigger] by_line(line) == moment(line) by {
        let weight = |part: PartRow| matrix(part, line);
        if eligible(line) && line.l_quantity == quantity {
            let count = |part: PartRow| ind(part.p_partkey == line.l_partkey && class_matches(part, class) && part.p_brand == brand);
            let value = line_weight(line, revenue);
            assert forall|part: PartRow| #[trigger] weight(part) == value * count(part) by {}
            lemma_sum_scale(db.part, count, weight, value);
            let total = part_sum(db, line.l_partkey, class, brand);
            assert(value * total == total * value) by (nonlinear_arith);
        } else { lemma_sum_zero(db.part, weight); }
    }
    lemma_sum_ext_all(db.lineitem, by_line, moment);
}

pub proof fn lemma_no_part(db: Db, line: LineitemRow, class: u8, brand: Seq<u8>)
    requires !scan_qualifying_lines_exists_1(db, line),
    ensures part_sum(db, line.l_partkey, class, brand) == 0,
{
    reveal(part_sum);
    let weight = |part: PartRow| ind(part.p_partkey == line.l_partkey && class_matches(part, class) && part.p_brand == brand);
    assert forall|index: int| #![trigger db.part[index]] 0 <= index < db.part.len() implies weight(db.part[index]) == 0 by {
        if weight(db.part[index]) != 0 {
            assert(scan_qualifying_lines_exists_1_where(line, db.part[index]));
            assert(scan_qualifying_lines_exists_1(db, line));
        }
    }
    lemma_sum_zero(db.part, weight);
}

pub proof fn lemma_line_scan(db: Db, parts: Store<PartCell>, rows: Seq<LineInput>, class: u8, brand: Seq<u8>, quantity: int, revenue: bool)
    requires scan_qualifying_lines_ok(db, rows),
        forall|key: int| #![trigger part_value(parts, key, class, brand)] part_value(parts, key, class, brand) == part_sum(db, key, class, brand),
    ensures line_scan_prefix(parts, rows, rows.len() as int, class, brand, quantity, revenue) == hist_sum(db, class, brand, quantity, revenue),
{
    let weight = |row: (int, int, int, int)| if row.1 == quantity {
        part_sum(db, row.0, class, brand) * if revenue { row.2 * (100 - row.3) } else { 1 }
    } else { 0 };
    let raw = |line: LineitemRow| if (line.l_shipmode == seq![b'A', b'I', b'R'] || line.l_shipmode == seq![b'A', b'I', b'R', b' ', b'R', b'E', b'G'])
        && line.l_shipinstruct == seq![b'D', b'E', b'L', b'I', b'V', b'E', b'R', b' ', b'I', b'N', b' ', b'P', b'E', b'R', b'S', b'O', b'N']
        && scan_qualifying_lines_exists_1(db, line) {
        weight((line.l_partkey, line.l_quantity, line.l_extendedprice, line.l_discount))
    } else { 0 };
    assert(sum(line_input_seq_view(rows), weight) == sum(db.lineitem, raw));
    lemma_sum_aligned(rows, line_input_seq_view(rows), |row: LineInput| line_scan_weight(parts, row, class, brand, quantity, revenue), weight);
    let moment = |line: LineitemRow| if eligible(line) && line.l_quantity == quantity {
        part_sum(db, line.l_partkey, class, brand) * line_weight(line, revenue)
    } else { 0 };
    assert forall|index: int| #![trigger db.lineitem[index]] 0 <= index < db.lineitem.len() ==> raw(db.lineitem[index]) == moment(db.lineitem[index]) by {
        let line = db.lineitem[index];
        if !scan_qualifying_lines_exists_1(db, line) { lemma_no_part(db, line, class, brand); }
    }
    lemma_sum_ext(db.lineitem, raw, moment);
    lemma_hist_line(db, class, brand, quantity, revenue);
}

}
