use vstd::prelude::*;
use verus_common::sums::*;
use verus_common::rel::*;
use verus_common::rat::Rat;
use verus_common::sqlfns::like_match;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::moments::*;
use crate::addressing::lemma_block_interval;

verus! {

pub open spec fn covered(parts: Seq<PartDelta>, keys: Seq<i64>) -> bool {
    forall|key: int| #[trigger] seq_view_i64(keys).contains(key) ||
        (part_change(parts, key) == 0 && promo_change(parts, key) == 0)
}

pub proof fn lemma_build_moments(db: Db, parts: Seq<PartInput>, lines: Seq<LineInput>, id: u64)
    requires scan_parts_ok(db, parts), scan_lines_ok(db, lines),
    ensures scanned_count(parts, lines, id) == block_count(db, id),
        scanned_total(parts, lines, id) == block_total(db, id),
        scanned_promo(parts, lines, id) == block_promo(db, id),
{
    lemma_build_join(db, parts, lines, |line: LineitemRow| in_block(id, line.l_shipdate), false, false);
    lemma_build_join(db, parts, lines, |line: LineitemRow| in_block(id, line.l_shipdate), true, false);
    lemma_build_join(db, parts, lines, |line: LineitemRow| in_block(id, line.l_shipdate), true, true);
}

pub proof fn lemma_apply_moments(db: Db, db2: Db, lines: Seq<LineDelta>, parts: Seq<PartDelta>,
    full: Seq<(PartRow, int)>, keys: Seq<i64>, rescans: Seq<LineInput>, id: u64)
    requires part_delta_ok(parts, full), capture_ok(db, db2, line_delta_seq_view(lines), full),
        rescan_lines_for_parts_ok(db2, keys, rescans),
        covered(parts, keys),
    ensures block_count(db2, id) == block_count(db, id) + captured_count(db, lines, id) + rescanned_count(parts, rescans, id),
        block_total(db2, id) == block_total(db, id) + captured_total(db, lines, id) + rescanned_total(parts, rescans, id),
        block_promo(db2, id) == block_promo(db, id) + captured_promo(db, lines, id) + rescanned_promo(parts, rescans, id),
{
    lemma_apply_join(db, db2, lines, parts, full, keys, rescans, |line: LineitemRow| in_block(id, line.l_shipdate), false, false);
    lemma_apply_join(db, db2, lines, parts, full, keys, rescans, |line: LineitemRow| in_block(id, line.l_shipdate), true, false);
    lemma_apply_join(db, db2, lines, parts, full, keys, rescans, |line: LineitemRow| in_block(id, line.l_shipdate), true, true);
}

pub open spec fn model_answer(count: int, total: int, promo: int) -> Seq<OutRow> {
    seq![OutRow { promo_revenue: if count != 0 { Some(Rat { num: 100000000 * promo, den: 1000000 * total }) } else { None } }]
}

pub proof fn lemma_model_result(db: Db, start: int, end: int, count: int, total: int, promo: int)
    requires count == interval_count(db, start, end), total == interval_total(db, start, end),
        promo == interval_promo(db, start, end), count == 0 || total != 0,
    ensures query_result(db, params(start, end), model_answer(count, total, promo)),
{
    let parameters = params(start, end);
    lemma_query_join(db, parameters, false, false);
    lemma_query_join(db, parameters, true, false);
    lemma_query_join(db, parameters, true, true);
    lemma_length_sum(tuples(db, parameters));
    lemma_sum_ext_all(tuples(db, parameters), |tuple: Tuple| tuple_weight(tuple, false, false), |tuple: Tuple| 1int);
    lemma_sum_ext_all(tuples(db, parameters), |tuple: Tuple| tuple_weight(tuple, true, false),
        |tuple: Tuple| tuple.lineitem.l_extendedprice * (100 - tuple.lineitem.l_discount));
    lemma_sum_ext_all(tuples(db, parameters), |tuple: Tuple| tuple_weight(tuple, true, true),
        |tuple: Tuple| if like_match(tuple.part.p_type, seq![b'P', b'R', b'O', b'M', b'O', b'%']) {
            tuple.lineitem.l_extendedprice * (100 - tuple.lineitem.l_discount)
        } else { 0 });
    assert(count == group_count(db, parameters));
    assert(count >= 0);
    assert(10000 * promo * 10000 == 100000000 * promo) by (nonlinear_arith);
    assert(row_ok(db, parameters, model_answer(count, total, promo)[0]));
}

pub proof fn lemma_execute_result(db: Db, start: int, end: int, count: int, total: int, promo: int,
    out: Seq<Option<(i128, i128)>>)
    requires count == interval_count(db, start, end), total == interval_total(db, start, end),
        promo == interval_promo(db, start, end), out.len() == 1,
        count == 0 ==> out[0] is None,
        count != 0 ==> total != 0 && out[0] is Some &&
            out[0].unwrap().0 as int == 100000000 * promo && out[0].unwrap().1 as int == 1000000 * total,
    ensures query_result(db, params(start, end), out_view(out)),
{
    lemma_model_result(db, start, end, count, total, promo);
    assert(out_view(out) =~= model_answer(count, total, promo));
}

pub proof fn lemma_catalog(parts: Seq<PartRow>, key: int, promotional: bool)
    ensures catalog(parts, key, promotional) == sum(parts, |part: PartRow| part_indicator(part, key, promotional)),
{
    if promotional {
        lemma_sum_ext_all(parts, |part: PartRow| if part.p_partkey == key &&
            like_match(part.p_type, seq![b'P', b'R', b'O', b'M', b'O', b'%']) { 1 } else { 0 },
            |part: PartRow| part_indicator(part, key, promotional));
    } else {
        lemma_sum_ext_all(parts, |part: PartRow| if part.p_partkey == key { 1 } else { 0 },
            |part: PartRow| part_indicator(part, key, promotional));
    }
}

pub proof fn lemma_scan_catalog(db: Db, parts: Seq<PartInput>, key: int, promotional: bool)
    requires scan_parts_ok(db, parts),
    ensures catalog(part_input_seq_view(parts), key, promotional) == catalog(db.part, key, promotional),
{
    lemma_catalog(part_input_seq_view(parts), key, promotional);
    lemma_catalog(db.part, key, promotional);
    assert(sum(part_input_seq_view(parts), |part: PartRow| part_indicator(part, key, promotional)) ==
        sum(db.part, |part: PartRow| part_indicator(part, key, promotional)));
}

pub proof fn lemma_orphan(db: Db, line: LineitemRow, promotional: bool)
    requires !scan_lines_exists_1(db, line),
    ensures catalog(db.part, line.l_partkey, promotional) == 0,
{
    lemma_catalog(db.part, line.l_partkey, promotional);
    let summand = |part: PartRow| part_indicator(part, line.l_partkey, promotional);
    assert forall|index: int| 0 <= index < db.part.len() implies summand(#[trigger] db.part[index]) == 0 by {
        if db.part[index].p_partkey == line.l_partkey { assert(scan_lines_exists_1(db, line)); }
    }
    lemma_sum_zero(db.part, summand);
}

pub proof fn lemma_rescan_catalog(db: Db, keys: Seq<i64>, parts: Seq<PartInput>, key: i64, promotional: bool)
    requires rescan_parts_for_lines_ok(db, keys, parts), keys.contains(key),
    ensures catalog(part_input_seq_view(parts), key as int, promotional) == catalog(db.part, key as int, promotional),
{
    let indicator = |part: PartRow| part_indicator(part, key as int, promotional);
    let restricted = |part: PartRow| if seq_view_i64(keys).contains(part.p_partkey) { indicator(part) } else { 0 };
    let position = choose|position: int| 0 <= position < keys.len() && #[trigger] keys[position] == key;
    assert(seq_view_i64(keys)[position] == key as int);
    assert(sum(part_input_seq_view(parts), indicator) == sum(db.part, restricted));
    assert forall|part: PartRow| #[trigger] restricted(part) == indicator(part) by {
        if part.p_partkey == key as int { assert(seq_view_i64(keys).contains(part.p_partkey)); }
    }
    lemma_sum_ext_all(db.part, restricted, indicator);
    lemma_catalog(part_input_seq_view(parts), key as int, promotional);
    lemma_catalog(db.part, key as int, promotional);
}

pub proof fn lemma_build_join(db: Db, parts: Seq<PartInput>, lines: Seq<LineInput>,
    selected: spec_fn(LineitemRow) -> bool, weighted: bool, promotional: bool)
    requires scan_parts_ok(db, parts), scan_lines_ok(db, lines),
    ensures joined(Db { part: part_input_seq_view(parts), lineitem: line_input_seq_view(lines) }, selected, weighted, promotional)
        == joined(db, selected, weighted, promotional),
{
    let scanned = |line: LineitemRow| contribution(part_input_seq_view(parts), line, selected, weighted, promotional);
    let original = |line: LineitemRow| contribution(db.part, line, selected, weighted, promotional);
    assert forall|line: LineitemRow| #[trigger] scanned(line) == original(line) by {
        lemma_scan_catalog(db, parts, line.l_partkey, promotional);
    }
    lemma_sum_ext_all(line_input_seq_view(lines), scanned, original);
    let filtered = |line: LineitemRow| if scan_lines_exists_1(db, line) { original(line) } else { 0 };
    assert(sum(line_input_seq_view(lines), original) == sum(db.lineitem, filtered));
    assert forall|line: LineitemRow| #[trigger] filtered(line) == original(line) by {
        if !scan_lines_exists_1(db, line) { lemma_orphan(db, line, promotional); }
    }
    lemma_sum_ext_all(db.lineitem, filtered, original);
}

pub proof fn lemma_delta_catalog(db: Db, db2: Db, parts: Seq<PartDelta>, full: Seq<(PartRow, int)>,
    key: int, promotional: bool)
    requires part_delta_ok(parts, full), bag_step(db.part, db2.part, full),
    ensures catalog(db2.part, key, promotional) == catalog(db.part, key, promotional) + changed(parts, key, promotional),
{
    lemma_catalog(db.part, key, promotional);
    lemma_catalog(db2.part, key, promotional);
    let projected = |delta: PartDelta| if delta.p_partkey as int == key && (!promotional || delta.is_promo != 0) {
        delta.sign as int
    } else { 0 };
    let original = |delta: (PartRow, int)| delta.1 * part_indicator(delta.0, key, promotional);
    let indices = Seq::new(parts.len(), |index: int| index);
    lemma_sum_index(parts, projected, |index: int| projected(parts[index]));
    lemma_sum_index(full, original, |index: int| original(full[index]));
    assert forall|index: int| 0 <= index < indices.len() implies
        projected(parts[#[trigger] indices[index]]) == original(full[indices[index]]) by {
        assert(indices[index] == index);
        assert(parts[index].p_partkey as int == full[index].0.p_partkey);
        assert((parts[index].is_promo != 0) == like_match(full[index].0.p_type, seq![b'P', b'R', b'O', b'M', b'O', b'%']));
    }
    lemma_sum_ext(indices, |index: int| projected(parts[index]), |index: int| original(full[index]));
    if promotional {
        lemma_sum_ext_all(parts, projected, |delta: PartDelta| if delta.p_partkey as int == key && delta.is_promo != 0 {
            delta.sign as int
        } else { 0 });
    } else {
        lemma_sum_ext_all(parts, projected, |delta: PartDelta| if delta.p_partkey as int == key { delta.sign as int } else { 0 });
    }
    let indicator = |part: PartRow| part_indicator(part, key, promotional);
    let signed = |delta: (PartRow, int)| delta.1 * indicator(delta.0);
    assert(sum(db2.part, indicator) == sum(db.part, indicator) + sum(full, signed));
    lemma_sum_ext_all(full, signed, original);
    assert(sum(db2.part, |part: PartRow| part_indicator(part, key, promotional)) ==
        sum(db.part, |part: PartRow| part_indicator(part, key, promotional)) + sum(full, original));
}

pub proof fn lemma_apply_join(db: Db, db2: Db, lines: Seq<LineDelta>, parts: Seq<PartDelta>, full: Seq<(PartRow, int)>,
    keys: Seq<i64>, rescans: Seq<LineInput>, selected: spec_fn(LineitemRow) -> bool, weighted: bool, promotional: bool)
    requires part_delta_ok(parts, full), capture_ok(db, db2, line_delta_seq_view(lines), full),
        rescan_lines_for_parts_ok(db2, keys, rescans),
        covered(parts, keys),
    ensures joined(db2, selected, weighted, promotional) == joined(db, selected, weighted, promotional) +
        captured(db, lines, selected, weighted, promotional) + rescanned(parts, rescans, selected, weighted, promotional),
{
    let before = |line: LineitemRow| contribution(db.part, line, selected, weighted, promotional);
    let after = |line: LineitemRow| contribution(db2.part, line, selected, weighted, promotional);
    let difference = |line: LineitemRow| delta_contribution(parts, line, selected, weighted, promotional);
    assert forall|line: LineitemRow| #[trigger] after(line) == before(line) + difference(line) by {
        lemma_delta_catalog(db, db2, parts, full, line.l_partkey, promotional);
        let previous = catalog(db.part, line.l_partkey, promotional);
        let next = catalog(db2.part, line.l_partkey, promotional);
        let delta = changed(parts, line.l_partkey, promotional);
        let factor = if weighted { weight(line) } else { 1 };
        assert(next == previous + delta);
        assert(next * factor == previous * factor + delta * factor) by (nonlinear_arith)
            requires next == previous + delta;
    }
    lemma_sum_add(db2.lineitem, before, difference, after);
    let line_change = |delta: (LineitemRow, int)| delta.1 * before(delta.0);
    let runtime_change = |delta: LineDelta| delta.sign as int * before(line_input_view(delta.row));
    lemma_sum_map(lines, |delta: LineDelta| (line_input_view(delta.row), delta.sign as int), line_change, runtime_change);
    assert(sum(db2.lineitem, before) == sum(db.lineitem, before) + sum(line_delta_seq_view(lines), line_change));
    let filtered = |line: LineitemRow| if seq_view_i64(keys).contains(line.l_partkey) { difference(line) } else { 0 };
    assert(sum(line_input_seq_view(rescans), difference) == sum(db2.lineitem, filtered));
    assert forall|line: LineitemRow| #[trigger] filtered(line) == difference(line) by {
        if !seq_view_i64(keys).contains(line.l_partkey) {
            assert(part_change(parts, line.l_partkey) == 0);
            assert(promo_change(parts, line.l_partkey) == 0);
        }
    }
    lemma_sum_ext_all(db2.lineitem, filtered, difference);
    lemma_sum_map(rescans, |line: LineInput| line_input_view(line), difference,
        |line: LineInput| difference(line_input_view(line)));
    lemma_sum_ext_all(lines, runtime_change,
        |delta: LineDelta| delta.sign as int * contribution(db.part, line_input_view(delta.row), selected, weighted, promotional));
    lemma_sum_ext_all(rescans, |line: LineInput| difference(line_input_view(line)),
        |line: LineInput| delta_contribution(parts, line_input_view(line), selected, weighted, promotional));
}

pub open spec fn tuple_weight(tuple: Tuple, weighted: bool, promotional: bool) -> int {
    if promotional && !like_match(tuple.part.p_type, seq![b'P', b'R', b'O', b'M', b'O', b'%']) { 0 }
    else if weighted { weight(tuple.lineitem) } else { 1 }
}

pub proof fn lemma_query_inner(db: Db, parameters: Params, line: LineitemRow, weighted: bool, promotional: bool)
    ensures sum(tuples_1(db, parameters, line), |tuple: Tuple| tuple_weight(tuple, weighted, promotional)) ==
        contribution(db.part, line, |row: LineitemRow| parameters.p1 <= row.l_shipdate < parameters.p2, weighted, promotional),
{
    let summand = |tuple: Tuple| tuple_weight(tuple, weighted, promotional);
    let bags = |part: PartRow| tuples_2(db, parameters, line, part);
    lemma_sum_flat_map(db.part, bags, summand);
    let nested = |part: PartRow| sum(bags(part), summand);
    let scalar = if weighted { weight(line) } else { 1 };
    let flat_weight = |part: PartRow| if parameters.p1 <= line.l_shipdate < parameters.p2 {
        scalar * part_indicator(part, line.l_partkey, promotional)
    } else { 0 };
    assert forall|part: PartRow| #[trigger] nested(part) == flat_weight(part) by {
        lemma_sum_opt(where_clause(db, parameters, line, part), Tuple { lineitem: line, part }, summand);
        assert(nested(part) == if where_clause(db, parameters, line, part) {
            summand(Tuple { lineitem: line, part })
        } else { 0 });
        assert(summand(Tuple { lineitem: line, part }) == if promotional &&
            !like_match(part.p_type, seq![b'P', b'R', b'O', b'M', b'O', b'%']) { 0 } else { scalar });
    }
    lemma_sum_ext_all(db.part, nested, flat_weight);
    lemma_catalog(db.part, line.l_partkey, promotional);
    if parameters.p1 <= line.l_shipdate < parameters.p2 {
        lemma_sum_scale(db.part, |part: PartRow| part_indicator(part, line.l_partkey, promotional), flat_weight, scalar);
        assert(scalar * catalog(db.part, line.l_partkey, promotional) == catalog(db.part, line.l_partkey, promotional) * scalar) by (nonlinear_arith);
    } else {
        lemma_sum_zero(db.part, flat_weight);
    }
}

pub proof fn lemma_query_join(db: Db, parameters: Params, weighted: bool, promotional: bool)
    ensures group_sum(db, parameters, |tuple: Tuple| tuple_weight(tuple, weighted, promotional)) ==
        joined(db, |row: LineitemRow| parameters.p1 <= row.l_shipdate < parameters.p2, weighted, promotional),
{
    let summand = |tuple: Tuple| tuple_weight(tuple, weighted, promotional);
    let bags = |line: LineitemRow| tuples_1(db, parameters, line);
    lemma_sum_flat_map(db.lineitem, bags, summand);
    let nested = |line: LineitemRow| sum(bags(line), summand);
    let factored = |line: LineitemRow| contribution(db.part, line,
        |row: LineitemRow| parameters.p1 <= row.l_shipdate < parameters.p2, weighted, promotional);
    assert forall|line: LineitemRow| #[trigger] nested(line) == factored(line) by {
        lemma_query_inner(db, parameters, line, weighted, promotional);
    }
    lemma_sum_ext_all(db.lineitem, nested, factored);
}

pub proof fn lemma_length_sum<A>(rows: Seq<A>)
    ensures sum(rows, |row: A| 1int) == rows.len(),
    decreases rows.len(),
{
    if rows.len() > 0 { lemma_length_sum(rows.drop_last()); }
}

pub proof fn lemma_empty_intervals(db: Db, start: int, end: int)
    requires end <= start,
    ensures interval_count(db, start, end) == 0, interval_total(db, start, end) == 0, interval_promo(db, start, end) == 0,
{
    lemma_sum_zero(db.lineitem, |line: LineitemRow| contribution(db.part, line, |row: LineitemRow| start <= row.l_shipdate < end, false, false));
    lemma_sum_zero(db.lineitem, |line: LineitemRow| contribution(db.part, line, |row: LineitemRow| start <= row.l_shipdate < end, true, false));
    lemma_sum_zero(db.lineitem, |line: LineitemRow| contribution(db.part, line, |row: LineitemRow| start <= row.l_shipdate < end, true, true));
}

pub proof fn lemma_interval_join_step(db: Db, initial: int, start: int, level: nat, id: u64, weighted: bool, promotional: bool)
    requires 0 <= start < 4294967296, level <= 32, start % pow2(level) == 0,
        start + pow2(level) <= 4294967296, initial <= start - 2147483648,
        id as int == pow2((32 - level) as nat) + start / pow2(level),
    ensures joined(db, |line: LineitemRow| initial <= line.l_shipdate < start + pow2(level) - 2147483648, weighted, promotional) ==
        joined(db, |line: LineitemRow| initial <= line.l_shipdate < start - 2147483648, weighted, promotional) +
        joined(db, |line: LineitemRow| in_block(id, line.l_shipdate), weighted, promotional),
{
    crate::addressing::lemma_pow_positive(level);
    let earlier = |line: LineitemRow| contribution(db.part, line,
        |row: LineitemRow| initial <= row.l_shipdate < start - 2147483648, weighted, promotional);
    let block = |line: LineitemRow| contribution(db.part, line, |row: LineitemRow| in_block(id, row.l_shipdate), weighted, promotional);
    let next = |line: LineitemRow| contribution(db.part, line,
        |row: LineitemRow| initial <= row.l_shipdate < start + pow2(level) - 2147483648, weighted, promotional);
    assert forall|line: LineitemRow| #[trigger] next(line) == earlier(line) + block(line) by {
        lemma_block_interval(id, line.l_shipdate, start, level);
        let scalar = catalog(db.part, line.l_partkey, promotional) * if weighted { weight(line) } else { 1 };
        assert(earlier(line) == if initial <= line.l_shipdate < start - 2147483648 { scalar } else { 0 });
        assert(block(line) == if start <= line.l_shipdate + 2147483648 < start + pow2(level) { scalar } else { 0 });
        assert(next(line) == if initial <= line.l_shipdate < start + pow2(level) - 2147483648 { scalar } else { 0 });
    }
    lemma_sum_add(db.lineitem, earlier, block, next);
}

pub proof fn lemma_interval_step(db: Db, initial: int, start: int, level: nat, id: u64)
    requires 0 <= start < 4294967296, level <= 32, start % pow2(level) == 0,
        start + pow2(level) <= 4294967296, initial <= start - 2147483648,
        id as int == pow2((32 - level) as nat) + start / pow2(level),
    ensures interval_count(db, initial, start + pow2(level) - 2147483648) == interval_count(db, initial, start - 2147483648) + block_count(db, id),
        interval_total(db, initial, start + pow2(level) - 2147483648) == interval_total(db, initial, start - 2147483648) + block_total(db, id),
        interval_promo(db, initial, start + pow2(level) - 2147483648) == interval_promo(db, initial, start - 2147483648) + block_promo(db, id),
{
    lemma_interval_join_step(db, initial, start, level, id, false, false);
    lemma_interval_join_step(db, initial, start, level, id, true, false);
    lemma_interval_join_step(db, initial, start, level, id, true, true);
}

}
