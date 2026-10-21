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

pub open spec fn line_keys(rows: Seq<LineDelta>) -> Seq<i64>
    decreases rows.len(),
{
    if rows.len() == 0 { Seq::empty() } else {
        let earlier = line_keys(rows.drop_last());
        let key = rows.last().row.l_partkey;
        if earlier.contains(key) { earlier } else { earlier.push(key) }
    }
}

pub open spec fn part_keys(rows: Seq<PartDelta>) -> Seq<i64>
    decreases rows.len(),
{
    if rows.len() == 0 { Seq::empty() } else {
        let earlier = part_keys(rows.drop_last());
        let key = rows.last().p_partkey;
        if earlier.contains(key) { earlier } else { earlier.push(key) }
    }
}

pub open spec fn has_change(parts: Seq<PartDelta>, key: i64) -> bool {
    part_change(parts, key as int) != 0 || promo_change(parts, key as int) != 0
}

pub open spec fn changed_keys(parts: Seq<PartDelta>, keys: Seq<i64>) -> Seq<i64>
    decreases keys.len(),
{
    if keys.len() == 0 { Seq::empty() } else {
        let earlier = changed_keys(parts, keys.drop_last());
        let key = keys.last();
        if has_change(parts, key) { earlier.push(key) } else { earlier }
    }
}

pub proof fn lemma_line_keys_contains(rows: Seq<LineDelta>, key: i64)
    ensures line_keys(rows).contains(key) == line_key_support(rows, key),
    decreases rows.len(),
{
    if rows.len() > 0 {
        let last = rows.len() - 1;
        let earlier = rows.drop_last();
        lemma_line_keys_contains(earlier, key);
        assert(rows.take(last) =~= earlier);
        assert(rows.take(last + 1) =~= rows);
        lemma_line_support_prefix(rows, last, key);
        if !line_keys(earlier).contains(rows.last().row.l_partkey) {
            lemma_push_contains(line_keys(earlier), rows.last().row.l_partkey, key);
        }
    }
}

pub proof fn lemma_part_keys_contains(rows: Seq<PartDelta>, key: i64)
    ensures part_keys(rows).contains(key) == part_key_support(rows, key),
    decreases rows.len(),
{
    if rows.len() > 0 {
        let last = rows.len() - 1;
        let earlier = rows.drop_last();
        lemma_part_keys_contains(earlier, key);
        assert(rows.take(last) =~= earlier);
        assert(rows.take(last + 1) =~= rows);
        lemma_part_support_prefix(rows, last, key);
        if !part_keys(earlier).contains(rows.last().p_partkey) {
            lemma_push_contains(part_keys(earlier), rows.last().p_partkey, key);
        }
    }
}

pub proof fn lemma_changed_keys_contains(parts: Seq<PartDelta>, keys: Seq<i64>, key: i64)
    ensures changed_keys(parts, keys).contains(key) == (keys.contains(key) && has_change(parts, key)),
    decreases keys.len(),
{
    if keys.len() > 0 {
        let earlier = keys.drop_last();
        lemma_changed_keys_contains(parts, earlier, key);
        assert(keys =~= earlier.push(keys.last()));
        lemma_push_contains(earlier, keys.last(), key);
        if has_change(parts, keys.last()) {
            lemma_push_contains(changed_keys(parts, earlier), keys.last(), key);
        }
    }
}

pub proof fn lemma_changed_keys_covered(parts: Seq<PartDelta>)
    ensures covered(parts, changed_keys(parts, part_keys(parts))),
{
    let keys = changed_keys(parts, part_keys(parts));
    assert forall|key: int| #[trigger] seq_view_i64(keys).contains(key) ||
        (part_change(parts, key) == 0 && promo_change(parts, key) == 0) by {
        if part_change(parts, key) != 0 || promo_change(parts, key) != 0 {
            lemma_change_support(parts, key);
            let runtime_key = key as i64;
            lemma_part_keys_contains(parts, runtime_key);
            lemma_changed_keys_contains(parts, part_keys(parts), runtime_key);
            let position = choose|position: int| 0 <= position < keys.len() && #[trigger] keys[position] == runtime_key;
            assert(seq_view_i64(keys)[position] == key);
        }
    }
}

pub proof fn lemma_line_delta_key(rows: Seq<LineDelta>, index: int)
    requires 0 <= index < rows.len(),
    ensures line_keys(rows).contains(rows[index].row.l_partkey),
{
    lemma_line_keys_contains(rows, rows[index].row.l_partkey);
}

}
