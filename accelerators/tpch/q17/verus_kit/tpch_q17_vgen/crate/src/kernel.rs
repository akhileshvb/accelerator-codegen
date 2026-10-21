use vstd::prelude::*;
use std::collections::HashMap;
use std::sync::Arc;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::aggregation::*;
use crate::maintenance::*;
use crate::model::*;
use crate::storage::*;
use crate::query_proofs::*;
use crate::semantics::*;
use crate::ordering::*;
use crate::maintenance_semantics::*;
use crate::bounds::*;
use verus_prooflib::wide::Wide;
use verus_common::sums::*;

verus! {

broadcast use vstd::seq_lib::group_seq_properties;

pub struct State {
    db: Ghost<Db>,
    lines: Store<Moment>,
    parts: Store<Vec<Variant>>,
    groups: Store<Vec<Group>>,
}

impl State {
    pub closed spec fn db(self) -> Db { self.db@ }

    #[verifier::type_invariant]
    spec fn valid(self) -> bool {
        state_meaning(self.db@, self.lines, self.parts, self.groups)
    }
}

#[verifier::rlimit(1)]
pub fn build(lines: &[LineInput], parts: Vec<PartInput>, Ghost(db): Ghost<Db>) -> (result: Option<State>)
    requires scan_lines_ok(db, lines@), scan_parts_ok(db, parts@),
    ensures result matches Some(state) ==> state.db() == db,
        envelope(db) ==> result is Some,
{
    if !rows_in_range(lines) || parts.len() > 1099511627776 {
        proof { if envelope(db) { lemma_scan_bounded(db, lines@); lemma_scan_parts_len(db, parts@); } }
        return None;
    }
    let (summaries, keys) = summarize(lines);
    let mut line_store: Store<Moment> = Store::new(keys.len());
    let mut part_store = Store::new(parts.len());
    let mut groups = Store::new(parts.len());
    let mut index = 0;
    let mut processed = Ghost(0int);
    proof {
        assert(keys@.take(0) =~= Seq::<i64>::empty());
        assert forall|key: i64| #[trigger] line_store.lookup(key).is_none() by {}
        assert forall|key: i64| match #[trigger] line_store.lookup(key) { Some(value) => value.count > 0, None => true } by {
            assert(line_store.lookup(key).is_none());
        }
        assert forall|key: i64| #[trigger] moment_at(line_store, key) == Moment { count: 0, price: 0 } by {
            assert(line_store.lookup(key).is_none());
        }
    }
    while index < keys.len()
        invariant index <= keys.len(), processed@ == index as int,
            line_store.wf(), part_store.wf(), groups.wf(), keys@.no_duplicates(),
            forall|key: i64| #[trigger] part_store.lookup(key).is_none(),
            forall|key: i64| #[trigger] groups.lookup(key).is_none(),
            forall|key: i64| match #[trigger] line_store.lookup(key) { Some(value) => value.count > 0, None => true },
            forall|key: i64| #![trigger summaries@[key]] summaries@.contains_key(key) ==> summaries@[key].count > 0,
            forall|key: i64| #[trigger] moment_at(line_store, key) == if keys@.take(processed@).contains(key) {
                accumulated(summaries@, key)
            } else { Moment { count: 0, price: 0 } },
        decreases keys.len() - index,
    {
        let key = keys[index];
        let before = Ghost(line_store);
        proof { verus_prooflib::lemma_prefix_fresh(keys@, index as int); }
        if let Some(moment) = summaries.get(&key) { line_store.put(key, Some(Arc::new(*moment))); }
        proof {
            assert forall|other: i64| #[trigger] moment_at(line_store, other) == if keys@.take(index as int + 1).contains(other) {
                accumulated(summaries@, other)
            } else { Moment { count: 0, price: 0 } } by {
                assert(moment_at(before@, other) == if keys@.take(index as int).contains(other) {
                    accumulated(summaries@, other)
                } else { Moment { count: 0, price: 0 } });
                assert(keys@.take(index as int + 1) =~= keys@.take(index as int).push(key));
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    proof {
        assert(keys@.take(keys@.len() as int) =~= keys@);
        keys@.to_set_ensures();
        assert forall|key: i64| #[trigger] moment_at(line_store, key) == accumulated(summaries@, key) by {
            if !keys@.contains(key) { assert(!summaries@.contains_key(key)); }
        }
        assert(line_model(line_store, line_input_seq_view(lines@)));
        lemma_empty_part_store(part_store); lemma_empty_group_store(groups);
    }
    let changes = build_changes(&parts, &groups);
    let old_parts = Ghost(part_store);
    let old_groups = Ghost(groups);
    apply_changes(&mut part_store, &line_store, &mut groups, &changes);
    proof {
        lemma_changes_fields(changes);
        lemma_input_parts_fit(parts@);
        let input = Db { lineitem: line_input_seq_view(lines@), part: part_input_seq_view(parts@) };
        assert forall|position: int| #![trigger input.part[position]] 0 <= position < input.part.len() implies changes.keys@.contains(input.part[position].p_partkey as i64) by {
            assert(input.part[position].p_partkey as i64 == parts@[position].p_partkey);
            assert(changes.keys@.contains(parts@[position].p_partkey));
        }
        assert forall|key: i64, brand: Seq<u8>, container: Seq<u8>| #[trigger] multiplicity(part_store, key, brand, container)
            == part_count(input.part, key as int, brand, container) by {
            assert(multiplicity(old_parts@, key, brand, container) == 0);
            assert(change_at(changes, key, brand, container) == part_count(input.part, key as int, brand, container));
        }
        assert forall|brand: Seq<u8>, container: Seq<u8>| #[trigger] group_count_at(groups, brand, container)
            == sum(changes.keys@, |key: i64| part_count(input.part, key as int, brand, container) * moment_at(line_store, key).count as int) by {
            assert(group_count_at(old_groups@, brand, container) == 0);
            assert forall|key: i64| #[trigger] change_at(changes, key, brand, container) == part_count(input.part, key as int, brand, container) by {}
            lemma_sum_ext_all(changes.keys@, |key: i64| change_at(changes, key, brand, container) * moment_at(line_store, key).count as int,
                |key: i64| part_count(input.part, key as int, brand, container) * moment_at(line_store, key).count as int);
        }
        assert forall|brand: Seq<u8>, container: Seq<u8>| #[trigger] group_price_at(groups, brand, container)
            == sum(changes.keys@, |key: i64| part_count(input.part, key as int, brand, container) * moment_at(line_store, key).price as int) by {
            assert(group_price_at(old_groups@, brand, container) == 0);
            assert forall|key: i64| #[trigger] change_at(changes, key, brand, container) == part_count(input.part, key as int, brand, container) by {}
            lemma_sum_ext_all(changes.keys@, |key: i64| change_at(changes, key, brand, container) * moment_at(line_store, key).price as int,
                |key: i64| part_count(input.part, key as int, brand, container) * moment_at(line_store, key).price as int);
        }
        assert forall|brand: Seq<u8>, container: Seq<u8>| #[trigger] group_refs_at(groups, brand, container)
            == sum(changes.keys@, |key: i64| part_count(input.part, key as int, brand, container)) by {
            assert(group_refs_at(old_groups@, brand, container) == 0);
            lemma_sum_ext_all(changes.keys@, |key: i64| change_at(changes, key, brand, container),
                |key: i64| part_count(input.part, key as int, brand, container));
        }
        lemma_build_group_model(input, line_store, groups, changes.keys@);
        lemma_build_state(db, lines@, parts@, line_store, part_store, groups);
    }
    Some(State { db: Ghost(db), lines: line_store, parts: part_store, groups })
}

pub fn execute(state: &State, p1: &[u8], p2: &[u8]) -> (result: Option<Vec<Option<(i128, i128)>>>)
    ensures result matches Some(out) ==> query_result(state.db(), params(p1@, p2@), out_view(out@)),
        envelope(state.db()) && answer_fits(state.db(), params(p1@, p2@)) ==> result is Some,
{
    proof { use_type_invariant(state); }
    let moment = read_group(&state.groups, p1, p2);
    let answer = if moment.count.is_pos() {
        let numerator = moment.price.mul(&Wide::from_i128(10));
        match numerator.to_i128() {
            Some(value) => Some((value, 7000)),
            None => {
                proof { lemma_answer_overflow(state.db(), p1@, p2@, moment.count@, moment.price@); }
                return None;
            }
        }
    } else { None };
    let mut output = Vec::new();
    output.push(answer);
    proof {
        lemma_execute_result(state.db(), p1@, p2@, answer, moment.count@, moment.price@);
        assert(output@ =~= seq![answer]);
    }
    Some(output)
}

pub open spec fn rescan_lines_for_parts_wanted(state: State, lineitem_delta: Seq<LineKeyDelta>, part_delta: Seq<PartDelta>) -> Seq<i64> {
    first_keys(lineitem_delta, lineitem_delta.len() as int)
}

pub fn rescan_lines_for_parts_keys(state: &State, lineitem_delta: &[LineKeyDelta], part_delta: &Vec<PartDelta>) -> (result: Vec<i64>)
    ensures result@ == rescan_lines_for_parts_wanted(*state, lineitem_delta@, part_delta@),
{
    touched_keys(lineitem_delta)
}

#[verifier::rlimit(1)]
pub fn apply(state: &State, lineitem_delta: &[LineKeyDelta], part_delta: &Vec<PartDelta>,
    lines_for_parts_keys: &[i64], lines_for_parts_rows: &[LineInput],
    Ghost(lineitem_full): Ghost<Seq<(LineitemRow, int)>>, Ghost(db2): Ghost<Db>) -> (result: Result<State, u32>)
    requires lineitem_delta_ok(lineitem_delta@, lineitem_full),
        capture_ok(state.db(), db2, lineitem_full, part_delta_seq_view(part_delta@)),
        lines_for_parts_keys@ == rescan_lines_for_parts_wanted(*state, lineitem_delta@, part_delta@),
        rescan_lines_for_parts_ok(db2, lines_for_parts_keys@, lines_for_parts_rows@),
    ensures result matches Ok(updated) ==> updated.db() == db2,
        envelope(state.db()) && envelope(db2) ==> result is Ok,
{
    proof { use_type_invariant(state); }
    let touched = touched_keys(lineitem_delta);
    proof { assert(state.lines.wf()); }
    let touched = crate::ordering::by_slot(&touched, state.lines.span);
    proof {
        assert forall|index: int| #![trigger lineitem_delta@[index]] 0 <= index < lineitem_delta@.len() implies touched@.contains(lineitem_delta@[index].l_partkey) by {}
    }
    proof {
        assert(touched@.no_duplicates());
        assert forall|position: int| #![trigger touched@[position]] 0 <= position < touched.len() implies lines_for_parts_keys@.contains(touched@[position]) by {}
    }
    if !rows_in_range(lines_for_parts_rows) {
        proof { if envelope(db2) { lemma_rescan_bounded(db2, lines_for_parts_keys@, lines_for_parts_rows@); } }
        return Err(2);
    }
    let (summaries, _summary_keys) = summarize(lines_for_parts_rows);
    let changes = delta_changes(part_delta, &state.groups);
    let mut lines = state.lines.fork();
    let mut parts = state.parts.fork();
    let mut groups = state.groups.fork();
    proof {
        lemma_line_fork(state.lines, lines, state.db().lineitem);
        lemma_part_fork(state.parts, parts);
        lemma_group_fork(state.groups, groups);
    }
    let mut index = 0;
    let mut processed = Ghost(0int);
    while index < touched.len()
        invariant index <= touched.len(), processed@ == index as int,
            lines.wf(), part_store_wf(parts), group_store_wf(groups),
            forall|key: i64| match #[trigger] lines.lookup(key) { Some(value) => value.count > 0, None => true },
            touched@.no_duplicates(),
            forall|key: i64, brand: Seq<u8>, container: Seq<u8>| #[trigger] multiplicity(parts, key, brand, container) == multiplicity(state.parts, key, brand, container),
            forall|key: i64| #![trigger summaries@[key]] summaries@.contains_key(key) ==> summaries@[key].count > 0,
            forall|brand: Seq<u8>, container: Seq<u8>| #[trigger] group_refs_at(groups, brand, container) == group_refs_at(state.groups, brand, container),
            forall|key: i64| #[trigger] moment_at(lines, key) == if touched@.take(processed@).contains(key) {
                accumulated(summaries@, key)
            } else { moment_at(state.lines, key) },
            forall|brand: Seq<u8>, container: Seq<u8>| #[trigger] group_count_at(groups, brand, container)
                == group_count_at(state.groups, brand, container) + verus_common::sums::sum(touched@.take(processed@), |key: i64|
                    multiplicity(state.parts, key, brand, container) * (accumulated(summaries@, key).count as int - moment_at(state.lines, key).count as int)),
            forall|brand: Seq<u8>, container: Seq<u8>| #[trigger] group_price_at(groups, brand, container)
                == group_price_at(state.groups, brand, container) + verus_common::sums::sum(touched@.take(processed@), |key: i64|
                    multiplicity(state.parts, key, brand, container) * (accumulated(summaries@, key).price as int - moment_at(state.lines, key).price as int)),
        decreases touched.len() - index,
    {
        let key = touched[index];
        let old_lines = Ghost(lines);
        let old_groups = Ghost(groups);
        proof { verus_prooflib::lemma_prefix_fresh(touched@, index as int); }
        let moment = match summaries.get(&key) { Some(value) => *value, None => zero() };
        replace_lines(&mut lines, &parts, &mut groups, key, moment);
        proof {
            assert forall|other: i64| match #[trigger] lines.lookup(other) { Some(value) => value.count > 0, None => true } by {
                assert(lines.lookup(other) == if other == key { if moment.count == 0 { None } else { Some(moment) } } else { old_lines@.lookup(other) });
            }
            assert(moment_at(old_lines@, key) == moment_at(state.lines, key));
            assert forall|brand: Seq<u8>, container: Seq<u8>| #[trigger] group_count_at(groups, brand, container)
                == group_count_at(state.groups, brand, container) + sum(touched@.take(index as int + 1), |key: i64|
                    multiplicity(state.parts, key, brand, container) * (accumulated(summaries@, key).count as int - moment_at(state.lines, key).count as int)) by {
                assert(group_count_at(old_groups@, brand, container) == group_count_at(state.groups, brand, container)
                    + sum(touched@.take(index as int), |key: i64| multiplicity(state.parts, key, brand, container) * (accumulated(summaries@, key).count as int - moment_at(state.lines, key).count as int)));
                lemma_sum_take(touched@, index as int, |key: i64| multiplicity(state.parts, key, brand, container) * (accumulated(summaries@, key).count as int - moment_at(state.lines, key).count as int));
                assert(multiplicity(parts, key, brand, container) == multiplicity(state.parts, key, brand, container));
            }
            assert forall|brand: Seq<u8>, container: Seq<u8>| #[trigger] group_price_at(groups, brand, container)
                == group_price_at(state.groups, brand, container) + sum(touched@.take(index as int + 1), |key: i64|
                    multiplicity(state.parts, key, brand, container) * (accumulated(summaries@, key).price as int - moment_at(state.lines, key).price as int)) by {
                assert(group_price_at(old_groups@, brand, container) == group_price_at(state.groups, brand, container)
                    + sum(touched@.take(index as int), |key: i64| multiplicity(state.parts, key, brand, container) * (accumulated(summaries@, key).price as int - moment_at(state.lines, key).price as int)));
                lemma_sum_take(touched@, index as int, |key: i64| multiplicity(state.parts, key, brand, container) * (accumulated(summaries@, key).price as int - moment_at(state.lines, key).price as int));
                assert(multiplicity(parts, key, brand, container) == multiplicity(state.parts, key, brand, container));
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    proof {
        assert forall|key: i64| #[trigger] moment_at(lines, key).count as int == line_count(db2.lineitem, key as int)
            && moment_at(lines, key).price as int == line_price(db2.lineitem, key as int) by {
            if touched@.contains(key) {
                let position = choose|position: int| 0 <= position < touched.len() && #[trigger] touched@[position] == key;
                assert(lines_for_parts_keys@.contains(key));
                lemma_rescan_lines(db2, lines_for_parts_keys@, lines_for_parts_rows@, key);
            } else {
                assert forall|position: int| #![trigger lineitem_full[position]] 0 <= position < lineitem_full.len()
                    implies lineitem_full[position].0.l_partkey != key as int by {
                    assert(lineitem_delta@[position].l_partkey as int == lineitem_full[position].0.l_partkey);
                    assert(touched@.contains(lineitem_delta@[position].l_partkey));
                }
                lemma_untouched_lines(state.db(), db2, lineitem_full, key);
            }
        }
        assert forall|key: i64| #[trigger] moment_at(lines, key).price as int == line_price(db2.lineitem, key as int) by {
            assert(moment_at(lines, key).count as int == line_count(db2.lineitem, key as int));
        }
        assert(line_model(lines, db2.lineitem));
    }
    let line_stage_groups = Ghost(groups);
    apply_changes(&mut parts, &lines, &mut groups, &changes);
    proof {
        lemma_changes_fields(changes);
        let delta = part_delta_seq_view(part_delta@);
        assert forall|position: int| #![trigger delta[position]] 0 <= position < delta.len()
            implies i64::MIN <= delta[position].0.p_partkey <= i64::MAX && changes.keys@.contains(delta[position].0.p_partkey as i64) by {
            assert(changes.keys@.contains(part_delta@[position].row.p_partkey));
        }
        lemma_part_capture_fit(state.db(), db2, delta);
        reveal(signed_part_count);
        assert forall|key: i64, brand: Seq<u8>, container: Seq<u8>| #[trigger] multiplicity(parts, key, brand, container)
            == part_count(db2.part, key as int, brand, container) by {
            lemma_part_step(state.db(), db2, delta, key, brand, container);
            assert(change_at(changes, key, brand, container) == signed_part_count(delta, key, brand, container));
        }
        assert forall|brand: Seq<u8>, container: Seq<u8>| #[trigger] group_refs_at(groups, brand, container)
            == references(db2.part, brand, container) by {
            lemma_reference_step(state.db(), db2, delta, changes.keys@, brand, container);
            lemma_sum_ext_all(changes.keys@, |key: i64| change_at(changes, key, brand, container), |key: i64| signed_part_count(delta, key, brand, container));
            assert(group_refs_at(line_stage_groups@, brand, container) == group_refs_at(state.groups, brand, container));
        }
        assert forall|brand: Seq<u8>, container: Seq<u8>| #[trigger] group_count_at(groups, brand, container)
            == joined_count(db2, brand, container) by {
            lemma_join_step(state.db(), db2, delta, touched@, changes.keys@, state.lines, lines, brand, container, false);
            lemma_sum_ext(touched@,
                |key: i64| multiplicity(state.parts, key, brand, container) * (accumulated(summaries@, key).count as int - moment_at(state.lines, key).count as int),
                |key: i64| part_count(state.db().part, key as int, brand, container) * (moment_value(lines, key, false) - moment_value(state.lines, key, false)));
            lemma_sum_ext_all(changes.keys@,
                |key: i64| change_at(changes, key, brand, container) * moment_at(lines, key).count as int,
                |key: i64| signed_part_count(delta, key, brand, container) * moment_value(lines, key, false));
            assert(group_count_at(line_stage_groups@, brand, container) == group_count_at(state.groups, brand, container)
                + sum(touched@, |key: i64| multiplicity(state.parts, key, brand, container) * (accumulated(summaries@, key).count as int - moment_at(state.lines, key).count as int)));
        }
        assert forall|brand: Seq<u8>, container: Seq<u8>| #[trigger] group_price_at(groups, brand, container)
            == joined_price(db2, brand, container) by {
            lemma_join_step(state.db(), db2, delta, touched@, changes.keys@, state.lines, lines, brand, container, true);
            lemma_sum_ext(touched@,
                |key: i64| multiplicity(state.parts, key, brand, container) * (accumulated(summaries@, key).price as int - moment_at(state.lines, key).price as int),
                |key: i64| part_count(state.db().part, key as int, brand, container) * (moment_value(lines, key, true) - moment_value(state.lines, key, true)));
            lemma_sum_ext_all(changes.keys@,
                |key: i64| change_at(changes, key, brand, container) * moment_at(lines, key).price as int,
                |key: i64| signed_part_count(delta, key, brand, container) * moment_value(lines, key, true));
            assert(group_price_at(line_stage_groups@, brand, container) == group_price_at(state.groups, brand, container)
                + sum(touched@, |key: i64| multiplicity(state.parts, key, brand, container) * (accumulated(summaries@, key).price as int - moment_at(state.lines, key).price as int)));
        }
        lemma_apply_state(db2, lines, parts, groups);
    }
    Ok(State { db: Ghost(db2), lines, parts, groups })
}

}
