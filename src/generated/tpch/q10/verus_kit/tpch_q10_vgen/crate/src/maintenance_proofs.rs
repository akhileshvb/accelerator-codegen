use vstd::prelude::*;
use verus_common::sums::*;
use verus_prooflib::ordered::Index;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::maintenance::*;
use crate::facts::*;
use crate::model::*;
use crate::values::*;
use crate::calendar::*;
use crate::capture_proofs::*;
use crate::scan_proofs::*;

verus! {

#[verifier::rlimit(10)]
pub proof fn lemma_patch_capture(before: Db, after: Db, keys: &Index<()>, patches: Patches,
    order_changes: Orders, line_changes: Index<Moments>, post_orders: Orders, post_lines: Index<Moments>,
    order_rows: Seq<OrderDelta>, line_rows: Seq<LineDelta>, full_lines: Seq<(LineitemRow, int)>,
    order_keys: Seq<i64>, order_rescan: Seq<OrderInput>, line_keys: Seq<i64>, line_rescan: Seq<ReturnedLineInput>,
    day: int, customer: int, grain: int, component: int)
    requires keys.wf(), orders_valid(order_changes), orders_valid(post_orders),
        lineitem_delta_ok(line_rows, full_lines),
        bag_step(before.orders, after.orders, order_delta_seq_view(order_rows)), bag_step(before.lineitem, after.lineitem, full_lines),
        rescan_orders_for_keys_ok(after, order_keys, order_rescan), rescan_returned_lines_for_keys_ok(after, line_keys, line_rescan),
        forall|key: (int, Seq<u8>)| #[trigger] keys.at(key).is_some() ==>
            key.1 == Seq::<u8>::empty() && seq_view_i64(order_keys).contains(key.0) && seq_view_i64(line_keys).contains(key.0),
        forall|key: (int, Seq<u8>)| #![trigger order_changes.at(key), line_changes.at(key)]
            order_changes.at(key).is_some() || line_changes.at(key).is_some() ==> keys.at(key).is_some(),
        forall|key: int, target_customer: int, target_day: int| #[trigger] order_count(order_changes, key, target_customer, target_day)
            == sum(order_rows, |delta: OrderDelta| if delta.row.o_orderkey == key && delta.row.o_custkey == target_customer
                && delta.row.o_orderdate == target_day { delta.sign as int } else { 0int }),
        forall|key: int, target_customer: int, target_day: int| #[trigger] order_count(post_orders, key, target_customer, target_day)
            == sum(order_rescan, |row: OrderInput| if row.o_orderkey == key && row.o_custkey == target_customer && row.o_orderdate == target_day { 1int } else { 0int }),
        forall|key: int, target_component: int| #[trigger] moment_at(line_changes, key).component(target_component)
            == sum(line_rows, |row: LineDelta| if row.l_orderkey == key { delta_line_weight(row, target_component) } else { 0 }),
        forall|key: int, target_component: int| #[trigger] moment_at(post_lines, key).component(target_component)
            == scanned_line_sum(line_rescan, key, target_component),
        patch_rows_sum(patches.entries(), day, customer, grain, component)
            == sum(keys.entries(), changed_key_weight(order_changes, line_changes, post_orders, post_lines, day, customer, grain, component)),
    ensures patch_rows_sum(patches.entries(), day, customer, grain, component)
        == fact_sum(after, day, customer, grain, component) - fact_sum(before, day, customer, grain, component),
{
    assert forall|key: int| #[trigger] order_group_at(order_changes, key, day, customer, grain)
        == order_delta_group(order_delta_seq_view(order_rows), key, day, customer, grain) by {
        lemma_captured_order_group(order_rows, order_changes, key, day, customer, grain);
    }
    assert forall|key: int| #[trigger] moment_at(line_changes, key).component(component) == line_delta_group(full_lines, key, component) by {
        reveal(line_delta_group);
        lemma_captured_lines(line_rows, full_lines, key, component);
    }
    assert forall|key: int| #[trigger] keys.at((key, Seq::empty())).is_some() implies
        order_group_at(post_orders, key, day, customer, grain) == order_group(after.orders, key, day, customer, grain) by {
        lemma_rescanned_order_group(after, order_keys, order_rescan, post_orders, key, day, customer, grain);
    }
    assert forall|key: int| #[trigger] keys.at((key, Seq::empty())).is_some() implies
        moment_at(post_lines, key).component(component) == line_group(after.lineitem, key, component) by {
        reveal(line_group);
        lemma_rescanned_lines(after, line_keys, line_rescan, key, component);
    }
    lemma_changed_key_sum(before, after, keys, order_changes, line_changes, post_orders, post_lines,
        order_delta_seq_view(order_rows), full_lines, day, customer, grain, component);
}

pub proof fn lemma_patch_bounds(patches: &Patches, day: int, customer: int, grain: int, component: int)
    requires patches.wf(), patches_valid(*patches), 0 <= grain <= 3,
        day < period_of(i32::MIN as int, grain) || day > period_of(i32::MAX as int, grain),
    ensures patch_rows_sum(patches.entries(), day, customer, grain, component) == 0,
{
    patches.model();
    let weight = patch_row_weight(day, customer, grain, component);
    assert forall|position: int| #![trigger patches.entries()[position]] 0 <= position < patches.entries().len()
        implies weight(patches.entries()[position]) == 0 by {
        let key = patches.entries()[position].0;
        assert(patches.at(key).is_some());
        crate::calendar_proofs::lemma_period_bounds(key.0, grain);
    }
    lemma_sum_zero(patches.entries(), weight);
}

pub proof fn lemma_updated_facts(before: Db, after: Db, previous: Index<Slice>, next: Index<Slice>, patches: &Patches, grain: int)
    requires 0 <= grain <= 3, facts_valid(before, previous, grain), slices_valid(next), patches.wf(), patches_valid(*patches),
        forall|day: int, customer: int, component: int| #[trigger] slice_at(next, day, customer).component(component)
            == slice_at(previous, day, customer).component(component) + patch_rows_sum(patches.entries(), day, customer, grain, component),
        forall|day: int, customer: int, component: int| #[trigger] patch_rows_sum(patches.entries(), day, customer, grain, component)
            == fact_sum(after, day, customer, grain, component) - fact_sum(before, day, customer, grain, component),
    ensures facts_valid(after, next, grain),
{
    reveal(facts_valid);
    assert forall|day: int, customer: int, component: int| #[trigger] slice_at(next, day, customer).component(component)
        == fact_sum(after, day, customer, grain, component) by {
        assert(slice_at(previous, day, customer).component(component) == fact_sum(before, day, customer, grain, component));
    }
    assert forall|key: (int, Seq<u8>)| #[trigger] next.at(key).is_some() implies {
        let slice = next.at(key).unwrap();
        &&& key.1 == Seq::<u8>::empty()
        &&& period_of(i32::MIN as int, grain) <= key.0 <= period_of(i32::MAX as int, grain)
        &&& slice.cells.entries().len() > 0
        &&& slice.valid()
    } by {
        if key.0 < period_of(i32::MIN as int, grain) || key.0 > period_of(i32::MAX as int, grain) {
            assert(previous.at((key.0, Seq::empty())).is_none());
            let slice = next.at(key).unwrap();
            slice.cells.model();
            let cell = slice.cells.entries()[0];
            assert(slice.cells.at(cell.0).is_some());
            lemma_patch_bounds(patches, key.0, cell.0.0, grain, 0);
            assert(slice_at(next, key.0, cell.0.0).count == cell.1.count);
            assert(slice_at(next, key.0, cell.0.0).component(0) == 0);
        }
    }
}

}
