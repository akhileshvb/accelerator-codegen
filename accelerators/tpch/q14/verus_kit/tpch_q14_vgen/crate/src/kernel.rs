use vstd::prelude::*;
use std::collections::HashMap;
use std::sync::Arc;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::moments::*;
use crate::moments::add;
use crate::storage::{Directory, Changes};
use crate::storage;
use verus_common::rel::pow2;
use crate::semantics::*;
use crate::prefixes::*;
use crate::addressing::*;
use verus_prooflib::wide::Wide;

verus! {

pub struct State {
    root: Option<Arc<Directory>>,
    database: Ghost<Db>,
}

impl State {
    pub closed spec fn db(self) -> Db { self.database@ }

    pub closed spec fn moment(self, id: u64) -> MomentsView {
        storage::lookup(self.root, 7, id)
    }

    #[verifier::type_invariant]
    spec fn invariant(self) -> bool {
        &&& storage::valid(self.root, 7)
        &&& forall|id: u64| id < 17179869184u64 ==> {
            &&& (#[trigger] self.moment(id)).count == block_count(self.db(), id)
            &&& self.moment(id).total == block_total(self.db(), id)
            &&& self.moment(id).promo == block_promo(self.db(), id)
        }
    }
}

fn collect_parts(parts: &Vec<PartInput>) -> (map: HashMap<i64, Moments>)
    ensures forall|key: i64|
        (#[trigger] key_value(map@, key)).count == part_count(part_input_seq_view(parts@), key as int) &&
        key_value(map@, key).promo == promo_count(part_input_seq_view(parts@), key as int),
{
    let mut counts: HashMap<i64, Moments> = HashMap::new();
    let mut index: usize = 0;
    let mut processed: Ghost<int> = Ghost(0int);
    while index < parts.len()
        invariant index <= parts.len(), processed@ == index as int,
            forall|key: i64| (#[trigger] key_value(counts@, key)).count ==
                part_count(part_input_seq_view(parts@.take(index as int)), key as int),
            forall|key: i64| (#[trigger] key_value(counts@, key)).promo ==
                promo_count(part_input_seq_view(parts@.take(index as int)), key as int),
        decreases parts.len() - index,
    {
        let part = &parts[index];
        let before = get_key(&counts, part.p_partkey);
        let change = Moments { count: Wide::from_i128(1), total: Wide::zero(),
            promo: Wide::from_i128(if is_promo(&part.p_type) { 1 } else { 0 }) };
        let after = add(&before, &change);
        let ghost previous = counts@;
        counts.insert(part.p_partkey, after);
        proof {
            assert forall|key: i64| (#[trigger] key_value(counts@, key)).count ==
                part_count(part_input_seq_view(parts@.take(index as int + 1)), key as int) &&
                key_value(counts@, key).promo == promo_count(part_input_seq_view(parts@.take(index as int + 1)), key as int) by {
                lemma_part_prefix(parts@, index as int, key as int, false);
                lemma_part_prefix(parts@, index as int, key as int, true);
                assert(key_value(previous, key).count == part_count(part_input_seq_view(parts@.take(index as int)), key as int));
                assert(key_value(previous, key).promo == promo_count(part_input_seq_view(parts@.take(index as int)), key as int));
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    proof { assert(parts@.take(index as int) =~= parts@); }
    counts
}

fn collect_part_changes(delta: &[PartDelta]) -> (out: (HashMap<i64, Moments>, Vec<i64>))
    ensures {
        &&& forall|key: i64| (#[trigger] key_value(out.0@, key)).count == part_change(delta@, key as int) &&
            key_value(out.0@, key).promo == promo_change(delta@, key as int)
        &&& out.1@ == part_keys(delta@)
    },
{
    let mut counts: HashMap<i64, Moments> = HashMap::new();
    let mut keys: Vec<i64> = Vec::new();
    let mut index: usize = 0;
    let mut processed: Ghost<int> = Ghost(0int);
    while index < delta.len()
        invariant index <= delta.len(), processed@ == index as int,
            forall|key: i64| (#[trigger] key_value(counts@, key)).count ==
                part_change(delta@.take(index as int), key as int),
            forall|key: i64| (#[trigger] key_value(counts@, key)).promo ==
                promo_change(delta@.take(index as int), key as int),
            forall|key: i64| #[trigger] counts@.dom().contains(key) == keys@.contains(key),
            keys@ == part_keys(delta@.take(index as int)),
        decreases delta.len() - index,
    {
        let part = &delta[index];
        let ghost previous = counts@;
        let ghost previous_keys = keys@;
        let add_key = !counts.contains_key(&part.p_partkey);
        if add_key { keys.push(part.p_partkey); }
        let before = get_key(&counts, part.p_partkey);
        let change = Moments { count: Wide::from_i128(part.sign as i128), total: Wide::zero(),
            promo: Wide::from_i128(if part.is_promo != 0 { part.sign as i128 } else { 0 }) };
        let after = add(&before, &change);
        counts.insert(part.p_partkey, after);
        proof {
            assert forall|key: i64| (#[trigger] key_value(counts@, key)).count == part_change(delta@.take(index as int + 1), key as int) &&
                key_value(counts@, key).promo == promo_change(delta@.take(index as int + 1), key as int) by {
                lemma_part_change_prefix(delta@, index as int, key as int, false);
                lemma_part_change_prefix(delta@, index as int, key as int, true);
                assert(key_value(previous, key).count == part_change(delta@.take(index as int), key as int));
                assert(key_value(previous, key).promo == promo_change(delta@.take(index as int), key as int));
            }
            assert(delta@.take(index as int + 1).drop_last() =~= delta@.take(index as int));
            assert(delta@.take(index as int + 1).last() == delta@[index as int]);
            assert(keys@ == part_keys(delta@.take(index as int + 1)));
            assert forall|key: i64| #[trigger] counts@.dom().contains(key) == keys@.contains(key) by {
                if add_key { lemma_push_contains(previous_keys, part.p_partkey, key); }
                assert(previous.dom().contains(key) == previous_keys.contains(key));
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    proof { assert(delta@.take(index as int) =~= delta@); }
    (counts, keys)
}

fn key_set(keys: &[i64]) -> (out: HashMap<i64, bool>)
    ensures forall|key: i64| #[trigger] out@.dom().contains(key) == keys@.contains(key),
{
    let mut out: HashMap<i64, bool> = HashMap::new();
    let mut index: usize = 0;
    let mut processed: Ghost<int> = Ghost(0int);
    while index < keys.len()
        invariant index <= keys.len(), processed@ == index as int,
            forall|key: i64| #[trigger] out@.dom().contains(key) == keys@.take(index as int).contains(key),
        decreases keys.len() - index,
    {
        out.insert(keys[index], true);
        proof {
            assert forall|key: i64| #[trigger] out@.dom().contains(key) == keys@.take(index as int + 1).contains(key) by {
                lemma_prefix_contains(keys@, index as int, key);
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    proof { assert(keys@.take(index as int) =~= keys@); }
    out
}

pub fn build(parts: Vec<PartInput>, lines: &[LineInput], Ghost(db): Ghost<Db>) -> (out: Option<State>)
    requires scan_parts_ok(db, parts@), scan_lines_ok(db, lines@),
    ensures out matches Some(state) ==> state.db() == db,
        out is Some,
{
    let counts = collect_parts(&parts);
    let mut changes = Changes::new();
    let mut index: usize = 0;
    let mut processed: Ghost<int> = Ghost(0int);
    while index < lines.len()
        invariant index <= lines.len(), processed@ == index as int, changes.wf(),
            forall|key: i64| (#[trigger] key_value(counts@, key)).count == part_count(part_input_seq_view(parts@), key as int) &&
                key_value(counts@, key).promo == promo_count(part_input_seq_view(parts@), key as int),
            forall|id: u64| (#[trigger] block_value(changes.blocks@, id)).count ==
                scanned_count(parts@, lines@.take(index as int), id),
            forall|id: u64| (#[trigger] block_value(changes.blocks@, id)).total ==
                scanned_total(parts@, lines@.take(index as int), id),
            forall|id: u64| (#[trigger] block_value(changes.blocks@, id)).promo ==
                scanned_promo(parts@, lines@.take(index as int), id),
        decreases lines.len() - index,
    {
        let line = lines[index];
        let value = line_moments(&get_key(&counts, line.l_partkey), line);
        changes.add_day(line.l_shipdate, &value);
        proof {
            assert forall|id: u64| (#[trigger] block_value(changes.blocks@, id)).count == scanned_count(parts@, lines@.take(index as int + 1), id) &&
                block_value(changes.blocks@, id).total == scanned_total(parts@, lines@.take(index as int + 1), id) &&
                block_value(changes.blocks@, id).promo == scanned_promo(parts@, lines@.take(index as int + 1), id) by {
                lemma_join_prefix(parts@, lines@, index as int, |row: LineitemRow| in_block(id, row.l_shipdate), false, false);
                lemma_join_prefix(parts@, lines@, index as int, |row: LineitemRow| in_block(id, row.l_shipdate), true, false);
                lemma_join_prefix(parts@, lines@, index as int, |row: LineitemRow| in_block(id, row.l_shipdate), true, true);
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    let root = changes.publish(&None);
    proof {
        assert(lines@.take(index as int) =~= lines@);
        assert forall|id: u64| id < 17179869184u64 implies {
            &&& (#[trigger] storage::lookup(root, 7, id)).count == block_count(db, id)
            &&& storage::lookup(root, 7, id).total == block_total(db, id)
            &&& storage::lookup(root, 7, id).promo == block_promo(db, id)
        } by { lemma_build_moments(db, parts@, lines@, id); }
    }
    Some(State { root, database: Ghost(db) })
}

pub fn execute(state: &State, p1: i32, p2: i32) -> (out: Option<Vec<Option<(i128, i128)>>>)
    ensures out matches Some(rows) ==> query_result(state.db(), params(p1 as int, p2 as int), out_view(rows@)),
        envelope(state.db()) && answer_fits(state.db(), params(p1 as int, p2 as int)) ==> out is Some,
{
    proof { use_type_invariant(state); }
    let mut rows: Vec<Option<(i128, i128)>> = Vec::new();
    if p2 <= p1 {
        rows.push(None);
        proof {
            lemma_empty_intervals(state.db(), p1 as int, p2 as int);
            lemma_execute_result(state.db(), p1 as int, p2 as int, 0, 0, 0, rows@);
        }
        return Some(rows);
    }
    let mut start = date_key(p1);
    let end = date_key(p2);
    let mut accumulator = empty();
    proof { lemma_empty_intervals(state.db(), p1 as int, p1 as int); }
    while start < end
        invariant date_key_spec(p1) <= start <= end, end == date_key_spec(p2), end < 4294967296u64,
            state.invariant(),
            accumulator.count@ == interval_count(state.db(), p1 as int, start as int - 2147483648),
            accumulator.total@ == interval_total(state.db(), p1 as int, start as int - 2147483648),
            accumulator.promo@ == interval_promo(state.db(), p1 as int, start as int - 2147483648),
        decreases end - start,
    {
        let mut level: u32 = 0;
        let mut width: u64 = 1;
        while level < 32 && start % (width * 2) == 0 && width * 2 <= end - start
            invariant level <= 32, width as int == pow2(level as nat), width > 0,
                width <= end - start, start % width == 0, width <= 4294967296u64,
                start < end < 4294967296u64, state.invariant(),
            decreases 32 - level,
        {
            level += 1;
            width *= 2;
        }
        let base = power(32 - level);
        proof { lemma_block_bounds(start as int - 2147483648, level as nat); }
        let id = base + start / width;
        let value = storage::get(&state.root, 7, id);
        accumulator = add(&accumulator, &value);
        proof {
            assert(state.moment(id) == storage::lookup(state.root, 7, id));
            lemma_interval_step(state.db(), p1 as int, start as int, level as nat, id);
        }
        start += width;
    }
    let ghost (count, total, promo) = (accumulator.count@, accumulator.total@, accumulator.promo@);
    if accumulator.count.is_zero() {
        rows.push(None);
        proof {
            lemma_execute_result(state.db(), p1 as int, p2 as int, count, total, promo, rows@);
        }
        return Some(rows);
    }
    if accumulator.total.is_zero() { return None; }
    let numerator = accumulator.promo.mul(&Wide::from_i128(100000000i128)).to_i128();
    let denominator = accumulator.total.mul(&Wide::from_i128(1000000i128)).to_i128();
    match (numerator, denominator) {
        (Some(num), Some(den)) => {
            rows.push(Some((num, den)));
            proof {
                lemma_execute_result(state.db(), p1 as int, p2 as int, count, total, promo, rows@);
            }
            Some(rows)
        },
        _ => {
            proof {
                lemma_model_result(state.db(), p1 as int, p2 as int, count, total, promo);
                let model = model_answer(count, total, promo);
                if answer_fits(state.db(), params(p1 as int, p2 as int)) {
                    assert(out_fits(model[0]));
                }
            }
            None
        },
    }
}

pub open spec fn rescan_parts_for_lines_wanted(s: State, lineitem_delta: Seq<LineDelta>, part_delta: Seq<PartDelta>) -> Seq<i64> {
    line_keys(lineitem_delta)
}

pub open spec fn rescan_lines_for_parts_wanted(s: State, lineitem_delta: Seq<LineDelta>, part_delta: Seq<PartDelta>) -> Seq<i64> {
    changed_keys(part_delta, part_keys(part_delta))
}

pub fn rescan_parts_for_lines_keys(
    state: &State,
    lineitem_delta: &[LineDelta],
    part_delta: &[PartDelta],
) -> (out: Vec<i64>)
    ensures out@ == rescan_parts_for_lines_wanted(*state, lineitem_delta@, part_delta@),
{
    let mut keys: Vec<i64> = Vec::new();
    let mut seen: HashMap<i64, bool> = HashMap::new();
    let mut index: usize = 0;
    let mut processed: Ghost<int> = Ghost(0int);
    while index < lineitem_delta.len()
        invariant index <= lineitem_delta.len(), processed@ == index as int,
            forall|key: i64| #[trigger] seen@.dom().contains(key) == keys@.contains(key),
            keys@ == line_keys(lineitem_delta@.take(index as int)),
        decreases lineitem_delta.len() - index,
    {
        let key = lineitem_delta[index].row.l_partkey;
        let ghost earlier = keys@;
        let ghost previous = seen@;
        let add_key = !seen.contains_key(&key);
        if add_key { seen.insert(key, true); keys.push(key); }
        proof {
            assert(lineitem_delta@.take(index as int + 1).drop_last() =~= lineitem_delta@.take(index as int));
            assert(lineitem_delta@.take(index as int + 1).last() == lineitem_delta@[index as int]);
            assert forall|candidate: i64| #[trigger] seen@.dom().contains(candidate) == keys@.contains(candidate) by {
                if add_key { lemma_push_contains(earlier, key, candidate); }
                assert(previous.dom().contains(candidate) == earlier.contains(candidate));
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    proof { assert(lineitem_delta@.take(index as int) =~= lineitem_delta@); }
    keys
}

pub fn rescan_lines_for_parts_keys(
    state: &State,
    lineitem_delta: &[LineDelta],
    part_delta: &[PartDelta],
) -> (out: Vec<i64>)
    ensures out@ == rescan_lines_for_parts_wanted(*state, lineitem_delta@, part_delta@),
{
    let mut result: Vec<i64> = Vec::new();
    let (changes, keys) = collect_part_changes(part_delta);
    let mut index: usize = 0;
    while index < keys.len()
        invariant index <= keys.len(),
            keys@ == part_keys(part_delta@),
            forall|key: i64| (#[trigger] key_value(changes@, key)).count == part_change(part_delta@, key as int) &&
                key_value(changes@, key).promo == promo_change(part_delta@, key as int),
            result@ == changed_keys(part_delta@, keys@.take(index as int)),
        decreases keys.len() - index,
    {
        let value = get_key(&changes, keys[index]);
        if !value.count.is_zero() || !value.promo.is_zero() { result.push(keys[index]); }
        proof {
            assert(keys@.take(index as int + 1).drop_last() =~= keys@.take(index as int));
            assert(keys@.take(index as int + 1).last() == keys@[index as int]);
        }
        index += 1;
    }
    proof { assert(keys@.take(index as int) =~= keys@); }
    result
}

fn apply_inner(
    state: &State,
    lineitem_delta: &[LineDelta],
    part_delta: &[PartDelta],
    parts_for_lines_keys: &[i64],
    parts_for_lines_rows: &Vec<PartInput>,
    lines_for_parts_keys: &[i64],
    lines_for_parts_rows: &[LineInput],
    Ghost(part_full): Ghost<Seq<(PartRow, int)>>,
    Ghost(db2): Ghost<Db>,
) -> (out: State)
    requires
        part_delta_ok(part_delta@, part_full),
        capture_ok(state.db(), db2, line_delta_seq_view(lineitem_delta@), part_full),
        parts_for_lines_keys@ == rescan_parts_for_lines_wanted(*state, lineitem_delta@, part_delta@),
        rescan_parts_for_lines_ok(db2, parts_for_lines_keys@, parts_for_lines_rows@),
        lines_for_parts_keys@ == rescan_lines_for_parts_wanted(*state, lineitem_delta@, part_delta@),
        rescan_lines_for_parts_ok(db2, lines_for_parts_keys@, lines_for_parts_rows@),
    ensures out.db() == db2,
{
    proof {
        use_type_invariant(state);
        lemma_changed_keys_covered(part_delta@);
    }
    let (part_changes, _) = collect_part_changes(part_delta);
    let after_counts = collect_parts(parts_for_lines_rows);
    let mut changes = Changes::new();
    let mut index: usize = 0;
    let mut processed: Ghost<int> = Ghost(0int);
    while index < lineitem_delta.len()
        invariant index <= lineitem_delta.len(), processed@ == index as int,
            state.invariant(), covered(part_delta@, lines_for_parts_keys@),
            parts_for_lines_keys@ == line_keys(lineitem_delta@),
            part_delta_ok(part_delta@, part_full), capture_ok(state.db(), db2, line_delta_seq_view(lineitem_delta@), part_full),
            rescan_parts_for_lines_ok(db2, parts_for_lines_keys@, parts_for_lines_rows@),
            forall|key: i64| (#[trigger] key_value(after_counts@, key)).count == part_count(part_input_seq_view(parts_for_lines_rows@), key as int) &&
                key_value(after_counts@, key).promo == promo_count(part_input_seq_view(parts_for_lines_rows@), key as int),
            forall|key: i64| (#[trigger] key_value(part_changes@, key)).count == part_change(part_delta@, key as int) &&
                key_value(part_changes@, key).promo == promo_change(part_delta@, key as int),
            forall|id: u64| (#[trigger] block_value(changes.blocks@, id)).count ==
                captured_count(state.db(), lineitem_delta@.take(index as int), id),
            changes.wf(),
            forall|id: u64| (#[trigger] block_value(changes.blocks@, id)).total ==
                captured_total(state.db(), lineitem_delta@.take(index as int), id),
            forall|id: u64| (#[trigger] block_value(changes.blocks@, id)).promo ==
                captured_promo(state.db(), lineitem_delta@.take(index as int), id),
        decreases lineitem_delta.len() - index,
    {
        let delta = lineitem_delta[index];
        let key = delta.row.l_partkey;
        let before = subtract(&get_key(&after_counts, key), &get_key(&part_changes, key));
        proof {
            lemma_line_delta_key(lineitem_delta@, index as int);
            lemma_rescan_catalog(db2, parts_for_lines_keys@, parts_for_lines_rows@, key, false);
            lemma_rescan_catalog(db2, parts_for_lines_keys@, parts_for_lines_rows@, key, true);
            lemma_delta_catalog(state.db(), db2, part_delta@, part_full, key as int, false);
            lemma_delta_catalog(state.db(), db2, part_delta@, part_full, key as int, true);
            assert(before.count@ == part_count(state.db().part, key as int));
            assert(before.promo@ == promo_count(state.db().part, key as int));
        }
        let value = scale(&line_moments(&before, delta.row), delta.sign);
        changes.add_day(delta.row.l_shipdate, &value);
        proof {
            assert forall|id: u64| (#[trigger] block_value(changes.blocks@, id)).count == captured_count(state.db(), lineitem_delta@.take(index as int + 1), id) &&
                block_value(changes.blocks@, id).total == captured_total(state.db(), lineitem_delta@.take(index as int + 1), id) &&
                block_value(changes.blocks@, id).promo == captured_promo(state.db(), lineitem_delta@.take(index as int + 1), id) by {
                lemma_captured_prefix(state.db(), lineitem_delta@, index as int, |line: LineitemRow| in_block(id, line.l_shipdate), false, false);
                lemma_captured_prefix(state.db(), lineitem_delta@, index as int, |line: LineitemRow| in_block(id, line.l_shipdate), true, false);
                lemma_captured_prefix(state.db(), lineitem_delta@, index as int, |line: LineitemRow| in_block(id, line.l_shipdate), true, true);
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    proof { assert(lineitem_delta@.take(index as int) =~= lineitem_delta@); }
    index = 0;
    processed = Ghost(0int);
    while index < lines_for_parts_rows.len()
        invariant index <= lines_for_parts_rows.len(), processed@ == index as int, changes.wf(),
            state.invariant(), covered(part_delta@, lines_for_parts_keys@),
            part_delta_ok(part_delta@, part_full), capture_ok(state.db(), db2, line_delta_seq_view(lineitem_delta@), part_full),
            rescan_lines_for_parts_ok(db2, lines_for_parts_keys@, lines_for_parts_rows@),
            forall|key: i64| (#[trigger] key_value(part_changes@, key)).count == part_change(part_delta@, key as int) &&
                key_value(part_changes@, key).promo == promo_change(part_delta@, key as int),
            forall|id: u64| (#[trigger] block_value(changes.blocks@, id)).count ==
                captured_count(state.db(), lineitem_delta@, id) +
                rescanned_count(part_delta@, lines_for_parts_rows@.take(index as int), id),
            forall|id: u64| (#[trigger] block_value(changes.blocks@, id)).total ==
                captured_total(state.db(), lineitem_delta@, id) +
                rescanned_total(part_delta@, lines_for_parts_rows@.take(index as int), id),
            forall|id: u64| (#[trigger] block_value(changes.blocks@, id)).promo ==
                captured_promo(state.db(), lineitem_delta@, id) +
                rescanned_promo(part_delta@, lines_for_parts_rows@.take(index as int), id),
        decreases lines_for_parts_rows.len() - index,
    {
        let line = lines_for_parts_rows[index];
        let value = line_moments(&get_key(&part_changes, line.l_partkey), line);
        changes.add_day(line.l_shipdate, &value);
        proof {
            assert forall|id: u64| (#[trigger] block_value(changes.blocks@, id)).count == captured_count(state.db(), lineitem_delta@, id) + rescanned_count(part_delta@, lines_for_parts_rows@.take(index as int + 1), id) &&
                block_value(changes.blocks@, id).total == captured_total(state.db(), lineitem_delta@, id) + rescanned_total(part_delta@, lines_for_parts_rows@.take(index as int + 1), id) &&
                block_value(changes.blocks@, id).promo == captured_promo(state.db(), lineitem_delta@, id) + rescanned_promo(part_delta@, lines_for_parts_rows@.take(index as int + 1), id) by {
                lemma_rescanned_prefix(part_delta@, lines_for_parts_rows@, index as int, |row: LineitemRow| in_block(id, row.l_shipdate), false, false);
                lemma_rescanned_prefix(part_delta@, lines_for_parts_rows@, index as int, |row: LineitemRow| in_block(id, row.l_shipdate), true, false);
                lemma_rescanned_prefix(part_delta@, lines_for_parts_rows@, index as int, |row: LineitemRow| in_block(id, row.l_shipdate), true, true);
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    let root = changes.publish(&state.root);
    proof {
        assert(lines_for_parts_rows@.take(index as int) =~= lines_for_parts_rows@);
        assert forall|id: u64| id < 17179869184u64 implies {
            &&& (#[trigger] storage::lookup(root, 7, id)).count == block_count(db2, id)
            &&& storage::lookup(root, 7, id).total == block_total(db2, id)
            &&& storage::lookup(root, 7, id).promo == block_promo(db2, id)
        } by {
            lemma_apply_moments(state.db(), db2, lineitem_delta@, part_delta@, part_full, lines_for_parts_keys@, lines_for_parts_rows@, id);
            assert(state.moment(id) == storage::lookup(state.root, 7, id));
        }
    }
    State { root, database: Ghost(db2) }
}

pub fn apply(
    state: &State,
    lineitem_delta: &[LineDelta],
    part_delta: &[PartDelta],
    parts_for_lines_keys: &[i64],
    parts_for_lines_rows: &Vec<PartInput>,
    lines_for_parts_keys: &[i64],
    lines_for_parts_rows: &[LineInput],
    Ghost(part_full): Ghost<Seq<(PartRow, int)>>,
    Ghost(db2): Ghost<Db>,
) -> (out: Result<State, u32>)
    requires
        part_delta_ok(part_delta@, part_full),
        capture_ok(state.db(), db2, line_delta_seq_view(lineitem_delta@), part_full),
        parts_for_lines_keys@ == rescan_parts_for_lines_wanted(*state, lineitem_delta@, part_delta@),
        rescan_parts_for_lines_ok(db2, parts_for_lines_keys@, parts_for_lines_rows@),
        lines_for_parts_keys@ == rescan_lines_for_parts_wanted(*state, lineitem_delta@, part_delta@),
        rescan_lines_for_parts_ok(db2, lines_for_parts_keys@, lines_for_parts_rows@),
    ensures out matches Ok(result) ==> result.db() == db2,
        out is Ok,
{
    Ok(apply_inner(state, lineitem_delta, part_delta, parts_for_lines_keys, parts_for_lines_rows,
        lines_for_parts_keys, lines_for_parts_rows, Ghost(part_full), Ghost(db2)))
}

}
