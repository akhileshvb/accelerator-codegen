use vstd::prelude::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::model::*;
use crate::maintenance::*;
use crate::database_proofs::*;
use crate::answer;
use crate::emission::OutputRow;
use verus_common::sums::*;
use verus_prooflib::ordered::KeyView;

verus! {

/// The bucket profiles and supplier variants of the database, with its table sizes.
pub struct State {
    facts: Facts,
    dimensions: Dimensions,
    lines: u64,
    suppliers: u64,
    database: Ghost<Db>,
}

/// The state keeps no history: applying a change updates the profiles in place.
pub closed spec fn history(s: State) -> nat { 0 }

impl State {
    pub closed spec fn db(self) -> Db { self.database@ }

    #[verifier::type_invariant]
    closed spec fn valid(self) -> bool {
        &&& facts_match(self.facts, self.database@.lineitem)
        &&& dimensions_match(self.dimensions, self.database@.supplier)
        &&& self.lines as int == self.database@.lineitem.len() && self.lines <= 0x10000000000
        &&& self.suppliers as int == self.database@.supplier.len() && self.suppliers <= 0x10000000000
    }
}

pub proof fn lemma_scan_lens(db: Db, lines: Seq<LineInput>, suppliers: Seq<SupplierInput>)
    requires scan_lines_ok(db, lines), scan_suppliers_ok(db, suppliers),
    ensures lines.len() == db.lineitem.len(), suppliers.len() == db.supplier.len(),
{
    let one_line = |row: LineitemRow| 1int;
    verus_prooflib::lemma_sum_ones(line_input_seq_view(lines));
    verus_prooflib::lemma_sum_ones(db.lineitem);
    assert(sum(line_input_seq_view(lines), one_line) == sum(db.lineitem, one_line));
    let one_supplier = |row: SupplierRow| 1int;
    verus_prooflib::lemma_sum_ones(supplier_input_seq_view(suppliers));
    verus_prooflib::lemma_sum_ones(db.supplier);
    assert(sum(supplier_input_seq_view(suppliers), one_supplier) == sum(db.supplier, one_supplier));
}


/// A bag's size changes by the sum of its delta's signs.
pub proof fn lemma_bag_len<A>(before: Seq<A>, after: Seq<A>, delta: Seq<(A, int)>)
    requires bag_step(before, after, delta),
    ensures after.len() == before.len() + sum(delta, |change: (A, int)| change.1),
{
    let one = |row: A| 1int;
    verus_prooflib::lemma_sum_ones(before);
    verus_prooflib::lemma_sum_ones(after);
    assert(sum(after, one) == sum(before, one) + sum(delta, |change: (A, int)| change.1 * one(change.0)));
    lemma_sum_ext_all(delta, |change: (A, int)| change.1 * one(change.0), |change: (A, int)| change.1);
}

#[verifier::rlimit(100)]
pub fn build(lines: &[LineInput], suppliers: Vec<SupplierInput>, Ghost(db): Ghost<Db>) -> (result: Option<State>)
    requires scan_lines_ok(db, lines@), scan_suppliers_ok(db, suppliers@),
    ensures result matches Some(state) ==> state.db() == db,
        result matches Some(state) ==> history(state) == 0,
        envelope(db) ==> result is Some,
{
    proof { lemma_scan_lens(db, lines@, suppliers@); }
    if lines.len() as u64 > 1099511627776u64 || suppliers.len() as u64 > 1099511627776u64 { return None; }
    let mut scratch = Scratch::new();
    proof { assert(line_input_seq_view(lines@.take(0)) =~= Seq::<LineitemRow>::empty()); }
    let mut position = 0;
    let mut processed = Ghost(0int);
    while position < lines.len()
        invariant position <= lines.len(), processed@ == position as int, lines.len() <= 1099511627776,
            scratch.well_formed(), scratch.bounded(position as int),
            scratch.matches_lines(line_input_seq_view(lines@.take(processed@))),
        decreases lines.len() - position,
    {
        let ghost before = scratch;
        scratch.add(&lines[position], 1, Ghost(position as int));
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
    proof {
        assert(line_input_seq_view(lines@.take(processed@)) =~= line_input_seq_view(lines@));
        assert(accumulated.matches_lines(line_input_seq_view(lines@)));
        lemma_scratch_lines(accumulated, lines@);
        assert forall|supplier: int, date: int| #[trigger] stored_day_count(empty_facts, supplier, date) + accumulated.count_int(supplier, date) == day_count(db.lineitem, supplier, date) by {
            let count = |row: LineitemRow| if row.l_shipdate == date && row.l_suppkey == supplier { 1int } else { 0int };
            assert(sum(line_input_seq_view(lines@), count) == sum(db.lineitem, count));
            assert(profile_at(empty_facts, bucket(date), supplier) is None);
        }
        assert forall|supplier: int, date: int| #[trigger] stored_day_revenue(empty_facts, supplier, date) + accumulated.revenue_int(supplier, date) == day_revenue(db.lineitem, supplier, date) by {
            let revenue = |row: LineitemRow| if row.l_shipdate == date && row.l_suppkey == supplier { weight(row) } else { 0int };
            assert(sum(line_input_seq_view(lines@), revenue) == sum(db.lineitem, revenue));
            assert(profile_at(empty_facts, bucket(date), supplier) is None);
        }
    }
    let facts = scratch.finish(&empty_facts, Ghost(db.lineitem));
    proof {
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
            dimension_changes_shape(changes),
            dimension_changes_match(changes, supplier_input_seq_view(suppliers@.take(processed@))),
        decreases suppliers.len() - position,
    {
        let ghost before = changes;
        add_supplier(&mut changes, &suppliers[position], 1);
        proof {
            let rows = supplier_input_seq_view(suppliers@);
            assert(supplier_input_seq_view(suppliers@.take(position as int)) =~= rows.take(position as int));
            assert(supplier_input_seq_view(suppliers@.take(position as int + 1)) =~= rows.take(position as int + 1));
            assert forall|supplier: int, key: KeyView| #[trigger] change_at(changes, supplier, key) ==
                supplier_count(rows.take(position as int + 1), supplier, key) by {
                let count = |row: SupplierRow| if row.s_suppkey == supplier && variant_key(row.s_name, row.s_address, row.s_phone) == key { 1int } else { 0int };
                lemma_sum_take(rows, position as int, count);
                assert(change_at(before, supplier, key) == supplier_count(rows.take(position as int), supplier, key));
            }
        }
        position += 1;
        processed = Ghost(position as int);
    }
    let empty_dimensions = Dimensions::new();
    proof {
        assert(supplier_input_seq_view(suppliers@.take(processed@)) =~= supplier_input_seq_view(suppliers@));
        assert(dimension_changes_match(changes, supplier_input_seq_view(suppliers@)));
        assert forall|supplier: int, key: KeyView| #[trigger] stored_supplier_count(empty_dimensions, supplier, key) + change_at(changes, supplier, key) == supplier_count(db.supplier, supplier, key) by {
            let count = |row: SupplierRow| if row.s_suppkey == supplier && variant_key(row.s_name, row.s_address, row.s_phone) == key { 1int } else { 0int };
            assert(sum(supplier_input_seq_view(suppliers@), count) == sum(db.supplier, count));
            assert(variant_at(empty_dimensions, supplier, key) is None);
        }
    }
    let dimensions = finish_suppliers(&empty_dimensions, &changes, Ghost(db.supplier));
    proof {
        assert forall|supplier: int, key: KeyView| #[trigger] stored_supplier_count(dimensions, supplier, key) == supplier_count(supplier_input_seq_view(suppliers@), supplier, key) by {
            assert(variant_at(empty_dimensions, supplier, key) is None);
            assert(change_at(changes, supplier, key) == supplier_count(supplier_input_seq_view(suppliers@), supplier, key));
        }
        lemma_build_ok(facts, dimensions, lines@, suppliers@, db);
    }
    Some(State { facts, dimensions, lines: lines.len() as u64, suppliers: suppliers.len() as u64, database: Ghost(db) })
}

pub fn execute(state: &State, p1: i32, p2: i32) -> (result: Option<Vec<OutputRow>>)
    ensures result matches Some(rows) ==> query_result(state.db(), params(p1 as int, p2 as int), out_view(rows@)),
        envelope(state.db()) && answer_fits(state.db(), params(p1 as int, p2 as int)) ==> result is Some,
{
    proof { use_type_invariant(state); }
    answer::execute(&state.facts, &state.dimensions, p1, p2, Ghost(state.db()))
}


/// The sum of the signs of a delta.
pub fn net_lines(delta: &[LineDelta]) -> (total: i128)
    requires unit_weights(line_delta_seq_view(delta@)),
    ensures total as int == sum(line_delta_seq_view(delta@), |change: (LineitemRow, int)| change.1),
        -(delta.len() as int) <= total <= delta.len() as int,
{
    let mut total = 0i128;
    let mut index = 0usize;
    proof {
        assert(line_delta_seq_view(delta@).take(0) =~= Seq::<(LineitemRow, int)>::empty());
        lemma_sum_empty(line_delta_seq_view(delta@).take(0), |change: (LineitemRow, int)| change.1);
    }
    while index < delta.len()
        invariant index <= delta.len(), unit_weights(line_delta_seq_view(delta@)), -(index as int) <= total <= index as int,
            total as int == sum(line_delta_seq_view(delta@).take(index as int), |change: (LineitemRow, int)| change.1),
        decreases delta.len() - index,
    {
        proof {
            lemma_sum_take(line_delta_seq_view(delta@), index as int, |change: (LineitemRow, int)| change.1);
            assert(line_delta_seq_view(delta@)[index as int].1 == delta@[index as int].sign as int);
        }
        total = total + delta[index].sign as i128;
        index += 1;
    }
    proof { assert(line_delta_seq_view(delta@).take(delta@.len() as int) =~= line_delta_seq_view(delta@)); }
    total
}

pub fn net_suppliers(delta: &Vec<SupplierDelta>) -> (total: i128)
    requires unit_weights(supplier_delta_seq_view(delta@)),
    ensures total as int == sum(supplier_delta_seq_view(delta@), |change: (SupplierRow, int)| change.1),
        -(delta.len() as int) <= total <= delta.len() as int,
{
    let mut total = 0i128;
    let mut index = 0usize;
    proof {
        assert(supplier_delta_seq_view(delta@).take(0) =~= Seq::<(SupplierRow, int)>::empty());
        lemma_sum_empty(supplier_delta_seq_view(delta@).take(0), |change: (SupplierRow, int)| change.1);
    }
    while index < delta.len()
        invariant index <= delta.len(), unit_weights(supplier_delta_seq_view(delta@)), -(index as int) <= total <= index as int,
            total as int == sum(supplier_delta_seq_view(delta@).take(index as int), |change: (SupplierRow, int)| change.1),
        decreases delta.len() - index,
    {
        proof {
            lemma_sum_take(supplier_delta_seq_view(delta@), index as int, |change: (SupplierRow, int)| change.1);
            assert(supplier_delta_seq_view(delta@)[index as int].1 == delta@[index as int].sign as int);
        }
        total = total + delta[index].sign as i128;
        index += 1;
    }
    proof { assert(supplier_delta_seq_view(delta@).take(delta@.len() as int) =~= supplier_delta_seq_view(delta@)); }
    total
}

#[verifier::rlimit(200)]
pub fn apply(state: &State, lineitem_delta: &[LineDelta], supplier_delta: &Vec<SupplierDelta>, Ghost(db2): Ghost<Db>) -> (result: Result<State, u32>)
    requires capture_ok(state.db(), db2, line_delta_seq_view(lineitem_delta@), supplier_delta_seq_view(supplier_delta@)),
    ensures result matches Ok(next) ==> next.db() == db2,
        result matches Ok(next) ==> history(next) <= history(*state) + lineitem_delta@.len() + supplier_delta@.len(),
        envelope(state.db()) && envelope(db2) && history(*state) + lineitem_delta@.len() + supplier_delta@.len() <= max_history() ==> result is Ok,
{
    proof { use_type_invariant(state); }
    let ghost db = state.db();
    let ghost ldelta = line_delta_seq_view(lineitem_delta@);
    let ghost sdelta = supplier_delta_seq_view(supplier_delta@);
    if lineitem_delta.len() as u128 + supplier_delta.len() as u128 > 4611686018427387904u128 { return Err(1); }
    let line_net = net_lines(lineitem_delta);
    let supplier_net = net_suppliers(supplier_delta);
    proof {
        lemma_bag_len(db.lineitem, db2.lineitem, ldelta);
        lemma_bag_len(db.supplier, db2.supplier, sdelta);
        lemma_delta_target(db, db2, ldelta, sdelta);
    }
    let new_lines = state.lines as i128 + line_net;
    let new_suppliers = state.suppliers as i128 + supplier_net;
    if new_lines > 1099511627776i128 || new_suppliers > 1099511627776i128 { return Err(2); }
    let mut scratch = Scratch::new();
    proof { assert(line_delta_seq_view(lineitem_delta@.take(0)) =~= Seq::<(LineitemRow, int)>::empty()); }
    let mut position = 0;
    let mut processed = Ghost(0int);
    while position < lineitem_delta.len()
        invariant position <= lineitem_delta.len(), processed@ == position as int, lineitem_delta.len() <= 4611686018427387904,
            scratch.well_formed(), scratch.bounded(position as int),
            unit_weights(ldelta), ldelta == line_delta_seq_view(lineitem_delta@),
            scratch.matches_delta(line_delta_seq_view(lineitem_delta@.take(processed@))),
        decreases lineitem_delta.len() - position,
    {
        let ghost before = scratch;
        proof { assert(ldelta[position as int].1 == lineitem_delta@[position as int].sign as int); }
        scratch.add(&lineitem_delta[position].row, lineitem_delta[position].sign, Ghost(position as int));
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
    proof {
        assert(line_delta_seq_view(lineitem_delta@.take(processed@)) =~= line_delta_seq_view(lineitem_delta@));
        assert(accumulated.matches_delta(line_delta_seq_view(lineitem_delta@)));
        lemma_scratch_deltas(accumulated, lineitem_delta@);
        assert(db2.lineitem.len() == new_lines as int);
    }
    let facts = scratch.finish(&state.facts, Ghost(db2.lineitem));
    proof {
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
        add_supplier(&mut changes, &supplier_delta[position].row, supplier_delta[position].sign);
        proof {
            let rows = supplier_delta_seq_view(supplier_delta@);
            assert(supplier_delta_seq_view(supplier_delta@.take(position as int)) =~= rows.take(position as int));
            assert(supplier_delta_seq_view(supplier_delta@.take(position as int + 1)) =~= rows.take(position as int + 1));
            assert forall|supplier: int, key: KeyView| #[trigger] change_at(changes, supplier, key) ==
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
    proof {
        assert(supplier_delta_seq_view(supplier_delta@.take(processed@)) =~= supplier_delta_seq_view(supplier_delta@));
        assert(dimension_delta_match(changes, supplier_delta_seq_view(supplier_delta@)));
        assert(db2.supplier.len() == new_suppliers as int);
        assert forall|supplier: int, key: KeyView| #[trigger] stored_supplier_count(state.dimensions, supplier, key) + change_at(changes, supplier, key) == supplier_count(db2.supplier, supplier, key) by {
            assert(stored_supplier_count(state.dimensions, supplier, key) == supplier_count(db.supplier, supplier, key));
        }
    }
    let dimensions = finish_suppliers(&state.dimensions, &changes, Ghost(db2.supplier));
    proof {
        assert(supplier_delta_seq_view(supplier_delta@.take(processed@)) =~= supplier_delta_seq_view(supplier_delta@));
        assert(dimensions_after(state.dimensions, dimensions, supplier_delta_seq_view(supplier_delta@)));
        lemma_apply_ok(state.facts, facts, state.dimensions, dimensions, db, db2,
            line_delta_seq_view(lineitem_delta@), supplier_delta_seq_view(supplier_delta@));
    }
    Ok(State { facts, dimensions, lines: new_lines as u64, suppliers: new_suppliers as u64, database: Ghost(db2) })
}

}
