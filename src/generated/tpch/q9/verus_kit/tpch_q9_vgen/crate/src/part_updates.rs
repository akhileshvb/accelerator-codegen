use vstd::prelude::*;
use verus_common::sums::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::kernel::State;
use crate::storage::*;
use crate::values::*;
use crate::meaning::*;
use crate::maintenance::*;
use crate::order_updates::order_changes_ready;
use crate::construction::*;
use crate::aggregate::add_part;

verus! {

pub open spec fn part_core(state: State, work: Work<Part>, db: Db) -> bool {
    work.valid() && parts_unique(work)
        && (forall|key: i64| #[trigger] overlay_green(state, work, key) == green(db, key as int))
        && forall|key: i64| #[trigger] work.rows@.contains_key(key) ==> 0 <= work.rows@[key].green <= u64::MAX
}

pub open spec fn initial_pairs(state: State, db: Db, key: i64, supplier: int, field: int) -> int {
    if eligibility_changed(state.db(), db, key as int) { 0int } else { cached_pair(state, key, supplier, field) }
}

pub open spec fn initial_moments(state: State, db: Db, key: i64, supplier: int, year: int, field: int) -> int {
    if eligibility_changed(state.db(), db, key as int) { 0int } else { cached_moment(state, key, supplier, year, field) }
}

pub open spec fn part_setup(state: State, work: Work<Part>, db: Db) -> bool {
    part_core(state, work, db)
        && (forall|key: i64, supplier: int, field: int| 0 <= field <= 1 ==> #[trigger] overlay_pair(state, work, key, supplier, field) == initial_pairs(state, db, key, supplier, field))
        && forall|key: i64, supplier: int, year: int, field: int| 0 <= field <= 2 ==> #[trigger] overlay_moment(state, work, key, supplier, year, field) == initial_moments(state, db, key, supplier, year, field)
}

pub open spec fn active_pair(db: Db, key: i64, supplier: int, field: int) -> int {
    if green(db, key as int) > 0 { if field == 0 { pair_count(db, key as int, supplier) } else { pair_cost(db, key as int, supplier) } } else { 0int }
}

pub open spec fn pairs_updated(state: State, work: Work<Part>, db: Db) -> bool {
    pairs_meaning(state, work, db)
        && forall|key: i64, supplier: int, year: int, field: int| 0 <= field <= 2 ==> #[trigger] overlay_moment(state, work, key, supplier, year, field) == initial_moments(state, db, key, supplier, year, field)
}

pub open spec fn pairs_meaning(state: State, work: Work<Part>, db: Db) -> bool {
    part_core(state, work, db)
        && forall|key: i64, supplier: int, field: int| 0 <= field <= 1 ==> #[trigger] overlay_pair(state, work, key, supplier, field) == active_pair(db, key, supplier, field)
}

pub open spec fn pair_input_weight(key: i64, supplier: int, field: int) -> spec_fn(PartsuppIn) -> int {
    |row: PartsuppIn| if row.ps_partkey == key && row.ps_suppkey as int == supplier { if field == 0 { 1int } else { row.ps_supplycost as int } } else { 0int }
}

pub open spec fn pair_change_weight(key: i64, supplier: int, field: int) -> spec_fn(PartsuppDelta) -> int {
    |change: PartsuppDelta| change.sign as int * pair_input_weight(key, supplier, field)(change.row)
}

pub proof fn lemma_pair_rescan(db: Db, keys: Seq<i64>, rows: Seq<PartsuppIn>, key: i64, supplier: int, field: int)
    requires rescan_partsupp_for_parts_ok(db, keys, rows), keys.contains(key),
    ensures sum(rows, pair_input_weight(key, supplier, field)) == if field == 0 { pair_count(db, key as int, supplier) } else { pair_cost(db, key as int, supplier) },
{
    let position = choose|position: int| 0 <= position < keys.len() && (#[trigger] keys[position]) == key;
    assert(seq_view_i64(keys)[position] == key as int);
    assert(seq_view_i64(keys).contains(key as int));
    let weight = |row: PartsuppRow| if row.ps_partkey == key as int && row.ps_suppkey == supplier { if field == 0 { 1int } else { row.ps_supplycost } } else { 0int };
    let filtered = |row: PartsuppRow| if seq_view_i64(keys).contains(row.ps_partkey) { weight(row) } else { 0int };
    assert(sum(partsupp_in_seq_view(rows), weight) == sum(db.partsupp, filtered));
    lemma_sum_map(rows, |row: PartsuppIn| partsupp_in_view(row), weight, pair_input_weight(key, supplier, field));
    lemma_sum_ext_all(db.partsupp, filtered, weight);
    if field == 0 {
        lemma_sum_ext_all(db.partsupp, weight, |row: PartsuppRow| if row.ps_partkey == key as int && row.ps_suppkey == supplier { 1int } else { 0int });
    } else {
        lemma_sum_ext_all(db.partsupp, weight, |row: PartsuppRow| if row.ps_partkey == key as int && row.ps_suppkey == supplier { row.ps_supplycost } else { 0int });
    }
}

pub proof fn lemma_pair_capture(db: Db, db2: Db, changes: Seq<PartsuppDelta>, key: i64, supplier: int, field: int)
    requires bag_step(db.partsupp, db2.partsupp, partsupp_delta_seq_view(changes)),
    ensures (if field == 0 { pair_count(db2, key as int, supplier) } else { pair_cost(db2, key as int, supplier) })
        == (if field == 0 { pair_count(db, key as int, supplier) } else { pair_cost(db, key as int, supplier) }) + sum(changes, pair_change_weight(key, supplier, field)),
{
    let weight = |row: PartsuppRow| if row.ps_partkey == key as int && row.ps_suppkey == supplier { if field == 0 { 1int } else { row.ps_supplycost } } else { 0int };
    let signed = |entry: (PartsuppRow, int)| entry.1 * weight(entry.0);
    assert(sum(db2.partsupp, weight) == sum(db.partsupp, weight) + sum(partsupp_delta_seq_view(changes), signed));
    lemma_sum_map(changes, |change: PartsuppDelta| (partsupp_in_view(change.row), change.sign as int), signed, pair_change_weight(key, supplier, field));
    if field == 0 {
        lemma_sum_ext_all(db.partsupp, weight, |row: PartsuppRow| if row.ps_partkey == key as int && row.ps_suppkey == supplier { 1int } else { 0int });
        lemma_sum_ext_all(db2.partsupp, weight, |row: PartsuppRow| if row.ps_partkey == key as int && row.ps_suppkey == supplier { 1int } else { 0int });
    } else {
        lemma_sum_ext_all(db.partsupp, weight, |row: PartsuppRow| if row.ps_partkey == key as int && row.ps_suppkey == supplier { row.ps_supplycost } else { 0int });
        lemma_sum_ext_all(db2.partsupp, weight, |row: PartsuppRow| if row.ps_partkey == key as int && row.ps_suppkey == supplier { row.ps_supplycost } else { 0int });
    }
}

pub open spec fn newly_active(state: State, db: Db, key: i64) -> bool {
    eligibility_changed(state.db(), db, key as int) && green(db, key as int) > 0
}

pub open spec fn stays_active(state: State, db: Db, key: i64) -> bool {
    !eligibility_changed(state.db(), db, key as int) && green(state.db(), key as int) > 0
}

#[verifier::spinoff_prover]
pub fn update_pairs(state: &State, work: &mut Work<Part>, flips: &Work<bool>, keys: &[i64], rows: &[PartsuppIn], changes: &[PartsuppDelta], Ghost(line_keys): Ghost<Seq<i64>>, Ghost(db): Ghost<Db>) -> (result: Option<()>)
    requires state.valid(), part_setup(*state, *old(work), db), flips_ready(*state, *flips, db, line_keys, keys@),
        rescan_partsupp_for_parts_ok(db, keys@, rows@), bag_step(state.db().partsupp, db.partsupp, partsupp_delta_seq_view(changes@)),
    ensures final(work).valid(), result.is_some() ==> pairs_updated(*state, *final(work), db),
{
    proof { state.lemma_base_facts(); }
    let mut index = 0;
    let mut processed = Ghost(0int);
    proof {
        assert forall|key: i64, supplier: int, field: int| 0 <= field <= 1 implies #[trigger] overlay_pair(*state, *work, key, supplier, field)
            == initial_pairs(*state, db, key, supplier, field) + if newly_active(*state, db, key) { sum(rows@.take(0), pair_input_weight(key, supplier, field)) } else { 0int } by {
            lemma_sum_empty(rows@.take(0), pair_input_weight(key, supplier, field));
        }
    }
    while index < rows.len()
        invariant index <= rows.len(), processed@ == index as int, part_core(*state, *work, db), state.valid(),
            flips_ready(*state, *flips, db, line_keys, keys@),
            forall|key: i64| #[trigger] cached_green(*state, key) == green(state.db(), key as int),
            forall|key: i64, supplier: int, field: int| 0 <= field <= 1 ==> #[trigger] overlay_pair(*state, *work, key, supplier, field)
                == initial_pairs(*state, db, key, supplier, field) + if newly_active(*state, db, key) { sum(rows@.take(processed@), pair_input_weight(key, supplier, field)) } else { 0int },
            forall|key: i64, supplier: int, year: int, field: int| 0 <= field <= 2 ==> #[trigger] overlay_moment(*state, *work, key, supplier, year, field) == initial_moments(*state, db, key, supplier, year, field),
        decreases rows.len() - index,
    {
        let row = rows[index];
        let ghost before = *work;
        proof {
            assert forall|key: i64| #[trigger] overlay_green(*state, before, key) == green(db, key as int) by {}
            assert forall|key: i64, supplier: int, field: int| 0 <= field <= 1 implies #[trigger] overlay_pair(*state, before, key, supplier, field)
                == initial_pairs(*state, db, key, supplier, field) + if newly_active(*state, db, key) { sum(rows@.take(processed@), pair_input_weight(key, supplier, field)) } else { 0int } by {}
            assert forall|key: i64, supplier: int, year: int, field: int| 0 <= field <= 2 implies #[trigger] overlay_moment(*state, before, key, supplier, year, field) == initial_moments(*state, db, key, supplier, year, field) by {}
        }
        if flips.rows.contains_key(&row.ps_partkey) && new_activity(state, work, row.ps_partkey) {
            let mut part = take_part(state, work, row.ps_partkey);
            let ghost old_part = part;
            bump_pair(&mut part, row.ps_suppkey, 1, row.ps_supplycost as i128)?;
            let ghost completed = part;
            proof {
                assert forall|supplier: int, year: int, field: int| 0 <= field <= 2 implies #[trigger] stored_moment(completed, supplier, year, field) == overlay_moment(*state, before, row.ps_partkey, supplier, year, field) by {
                    assert(completed.moments@ == old_part.moments@);
                    assert(stored_moment(completed, supplier, year, field) == stored_moment(old_part, supplier, year, field));
                }
                assert forall|supplier: int, field: int| 0 <= field <= 1 implies #[trigger] stored_pair(completed, supplier, field) == overlay_pair(*state, before, row.ps_partkey, supplier, field)
                    + if supplier == row.ps_suppkey as int { if field == 0 { 1int } else { row.ps_supplycost as int } } else { 0int } by {}
            }
            work.put(row.ps_partkey, part);
        }
        proof {
            assert forall|key: i64| #[trigger] overlay_green(*state, *work, key) == green(db, key as int) by {
                assert(overlay_green(*state, *work, key) == overlay_green(*state, before, key));
            }
            assert forall|key: i64| #[trigger] work.rows@.contains_key(key) implies 0 <= work.rows@[key].green <= u64::MAX by {
                if key != row.ps_partkey { assert(work.rows@[key] == before.rows@[key]); }
            }
            assert forall|key: i64, supplier: int, field: int| 0 <= field <= 1 implies #[trigger] overlay_pair(*state, *work, key, supplier, field)
                == initial_pairs(*state, db, key, supplier, field) + if newly_active(*state, db, key) { sum(rows@.take(index as int + 1), pair_input_weight(key, supplier, field)) } else { 0int } by {
                lemma_sum_take(rows@, index as int, pair_input_weight(key, supplier, field));
                if key != row.ps_partkey { assert(overlay_pair(*state, *work, key, supplier, field) == overlay_pair(*state, before, key, supplier, field)); }
            }
            assert forall|key: i64, supplier: int, year: int, field: int| 0 <= field <= 2 implies #[trigger] overlay_moment(*state, *work, key, supplier, year, field) == initial_moments(*state, db, key, supplier, year, field) by {
                if key != row.ps_partkey { assert(overlay_moment(*state, *work, key, supplier, year, field) == overlay_moment(*state, before, key, supplier, year, field)); }
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    proof {
        assert(rows@.take(processed@) =~= rows@);
        assert forall|key: i64, supplier: int, field: int| 0 <= field <= 1 implies #[trigger] overlay_pair(*state, *work, key, supplier, field)
            == initial_pairs(*state, db, key, supplier, field)
                + (if newly_active(*state, db, key) { sum(rows@, pair_input_weight(key, supplier, field)) } else { 0int })
                + (if stays_active(*state, db, key) { sum(changes@.take(0), pair_change_weight(key, supplier, field)) } else { 0int }) by {
            lemma_sum_empty(changes@.take(0), pair_change_weight(key, supplier, field));
        }
    }
    index = 0;
    processed = Ghost(0int);
    while index < changes.len()
        invariant index <= changes.len(), processed@ == index as int, part_core(*state, *work, db), state.valid(),
            flips_ready(*state, *flips, db, line_keys, keys@),
            forall|key: i64| #[trigger] cached_green(*state, key) == green(state.db(), key as int),
            forall|key: i64, supplier: int, field: int| 0 <= field <= 1 ==> #[trigger] overlay_pair(*state, *work, key, supplier, field)
                == initial_pairs(*state, db, key, supplier, field)
                    + (if newly_active(*state, db, key) { sum(rows@, pair_input_weight(key, supplier, field)) } else { 0int })
                    + (if stays_active(*state, db, key) { sum(changes@.take(processed@), pair_change_weight(key, supplier, field)) } else { 0int }),
            forall|key: i64, supplier: int, year: int, field: int| 0 <= field <= 2 ==> #[trigger] overlay_moment(*state, *work, key, supplier, year, field) == initial_moments(*state, db, key, supplier, year, field),
        decreases changes.len() - index,
    {
        let change = changes[index];
        let key = change.row.ps_partkey;
        let ghost before = *work;
        proof {
            assert forall|target: i64| #[trigger] overlay_green(*state, before, target) == green(db, target as int) by {}
            assert forall|target: i64, supplier: int, field: int| 0 <= field <= 1 implies #[trigger] overlay_pair(*state, before, target, supplier, field)
                == initial_pairs(*state, db, target, supplier, field)
                    + (if newly_active(*state, db, target) { sum(rows@, pair_input_weight(target, supplier, field)) } else { 0int })
                    + (if stays_active(*state, db, target) { sum(changes@.take(processed@), pair_change_weight(target, supplier, field)) } else { 0int }) by {}
            assert forall|target: i64, supplier: int, year: int, field: int| 0 <= field <= 2 implies #[trigger] overlay_moment(*state, before, target, supplier, year, field) == initial_moments(*state, db, target, supplier, year, field) by {}
        }
        let changed = activity(state, key) && !flips.rows.contains_key(&key);
        proof { assert(changed == stays_active(*state, db, key)); }
        if changed {
            let mut part = take_part(state, work, key);
            let ghost old_part = part;
            let cost = (change.row.ps_supplycost as i128).checked_mul(change.sign as i128)?;
            proof {
                assert(cost as int == change.row.ps_supplycost as int * change.sign as int);
                assert(cost as int == change.sign as int * change.row.ps_supplycost as int) by (nonlinear_arith)
                    requires cost as int == change.row.ps_supplycost as int * change.sign as int;
                assert forall|supplier: int, field: int| 0 <= field <= 1 implies #[trigger] stored_pair(old_part, supplier, field) == overlay_pair(*state, before, key, supplier, field) by {}
            }
            bump_pair(&mut part, change.row.ps_suppkey, change.sign as i128, cost)?;
            let ghost completed = part;
            proof {
                assert forall|supplier: int, year: int, field: int| 0 <= field <= 2 implies #[trigger] stored_moment(completed, supplier, year, field) == overlay_moment(*state, before, key, supplier, year, field) by {
                    assert(completed.moments@ == old_part.moments@);
                    assert(stored_moment(completed, supplier, year, field) == stored_moment(old_part, supplier, year, field));
                }
                assert forall|supplier: int, field: int| 0 <= field <= 1 implies #[trigger] stored_pair(completed, supplier, field) == overlay_pair(*state, before, key, supplier, field)
                    + pair_change_weight(key, supplier, field)(change) by {
                    assert(stored_pair(completed, supplier, field) == stored_pair(old_part, supplier, field)
                        + if supplier == change.row.ps_suppkey as int { if field == 0 { change.sign as int } else { cost as int } } else { 0int });
                }
            }
            work.put(key, part);
        }
        proof {
            assert forall|target: i64| #[trigger] overlay_green(*state, *work, target) == green(db, target as int) by {
                assert(overlay_green(*state, *work, target) == overlay_green(*state, before, target));
            }
            assert forall|target: i64| #[trigger] work.rows@.contains_key(target) implies 0 <= work.rows@[target].green <= u64::MAX by {
                if target != key { assert(work.rows@[target] == before.rows@[target]); }
            }
            assert forall|target: i64, supplier: int, field: int| 0 <= field <= 1 implies #[trigger] overlay_pair(*state, *work, target, supplier, field)
                == initial_pairs(*state, db, target, supplier, field)
                    + (if newly_active(*state, db, target) { sum(rows@, pair_input_weight(target, supplier, field)) } else { 0int })
                    + (if stays_active(*state, db, target) { sum(changes@.take(index as int + 1), pair_change_weight(target, supplier, field)) } else { 0int }) by {
                lemma_sum_take(changes@, index as int, pair_change_weight(target, supplier, field));
                if target != key { assert(overlay_pair(*state, *work, target, supplier, field) == overlay_pair(*state, before, target, supplier, field)); }
                assert(overlay_pair(*state, *work, target, supplier, field) == overlay_pair(*state, before, target, supplier, field)
                    + if stays_active(*state, db, target) { pair_change_weight(target, supplier, field)(change) } else { 0int });
            }
            assert forall|target: i64, supplier: int, year: int, field: int| 0 <= field <= 2 implies #[trigger] overlay_moment(*state, *work, target, supplier, year, field) == initial_moments(*state, db, target, supplier, year, field) by {
                if target != key { assert(overlay_moment(*state, *work, target, supplier, year, field) == overlay_moment(*state, before, target, supplier, year, field)); }
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    proof {
        state.lemma_base_facts();
        assert(changes@.take(processed@) =~= changes@);
        assert forall|key: i64, supplier: int, field: int| 0 <= field <= 1 implies #[trigger] overlay_pair(*state, *work, key, supplier, field) == active_pair(db, key, supplier, field) by {
            if newly_active(*state, db, key) {
                assert(flips.rows@.contains_key(key));
                lemma_pair_rescan(db, keys@, rows@, key, supplier, field);
            } else if stays_active(*state, db, key) {
                assert(state.part_model(key).is_some());
                assert(cached_pair(*state, key, supplier, field) == if field == 0 { pair_count(state.db(), key as int, supplier) } else { pair_cost(state.db(), key as int, supplier) });
                lemma_pair_capture(state.db(), db, changes@, key, supplier, field);
            } else if !eligibility_changed(state.db(), db, key as int) {
                assert(state.part_model(key).is_none());
            }
        }
    }
    Some(())
}

#[verifier::spinoff_prover]
pub fn add_part_line(state: &State, work: &mut Work<Part>, line: LineIn, years: &Vec<Year>, sign: i128, Ghost(db): Ghost<Db>) -> (result: Option<()>)
    requires state.valid(), part_core(*state, *old(work), db),
    ensures final(work).valid(), result.is_some() ==> part_core(*state, *final(work), db),
        result.is_some() ==> forall|key: i64, supplier: int, field: int| 0 <= field <= 1 ==>
            #[trigger] overlay_pair(*state, *final(work), key, supplier, field) == overlay_pair(*state, *old(work), key, supplier, field),
        result.is_some() ==> forall|key: i64, supplier: int, target_year: int, field: int| 0 <= field <= 2 ==>
            #[trigger] overlay_moment(*state, *final(work), key, supplier, target_year, field) == overlay_moment(*state, *old(work), key, supplier, target_year, field)
                + if key == line.l_partkey && supplier == line.l_suppkey as int { year_at(years@, target_year) * line_weight(line_in_view(line), field) * sign as int } else { 0int },
{
    let ghost initial = *work;
    let mut part = take_part(state, work, line.l_partkey);
    let ghost before = part;
    proof {
        assert forall|supplier: int, field: int| 0 <= field <= 1 implies #[trigger] stored_pair(before, supplier, field) == overlay_pair(*state, initial, line.l_partkey, supplier, field) by {}
        assert forall|supplier: int, target_year: int, field: int| 0 <= field <= 2 implies #[trigger] stored_moment(before, supplier, target_year, field) == overlay_moment(*state, initial, line.l_partkey, supplier, target_year, field) by {}
    }
    add_line(&mut part, line, years, sign)?;
    let ghost completed = part;
    proof {
        assert forall|supplier: int, field: int| 0 <= field <= 1 implies #[trigger] stored_pair(completed, supplier, field) == overlay_pair(*state, initial, line.l_partkey, supplier, field) by {
            assert(completed.pairs@ == before.pairs@);
            assert(stored_pair(completed, supplier, field) == stored_pair(before, supplier, field));
        }
        assert forall|supplier: int, target_year: int, field: int| 0 <= field <= 2 implies #[trigger] stored_moment(completed, supplier, target_year, field) == overlay_moment(*state, initial, line.l_partkey, supplier, target_year, field)
            + if supplier == line.l_suppkey as int { year_at(years@, target_year) * line_weight(line_in_view(line), field) * sign as int } else { 0int } by {}
    }
    work.put(line.l_partkey, part);
    proof {
        assert forall|key: i64| #[trigger] overlay_green(*state, *work, key) == green(db, key as int) by {
            assert(overlay_green(*state, *work, key) == overlay_green(*state, initial, key));
        }
        assert forall|key: i64| #[trigger] work.rows@.contains_key(key) implies 0 <= work.rows@[key].green <= u64::MAX by {
            if key != line.l_partkey { assert(work.rows@[key] == initial.rows@[key]); }
        }
        assert forall|key: i64, supplier: int, field: int| 0 <= field <= 1 implies #[trigger] overlay_pair(*state, *work, key, supplier, field) == overlay_pair(*state, initial, key, supplier, field) by {
            if key != line.l_partkey { assert(work.rows@.contains_key(key) == initial.rows@.contains_key(key)); }
        }
        assert forall|key: i64, supplier: int, target_year: int, field: int| 0 <= field <= 2 implies #[trigger] overlay_moment(*state, *work, key, supplier, target_year, field) == overlay_moment(*state, initial, key, supplier, target_year, field)
            + if key == line.l_partkey && supplier == line.l_suppkey as int { year_at(years@, target_year) * line_weight(line_in_view(line), field) * sign as int } else { 0int } by {
            if key != line.l_partkey { assert(work.rows@.contains_key(key) == initial.rows@.contains_key(key)); }
        }
    }
    Some(())
}

pub open spec fn moment_input_weight(db: Db, key: i64, supplier: int, year: int, field: int) -> spec_fn(LineIn) -> int {
    |row: LineIn| if row.l_partkey == key && row.l_suppkey as int == supplier { dates(db, row.l_orderkey as int, year) * line_weight(line_in_view(row), field) } else { 0int }
}

pub open spec fn moment_change_weight(db: Db, key: i64, supplier: int, year: int, field: int) -> spec_fn(LineDelta) -> int {
    |change: LineDelta| moment_input_weight(db, key, supplier, year, field)(change.row) * change.sign as int
}

pub open spec fn moment_order_weight(db: Db, db2: Db, key: i64, supplier: int, year: int, field: int) -> spec_fn(LineIn) -> int {
    |row: LineIn| if row.l_partkey == key && row.l_suppkey as int == supplier {
        (dates(db2, row.l_orderkey as int, year) - dates(db, row.l_orderkey as int, year)) * line_weight(line_in_view(row), field)
    } else { 0int }
}

pub proof fn lemma_moment_rescan(db: Db, keys: Seq<i64>, rows: Seq<LineIn>, key: i64, supplier: int, year: int, field: int)
    requires rescan_lines_for_parts_ok(db, keys, rows), keys.contains(key),
    ensures sum(rows, moment_input_weight(db, key, supplier, year, field)) == moments(db, key as int, supplier, year, field),
{
    let position = choose|position: int| 0 <= position < keys.len() && (#[trigger] keys[position]) == key;
    assert(seq_view_i64(keys)[position] == key as int);
    assert(seq_view_i64(keys).contains(key as int));
    let weight = |row: LineitemRow| if row.l_partkey == key as int && row.l_suppkey == supplier { dates(db, row.l_orderkey, year) * line_weight(row, field) } else { 0int };
    let filtered = |row: LineitemRow| if seq_view_i64(keys).contains(row.l_partkey) { weight(row) } else { 0int };
    assert(sum(line_in_seq_view(rows), weight) == sum(db.lineitem, filtered));
    lemma_sum_map(rows, |row: LineIn| line_in_view(row), weight, moment_input_weight(db, key, supplier, year, field));
    lemma_sum_ext_all(db.lineitem, filtered, weight);
}

pub proof fn lemma_dates_frame(db: Db, db2: Db, changes: Seq<OrderDelta>, keys: Seq<i64>, key: int, year: int)
    requires bag_step(db.orders, db2.orders, order_delta_seq_view(changes)),
        forall|position: int| 0 <= position < changes.len() ==> keys.contains((#[trigger] changes[position]).row.o_orderkey),
        !seq_view_i64(keys).contains(key),
    ensures dates(db2, key, year) == dates(db, key, year),
{
    let weight = |row: OrdersRow| if row.o_orderkey == key && verus_common::sqlfns::civil_year(row.o_orderdate) == year { 1int } else { 0int };
    let signed = |entry: (OrdersRow, int)| entry.1 * weight(entry.0);
    let projected = |change: OrderDelta| signed((order_in_view(change.row), change.sign as int));
    assert forall|position: int| 0 <= position < changes.len() implies projected(#[trigger] changes[position]) == 0 by {
        let order = changes[position].row.o_orderkey;
        let offset = choose|offset: int| 0 <= offset < keys.len() && (#[trigger] keys[offset]) == order;
        assert(seq_view_i64(keys)[offset] == order as int);
        assert(seq_view_i64(keys).contains(order as int));
    }
    lemma_sum_zero(changes, projected);
    lemma_sum_map(changes, |change: OrderDelta| (order_in_view(change.row), change.sign as int), signed, projected);
    assert(sum(db2.orders, weight) == sum(db.orders, weight) + sum(order_delta_seq_view(changes), signed));
}

#[verifier::spinoff_prover]
pub proof fn lemma_moment_step(db: Db, db2: Db, line_changes: Seq<LineDelta>, order_changes: Seq<OrderDelta>, keys: Seq<i64>, rows: Seq<LineIn>, key: i64, supplier: int, year: int, field: int)
    requires bag_step(db.lineitem, db2.lineitem, line_delta_seq_view(line_changes)),
        bag_step(db.orders, db2.orders, order_delta_seq_view(order_changes)), rescan_lines_for_orders_ok(db2, keys, rows),
        forall|position: int| 0 <= position < order_changes.len() ==> keys.contains((#[trigger] order_changes[position]).row.o_orderkey),
    ensures moments(db2, key as int, supplier, year, field) == moments(db, key as int, supplier, year, field)
        + sum(line_changes, moment_change_weight(db, key, supplier, year, field)) + sum(rows, moment_order_weight(db, db2, key, supplier, year, field)),
{
    hide(dates);
    let old_weight = |row: LineitemRow| if row.l_partkey == key as int && row.l_suppkey == supplier { dates(db, row.l_orderkey, year) * line_weight(row, field) } else { 0int };
    let new_weight = |row: LineitemRow| if row.l_partkey == key as int && row.l_suppkey == supplier { dates(db2, row.l_orderkey, year) * line_weight(row, field) } else { 0int };
    let difference = |row: LineitemRow| if row.l_partkey == key as int && row.l_suppkey == supplier { (dates(db2, row.l_orderkey, year) - dates(db, row.l_orderkey, year)) * line_weight(row, field) } else { 0int };
    assert forall|row: LineitemRow| #[trigger] difference(row) == new_weight(row) - old_weight(row) by {
        let old = dates(db, row.l_orderkey, year);
        let new = dates(db2, row.l_orderkey, year);
        let amount = line_weight(row, field);
        assert((new - old) * amount == new * amount - old * amount) by (nonlinear_arith);
    }
    lemma_sum_sub(db2.lineitem, new_weight, old_weight, difference);
    let filtered = |row: LineitemRow| if seq_view_i64(keys).contains(row.l_orderkey) { difference(row) } else { 0int };
    assert forall|row: LineitemRow| #[trigger] filtered(row) == difference(row) by {
        if !seq_view_i64(keys).contains(row.l_orderkey) {
            lemma_dates_frame(db, db2, order_changes, keys, row.l_orderkey, year);
        }
    }
    assert(sum(line_in_seq_view(rows), difference) == sum(db2.lineitem, filtered));
    lemma_sum_ext_all(db2.lineitem, filtered, difference);
    lemma_sum_map(rows, |row: LineIn| line_in_view(row), difference, moment_order_weight(db, db2, key, supplier, year, field));
    let signed = |entry: (LineitemRow, int)| entry.1 * old_weight(entry.0);
    assert(sum(db2.lineitem, old_weight) == sum(db.lineitem, old_weight) + sum(line_delta_seq_view(line_changes), signed));
    let projected = |change: LineDelta| signed((line_in_view(change.row), change.sign as int));
    lemma_sum_map(line_changes, |change: LineDelta| (line_in_view(change.row), change.sign as int), signed, projected);
    assert forall|change: LineDelta| #[trigger] projected(change) == moment_change_weight(db, key, supplier, year, field)(change) by {
        let amount = old_weight(line_in_view(change.row));
        let sign = change.sign as int;
        assert(moment_input_weight(db, key, supplier, year, field)(change.row) == amount);
        assert(sign * amount == amount * sign) by (nonlinear_arith);
    }
    lemma_sum_ext_all(line_changes, projected, moment_change_weight(db, key, supplier, year, field));
}

pub open spec fn moment_progress(state: State, work: Work<Part>, db: Db, part_rows: Seq<LineIn>, changes: Seq<LineDelta>, order_rows: Seq<LineIn>) -> bool {
    forall|key: i64, supplier: int, year: int, field: int| 0 <= field <= 2 ==>
        #[trigger] overlay_moment(state, work, key, supplier, year, field) == initial_moments(state, db, key, supplier, year, field)
            + (if newly_active(state, db, key) { sum(part_rows, moment_input_weight(db, key, supplier, year, field)) } else { 0int })
            + (if stays_active(state, db, key) {
                sum(changes, moment_change_weight(state.db(), key, supplier, year, field))
                    + sum(order_rows, moment_order_weight(state.db(), db, key, supplier, year, field))
            } else { 0int })
}

pub open spec fn parts_updated(state: State, work: Work<Part>, db: Db) -> bool {
    pairs_meaning(state, work, db)
        && forall|key: i64, supplier: int, year: int, field: int| 0 <= field <= 2 ==>
            #[trigger] overlay_moment(state, work, key, supplier, year, field) == if green(db, key as int) > 0 { moments(db, key as int, supplier, year, field) } else { 0int }
}

#[verifier::spinoff_prover]
pub fn update_moments(state: &State, work: &mut Work<Part>, flips: &Work<bool>,
    part_keys: &[i64], part_rows: &[LineIn], pair_keys: &[i64],
    changes: &[LineDelta], order_deltas: &[OrderDelta], order_changes: &Work<Vec<Year>>,
    order_keys: &[i64], order_rows: &[LineIn], post_keys: &[i64], post: &Work<Vec<Year>>, Ghost(db): Ghost<Db>,
) -> (result: Option<()>)
    requires state.valid(), pairs_updated(*state, *old(work), db),
        flips_ready(*state, *flips, db, part_keys@, pair_keys@),
        order_changes_ready(*order_changes, order_deltas@),
        forall|key: i64| #[trigger] order_changes.rows@.contains_key(key) ==> order_keys@.contains(key),
        post_dates_ready(*post, post_keys@, db),
        rescan_lines_for_parts_ok(db, part_keys@, part_rows@), rescan_lines_for_orders_ok(db, order_keys@, order_rows@),
        bag_step(state.db().lineitem, db.lineitem, line_delta_seq_view(changes@)),
        bag_step(state.db().orders, db.orders, order_delta_seq_view(order_deltas@)),
    ensures final(work).valid(), result.is_some() ==> parts_updated(*state, *final(work), db),
{
    hide(dates);
    hide(moments);
    proof {
        state.lemma_base_facts();
        assert forall|key: i64, supplier: int, year: int, field: int| 0 <= field <= 2 implies #[trigger] overlay_moment(*state, *work, key, supplier, year, field)
            == initial_moments(*state, db, key, supplier, year, field)
                + (if newly_active(*state, db, key) { sum(part_rows@.take(0), moment_input_weight(db, key, supplier, year, field)) } else { 0int })
                + (if stays_active(*state, db, key) {
                    sum(changes@.take(0), moment_change_weight(state.db(), key, supplier, year, field))
                        + sum(order_rows@.take(0), moment_order_weight(state.db(), db, key, supplier, year, field))
                } else { 0int }) by {
            lemma_sum_empty(part_rows@.take(0), moment_input_weight(db, key, supplier, year, field));
            lemma_sum_empty(changes@.take(0), moment_change_weight(state.db(), key, supplier, year, field));
            lemma_sum_empty(order_rows@.take(0), moment_order_weight(state.db(), db, key, supplier, year, field));
        }
    }
    let mut index = 0;
    let mut processed = Ghost(0int);
    while index < part_rows.len()
        invariant index <= part_rows.len(), processed@ == index as int, state.valid(), pairs_meaning(*state, *work, db),
            flips_ready(*state, *flips, db, part_keys@, pair_keys@), post_dates_ready(*post, post_keys@, db),
            moment_progress(*state, *work, db, part_rows@.take(processed@), changes@.take(0), order_rows@.take(0)),
        decreases part_rows.len() - index,
    {
        let line = part_rows[index];
        let ghost before = *work;
        proof { assert(moment_progress(*state, before, db, part_rows@.take(processed@), changes@.take(0), order_rows@.take(0))); }
        let changed = flips.rows.contains_key(&line.l_partkey) && new_activity(state, work, line.l_partkey);
        proof { assert(changed == newly_active(*state, db, line.l_partkey)); }
        if changed {
            if !has_key(post_keys, line.l_orderkey) { return None; }
            let years = post.rows.get(&line.l_orderkey)?;
            proof {
                assert forall|year: int| #[trigger] year_at(years@, year) == dates(db, line.l_orderkey as int, year) by {
                    assert(year_at(years@, year) == post_year(*post, line.l_orderkey, year));
                }
            }
            add_part_line(state, work, line, years, 1, Ghost(db))?;
        }
        proof {
            assert forall|key: i64, supplier: int, year: int, field: int| 0 <= field <= 2 implies #[trigger] overlay_moment(*state, *work, key, supplier, year, field)
                == initial_moments(*state, db, key, supplier, year, field)
                    + (if newly_active(*state, db, key) { sum(part_rows@.take(index as int + 1), moment_input_weight(db, key, supplier, year, field)) } else { 0int })
                    + (if stays_active(*state, db, key) {
                        sum(changes@.take(0), moment_change_weight(state.db(), key, supplier, year, field))
                            + sum(order_rows@.take(0), moment_order_weight(state.db(), db, key, supplier, year, field))
                    } else { 0int }) by {
                lemma_sum_take(part_rows@, index as int, moment_input_weight(db, key, supplier, year, field));
                assert(overlay_moment(*state, *work, key, supplier, year, field) == overlay_moment(*state, before, key, supplier, year, field)
                    + if newly_active(*state, db, key) { moment_input_weight(db, key, supplier, year, field)(line) } else { 0int });
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    proof { assert(part_rows@.take(processed@) =~= part_rows@); }
    index = 0;
    processed = Ghost(0int);
    while index < changes.len()
        invariant index <= changes.len(), processed@ == index as int, state.valid(), pairs_meaning(*state, *work, db),
            flips_ready(*state, *flips, db, part_keys@, pair_keys@), post_dates_ready(*post, post_keys@, db),
            bag_step(state.db().orders, db.orders, order_delta_seq_view(order_deltas@)),
            forall|key: i64| #[trigger] cached_green(*state, key) == green(state.db(), key as int),
            moment_progress(*state, *work, db, part_rows@, changes@.take(processed@), order_rows@.take(0)),
        decreases changes.len() - index,
    {
        let change = changes[index];
        let key = change.row.l_partkey;
        let ghost before = *work;
        proof { assert(moment_progress(*state, before, db, part_rows@, changes@.take(processed@), order_rows@.take(0))); }
        let changed = activity(state, key) && !flips.rows.contains_key(&key);
        proof { assert(changed == stays_active(*state, db, key)); }
        if changed {
            if state.order_at(change.row.l_orderkey).is_none() && !has_key(post_keys, change.row.l_orderkey) { return None; }
            let years = before_dates(state, post, order_deltas, change.row.l_orderkey, Ghost(db))?;
            add_part_line(state, work, change.row, &years, change.sign as i128, Ghost(db))?;
        }
        proof {
            assert forall|target: i64, supplier: int, year: int, field: int| 0 <= field <= 2 implies #[trigger] overlay_moment(*state, *work, target, supplier, year, field)
                == initial_moments(*state, db, target, supplier, year, field)
                    + (if newly_active(*state, db, target) { sum(part_rows@, moment_input_weight(db, target, supplier, year, field)) } else { 0int })
                    + (if stays_active(*state, db, target) {
                        sum(changes@.take(index as int + 1), moment_change_weight(state.db(), target, supplier, year, field))
                            + sum(order_rows@.take(0), moment_order_weight(state.db(), db, target, supplier, year, field))
                    } else { 0int }) by {
                lemma_sum_take(changes@, index as int, moment_change_weight(state.db(), target, supplier, year, field));
                assert(overlay_moment(*state, *work, target, supplier, year, field) == overlay_moment(*state, before, target, supplier, year, field)
                    + if stays_active(*state, db, target) { moment_change_weight(state.db(), target, supplier, year, field)(change) } else { 0int });
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    proof {
        assert(changes@.take(processed@) =~= changes@);
        state.lemma_base_facts();
        assert forall|key: i64, year: int| #[trigger] post_year(*order_changes, key, year) == dates(db, key as int, year) - dates(state.db(), key as int, year) by {
            lemma_order_capture(state.db(), db, order_deltas@, key, year);
        }
    }
    index = 0;
    processed = Ghost(0int);
    while index < order_rows.len()
        invariant index <= order_rows.len(), processed@ == index as int, state.valid(), pairs_meaning(*state, *work, db),
            flips_ready(*state, *flips, db, part_keys@, pair_keys@),
            forall|key: i64| #[trigger] cached_green(*state, key) == green(state.db(), key as int),
            forall|key: i64, year: int| #[trigger] post_year(*order_changes, key, year) == dates(db, key as int, year) - dates(state.db(), key as int, year),
            moment_progress(*state, *work, db, part_rows@, changes@, order_rows@.take(processed@)),
        decreases order_rows.len() - index,
    {
        let line = order_rows[index];
        let ghost before = *work;
        proof { assert(moment_progress(*state, before, db, part_rows@, changes@, order_rows@.take(processed@))); }
        let changed = activity(state, line.l_partkey) && !flips.rows.contains_key(&line.l_partkey);
        proof { assert(changed == stays_active(*state, db, line.l_partkey)); }
        if changed {
            if let Some(years) = order_changes.rows.get(&line.l_orderkey) {
                proof {
                    assert forall|year: int| #[trigger] year_at(years@, year) == dates(db, line.l_orderkey as int, year) - dates(state.db(), line.l_orderkey as int, year) by {
                        assert(year_at(years@, year) == post_year(*order_changes, line.l_orderkey, year));
                    }
                }
                add_part_line(state, work, line, years, 1, Ghost(db))?;
            }
        }
        proof {
            assert forall|key: i64, supplier: int, year: int, field: int| 0 <= field <= 2 implies #[trigger] overlay_moment(*state, *work, key, supplier, year, field)
                == initial_moments(*state, db, key, supplier, year, field)
                    + (if newly_active(*state, db, key) { sum(part_rows@, moment_input_weight(db, key, supplier, year, field)) } else { 0int })
                    + (if stays_active(*state, db, key) {
                        sum(changes@, moment_change_weight(state.db(), key, supplier, year, field))
                            + sum(order_rows@.take(index as int + 1), moment_order_weight(state.db(), db, key, supplier, year, field))
                    } else { 0int }) by {
                lemma_sum_take(order_rows@, index as int, moment_order_weight(state.db(), db, key, supplier, year, field));
                if !order_changes.rows@.contains_key(line.l_orderkey) { assert(post_year(*order_changes, line.l_orderkey, year) == 0); }
                assert(overlay_moment(*state, *work, key, supplier, year, field) == overlay_moment(*state, before, key, supplier, year, field)
                    + if stays_active(*state, db, key) { moment_order_weight(state.db(), db, key, supplier, year, field)(line) } else { 0int });
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    proof {
        assert(order_rows@.take(processed@) =~= order_rows@);
        state.lemma_base_facts();
        assert forall|position: int| 0 <= position < order_deltas.len() implies order_keys@.contains((#[trigger] order_deltas[position]).row.o_orderkey) by {
            let key = order_deltas[position].row.o_orderkey;
            assert(order_deltas@.map_values(crate::order_updates::order_key_of())[position] == key);
            assert(order_deltas@.map_values(crate::order_updates::order_key_of()).contains(key));
            assert(order_changes.rows@.contains_key(key));
        }
        assert forall|key: i64, supplier: int, year: int, field: int| 0 <= field <= 2 implies #[trigger] overlay_moment(*state, *work, key, supplier, year, field)
            == if green(db, key as int) > 0 { moments(db, key as int, supplier, year, field) } else { 0int } by {
            if newly_active(*state, db, key) {
                assert(flips.rows@.contains_key(key));
                lemma_moment_rescan(db, part_keys@, part_rows@, key, supplier, year, field);
            } else if stays_active(*state, db, key) {
                assert(state.part_model(key).is_some());
                assert(cached_moment(*state, key, supplier, year, field) == moments(state.db(), key as int, supplier, year, field));
                lemma_moment_step(state.db(), db, changes@, order_deltas@, order_keys@, order_rows@, key, supplier, year, field);
            } else if !eligibility_changed(state.db(), db, key as int) {
                assert(state.part_model(key).is_none());
            }
        }
    }
    Some(())
}

pub open spec fn parts_normalized(state: State, work: Work<Part>, db: Db) -> bool {
    parts_updated(state, work, db)
        && forall|key: i64| #[trigger] work.rows@.contains_key(key) && work.rows@[key].green > 0 ==> part_valid(work.rows@[key], db, key as int)
}

pub open spec fn part_difference_weight(db: Db, db2: Db, supplier: i64, year: int, field: int) -> spec_fn(i64) -> int {
    |key: i64| part_database(db2, key as int, supplier as int, year, field) - part_database(db, key as int, supplier as int, year, field)
}

pub proof fn lemma_part_amount(part: Part, db: Db, key: i64, supplier: i64, year: int, field: int)
    requires 0 <= field <= 1, part.green == green(db, key as int), 0 <= part.green,
        part.green > 0 ==> part_valid(part, db, key as int),
    ensures part_fold(part, part.moments@, supplier, year, field) == part_database(db, key as int, supplier as int, year, field),
{
    hide(moments); hide(pair_count); hide(pair_cost); hide(green);
    lemma_part_fold(part, part.moments@, supplier, year, field);
    assert(moment_at(part.moments@, supplier as int, year, 0) == stored_moment(part, supplier as int, year, 0));
    assert(moment_at(part.moments@, supplier as int, year, 1) == stored_moment(part, supplier as int, year, 1));
    assert(moment_at(part.moments@, supplier as int, year, 2) == stored_moment(part, supplier as int, year, 2));
}

#[verifier::spinoff_prover]
pub fn normalize_parts(state: &State, work: &mut Work<Part>, changes: &mut Work<Vec<Total>>, Ghost(db): Ghost<Db>) -> (result: Option<()>)
    requires state.valid(), parts_updated(*state, *old(work), db), old(changes).valid(), old(changes).rows@ == Map::<i64, Vec<Total>>::empty(),
    ensures final(work).valid(), final(changes).valid(), final(work).keys@ == old(work).keys@,
        result.is_some() ==> parts_normalized(*state, *final(work), db),
        result.is_some() ==> forall|supplier: i64, year: int, field: int| 0 <= field <= 1 ==>
            #[trigger] work_total(*final(changes), supplier, year, field) == sum(old(work).keys@, part_difference_weight(state.db(), db, supplier, year, field)),
{
    hide(moments); hide(pair_count); hide(pair_cost); hide(green);
    let ghost initial = *work;
    proof {
        state.lemma_base_facts();
        assert forall|supplier: i64, year: int, field: int| 0 <= field <= 1 implies #[trigger] work_total(*changes, supplier, year, field)
            == sum(work.keys@.take(0), part_difference_weight(state.db(), db, supplier, year, field)) by {
            lemma_sum_empty(work.keys@.take(0), part_difference_weight(state.db(), db, supplier, year, field));
        }
    }
    let mut index = 0;
    let mut processed = Ghost(0int);
    while index < work.keys.len()
        invariant index <= work.keys.len(), processed@ == index as int, state.valid(), parts_updated(*state, *work, db), changes.valid(),
            initial == *old(work), work.keys@ == initial.keys@,
            forall|key: i64| work.keys@.take(processed@).contains(key) && #[trigger] work.rows@.contains_key(key) && work.rows@[key].green > 0 ==> part_valid(work.rows@[key], db, key as int),
            forall|supplier: i64, year: int, field: int| 0 <= field <= 1 ==> #[trigger] work_total(*changes, supplier, year, field)
                == sum(work.keys@.take(processed@), part_difference_weight(state.db(), db, supplier, year, field)),
        decreases work.keys.len() - index,
    {
        let key = work.keys[index];
        let ghost before = *work;
        let ghost before_changes = *changes;
        proof {
            assert(parts_updated(*state, before, db));
            assert forall|supplier: i64, year: int, field: int| 0 <= field <= 1 implies #[trigger] work_total(before_changes, supplier, year, field)
                == sum(work.keys@.take(processed@), part_difference_weight(state.db(), db, supplier, year, field)) by {}
        }
        let mut part = work.take(key)?;
        let ghost old_part = part;
        proof {
            assert(before.rows@.contains_key(key));
            assert(old_part == before.rows@[key]);
            assert(old_part.green as int == overlay_green(*state, before, key));
            assert forall|supplier: int, field: int| 0 <= field <= 1 implies #[trigger] stored_pair(old_part, supplier, field) == active_pair(db, key, supplier, field) by {
                assert(stored_pair(old_part, supplier, field) == overlay_pair(*state, before, key, supplier, field));
            }
            assert forall|supplier: int, year: int, field: int| 0 <= field <= 2 implies #[trigger] stored_moment(old_part, supplier, year, field)
                == if green(db, key as int) > 0 { moments(db, key as int, supplier, year, field) } else { 0int } by {
                assert(stored_moment(old_part, supplier, year, field) == overlay_moment(*state, before, key, supplier, year, field));
            }
            assert(old_part.green == green(db, key as int));
        }
        prune_part(&mut part)?;
        let ghost completed = part;
        proof {
            assert(completed.green == green(db, key as int));
            assert forall|supplier: int, field: int| 0 <= field <= 1 implies #[trigger] stored_pair(completed, supplier, field) == active_pair(db, key, supplier, field) by {
                if completed.green == 0 {
                    lemma_sum_empty(completed.pairs@, |row: Pair| if row.supplier == supplier { if field == 0 { row.count as int } else { row.cost as int } } else { 0int });
                }
            }
            assert forall|supplier: int, year: int, field: int| 0 <= field <= 2 implies #[trigger] stored_moment(completed, supplier, year, field)
                == if green(db, key as int) > 0 { moments(db, key as int, supplier, year, field) } else { 0int } by {
                if completed.green == 0 {
                    lemma_sum_empty(completed.moments@, |row: Moment| if row.supplier == supplier && row.year == year { if field == 0 { row.cell.count as int } else if field == 1 { row.cell.revenue as int } else { row.cell.quantity as int } } else { 0int });
                }
            }
            assert(completed.green > 0 ==> part_valid(completed, db, key as int));
        }
        if let Some(prior) = state.part_at(key) { add_part(changes, &prior, -1)?; }
        proof {
            state.lemma_base_facts();
            assert forall|supplier: i64, year: int, field: int| 0 <= field <= 1 implies #[trigger] work_total(*changes, supplier, year, field)
                == work_total(before_changes, supplier, year, field) - part_database(state.db(), key as int, supplier as int, year, field) by {
                if state.part_model(key).is_some() {
                    let prior = state.part_model(key).unwrap();
                    lemma_part_amount(prior, state.db(), key, supplier, year, field);
                } else {
                    assert(cached_green(*state, key) == 0);
                    assert(part_database(state.db(), key as int, supplier as int, year, field) == 0);
                }
            }
        }
        let ghost removed = *changes;
        add_part(changes, &part, 1)?;
        proof {
            assert forall|supplier: i64, year: int, field: int| 0 <= field <= 1 implies #[trigger] work_total(*changes, supplier, year, field)
                == sum(initial.keys@.take(index as int + 1), part_difference_weight(state.db(), db, supplier, year, field)) by {
                lemma_part_amount(completed, db, key, supplier, year, field);
                lemma_sum_take(initial.keys@, index as int, part_difference_weight(state.db(), db, supplier, year, field));
                assert(work_total(removed, supplier, year, field) == work_total(before_changes, supplier, year, field) - part_database(state.db(), key as int, supplier as int, year, field));
            }
        }
        work.put(key, part);
        proof {
            assert forall|target: i64| #[trigger] overlay_green(*state, *work, target) == green(db, target as int) by {
                assert(overlay_green(*state, *work, target) == overlay_green(*state, before, target));
            }
            assert forall|target: i64, supplier: int, field: int| 0 <= field <= 1 implies #[trigger] overlay_pair(*state, *work, target, supplier, field) == active_pair(db, target, supplier, field) by {
                if target != key { assert(overlay_pair(*state, *work, target, supplier, field) == overlay_pair(*state, before, target, supplier, field)); }
            }
            assert forall|target: i64, supplier: int, year: int, field: int| 0 <= field <= 2 implies #[trigger] overlay_moment(*state, *work, target, supplier, year, field)
                == if green(db, target as int) > 0 { moments(db, target as int, supplier, year, field) } else { 0int } by {
                if target != key { assert(overlay_moment(*state, *work, target, supplier, year, field) == overlay_moment(*state, before, target, supplier, year, field)); }
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    proof {
        assert(initial.keys@.take(processed@) =~= initial.keys@);
        assert forall|key: i64| #[trigger] work.rows@.contains_key(key) && work.rows@[key].green > 0 implies part_valid(work.rows@[key], db, key as int) by {
            assert(work.keys@.contains(key));
        }
    }
    Some(())
}

#[verifier::spinoff_prover]
pub fn publish_parts(state: &State, work: &mut Work<Part>, Ghost(db): Ghost<Db>) -> (result: Option<Pages<Part>>)
    requires state.valid(), parts_normalized(*state, *old(work), db),
    ensures final(work).valid(), result matches Some(pages) ==> part_pages_ready(pages, db),
{
    hide(green); hide(moments); hide(pair_count); hide(pair_cost);
    broadcast use vstd::seq_lib::group_seq_properties;
    proof { state.lemma_base_facts(); }
    let ghost initial = *work;
    let mut pages = state.share_parts();
    let mut index = 0;
    while index < work.keys.len()
        invariant index <= work.keys.len(), work.valid(), pages.valid(), state.valid(),
            work.keys@ == initial.keys@, initial.valid(), parts_normalized(*state, initial, db),
            forall|key: i64| #[trigger] work.rows@.contains_key(key) <==> initial.rows@.contains_key(key) && !initial.keys@.take(index as int).contains(key),
            forall|key: i64| #[trigger] work.rows@.contains_key(key) ==> work.rows@[key] == initial.rows@[key],
            forall|key: i64| #[trigger] pages.at(key) == if initial.keys@.take(index as int).contains(key) && initial.rows@.contains_key(key) {
                let part = initial.rows@[key];
                if part.green == 0 { None } else { Some(part) }
            } else { state.part_model(key) },
        decreases work.keys.len() - index,
    {
        let key = work.keys[index];
        proof { assert(!initial.keys@.take(index as int).contains(key)); }
        let part = work.take(key)?;
        pages.put(key, if part.green == 0 { None } else { Some(part) });
        proof {
            assert(initial.keys@.take(index as int + 1) =~= initial.keys@.take(index as int).push(key));
            verus_prooflib::lemma_push_membership(initial.keys@.take(index as int), key);
            assert forall|target: i64| #[trigger] pages.at(target) == if initial.keys@.take(index as int + 1).contains(target) && initial.rows@.contains_key(target) {
                let part = initial.rows@[target];
                if part.green == 0 { None } else { Some(part) }
            } else { state.part_model(target) } by {}
            assert forall|target: i64| #[trigger] work.rows@.contains_key(target) <==> initial.rows@.contains_key(target) && !initial.keys@.take(index as int + 1).contains(target) by {}
        }
        index += 1;
    }
    proof {
        state.lemma_base_facts();
        assert(initial.keys@.take(index as int) =~= initial.keys@);
        assert forall|key: i64| #[trigger] pages.at(key).is_some() <==> green(db, key as int) > 0 by {
            assert(overlay_green(*state, initial, key) == green(db, key as int));
            if initial.rows@.contains_key(key) { assert(initial.keys@.contains(key)); }
            else if state.part_model(key).is_none() { assert(cached_green(*state, key) == 0); }
        }
        assert forall|key: i64| #[trigger] pages.at(key).is_some() implies part_valid(pages.at(key).unwrap(), db, key as int) by {
            if initial.rows@.contains_key(key) { assert(initial.keys@.contains(key)); }
            else {
                let part = state.part_model(key).unwrap();
                assert(part_valid(part, state.db(), key as int));
                assert(part.green == cached_green(*state, key));
                assert(part.green == overlay_green(*state, initial, key));
                assert forall|supplier: int, field: int| 0 <= field <= 1 implies #[trigger] stored_pair(part, supplier, field) == if field == 0 { pair_count(db, key as int, supplier) } else { pair_cost(db, key as int, supplier) } by {
                    assert(stored_pair(part, supplier, field) == overlay_pair(*state, initial, key, supplier, field));
                }
                assert forall|supplier: int, year: int, field: int| 0 <= field <= 2 implies #[trigger] stored_moment(part, supplier, year, field) == moments(db, key as int, supplier, year, field) by {
                    assert(stored_moment(part, supplier, year, field) == overlay_moment(*state, initial, key, supplier, year, field));
                }
            }
        }
    }
    Some(pages)
}

}
