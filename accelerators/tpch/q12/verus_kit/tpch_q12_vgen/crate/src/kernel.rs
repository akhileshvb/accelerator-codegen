use vstd::prelude::*;
use std::sync::Arc;
use verus_common::sums::sum;
use verus_common::sums::lemma_sum_take;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::counts::{Counts, Tally};
use verus_prooflib::wide::Wide;
use std::collections::HashMap;
use crate::histogram::{Directory, Layers, Sparse};
use crate::moments::{self, Orders, Lines, Cells};
use crate::bytes;

verus! {

broadcast use vstd::seq_lib::group_seq_properties;

pub open spec fn high_priority(order: OrdersRow) -> bool {
    order.o_orderpriority == seq![b'1', b'-', b'U', b'R', b'G', b'E', b'N', b'T']
        || order.o_orderpriority == seq![b'2', b'-', b'H', b'I', b'G', b'H']
}

pub open spec fn order_count(db: Db, key: int, high: bool) -> int {
    sum(db.orders, |order: OrdersRow| if order.o_orderkey == key && high_priority(order) == high { 1 } else { 0 })
}

pub open spec fn line_count(db: Db, key: int, mode: Seq<u8>, day: int) -> int {
    sum(db.lineitem, |line: LineitemRow| if line.l_orderkey == key
        && crate::semantics::line_selected(line, mode, day) { 1 } else { 0 })
}

pub open spec fn cell_count(db: Db, mode: Seq<u8>, day: int, high: bool) -> int {
    sum(db.orders, |order: OrdersRow| if high_priority(order) == high {
        line_count(db, order.o_orderkey, mode, day)
    } else { 0 })
}

pub open spec fn directory_at(directory: Arc<Directory>, mode: Seq<u8>, day: int) -> Counts {
    if i32::MIN <= day <= i32::MAX { directory.at(mode, day as i32) }
    else { Counts { high: 0, low: 0 } }
}

pub struct State {
    directory: Arc<Directory>,
    database: Ghost<Db>,
}

impl State {
    pub closed spec fn db(self) -> Db { self.database@ }

    pub closed spec fn at(self, mode: Seq<u8>, day: int) -> Counts {
        directory_at(self.directory, mode, day)
    }

    #[verifier::type_invariant]
    spec fn wf(self) -> bool {
        &&& self.directory.wf()
        &&& forall|mode: Seq<u8>, day: int| #![trigger self.at(mode, day)]
            self.at(mode, day).high == cell_count(self.db(), mode, day, true)
                && self.at(mode, day).low == cell_count(self.db(), mode, day, false)
    }

    proof fn lemma_range(self, parameters: Params, mode: Seq<u8>, high: bool)
        requires self.wf(), i32::MIN <= parameters.p3 <= i32::MAX, i32::MIN <= parameters.p4 <= i32::MAX,
        ensures crate::semantics::range_cells(self.db(), mode, parameters.p3, parameters.p4, high)
            == self.directory.range_count(mode, parameters.p3, parameters.p4, high),
    {
        let offsets = verus_common::sums::rng(if parameters.p4 > parameters.p3 { parameters.p4 - parameters.p3 } else { 0 });
        let cells = |offset: int| cell_count(self.db(), mode, parameters.p3 + offset, high);
        let ghost directory_model = *self.directory;
        let directory = directory_model.range_weight(mode, parameters.p3, high);
        assert forall|index: int| #![trigger offsets[index]] 0 <= index < offsets.len() implies
            cells(offsets[index]) == directory(offsets[index]) by {
            let day = parameters.p3 + offsets[index];
            assert(i32::MIN <= day <= i32::MAX);
            assert(self.at(mode, day).high == cell_count(self.db(), mode, day, true));
            assert(self.at(mode, day).low == cell_count(self.db(), mode, day, false));
        }
        verus_common::sums::lemma_sum_ext(offsets, cells, directory);
    }

    proof fn lemma_group_range(self, parameters: Params, mode: Seq<u8>)
        requires self.wf(), i32::MIN <= parameters.p3 <= i32::MAX, i32::MIN <= parameters.p4 <= i32::MAX,
        ensures row_of(self.db(), parameters, mode).high_line_count ==
            if mode == parameters.p1 || mode == parameters.p2 { self.directory.range_count(mode, parameters.p3, parameters.p4, true) } else { 0 },
            row_of(self.db(), parameters, mode).low_line_count ==
            if mode == parameters.p1 || mode == parameters.p2 { self.directory.range_count(mode, parameters.p3, parameters.p4, false) } else { 0 },
            group_count(self.db(), parameters, mode) == row_of(self.db(), parameters, mode).high_line_count + row_of(self.db(), parameters, mode).low_line_count,
    {
        crate::semantics::lemma_range_sql(self.db(), parameters, mode, true);
        crate::semantics::lemma_range_sql(self.db(), parameters, mode, false);
        crate::semantics::lemma_group_case(self.db(), parameters, mode, true);
        crate::semantics::lemma_group_case(self.db(), parameters, mode, false);
        crate::semantics::lemma_group_count(self.db(), parameters, mode);
        self.lemma_range(parameters, mode, true);
        self.lemma_range(parameters, mode, false);
    }
}

pub open spec fn scan_order_count(rows: Seq<OrderKeyInput>, key: i64) -> int {
    sum(rows, |row: OrderKeyInput| if row.o_orderkey == key { 1 } else { 0 })
}

pub open spec fn scan_line_count(rows: Seq<QualifyingLineInput>, key: i64, mode: Seq<u8>, day: i32) -> int {
    sum(rows, scan_line_weight(key, mode, day))
}

pub open spec fn scan_line_weight(key: i64, mode: Seq<u8>, day: i32) -> spec_fn(QualifyingLineInput) -> int {
    |row: QualifyingLineInput| if row.l_orderkey == key && row.l_shipmode@ == mode
        && row.l_receiptdate == day { 1 } else { 0 }
}

fn fold_orders(high: &[OrderKeyInput], low: &[OrderKeyInput]) -> (value: Orders)
    ensures value.wf()
        && forall|key: i64| #![trigger value.at(key)]
            value.at(key).high == scan_order_count(high@, key)
                && value.at(key).low == scan_order_count(low@, key),
{
    let mut out = Orders::new();
    let mut index = 0;
    let mut processed = Ghost(0int);
    while index < high.len()
        invariant index <= high.len(), processed@ == index as int, out.wf(),
            forall|key: i64| #![trigger out.at(key)]
                out.at(key).high == scan_order_count(high@.take(processed@), key)
                    && out.at(key).low == 0,
        decreases high.len() - index,
    {
        out.add(high[index].o_orderkey, &Tally::of(1, 0));
        proof {
            assert forall|key: i64| #![trigger out.at(key)]
                out.at(key).high == scan_order_count(high@.take(index as int + 1), key) by {
                lemma_sum_take(high@, index as int, |row: OrderKeyInput| if row.o_orderkey == key { 1 } else { 0 });
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    proof { assert(high@.take(processed@) =~= high@); }
    index = 0;
    processed = Ghost(0int);
    while index < low.len()
        invariant index <= low.len(), processed@ == index as int, out.wf(),
            forall|key: i64| #![trigger out.at(key)]
                out.at(key).high == scan_order_count(high@, key)
                    && out.at(key).low == scan_order_count(low@.take(processed@), key),
        decreases low.len() - index,
    {
        out.add(low[index].o_orderkey, &Tally::of(0, 1));
        proof {
            assert forall|key: i64| #![trigger out.at(key)]
                out.at(key).low == scan_order_count(low@.take(index as int + 1), key) by {
                lemma_sum_take(low@, index as int, |row: OrderKeyInput| if row.o_orderkey == key { 1 } else { 0 });
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    proof { assert(low@.take(processed@) =~= low@); }
    out
}

fn fold_lines(rows: &Vec<QualifyingLineInput>) -> (value: Lines)
    ensures value.wf()
        && forall|key: i64, mode: Seq<u8>, day: i32| #![trigger value.at(key, mode, day)]
            value.at(key, mode, day) == scan_line_count(rows@, key, mode, day),
{
    let mut out = Lines::new();
    let mut index = 0;
    let mut processed = Ghost(0int);
    while index < rows.len()
        invariant index <= rows@.len(), processed@ == index as int, out.wf(),
            forall|key: i64, mode: Seq<u8>, day: i32| #![trigger out.at(key, mode, day)]
                out.at(key, mode, day) == scan_line_count(rows@.take(processed@), key, mode, day),
        decreases rows@.len() - index,
    {
        let row = &rows[index];
        out.add(row.l_orderkey, &row.l_shipmode, row.l_receiptdate, 1);
        proof {
            assert forall|key: i64, mode: Seq<u8>, day: i32| #![trigger out.at(key, mode, day)]
                out.at(key, mode, day) == scan_line_count(rows@.take(index as int + 1), key, mode, day) by {
                lemma_sum_take(rows@, index as int, scan_line_weight(key, mode, day));
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    proof { assert(rows@.take(processed@) =~= rows@); }
    out
}

pub open spec fn joined_weight(orders: Orders, mode: Seq<u8>, day: i32, high: bool) -> spec_fn(QualifyingLineInput) -> int {
    |row: QualifyingLineInput| if row.l_shipmode@ == mode && row.l_receiptdate == day {
        if high { orders.at(row.l_orderkey).high as int } else { orders.at(row.l_orderkey).low as int }
    } else { 0 }
}

pub open spec fn joined_scan(rows: Seq<QualifyingLineInput>, orders: Orders,
    mode: Seq<u8>, day: i32, high: bool) -> int {
    sum(rows, joined_weight(orders, mode, day, high))
}

fn publish_mode(base: &Arc<Directory>, next: &mut Directory, entry: &crate::moments::CellMode)
    requires base.wf(), old(next).wf(), entry.days.wf(),
        forall|day: i32| #![trigger old(next).at(entry.mode@, day)] old(next).at(entry.mode@, day) == base.at(entry.mode@, day),
        forall|day: i32| #![trigger entry.days.at(day)]
            base.at(entry.mode@, day).high + entry.days.at(day).high >= 0
                && base.at(entry.mode@, day).low + entry.days.at(day).low >= 0,
    ensures final(next).wf(),
        forall|day: i32| #![trigger final(next).at(entry.mode@, day)]
            final(next).at(entry.mode@, day).high == base.at(entry.mode@, day).high + entry.days.at(day).high
                && final(next).at(entry.mode@, day).low == base.at(entry.mode@, day).low + entry.days.at(day).low,
        forall|text: Seq<u8>, day: i32| #![trigger final(next).at(text, day)]
            text != entry.mode@ ==> final(next).at(text, day) == old(next).at(text, day),
{
    let existing = base.find(&entry.mode);
    let mut patch = Sparse::new();
    let mut index = 0;
    while index < entry.days.keys.len()
        invariant index <= entry.days.keys@.len(), base.wf(), entry.days.wf(), patch.wf(),
            *next == *old(next), next.wf(),
            forall|day: i32| #![trigger entry.days.at(day)]
                base.at(entry.mode@, day).high + entry.days.at(day).high >= 0
                    && base.at(entry.mode@, day).low + entry.days.at(day).low >= 0,
            forall|day: i32| #![trigger next.at(entry.mode@, day)] next.at(entry.mode@, day) == base.at(entry.mode@, day),
            existing matches Some(days) ==> days.wf()
                && forall|day: i32| #![trigger days.at(day)] days.at(day) == base.at(entry.mode@, day),
            existing.is_none() ==> forall|day: i32| #![trigger base.at(entry.mode@, day)]
                base.at(entry.mode@, day).high == 0 && base.at(entry.mode@, day).low == 0,
            patch.values@.dom() == entry.days.keys@.take(index as int).to_set().filter(|day: i32|
                entry.days.at(day).high != 0 || entry.days.at(day).low != 0),
            forall|day: i32| #![trigger patch.values@[day]] patch.values@.contains_key(day) ==>
                patch.at(day).nonnegative() && patch.at(day).high == base.at(entry.mode@, day).high + entry.days.at(day).high
                    && patch.at(day).low == base.at(entry.mode@, day).low + entry.days.at(day).low,
        decreases entry.days.keys@.len() - index,
    {
        let day = entry.days.keys[index];
        let change = entry.days.get(day);
        if !change.is_zero() {
            let before = match &existing { Some(days) => days.get(day), None => Tally::zero() };
            let after = before.add(&change);
            proof {
                assert(before@ == base.at(entry.mode@, day));
                assert(change@ == entry.days.at(day));
            }
            patch.put(day, after);
            proof {
                assert(patch.at(day).nonnegative());
                assert(patch.values@.contains_key(day));
            }
        }
        proof {
            entry.days.keys@.take(index as int).to_set_ensures();
            entry.days.keys@.take(index as int + 1).to_set_ensures();
            assert(entry.days.keys@.take(index as int + 1) =~= entry.days.keys@.take(index as int).push(day));
            assert(patch.values@.dom() =~= entry.days.keys@.take(index as int + 1).to_set().filter(|date: i32|
                entry.days.at(date).high != 0 || entry.days.at(date).low != 0));
        }
        index += 1;
    }
    proof {
        entry.days.keys@.to_set_ensures();
        assert(entry.days.keys@.take(index as int) =~= entry.days.keys@);
    }
    if patch.keys.len() == 0 {
        proof {
            patch.keys@.to_set_ensures();
            assert forall|day: i32| #![trigger entry.days.at(day)] entry.days.at(day).high == 0 && entry.days.at(day).low == 0 by {
                entry.days.keys@.to_set_ensures();
                if entry.days.values@.contains_key(day) {
                    assert(entry.days.keys@.contains(day));
                    assert(patch.values@.contains_key(day) == (entry.days.at(day).high != 0 || entry.days.at(day).low != 0));
                }
            }
        }
        return;
    }
    let days = match existing {
        Some(days) => days.replace(patch),
        None => {
            let mut clean = Sparse::new();
            index = 0;
            while index < patch.keys.len()
                invariant index <= patch.keys@.len(), clean.wf(), patch.wf(), entry.days.wf(), base.wf(),
                    *next == *old(next), next.wf(),
                    forall|day: i32| #![trigger next.at(entry.mode@, day)] next.at(entry.mode@, day) == base.at(entry.mode@, day),
                    forall|day: i32| #![trigger base.at(entry.mode@, day)] base.at(entry.mode@, day).high == 0 && base.at(entry.mode@, day).low == 0,
                    patch.values@.dom() == entry.days.values@.dom().filter(|day: i32| entry.days.at(day).high != 0 || entry.days.at(day).low != 0),
                    forall|day: i32| #![trigger patch.values@[day]] patch.values@.contains_key(day) ==> patch.at(day).nonnegative()
                        && patch.at(day).high == entry.days.at(day).high && patch.at(day).low == entry.days.at(day).low,
                    clean.values@.dom() == patch.keys@.take(index as int).to_set().filter(|day: i32| patch.at(day).positive()),
                    forall|day: i32| #![trigger clean.values@[day]] clean.values@.contains_key(day) ==> clean.at(day) == patch.at(day) && clean.at(day).positive(),
                decreases patch.keys@.len() - index,
            {
                let day = patch.keys[index];
                let value = patch.get(day);
                if !value.is_zero() { clean.put(day, value); }
                proof {
                    patch.keys@.take(index as int).to_set_ensures();
                    patch.keys@.take(index as int + 1).to_set_ensures();
                    assert(patch.keys@.take(index as int + 1) =~= patch.keys@.take(index as int).push(day));
                    assert(clean.values@.dom() =~= patch.keys@.take(index as int + 1).to_set().filter(|date: i32| patch.at(date).positive()));
                }
                index += 1;
            }
            proof {
                patch.keys@.to_set_ensures();
                assert(patch.keys@.take(index as int) =~= patch.keys@);
                assert forall|day: i32| #![trigger clean.at(day)] clean.at(day).high == entry.days.at(day).high
                    && clean.at(day).low == entry.days.at(day).low by {}
            }
            Layers::from_base(clean)
        },
    };
    proof {
        assert forall|day: i32| #![trigger days.at(day)] days.at(day).high == base.at(entry.mode@, day).high + entry.days.at(day).high
            && days.at(day).low == base.at(entry.mode@, day).low + entry.days.at(day).low by {}
    }
    next.install(entry.mode.clone(), Arc::new(days));
}

fn publish(base: &Arc<Directory>, changes: &Cells) -> (next: Arc<Directory>)
    requires base.wf(), changes.wf(),
        forall|mode: Seq<u8>, day: i32| #![trigger changes.at(mode, day)]
            base.at(mode, day).high + changes.at(mode, day).high >= 0
                && base.at(mode, day).low + changes.at(mode, day).low >= 0,
    ensures next.wf()
        && forall|mode: Seq<u8>, day: i32| #![trigger next.at(mode, day)]
            next.at(mode, day).high == base.at(mode, day).high + changes.at(mode, day).high
                && next.at(mode, day).low == base.at(mode, day).low + changes.at(mode, day).low,
{
    if !changes.has_nonzero() {
        proof {
            assert forall|mode: Seq<u8>, day: i32| #![trigger changes.at(mode, day)] changes.at(mode, day).high == 0 && changes.at(mode, day).low == 0 by {
                assert(!changes.active_mode(mode));
                changes.lemma_inactive(mode, day);
            }
        }
        return base.clone();
    }
    let mut next = base.share();
    let mut hash_index = 0;
    while hash_index < changes.hashes.len()
        invariant hash_index <= changes.hashes@.len(), changes.wf(), base.wf(), next.wf(),
            forall|mode: Seq<u8>, day: i32| #![trigger changes.at(mode, day)]
            base.at(mode, day).high + changes.at(mode, day).high >= 0
                && base.at(mode, day).low + changes.at(mode, day).low >= 0,
            forall|mode: Seq<u8>, day: i32| #![trigger next.at(mode, day)]
                next.at(mode, day).high == base.at(mode, day).high
                    + if changes.mode_prefix(hash_index as nat, 0).contains(mode) { changes.at(mode, day).high as int } else { 0 }
                && next.at(mode, day).low == base.at(mode, day).low
                    + if changes.mode_prefix(hash_index as nat, 0).contains(mode) { changes.at(mode, day).low as int } else { 0 },
        decreases changes.hashes@.len() - hash_index,
    {
        let code = changes.hashes[hash_index];
        match changes.buckets.get(&code) {
            Some(bucket) => {
                let mut mode_index = 0;
                while mode_index < bucket.len()
                    invariant mode_index <= bucket@.len(), changes.wf(), base.wf(), next.wf(), hash_index < changes.hashes@.len(),
                        forall|mode: Seq<u8>, day: i32| #![trigger changes.at(mode, day)]
            base.at(mode, day).high + changes.at(mode, day).high >= 0
                && base.at(mode, day).low + changes.at(mode, day).low >= 0,
                        code == changes.hashes@[hash_index as int], bucket@ == changes.buckets@[code]@,
                        forall|mode: Seq<u8>, day: i32| #![trigger next.at(mode, day)]
                            next.at(mode, day).high == base.at(mode, day).high
                                + if changes.mode_prefix(hash_index as nat, mode_index as int).contains(mode) { changes.at(mode, day).high as int } else { 0 }
                            && next.at(mode, day).low == base.at(mode, day).low
                                + if changes.mode_prefix(hash_index as nat, mode_index as int).contains(mode) { changes.at(mode, day).low as int } else { 0 },
                    decreases bucket@.len() - mode_index,
                {
                    let entry = &bucket[mode_index];
                    proof {
                        changes.lemma_mode_fresh(hash_index as nat, mode_index as int);
                        assert forall|day: i32| #![trigger changes.at(entry.mode@, day)]
                            changes.at(entry.mode@, day) == entry.days.at(day) by { changes.lemma_index(code, mode_index as int, day); }
                        assert forall|day: i32| #![trigger entry.days.at(day)]
                            base.at(entry.mode@, day).high + entry.days.at(day).high >= 0
                                && base.at(entry.mode@, day).low + entry.days.at(day).low >= 0 by {
                            assert(changes.at(entry.mode@, day) == entry.days.at(day));
                            assert(base.at(entry.mode@, day).high + changes.at(entry.mode@, day).high >= 0);
                            assert(base.at(entry.mode@, day).low + changes.at(entry.mode@, day).low >= 0);
                        }
                    }
                    publish_mode(base, &mut next, entry);
                    proof { changes.lemma_mode_step(hash_index as nat, mode_index as int); }
                    mode_index += 1;
                }
                proof { changes.lemma_hash_step(hash_index as nat); }
            },
            None => {},
        }
        hash_index += 1;
    }
    proof {
        assert forall|mode: Seq<u8>, day: i32| #![trigger next.at(mode, day)]
            next.at(mode, day).high == base.at(mode, day).high + changes.at(mode, day).high
                && next.at(mode, day).low == base.at(mode, day).low + changes.at(mode, day).low by {
            if !changes.modes(changes.hashes@.len()).contains(mode) { changes.lemma_mode_complete(mode, day); }
        }
    }
    Arc::new(next)
}

pub fn build(
    high_orders: &[OrderKeyInput], low_orders: &[OrderKeyInput],
    qualifying_lines: Vec<QualifyingLineInput>, Ghost(db): Ghost<Db>,
) -> (r: Option<State>)
    requires scan_high_orders_ok(db, high_orders@), scan_low_orders_ok(db, low_orders@),
        scan_qualifying_lines_ok(db, qualifying_lines@),
    ensures r matches Some(s) ==> s.db() == db, r is Some,
{
    let orders = fold_orders(high_orders, low_orders);
    let mut cells = Cells::new();
    let mut index = 0;
    let mut processed = Ghost(0int);
    while index < qualifying_lines.len()
        invariant index <= qualifying_lines@.len(), processed@ == index as int, cells.wf(),
            forall|mode: Seq<u8>, day: i32| #![trigger cells.at(mode, day)]
                cells.at(mode, day).high == joined_scan(qualifying_lines@.take(processed@), orders, mode, day, true)
                    && cells.at(mode, day).low == joined_scan(qualifying_lines@.take(processed@), orders, mode, day, false),
        decreases qualifying_lines@.len() - index,
    {
        let row = &qualifying_lines[index];
        let weight = orders.get(row.l_orderkey);
        if !weight.is_zero() {
            cells.add(&row.l_shipmode, row.l_receiptdate, &weight);
        }
        proof {
            assert forall|mode: Seq<u8>, day: i32| #![trigger cells.at(mode, day)]
                cells.at(mode, day).high == joined_scan(qualifying_lines@.take(index as int + 1), orders, mode, day, true)
                    && cells.at(mode, day).low == joined_scan(qualifying_lines@.take(index as int + 1), orders, mode, day, false) by {
                lemma_sum_take(qualifying_lines@, index as int, joined_weight(orders, mode, day, true));
                lemma_sum_take(qualifying_lines@, index as int, joined_weight(orders, mode, day, false));
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    proof { assert(qualifying_lines@.take(processed@) =~= qualifying_lines@); }
    let empty = Arc::new(Directory::new());
    proof {
        assert forall|mode: Seq<u8>, day: i32| #![trigger cells.at(mode, day)]
            empty.at(mode, day).high + cells.at(mode, day).high >= 0 && empty.at(mode, day).low + cells.at(mode, day).low >= 0 by {
            crate::semantics::lemma_build_cell(db, high_orders@, low_orders@, qualifying_lines@, orders, mode, day as int, true);
            crate::semantics::lemma_build_cell(db, high_orders@, low_orders@, qualifying_lines@, orders, mode, day as int, false);
            crate::semantics::lemma_cell_count_nonneg(db, mode, day as int, true);
            crate::semantics::lemma_cell_count_nonneg(db, mode, day as int, false);
        }
    }
    let directory = publish(&empty, &cells);
    proof {
        assert forall|mode: Seq<u8>, day: int| #![trigger cell_count(db, mode, day, true)]
            cell_count(db, mode, day, true) == if i32::MIN <= day <= i32::MAX { directory.at(mode, day as i32).high as int } else { 0 } by {
            crate::semantics::lemma_build_cell(db, high_orders@, low_orders@, qualifying_lines@, orders, mode, day, true);
        }
        assert forall|mode: Seq<u8>, day: int| #![trigger cell_count(db, mode, day, false)]
            cell_count(db, mode, day, false) == if i32::MIN <= day <= i32::MAX { directory.at(mode, day as i32).low as int } else { 0 } by {
            crate::semantics::lemma_build_cell(db, high_orders@, low_orders@, qualifying_lines@, orders, mode, day, false);
        }
    }
    Some(State { directory, database: Ghost(db) })
}

pub open spec fn rows_model(rows: Seq<(Vec<u8>, Tally)>) -> Seq<OutRow> {
    rows.map_values(|row: (Vec<u8>, Tally)| OutRow { l_shipmode: row.0@, high_line_count: row.1@.high, low_line_count: row.1@.low })
}

fn emit(directory: &Directory, mode: &[u8], start: i32, end: i32) -> (out: Option<(Vec<u8>, Tally)>)
    requires directory.wf(),
    ensures out matches Some(row) ==> row.0@ == mode@
        && row.1@.nonnegative() && (row.1@.high > 0 || row.1@.low > 0)
        && row.1@.high == directory.range_count(mode@, start as int, end as int, true)
        && row.1@.low == directory.range_count(mode@, start as int, end as int, false),
        out is None ==> directory.range_count(mode@, start as int, end as int, true) == 0
            && directory.range_count(mode@, start as int, end as int, false) == 0,
{
    let days = match directory.find(mode) {
        Some(value) => value,
        None => {
            proof {
                directory.lemma_range_missing(mode@, start as int, end as int, true);
                directory.lemma_range_missing(mode@, start as int, end as int, false);
            }
            return None;
        },
    };
    let total = days.range(start, end);
    proof {
        let ghost days_model = *days;
        directory.lemma_range_mode(mode@, start as int, end as int, days_model, true);
        directory.lemma_range_mode(mode@, start as int, end as int, days_model, false);
    }
    if total.is_zero() { return None; }
    Some((bytes::copy(mode), total))
}

fn narrow(rows: &Vec<(Vec<u8>, Tally)>) -> (out: Option<Vec<(Vec<u8>, i64, i64)>>)
    ensures out matches Some(narrowed) ==> out_view(narrowed@) == rows_model(rows@),
        out is None ==> exists|index: int| 0 <= index < rows@.len() && !out_fits(rows_model(rows@)[index]),
{
    let mut out: Vec<(Vec<u8>, i64, i64)> = Vec::new();
    let mut index = 0;
    while index < rows.len()
        invariant index <= rows@.len(), out_view(out@) == rows_model(rows@).take(index as int),
        decreases rows@.len() - index,
    {
        let row = &rows[index];
        let high = row.1.high.to_i128();
        let low = row.1.low.to_i128();
        match (high, low) {
            (Some(high), Some(low)) if i64::MIN as i128 <= high && high <= i64::MAX as i128
                && i64::MIN as i128 <= low && low <= i64::MAX as i128 => {
                let ghost previous = out@;
                out.push((bytes::copy(&row.0), high as i64, low as i64));
                proof {
                    let model = rows_model(rows@);
                    assert(out@ =~= previous.push(out@.last()));
                    assert(out_view(out@) =~= out_view(previous).push(out_view(out@).last()));
                    assert(out_view(out@).last() == model[index as int]);
                    assert(model.take(index as int + 1) =~= model.take(index as int).push(model[index as int]));
                    assert(out_view(out@) =~= model.take(index as int + 1));
                }
            },
            _ => {
                proof { assert(!out_fits(rows_model(rows@)[index as int])); }
                return None;
            },
        }
        index += 1;
    }
    proof { assert(rows_model(rows@).take(index as int) =~= rows_model(rows@)); }
    Some(out)
}

pub fn execute(s: &State, p1: &[u8], p2: &[u8], p3: i32, p4: i32)
    -> (r: Option<Vec<(Vec<u8>, i64, i64)>>)
    ensures r matches Some(out) ==> query_result(s.db(), params(p1@, p2@, p3 as int, p4 as int), out_view(out@)),
        answer_fits(s.db(), params(p1@, p2@, p3 as int, p4 as int)) ==> r is Some,
{
    proof { use_type_invariant(s); }
    let ghost parameters = params(p1@, p2@, p3 as int, p4 as int);
    let mut rows: Vec<(Vec<u8>, Tally)> = Vec::new();
    if p4 <= p3 {
        proof {
            assert forall|mode: Seq<u8>| #![trigger group_count(s.db(), parameters, mode)] group_count(s.db(), parameters, mode) == 0 by {
                s.lemma_group_range(parameters, mode);
            }
            crate::semantics::lemma_execute_result(s.db(), parameters, rows_model(rows@));
            assert(out_view(Seq::<(Vec<u8>, i64, i64)>::empty()) =~= rows_model(rows@));
        }
        return Some(Vec::new());
    }
    let (first, second) = if bytes::less(p2, p1) { (p2, p1) } else { (p1, p2) };
    let first_row = emit(&s.directory, first, p3, p4);
    let ghost first_model = first_row;
    match first_row { Some(row) => rows.push(row), None => {}, }
    let ghost after_first = rows@;
    let ghost mut second_model: Option<(Vec<u8>, Tally)> = None;
    if !bytes::equal(first, second) {
        let second_row = emit(&s.directory, second, p3, p4);
        proof { second_model = second_row; }
        match second_row { Some(row) => rows.push(row), None => {}, }
    }
    proof {
        verus_common::bytes::lemma_bytes_lt_asym(first@, second@);
        assert(!verus_common::bytes::bytes_lt(second@, first@));
        assert forall|index: int| #![trigger rows_model(rows@)[index]] 0 <= index < rows@.len() implies {
            let row = rows_model(rows@)[index];
            row == row_of(s.db(), parameters, row.l_shipmode) && row.high_line_count + row.low_line_count > 0
        } by {
            s.lemma_group_range(parameters, rows_model(rows@)[index].l_shipmode);
        }
        assert forall|left: int, right: int| #![trigger rows_model(rows@)[left], rows_model(rows@)[right]]
            0 <= left < right < rows@.len() implies verus_common::bytes::bytes_lt(rows_model(rows@)[left].l_shipmode, rows_model(rows@)[right].l_shipmode) by {
            verus_common::bytes::lemma_bytes_lt_total(first@, second@);
        }
        assert forall|mode: Seq<u8>| #![trigger group_count(s.db(), parameters, mode)] group_count(s.db(), parameters, mode) > 0 implies
            exists|index: int| #![trigger rows_model(rows@)[index]] 0 <= index < rows@.len() && rows_model(rows@)[index].l_shipmode == mode by {
            s.lemma_group_range(parameters, mode);
            if mode == first@ {
                assert(first_model.is_some());
                assert(rows_model(rows@)[0].l_shipmode == mode);
            } else {
                assert(mode == second@);
                assert(second_model.is_some());
                assert(rows_model(rows@)[after_first.len() as int].l_shipmode == mode);
            }
        }
        crate::semantics::lemma_execute_result(s.db(), parameters, rows_model(rows@));
    }
    match narrow(&rows) {
        Some(out) => Some(out),
        None => {
            proof {
                if answer_fits(s.db(), parameters) {
                    assert(query_result(s.db(), parameters, rows_model(rows@)));
                }
            }
            None
        },
    }
}

pub open spec fn extend_distinct(acc: Seq<i64>, keys: Seq<i64>) -> Seq<i64>
    decreases keys.len(),
{
    if keys.len() == 0 { acc } else {
        let before = extend_distinct(acc, keys.drop_last());
        if before.contains(keys.last()) { before } else { before.push(keys.last()) }
    }
}

pub open spec fn order_keys(rows: Seq<OrderDelta>) -> Seq<i64> {
    rows.map_values(|row: OrderDelta| row.o_orderkey)
}

pub open spec fn line_keys(rows: Seq<LineDelta>) -> Seq<i64> {
    rows.map_values(|row: LineDelta| row.row.l_orderkey)
}

/// The keys whose rows are rescanned: each key of a delta row, once, orders first.
pub open spec fn wanted_keys(lineitem_delta: Seq<LineDelta>, orders_delta: Seq<OrderDelta>) -> Seq<i64> {
    extend_distinct(extend_distinct(Seq::empty(), order_keys(orders_delta)), line_keys(lineitem_delta))
}

pub open spec fn rescan_high_orders_for_keys_wanted(s: State, lineitem_delta: Seq<LineDelta>, orders_delta: Seq<OrderDelta>) -> Seq<i64> {
    wanted_keys(lineitem_delta, orders_delta)
}

pub open spec fn rescan_low_orders_for_keys_wanted(s: State, lineitem_delta: Seq<LineDelta>, orders_delta: Seq<OrderDelta>) -> Seq<i64> {
    wanted_keys(lineitem_delta, orders_delta)
}

pub open spec fn rescan_qualifying_lines_for_keys_wanted(s: State, lineitem_delta: Seq<LineDelta>, orders_delta: Seq<OrderDelta>) -> Seq<i64> {
    wanted_keys(lineitem_delta, orders_delta)
}

proof fn lemma_extend_distinct(acc: Seq<i64>, keys: Seq<i64>)
    ensures acc.no_duplicates() ==> extend_distinct(acc, keys).no_duplicates(),
        forall|key: i64| #![trigger extend_distinct(acc, keys).contains(key)]
            extend_distinct(acc, keys).contains(key) <==> acc.contains(key) || keys.contains(key),
    decreases keys.len(),
{
    if keys.len() > 0 {
        let init = keys.drop_last();
        lemma_extend_distinct(acc, init);
        let before = extend_distinct(acc, init);
        if !before.contains(keys.last()) && acc.no_duplicates() {
            verus_prooflib::lemma_unique_push(before, keys.last());
        }
        assert forall|key: i64| #![trigger extend_distinct(acc, keys).contains(key)]
            extend_distinct(acc, keys).contains(key) <==> acc.contains(key) || keys.contains(key) by {
            if keys.contains(key) && key != keys.last() {
                let index = choose|index: int| 0 <= index < keys.len() && keys[index] == key;
                assert(init[index] == key);
            }
            if init.contains(key) {
                let index = choose|index: int| 0 <= index < init.len() && init[index] == key;
                assert(keys[index] == key);
            }
        }
    }
}

pub proof fn lemma_wanted(lineitem_delta: Seq<LineDelta>, orders_delta: Seq<OrderDelta>)
    ensures wanted_keys(lineitem_delta, orders_delta).no_duplicates(),
        forall|key: i64| #![trigger wanted_keys(lineitem_delta, orders_delta).contains(key)]
            !wanted_keys(lineitem_delta, orders_delta).contains(key) ==>
                moments::order_change(orders_delta, key, true) == 0 && moments::order_change(orders_delta, key, false) == 0
                && forall|mode: Seq<u8>, day: i32| #![trigger moments::line_change(lineitem_delta, key, mode, day)]
                    moments::line_change(lineitem_delta, key, mode, day) == 0,
{
    let ordered = extend_distinct(Seq::empty(), order_keys(orders_delta));
    lemma_extend_distinct(Seq::empty(), order_keys(orders_delta));
    lemma_extend_distinct(ordered, line_keys(lineitem_delta));
    assert forall|key: i64| #![trigger wanted_keys(lineitem_delta, orders_delta).contains(key)]
        !wanted_keys(lineitem_delta, orders_delta).contains(key) implies
            moments::order_change(orders_delta, key, true) == 0 && moments::order_change(orders_delta, key, false) == 0
            && forall|mode: Seq<u8>, day: i32| #![trigger moments::line_change(lineitem_delta, key, mode, day)]
                moments::line_change(lineitem_delta, key, mode, day) == 0 by {
        assert forall|index: int| #![trigger orders_delta[index]] 0 <= index < orders_delta.len() implies orders_delta[index].o_orderkey != key by {
            assert(order_keys(orders_delta)[index] == orders_delta[index].o_orderkey);
        }
        assert forall|index: int| #![trigger lineitem_delta[index]] 0 <= index < lineitem_delta.len() implies lineitem_delta[index].row.l_orderkey != key by {
            assert(line_keys(lineitem_delta)[index] == lineitem_delta[index].row.l_orderkey);
        }
        verus_common::sums::lemma_sum_zero(orders_delta, |row: OrderDelta| if row.o_orderkey == key && (row.is_high != 0) == true { row.sign as int } else { 0 });
        verus_common::sums::lemma_sum_zero(orders_delta, |row: OrderDelta| if row.o_orderkey == key && (row.is_high != 0) == false { row.sign as int } else { 0 });
        assert forall|mode: Seq<u8>, day: i32| #![trigger moments::line_change(lineitem_delta, key, mode, day)]
            moments::line_change(lineitem_delta, key, mode, day) == 0 by {
            verus_common::sums::lemma_sum_zero(lineitem_delta, moments::line_change_weight(key, mode, day));
        }
    }
}

fn plan(lineitem_delta: &Vec<LineDelta>, orders_delta: &[OrderDelta]) -> (out: Vec<i64>)
    ensures out@ == wanted_keys(lineitem_delta@, orders_delta@),
{
    let mut out: Vec<i64> = Vec::new();
    let mut seen: HashMap<i64, bool> = HashMap::new();
    let mut index = 0;
    while index < orders_delta.len()
        invariant index <= orders_delta@.len(), seen@.dom() == out@.to_set(),
            out@ == extend_distinct(Seq::empty(), order_keys(orders_delta@).take(index as int)),
        decreases orders_delta.len() - index,
    {
        let key = orders_delta[index].o_orderkey;
        proof {
            assert(order_keys(orders_delta@).take(index as int + 1).drop_last() =~= order_keys(orders_delta@).take(index as int));
            assert(order_keys(orders_delta@).take(index as int + 1).last() == key);
            out@.to_set_ensures();
        }
        if !seen.contains_key(&key) {
            proof { lemma_push_to_set_i64(out@, key); }
            seen.insert(key, true);
            out.push(key);
        }
        index += 1;
    }
    proof { assert(order_keys(orders_delta@).take(index as int) =~= order_keys(orders_delta@)); }
    let ghost ordered = out@;
    let mut index = 0;
    while index < lineitem_delta.len()
        invariant index <= lineitem_delta@.len(), seen@.dom() == out@.to_set(),
            out@ == extend_distinct(ordered, line_keys(lineitem_delta@).take(index as int)),
        decreases lineitem_delta.len() - index,
    {
        let key = lineitem_delta[index].row.l_orderkey;
        proof {
            assert(line_keys(lineitem_delta@).take(index as int + 1).drop_last() =~= line_keys(lineitem_delta@).take(index as int));
            assert(line_keys(lineitem_delta@).take(index as int + 1).last() == key);
            out@.to_set_ensures();
        }
        if !seen.contains_key(&key) {
            proof { lemma_push_to_set_i64(out@, key); }
            seen.insert(key, true);
            out.push(key);
        }
        index += 1;
    }
    proof { assert(line_keys(lineitem_delta@).take(index as int) =~= line_keys(lineitem_delta@)); }
    out
}

proof fn lemma_push_to_set_i64(keys: Seq<i64>, key: i64)
    ensures keys.push(key).to_set() == keys.to_set().insert(key),
{
    verus_prooflib::lemma_push_to_set(keys, key);
}

pub fn rescan_high_orders_for_keys_keys(s: &State, lineitem_delta: &Vec<LineDelta>, orders_delta: &[OrderDelta]) -> (r: Vec<i64>)
    ensures r@ == rescan_high_orders_for_keys_wanted(*s, lineitem_delta@, orders_delta@),
{
    plan(lineitem_delta, orders_delta)
}

pub fn rescan_low_orders_for_keys_keys(s: &State, lineitem_delta: &Vec<LineDelta>, orders_delta: &[OrderDelta]) -> (r: Vec<i64>)
    ensures r@ == rescan_low_orders_for_keys_wanted(*s, lineitem_delta@, orders_delta@),
{
    plan(lineitem_delta, orders_delta)
}

pub fn rescan_qualifying_lines_for_keys_keys(s: &State, lineitem_delta: &Vec<LineDelta>, orders_delta: &[OrderDelta]) -> (r: Vec<i64>)
    ensures r@ == rescan_qualifying_lines_for_keys_wanted(*s, lineitem_delta@, orders_delta@),
{
    plan(lineitem_delta, orders_delta)
}

fn accumulate_mode(patch: &mut Cells, entry: &moments::CellMode, factor: &Tally)
    requires old(patch).wf(), entry.days.wf(),
    ensures final(patch).wf(),
        forall|mode: Seq<u8>, day: i32| #![trigger final(patch).at(mode, day)]
            final(patch).at(mode, day).high == old(patch).at(mode, day).high
                + if mode == entry.mode@ { factor@.high * entry.days.at(day).high } else { 0 }
            && final(patch).at(mode, day).low == old(patch).at(mode, day).low
                + if mode == entry.mode@ { factor@.low * entry.days.at(day).high } else { 0 },
{
    let ghost before = *patch;
    let mut index = 0;
    while index < entry.days.keys.len()
        invariant index <= entry.days.keys@.len(), patch.wf(), entry.days.wf(), before == *old(patch),
            forall|mode: Seq<u8>, day: i32| #![trigger patch.at(mode, day)]
                patch.at(mode, day).high == before.at(mode, day).high
                    + if mode == entry.mode@ && entry.days.keys@.take(index as int).contains(day) { factor@.high * entry.days.at(day).high } else { 0 }
                && patch.at(mode, day).low == before.at(mode, day).low
                    + if mode == entry.mode@ && entry.days.keys@.take(index as int).contains(day) { factor@.low * entry.days.at(day).high } else { 0 },
        decreases entry.days.keys@.len() - index,
    {
        let day = entry.days.keys[index];
        let value = entry.days.get(day).high;
        let change = factor.scale(&value);
        proof {
            verus_prooflib::lemma_prefix_fresh(entry.days.keys@, index as int);
        }
        if !change.is_zero() { patch.add(&entry.mode, day, &change); }
        proof {
            assert(entry.days.keys@.take(index as int + 1) =~= entry.days.keys@.take(index as int).push(day));
        }
        index += 1;
    }
    proof {
        entry.days.keys@.to_set_ensures();
        assert(entry.days.keys@.take(index as int) =~= entry.days.keys@);
        assert forall|mode: Seq<u8>, day: i32| #![trigger patch.at(mode, day)]
            patch.at(mode, day).high == before.at(mode, day).high + if mode == entry.mode@ { factor@.high * entry.days.at(day).high } else { 0 }
            && patch.at(mode, day).low == before.at(mode, day).low + if mode == entry.mode@ { factor@.low * entry.days.at(day).high } else { 0 } by {
            assert(entry.days.keys@.take(index as int).contains(day) == entry.days.keys@.contains(day));
            if !entry.days.keys@.contains(day) {
                assert(!entry.days.values@.contains_key(day));
                assert(entry.days.at(day).high == 0);
            }
            assert(patch.at(mode, day).high == before.at(mode, day).high
                + if mode == entry.mode@ && entry.days.keys@.take(index as int).contains(day) { factor@.high * entry.days.at(day).high } else { 0 });
        }
    }
}

fn accumulate_cells(patch: &mut Cells, cells: &Cells, factor: &Tally)
    requires old(patch).wf(), cells.wf(),
    ensures final(patch).wf(),
        forall|mode: Seq<u8>, day: i32| #![trigger final(patch).at(mode, day)]
            final(patch).at(mode, day).high == old(patch).at(mode, day).high + factor@.high * cells.at(mode, day).high
            && final(patch).at(mode, day).low == old(patch).at(mode, day).low + factor@.low * cells.at(mode, day).high,
{
    let ghost before = *patch;
    let mut hash_index = 0;
    while hash_index < cells.hashes.len()
        invariant hash_index <= cells.hashes@.len(), cells.wf(), patch.wf(), before == *old(patch),
            forall|mode: Seq<u8>, day: i32| #![trigger patch.at(mode, day)]
                patch.at(mode, day).high == before.at(mode, day).high
                    + if cells.mode_prefix(hash_index as nat, 0).contains(mode) { factor@.high * cells.at(mode, day).high } else { 0 }
                && patch.at(mode, day).low == before.at(mode, day).low
                    + if cells.mode_prefix(hash_index as nat, 0).contains(mode) { factor@.low * cells.at(mode, day).high } else { 0 },
        decreases cells.hashes@.len() - hash_index,
    {
        let code = cells.hashes[hash_index];
        match cells.buckets.get(&code) {
            Some(bucket) => {
                let mut mode_index = 0;
                while mode_index < bucket.len()
                    invariant mode_index <= bucket@.len(), hash_index < cells.hashes@.len(),
                        cells.wf(), patch.wf(), before == *old(patch),
                        code == cells.hashes@[hash_index as int], bucket@ == cells.buckets@[code]@,
                        forall|mode: Seq<u8>, day: i32| #![trigger patch.at(mode, day)]
                            patch.at(mode, day).high == before.at(mode, day).high
                                + if cells.mode_prefix(hash_index as nat, mode_index as int).contains(mode) { factor@.high * cells.at(mode, day).high } else { 0 }
                            && patch.at(mode, day).low == before.at(mode, day).low
                                + if cells.mode_prefix(hash_index as nat, mode_index as int).contains(mode) { factor@.low * cells.at(mode, day).high } else { 0 },
                    decreases bucket@.len() - mode_index,
                {
                    let entry = &bucket[mode_index];
                    proof {
                        cells.lemma_mode_fresh(hash_index as nat, mode_index as int);
                        assert forall|day: i32| #![trigger cells.at(entry.mode@, day)]
                            cells.at(entry.mode@, day) == entry.days.at(day) by { cells.lemma_index(code, mode_index as int, day); }
                    }
                    accumulate_mode(patch, entry, factor);
                    proof { cells.lemma_mode_step(hash_index as nat, mode_index as int); }
                    mode_index += 1;
                }
                proof { cells.lemma_hash_step(hash_index as nat); }
            },
            None => { proof { cells.hashes@.to_set_ensures(); assert(false); } },
        }
        hash_index += 1;
    }
    proof {
        assert forall|mode: Seq<u8>, day: i32| #![trigger patch.at(mode, day)]
            patch.at(mode, day).high == before.at(mode, day).high + factor@.high * cells.at(mode, day).high
            && patch.at(mode, day).low == before.at(mode, day).low + factor@.low * cells.at(mode, day).high by {
            if !cells.modes(cells.hashes@.len()).contains(mode) { cells.lemma_mode_complete(mode, day); }
        }
    }
}

fn maintain_key(patch: &mut Cells, post: &Cells, delta: &Cells, post_orders: &Tally, delta_orders: &Tally)
    requires old(patch).wf(), post.wf(), delta.wf(),
    ensures final(patch).wf(),
        forall|mode: Seq<u8>, day: i32| #![trigger final(patch).at(mode, day)]
            final(patch).at(mode, day).high == old(patch).at(mode, day).high
                + delta_orders@.high * (post.at(mode, day).high - delta.at(mode, day).high)
                + post_orders@.high * delta.at(mode, day).high
            && final(patch).at(mode, day).low == old(patch).at(mode, day).low
                + delta_orders@.low * (post.at(mode, day).high - delta.at(mode, day).high)
                + post_orders@.low * delta.at(mode, day).high,
{
    let old_orders = post_orders.sub(delta_orders);
    let ghost before = *patch;
    accumulate_cells(patch, post, delta_orders);
    accumulate_cells(patch, delta, &old_orders);
    proof {
        assert forall|mode: Seq<u8>, day: i32| #![trigger patch.at(mode, day)]
            patch.at(mode, day).high == before.at(mode, day).high
                + delta_orders@.high * (post.at(mode, day).high - delta.at(mode, day).high) + post_orders@.high * delta.at(mode, day).high
            && patch.at(mode, day).low == before.at(mode, day).low
                + delta_orders@.low * (post.at(mode, day).high - delta.at(mode, day).high) + post_orders@.low * delta.at(mode, day).high by {
            assert(delta_orders@.high * post.at(mode, day).high + old_orders@.high * delta.at(mode, day).high
                == delta_orders@.high * (post.at(mode, day).high - delta.at(mode, day).high) + post_orders@.high * delta.at(mode, day).high) by (nonlinear_arith)
                requires old_orders@.high == post_orders@.high - delta_orders@.high;
            assert(delta_orders@.low * post.at(mode, day).high + old_orders@.low * delta.at(mode, day).high
                == delta_orders@.low * (post.at(mode, day).high - delta.at(mode, day).high) + post_orders@.low * delta.at(mode, day).high) by (nonlinear_arith)
                requires old_orders@.low == post_orders@.low - delta_orders@.low;
        }
    }
}

pub open spec fn key_effect(db: Db, post_db: Db, key: i64, mode: Seq<u8>, day: i32, high: bool) -> int {
    crate::update_proofs::effect(db, post_db, key as int, mode, day as int, high)
}

pub fn apply(
    s: &State, lineitem_delta: &Vec<LineDelta>, orders_delta: &[OrderDelta],
    high_orders_for_keys_keys: &[i64], high_orders_for_keys_rows: &[OrderKeyInput],
    low_orders_for_keys_keys: &[i64], low_orders_for_keys_rows: &[OrderKeyInput],
    qualifying_lines_for_keys_keys: &[i64], qualifying_lines_for_keys_rows: &Vec<QualifyingLineInput>,
    Ghost(orders_full): Ghost<Seq<(OrdersRow, int)>>, Ghost(db2): Ghost<Db>,
) -> (r: Result<State, u32>)
    requires orders_delta_ok(orders_delta@, orders_full),
        capture_ok(s.db(), db2, line_delta_seq_view(lineitem_delta@), orders_full),
        high_orders_for_keys_keys@ == rescan_high_orders_for_keys_wanted(*s, lineitem_delta@, orders_delta@),
        rescan_high_orders_for_keys_ok(db2, high_orders_for_keys_keys@, high_orders_for_keys_rows@),
        low_orders_for_keys_keys@ == rescan_low_orders_for_keys_wanted(*s, lineitem_delta@, orders_delta@),
        rescan_low_orders_for_keys_ok(db2, low_orders_for_keys_keys@, low_orders_for_keys_rows@),
        qualifying_lines_for_keys_keys@ == rescan_qualifying_lines_for_keys_wanted(*s, lineitem_delta@, orders_delta@),
        rescan_qualifying_lines_for_keys_ok(db2, qualifying_lines_for_keys_keys@, qualifying_lines_for_keys_rows@),
    ensures r matches Ok(s2) ==> s2.db() == db2, r is Ok,
{
    proof { use_type_invariant(s); }
    let delta_orders = moments::order_deltas(orders_delta);
    let delta_lines = moments::line_deltas(lineitem_delta);
    let keys = high_orders_for_keys_keys;
    proof { lemma_wanted(lineitem_delta@, orders_delta@); }
    let post_orders = fold_orders(high_orders_for_keys_rows, low_orders_for_keys_rows);
    let post_lines = fold_lines(qualifying_lines_for_keys_rows);
    proof {
        assert forall|key: i64| #![trigger keys@.contains(key)] keys@.contains(key) implies
            post_orders.at(key).high == order_count(db2, key as int, true)
            && post_orders.at(key).low == order_count(db2, key as int, false) by {
            let offset = choose|offset: int| #![trigger keys@[offset]] 0 <= offset < keys@.len() && keys@[offset] == key;
            assert(high_orders_for_keys_keys@.contains(key));
            assert(low_orders_for_keys_keys@.contains(key));
            crate::update_proofs::lemma_rescan_orders(db2, high_orders_for_keys_keys@, low_orders_for_keys_keys@,
                high_orders_for_keys_rows@, low_orders_for_keys_rows@, key);
        }
        assert forall|key: i64, mode: Seq<u8>, day: i32| #![trigger post_lines.at(key, mode, day)] keys@.contains(key) implies
            post_lines.at(key, mode, day) == line_count(db2, key as int, mode, day as int) by {
            let offset = choose|offset: int| #![trigger keys@[offset]] 0 <= offset < keys@.len() && keys@[offset] == key;
            assert(qualifying_lines_for_keys_keys@.contains(key));
            crate::update_proofs::lemma_rescan_lines(db2, qualifying_lines_for_keys_keys@, qualifying_lines_for_keys_rows@, key, mode, day as int);
        }
    }
    let empty = Cells::new();
    let mut patch = Cells::new();
    let mut index = 0;
    let mut processed = Ghost(0int);
    while index < keys.len()
        invariant index <= keys@.len(), processed@ == index as int, patch.wf(), empty.wf(),
            forall|mode: Seq<u8>, day: i32| #![trigger empty.at(mode, day)] empty.at(mode, day).high == 0 && empty.at(mode, day).low == 0,
            keys@.no_duplicates(),
            rescan_qualifying_lines_for_keys_ok(db2, qualifying_lines_for_keys_keys@, qualifying_lines_for_keys_rows@),
            forall|key: i64| #![trigger keys@.contains(key)] keys@.contains(key) ==>
                qualifying_lines_for_keys_keys@.contains(key),
            keys@ == wanted_keys(lineitem_delta@, orders_delta@),
            post_orders.wf(), post_lines.wf(), delta_orders.wf(), delta_lines.wf(), s.wf(),
            orders_delta_ok(orders_delta@, orders_full), capture_ok(s.db(), db2, line_delta_seq_view(lineitem_delta@), orders_full),
            forall|key: i64| #![trigger post_orders.at(key)] keys@.contains(key) ==>
                post_orders.at(key).high == order_count(db2, key as int, true) && post_orders.at(key).low == order_count(db2, key as int, false),
            forall|key: i64, mode: Seq<u8>, day: i32| #![trigger post_lines.at(key, mode, day)] keys@.contains(key) ==>
                post_lines.at(key, mode, day) == line_count(db2, key as int, mode, day as int),
            forall|key: i64| #![trigger delta_orders.at(key)] delta_orders.at(key).high == moments::order_change(orders_delta@, key, true)
                && delta_orders.at(key).low == moments::order_change(orders_delta@, key, false),
            forall|key: i64, mode: Seq<u8>, day: i32| #![trigger delta_lines.at(key, mode, day)]
                delta_lines.at(key, mode, day) == moments::line_change(lineitem_delta@, key, mode, day),
            forall|mode: Seq<u8>, day: i32| #![trigger patch.at(mode, day)]
                patch.at(mode, day).high == sum(keys@.take(processed@), |key: i64| key_effect(s.db(), db2, key, mode, day, true))
                    && patch.at(mode, day).low == sum(keys@.take(processed@), |key: i64| key_effect(s.db(), db2, key, mode, day, false)),
        decreases keys@.len() - index,
    {
        let key = keys[index];
        let post = match post_lines.profiles.get(&key) { Some(value) => value, None => &empty };
        let delta = match delta_lines.profiles.get(&key) { Some(value) => value, None => &empty };
        proof {
            assert(keys@.contains(key));
            assert(post.wf() && delta.wf());
            assert forall|mode: Seq<u8>, day: i32| #![trigger post.at(mode, day)]
                post.at(mode, day).high == post_lines.at(key, mode, day) && delta.at(mode, day).high == delta_lines.at(key, mode, day) by {}
        }
        maintain_key(&mut patch, post, delta, &post_orders.get(key), &delta_orders.get(key));
        proof {
            assert forall|mode: Seq<u8>, day: i32| #![trigger patch.at(mode, day)]
                patch.at(mode, day).high == sum(keys@.take(index as int + 1), |key: i64| key_effect(s.db(), db2, key, mode, day, true))
                && patch.at(mode, day).low == sum(keys@.take(index as int + 1), |key: i64| key_effect(s.db(), db2, key, mode, day, false)) by {
                crate::update_proofs::lemma_capture_orders(s.db(), db2, orders_delta@, orders_full, key as int, true);
                crate::update_proofs::lemma_capture_orders(s.db(), db2, orders_delta@, orders_full, key as int, false);
                crate::update_proofs::lemma_capture_lines(s.db(), db2, lineitem_delta@, key as int, mode, day as int);
                verus_prooflib::lemma_bilinear_delta(order_count(s.db(), key as int, true), order_count(db2, key as int, true),
                    line_count(s.db(), key as int, mode, day as int), line_count(db2, key as int, mode, day as int));
                verus_prooflib::lemma_bilinear_delta(order_count(s.db(), key as int, false), order_count(db2, key as int, false),
                    line_count(s.db(), key as int, mode, day as int), line_count(db2, key as int, mode, day as int));
                lemma_sum_take(keys@, index as int, |key: i64| key_effect(s.db(), db2, key, mode, day, true));
                lemma_sum_take(keys@, index as int, |key: i64| key_effect(s.db(), db2, key, mode, day, false));
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    proof {
        assert(keys@.take(processed@) =~= keys@);
        assert forall|mode: Seq<u8>, day: int| #![trigger cell_count(db2, mode, day, true)]
            (if i32::MIN <= day <= i32::MAX { s.directory.at(mode, day as i32).high + patch.at(mode, day as i32).high } else { 0 }) == cell_count(db2, mode, day, true)
            && (if i32::MIN <= day <= i32::MAX { s.directory.at(mode, day as i32).low + patch.at(mode, day as i32).low } else { 0 }) == cell_count(db2, mode, day, false) by {
            assert forall|key: i64| #![trigger keys@.contains(key)] !keys@.contains(key) implies
                moments::order_change(orders_delta@, key, true) == 0 && moments::order_change(orders_delta@, key, false) == 0
                && (i32::MIN <= day <= i32::MAX ==> moments::line_change(lineitem_delta@, key, mode, day as i32) == 0) by {
                if i32::MIN <= day <= i32::MAX { assert(delta_lines.at(key, mode, day as i32) == 0); }
            }
            crate::update_proofs::lemma_global_delta(s.db(), db2, orders_delta@, orders_full, lineitem_delta@, keys@, mode, day, true);
            crate::update_proofs::lemma_global_delta(s.db(), db2, orders_delta@, orders_full, lineitem_delta@, keys@, mode, day, false);
            assert(s.at(mode, day).high == cell_count(s.db(), mode, day, true));
            assert(s.at(mode, day).low == cell_count(s.db(), mode, day, false));
            if i32::MIN <= day <= i32::MAX {
                verus_common::sums::lemma_sum_ext_all(keys@, |key: i64| crate::update_proofs::effect(s.db(), db2, key as int, mode, day, true),
                    |key: i64| key_effect(s.db(), db2, key, mode, day as i32, true));
                verus_common::sums::lemma_sum_ext_all(keys@, |key: i64| crate::update_proofs::effect(s.db(), db2, key as int, mode, day, false),
                    |key: i64| key_effect(s.db(), db2, key, mode, day as i32, false));
            } else {
                assert forall|offset: int| #![trigger keys@[offset]] 0 <= offset < keys@.len() implies
                    line_count(db2, keys@[offset] as int, mode, day) == 0 && line_count(s.db(), keys@[offset] as int, mode, day) == 0 by {
                    let key = keys@[offset];
                    assert(qualifying_lines_for_keys_keys@.contains(key));
                    crate::update_proofs::lemma_rescan_lines(db2, qualifying_lines_for_keys_keys@, qualifying_lines_for_keys_rows@, key, mode, day);
                    crate::update_proofs::lemma_capture_lines(s.db(), db2, lineitem_delta@, key as int, mode, day);
                }
                verus_common::sums::lemma_sum_zero(keys@, |key: i64| crate::update_proofs::effect(s.db(), db2, key as int, mode, day, true));
                verus_common::sums::lemma_sum_zero(keys@, |key: i64| crate::update_proofs::effect(s.db(), db2, key as int, mode, day, false));
            }
        }
        assert forall|mode: Seq<u8>, day: i32| #![trigger patch.at(mode, day)]
            s.directory.at(mode, day).high + patch.at(mode, day).high >= 0
                && s.directory.at(mode, day).low + patch.at(mode, day).low >= 0 by {
            crate::semantics::lemma_cell_count_nonneg(db2, mode, day as int, true);
            crate::semantics::lemma_cell_count_nonneg(db2, mode, day as int, false);
        }
    }
    let directory = publish(&s.directory, &patch);
    proof {
        assert forall|mode: Seq<u8>, day: int| #![trigger directory_at(directory, mode, day)]
            directory_at(directory, mode, day).high == cell_count(db2, mode, day, true)
            && directory_at(directory, mode, day).low == cell_count(db2, mode, day, false) by {
            assert((if i32::MIN <= day <= i32::MAX { s.directory.at(mode, day as i32).high + patch.at(mode, day as i32).high } else { 0 }) == cell_count(db2, mode, day, true));
        }
    }
    Ok(State { directory, database: Ghost(db2) })
}

}
