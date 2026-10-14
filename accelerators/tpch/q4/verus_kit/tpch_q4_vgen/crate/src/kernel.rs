use vstd::prelude::*;
use std::sync::Arc;
use verus_common::sums::*;
use crate::semantic::*;
use crate::emission::sorted_output;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::rollup::*;
use crate::maintenance::{changes, support};

verus! {

broadcast use vstd::seq_lib::group_seq_properties;

pub struct State {
    layers: Vec<Arc<Layer>>,
    database: Ghost<Db>,
}

impl State {
    pub closed spec fn db(self) -> Db { self.database@ }

    #[verifier::type_invariant]
    pub closed spec fn valid(self) -> bool {
        &&& forall|index: int| #![trigger self.layers@[index]]
            0 <= index < self.layers@.len() ==> layer_valid(*self.layers@[index])
        &&& forall|key: Key, priority: Seq<u8>| #[trigger] effective(self.layers@, key, priority)
            == if key_height(key) <= 32 { db_count(self.db(), key, priority) } else { 0 }
    }
}

pub fn build(qualifying_orders: Vec<DatePriorityInput>, Ghost(db): Ghost<Db>) -> (out: Option<State>)
    requires scan_qualifying_orders_ok(db, qualifying_orders@),
    ensures out matches Some(state) ==> state.db() == db,
{
    let mut patch = empty_patch();
    let mut index = 0;
    let mut frontier: Ghost<int> = Ghost(0);
    while index < qualifying_orders.len()
        invariant index <= qualifying_orders.len(), frontier@ == index as int, patch_valid(patch),
            forall|key: Key, priority: Seq<u8>| #[trigger] patch_count(patch, key, priority)
                == sum(qualifying_orders@.take(frontier@), |row: DatePriorityInput| build_weight(row, key, priority)),
        decreases qualifying_orders.len() - index,
    {
        let row = &qualifying_orders[index];
        if !add_order(&mut patch, row.o_orderdate, &row.o_orderpriority, 1) { return None; }
        proof {
            assert forall|key: Key, priority: Seq<u8>| #[trigger] patch_count(patch, key, priority)
                == sum(qualifying_orders@.take(index as int + 1), |row: DatePriorityInput| build_weight(row, key, priority)) by {
                lemma_sum_take(qualifying_orders@, index as int, |row: DatePriorityInput| build_weight(row, key, priority));
            }
        }
        index += 1;
        frontier = Ghost(index as int);
    }
    let layers = Vec::new();
    let layer = match finish_patch(patch, &layers) { Some(value) => value, None => return None };
    proof {
        assert forall|key: Key, priority: Seq<u8>| #[trigger] layer_count(layer, key, priority)
            == if key_height(key) <= 32 { db_count(db, key, priority) } else { 0 } by {
            assert(replacement(layer.cells@, layers@, key, priority) == patch_count(patch, key, priority));
            lemma_build_meaning(db, qualifying_orders@, key, priority);
        }
    }
    let layers = publish(&layers, layer);
    proof {
        assert forall|key: Key, priority: Seq<u8>| #[trigger] effective(layers@, key, priority)
            == if key_height(key) <= 32 { db_count(db, key, priority) } else { 0 } by {
            assert(layer_count(layer, key, priority) == if key_height(key) <= 32 { db_count(db, key, priority) } else { 0 });
            lemma_build_meaning(db, qualifying_orders@, key, priority);
            assert(qualifying_orders@.take(qualifying_orders@.len() as int) =~= qualifying_orders@);
        }
    }
    Some(State { layers, database: Ghost(db) })
}

pub fn execute(state: &State, p1: i32, p2: i32) -> (out: Option<Vec<(Vec<u8>, i64)>>)
    ensures out matches Some(rows) ==> query_result(state.db(), params(p1 as int, p2 as int), out_view(rows@)),
{
    proof { use_type_invariant(state); }
    if p1 >= p2 {
        let out = Vec::new();
        proof {
            assert forall|priority: Seq<u8>| #[trigger] group_count(state.db(), params(p1 as int, p2 as int), priority) == 0 by {
                lemma_query_count(state.db(), params(p1 as int, p2 as int), priority);
                lemma_window_empty(state.db(), p1 as int + 2147483648, p2 as int + 2147483648, priority);
            }
        }
        return Some(out);
    }
    let start = (p1 as i64 + 2147483648) as u64;
    let end = (p2 as i64 + 2147483648) as u64;
    let mut cursor = start;
    let mut answer: Bucket = Vec::new();
    proof {
        assert forall|priority: Seq<u8>| #[trigger] bucket_count(answer@, priority) == window_count(state.db(), start as int, start as int, priority) by {
            lemma_window_empty(state.db(), start as int, start as int, priority);
        }
    }
    while cursor < end
        invariant start <= cursor <= end, bucket_unique(answer@),
            end < 4294967296, state.valid(),
            forall|priority: Seq<u8>| #[trigger] bucket_count(answer@, priority)
                == window_count(state.db(), start as int, cursor as int, priority),
        decreases end - cursor,
    {
        let mut width: u64 = 1;
        let mut height: u32 = 0;
        while height < 32
            invariant height <= 32, width > 0, width as int == block_width(height),
                cursor % width == 0, width <= end - cursor, cursor < end < 4294967296,
            decreases 32 - height,
        {
            let next = width * 2;
            if cursor % next != 0 || next > end - cursor { break; }
            width = next;
            height += 1;
        }
        let key = make_key(height, cursor / width);
        let previous: Ghost<Bucket> = Ghost(answer);
        match lookup(&state.layers, key) {
            Some(bucket) => {
                if !add_all(&mut answer, &*bucket) { return None; }
            },
            None => {},
        }
        proof {
            verus_prooflib::lemma_aligned_boundary(cursor as int, width as int);
            assert(cursor as int + width == (key_bucket(key) + 1) * block_width(key_height(key))) by (nonlinear_arith)
                requires cursor as int == key_bucket(key) * block_width(key_height(key)), width as int == block_width(key_height(key));
            assert forall|priority: Seq<u8>| #[trigger] bucket_count(answer@, priority)
                == window_count(state.db(), start as int, cursor as int + width, priority) by {
                assert(effective(state.layers@, key, priority) == db_count(state.db(), key, priority));
                lemma_window_step(state.db(), start as int, cursor as int, cursor as int + width, key, priority);
            }
        }
        cursor += width;
    }
    proof {
        assert forall|priority: Seq<u8>| #[trigger] bucket_count(answer@, priority)
            == group_count(state.db(), params(p1 as int, p2 as int), priority) by {
            lemma_query_count(state.db(), params(p1 as int, p2 as int), priority);
        }
    }
    let bucket: Ghost<Bucket> = Ghost(answer);
    let result = sorted_output(answer);
    proof {
        if let Some(out) = &result {
            lemma_output_meaning(state.db(), params(p1 as int, p2 as int), bucket@@, out@);
        }
    }
    result
}

pub fn rescan_orders_for_line_changes_keys(
    state: &State, lineitem_delta: &[LineDelta], orders_delta: &Vec<OrderDelta>,
) -> Vec<i64> {
    support(lineitem_delta, orders_delta, false)
}

pub fn rescan_qualifying_lines_for_keys_keys(
    state: &State, lineitem_delta: &[LineDelta], orders_delta: &Vec<OrderDelta>,
) -> Vec<i64> {
    support(lineitem_delta, orders_delta, true)
}

pub fn apply(
    state: &State, lineitem_delta: &[LineDelta], orders_delta: &Vec<OrderDelta>,
    orders_for_line_changes_keys: &[i64], orders_for_line_changes_rows: &Vec<OrderInput>,
    qualifying_lines_for_keys_keys: &[i64], qualifying_lines_for_keys_rows: &[QualifyingLineKeyInput],
    Ghost(db2): Ghost<Db>,
) -> (out: Result<State, u32>)
    requires
        capture_ok(state.db(), db2, line_delta_seq_view(lineitem_delta@), order_delta_seq_view(orders_delta@)),
        rescan_orders_for_line_changes_ok(db2, orders_for_line_changes_keys@, orders_for_line_changes_rows@),
        rescan_qualifying_lines_for_keys_ok(db2, qualifying_lines_for_keys_keys@, qualifying_lines_for_keys_rows@),
    ensures out matches Ok(next) ==> next.db() == db2,
{
    proof { use_type_invariant(state); }
    let patch = match changes(lineitem_delta, orders_delta,
        orders_for_line_changes_keys, orders_for_line_changes_rows,
        qualifying_lines_for_keys_keys, qualifying_lines_for_keys_rows) {
        Some(patch) => patch,
        None => return Err(1),
    };
    let layer = match finish_patch(patch, &state.layers) { Some(layer) => layer, None => return Err(2) };
    let layers = publish(&state.layers, layer);
    proof {
        assert forall|key: Key, priority: Seq<u8>| #[trigger] effective(layers@, key, priority)
            == if key_height(key) <= 32 { db_count(db2, key, priority) } else { 0 } by {
            assert(replacement(layer.cells@, state.layers@, key, priority)
                == effective(state.layers@, key, priority) + patch_count(patch, key, priority));
            lemma_apply_meaning(state.db(), db2, lineitem_delta@, orders_delta@,
                orders_for_line_changes_keys@, orders_for_line_changes_rows@,
                qualifying_lines_for_keys_keys@, qualifying_lines_for_keys_rows@, key, priority);
        }
    }
    Ok(State { layers, database: Ghost(db2) })
}

}
