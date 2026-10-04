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

verus! {

pub struct State {
    root: Option<Arc<Directory>>,
    database: Ghost<Db>,
}

impl State {
    pub closed spec fn db(self) -> Db { self.database@ }

    pub closed spec fn moment(self, id: u64) -> Moments {
        storage::lookup(self.root, 7, id)
    }

    #[verifier::type_invariant]
    spec fn invariant(self) -> bool {
        &&& storage::valid(self.root, 7)
        &&& forall|id: u64| id < 17179869184u64 ==> {
            &&& (#[trigger] self.moment(id)).count as int == block_count(self.db(), id)
            &&& self.moment(id).total as int == block_total(self.db(), id)
            &&& self.moment(id).promo as int == block_promo(self.db(), id)
        }
    }
}

fn collect_parts(parts: &Vec<PartInput>) -> (out: Option<HashMap<i64, Moments>>)
    ensures out matches Some(map) ==> forall|key: i64|
        (#[trigger] key_value(map@, key)).count as int == part_count(part_input_seq_view(parts@), key as int) &&
        key_value(map@, key).promo as int == promo_count(part_input_seq_view(parts@), key as int),
{
    let mut counts: HashMap<i64, Moments> = HashMap::new();
    let mut index: usize = 0;
    let mut processed: Ghost<int> = Ghost(0int);
    while index < parts.len()
        invariant index <= parts.len(), processed@ == index as int,
            forall|key: i64| (#[trigger] key_value(counts@, key)).count as int ==
                part_count(part_input_seq_view(parts@.take(index as int)), key as int),
            forall|key: i64| (#[trigger] key_value(counts@, key)).promo as int ==
                promo_count(part_input_seq_view(parts@.take(index as int)), key as int),
        decreases parts.len() - index,
    {
        let part = &parts[index];
        let before = get_key(&counts, part.p_partkey);
        let change = Moments { count: 1, total: 0, promo: if is_promo(&part.p_type) { 1 } else { 0 } };
        let after = match add(before, change) { Some(value) => value, None => return None };
        let ghost previous = counts@;
        counts.insert(part.p_partkey, after);
        proof {
            assert forall|key: i64| (#[trigger] key_value(counts@, key)).count as int ==
                part_count(part_input_seq_view(parts@.take(index as int + 1)), key as int) &&
                key_value(counts@, key).promo as int == promo_count(part_input_seq_view(parts@.take(index as int + 1)), key as int) by {
                lemma_part_prefix(parts@, index as int, key as int, false);
                lemma_part_prefix(parts@, index as int, key as int, true);
                assert(key_value(previous, key).count as int == part_count(part_input_seq_view(parts@.take(index as int)), key as int));
                assert(key_value(previous, key).promo as int == promo_count(part_input_seq_view(parts@.take(index as int)), key as int));
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    proof { assert(parts@.take(index as int) =~= parts@); }
    Some(counts)
}

fn collect_part_changes(delta: &[PartDelta]) -> (out: Option<(HashMap<i64, Moments>, Vec<i64>)>)
    ensures out matches Some((map, keys)) ==> {
        &&& forall|key: i64| (#[trigger] key_value(map@, key)).count as int == part_change(delta@, key as int) &&
            key_value(map@, key).promo as int == promo_change(delta@, key as int)
        &&& keys@.no_duplicates()
        &&& forall|key: i64| #[trigger] keys@.contains(key) == part_key_support(delta@, key)
    },
{
    let mut counts: HashMap<i64, Moments> = HashMap::new();
    let mut keys: Vec<i64> = Vec::new();
    let mut index: usize = 0;
    let mut processed: Ghost<int> = Ghost(0int);
    while index < delta.len()
        invariant index <= delta.len(), processed@ == index as int,
            forall|key: i64| (#[trigger] key_value(counts@, key)).count as int ==
                part_change(delta@.take(index as int), key as int),
            forall|key: i64| (#[trigger] key_value(counts@, key)).promo as int ==
                promo_change(delta@.take(index as int), key as int),
            keys@.no_duplicates(),
            forall|key: i64| #[trigger] counts@.dom().contains(key) == keys@.contains(key),
            forall|key: i64| #[trigger] keys@.contains(key) == part_key_support(delta@.take(index as int), key),
        decreases delta.len() - index,
    {
        let part = &delta[index];
        let ghost previous = counts@;
        let ghost previous_keys = keys@;
        let add_key = !counts.contains_key(&part.p_partkey);
        if add_key { keys.push(part.p_partkey); }
        let before = get_key(&counts, part.p_partkey);
        let change = Moments { count: part.sign as i128, total: 0,
            promo: if part.is_promo != 0 { part.sign as i128 } else { 0 } };
        let after = match add(before, change) { Some(value) => value, None => return None };
        counts.insert(part.p_partkey, after);
        proof {
            assert forall|key: i64| (#[trigger] key_value(counts@, key)).count as int == part_change(delta@.take(index as int + 1), key as int) &&
                key_value(counts@, key).promo as int == promo_change(delta@.take(index as int + 1), key as int) by {
                lemma_part_change_prefix(delta@, index as int, key as int, false);
                lemma_part_change_prefix(delta@, index as int, key as int, true);
                assert(key_value(previous, key).count as int == part_change(delta@.take(index as int), key as int));
                assert(key_value(previous, key).promo as int == promo_change(delta@.take(index as int), key as int));
            }
            assert forall|key: i64| #[trigger] keys@.contains(key) == part_key_support(delta@.take(index as int + 1), key) by {
                if add_key { lemma_push_contains(previous_keys, part.p_partkey, key); }
                lemma_part_support_prefix(delta@, index as int, key);
                assert(previous_keys.contains(key) == part_key_support(delta@.take(index as int), key));
            }
            assert forall|key: i64| #[trigger] counts@.dom().contains(key) == keys@.contains(key) by {
                if add_key { lemma_push_contains(previous_keys, part.p_partkey, key); }
                assert(previous.dom().contains(key) == previous_keys.contains(key));
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    proof { assert(delta@.take(index as int) =~= delta@); }
    Some((counts, keys))
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
{
    let counts = match collect_parts(&parts) { Some(value) => value, None => return None };
    let mut changes = Changes::new();
    let mut index: usize = 0;
    let mut processed: Ghost<int> = Ghost(0int);
    while index < lines.len()
        invariant index <= lines.len(), processed@ == index as int, changes.wf(),
            forall|key: i64| (#[trigger] key_value(counts@, key)).count as int == part_count(part_input_seq_view(parts@), key as int) &&
                key_value(counts@, key).promo as int == promo_count(part_input_seq_view(parts@), key as int),
            forall|id: u64| (#[trigger] block_value(changes.blocks@, id)).count as int ==
                scanned_count(parts@, lines@.take(index as int), id),
            forall|id: u64| (#[trigger] block_value(changes.blocks@, id)).total as int ==
                scanned_total(parts@, lines@.take(index as int), id),
            forall|id: u64| (#[trigger] block_value(changes.blocks@, id)).promo as int ==
                scanned_promo(parts@, lines@.take(index as int), id),
        decreases lines.len() - index,
    {
        let line = lines[index];
        let value = match line_moments(get_key(&counts, line.l_partkey), line) {
            Some(value) => value, None => return None,
        };
        if !changes.add_day(line.l_shipdate, value) { return None; }
        proof {
            assert forall|id: u64| (#[trigger] block_value(changes.blocks@, id)).count as int == scanned_count(parts@, lines@.take(index as int + 1), id) &&
                block_value(changes.blocks@, id).total as int == scanned_total(parts@, lines@.take(index as int + 1), id) &&
                block_value(changes.blocks@, id).promo as int == scanned_promo(parts@, lines@.take(index as int + 1), id) by {
                lemma_join_prefix(parts@, lines@, index as int, |row: LineitemRow| in_block(id, row.l_shipdate), false, false);
                lemma_join_prefix(parts@, lines@, index as int, |row: LineitemRow| in_block(id, row.l_shipdate), true, false);
                lemma_join_prefix(parts@, lines@, index as int, |row: LineitemRow| in_block(id, row.l_shipdate), true, true);
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    let root = match changes.publish(&None) { Some(value) => value, None => return None };
    proof {
        assert(lines@.take(index as int) =~= lines@);
        assert forall|id: u64| id < 17179869184u64 implies {
            &&& (#[trigger] storage::lookup(root, 7, id)).count as int == block_count(db, id)
            &&& storage::lookup(root, 7, id).total as int == block_total(db, id)
            &&& storage::lookup(root, 7, id).promo as int == block_promo(db, id)
        } by { lemma_build_moments(db, parts@, lines@, id); }
    }
    Some(State { root, database: Ghost(db) })
}

pub fn execute(state: &State, p1: i32, p2: i32) -> (out: Option<Vec<Option<(i128, i128)>>>)
    ensures out matches Some(rows) ==> query_result(state.db(), params(p1 as int, p2 as int), out_view(rows@)),
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
            accumulator.count as int == interval_count(state.db(), p1 as int, start as int - 2147483648),
            accumulator.total as int == interval_total(state.db(), p1 as int, start as int - 2147483648),
            accumulator.promo as int == interval_promo(state.db(), p1 as int, start as int - 2147483648),
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
        accumulator = match add(accumulator, value) { Some(value) => value, None => return None };
        proof {
            assert(state.moment(id) == storage::lookup(state.root, 7, id));
            lemma_interval_step(state.db(), p1 as int, start as int, level as nat, id);
        }
        start += width;
    }
    if accumulator.count == 0 {
        rows.push(None);
        proof {
            lemma_execute_result(state.db(), p1 as int, p2 as int, accumulator.count as int,
                accumulator.total as int, accumulator.promo as int, rows@);
        }
    } else {
        if accumulator.total == 0 { return None; }
        let numerator = match accumulator.promo.checked_mul(100000000i128) {
            Some(value) => value, None => return None,
        };
        let denominator = match accumulator.total.checked_mul(1000000i128) {
            Some(value) => value, None => return None,
        };
        rows.push(Some((numerator, denominator)));
        proof {
            lemma_execute_result(state.db(), p1 as int, p2 as int, accumulator.count as int,
                accumulator.total as int, accumulator.promo as int, rows@);
        }
    }
    Some(rows)
}

pub fn rescan_parts_for_lines_keys(
    _state: &State,
    lineitem_delta: &[LineDelta],
    _part_delta: &[PartDelta],
) -> (out: Vec<i64>)
    ensures out@.no_duplicates(),
        forall|key: i64| #[trigger] out@.contains(key) == line_key_support(lineitem_delta@, key),
{
    let mut keys: Vec<i64> = Vec::new();
    let mut seen: HashMap<i64, bool> = HashMap::new();
    let mut index: usize = 0;
    let mut processed: Ghost<int> = Ghost(0int);
    while index < lineitem_delta.len()
        invariant index <= lineitem_delta.len(), processed@ == index as int,
            keys@.no_duplicates(),
            forall|key: i64| #[trigger] seen@.dom().contains(key) == keys@.contains(key),
            forall|key: i64| #[trigger] keys@.contains(key) == line_key_support(lineitem_delta@.take(index as int), key),
        decreases lineitem_delta.len() - index,
    {
        let key = lineitem_delta[index].row.l_partkey;
        let ghost earlier = keys@;
        let ghost previous = seen@;
        let add_key = !seen.contains_key(&key);
        if add_key { seen.insert(key, true); keys.push(key); }
        proof {
            assert forall|candidate: i64| #[trigger] keys@.contains(candidate) == line_key_support(lineitem_delta@.take(index as int + 1), candidate) by {
                if add_key { lemma_push_contains(earlier, key, candidate); }
                lemma_line_support_prefix(lineitem_delta@, index as int, candidate);
                assert(earlier.contains(candidate) == line_key_support(lineitem_delta@.take(index as int), candidate));
            }
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
    _state: &State,
    _lineitem_delta: &[LineDelta],
    part_delta: &[PartDelta],
) -> (out: Vec<i64>)
{
    let mut result: Vec<i64> = Vec::new();
    let (changes, keys) = match collect_part_changes(part_delta) {
        Some(value) => value,
        None => {
            let mut index: usize = 0;
            let mut processed: Ghost<int> = Ghost(0int);
            while index < part_delta.len()
                invariant index <= part_delta.len(), processed@ == index as int,
                decreases part_delta.len() - index,
            {
                result.push(part_delta[index].p_partkey);
                index += 1;
                processed = Ghost(index as int);
            }
            return result;
        },
    };
    let mut index: usize = 0;
    while index < keys.len()
        invariant index <= keys.len(),
        decreases keys.len() - index,
    {
        let value = get_key(&changes, keys[index]);
        if value.count != 0 || value.promo != 0 { result.push(keys[index]); }
        index += 1;
    }
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
) -> (out: Option<State>)
    requires
        part_delta_ok(part_delta@, part_full),
        capture_ok(state.db(), db2, line_delta_seq_view(lineitem_delta@), part_full),
        rescan_parts_for_lines_ok(db2, parts_for_lines_keys@, parts_for_lines_rows@),
        rescan_lines_for_parts_ok(db2, lines_for_parts_keys@, lines_for_parts_rows@),
    ensures out matches Some(result) ==> result.db() == db2,
{
    proof { use_type_invariant(state); }
    let (part_changes, part_keys) = match collect_part_changes(part_delta) {
        Some(value) => value, None => return None,
    };
    let part_coverage = key_set(parts_for_lines_keys);
    let line_coverage = key_set(lines_for_parts_keys);
    let after_counts = match collect_parts(parts_for_lines_rows) { Some(value) => value, None => return None };
    let mut index: usize = 0;
    while index < part_keys.len()
        invariant index <= part_keys.len(),
            forall|key: i64| (#[trigger] key_value(part_changes@, key)).count as int == part_change(part_delta@, key as int) &&
                key_value(part_changes@, key).promo as int == promo_change(part_delta@, key as int),
            forall|key: i64| #[trigger] line_coverage@.dom().contains(key) == lines_for_parts_keys@.contains(key),
            forall|key: i64| #[trigger] part_keys@.contains(key) == part_key_support(part_delta@, key),
            forall|position: int| 0 <= position < index ==>
                ((#[trigger] part_keys@[position]) == part_keys@[position] &&
                (part_change(part_delta@, part_keys@[position] as int) != 0 || promo_change(part_delta@, part_keys@[position] as int) != 0)
                    ==> lines_for_parts_keys@.contains(part_keys@[position])),
        decreases part_keys.len() - index,
    {
        let key = part_keys[index];
        let change = get_key(&part_changes, key);
        if (change.count != 0 || change.promo != 0) && !line_coverage.contains_key(&key) { return None; }
        index += 1;
    }
    proof {
        assert forall|key: int| #[trigger] seq_view_i64(lines_for_parts_keys@).contains(key) ||
            (part_change(part_delta@, key) == 0 && promo_change(part_delta@, key) == 0) by {
            if part_change(part_delta@, key) != 0 || promo_change(part_delta@, key) != 0 {
            lemma_change_support(part_delta@, key);
            let runtime_key = key as i64;
            assert(part_keys@.contains(runtime_key));
            let position = choose|position: int| 0 <= position < part_keys.len() && #[trigger] part_keys@[position] == runtime_key;
            assert(lines_for_parts_keys@.contains(runtime_key));
            let covered_index = choose|covered_index: int| 0 <= covered_index < lines_for_parts_keys.len() && #[trigger] lines_for_parts_keys@[covered_index] == runtime_key;
            assert(seq_view_i64(lines_for_parts_keys@)[covered_index] == key);
            }
        }
    }
    let mut changes = Changes::new();
    index = 0;
    let mut processed: Ghost<int> = Ghost(0int);
    while index < lineitem_delta.len()
        invariant index <= lineitem_delta.len(), processed@ == index as int,
            state.invariant(), covered(part_delta@, lines_for_parts_keys@),
            forall|key: i64| #[trigger] part_coverage@.dom().contains(key) == parts_for_lines_keys@.contains(key),
            part_delta_ok(part_delta@, part_full), capture_ok(state.db(), db2, line_delta_seq_view(lineitem_delta@), part_full),
            rescan_parts_for_lines_ok(db2, parts_for_lines_keys@, parts_for_lines_rows@),
            forall|key: i64| (#[trigger] key_value(after_counts@, key)).count as int == part_count(part_input_seq_view(parts_for_lines_rows@), key as int) &&
                key_value(after_counts@, key).promo as int == promo_count(part_input_seq_view(parts_for_lines_rows@), key as int),
            forall|key: i64| (#[trigger] key_value(part_changes@, key)).count as int == part_change(part_delta@, key as int) &&
                key_value(part_changes@, key).promo as int == promo_change(part_delta@, key as int),
            forall|id: u64| (#[trigger] block_value(changes.blocks@, id)).count as int ==
                captured_count(state.db(), lineitem_delta@.take(index as int), id),
            changes.wf(),
            forall|id: u64| (#[trigger] block_value(changes.blocks@, id)).total as int ==
                captured_total(state.db(), lineitem_delta@.take(index as int), id),
            forall|id: u64| (#[trigger] block_value(changes.blocks@, id)).promo as int ==
                captured_promo(state.db(), lineitem_delta@.take(index as int), id),
        decreases lineitem_delta.len() - index,
    {
        let delta = lineitem_delta[index];
        let key = delta.row.l_partkey;
        if !part_coverage.contains_key(&key) { return None; }
        let before = match subtract(get_key(&after_counts, key), get_key(&part_changes, key)) {
            Some(value) => value, None => return None,
        };
        proof {
            lemma_rescan_catalog(db2, parts_for_lines_keys@, parts_for_lines_rows@, key, false);
            lemma_rescan_catalog(db2, parts_for_lines_keys@, parts_for_lines_rows@, key, true);
            lemma_delta_catalog(state.db(), db2, part_delta@, part_full, key as int, false);
            lemma_delta_catalog(state.db(), db2, part_delta@, part_full, key as int, true);
            assert(before.count as int == part_count(state.db().part, key as int));
            assert(before.promo as int == promo_count(state.db().part, key as int));
        }
        if before.count < 0 || before.promo < 0 || before.promo > before.count { return None; }
        let value = match line_moments(before, delta.row) { Some(value) => value, None => return None };
        let value = match scale(value, delta.sign as i128) { Some(value) => value, None => return None };
        if !changes.add_day(delta.row.l_shipdate, value) { return None; }
        proof {
            assert forall|id: u64| (#[trigger] block_value(changes.blocks@, id)).count as int == captured_count(state.db(), lineitem_delta@.take(index as int + 1), id) &&
                block_value(changes.blocks@, id).total as int == captured_total(state.db(), lineitem_delta@.take(index as int + 1), id) &&
                block_value(changes.blocks@, id).promo as int == captured_promo(state.db(), lineitem_delta@.take(index as int + 1), id) by {
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
            forall|key: i64| (#[trigger] key_value(part_changes@, key)).count as int == part_change(part_delta@, key as int) &&
                key_value(part_changes@, key).promo as int == promo_change(part_delta@, key as int),
            forall|id: u64| (#[trigger] block_value(changes.blocks@, id)).count as int ==
                captured_count(state.db(), lineitem_delta@, id) +
                rescanned_count(part_delta@, lines_for_parts_rows@.take(index as int), id),
            forall|id: u64| (#[trigger] block_value(changes.blocks@, id)).total as int ==
                captured_total(state.db(), lineitem_delta@, id) +
                rescanned_total(part_delta@, lines_for_parts_rows@.take(index as int), id),
            forall|id: u64| (#[trigger] block_value(changes.blocks@, id)).promo as int ==
                captured_promo(state.db(), lineitem_delta@, id) +
                rescanned_promo(part_delta@, lines_for_parts_rows@.take(index as int), id),
        decreases lines_for_parts_rows.len() - index,
    {
        let line = lines_for_parts_rows[index];
        let value = match line_moments(get_key(&part_changes, line.l_partkey), line) {
            Some(value) => value, None => return None,
        };
        if !changes.add_day(line.l_shipdate, value) { return None; }
        proof {
            assert forall|id: u64| (#[trigger] block_value(changes.blocks@, id)).count as int == captured_count(state.db(), lineitem_delta@, id) + rescanned_count(part_delta@, lines_for_parts_rows@.take(index as int + 1), id) &&
                block_value(changes.blocks@, id).total as int == captured_total(state.db(), lineitem_delta@, id) + rescanned_total(part_delta@, lines_for_parts_rows@.take(index as int + 1), id) &&
                block_value(changes.blocks@, id).promo as int == captured_promo(state.db(), lineitem_delta@, id) + rescanned_promo(part_delta@, lines_for_parts_rows@.take(index as int + 1), id) by {
                lemma_rescanned_prefix(part_delta@, lines_for_parts_rows@, index as int, |row: LineitemRow| in_block(id, row.l_shipdate), false, false);
                lemma_rescanned_prefix(part_delta@, lines_for_parts_rows@, index as int, |row: LineitemRow| in_block(id, row.l_shipdate), true, false);
                lemma_rescanned_prefix(part_delta@, lines_for_parts_rows@, index as int, |row: LineitemRow| in_block(id, row.l_shipdate), true, true);
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    let root = match changes.publish(&state.root) { Some(value) => value, None => return None };
    proof {
        assert(lines_for_parts_rows@.take(index as int) =~= lines_for_parts_rows@);
        assert forall|id: u64| id < 17179869184u64 implies {
            &&& (#[trigger] storage::lookup(root, 7, id)).count as int == block_count(db2, id)
            &&& storage::lookup(root, 7, id).total as int == block_total(db2, id)
            &&& storage::lookup(root, 7, id).promo as int == block_promo(db2, id)
        } by {
            lemma_apply_moments(state.db(), db2, lineitem_delta@, part_delta@, part_full, lines_for_parts_keys@, lines_for_parts_rows@, id);
            assert(state.moment(id) == storage::lookup(state.root, 7, id));
        }
    }
    Some(State { root, database: Ghost(db2) })
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
        rescan_parts_for_lines_ok(db2, parts_for_lines_keys@, parts_for_lines_rows@),
        rescan_lines_for_parts_ok(db2, lines_for_parts_keys@, lines_for_parts_rows@),
    ensures out matches Ok(result) ==> result.db() == db2,
{
    match apply_inner(state, lineitem_delta, part_delta, parts_for_lines_keys, parts_for_lines_rows,
        lines_for_parts_keys, lines_for_parts_rows, Ghost(part_full), Ghost(db2)) {
        Some(result) => Ok(result), None => Err(1),
    }
}

}
