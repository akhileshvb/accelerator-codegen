use vstd::prelude::*;
use std::sync::Arc;
use verus_common::sums::*;
use verus_prooflib::wide::Wide;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::cube::*;
use crate::runs::*;
use crate::proofs::*;

verus! {

/// The kernel keeps no state that grows with the changed rows.
pub open spec fn history(s: State) -> nat { 0 }

pub struct Overlay {
    pub cells: Arc<Vec<Cell>>,
    pub credit: usize,
}

pub open spec fn runs_valid(overlays: Seq<Overlay>) -> bool {
    forall|index: int| 0 <= index < overlays.len() ==>
        sorted((*(#[trigger] overlays[index]).cells)@) && nonzero((*overlays[index].cells)@)
}

pub open spec fn overlay_weight(overlays: Seq<Overlay>, weight: spec_fn(Key) -> int, revenue: bool) -> int {
    sum(overlays, |overlay: Overlay| weighted((*overlay.cells)@, weight, revenue))
}

pub proof fn lemma_overlay_push(overlays: Seq<Overlay>, overlay: Overlay)
    ensures forall|weight: spec_fn(Key) -> int, revenue: bool| #[trigger] overlay_weight(overlays.push(overlay), weight, revenue) ==
        overlay_weight(overlays, weight, revenue) + weighted((*overlay.cells)@, weight, revenue),
{
    assert forall|weight: spec_fn(Key) -> int, revenue: bool| #[trigger] overlay_weight(overlays.push(overlay), weight, revenue) ==
        overlay_weight(overlays, weight, revenue) + weighted((*overlay.cells)@, weight, revenue) by {
        lemma_sum_push(overlays, overlay, |overlay: Overlay| weighted((*overlay.cells)@, weight, revenue));
    }
}

pub proof fn lemma_overlay_take(overlays: Seq<Overlay>, index: int)
    requires 0 <= index < overlays.len(),
    ensures forall|weight: spec_fn(Key) -> int, revenue: bool| #[trigger] overlay_weight(overlays.take(index + 1), weight, revenue) ==
        overlay_weight(overlays.take(index), weight, revenue) + weighted((*overlays[index].cells)@, weight, revenue),
{
    assert forall|weight: spec_fn(Key) -> int, revenue: bool| #[trigger] overlay_weight(overlays.take(index + 1), weight, revenue) ==
        overlay_weight(overlays.take(index), weight, revenue) + weighted((*overlays[index].cells)@, weight, revenue) by {
        lemma_sum_take(overlays, index, |overlay: Overlay| weighted((*overlay.cells)@, weight, revenue));
    }
}

pub open spec fn prefix_weight(parameters: Params, processed: int) -> spec_fn(Key) -> int {
    |key: Key| if parameters.p2 - 1 <= key.0 < parameters.p2 - 1 + processed &&
        parameters.p1 <= key.1 < parameters.p4 && key.2 < parameters.p3 { 1int } else { 0int }
}

pub proof fn lemma_prefix_step(cells: Seq<Cell>, parameters: Params, processed: int, revenue: bool)
    requires 0 <= processed < 3,
    ensures weighted(cells, prefix_weight(parameters, processed + 1), revenue) ==
        weighted(cells, prefix_weight(parameters, processed), revenue) +
        weighted(cells, selection_weight(parameters.p2 - 1 + processed, parameters.p1, parameters.p3, parameters.p4), revenue),
{
    let previous = |cell: Cell| (prefix_weight(parameters, processed))(cell.key) * measure(cell, revenue);
    let exact = |cell: Cell| (selection_weight(parameters.p2 - 1 + processed, parameters.p1, parameters.p3, parameters.p4))(cell.key) * measure(cell, revenue);
    let next = |cell: Cell| (prefix_weight(parameters, processed + 1))(cell.key) * measure(cell, revenue);
    assert forall|cell: Cell| #[trigger] next(cell) == previous(cell) + exact(cell) by {};
    lemma_sum_add(cells, previous, exact, next);
}

pub struct State {
    base: Arc<Vec<Cell>>,
    overlays: Vec<Overlay>,
    incoming_credit: usize,
    database: Ghost<Db>,
}

impl State {
    pub closed spec fn db(self) -> Db { self.database@ }

    pub closed spec fn logical(self, weight: spec_fn(Key) -> int, revenue: bool) -> int {
        weighted((*self.base)@, weight, revenue) + overlay_weight(self.overlays@, weight, revenue)
    }

    pub closed spec fn count(self, key: Key) -> int {
        self.logical(|other: Key| if other == key { 1int } else { 0int }, false)
    }

    pub closed spec fn revenue(self, key: Key) -> int {
        self.logical(|other: Key| if other == key { 1int } else { 0int }, true)
    }

    #[verifier::type_invariant]
    closed spec fn wf(self) -> bool {
        &&& rows_valid(self.db().lineitem)
        &&& sorted((*self.base)@) && nonzero((*self.base)@)
        &&& runs_valid(self.overlays@)
        &&& forall|weight: spec_fn(Key) -> int, revenue: bool| #[trigger] self.logical(weight, revenue) ==
            table_weight(self.db().lineitem, weight, revenue)
    }

    proof fn lemma_query_step(&self, parameters: Params, processed: int, revenue: bool)
        requires 0 <= processed < 3,
        ensures self.logical(prefix_weight(parameters, processed + 1), revenue) ==
            self.logical(prefix_weight(parameters, processed), revenue) +
            self.logical(selection_weight(parameters.p2 - 1 + processed, parameters.p1, parameters.p3, parameters.p4), revenue),
    {
        lemma_prefix_step((*self.base)@, parameters, processed, revenue);
        let previous = |overlay: Overlay| weighted((*overlay.cells)@, prefix_weight(parameters, processed), revenue);
        let exact = |overlay: Overlay| weighted((*overlay.cells)@,
            selection_weight(parameters.p2 - 1 + processed, parameters.p1, parameters.p3, parameters.p4), revenue);
        let next = |overlay: Overlay| weighted((*overlay.cells)@, prefix_weight(parameters, processed + 1), revenue);
        assert forall|overlay: Overlay| #[trigger] next(overlay) == previous(overlay) + exact(overlay) by {
            lemma_prefix_step((*overlay.cells)@, parameters, processed, revenue);
        }
        lemma_sum_add(self.overlays@, previous, exact, next);
    }

    proof fn lemma_outside(&self, parameters: Params, discount: int, revenue: bool)
        requires discount < i64::MIN || discount > i64::MAX,
        ensures self.logical(selection_weight(discount, parameters.p1, parameters.p3, parameters.p4), revenue) == 0,
    {
        let weight = selection_weight(discount, parameters.p1, parameters.p3, parameters.p4);
        lemma_sum_zero((*self.base)@, |cell: Cell| weight(cell.key) * measure(cell, revenue));
        assert forall|overlay: Overlay| #[trigger] weighted((*overlay.cells)@, weight, revenue) == 0 by {
            lemma_sum_zero((*overlay.cells)@, |cell: Cell| weight(cell.key) * measure(cell, revenue));
        }
        lemma_sum_zero(self.overlays@, |overlay: Overlay| weighted((*overlay.cells)@, weight, revenue));
    }
}

pub fn build(lineitem_cube: &[LineitemInput], Ghost(db): Ghost<Db>) -> (result: Option<State>)
    requires scan_lineitem_cube_ok(db, lineitem_cube@),
    ensures result matches Some(state) ==> state.db() == db, result is Some,
{
    let mut accumulator = Accumulator::new();
    let mut position = 0usize;
    let mut processed: Ghost<int> = Ghost(0int);
    while position < lineitem_cube.len()
        invariant
            position <= lineitem_cube.len(), processed@ == position, accumulator.wf(),
            forall|weight: spec_fn(Key) -> int, revenue: bool| #[trigger] weighted(accumulator.cells@, weight, revenue) ==
                input_weight(lineitem_cube@.take(processed@), weight, revenue),
        decreases lineitem_cube.len() - position,
    {
        accumulator.add(lineitem_cube[position], 1);
        proof {
            assert forall|weight: spec_fn(Key) -> int, revenue: bool| #[trigger] input_weight(lineitem_cube@.take(position as int + 1), weight, revenue) ==
                input_weight(lineitem_cube@.take(position as int), weight, revenue) +
                weight(row_key(lineitem_input_view(lineitem_cube@[position as int]))) * row_measure(lineitem_input_view(lineitem_cube@[position as int]), revenue) by {
                lemma_sum_take(lineitem_cube@, position as int, |row: LineitemInput| weight(row_key(lineitem_input_view(row))) * row_measure(lineitem_input_view(row), revenue));
            }
        }
        position += 1;
        processed = Ghost(position as int);
    }
    let cells = seal(accumulator.cells);
    proof {
        assert(lineitem_cube@.take(position as int) == lineitem_cube@);
        lemma_build_transfer(db, lineitem_cube@);
    }
    Some(State { base: Arc::new(cells), overlays: Vec::new(), incoming_credit: 0, database: Ghost(db) })
}

pub fn execute(state: &State, p1: i32, p2: i64, p3: i64, p4: i32) -> (result: Option<Vec<Option<i128>>>)
    ensures
        result matches Some(output) ==>
            query_result(state.db(), params(p1 as int, p2 as int, p3 as int, p4 as int), out_view(output@)),
        answer_fits(state.db(), params(p1 as int, p2 as int, p3 as int, p4 as int)) ==> result is Some,
{
    proof { use_type_invariant(state); }
    let ghost parameters = params(p1 as int, p2 as int, p3 as int, p4 as int);
    proof { lemma_query_transfer(state.db(), parameters); }
    let mut output = Vec::new();
    if p1 >= p4 {
        output.push(None);
        proof {
            lemma_sum_zero(state.db().lineitem, |row: LineitemRow| (query_weight(parameters))(row_key(row)) * row_measure(row, false));
            assert(out_view(output@) == rows(state.db(), parameters));
            lemma_query_result_iff(state.db(), parameters, out_view(output@));
        }
        return Some(output);
    }
    let mut count = Wide::zero();
    let mut revenue = Wide::zero();
    let mut offset = 0usize;
    proof {
        lemma_sum_zero(state.db().lineitem, |row: LineitemRow| (prefix_weight(parameters, 0))(row_key(row)) * row_measure(row, false));
        lemma_sum_zero(state.db().lineitem, |row: LineitemRow| (prefix_weight(parameters, 0))(row_key(row)) * row_measure(row, true));
        assert(state.logical(prefix_weight(parameters, 0), false) == 0);
        assert(state.logical(prefix_weight(parameters, 0), true) == 0);
    }
    while offset < 3
        invariant
            state.wf(), p1 < p4, offset <= 3,
            parameters == params(p1 as int, p2 as int, p3 as int, p4 as int),
            count@ == state.logical(prefix_weight(parameters, offset as int), false),
            revenue@ == state.logical(prefix_weight(parameters, offset as int), true),
        decreases 3 - offset,
    {
        let ghost previous_count = count@;
        let ghost previous_revenue = revenue@;
        proof {
            state.lemma_query_step(parameters, offset as int, false);
            state.lemma_query_step(parameters, offset as int, true);
        }
        let discount = p2 as i128 - 1 + offset as i128;
        if discount >= i64::MIN as i128 && discount <= i64::MAX as i128 {
            let discount = discount as i64;
            let weights = query_run(&*state.base, discount, p1, p3, p4);
            count = count.add(&weights.0);
            revenue = revenue.add(&weights.1);
            let ghost count_before_runs = count@;
            let ghost revenue_before_runs = revenue@;
            let mut index = 0usize;
            while index < state.overlays.len()
                invariant
                    state.wf(), p1 < p4, index <= state.overlays.len(),
                    parameters == params(p1 as int, p2 as int, p3 as int, p4 as int),
                    offset < 3, discount as int == p2 as int - 1 + offset as int,
                    previous_count == state.logical(prefix_weight(parameters, offset as int), false),
                    previous_revenue == state.logical(prefix_weight(parameters, offset as int), true),
                    count_before_runs == previous_count + weighted((*state.base)@, selection_weight(discount as int, p1 as int, p3 as int, p4 as int), false),
                    revenue_before_runs == previous_revenue + weighted((*state.base)@, selection_weight(discount as int, p1 as int, p3 as int, p4 as int), true),
                    count@ == count_before_runs + overlay_weight(state.overlays@.take(index as int), selection_weight(discount as int, p1 as int, p3 as int, p4 as int), false),
                    revenue@ == revenue_before_runs + overlay_weight(state.overlays@.take(index as int), selection_weight(discount as int, p1 as int, p3 as int, p4 as int), true),
                decreases state.overlays.len() - index,
            {
                let weights = query_run(&*state.overlays[index].cells, discount, p1, p3, p4);
                count = count.add(&weights.0);
                revenue = revenue.add(&weights.1);
                proof { lemma_overlay_take(state.overlays@, index as int); }
                index += 1;
            }
            proof { assert(state.overlays@.take(index as int) == state.overlays@); }
        } else {
            proof {
                state.lemma_outside(parameters, discount as int, false);
                state.lemma_outside(parameters, discount as int, true);
            }
        }
        proof {
            state.lemma_query_step(parameters, offset as int, false);
            state.lemma_query_step(parameters, offset as int, true);
            assert(count@ == previous_count + state.logical(selection_weight(parameters.p2 - 1 + offset as int, parameters.p1, parameters.p3, parameters.p4), false));
            assert(revenue@ == previous_revenue + state.logical(selection_weight(parameters.p2 - 1 + offset as int, parameters.p1, parameters.p3, parameters.p4), true));
        }
        offset += 1;
    }
    proof {
        assert(prefix_weight(parameters, 3) == query_weight(parameters)) by {
            assert forall|key: Key| #[trigger] (prefix_weight(parameters, 3))(key) == (query_weight(parameters))(key) by {};
        }
        lemma_query_transfer(state.db(), parameters);
        lemma_query_result_iff(state.db(), parameters, Seq::empty());
        assert(count@ == group_count(state.db(), parameters));
    }
    if count.is_pos() {
        match revenue.to_i128() {
            Some(value) => output.push(Some(value)),
            None => {
                proof {
                    let answer = rows(state.db(), parameters);
                    assert(answer[0].revenue == Some(revenue@));
                    assert(!out_fits(answer[0]));
                    assert(query_result(state.db(), parameters, answer));
                    assert(!answer_fits(state.db(), parameters)) by {
                        if answer_fits(state.db(), parameters) { assert(out_fits(answer[0])); }
                    }
                }
                return None;
            },
        }
    } else {
        output.push(None);
    }
    proof {
        assert(out_view(output@) =~= rows(state.db(), parameters));
        lemma_query_result_iff(state.db(), parameters, out_view(output@));
    }
    Some(output)
}

/// Saturating doubling: credit only paces compaction, so no overflow can decline.
fn double(credit: usize) -> (result: usize)
    ensures result == if credit <= usize::MAX / 2 { 2 * credit } else { usize::MAX as int },
{
    if credit <= usize::MAX / 2 { credit * 2 } else { usize::MAX }
}

pub fn apply(state: &State, lineitem_delta: &[LineitemDelta], Ghost(db2): Ghost<Db>) -> (result: Result<State, u32>)
    requires capture_ok(state.db(), db2, lineitem_delta_seq_view(lineitem_delta@)),
    ensures result matches Ok(next) ==> next.db() == db2, result is Ok,
{
    proof { use_type_invariant(state); }
    let mut accumulator = Accumulator::new();
    let mut position = 0usize;
    let mut processed: Ghost<int> = Ghost(0int);
    while position < lineitem_delta.len()
        invariant
            position <= lineitem_delta.len(), processed@ == position, accumulator.wf(),
            forall|weight: spec_fn(Key) -> int, revenue: bool| #[trigger] weighted(accumulator.cells@, weight, revenue) ==
                delta_weight(lineitem_delta@.take(processed@), weight, revenue),
        decreases lineitem_delta.len() - position,
    {
        let delta = lineitem_delta[position];
        accumulator.add(delta.row, delta.sign);
        proof {
            assert forall|weight: spec_fn(Key) -> int, revenue: bool| #[trigger] delta_weight(lineitem_delta@.take(position as int + 1), weight, revenue) ==
                delta_weight(lineitem_delta@.take(position as int), weight, revenue) + weight(row_key(lineitem_input_view(delta.row))) *
                    ((delta.sign as int) * row_measure(lineitem_input_view(delta.row), revenue)) by {
                lemma_sum_take(lineitem_delta@, position as int, |delta: LineitemDelta| weight(row_key(lineitem_input_view(delta.row))) *
                    ((delta.sign as int) * row_measure(lineitem_input_view(delta.row), revenue)));
            }
        }
        position += 1;
        processed = Ghost(position as int);
    }
    let cells = seal(accumulator.cells);
    proof {
        assert(lineitem_delta@.take(position as int) == lineitem_delta@);
        lemma_apply_transfer(state.db(), db2, lineitem_delta@);
    }
    let ghost delta_cells = cells@;
    let mut credit = if cells.len() == 0 { 0usize } else { 1usize };
    while credit < cells.len()
        invariant credit == 0 ==> cells.len() == 0,
        decreases if credit < cells.len() { cells.len() as int - credit as int } else { 0int },
    {
        let ghost previous_credit = credit;
        proof { assert(credit > 0); }
        credit = double(credit);
        proof { assert(credit as int == 2 * previous_credit as int || credit == usize::MAX); }
    }
    let incoming_credit = state.incoming_credit.saturating_add(credit);
    let mut carry = Arc::new(cells);
    let mut overlays = Vec::new();
    let mut index = 0usize;
    while index < state.overlays.len()
        invariant
            state.wf(), index <= state.overlays.len(), runs_valid(overlays@),
            sorted((*carry)@), nonzero((*carry)@),
            forall|weight: spec_fn(Key) -> int, revenue: bool| #[trigger] weighted(delta_cells, weight, revenue) == delta_weight(lineitem_delta@, weight, revenue),
            forall|weight: spec_fn(Key) -> int, revenue: bool| #[trigger] overlay_weight(overlays@, weight, revenue) + weighted((*carry)@, weight, revenue) ==
                overlay_weight(state.overlays@.take(index as int), weight, revenue) + weighted(delta_cells, weight, revenue),
        decreases 2 * (state.overlays.len() - index) + if carry.len() == 0 { 0int } else { 1int },
    {
        let ghost previous_overlays = overlays@;
        let old = &state.overlays[index];
        if carry.len() == 0 || old.credit < credit {
            let overlay = Overlay { cells: old.cells.clone(), credit: old.credit };
            overlays.push(overlay);
            proof {
                lemma_overlay_push(previous_overlays, overlay);
                lemma_overlay_take(state.overlays@, index as int);
            }
            index += 1;
        } else if old.credit == credit {
            let merged = merge(&*old.cells, &*carry);
            credit = double(credit);
            carry = Arc::new(merged);
            proof { lemma_overlay_take(state.overlays@, index as int); }
            index += 1;
        } else {
            let overlay = Overlay { cells: carry.clone(), credit };
            overlays.push(overlay);
            proof { lemma_overlay_push(previous_overlays, overlay); }
            carry = Arc::new(Vec::new());
        }
    }
    proof { assert(state.overlays@.take(index as int) == state.overlays@); }
    if carry.len() != 0 {
        let ghost previous_overlays = overlays@;
        let overlay = Overlay { cells: carry, credit };
        overlays.push(overlay);
        proof { lemma_overlay_push(previous_overlays, overlay); }
    }
    proof {
        lemma_apply_transfer(state.db(), db2, lineitem_delta@);
        assert forall|weight: spec_fn(Key) -> int, revenue: bool| #[trigger] weighted((*state.base)@, weight, revenue) + overlay_weight(overlays@, weight, revenue) ==
            table_weight(db2.lineitem, weight, revenue) by {
            assert(state.logical(weight, revenue) == table_weight(state.db().lineitem, weight, revenue));
            assert(weighted(delta_cells, weight, revenue) == delta_weight(lineitem_delta@, weight, revenue));
            assert(overlay_weight(overlays@, weight, revenue) == overlay_weight(state.overlays@, weight, revenue) + weighted(delta_cells, weight, revenue));
        }
    }
    let threshold = if state.base.len() == 0 { 1usize } else { state.base.len() };
    if incoming_credit >= threshold {
        let mut combined = Vec::new();
        let mut index = 0usize;
        while index < overlays.len()
            invariant
                index <= overlays.len(), runs_valid(overlays@), sorted(combined@), nonzero(combined@),
                forall|weight: spec_fn(Key) -> int, revenue: bool| #[trigger] weighted(combined@, weight, revenue) ==
                    overlay_weight(overlays@.take(index as int), weight, revenue),
            decreases overlays.len() - index,
        {
            combined = merge(&combined, &*overlays[index].cells);
            proof { lemma_overlay_take(overlays@, index as int); }
            index += 1;
        }
        proof { assert(overlays@.take(index as int) == overlays@); }
        let base = merge(&*state.base, &combined);
        Ok(State { base: Arc::new(base), overlays: Vec::new(), incoming_credit: 0, database: Ghost(db2) })
    } else {
        Ok(State { base: state.base.clone(), overlays, incoming_credit, database: Ghost(db2) })
    }
}

}
