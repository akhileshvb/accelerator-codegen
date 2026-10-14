use vstd::prelude::*;
use std::collections::HashMap;
use verus_common::sums::*;
use crate::gen_obligations::*;
use crate::storage::*;
use crate::maintenance;

verus! {

pub open spec fn selected(orders: Map<i64, bool>, suppliers: Map<i64, bool>, line: LineitemInput, supplier_mode: bool) -> bool {
    if supplier_mode { suppliers.contains_key(line.l_suppkey) && !orders.contains_key(line.l_orderkey) }
    else { orders.contains_key(line.l_orderkey) }
}

pub open spec fn rescan_weight(before: (Table, Table, Table), after: (Table, Table, Table),
    orders: Map<i64, bool>, suppliers: Map<i64, bool>, line: LineitemInput, supplier_mode: bool, nation: i64, day: i32, revenue: bool) -> int {
    if selected(orders, suppliers, line, supplier_mode) {
        if revenue { maintenance::difference(before, after, line, nation, day).1 }
        else { maintenance::difference(before, after, line, nation, day).0 }
    } else { 0 }
}

pub fn rescan(before_customers: &Table, before_orders: &Table, before_suppliers: &Table,
    after_customers: &Table, after_orders: &Table, after_suppliers: &Table,
    orders: &HashMap<i64, bool>, suppliers: &HashMap<i64, bool>, lines: &[LineitemInput], supplier_mode: bool, changes: &mut Bucket) -> (ok: bool)
    requires before_customers.wf(), before_customers.dimension(), before_orders.wf(), before_orders.counts_nonnegative(), before_suppliers.wf(), before_suppliers.dimension(),
        after_customers.wf(), after_customers.dimension(), after_orders.wf(), after_orders.counts_nonnegative(), after_suppliers.wf(), after_suppliers.dimension(), old(changes).wf(),
    ensures final(changes).wf(), ok ==> forall|nation: i64, sub: i64, day: i32| #[trigger] final(changes).at(nation, sub, day) ==
        (old(changes).at(nation, sub, day).0 + if sub == 0 { sum(lines@, |line: LineitemInput| rescan_weight((*before_customers, *before_orders, *before_suppliers), (*after_customers, *after_orders, *after_suppliers), orders@, suppliers@, line, supplier_mode, nation, day, false)) } else { 0 },
         old(changes).at(nation, sub, day).1 + if sub == 0 { sum(lines@, |line: LineitemInput| rescan_weight((*before_customers, *before_orders, *before_suppliers), (*after_customers, *after_orders, *after_suppliers), orders@, suppliers@, line, supplier_mode, nation, day, true)) } else { 0 }),
{
    let ghost entry = *changes;
    let ghost before = (*before_customers, *before_orders, *before_suppliers);
    let ghost after = (*after_customers, *after_orders, *after_suppliers);
    let mut index = 0;
    let mut processed: Ghost<int> = Ghost(0int);
    while index < lines.len()
        invariant index <= lines.len(), processed@ == index, changes.wf(), entry == *old(changes),
            before == (*before_customers, *before_orders, *before_suppliers), after == (*after_customers, *after_orders, *after_suppliers),
            before_customers.wf(), before_customers.dimension(), before_orders.wf(), before_orders.counts_nonnegative(), before_suppliers.wf(), before_suppliers.dimension(),
            after_customers.wf(), after_customers.dimension(), after_orders.wf(), after_orders.counts_nonnegative(), after_suppliers.wf(), after_suppliers.dimension(),
            forall|nation: i64, sub: i64, day: i32| #[trigger] changes.at(nation, sub, day) ==
                (entry.at(nation, sub, day).0 + if sub == 0 { sum(lines@.take(processed@), |line: LineitemInput| rescan_weight(before, after, orders@, suppliers@, line, supplier_mode, nation, day, false)) } else { 0 },
                 entry.at(nation, sub, day).1 + if sub == 0 { sum(lines@.take(processed@), |line: LineitemInput| rescan_weight(before, after, orders@, suppliers@, line, supplier_mode, nation, day, true)) } else { 0 }),
        decreases lines.len() - index,
    {
        let line = lines[index];
        let include = if supplier_mode { suppliers.contains_key(&line.l_suppkey) && !orders.contains_key(&line.l_orderkey) } else { orders.contains_key(&line.l_orderkey) };
        if include {
            if !maintenance::contribute(after_customers, after_orders, after_suppliers, line, 1, changes)
                || !maintenance::contribute(before_customers, before_orders, before_suppliers, line, -1, changes) { return false; }
        }
        proof {
            assert forall|nation: i64, sub: i64, day: i32| #[trigger] changes.at(nation, sub, day) ==
                (entry.at(nation, sub, day).0 + if sub == 0 { sum(lines@.take(index as int + 1), |line: LineitemInput| rescan_weight(before, after, orders@, suppliers@, line, supplier_mode, nation, day, false)) } else { 0 },
                 entry.at(nation, sub, day).1 + if sub == 0 { sum(lines@.take(index as int + 1), |line: LineitemInput| rescan_weight(before, after, orders@, suppliers@, line, supplier_mode, nation, day, true)) } else { 0 }) by {
                lemma_sum_take(lines@, index as int, |line: LineitemInput| rescan_weight(before, after, orders@, suppliers@, line, supplier_mode, nation, day, false));
                lemma_sum_take(lines@, index as int, |line: LineitemInput| rescan_weight(before, after, orders@, suppliers@, line, supplier_mode, nation, day, true));
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    proof { assert(lines@.take(lines@.len() as int) =~= lines@); }
    true
}

pub open spec fn captured_weight(before: (Table, Table, Table), delta: LineitemDelta, nation: i64, day: i32, revenue: bool) -> int {
    delta.sign * if revenue { maintenance::contribution(before.0, before.1, before.2, delta.row, nation, day).1 }
        else { maintenance::contribution(before.0, before.1, before.2, delta.row, nation, day).0 }
}

pub fn capture(customers: &Table, orders: &Table, suppliers: &Table, deltas: &[LineitemDelta], changes: &mut Bucket) -> (ok: bool)
    requires customers.wf(), customers.dimension(), orders.wf(), orders.counts_nonnegative(), suppliers.wf(), suppliers.dimension(), old(changes).wf(),
    ensures final(changes).wf(), ok ==> forall|nation: i64, sub: i64, day: i32| #[trigger] final(changes).at(nation, sub, day) ==
        (old(changes).at(nation, sub, day).0 + if sub == 0 { sum(deltas@, |delta: LineitemDelta| captured_weight((*customers, *orders, *suppliers), delta, nation, day, false)) } else { 0 },
         old(changes).at(nation, sub, day).1 + if sub == 0 { sum(deltas@, |delta: LineitemDelta| captured_weight((*customers, *orders, *suppliers), delta, nation, day, true)) } else { 0 }),
{
    let ghost entry = *changes;
    let ghost before = (*customers, *orders, *suppliers);
    let mut index = 0;
    let mut processed: Ghost<int> = Ghost(0int);
    while index < deltas.len()
        invariant index <= deltas.len(), processed@ == index, changes.wf(), entry == *old(changes), before == (*customers, *orders, *suppliers),
            customers.wf(), customers.dimension(), orders.wf(), orders.counts_nonnegative(), suppliers.wf(), suppliers.dimension(),
            forall|nation: i64, sub: i64, day: i32| #[trigger] changes.at(nation, sub, day) ==
                (entry.at(nation, sub, day).0 + if sub == 0 { sum(deltas@.take(processed@), |delta: LineitemDelta| captured_weight(before, delta, nation, day, false)) } else { 0 },
                 entry.at(nation, sub, day).1 + if sub == 0 { sum(deltas@.take(processed@), |delta: LineitemDelta| captured_weight(before, delta, nation, day, true)) } else { 0 }),
        decreases deltas.len() - index,
    {
        let delta = deltas[index];
        if !maintenance::contribute(customers, orders, suppliers, delta.row, delta.sign as i128, changes) { return false; }
        proof {
            assert forall|nation: i64, sub: i64, day: i32| #[trigger] changes.at(nation, sub, day) ==
                (entry.at(nation, sub, day).0 + if sub == 0 { sum(deltas@.take(index as int + 1), |delta: LineitemDelta| captured_weight(before, delta, nation, day, false)) } else { 0 },
                 entry.at(nation, sub, day).1 + if sub == 0 { sum(deltas@.take(index as int + 1), |delta: LineitemDelta| captured_weight(before, delta, nation, day, true)) } else { 0 }) by {
                lemma_sum_take(deltas@, index as int, |delta: LineitemDelta| captured_weight(before, delta, nation, day, false));
                lemma_sum_take(deltas@, index as int, |delta: LineitemDelta| captured_weight(before, delta, nation, day, true));
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    proof { assert(deltas@.take(deltas@.len() as int) =~= deltas@); }
    true
}

}
