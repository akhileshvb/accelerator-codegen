use vstd::prelude::*;
use crate::storage::*;
use crate::values::*;
use crate::aggregate::*;
use crate::kernel::State;
use crate::gen_obligations::*;
use crate::meaning::*;
use verus_common::sums::*;
use verus_common::sqlfns::civil_year;
use crate::gen_spec::{Db, OrdersRow};

verus! {

pub open spec fn cached_green(state: State, key: i64) -> int {
    match state.part_model(key) { Some(part) => part.green as int, None => 0int }
}

pub open spec fn overlay_green(state: State, work: Work<Part>, key: i64) -> int {
    if work.rows@.contains_key(key) { work.rows@[key].green as int } else { cached_green(state, key) }
}

pub open spec fn cached_pair(state: State, key: i64, supplier: int, field: int) -> int {
    match state.part_model(key) { Some(part) => stored_pair(part, supplier, field), None => 0int }
}

pub open spec fn cached_moment(state: State, key: i64, supplier: int, year: int, field: int) -> int {
    match state.part_model(key) { Some(part) => stored_moment(part, supplier, year, field), None => 0int }
}

pub open spec fn overlay_pair(state: State, work: Work<Part>, key: i64, supplier: int, field: int) -> int {
    if work.rows@.contains_key(key) { stored_pair(work.rows@[key], supplier, field) } else { cached_pair(state, key, supplier, field) }
}

pub open spec fn overlay_moment(state: State, work: Work<Part>, key: i64, supplier: int, year: int, field: int) -> int {
    if work.rows@.contains_key(key) { stored_moment(work.rows@[key], supplier, year, field) } else { cached_moment(state, key, supplier, year, field) }
}

pub open spec fn part_change_weight(key: i64) -> spec_fn(PartDelta) -> int {
    |change: PartDelta| if change.p_partkey == key && change.is_green != 0 { change.sign as int } else { 0int }
}

pub open spec fn part_key_of() -> spec_fn(PartDelta) -> i64 {
    |change: PartDelta| change.p_partkey
}

pub fn has_key(keys: &[i64], key: i64) -> (result: bool)
    ensures result == keys@.contains(key),
{
    let mut index = 0;
    while index < keys.len()
        invariant index <= keys.len(), !keys@.take(index as int).contains(key),
        decreases keys.len() - index,
    {
        if keys[index] == key { return true; }
        index += 1;
    }
    proof { assert(keys@.take(index as int) =~= keys@); }
    false
}

pub fn touched_parts(state: &State, changes: &[PartDelta]) -> (result: Option<Work<Part>>)
    requires state.valid(),
    ensures result matches Some(work) ==> work.valid()
        && (forall|key: i64| #[trigger] overlay_green(*state, work, key) == cached_green(*state, key) + sum(changes@, part_change_weight(key)))
        && (forall|key: i64| #[trigger] work.rows@.contains_key(key) <==> changes@.map_values(part_key_of()).contains(key))
        && (forall|key: i64| #[trigger] work.rows@.contains_key(key) ==> 0 <= work.rows@[key].green <= u64::MAX)
        && parts_unique(work)
        && (forall|key: i64, supplier: int, field: int| #[trigger] overlay_pair(*state, work, key, supplier, field) == cached_pair(*state, key, supplier, field))
        && (forall|key: i64, supplier: int, year: int, field: int| #[trigger] overlay_moment(*state, work, key, supplier, year, field) == cached_moment(*state, key, supplier, year, field)),
{
    broadcast use vstd::seq_lib::group_seq_properties;
    let mut work = Work::new();
    let mut index = 0;
    let mut processed = Ghost(0int);
    while index < changes.len()
        invariant index <= changes.len(), processed@ == index as int,
            work.valid(), state.valid(), parts_unique(work),
            forall|key: i64, supplier: int, field: int| #[trigger] overlay_pair(*state, work, key, supplier, field) == cached_pair(*state, key, supplier, field),
            forall|key: i64, supplier: int, year: int, field: int| #[trigger] overlay_moment(*state, work, key, supplier, year, field) == cached_moment(*state, key, supplier, year, field),
            forall|key: i64| #[trigger] overlay_green(*state, work, key) == cached_green(*state, key) + sum(changes@.take(processed@), part_change_weight(key)),
            forall|key: i64| #[trigger] work.rows@.contains_key(key) <==> changes@.take(processed@).map_values(part_key_of()).contains(key),
        decreases changes.len() - index,
    {
        let change = changes[index];
        let key = change.p_partkey;
        let ghost before = work;
        proof {
            assert forall|target: i64| #[trigger] overlay_green(*state, before, target) == cached_green(*state, target) + sum(changes@.take(index as int), part_change_weight(target)) by {}
            assert forall|target: i64| #[trigger] before.rows@.contains_key(target) <==> changes@.take(index as int).map_values(part_key_of()).contains(target) by {}
            assert forall|target: i64, supplier: int, field: int| #[trigger] overlay_pair(*state, before, target, supplier, field) == cached_pair(*state, target, supplier, field) by {}
            assert forall|target: i64, supplier: int, target_year: int, field: int| #[trigger] overlay_moment(*state, before, target, supplier, target_year, field) == cached_moment(*state, target, supplier, target_year, field) by {}
        }
        let mut part = take_part(state, &mut work, key);
        let ghost old_part = part;
        proof {
            assert(part_unique(part));
            assert forall|supplier: int, field: int| #[trigger] stored_pair(old_part, supplier, field) == cached_pair(*state, key, supplier, field) by {
                assert(stored_pair(old_part, supplier, field) == overlay_pair(*state, before, key, supplier, field));
            }
            assert forall|supplier: int, target_year: int, field: int| #[trigger] stored_moment(old_part, supplier, target_year, field) == cached_moment(*state, key, supplier, target_year, field) by {
                assert(stored_moment(old_part, supplier, target_year, field) == overlay_moment(*state, before, key, supplier, target_year, field));
            }
        }
        proof { assert(part.green as int == overlay_green(*state, before, key)); }
        if change.is_green != 0 { part.green = part.green.checked_add(change.sign as i128)?; }
        let ghost completed = part;
        proof {
            assert forall|supplier: int, field: int| #[trigger] stored_pair(completed, supplier, field) == cached_pair(*state, key, supplier, field) by {
                assert(completed.pairs@ == old_part.pairs@);
                assert(stored_pair(completed, supplier, field) == stored_pair(old_part, supplier, field));
            }
            assert forall|supplier: int, target_year: int, field: int| #[trigger] stored_moment(completed, supplier, target_year, field) == cached_moment(*state, key, supplier, target_year, field) by {
                assert(completed.moments@ == old_part.moments@);
                assert(stored_moment(completed, supplier, target_year, field) == stored_moment(old_part, supplier, target_year, field));
            }
        }
        work.put(key, part);
        proof {
            assert forall|target: i64, supplier: int, field: int| #[trigger] overlay_pair(*state, work, target, supplier, field) == cached_pair(*state, target, supplier, field) by {
                if target == key { assert(work.rows@[key].pairs@ == old_part.pairs@); }
                else { assert(overlay_pair(*state, work, target, supplier, field) == overlay_pair(*state, before, target, supplier, field)); }
            }
            assert forall|target: i64, supplier: int, target_year: int, field: int| #[trigger] overlay_moment(*state, work, target, supplier, target_year, field) == cached_moment(*state, target, supplier, target_year, field) by {
                if target == key { assert(work.rows@[key].moments@ == old_part.moments@); }
                else { assert(overlay_moment(*state, work, target, supplier, target_year, field) == overlay_moment(*state, before, target, supplier, target_year, field)); }
            }
            assert forall|target: i64| #[trigger] overlay_green(*state, work, target) == cached_green(*state, target) + sum(changes@.take(index as int + 1), part_change_weight(target)) by {
                lemma_sum_take(changes@, index as int, part_change_weight(target));
                if target != key { assert(overlay_green(*state, work, target) == overlay_green(*state, before, target)); }
            }
            assert(changes@.take(index as int + 1).map_values(part_key_of())
                =~= changes@.take(index as int).map_values(part_key_of()).push(key));
            verus_prooflib::lemma_push_membership(changes@.take(index as int).map_values(part_key_of()), key);
            assert forall|target: i64| #[trigger] work.rows@.contains_key(target) <==> changes@.take(index as int + 1).map_values(part_key_of()).contains(target) by {
                assert(work.rows@.contains_key(target) == (before.rows@.contains_key(target) || target == key));
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    proof { assert(changes@.take(processed@) =~= changes@); }
    index = 0;
    while index < work.keys.len()
        invariant index <= work.keys.len(),
            work.valid(), parts_unique(work),
            forall|key: i64, supplier: int, field: int| #[trigger] overlay_pair(*state, work, key, supplier, field) == cached_pair(*state, key, supplier, field),
            forall|key: i64, supplier: int, target_year: int, field: int| #[trigger] overlay_moment(*state, work, key, supplier, target_year, field) == cached_moment(*state, key, supplier, target_year, field),
            forall|key: i64| #[trigger] overlay_green(*state, work, key) == cached_green(*state, key) + sum(changes@, part_change_weight(key)),
            forall|key: i64| #[trigger] work.rows@.contains_key(key) <==> changes@.map_values(part_key_of()).contains(key),
            forall|key: i64| work.keys@.take(index as int).contains(key) && #[trigger] work.rows@.contains_key(key) ==> 0 <= work.rows@[key].green <= u64::MAX,
        decreases work.keys.len() - index,
    {
        let key = work.keys[index];
        let part = work.rows.get(&key)?;
        if part.green < 0 || part.green > u64::MAX as i128 { return None; }
        index += 1;
    }
    proof {
        assert(work.keys@.take(index as int) =~= work.keys@);
        assert forall|key: i64| #[trigger] work.rows@.contains_key(key) implies 0 <= work.rows@[key].green <= u64::MAX by {
            assert(work.keys@.contains(key));
        }
    }
    Some(work)
}

pub fn activity(state: &State, key: i64) -> (result: bool)
    ensures result == (cached_green(*state, key) > 0),
{
    match state.part_at(key) { Some(part) => part.green > 0, None => false }
}

pub fn new_activity(state: &State, work: &Work<Part>, key: i64) -> (result: bool)
    ensures result == (overlay_green(*state, *work, key) > 0),
{
    match work.rows.get(&key) { Some(part) => part.green > 0, None => activity(state, key) }
}

pub fn flipped(state: &State, work: &Work<Part>, key: i64) -> (result: bool)
    ensures result == ((cached_green(*state, key) > 0) != (overlay_green(*state, *work, key) > 0)),
{
    activity(state, key) != new_activity(state, work, key)
}

pub open spec fn eligibility_changed(db: Db, db2: Db, key: int) -> bool {
    (green(db, key) > 0) != (green(db2, key) > 0)
}

pub open spec fn flips_ready(state: State, flips: Work<bool>, db2: Db, line_keys: Seq<i64>, pair_keys: Seq<i64>) -> bool {
    flips.valid()
        && (forall|key: i64| #[trigger] flips.rows@.contains_key(key) <==> eligibility_changed(state.db(), db2, key as int))
        && forall|key: i64| #[trigger] flips.rows@.contains_key(key) ==> line_keys.contains(key) && pair_keys.contains(key)
}

#[verifier::spinoff_prover]
pub fn prepare_flips(state: &State, parts: &mut Work<Part>, line_keys: &[i64], pair_keys: &[i64], Ghost(db2): Ghost<Db>) -> (result: Option<Work<bool>>)
    requires state.valid(), old(parts).valid(), parts_unique(*old(parts)),
        forall|key: i64| #[trigger] overlay_green(*state, *old(parts), key) == green(db2, key as int),
        forall|key: i64| #[trigger] old(parts).rows@.contains_key(key) ==> 0 <= old(parts).rows@[key].green <= u64::MAX,
        forall|key: i64, supplier: int, field: int| #[trigger] overlay_pair(*state, *old(parts), key, supplier, field) == cached_pair(*state, key, supplier, field),
        forall|key: i64, supplier: int, target_year: int, field: int| #[trigger] overlay_moment(*state, *old(parts), key, supplier, target_year, field) == cached_moment(*state, key, supplier, target_year, field),
    ensures final(parts).valid(), final(parts).keys@ == old(parts).keys@,
        forall|key: i64| #[trigger] overlay_green(*state, *final(parts), key) == green(db2, key as int),
        result matches Some(flips) ==> flips_ready(*state, flips, db2, line_keys@, pair_keys@),
        parts_unique(*final(parts)),
        forall|key: i64| #[trigger] final(parts).rows@.contains_key(key) ==> 0 <= final(parts).rows@[key].green <= u64::MAX,
        result.is_some() ==> forall|key: i64, supplier: int, field: int| #[trigger] overlay_pair(*state, *final(parts), key, supplier, field)
            == if eligibility_changed(state.db(), db2, key as int) { 0int } else { cached_pair(*state, key, supplier, field) },
        result.is_some() ==> forall|key: i64, supplier: int, target_year: int, field: int| #[trigger] overlay_moment(*state, *final(parts), key, supplier, target_year, field)
            == if eligibility_changed(state.db(), db2, key as int) { 0int } else { cached_moment(*state, key, supplier, target_year, field) },
{
    broadcast use vstd::seq_lib::group_seq_properties;
    proof { state.lemma_base_facts(); }
    let ghost initial = *parts;
    let mut flips: Work<bool> = Work::new();
    let mut index = 0;
    while index < parts.keys.len()
        invariant index <= parts.keys.len(), parts.valid(), flips.valid(), state.valid(),
            initial == *old(parts), parts.keys@ == initial.keys@,
            parts_unique(*parts),
            forall|key: i64| #[trigger] parts.rows@.contains_key(key) ==> 0 <= parts.rows@[key].green <= u64::MAX,
            forall|key: i64| #[trigger] overlay_green(*state, *parts, key) == green(db2, key as int),
            forall|key: i64| #[trigger] cached_green(*state, key) == green(state.db(), key as int),
            forall|key: i64| #[trigger] flips.rows@.contains_key(key) <==> initial.keys@.take(index as int).contains(key) && eligibility_changed(state.db(), db2, key as int),
            forall|key: i64| #[trigger] flips.rows@.contains_key(key) ==> line_keys@.contains(key) && pair_keys@.contains(key),
            forall|key: i64, supplier: int, field: int| #[trigger] overlay_pair(*state, *parts, key, supplier, field)
                == if initial.keys@.take(index as int).contains(key) && eligibility_changed(state.db(), db2, key as int) { 0int } else { cached_pair(*state, key, supplier, field) },
            forall|key: i64, supplier: int, target_year: int, field: int| #[trigger] overlay_moment(*state, *parts, key, supplier, target_year, field)
                == if initial.keys@.take(index as int).contains(key) && eligibility_changed(state.db(), db2, key as int) { 0int } else { cached_moment(*state, key, supplier, target_year, field) },
        decreases parts.keys.len() - index,
    {
        let key = parts.keys[index];
        let ghost before_flips = flips;
        let ghost before_parts = *parts;
        proof {
            assert forall|target: i64| #[trigger] overlay_green(*state, before_parts, target) == green(db2, target as int) by {}
            assert forall|target: i64, supplier: int, field: int| #[trigger] overlay_pair(*state, before_parts, target, supplier, field)
                == if initial.keys@.take(index as int).contains(target) && eligibility_changed(state.db(), db2, target as int) { 0int } else { cached_pair(*state, target, supplier, field) } by {}
            assert forall|target: i64, supplier: int, target_year: int, field: int| #[trigger] overlay_moment(*state, before_parts, target, supplier, target_year, field)
                == if initial.keys@.take(index as int).contains(target) && eligibility_changed(state.db(), db2, target as int) { 0int } else { cached_moment(*state, target, supplier, target_year, field) } by {}
        }
        let changed = flipped(state, parts, key);
        if changed {
            if !has_key(line_keys, key) || !has_key(pair_keys, key) { return None; }
            flips.put(key, true);
            let mut part = parts.take(key)?;
            part.pairs.clear();
            part.moments.clear();
            let ghost cleared = part;
            proof {
                assert forall|supplier: int, field: int| #[trigger] stored_pair(cleared, supplier, field) == 0 by {
                    lemma_sum_empty(cleared.pairs@, |row: Pair| if row.supplier == supplier { if field == 0 { row.count as int } else { row.cost as int } } else { 0int });
                }
                assert forall|supplier: int, target_year: int, field: int| #[trigger] stored_moment(cleared, supplier, target_year, field) == 0 by {
                    lemma_sum_empty(cleared.moments@, |row: Moment| if row.supplier == supplier && row.year == target_year { if field == 0 { row.cell.count as int } else if field == 1 { row.cell.revenue as int } else { row.cell.quantity as int } } else { 0int });
                }
            }
            parts.put(key, part);
        }
        proof {
            assert forall|target: i64| #[trigger] overlay_green(*state, *parts, target) == green(db2, target as int) by {
                assert(overlay_green(*state, *parts, target) == overlay_green(*state, before_parts, target));
            }
            assert(initial.keys@.take(index as int + 1) =~= initial.keys@.take(index as int).push(key));
            verus_prooflib::lemma_push_membership(initial.keys@.take(index as int), key);
            assert forall|target: i64| #[trigger] flips.rows@.contains_key(target) <==> initial.keys@.take(index as int + 1).contains(target) && eligibility_changed(state.db(), db2, target as int) by {
                if target != key { assert(flips.rows@.contains_key(target) == before_flips.rows@.contains_key(target)); }
            }
            assert forall|target: i64, supplier: int, field: int| #[trigger] overlay_pair(*state, *parts, target, supplier, field)
                == if initial.keys@.take(index as int + 1).contains(target) && eligibility_changed(state.db(), db2, target as int) { 0int } else { cached_pair(*state, target, supplier, field) } by {
                if target != key || !changed { assert(overlay_pair(*state, *parts, target, supplier, field) == overlay_pair(*state, before_parts, target, supplier, field)); }
            }
            assert forall|target: i64, supplier: int, target_year: int, field: int| #[trigger] overlay_moment(*state, *parts, target, supplier, target_year, field)
                == if initial.keys@.take(index as int + 1).contains(target) && eligibility_changed(state.db(), db2, target as int) { 0int } else { cached_moment(*state, target, supplier, target_year, field) } by {
                if target != key || !changed { assert(overlay_moment(*state, *parts, target, supplier, target_year, field) == overlay_moment(*state, before_parts, target, supplier, target_year, field)); }
            }
        }
        index += 1;
    }
    proof {
        assert(initial.keys@.take(index as int) =~= initial.keys@);
        assert forall|key: i64| #[trigger] flips.rows@.contains_key(key) <==> eligibility_changed(state.db(), db2, key as int) by {
            if !parts.keys@.contains(key) {
                assert(!parts.rows@.contains_key(key));
                assert(overlay_green(*state, *parts, key) == cached_green(*state, key));
            }
        }
        assert forall|key: i64| #[trigger] eligibility_changed(state.db(), db2, key as int) implies initial.keys@.contains(key) by {
            assert(flips.rows@.contains_key(key));
        }
        assert forall|key: i64, supplier: int, field: int| #[trigger] overlay_pair(*state, *parts, key, supplier, field)
            == if eligibility_changed(state.db(), db2, key as int) { 0int } else { cached_pair(*state, key, supplier, field) } by {}
        assert forall|key: i64, supplier: int, target_year: int, field: int| #[trigger] overlay_moment(*state, *parts, key, supplier, target_year, field)
            == if eligibility_changed(state.db(), db2, key as int) { 0int } else { cached_moment(*state, key, supplier, target_year, field) } by {}
    }
    Some(flips)
}

pub fn take_part(state: &State, work: &mut Work<Part>, key: i64) -> (result: Part)
    requires state.valid(), old(work).valid(),
    ensures final(work).valid(), final(work).keys@ == old(work).keys@,
        final(work).rows@ == old(work).rows@.remove(key),
        result.green as int == overlay_green(*state, *old(work), key),
        forall|supplier: int, field: int| #[trigger] stored_pair(result, supplier, field) == overlay_pair(*state, *old(work), key, supplier, field),
        forall|supplier: int, target_year: int, field: int| #[trigger] stored_moment(result, supplier, target_year, field) == overlay_moment(*state, *old(work), key, supplier, target_year, field),
        parts_unique(*old(work)) ==> parts_unique(*final(work)) && part_unique(result),
        (forall|target: i64| #[trigger] old(work).rows@.contains_key(target) ==> 0 <= old(work).rows@[target].green <= u64::MAX) ==> 0 <= result.green <= u64::MAX,
{
    let ghost initial = *work;
    let result = match work.take(key) {
        Some(part) => part,
        None => match state.part_at(key) { Some(part) => copy_part(&part), None => empty_part() },
    };
    proof {
        if !initial.rows@.contains_key(key) && state.part_model(key).is_none() {
            assert forall|supplier: int, field: int| #[trigger] stored_pair(result, supplier, field) == 0 by {
                lemma_sum_empty(result.pairs@, |row: Pair| if row.supplier == supplier { if field == 0 { row.count as int } else { row.cost as int } } else { 0int });
            }
            assert forall|supplier: int, target_year: int, field: int| #[trigger] stored_moment(result, supplier, target_year, field) == 0 by {
                lemma_sum_empty(result.moments@, |row: Moment| if row.supplier == supplier && row.year == target_year { if field == 0 { row.cell.count as int } else if field == 1 { row.cell.revenue as int } else { row.cell.quantity as int } } else { 0int });
            }
        }
    }
    result
}

pub open spec fn cached_references(state: State, key: i64) -> int {
    match state.order_model(key) { Some(order) => order.refs as int, None => 0int }
}

pub open spec fn overlay_references(state: State, work: Work<Order>, key: i64) -> int {
    if work.rows@.contains_key(key) { work.rows@[key].refs as int } else { cached_references(state, key) }
}

pub fn bump_reference(state: &State, work: &mut Work<Order>, key: i64, amount: i128) -> (result: Option<()>)
    requires old(work).valid(),
    ensures final(work).valid(),
        result.is_some() ==> forall|target: i64| #[trigger] overlay_references(*state, *final(work), target)
            == overlay_references(*state, *old(work), target) + if target == key { amount as int } else { 0int },
        result.is_some() ==> final(work).rows@.contains_key(key),
        result.is_some() ==> forall|target: i64| target != key ==> (#[trigger] final(work).rows@.contains_key(target) == old(work).rows@.contains_key(target))
            && (final(work).rows@.contains_key(target) ==> final(work).rows@[target] == old(work).rows@[target]),
        result.is_some() ==> final(work).rows@[key].years@
            == if old(work).rows@.contains_key(key) { old(work).rows@[key].years@ }
                else if state.order_model(key).is_some() { state.order_model(key).unwrap().years@ }
                else { Seq::empty() },
{
    let ghost initial = *work;
    let mut order = match work.take(key) {
        Some(order) => order,
        None => match state.order_at(key) {
            Some(order) => Order { refs: order.refs, years: copy_years(&order.years) },
            None => Order { refs: 0, years: Vec::new() },
        },
    };
    proof { assert(order.refs as int == overlay_references(*state, initial, key)); }
    order.refs = order.refs.checked_add(amount)?;
    work.put(key, order);
    proof {
        assert forall|target: i64| #[trigger] overlay_references(*state, *work, target)
            == overlay_references(*state, initial, target) + if target == key { amount as int } else { 0int } by {}
    }
    Some(())
}

pub open spec fn post_year(work: Work<Vec<Year>>, key: i64, year: int) -> int {
    if work.rows@.contains_key(key) { year_at(work.rows@[key]@, year) } else { 0int }
}

pub open spec fn order_input_weight(key: i64, year: int) -> spec_fn(OrderIn) -> int {
    |row: OrderIn| if row.o_orderkey == key && civil_year(row.o_orderdate as int) == year { 1int } else { 0int }
}

pub proof fn lemma_year_count(rows: Seq<Year>, position: int)
    requires 0 <= position < rows.len(), rows.map_values(|row: Year| row.year).no_duplicates(),
    ensures year_at(rows, rows[position].year as int) == rows[position].count as int,
{
    let years = rows.map_values(|row: Year| row.year);
    let target = rows[position].year as int;
    let weight = |row: Year| if row.year == target { row.count as int } else { 0int };
    assert forall|index: int| 0 <= index < rows.len() && index != position implies weight(#[trigger] rows[index]) == 0 by {
        assert(years[index] != years[position]);
    }
    lemma_sum_single_match(rows, weight, position);
}

pub proof fn lemma_order_input_bound(rows: Seq<OrderIn>, key: i64, year: int)
    ensures 0 <= sum(rows, order_input_weight(key, year)) <= rows.len(),
{
    lemma_sum_nonneg(rows, order_input_weight(key, year));
    lemma_sum_le(rows, order_input_weight(key, year), |row: OrderIn| 1int);
    verus_prooflib::lemma_sum_constant_one(rows);
}

#[verifier::spinoff_prover]
pub fn post_dates(keys: &[i64], rows: &[OrderIn]) -> (result: Work<Vec<Year>>)
    ensures result.valid(),
        forall|key: i64| #[trigger] keys@.contains(key) ==> result.rows@.contains_key(key),
        forall|key: i64, year: int| #[trigger] post_year(result, key, year) == sum(rows@, order_input_weight(key, year)),
        forall|key: i64| #[trigger] result.rows@.contains_key(key) ==> years_positive(result.rows@[key]@)
            && result.rows@[key]@.map_values(|row: Year| row.year).no_duplicates(),
{
    let mut work: Work<Vec<Year>> = Work::new();
    let mut index = 0;
    while index < keys.len()
        invariant index <= keys.len(),
            work.valid(),
            forall|key: i64| #[trigger] keys@.take(index as int).contains(key) ==> work.rows@.contains_key(key),
            forall|key: i64| #[trigger] work.rows@.contains_key(key) ==> work.rows@[key]@ == Seq::<Year>::empty(),
        decreases keys.len() - index,
    {
        if !work.rows.contains_key(&keys[index]) { work.put(keys[index], Vec::new()); }
        proof {
            assert(keys@.take(index as int + 1) =~= keys@.take(index as int).push(keys[index as int]));
            verus_prooflib::lemma_push_membership(keys@.take(index as int), keys[index as int]);
        }
        index += 1;
    }
    proof {
        assert(keys@.take(index as int) =~= keys@);
        assert forall|key: i64, year: int| #[trigger] post_year(work, key, year) == sum(rows@.take(0), order_input_weight(key, year)) by {
            lemma_sum_empty(rows@.take(0), order_input_weight(key, year));
            if work.rows@.contains_key(key) {
                lemma_sum_empty(work.rows@[key]@, |row: Year| if row.year == year { row.count as int } else { 0int });
            }
        }
    }
    index = 0;
    let mut processed = Ghost(0int);
    while index < rows.len()
        invariant index <= rows.len(), processed@ == index as int,
            work.valid(),
            forall|key: i64| #[trigger] keys@.contains(key) ==> work.rows@.contains_key(key),
            forall|key: i64, year: int| #[trigger] post_year(work, key, year) == sum(rows@.take(processed@), order_input_weight(key, year)),
            forall|key: i64| #[trigger] work.rows@.contains_key(key) ==> years_positive(work.rows@[key]@)
                && work.rows@[key]@.map_values(|row: Year| row.year).no_duplicates(),
        decreases rows.len() - index,
    {
        let row = rows[index];
        let ghost before = work;
        let mut years = match work.take(row.o_orderkey) { Some(years) => years, None => Vec::new() };
        let target_year = year(row.o_orderdate);
        proof {
            assert(years_positive(years@));
            assert(years@.map_values(|row: Year| row.year).no_duplicates());
            assert forall|target: int| #[trigger] year_at(years@, target) == post_year(before, row.o_orderkey, target) by {
                if !before.rows@.contains_key(row.o_orderkey) {
                    lemma_sum_empty(years@, |row: Year| if row.year == target { row.count as int } else { 0int });
                }
            }
            assert forall|position: int| 0 <= position < years.len() && (#[trigger] years[position]).year == target_year implies
                i128::MIN <= years[position].count as int + 1 <= i128::MAX by {
                lemma_year_count(years@, position);
                lemma_order_input_bound(rows@.take(processed@), row.o_orderkey, target_year as int);
            }
        }
        let changed = bump_year(&mut years, target_year, 1);
        proof { assert(changed.is_some()); }
        work.put(row.o_orderkey, years);
        proof {
            assert forall|key: i64, target: int| #[trigger] post_year(work, key, target) == sum(rows@.take(index as int + 1), order_input_weight(key, target)) by {
                lemma_sum_take(rows@, index as int, order_input_weight(key, target));
                if key != row.o_orderkey { assert(post_year(work, key, target) == post_year(before, key, target)); }
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    proof { assert(rows@.take(processed@) =~= rows@); }
    work
}

pub open spec fn order_change_weight(key: i64, year: int) -> spec_fn(OrderDelta) -> int {
    |change: OrderDelta| change.sign as int * order_input_weight(key, year)(change.row)
}

pub open spec fn post_dates_ready(work: Work<Vec<Year>>, keys: Seq<i64>, db: Db) -> bool {
    work.valid()
        && (forall|key: i64| #[trigger] keys.contains(key) ==> work.rows@.contains_key(key))
        && (forall|key: i64, year: int| keys.contains(key) ==> #[trigger] post_year(work, key, year) == dates(db, key as int, year))
        && forall|key: i64| #[trigger] work.rows@.contains_key(key) ==> years_positive(work.rows@[key]@)
            && work.rows@[key]@.map_values(|row: Year| row.year).no_duplicates()
}

pub proof fn lemma_rescan_dates(db: Db, keys: Seq<i64>, rows: Seq<OrderIn>, key: i64, year: int)
    requires rescan_orders_for_keys_ok(db, keys, rows), keys.contains(key),
    ensures sum(rows, order_input_weight(key, year)) == dates(db, key as int, year),
{
    let position = choose|position: int| 0 <= position < keys.len() && (#[trigger] keys[position]) == key;
    assert(seq_view_i64(keys)[position] == key as int);
    assert(seq_view_i64(keys).contains(key as int));
    let weight = |row: OrdersRow| if row.o_orderkey == key as int && civil_year(row.o_orderdate) == year { 1int } else { 0int };
    let filtered = |row: OrdersRow| if seq_view_i64(keys).contains(row.o_orderkey) { weight(row) } else { 0int };
    assert(sum(order_in_seq_view(rows), weight) == sum(db.orders, filtered));
    lemma_sum_map(rows, |row: OrderIn| order_in_view(row), weight, order_input_weight(key, year));
    lemma_sum_ext_all(db.orders, filtered, weight);
}

pub proof fn lemma_order_capture(db: Db, db2: Db, changes: Seq<OrderDelta>, key: i64, year: int)
    requires bag_step(db.orders, db2.orders, order_delta_seq_view(changes)),
    ensures dates(db2, key as int, year) == dates(db, key as int, year) + sum(changes, order_change_weight(key, year)),
{
    let weight = |row: OrdersRow| if row.o_orderkey == key as int && civil_year(row.o_orderdate) == year { 1int } else { 0int };
    let signed = |entry: (OrdersRow, int)| entry.1 * weight(entry.0);
    assert(sum(db2.orders, weight) == sum(db.orders, weight) + sum(order_delta_seq_view(changes), signed));
    lemma_sum_map(changes, |change: OrderDelta| (order_in_view(change.row), change.sign as int), signed, order_change_weight(key, year));
}

pub fn rescan_dates(keys: &[i64], rows: &[OrderIn], Ghost(db): Ghost<Db>) -> (result: Work<Vec<Year>>)
    requires rescan_orders_for_keys_ok(db, keys@, rows@),
    ensures post_dates_ready(result, keys@, db),
{
    let work = post_dates(keys, rows);
    proof {
        assert forall|key: i64, year: int| keys@.contains(key) implies #[trigger] post_year(work, key, year) == dates(db, key as int, year) by {
            lemma_rescan_dates(db, keys@, rows@, key, year);
        }
    }
    work
}

#[verifier::spinoff_prover]
pub fn before_dates(state: &State, post: &Work<Vec<Year>>, changes: &[OrderDelta], key: i64, Ghost(db2): Ghost<Db>) -> (result: Option<Vec<Year>>)
    requires bag_step(state.db().orders, db2.orders, order_delta_seq_view(changes@)),
        state.order_model(key).is_none() ==> post.rows@.contains_key(key)
            && post.rows@[key]@.map_values(|row: Year| row.year).no_duplicates()
            && forall|year: int| #[trigger] post_year(*post, key, year) == dates(db2, key as int, year),
    ensures result matches Some(years) ==> years_positive(years@)
        && years@.map_values(|row: Year| row.year).no_duplicates()
        && forall|year: int| #[trigger] year_at(years@, year) == dates(state.db(), key as int, year),
{
    if let Some(order) = state.order_at(key) { return Some(copy_years(&order.years)); }
    let mut years = copy_years(post.rows.get(&key)?);
    let ghost initial = years@;
    proof {
        assert forall|year: int| #[trigger] year_at(years@, year) == year_at(initial, year) - sum(changes@.take(0), order_change_weight(key, year)) by {
            lemma_sum_empty(changes@.take(0), order_change_weight(key, year));
        }
    }
    let mut index = 0;
    let mut processed = Ghost(0int);
    while index < changes.len()
        invariant index <= changes.len(), processed@ == index as int,
            post.rows@.contains_key(key), initial == post.rows@[key]@,
            bag_step(state.db().orders, db2.orders, order_delta_seq_view(changes@)),
            forall|year: int| #[trigger] post_year(*post, key, year) == dates(db2, key as int, year),
            years@.map_values(|row: Year| row.year).no_duplicates(),
            forall|year: int| #[trigger] year_at(years@, year) == year_at(initial, year) - sum(changes@.take(processed@), order_change_weight(key, year)),
        decreases changes.len() - index,
    {
        let change = changes[index];
        let ghost before_years = years@;
        proof {
            assert forall|target: int| #[trigger] year_at(before_years, target) == year_at(initial, target) - sum(changes@.take(index as int), order_change_weight(key, target)) by {}
        }
        if change.row.o_orderkey == key {
            let target_year = year(change.row.o_orderdate);
            bump_year(&mut years, target_year, -(change.sign as i128))?;
        }
        proof {
            assert forall|target: int| #[trigger] year_at(years@, target) == year_at(initial, target) - sum(changes@.take(index as int + 1), order_change_weight(key, target)) by {
                lemma_sum_take(changes@, index as int, order_change_weight(key, target));
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    proof {
        assert(changes@.take(processed@) =~= changes@);
        assert forall|target: int| #[trigger] year_at(initial, target) == dates(db2, key as int, target) by {
            assert(year_at(initial, target) == post_year(*post, key, target));
        }
        assert forall|target: int| #[trigger] year_at(years@, target) == dates(state.db(), key as int, target) by {
            lemma_order_capture(state.db(), db2, changes@, key, target);
        }
    }
    prune_years(&mut years)?;
    Some(years)
}

pub fn copy_cached_dates(state: &State, key: i64) -> (result: Option<Vec<Year>>)
    requires state.valid(), state.order_model(key).is_some(),
    ensures result matches Some(years) ==> years_positive(years@)
        && years@.map_values(|row: Year| row.year).no_duplicates()
        && forall|year: int| #[trigger] year_at(years@, year) == dates(state.db(), key as int, year),
{
    let order = state.order_at(key)?;
    Some(copy_years(&order.years))
}

}
