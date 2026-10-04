use vstd::prelude::*;
use verus_common::sums::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::kernel::State;
use crate::storage::*;
use crate::values::*;
use crate::meaning::*;
use crate::maintenance::*;
use crate::aggregate::prune_years;

verus! {

pub open spec fn order_key_of() -> spec_fn(OrderDelta) -> i64 {
    |change: OrderDelta| change.row.o_orderkey
}

pub open spec fn order_changes_ready(work: Work<Vec<Year>>, changes: Seq<OrderDelta>) -> bool {
    work.valid()
        && (forall|key: i64| #[trigger] work.rows@.contains_key(key) <==> changes.map_values(order_key_of()).contains(key))
        && (forall|key: i64, year: int| #[trigger] post_year(work, key, year) == sum(changes, order_change_weight(key, year)))
        && forall|key: i64| #[trigger] work.rows@.contains_key(key) ==> work.rows@[key]@.map_values(|row: Year| row.year).no_duplicates()
}

#[verifier::spinoff_prover]
pub fn collect_order_changes(keys: &[i64], changes: &[OrderDelta]) -> (result: Option<Work<Vec<Year>>>)
    ensures result matches Some(work) ==> order_changes_ready(work, changes@)
        && forall|key: i64| #[trigger] work.rows@.contains_key(key) ==> keys@.contains(key),
{
    let mut work: Work<Vec<Year>> = Work::new();
    proof {
        assert forall|key: i64, year: int| #[trigger] post_year(work, key, year) == sum(changes@.take(0), order_change_weight(key, year)) by {
            lemma_sum_empty(changes@.take(0), order_change_weight(key, year));
        }
    }
    let mut index = 0;
    let mut processed = Ghost(0int);
    while index < changes.len()
        invariant index <= changes.len(), processed@ == index as int, work.valid(),
            forall|key: i64| #[trigger] work.rows@.contains_key(key) <==> changes@.take(processed@).map_values(order_key_of()).contains(key),
            forall|key: i64, year: int| #[trigger] post_year(work, key, year) == sum(changes@.take(processed@), order_change_weight(key, year)),
            forall|key: i64| #[trigger] work.rows@.contains_key(key) ==> keys@.contains(key)
                && work.rows@[key]@.map_values(|row: Year| row.year).no_duplicates(),
        decreases changes.len() - index,
    {
        let change = changes[index];
        let key = change.row.o_orderkey;
        if !has_key(keys, key) { return None; }
        let ghost before = work;
        let mut years = match work.take(key) { Some(years) => years, None => Vec::new() };
        proof {
            assert(years@.map_values(|row: Year| row.year).no_duplicates());
            assert forall|target: int| #[trigger] year_at(years@, target) == post_year(before, key, target) by {
                if !before.rows@.contains_key(key) {
                    lemma_sum_empty(years@, |row: Year| if row.year == target { row.count as int } else { 0int });
                }
            }
        }
        bump_year(&mut years, year(change.row.o_orderdate), change.sign as i128)?;
        work.put(key, years);
        proof {
            assert forall|target: i64, target_year: int| #[trigger] post_year(work, target, target_year) == sum(changes@.take(index as int + 1), order_change_weight(target, target_year)) by {
                lemma_sum_take(changes@, index as int, order_change_weight(target, target_year));
                if target != key { assert(post_year(work, target, target_year) == post_year(before, target, target_year)); }
            }
            assert(changes@.take(index as int + 1).map_values(order_key_of()) =~= changes@.take(index as int).map_values(order_key_of()).push(key));
            verus_prooflib::lemma_push_membership(changes@.take(index as int).map_values(order_key_of()), key);
        }
        index += 1;
        processed = Ghost(index as int);
    }
    proof { assert(changes@.take(processed@) =~= changes@); }
    Some(work)
}

pub open spec fn line_reference_change(db: Db, key: i64) -> spec_fn(LineDelta) -> int {
    |change: LineDelta| if change.row.l_orderkey == key && green(db, change.row.l_partkey as int) > 0 { change.sign as int } else { 0int }
}

pub open spec fn flipped_reference_weight(db: Db, db2: Db, key: i64) -> spec_fn(LineIn) -> int {
    |row: LineIn| if row.l_orderkey == key {
        (if green(db2, row.l_partkey as int) > 0 { 1int } else { 0int })
            - (if green(db, row.l_partkey as int) > 0 { 1int } else { 0int })
    } else { 0int }
}

#[verifier::spinoff_prover]
pub proof fn lemma_reference_step(state: State, db2: Db, changes: Seq<LineDelta>, flips: Work<bool>, keys: Seq<i64>, pair_keys: Seq<i64>, rows: Seq<LineIn>, key: i64)
    requires state.valid(), bag_step(state.db().lineitem, db2.lineitem, line_delta_seq_view(changes)),
        flips_ready(state, flips, db2, keys, pair_keys), rescan_lines_for_parts_ok(db2, keys, rows),
        forall|part: int| #[trigger] eligibility_changed(state.db(), db2, part) ==> i64::MIN <= part <= i64::MAX,
    ensures references(db2, key as int) == references(state.db(), key as int)
        + sum(changes, line_reference_change(state.db(), key)) + sum(rows, flipped_reference_weight(state.db(), db2, key)),
{
    let db = state.db();
    let old_weight = reference_weight(db, key as int);
    let new_weight = reference_weight(db2, key as int);
    let difference = |row: LineitemRow| new_weight(row) - old_weight(row);
    let filtered = |row: LineitemRow| if seq_view_i64(keys).contains(row.l_partkey) { difference(row) } else { 0int };
    assert forall|row: LineitemRow| #[trigger] filtered(row) == difference(row) by {
        if eligibility_changed(db, db2, row.l_partkey) {
            let part = row.l_partkey as i64;
            assert(part as int == row.l_partkey);
            assert(flips.rows@.contains_key(part));
            assert(keys.contains(part));
            let position = choose|position: int| 0 <= position < keys.len() && (#[trigger] keys[position]) == part;
            assert(seq_view_i64(keys)[position] == row.l_partkey);
            assert(seq_view_i64(keys).contains(row.l_partkey));
        }
    }
    assert(sum(line_in_seq_view(rows), difference) == sum(db2.lineitem, filtered));
    lemma_sum_ext_all(db2.lineitem, filtered, difference);
    lemma_sum_map(rows, |row: LineIn| line_in_view(row), difference, flipped_reference_weight(db, db2, key));
    lemma_sum_sub(db2.lineitem, new_weight, old_weight, difference);
    let signed = |change: (LineitemRow, int)| change.1 * old_weight(change.0);
    assert(sum(db2.lineitem, old_weight) == sum(db.lineitem, old_weight) + sum(line_delta_seq_view(changes), signed));
    lemma_sum_map(changes, |change: LineDelta| (line_in_view(change.row), change.sign as int), signed, line_reference_change(db, key));
}

pub open spec fn order_overlay_ready(state: State, work: Work<Order>, db: Db) -> bool {
    work.valid()
        && (forall|key: i64| #[trigger] overlay_references(state, work, key) == references(db, key as int))
        && (forall|key: i64| #[trigger] work.rows@.contains_key(key) ==> {
            let order = work.rows@[key];
            0 <= order.refs <= u64::MAX && (order.refs > 0 ==> order_value_valid(order, db, key as int))
        })
        && forall|key: i64| !work.rows@.contains_key(key) && (#[trigger] state.order_model(key)).is_some() ==>
            order_value_valid(state.order_model(key).unwrap(), db, key as int)
}

pub proof fn lemma_unchanged_dates(db: Db, db2: Db, changes: Seq<OrderDelta>, key: i64, year: int)
    requires bag_step(db.orders, db2.orders, order_delta_seq_view(changes)),
        !changes.map_values(order_key_of()).contains(key),
    ensures dates(db2, key as int, year) == dates(db, key as int, year),
{
    assert forall|position: int| 0 <= position < changes.len() implies order_change_weight(key, year)(#[trigger] changes[position]) == 0 by {
        assert(changes.map_values(order_key_of())[position] == changes[position].row.o_orderkey);
        assert(changes.map_values(order_key_of()).contains(changes[position].row.o_orderkey));
    }
    lemma_sum_zero(changes, order_change_weight(key, year));
    lemma_order_capture(db, db2, changes, key, year);
}

#[verifier::spinoff_prover]
pub fn update_orders(
    state: &State, parts: &Work<Part>, flips: &Work<bool>, order_changes: &Work<Vec<Year>>,
    line_changes: &[LineDelta], order_deltas: &[OrderDelta], line_keys: &[i64], pair_keys: &[i64],
    lines: &[LineIn], post_keys: &[i64], post: &Work<Vec<Year>>, Ghost(db2): Ghost<Db>,
) -> (result: Option<Work<Order>>)
    requires state.valid(), parts.valid(),
        forall|key: i64| #[trigger] overlay_green(*state, *parts, key) == green(db2, key as int),
        flips_ready(*state, *flips, db2, line_keys@, pair_keys@),
        order_changes_ready(*order_changes, order_deltas@), post_dates_ready(*post, post_keys@, db2),
        bag_step(state.db().lineitem, db2.lineitem, line_delta_seq_view(line_changes@)),
        bag_step(state.db().orders, db2.orders, order_delta_seq_view(order_deltas@)),
        rescan_lines_for_parts_ok(db2, line_keys@, lines@),
        forall|key: int| #[trigger] eligibility_changed(state.db(), db2, key) ==> i64::MIN <= key <= i64::MAX,
    ensures result matches Some(work) ==> order_overlay_ready(*state, work, db2),
{
    proof { state.lemma_base_facts(); }
    let mut work: Work<Order> = Work::new();
    proof {
        assert forall|key: i64| #[trigger] overlay_references(*state, work, key) == cached_references(*state, key)
            + sum(line_changes@.take(0), line_reference_change(state.db(), key)) by {
            lemma_sum_empty(line_changes@.take(0), line_reference_change(state.db(), key));
        }
    }
    let mut index = 0;
    let mut processed = Ghost(0int);
    while index < line_changes.len()
        invariant index <= line_changes.len(), processed@ == index as int, work.valid(), state.valid(),
            forall|part: i64| #[trigger] cached_green(*state, part) == green(state.db(), part as int),
            forall|key: i64| #[trigger] overlay_references(*state, work, key) == cached_references(*state, key)
                + sum(line_changes@.take(processed@), line_reference_change(state.db(), key)),
        decreases line_changes.len() - index,
    {
        let change = line_changes[index];
        let ghost before = work;
        proof {
            assert forall|key: i64| #[trigger] overlay_references(*state, before, key) == cached_references(*state, key)
                + sum(line_changes@.take(processed@), line_reference_change(state.db(), key)) by {}
        }
        if activity(state, change.row.l_partkey) { bump_reference(state, &mut work, change.row.l_orderkey, change.sign as i128)?; }
        proof {
            assert forall|key: i64| #[trigger] overlay_references(*state, work, key) == cached_references(*state, key)
                + sum(line_changes@.take(index as int + 1), line_reference_change(state.db(), key)) by {
                lemma_sum_take(line_changes@, index as int, line_reference_change(state.db(), key));
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    proof {
        assert(line_changes@.take(processed@) =~= line_changes@);
        assert forall|key: i64| #[trigger] overlay_references(*state, work, key) == cached_references(*state, key)
            + sum(line_changes@, line_reference_change(state.db(), key)) + sum(lines@.take(0), flipped_reference_weight(state.db(), db2, key)) by {
            lemma_sum_empty(lines@.take(0), flipped_reference_weight(state.db(), db2, key));
        }
    }
    index = 0;
    processed = Ghost(0int);
    while index < lines.len()
        invariant index <= lines.len(), processed@ == index as int, work.valid(), state.valid(),
            flips_ready(*state, *flips, db2, line_keys@, pair_keys@),
            forall|key: i64| #[trigger] overlay_green(*state, *parts, key) == green(db2, key as int),
            forall|key: i64| #[trigger] overlay_references(*state, work, key) == cached_references(*state, key)
                + sum(line_changes@, line_reference_change(state.db(), key)) + sum(lines@.take(processed@), flipped_reference_weight(state.db(), db2, key)),
        decreases lines.len() - index,
    {
        let line = lines[index];
        let ghost before = work;
        proof {
            assert forall|key: i64| #[trigger] overlay_references(*state, before, key) == cached_references(*state, key)
                + sum(line_changes@, line_reference_change(state.db(), key)) + sum(lines@.take(processed@), flipped_reference_weight(state.db(), db2, key)) by {}
        }
        if flips.rows.contains_key(&line.l_partkey) {
            let amount: i128 = if new_activity(state, parts, line.l_partkey) { 1 } else { -1 };
            proof {
                assert(amount as int == (if green(db2, line.l_partkey as int) > 0 { 1int } else { 0int })
                    - (if green(state.db(), line.l_partkey as int) > 0 { 1int } else { 0int }));
            }
            bump_reference(state, &mut work, line.l_orderkey, amount)?;
        }
        proof {
            assert forall|key: i64| #[trigger] overlay_references(*state, work, key) == cached_references(*state, key)
                + sum(line_changes@, line_reference_change(state.db(), key)) + sum(lines@.take(index as int + 1), flipped_reference_weight(state.db(), db2, key)) by {
                lemma_sum_take(lines@, index as int, flipped_reference_weight(state.db(), db2, key));
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    proof {
        assert(lines@.take(processed@) =~= lines@);
        state.lemma_base_facts();
        assert forall|key: i64| #[trigger] overlay_references(*state, work, key) == references(db2, key as int) by {
            lemma_reference_step(*state, db2, line_changes@, *flips, line_keys@, pair_keys@, lines@, key);
        }
    }
    index = 0;
    while index < order_changes.keys.len()
        invariant index <= order_changes.keys.len(), work.valid(), state.valid(),
            order_changes_ready(*order_changes, order_deltas@),
            forall|key: i64| #[trigger] overlay_references(*state, work, key) == references(db2, key as int),
            forall|key: i64| order_changes.keys@.take(index as int).contains(key) && order_changes.rows@.contains_key(key)
                && (#[trigger] state.order_model(key)).is_some() ==> work.rows@.contains_key(key),
        decreases order_changes.keys.len() - index,
    {
        let key = order_changes.keys[index];
        if state.order_at(key).is_some() { bump_reference(state, &mut work, key, 0)?; }
        proof {
            assert(order_changes.keys@.take(index as int + 1) =~= order_changes.keys@.take(index as int).push(key));
            verus_prooflib::lemma_push_membership(order_changes.keys@.take(index as int), key);
        }
        index += 1;
    }
    proof {
        assert(order_changes.keys@.take(index as int) =~= order_changes.keys@);
        assert forall|key: i64| order_changes.rows@.contains_key(key) && (#[trigger] state.order_model(key)).is_some() implies work.rows@.contains_key(key) by {
            assert(order_changes.keys@.contains(key));
        }
    }
    let ghost dirty = work;
    index = 0;
    while index < work.keys.len()
        invariant index <= work.keys.len(), work.valid(), state.valid(), work.keys@ == dirty.keys@,
            post_dates_ready(*post, post_keys@, db2), order_changes_ready(*order_changes, order_deltas@),
            forall|key: i64| #[trigger] work.rows@.contains_key(key) <==> dirty.rows@.contains_key(key),
            forall|key: i64| #[trigger] overlay_references(*state, work, key) == references(db2, key as int),
            forall|key: i64| order_changes.rows@.contains_key(key) && (#[trigger] state.order_model(key)).is_some() ==> work.rows@.contains_key(key),
            forall|key: i64| work.keys@.take(index as int).contains(key) && #[trigger] work.rows@.contains_key(key) ==> {
                let order = work.rows@[key];
                0 <= order.refs <= u64::MAX
                    && years_positive(order.years@)
                    && order.years@.map_values(|row: Year| row.year).no_duplicates()
                    && (order.refs <= 0 || {
                    if order_changes.rows@.contains_key(key) || state.order_model(key).is_none() {
                        forall|year: int| #[trigger] year_at(order.years@, year) == dates(db2, key as int, year)
                    } else {
                        forall|year: int| #[trigger] year_at(order.years@, year) == dates(state.db(), key as int, year)
                    }
                })
            },
        decreases work.keys.len() - index,
    {
        let key = work.keys[index];
        let ghost before = work;
        proof { assert forall|target: i64| #[trigger] overlay_references(*state, before, target) == references(db2, target as int) by {} }
        let mut order = work.take(key)?;
        let ghost reference_count = order.refs as int;
        proof {
            assert(before.rows@.contains_key(key));
            assert(order == before.rows@[key]);
            assert(reference_count == overlay_references(*state, before, key));
            assert(reference_count == references(db2, key as int));
        }
        if order.refs < 0 || order.refs > u64::MAX as i128 { return None; }
        if order.refs > 0 {
            if order_changes.rows.contains_key(&key) || state.order_at(key).is_none() {
                if !has_key(post_keys, key) { return None; }
                order.years = copy_years(post.rows.get(&key)?);
                let ghost rebuilt = order.years@;
                proof {
                    assert forall|year: int| #[trigger] year_at(rebuilt, year) == dates(db2, key as int, year) by {
                        assert(year_at(rebuilt, year) == post_year(*post, key, year));
                    }
                }
                prune_years(&mut order.years)?;
            } else {
                order.years = copy_cached_dates(state, key)?;
            }
        } else { order.years.clear(); }
        work.put(key, order);
        proof {
            assert forall|target: i64| #[trigger] overlay_references(*state, work, target) == references(db2, target as int) by {
                assert(overlay_references(*state, work, target) == overlay_references(*state, before, target));
            }
            assert(0 <= work.rows@[key].refs <= u64::MAX);
        }
        index += 1;
    }
    proof {
        assert(work.keys@.take(index as int) =~= work.keys@);
        assert forall|key: i64| #[trigger] work.rows@.contains_key(key) implies {
            let order = work.rows@[key];
            0 <= order.refs <= u64::MAX && (order.refs > 0 ==> order_value_valid(order, db2, key as int))
        } by {
            assert(work.keys@.contains(key));
            let order = work.rows@[key];
            assert(order.refs as int == overlay_references(*state, work, key));
            assert(order.refs as int == references(db2, key as int));
            if order.refs > 0 {
                if order_changes.rows@.contains_key(key) || state.order_model(key).is_none() {
                    assert forall|year: int| #[trigger] year_at(order.years@, year) == dates(db2, key as int, year) by {}
                } else {
                    assert(!order_deltas@.map_values(order_key_of()).contains(key)) by {
                        assert(order_changes.rows@.contains_key(key)
                            <==> order_deltas@.map_values(order_key_of()).contains(key));
                    }
                    assert forall|year: int| #[trigger] year_at(order.years@, year) == dates(db2, key as int, year) by {
                        assert(year_at(order.years@, year) == dates(state.db(), key as int, year));
                        lemma_unchanged_dates(state.db(), db2, order_deltas@, key, year);
                    }
                }
            }
        }
        state.lemma_base_facts();
        assert forall|key: i64| !work.rows@.contains_key(key) && (#[trigger] state.order_model(key)).is_some()
            implies order_value_valid(state.order_model(key).unwrap(), db2, key as int) by {
            assert(!order_changes.rows@.contains_key(key));
            let order = state.order_model(key).unwrap();
            assert(order_value_valid(order, state.db(), key as int));
            assert(order.refs as int == cached_references(*state, key));
            assert(cached_references(*state, key) == overlay_references(*state, work, key));
            assert(order.refs as int == references(db2, key as int));
            assert forall|year: int| #[trigger] year_at(order.years@, year) == dates(db2, key as int, year) by {
                lemma_unchanged_dates(state.db(), db2, order_deltas@, key, year);
            }
        }
    }
    Some(work)
}

#[verifier::spinoff_prover]
pub fn publish_orders(state: &State, work: &mut Work<Order>, Ghost(db): Ghost<Db>) -> (result: Option<Pages<Order>>)
    requires state.valid(), order_overlay_ready(*state, *old(work), db),
    ensures final(work).valid(), result matches Some(pages) ==> order_pages_ready(pages, db),
{
    broadcast use vstd::seq_lib::group_seq_properties;
    proof { state.lemma_base_facts(); }
    let ghost initial = *work;
    let mut pages = state.share_orders();
    let mut index = 0;
    while index < work.keys.len()
        invariant index <= work.keys.len(), work.valid(), pages.valid(), state.valid(),
            work.keys@ == initial.keys@, initial.valid(), order_overlay_ready(*state, initial, db),
            forall|key: i64| #[trigger] work.rows@.contains_key(key) <==> initial.rows@.contains_key(key) && !initial.keys@.take(index as int).contains(key),
            forall|key: i64| #[trigger] work.rows@.contains_key(key) ==> work.rows@[key] == initial.rows@[key],
            forall|key: i64| #[trigger] pages.at(key) == if initial.keys@.take(index as int).contains(key) && initial.rows@.contains_key(key) {
                let order = initial.rows@[key];
                if order.refs == 0 { None } else { Some(order) }
            } else { state.order_model(key) },
        decreases work.keys.len() - index,
    {
        let key = work.keys[index];
        proof { assert(!initial.keys@.take(index as int).contains(key)); }
        let order = work.take(key)?;
        pages.put(key, if order.refs == 0 { None } else { Some(order) });
        proof {
            assert(initial.keys@.take(index as int + 1) =~= initial.keys@.take(index as int).push(key));
            verus_prooflib::lemma_push_membership(initial.keys@.take(index as int), key);
            assert forall|target: i64| #[trigger] pages.at(target) == if initial.keys@.take(index as int + 1).contains(target) && initial.rows@.contains_key(target) {
                let order = initial.rows@[target];
                if order.refs == 0 { None } else { Some(order) }
            } else { state.order_model(target) } by {}
            assert forall|target: i64| #[trigger] work.rows@.contains_key(target) <==> initial.rows@.contains_key(target) && !initial.keys@.take(index as int + 1).contains(target) by {}
        }
        index += 1;
    }
    proof {
        state.lemma_base_facts();
        assert(initial.keys@.take(index as int) =~= initial.keys@);
        assert forall|key: i64| #[trigger] pages.at(key).is_some() <==> references(db, key as int) > 0 by {
            assert(overlay_references(*state, initial, key) == references(db, key as int));
            if initial.rows@.contains_key(key) {
                assert(initial.keys@.contains(key));
                assert(pages.at(key).is_some() == (initial.rows@[key].refs > 0));
            } else {
                assert(pages.at(key) == state.order_model(key));
                if state.order_model(key).is_some() {
                    assert(order_value_valid(state.order_model(key).unwrap(), db, key as int));
                } else { assert(cached_references(*state, key) == 0); }
            }
        }
        assert forall|key: i64| #[trigger] pages.at(key).is_some() implies order_value_valid(pages.at(key).unwrap(), db, key as int) by {
            if initial.rows@.contains_key(key) { assert(initial.keys@.contains(key)); }
        }
    }
    Some(pages)
}

}
