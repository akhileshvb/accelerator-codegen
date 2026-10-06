use vstd::prelude::*;
use std::sync::Arc;
use verus_common::sums::sum;
use verus_prooflib::ordered::Index;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::facts::*;
use crate::dimensions::*;
use crate::paged::*;
use crate::customers::*;
use crate::model::*;
use crate::maintenance::*;
use crate::output::{OutputRow, run};

verus! {

pub struct State {
    pub(crate) days: Index<Slice>,
    pub(crate) periods: Periods,
    pub(crate) dimensions: Dimensions,
    db: Ghost<Db>,
}

impl State {
    pub closed spec fn db(self) -> Db { self.db@ }

    #[verifier::type_invariant]
    pub closed spec fn valid(self) -> bool {
        &&& facts_valid(self.db(), self.days, 0)
        &&& facts_valid(self.db(), self.periods.months, 1)
        &&& facts_valid(self.db(), self.periods.quarters, 2)
        &&& facts_valid(self.db(), self.periods.years, 3)
        &&& dimensions_valid(self.db(), self.dimensions)
    }
}

/// Lines, orders and their per-grain facts, built chunk by chunk so the intermediate patches stay small.
pub fn build_facts(orders: &[OrderInput], returned_lines: &[ReturnedLineInput], Ghost(db): Ghost<Db>)
    -> (result: Option<(Index<Slice>, Periods)>)
    requires scan_orders_ok(db, orders@), scan_returned_lines_ok(db, returned_lines@),
    ensures result matches Some((days, periods)) ==> facts_valid(db, days, 0) && facts_valid(db, periods.months, 1)
        && facts_valid(db, periods.quarters, 2) && facts_valid(db, periods.years, 3),
{
    let lines = match scan_lines(returned_lines) { Some(value) => value, None => return None };
    let mut days = Index::new();
    let mut periods = Periods::new();
    let mut start = 0usize;
    while start < orders.len()
        invariant start <= orders.len(),
            slices_valid(days), slices_valid(periods.months), slices_valid(periods.quarters), slices_valid(periods.years),
            forall|key: i64, component: int| #[trigger] hash_moment_at(lines@, key).component(component)
                == scanned_line_sum(returned_lines@, key as int, component),
            forall|day: int, customer: int, component: int| #[trigger] slice_at(days, day, customer).component(component)
                == scanned_fact_sum(orders@.take(start as int), returned_lines@, day, customer, 0, component),
            forall|day: int, customer: int, component: int| #[trigger] slice_at(periods.months, day, customer).component(component)
                == scanned_fact_sum(orders@.take(start as int), returned_lines@, day, customer, 1, component),
            forall|day: int, customer: int, component: int| #[trigger] slice_at(periods.quarters, day, customer).component(component)
                == scanned_fact_sum(orders@.take(start as int), returned_lines@, day, customer, 2, component),
            forall|day: int, customer: int, component: int| #[trigger] slice_at(periods.years, day, customer).component(component)
                == scanned_fact_sum(orders@.take(start as int), returned_lines@, day, customer, 3, component),
        decreases orders.len() - start,
    {
        let end = if orders.len() - start > 262144 { start + 262144 } else { orders.len() };
        let chunk = vstd::slice::slice_subrange(orders, start, end);
        let patches = match build_chunk_patches(chunk, Ghost(returned_lines@), &lines) { Some(value) => value, None => return None };
        if !apply_patches(&mut days, &mut periods, &patches) { return None; }
        proof {
            assert(chunk@ == orders@.subrange(start as int, end as int));
            assert forall|day: int, customer: int, component: int| #[trigger] slice_at(days, day, customer).component(component)
                == scanned_fact_sum(orders@.take(end as int), returned_lines@, day, customer, 0, component) by {
                lemma_scanned_fact_split(orders@, returned_lines@, start as int, end as int, day, customer, 0, component);
            }
            assert forall|day: int, customer: int, component: int| #[trigger] slice_at(periods.months, day, customer).component(component)
                == scanned_fact_sum(orders@.take(end as int), returned_lines@, day, customer, 1, component) by {
                lemma_scanned_fact_split(orders@, returned_lines@, start as int, end as int, day, customer, 1, component);
            }
            assert forall|day: int, customer: int, component: int| #[trigger] slice_at(periods.quarters, day, customer).component(component)
                == scanned_fact_sum(orders@.take(end as int), returned_lines@, day, customer, 2, component) by {
                lemma_scanned_fact_split(orders@, returned_lines@, start as int, end as int, day, customer, 2, component);
            }
            assert forall|day: int, customer: int, component: int| #[trigger] slice_at(periods.years, day, customer).component(component)
                == scanned_fact_sum(orders@.take(end as int), returned_lines@, day, customer, 3, component) by {
                lemma_scanned_fact_split(orders@, returned_lines@, start as int, end as int, day, customer, 3, component);
            }
        }
        start = end;
    }
    proof {
        assert(orders@.take(start as int) =~= orders@);
        crate::scan_proofs::lemma_facts_from_scans(db, days, orders@, returned_lines@, 0);
        crate::scan_proofs::lemma_facts_from_scans(db, periods.months, orders@, returned_lines@, 1);
        crate::scan_proofs::lemma_facts_from_scans(db, periods.quarters, orders@, returned_lines@, 2);
        crate::scan_proofs::lemma_facts_from_scans(db, periods.years, orders@, returned_lines@, 3);
    }
    Some((days, periods))
}

pub fn build(mut customers: Vec<CustomerInput>, nations: Vec<NationInput>, orders: &[OrderInput],
    returned_lines: &[ReturnedLineInput], Ghost(db): Ghost<Db>) -> (result: Option<State>)
    requires scan_customers_ok(db, customers@), scan_nations_ok(db, nations@),
        scan_orders_ok(db, orders@), scan_returned_lines_ok(db, returned_lines@),
    ensures result matches Some(state) ==> state.db() == db,
{
    let ghost original = customers@;
    let mut dimensions = Dimensions::new();
    proof { assert(original.skip(original.len() as int) =~= Seq::<CustomerInput>::empty()); }
    while customers.len() > 0
        invariant
            customer_shape(dimensions),
            nation_index_valid(dimensions.nations),
            maxima_valid(dimensions.nation_totals, dimensions.nation_maxima),
            forall|key: int| #[trigger] total_at(dimensions.nation_totals, key) == 0,
            forall|row: NationRow| #[trigger] nation_count(dimensions.nations, row) == 0,
            customers@ == original.take(customers.len() as int),
            original == customers@ + original.skip(customers.len() as int),
            forall|weight: spec_fn(CustomerRow) -> int| #[trigger] table_sum(dimensions.customers, weight)
                == sum(customer_input_seq_view(original.skip(customers.len() as int)), weight),
        decreases customers.len(),
    {
        let ghost remaining = customers.len() as int;
        let row = match customers.pop() { Some(value) => value, None => return None };
        proof {
            assert(customers@ =~= original.take(remaining - 1));
            assert(original.skip(remaining - 1) =~= seq![row] + original.skip(remaining));
            assert(customer_input_seq_view(original.skip(remaining - 1))
                =~= seq![customer_input_view(row)] + customer_input_seq_view(original.skip(remaining)));
        }
        let shared = Arc::new(row);
        if !dimensions.edit_customer(&shared, 1) { return None; }
        proof {
            assert forall|weight: spec_fn(CustomerRow) -> int| #[trigger] table_sum(dimensions.customers, weight)
                == sum(customer_input_seq_view(original.skip(remaining - 1)), weight) by {
                verus_common::sums::lemma_sum_concat(seq![customer_input_view(row)],
                    customer_input_seq_view(original.skip(remaining)), weight);
                verus_common::sums::lemma_sum_singleton(customer_input_view(row), weight);
            }
            assert(original == customers@ + original.skip(customers.len() as int)) by {
                assert(original =~= original.take(remaining - 1) + original.skip(remaining - 1));
            }
        }
    }
    proof {
        assert(original.skip(0) =~= original);
        assert(customers@ =~= Seq::<CustomerInput>::empty());
    }
    let mut index = 0usize;
    index = 0;
    while index < nations.len()
        invariant index <= nations.len(),
            customer_shape(dimensions),
            nation_index_valid(dimensions.nations),
            maxima_valid(dimensions.nation_totals, dimensions.nation_maxima),
            forall|weight: spec_fn(CustomerRow) -> int| #[trigger] table_sum(dimensions.customers, weight)
                == sum(customer_input_seq_view(original), weight),
            forall|key: int| #[trigger] total_at(dimensions.nation_totals, key)
                == sum(nations@.take(index as int), |input: NationInput| if input.n_nationkey == key { 1 } else { 0 }),
            forall|row: NationRow| #[trigger] nation_count(dimensions.nations, row)
                == sum(nations@.take(index as int), |input: NationInput| if nation_input_view(input) == row { 1 } else { 0 }),
        decreases nations.len() - index,
    {
        let ghost processed = index as int;
        if !dimensions.edit_nation(&nations[index], 1) { return None; }
        proof {
            assert forall|key: int| #[trigger] total_at(dimensions.nation_totals, key)
                == sum(nations@.take(index as int + 1), |input: NationInput| if input.n_nationkey == key { 1 } else { 0 }) by {
                verus_common::sums::lemma_sum_take(nations@, index as int,
                    |input: NationInput| if input.n_nationkey == key { 1 } else { 0 });
            }
            assert forall|row: NationRow| #[trigger] nation_count(dimensions.nations, row)
                == sum(nations@.take(index as int + 1), |input: NationInput| if nation_input_view(input) == row { 1 } else { 0 }) by {
                verus_common::sums::lemma_sum_take(nations@, index as int,
                    |input: NationInput| if nation_input_view(input) == row { 1 } else { 0 });
            }
        }
        index += 1;
    }
    proof {
        assert(nations@.take(index as int) =~= nations@);
        lemma_dimensions_from_scans(db, dimensions, original, nations@);
    }
    let (days, periods) = match build_facts(orders, returned_lines, Ghost(db)) { Some(value) => value, None => return None };
    Some(State { days, periods, dimensions, db: Ghost(db) })
}

pub fn execute(state: &State, p1: i32, p2: i32) -> (result: Option<Vec<OutputRow>>)
    ensures result matches Some(out) ==> query_result(state.db(), params(p1 as int, p2 as int), out_view(out@)),
{
    proof { use_type_invariant(state); reveal(State::valid); }
    run(&state.days, &state.periods, &state.dimensions, p1, p2, Ghost(state.db()))
}

pub fn rescan_orders_for_keys_keys(state: &State, customer_delta: &Vec<CustomerDelta>,
    lineitem_delta: &[LineDelta], nation_delta: &Vec<NationDelta>, orders_delta: &[OrderDelta]) -> Vec<i64>
{
    planned_keys(orders_delta, lineitem_delta)
}

pub fn rescan_returned_lines_for_keys_keys(state: &State, customer_delta: &Vec<CustomerDelta>,
    lineitem_delta: &[LineDelta], nation_delta: &Vec<NationDelta>, orders_delta: &[OrderDelta]) -> Vec<i64>
{
    planned_keys(orders_delta, lineitem_delta)
}

#[verifier::rlimit(20)]
pub fn apply(state: &State, customer_delta: &Vec<CustomerDelta>, lineitem_delta: &[LineDelta],
    nation_delta: &Vec<NationDelta>, orders_delta: &[OrderDelta], orders_for_keys_keys: &[i64],
    orders_for_keys_rows: &[OrderInput], returned_lines_for_keys_keys: &[i64],
    returned_lines_for_keys_rows: &[ReturnedLineInput], Ghost(lineitem_full): Ghost<Seq<(LineitemRow, int)>>,
    Ghost(db2): Ghost<Db>) -> (result: Result<State, u32>)
    requires lineitem_delta_ok(lineitem_delta@, lineitem_full),
        capture_ok(state.db(), db2, customer_delta_seq_view(customer_delta@), lineitem_full,
            nation_delta_seq_view(nation_delta@), order_delta_seq_view(orders_delta@)),
        rescan_orders_for_keys_ok(db2, orders_for_keys_keys@, orders_for_keys_rows@),
        rescan_returned_lines_for_keys_ok(db2, returned_lines_for_keys_keys@, returned_lines_for_keys_rows@),
    ensures result matches Ok(next) ==> next.db() == db2,
{
    proof {
        use_type_invariant(state);
        reveal(State::valid);
        lemma_facts_shape(state.db(), state.days, 0);
        lemma_facts_shape(state.db(), state.periods.months, 1);
        lemma_facts_shape(state.db(), state.periods.quarters, 2);
        lemma_facts_shape(state.db(), state.periods.years, 3);
        lemma_dimensions_shape(state.db(), state.dimensions);
    }
    let order_changes = match collect_order_changes(orders_delta) { Some(value) => value, None => return Err(1) };
    let line_changes = match collect_line_changes(lineitem_delta) { Some(value) => value, None => return Err(1) };
    let keys = support(&order_changes, &line_changes);
    if !covers(&keys, orders_for_keys_keys) || !covers(&keys, returned_lines_for_keys_keys) { return Err(2); }
    let post_orders = match collect_orders(orders_for_keys_rows) { Some(value) => value, None => return Err(1) };
    let post_lines = match collect_lines(returned_lines_for_keys_rows) { Some(value) => value, None => return Err(1) };
    let patches = match changed_patches(&keys, &order_changes, &line_changes, &post_orders, &post_lines) {
        Some(value) => value, None => return Err(3),
    };
    let key_snapshot = keys.share();
    let patch_snapshot = patches.share();
    proof {
        assert forall|day: int, customer: int, grain: int, component: int|
            #[trigger] patch_rows_sum(patches.entries(), day, customer, grain, component)
                == fact_sum(db2, day, customer, grain, component) - fact_sum(state.db(), day, customer, grain, component) by {
            crate::maintenance_proofs::lemma_patch_capture(state.db(), db2, &keys, patches,
                order_changes, line_changes, post_orders, post_lines, orders_delta@, lineitem_delta@, lineitem_full,
                orders_for_keys_keys@, orders_for_keys_rows@, returned_lines_for_keys_keys@, returned_lines_for_keys_rows@,
                day, customer, grain, component);
        }
        assert forall|day: int, customer: int, component: int|
            #[trigger] patch_at(patches, day, customer).component(component)
                == patch_rows_sum(patches.entries(), day, customer, 0, component) by {
            lemma_patch_rows(&patches, day, customer, component);
        }
    }
    let mut days = state.days.share();
    let mut periods = state.periods.share();
    let mut dimensions = state.dimensions.share();
    proof {
        assert forall|key: (int, Seq<u8>)| #[trigger] days.at(key) == state.days.at(key) by {}
        assert(slices_valid(days));
        assert(slices_valid(periods.months));
        assert(slices_valid(periods.quarters));
        assert(slices_valid(periods.years));
        assert forall|key: (int, Seq<u8>)| #[trigger] dimensions.nations.at(key) == state.dimensions.nations.at(key) by {}
        assert(customer_shape(dimensions));
        assert(nation_index_valid(dimensions.nations));
        assert(maxima_valid(dimensions.nation_totals, dimensions.nation_maxima));
        lemma_facts_equal(state.db(), state.days, days, 0);
        lemma_facts_equal(state.db(), state.periods.months, periods.months, 1);
        lemma_facts_equal(state.db(), state.periods.quarters, periods.quarters, 2);
        lemma_facts_equal(state.db(), state.periods.years, periods.years, 3);
        lemma_dimensions_equal(state.db(), state.dimensions, dimensions);
    }
    let ghost previous_days = days;
    let ghost previous_periods = periods;
    let ghost previous_dimensions = dimensions;
    if !apply_patches(&mut days, &mut periods, &patches) { return Err(3); }
    proof {
        crate::maintenance_proofs::lemma_updated_facts(state.db(), db2, previous_days, days, &patches, 0);
        crate::maintenance_proofs::lemma_updated_facts(state.db(), db2, previous_periods.months, periods.months, &patches, 1);
        crate::maintenance_proofs::lemma_updated_facts(state.db(), db2, previous_periods.quarters, periods.quarters, &patches, 2);
        crate::maintenance_proofs::lemma_updated_facts(state.db(), db2, previous_periods.years, periods.years, &patches, 3);
    }
    if !apply_dimensions(&mut dimensions, customer_delta, nation_delta) { return Err(3); }
    proof {
        crate::dimension_updates::lemma_dimensions_capture(state.db(), db2, previous_dimensions, dimensions, customer_delta@, nation_delta@);
    }
    Ok(State { days, periods, dimensions, db: Ghost(db2) })
}

}
