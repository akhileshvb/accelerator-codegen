use vstd::prelude::*;
use verus_common::sums::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::moments::*;
use crate::semantics::*;

verus! {

pub proof fn lemma_prefix_contains<A>(rows: Seq<A>, index: int, value: A)
    requires 0 <= index < rows.len(),
    ensures rows.take(index + 1).contains(value) == (rows.take(index).contains(value) || rows[index] == value),
{
    let earlier = rows.take(index);
    let next = rows.take(index + 1);
    assert(next =~= earlier.push(rows[index]));
    if next.contains(value) {
        let position = choose|position: int| 0 <= position < next.len() && #[trigger] next[position] == value;
        if position < index { assert(earlier[position] == value); } else { assert(rows[index] == value); }
    }
    if earlier.contains(value) {
        let position = choose|position: int| 0 <= position < earlier.len() && #[trigger] earlier[position] == value;
        assert(next[position] == value);
    }
    if rows[index] == value { assert(next[index] == value); }
}

pub proof fn lemma_part_prefix(parts: Seq<PartInput>, index: int, key: int, promotional: bool)
    requires 0 <= index < parts.len(),
    ensures catalog(part_input_seq_view(parts.take(index + 1)), key, promotional) ==
        catalog(part_input_seq_view(parts.take(index)), key, promotional) + part_indicator(part_input_view(parts[index]), key, promotional),
{
    let view = part_input_seq_view(parts);
    assert(part_input_seq_view(parts.take(index)) =~= view.take(index));
    assert(part_input_seq_view(parts.take(index + 1)) =~= view.take(index + 1));
    lemma_catalog(view.take(index), key, promotional);
    lemma_catalog(view.take(index + 1), key, promotional);
    lemma_sum_take(view, index, |part: PartRow| part_indicator(part, key, promotional));
}

pub proof fn lemma_push_contains<A>(rows: Seq<A>, item: A, value: A)
    ensures rows.push(item).contains(value) == (rows.contains(value) || item == value),
{
    let next = rows.push(item);
    lemma_prefix_contains(next, rows.len() as int, value);
    assert(next.take(rows.len() as int) =~= rows);
    assert(next.take(next.len() as int) =~= next);
}

pub proof fn lemma_part_change_prefix(parts: Seq<PartDelta>, index: int, key: int, promotional: bool)
    requires 0 <= index < parts.len(),
    ensures changed(parts.take(index + 1), key, promotional) == changed(parts.take(index), key, promotional) +
        if parts[index].p_partkey as int == key && (!promotional || parts[index].is_promo != 0) { parts[index].sign as int } else { 0 },
{
    if promotional {
        lemma_sum_take(parts, index, |part: PartDelta| if part.p_partkey as int == key && part.is_promo != 0 { part.sign as int } else { 0 });
    } else {
        lemma_sum_take(parts, index, |part: PartDelta| if part.p_partkey as int == key { part.sign as int } else { 0 });
    }
}

pub proof fn lemma_line_support_prefix(rows: Seq<LineDelta>, index: int, key: i64)
    requires 0 <= index < rows.len(),
    ensures line_key_support(rows.take(index + 1), key) ==
        (line_key_support(rows.take(index), key) || rows[index].row.l_partkey == key),
{
    let earlier = rows.take(index);
    let next = rows.take(index + 1);
    if line_key_support(next, key) {
        let position = choose|position: int| 0 <= position < next.len() && (#[trigger] next[position]).row.l_partkey == key;
        if position < index { assert(earlier[position].row.l_partkey == key); }
        else { assert(rows[index].row.l_partkey == key); }
    }
    if line_key_support(earlier, key) {
        let position = choose|position: int| 0 <= position < earlier.len() && (#[trigger] earlier[position]).row.l_partkey == key;
        assert(next[position].row.l_partkey == key);
    }
    if rows[index].row.l_partkey == key { assert(next[index].row.l_partkey == key); }
}

pub proof fn lemma_part_support_prefix(rows: Seq<PartDelta>, index: int, key: i64)
    requires 0 <= index < rows.len(),
    ensures part_key_support(rows.take(index + 1), key) ==
        (part_key_support(rows.take(index), key) || rows[index].p_partkey == key),
{
    let earlier = rows.take(index);
    let next = rows.take(index + 1);
    if part_key_support(next, key) {
        let position = choose|position: int| 0 <= position < next.len() && (#[trigger] next[position]).p_partkey == key;
        if position < index { assert(earlier[position].p_partkey == key); }
        else { assert(rows[index].p_partkey == key); }
    }
    if part_key_support(earlier, key) {
        let position = choose|position: int| 0 <= position < earlier.len() && (#[trigger] earlier[position]).p_partkey == key;
        assert(next[position].p_partkey == key);
    }
    if rows[index].p_partkey == key { assert(next[index].p_partkey == key); }
}

pub proof fn lemma_join_prefix(parts: Seq<PartInput>, rows: Seq<LineInput>, index: int,
    selected: spec_fn(LineitemRow) -> bool, weighted: bool, promotional: bool)
    requires 0 <= index < rows.len(),
    ensures joined(Db { part: part_input_seq_view(parts), lineitem: line_input_seq_view(rows.take(index + 1)) }, selected, weighted, promotional) ==
        joined(Db { part: part_input_seq_view(parts), lineitem: line_input_seq_view(rows.take(index)) }, selected, weighted, promotional) +
        contribution(part_input_seq_view(parts), line_input_view(rows[index]), selected, weighted, promotional),
{
    let view = line_input_seq_view(rows);
    assert(line_input_seq_view(rows.take(index)) =~= view.take(index));
    assert(line_input_seq_view(rows.take(index + 1)) =~= view.take(index + 1));
    lemma_sum_take(view, index, |line: LineitemRow| contribution(part_input_seq_view(parts), line, selected, weighted, promotional));
}

pub proof fn lemma_captured_prefix(db: Db, rows: Seq<LineDelta>, index: int,
    selected: spec_fn(LineitemRow) -> bool, weighted: bool, promotional: bool)
    requires 0 <= index < rows.len(),
    ensures captured(db, rows.take(index + 1), selected, weighted, promotional) ==
        captured(db, rows.take(index), selected, weighted, promotional) + rows[index].sign as int *
        contribution(db.part, line_input_view(rows[index].row), selected, weighted, promotional),
{
    lemma_sum_take(rows, index, |delta: LineDelta| delta.sign as int * contribution(db.part, line_input_view(delta.row), selected, weighted, promotional));
}

pub proof fn lemma_rescanned_prefix(parts: Seq<PartDelta>, rows: Seq<LineInput>, index: int,
    selected: spec_fn(LineitemRow) -> bool, weighted: bool, promotional: bool)
    requires 0 <= index < rows.len(),
    ensures rescanned(parts, rows.take(index + 1), selected, weighted, promotional) ==
        rescanned(parts, rows.take(index), selected, weighted, promotional) +
        delta_contribution(parts, line_input_view(rows[index]), selected, weighted, promotional),
{
    lemma_sum_take(rows, index, |line: LineInput| delta_contribution(parts, line_input_view(line), selected, weighted, promotional));
}

pub proof fn lemma_change_support(parts: Seq<PartDelta>, key: int)
    ensures (part_change(parts, key) != 0 || promo_change(parts, key) != 0) ==>
        -9223372036854775808 <= key <= 9223372036854775807 && part_key_support(parts, key as i64),
{
    if part_change(parts, key) != 0 {
        let position = lemma_sum_nonzero_witness(parts, |part: PartDelta| if part.p_partkey as int == key { part.sign as int } else { 0 });
        assert(parts[position].p_partkey as int == key);
    } else if promo_change(parts, key) != 0 {
        let position = lemma_sum_nonzero_witness(parts, |part: PartDelta| if part.p_partkey as int == key && part.is_promo != 0 { part.sign as int } else { 0 });
        assert(parts[position].p_partkey as int == key);
    }
}

}
