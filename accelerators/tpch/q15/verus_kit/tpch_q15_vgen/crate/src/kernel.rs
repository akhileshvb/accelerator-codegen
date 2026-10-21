use vstd::prelude::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::model::*;
use crate::maintenance::*;
use crate::output;
use crate::database_proofs::*;
use std::sync::Arc;
use crate::general::*;
use crate::log::*;
use crate::query_proofs::window_weight;
use verus_common::sums::*;

verus! {
pub struct Fast {
    pub(crate) facts: Facts,
    pub(crate) dimensions: Dimensions,
    database: Ghost<Db>,
}
impl Fast {
    pub closed spec fn db(self) -> Db { self.database@ }
    #[verifier::type_invariant]
    closed spec fn valid(self) -> bool {
        facts_match(self.facts, self.db().lineitem)
            && dimensions_match(self.dimensions, self.db().supplier)
    }
}
pub fn fast_build(lines: &[LineInput], suppliers: &Vec<SupplierInput>, Ghost(db): Ghost<Db>)
    -> (result: Option<Fast>)
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
    Some(Fast { facts, dimensions, database: Ghost(db) })
}
pub fn fast_execute(state: &Fast, p1: i32, p2: i32)
    -> (result: Option<Vec<(i64, Vec<u8>, Vec<u8>, Vec<u8>, i128)>>)
    ensures result matches Some(rows) ==> query_result(state.db(), params(p1 as int, p2 as int), out_view(rows@)),
{
    proof { use_type_invariant(state); }
    output::execute(&state.facts, &state.dimensions, p1, p2, Ghost(state.db()))
}
pub fn fast_apply(state: &Fast, lineitem_delta: &[LineDelta], supplier_delta: &Vec<SupplierDelta>, Ghost(db2): Ghost<Db>)
    -> (result: Result<Fast, u32>)
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
    Ok(Fast { facts, dimensions, database: Ghost(db2) })
}

pub proof fn lemma_sum_pointwise<A, B>(left: Seq<A>, right: Seq<B>, f: spec_fn(A) -> int, g: spec_fn(B) -> int)
    requires left.len() == right.len(), forall|index: int| 0 <= index < left.len() ==> f(#[trigger] left[index]) == g(right[index]),
    ensures sum(left, f) == sum(right, g),
    decreases left.len(),
{
    if left.len() > 0 {
        let rest_left = left.drop_last();
        let rest_right = right.drop_last();
        assert forall|index: int| 0 <= index < rest_left.len() implies f(#[trigger] rest_left[index]) == g(rest_right[index]) by {
            assert(rest_left[index] == left[index]);
            assert(rest_right[index] == right[index]);
        }
        lemma_sum_pointwise(rest_left, rest_right, f, g);
        assert(left =~= rest_left.push(left.last()));
        assert(right =~= rest_right.push(right.last()));
        lemma_sum_push(rest_left, left.last(), f);
        lemma_sum_push(rest_right, right.last(), g);
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

/// The bucket profiles (when they could be built) and the signed logs the general path replays.
pub struct State {
    fast: Option<Fast>,
    lines: Log<LineEntry>,
    suppliers: Log<SuppEntry>,
    appended: u64,
    database: Ghost<Db>,
}

pub closed spec fn history(s: State) -> nat { s.appended as nat }

impl State {
    pub closed spec fn db(self) -> Db { self.database@ }

    #[verifier::type_invariant]
    spec fn valid(self) -> bool {
        &&& self.fast matches Some(fast) ==> fast.db() == self.database@
        &&& unit_lines(self.lines.entries())
        &&& unit_supps(self.suppliers.entries())
        &&& self.appended <= 4611686018427387904
        &&& self.lines.entries().len() <= 1099511627776 + self.appended
        &&& self.suppliers.entries().len() <= 1099511627776 + self.appended
        &&& forall|f: spec_fn(LineitemRow) -> int| #[trigger] line_log_sum(self.lines.entries(), f) == sum(self.database@.lineitem, f)
        &&& forall|f: spec_fn(SupplierRow) -> int| #[trigger] supp_log_sum(self.suppliers.entries(), f) == sum(self.database@.supplier, f)
    }
}


pub proof fn lemma_window_log(db: Db, entries: Seq<LineEntry>, lower: int, upper: int, supplier: int, revenue: bool)
    requires forall|f: spec_fn(LineitemRow) -> int| #[trigger] line_log_sum(entries, f) == sum(db.lineitem, f),
    ensures crate::query_proofs::window_sum(db.lineitem, supplier, lower, upper, revenue) == sum(entries, win_term(supplier, lower, upper, revenue)),
{
    let f = window_weight(supplier, lower, upper, revenue);
    assert(line_log_sum(entries, f) == sum(db.lineitem, f));
    lemma_sum_ext_all(entries, line_term(f), win_term(supplier, lower, upper, revenue));
}

pub proof fn lemma_triple_log(db: Db, entries: Seq<SuppEntry>, supplier: int, name: Seq<u8>, address: Seq<u8>, phone: Seq<u8>)
    requires forall|f: spec_fn(SupplierRow) -> int| #[trigger] supp_log_sum(entries, f) == sum(db.supplier, f),
    ensures db_triple_count(db.supplier, supplier, name, address, phone) == sum(entries, triple_weight(supplier, name, address, phone)),
{
    let f = |row: SupplierRow| ind(row.s_suppkey == supplier && row.s_name == name && row.s_address == address && row.s_phone == phone);
    assert(supp_log_sum(entries, f) == sum(db.supplier, f));
    lemma_sum_ext_all(entries, supp_term(f), triple_weight(supplier, name, address, phone));
}

#[verifier::rlimit(60)]
pub fn build(lines: &[LineInput], suppliers: Vec<SupplierInput>, Ghost(db): Ghost<Db>) -> (result: Option<State>)
    requires scan_lines_ok(db, lines@), scan_suppliers_ok(db, suppliers@),
    ensures result matches Some(state) ==> state.db() == db,
        result matches Some(state) ==> history(state) == 0,
        envelope(db) ==> result is Some,
{
    proof { lemma_scan_lens(db, lines@, suppliers@); }
    if lines.len() as u64 > 1099511627776u64 || suppliers.len() as u64 > 1099511627776u64 { return None; }
    let fast = fast_build(lines, &suppliers, Ghost(db));
    let mut line_entries: Vec<LineEntry> = Vec::new();
    let mut index = 0usize;
    while index < lines.len()
        invariant index <= lines.len(), line_entries@.len() == index,
            forall|k: int| 0 <= k < index ==> line_entries@[k].suppkey == lines@[k].l_suppkey && line_entries@[k].price == lines@[k].l_extendedprice
                && line_entries@[k].discount == lines@[k].l_discount && line_entries@[k].shipdate == lines@[k].l_shipdate && line_entries@[k].sign == 1,
        decreases lines.len() - index,
    {
        line_entries.push(LineEntry { suppkey: lines[index].l_suppkey, price: lines[index].l_extendedprice,
            discount: lines[index].l_discount, shipdate: lines[index].l_shipdate, sign: 1 });
        index += 1;
    }
    let ghost line_view = line_input_seq_view(lines@);
    let ghost original = supplier_input_seq_view(suppliers@);
    let ghost suppliers_original = suppliers@;
    let mut remaining = suppliers;
    let mut supplier_entries: Vec<SuppEntry> = Vec::new();
    proof {
        assert(supplier_input_seq_view(remaining@) == original);
        assert(supplier_entries@ =~= Seq::<SuppEntry>::empty());
        assert forall|f: spec_fn(SupplierRow) -> int| #[trigger] supp_log_sum(supplier_entries@, f) + sum(supplier_input_seq_view(remaining@), f)
            == sum(original, f) by {
            lemma_sum_empty(supplier_entries@, supp_term(f));
        }
    }
    while remaining.len() > 0
        invariant remaining.len() + supplier_entries.len() == suppliers_original.len(),
            forall|k: int| 0 <= k < supplier_entries@.len() ==> supplier_entries@[k].sign == 1,
            forall|f: spec_fn(SupplierRow) -> int| #[trigger] supp_log_sum(supplier_entries@, f) + sum(supplier_input_seq_view(remaining@), f)
                == sum(original, f),
        decreases remaining.len(),
    {
        let ghost before_entries = supplier_entries@;
        let ghost before_remaining = remaining@;
        let row = remaining.pop().unwrap();
        let ghost viewed = SupplierRow { s_suppkey: row.s_suppkey as int, s_name: row.s_name@, s_address: row.s_address@, s_phone: row.s_phone@ };
        supplier_entries.push(SuppEntry { suppkey: row.s_suppkey, name: Arc::new(row.s_name), address: Arc::new(row.s_address),
            phone: Arc::new(row.s_phone), sign: 1 });
        proof {
            assert(supplier_input_seq_view(before_remaining) =~= supplier_input_seq_view(remaining@).push(viewed));
            assert forall|f: spec_fn(SupplierRow) -> int| #[trigger] supp_log_sum(supplier_entries@, f) + sum(supplier_input_seq_view(remaining@), f)
                == sum(original, f) by {
                assert(supp_log_sum(before_entries, f) + sum(supplier_input_seq_view(before_remaining), f) == sum(original, f));
                assert(supplier_entries@ =~= before_entries.push(supplier_entries@.last()));
                lemma_sum_push(before_entries, supplier_entries@.last(), supp_term(f));
                lemma_sum_push(supplier_input_seq_view(remaining@), viewed, f);
                assert(supp_row(supplier_entries@.last()) == viewed);
            }
        }
    }
    let ghost final_lines = line_entries@;
    let ghost final_suppliers = supplier_entries@;
    let lines_log = Log::empty().with_chunk(line_entries);
    let suppliers_log = Log::empty().with_chunk(supplier_entries);
    proof {
        assert(lines_log.entries() =~= final_lines);
        assert(suppliers_log.entries() =~= final_suppliers);
        assert(supplier_input_seq_view(remaining@) =~= Seq::<SupplierRow>::empty());
        assert forall|f: spec_fn(LineitemRow) -> int| #[trigger] line_log_sum(lines_log.entries(), f) == sum(db.lineitem, f) by {
            lemma_sum_pointwise(final_lines, line_view, line_term(f), f);
            assert forall|k: int| 0 <= k < final_lines.len() implies line_term(f)(#[trigger] final_lines[k]) == f(line_view[k]) by {
                assert(line_view[k] == line_input_view(lines@[k]));
                assert(line_row(final_lines[k]) == line_view[k]);
            }
        }
        assert forall|f: spec_fn(SupplierRow) -> int| #[trigger] supp_log_sum(suppliers_log.entries(), f) == sum(db.supplier, f) by {
            assert(supp_log_sum(final_suppliers, f) + sum(supplier_input_seq_view(remaining@), f) == sum(original, f));
            lemma_sum_empty(supplier_input_seq_view(remaining@), f);
        }
        assert forall|k: int| 0 <= k < lines_log.entries().len() implies
            (#[trigger] lines_log.entries()[k]).sign == 1 || lines_log.entries()[k].sign == -1 by {}
        assert forall|k: int| 0 <= k < suppliers_log.entries().len() implies
            (#[trigger] suppliers_log.entries()[k]).sign == 1 || suppliers_log.entries()[k].sign == -1 by {}
        assert(final_suppliers.len() == suppliers_original.len());
        assert(final_lines.len() == lines@.len());
    }
    let state = State { fast, lines: lines_log, suppliers: suppliers_log, appended: 0, database: Ghost(db) };
    Some(state)
}

pub fn execute(state: &State, p1: i32, p2: i32) -> (result: Option<Vec<(i64, Vec<u8>, Vec<u8>, Vec<u8>, i128)>>)
    ensures result matches Some(rows) ==> query_result(state.db(), params(p1 as int, p2 as int), out_view(rows@)),
        envelope(state.db()) && answer_fits(state.db(), params(p1 as int, p2 as int)) ==> result is Some,
{
    proof { use_type_invariant(state); }
    if let Some(fast) = &state.fast {
        if let Some(rows) = fast_execute(fast, p1, p2) { return Some(rows); }
    }
    proof {
        assert forall|supplier: int, revenue: bool| #[trigger] crate::query_proofs::window_sum(state.db().lineitem, supplier, p1 as int, p2 as int, revenue)
            == sum(state.lines.entries(), win_term(supplier, p1 as int, p2 as int, revenue)) by {
            lemma_window_log(state.db(), state.lines.entries(), p1 as int, p2 as int, supplier, revenue);
        }
        assert forall|supplier: int, name: Seq<u8>, address: Seq<u8>, phone: Seq<u8>| #[trigger] db_triple_count(state.db().supplier, supplier, name, address, phone)
            == sum(state.suppliers.entries(), triple_weight(supplier, name, address, phone)) by {
            lemma_triple_log(state.db(), state.suppliers.entries(), supplier, name, address, phone);
        }
    }
    replay(&state.lines, &state.suppliers, p1, p2, Ghost(state.db()))
}

#[verifier::rlimit(60)]
pub fn apply(state: &State, lineitem_delta: &[LineDelta], supplier_delta: &Vec<SupplierDelta>, Ghost(db2): Ghost<Db>)
    -> (result: Result<State, u32>)
    requires capture_ok(state.db(), db2, line_delta_seq_view(lineitem_delta@), supplier_delta_seq_view(supplier_delta@)),
    ensures result matches Ok(next) ==> next.db() == db2,
        result matches Ok(next) ==> history(next) <= history(*state) + lineitem_delta@.len() + supplier_delta@.len(),
        envelope(state.db()) && envelope(db2) && history(*state) + lineitem_delta@.len() + supplier_delta@.len() <= max_history() ==> result is Ok,
{
    proof { use_type_invariant(state); }
    let total = state.appended as u128 + lineitem_delta.len() as u128 + supplier_delta.len() as u128;
    if total > 4611686018427387904 { return Err(1); }
    let mut line_entries: Vec<LineEntry> = Vec::new();
    let mut index = 0usize;
    while index < lineitem_delta.len()
        invariant index <= lineitem_delta.len(), line_entries@.len() == index,
            forall|k: int| 0 <= k < index ==> line_entries@[k].suppkey == lineitem_delta@[k].row.l_suppkey && line_entries@[k].price == lineitem_delta@[k].row.l_extendedprice
                && line_entries@[k].discount == lineitem_delta@[k].row.l_discount && line_entries@[k].shipdate == lineitem_delta@[k].row.l_shipdate
                && line_entries@[k].sign == lineitem_delta@[k].sign,
        decreases lineitem_delta.len() - index,
    {
        let delta = &lineitem_delta[index];
        line_entries.push(LineEntry { suppkey: delta.row.l_suppkey, price: delta.row.l_extendedprice,
            discount: delta.row.l_discount, shipdate: delta.row.l_shipdate, sign: delta.sign });
        index += 1;
    }
    let mut supplier_entries: Vec<SuppEntry> = Vec::new();
    index = 0;
    while index < supplier_delta.len()
        invariant index <= supplier_delta.len(), supplier_entries@.len() == index,
            forall|k: int| 0 <= k < index ==> supplier_entries@[k].suppkey == supplier_delta@[k].row.s_suppkey
                && supplier_entries@[k].sign == supplier_delta@[k].sign
                && (*supplier_entries@[k].name)@ == supplier_delta@[k].row.s_name@
                && (*supplier_entries@[k].address)@ == supplier_delta@[k].row.s_address@
                && (*supplier_entries@[k].phone)@ == supplier_delta@[k].row.s_phone@,
        decreases supplier_delta.len() - index,
    {
        let delta = &supplier_delta[index];
        supplier_entries.push(SuppEntry { suppkey: delta.row.s_suppkey, name: Arc::new(crate::values::copy_bytes(&delta.row.s_name)),
            address: Arc::new(crate::values::copy_bytes(&delta.row.s_address)), phone: Arc::new(crate::values::copy_bytes(&delta.row.s_phone)),
            sign: delta.sign });
        index += 1;
    }
    let fast = match &state.fast {
        Some(fast) => match fast_apply(fast, lineitem_delta, supplier_delta, Ghost(db2)) {
            Ok(next) => Some(next),
            Err(_) => None,
        },
        None => None,
    };
    let ghost new_lines = line_entries@;
    let ghost new_suppliers = supplier_entries@;
    let lines_log = state.lines.with_chunk(line_entries);
    let suppliers_log = state.suppliers.with_chunk(supplier_entries);
    proof {
        let db = state.db();
        let ldelta = line_delta_seq_view(lineitem_delta@);
        let sdelta = supplier_delta_seq_view(supplier_delta@);
        assert(lines_log.entries() == state.lines.entries() + new_lines);
        assert(suppliers_log.entries() == state.suppliers.entries() + new_suppliers);
        assert forall|k: int| 0 <= k < lines_log.entries().len() implies
            (#[trigger] lines_log.entries()[k]).sign == 1 || lines_log.entries()[k].sign == -1 by {
            if k < state.lines.entries().len() { assert(lines_log.entries()[k] == state.lines.entries()[k]); }
            else {
                let offset = k - state.lines.entries().len();
                assert(lines_log.entries()[k] == new_lines[offset]);
                assert(ldelta[offset].1 == lineitem_delta@[offset].sign as int);
            }
        }
        assert forall|k: int| 0 <= k < suppliers_log.entries().len() implies
            (#[trigger] suppliers_log.entries()[k]).sign == 1 || suppliers_log.entries()[k].sign == -1 by {
            if k < state.suppliers.entries().len() { assert(suppliers_log.entries()[k] == state.suppliers.entries()[k]); }
            else {
                let offset = k - state.suppliers.entries().len();
                assert(suppliers_log.entries()[k] == new_suppliers[offset]);
                assert(sdelta[offset].1 == supplier_delta@[offset].sign as int);
            }
        }
        assert forall|f: spec_fn(LineitemRow) -> int| #[trigger] line_log_sum(lines_log.entries(), f) == sum(db2.lineitem, f) by {
            lemma_sum_concat(state.lines.entries(), new_lines, line_term(f));
            assert(line_log_sum(state.lines.entries(), f) == sum(db.lineitem, f));
            assert(sum(db2.lineitem, f) == sum(db.lineitem, f) + sum(ldelta, |d: (LineitemRow, int)| d.1 * f(d.0)));
            assert(new_lines.len() == ldelta.len());
            assert forall|k: int| 0 <= k < new_lines.len() implies line_term(f)(#[trigger] new_lines[k]) == (|d: (LineitemRow, int)| d.1 * f(d.0))(ldelta[k]) by {
                assert(new_lines[k].sign as int == ldelta[k].1);
                assert(line_row(new_lines[k]) == ldelta[k].0);
            }
            lemma_sum_pointwise(new_lines, ldelta, line_term(f), |d: (LineitemRow, int)| d.1 * f(d.0));
        }
        assert forall|f: spec_fn(SupplierRow) -> int| #[trigger] supp_log_sum(suppliers_log.entries(), f) == sum(db2.supplier, f) by {
            lemma_sum_concat(state.suppliers.entries(), new_suppliers, supp_term(f));
            assert(supp_log_sum(state.suppliers.entries(), f) == sum(db.supplier, f));
            assert(sum(db2.supplier, f) == sum(db.supplier, f) + sum(sdelta, |d: (SupplierRow, int)| d.1 * f(d.0)));
            assert(new_suppliers.len() == sdelta.len());
            assert forall|k: int| 0 <= k < new_suppliers.len() implies supp_term(f)(#[trigger] new_suppliers[k]) == (|d: (SupplierRow, int)| d.1 * f(d.0))(sdelta[k]) by {
                assert(new_suppliers[k].sign as int == sdelta[k].1);
                assert(supp_row(new_suppliers[k]) == sdelta[k].0);
            }
            lemma_sum_pointwise(new_suppliers, sdelta, supp_term(f), |d: (SupplierRow, int)| d.1 * f(d.0));
        }
    }
    let next = State { fast, lines: lines_log, suppliers: suppliers_log, appended: total as u64, database: Ghost(db2) };
    Ok(next)
}

}
