use vstd::prelude::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::model::*;
use crate::maintenance::*;
use crate::output;
use crate::database_proofs::*;
use verus_common::sums::*;

verus! {
pub struct State {
    pub(crate) facts: Facts,
    pub(crate) dimensions: Dimensions,
    database: Ghost<Db>,
}
impl State {
    pub closed spec fn db(self) -> Db { self.database@ }
    #[verifier::type_invariant]
    closed spec fn valid(self) -> bool {
        facts_match(self.facts, self.db().lineitem)
            && dimensions_match(self.dimensions, self.db().supplier)
    }
}
pub fn build(lines: &[LineInput], suppliers: Vec<SupplierInput>, Ghost(db): Ghost<Db>)
    -> (result: Option<State>)
    requires scan_lines_ok(db, lines@), scan_suppliers_ok(db, suppliers@),
    ensures result matches Some(state) ==> state.db() == db,
{
    proof {
    }
    let mut scratch = Scratch::new();
    proof { assert(line_input_seq_view(lines@.take(0)) =~= Seq::<LineitemRow>::empty()); }
    let mut position = 0;
    let mut processed = Ghost(0int);
    while position < lines.len()
        invariant position <= lines.len(), processed@ == position as int,
            scratch.well_formed(),
            scratch.matches_lines(line_input_seq_view(lines@.take(processed@))),
        decreases lines.len() - position,
    {
        let ghost before = scratch;
        if !scratch.add(&lines[position], 1) { return None; }
        proof {
            let rows = line_input_seq_view(lines@);
            assert(line_input_seq_view(lines@.take(position as int)) =~= rows.take(position as int));
            assert(line_input_seq_view(lines@.take(position as int + 1)) =~= rows.take(position as int + 1));
            assert forall|supplier: i64, date: i32| #[trigger] scratch.count(supplier, date)
                == day_count(rows.take(position as int + 1), supplier as int, date as int) &&
                scratch.revenue(supplier, date) == day_revenue(rows.take(position as int + 1), supplier as int, date as int) by {
                let count = |row: LineitemRow| if row.l_shipdate == date as int && row.l_suppkey == supplier as int { 1int } else { 0int };
                let revenue = |row: LineitemRow| if row.l_shipdate == date as int && row.l_suppkey == supplier as int { weight(row) } else { 0int };
                lemma_sum_take(rows, position as int, count);
                lemma_sum_take(rows, position as int, revenue);
                assert(before.count(supplier, date) == day_count(rows.take(position as int), supplier as int, date as int));
                assert(before.revenue(supplier, date) == day_revenue(rows.take(position as int), supplier as int, date as int));
            }
            assert forall|supplier: i64, date: i32| #[trigger] scratch.revenue(supplier, date) ==
                day_revenue(rows.take(position as int + 1), supplier as int, date as int) by {
                assert(scratch.count(supplier, date) == day_count(rows.take(position as int + 1), supplier as int, date as int));
            }
        }
        position += 1;
        processed = Ghost(position as int);
    }
    let ghost accumulated = scratch;
    let empty_facts = Facts::new();
    let facts = match scratch.finish(&empty_facts) { Some(root) => root, None => return None };
    proof {
        assert(line_input_seq_view(lines@.take(processed@)) =~= line_input_seq_view(lines@));
        assert(accumulated.matches_lines(line_input_seq_view(lines@)));
        assert forall|supplier: int, date: int| #[trigger] stored_day_count(facts, supplier, date) == accumulated.count_int(supplier, date) by {
            assert(profile_at(empty_facts, bucket(date), supplier) is None);
        }
        assert forall|supplier: int, date: int| #[trigger] stored_day_revenue(facts, supplier, date) == accumulated.revenue_int(supplier, date) by {
            assert(profile_at(empty_facts, bucket(date), supplier) is None);
        }
        lemma_scan_scratch(facts, accumulated, lines@);
    }
    let mut changes = DimensionChanges::new();
    let mut position = 0;
    let mut processed = Ghost(0int);
    while position < suppliers.len()
        invariant position <= suppliers.len(), processed@ == position as int,
            facts_match(facts, line_input_seq_view(lines@)),
            dimension_changes_shape(changes),
            dimension_changes_match(changes, supplier_input_seq_view(suppliers@.take(processed@))),
        decreases suppliers.len() - position,
    {
        let ghost before = changes;
        if !add_supplier(&mut changes, &suppliers[position], 1) { return None; }
        proof {
            let rows = supplier_input_seq_view(suppliers@);
            assert(supplier_input_seq_view(suppliers@.take(position as int)) =~= rows.take(position as int));
            assert(supplier_input_seq_view(suppliers@.take(position as int + 1)) =~= rows.take(position as int + 1));
            assert forall|supplier: int, key: verus_prooflib::ordered::KeyView| #[trigger] change_at(changes, supplier, key) ==
                supplier_count(rows.take(position as int + 1), supplier, key) by {
                let count = |row: SupplierRow| if row.s_suppkey == supplier && variant_key(row.s_name, row.s_address, row.s_phone) == key { 1int } else { 0int };
                lemma_sum_take(rows, position as int, count);
                assert(change_at(before, supplier, key) == supplier_count(rows.take(position as int), supplier, key));
            }
        }
        position += 1;
        processed = Ghost(position as int);
    }
    proof {
        assert(supplier_input_seq_view(suppliers@.take(processed@)) =~= supplier_input_seq_view(suppliers@));
        assert(dimension_changes_match(changes, supplier_input_seq_view(suppliers@)));
    }
    let empty_dimensions = Dimensions::new();
    let dimensions = match finish_suppliers(&empty_dimensions, &changes) {
        Some(root) => root, None => return None,
    };
    proof {
        assert(facts_match(facts, line_input_seq_view(lines@)));
        assert forall|supplier: int, key: verus_prooflib::ordered::KeyView| #[trigger] stored_supplier_count(dimensions, supplier, key) == supplier_count(supplier_input_seq_view(suppliers@), supplier, key) by {
            assert(variant_at(empty_dimensions, supplier, key) is None);
            assert(change_at(changes, supplier, key) == supplier_count(supplier_input_seq_view(suppliers@), supplier, key));
        }
        lemma_build_ok(facts, dimensions, lines@, suppliers@, db);
    }
    Some(State { facts, dimensions, database: Ghost(db) })
}
pub fn execute(state: &State, p1: i32, p2: i32)
    -> (result: Option<Vec<(i64, Vec<u8>, Vec<u8>, Vec<u8>, i128)>>)
    ensures result matches Some(rows) ==> query_result(state.db(), params(p1 as int, p2 as int), out_view(rows@)),
{
    proof { use_type_invariant(state); }
    output::execute(&state.facts, &state.dimensions, p1, p2, Ghost(state.db()))
}
pub fn apply(state: &State, lineitem_delta: &[LineDelta], supplier_delta: &Vec<SupplierDelta>, Ghost(db2): Ghost<Db>)
    -> (result: Result<State, u32>)
    requires capture_ok(state.db(), db2, line_delta_seq_view(lineitem_delta@), supplier_delta_seq_view(supplier_delta@)),
    ensures result matches Ok(next) ==> next.db() == db2,
{
    proof { use_type_invariant(state); }
    let mut scratch = Scratch::new();
    proof { assert(line_delta_seq_view(lineitem_delta@.take(0)) =~= Seq::<(LineitemRow, int)>::empty()); }
    let mut position = 0;
    let mut processed = Ghost(0int);
    while position < lineitem_delta.len()
        invariant position <= lineitem_delta.len(), processed@ == position as int,
            scratch.well_formed(),
            scratch.matches_delta(line_delta_seq_view(lineitem_delta@.take(processed@))),
        decreases lineitem_delta.len() - position,
    {
        let ghost before = scratch;
        if !scratch.add(&lineitem_delta[position].row, lineitem_delta[position].sign) { return Err(1); }
        proof {
            let rows = line_delta_seq_view(lineitem_delta@);
            assert(line_delta_seq_view(lineitem_delta@.take(position as int)) =~= rows.take(position as int));
            assert(line_delta_seq_view(lineitem_delta@.take(position as int + 1)) =~= rows.take(position as int + 1));
            assert forall|supplier: i64, date: i32| #[trigger] scratch.count(supplier, date) == sum(rows.take(position as int + 1), |change: (LineitemRow, int)|
                    if change.0.l_suppkey == supplier as int && change.0.l_shipdate == date as int { change.1 } else { 0int }) &&
                scratch.revenue(supplier, date) == sum(rows.take(position as int + 1), |change: (LineitemRow, int)|
                    if change.0.l_suppkey == supplier as int && change.0.l_shipdate == date as int { change.1 * weight(change.0) } else { 0int }) by {
                let count = |change: (LineitemRow, int)| if change.0.l_suppkey == supplier as int && change.0.l_shipdate == date as int { change.1 } else { 0int };
                let revenue = |change: (LineitemRow, int)| if change.0.l_suppkey == supplier as int && change.0.l_shipdate == date as int { change.1 * weight(change.0) } else { 0int };
                lemma_sum_take(rows, position as int, count);
                lemma_sum_take(rows, position as int, revenue);
                assert(before.count(supplier, date) == sum(rows.take(position as int), count));
                assert(before.revenue(supplier, date) == sum(rows.take(position as int), revenue));
            }
            assert forall|supplier: i64, date: i32| #[trigger] scratch.revenue(supplier, date) ==
                sum(rows.take(position as int + 1), |change: (LineitemRow, int)| if change.0.l_suppkey == supplier as int &&
                    change.0.l_shipdate == date as int { change.1 * weight(change.0) } else { 0int }) by {
                assert(scratch.count(supplier, date) == sum(rows.take(position as int + 1), |change: (LineitemRow, int)|
                    if change.0.l_suppkey == supplier as int && change.0.l_shipdate == date as int { change.1 } else { 0int }));
            }
        }
        position += 1;
        processed = Ghost(position as int);
    }
    let ghost accumulated = scratch;
    let facts = match scratch.finish(&state.facts) { Some(root) => root, None => return Err(2) };
    proof {
        assert(line_delta_seq_view(lineitem_delta@.take(processed@)) =~= line_delta_seq_view(lineitem_delta@));
        assert(accumulated.matches_delta(line_delta_seq_view(lineitem_delta@)));
        lemma_delta_scratch(state.facts, facts, accumulated, lineitem_delta@);
    }
    let mut changes = DimensionChanges::new();
    let mut position = 0;
    let mut processed = Ghost(0int);
    while position < supplier_delta.len()
        invariant position <= supplier_delta.len(), processed@ == position as int,
            facts_after(state.facts, facts, line_delta_seq_view(lineitem_delta@)),
            dimensions_shape(state.dimensions),
            dimension_changes_shape(changes),
            dimension_delta_match(changes, supplier_delta_seq_view(supplier_delta@.take(processed@))),
        decreases supplier_delta.len() - position,
    {
        let ghost before = changes;
        if !add_supplier(&mut changes, &supplier_delta[position].row, supplier_delta[position].sign) { return Err(3); }
        proof {
            let rows = supplier_delta_seq_view(supplier_delta@);
            assert(supplier_delta_seq_view(supplier_delta@.take(position as int)) =~= rows.take(position as int));
            assert(supplier_delta_seq_view(supplier_delta@.take(position as int + 1)) =~= rows.take(position as int + 1));
            assert forall|supplier: int, key: verus_prooflib::ordered::KeyView| #[trigger] change_at(changes, supplier, key) ==
                sum(rows.take(position as int + 1), |change: (SupplierRow, int)| if change.0.s_suppkey == supplier &&
                    variant_key(change.0.s_name, change.0.s_address, change.0.s_phone) == key { change.1 } else { 0int }) by {
                let count = |change: (SupplierRow, int)| if change.0.s_suppkey == supplier &&
                    variant_key(change.0.s_name, change.0.s_address, change.0.s_phone) == key { change.1 } else { 0int };
                lemma_sum_take(rows, position as int, count);
                assert(change_at(before, supplier, key) == sum(rows.take(position as int), count));
            }
        }
        position += 1;
        processed = Ghost(position as int);
    }
    let dimensions = match finish_suppliers(&state.dimensions, &changes) {
        Some(root) => root, None => return Err(4),
    };
    proof {
        assert(facts_after(state.facts, facts, line_delta_seq_view(lineitem_delta@)));
        assert(supplier_delta_seq_view(supplier_delta@.take(processed@)) =~= supplier_delta_seq_view(supplier_delta@));
        assert(dimension_delta_match(changes, supplier_delta_seq_view(supplier_delta@)));
        assert(dimensions_after(state.dimensions, dimensions, supplier_delta_seq_view(supplier_delta@)));
        lemma_apply_ok(state.facts, facts, state.dimensions, dimensions, state.db(), db2,
            line_delta_seq_view(lineitem_delta@), supplier_delta_seq_view(supplier_delta@));
    }
    Ok(State { facts, dimensions, database: Ghost(db2) })
}
}
