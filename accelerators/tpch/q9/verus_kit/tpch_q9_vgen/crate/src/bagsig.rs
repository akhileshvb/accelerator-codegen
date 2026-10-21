// Signed events: a table as the signed sum of row events, and the per-row weights the indexes count.
use vstd::prelude::*;
use verus_common::sums::*;
use verus_common::sqlfns::*;
use crate::gen_spec::*;

verus! {

/// The signed weight of an event: its sign times the weight of its row.
pub open spec fn signed<R>(w: spec_fn(R) -> int) -> spec_fn((R, int)) -> int {
    |d: (R, int)| d.1 * w(d.0)
}

pub open spec fn dates_w(k: int, y: int) -> spec_fn(OrdersRow) -> int {
    |r: OrdersRow| if r.o_orderkey == k && civil_year(r.o_orderdate) == y { 1int } else { 0int }
}

pub open spec fn sup_w(k: int, n: int) -> spec_fn(SupplierRow) -> int {
    |r: SupplierRow| if r.s_suppkey == k && r.s_nationkey == n { 1int } else { 0int }
}

pub open spec fn ps_w(k: int, s: int, f: int) -> spec_fn(PartsuppRow) -> int {
    |r: PartsuppRow| if r.ps_partkey == k && r.ps_suppkey == s { if f == 0 { 1int } else { r.ps_supplycost } } else { 0int }
}

/// bag_step from the empty table: the table is exactly the signed events.
pub open spec fn built_from<R>(table: Seq<R>, events: Seq<(R, int)>) -> bool {
    bag_step(Seq::<R>::empty(), table, events)
}

pub proof fn lemma_built_sum<R>(table: Seq<R>, events: Seq<(R, int)>, w: spec_fn(R) -> int)
    requires built_from(table, events),
    ensures sum(table, w) == sum(events, signed(w)),
{
    lemma_sum_empty(Seq::<R>::empty(), w);
}

}
