#![allow(unused_imports)]
use vstd::prelude::*;
use crate::custs::Custs;
use crate::paged::{Paged, PageCursor, entry_sorted, Ent};
use crate::rank_ent::*;
use verus_common::rat::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use verus_prooflib::ordered::Index;
use crate::model::*;
use crate::output::wide_out;

verus! {

pub struct State {
    pub(crate) customers: Custs,
    pub(crate) pages: Index<Page>,
    pub(crate) ranks: Index<Paged<RankEnt>>,
    pub(crate) database: Ghost<Db>,
}

impl State {
    pub closed spec fn db(self) -> Db {
        self.database@
    }

    #[verifier::type_invariant]
    pub(crate) closed spec fn valid(self) -> bool {
        state_ok(self.customers, self.pages, self.ranks, self.database@)
    }

    pub(crate) fn facts(&self)
        ensures state_ok(self.customers, self.pages, self.ranks, self.db()),
    {
        proof { use_type_invariant(self); }
    }

    pub(crate) fn from_parts(customers: Custs, pages: Index<Page>, ranks: Index<Paged<RankEnt>>, Ghost(db): Ghost<Db>) -> (result: Self)
        requires state_ok(customers, pages, ranks, db),
        ensures result.db() == db,
    {
        State { customers, pages, ranks, database: Ghost(db) }
    }
}

pub fn build(
    customers: Vec<CustomerInput>,
    orders: &[OrderInput],
    shipments: &[ShipmentInput],
    Ghost(db): Ghost<Db>,
) -> (r: Option<State>)
    requires
        scan_customers_ok(db, customers@),
        scan_orders_ok(db, orders@),
        scan_shipments_ok(db, shipments@),
    ensures
        r matches Some(s) ==> s.db() == db,
        envelope(db) ==> r is Some,
{
    crate::construction::construct(customers, orders, shipments, Ghost(db))
}

pub fn execute(
    s: &State,
    p1: &[u8],
    p2: i32,
) -> (r: Option<Vec<(i64, i128, i32, i64)>>)
    ensures
        r matches Some(out) ==> query_result(s.db(), params(p1@, p2 as int), out_view(out@)),
        answer_fits(s.db(), params(p1@, p2 as int)) ==> r is Some,
{
    let rows = crate::output::read(s, p1, p2);
    let mut out: Vec<(i64, i128, i32, i64)> = Vec::new();
    let mut index = 0;
    while index < rows.len()
        invariant index <= rows.len(), out@.len() == index,
            query_result(s.db(), params(p1@, p2 as int), wide_out(rows@)), out_view(out@) == wide_out(rows@.take(index as int)),
        decreases rows.len() - index,
    {
        let row = &rows[index];
        let revenue = match row.1.to_i128() {
            Some(value) => value,
            None => {
                proof {
                    let answer = wide_out(rows@);
                    assert(answer.len() == rows@.len());
                    assert(answer[index as int].revenue == rows@[index as int].1@);
                    assert(!out_fits(answer[index as int]));
                    assert(query_result(s.db(), params(p1@, p2 as int), answer));
                    assert(!answer_fits(s.db(), params(p1@, p2 as int))) by {
                        if answer_fits(s.db(), params(p1@, p2 as int)) { assert(out_fits(answer[index as int])); }
                    }
                }
                return None;
            },
        };
        let ghost before = out@;
        out.push((row.0, revenue, row.2, row.3));
        proof {
            let target = rows@.take(index as int + 1);
            assert(target =~= rows@.take(index as int).push(rows@[index as int]));
            assert(out@ =~= before.push((row.0, revenue, row.2, row.3)));
            assert(out_view(out@) =~= wide_out(target)) by {
                assert(out_view(out@).len() == out@.len() && wide_out(target).len() == target.len());
                assert forall|i: int| 0 <= i < out_view(out@).len() implies out_view(out@)[i] == wide_out(target)[i] by {
                    if i < index as int {
                        assert(out@[i] == before[i]);
                        assert(target[i] == rows@.take(index as int)[i]);
                        assert(out_view(before)[i] == wide_out(rows@.take(index as int))[i]);
                    } else {
                        assert(target[i] == rows@[index as int]);
                    }
                }
            }
        }
        index += 1;
    }
    proof { assert(rows@.take(index as int) =~= rows@); }
    Some(out)
}

/// The state keeps nothing that grows with past writes.
pub open spec fn history(s: State) -> nat {
    0
}

/// The orderkeys whose orders the host rescans: those of the transaction's lineitem and orders changes.
pub open spec fn rescan_orders_by_key_wanted(s: State, customer_delta: Seq<CustomerDelta>,
    lineitem_delta: Seq<ShipmentKeyDelta>, orders_delta: Seq<OrderKeyDelta>) -> Seq<i64> {
    crate::maintenance::requested_keys(lineitem_delta, orders_delta)
}

/// The orderkeys whose lineitems the host rescans: the same keys.
pub open spec fn rescan_shipments_by_key_wanted(s: State, customer_delta: Seq<CustomerDelta>,
    lineitem_delta: Seq<ShipmentKeyDelta>, orders_delta: Seq<OrderKeyDelta>) -> Seq<i64> {
    crate::maintenance::requested_keys(lineitem_delta, orders_delta)
}

pub fn rescan_orders_by_key_keys(
    s: &State,
    customer_delta: &Vec<CustomerDelta>,
    lineitem_delta: &[ShipmentKeyDelta],
    orders_delta: &[OrderKeyDelta],
) -> (r: Vec<i64>)
    ensures r@ == rescan_orders_by_key_wanted(*s, customer_delta@, lineitem_delta@, orders_delta@),
{
    crate::maintenance::fact_keys(lineitem_delta, orders_delta)
}

pub fn rescan_shipments_by_key_keys(
    s: &State,
    customer_delta: &Vec<CustomerDelta>,
    lineitem_delta: &[ShipmentKeyDelta],
    orders_delta: &[OrderKeyDelta],
) -> (r: Vec<i64>)
    ensures r@ == rescan_shipments_by_key_wanted(*s, customer_delta@, lineitem_delta@, orders_delta@),
{
    crate::maintenance::fact_keys(lineitem_delta, orders_delta)
}

pub fn apply(
    s: &State,
    customer_delta: &Vec<CustomerDelta>,
    lineitem_delta: &[ShipmentKeyDelta],
    orders_delta: &[OrderKeyDelta],
    orders_by_key_keys: &[i64],
    orders_by_key_rows: &[OrderInput],
    shipments_by_key_keys: &[i64],
    shipments_by_key_rows: &[ShipmentInput],
    Ghost(lineitem_full): Ghost<Seq<(LineitemRow, int)>>,
    Ghost(orders_full): Ghost<Seq<(OrdersRow, int)>>,
    Ghost(db2): Ghost<Db>,
) -> (r: Result<State, u32>)
    requires
        lineitem_delta_ok(lineitem_delta@, lineitem_full),
        orders_delta_ok(orders_delta@, orders_full),
        capture_ok(s.db(), db2, customer_delta_seq_view(customer_delta@), lineitem_full, orders_full),
        orders_by_key_keys@ == rescan_orders_by_key_wanted(*s, customer_delta@, lineitem_delta@, orders_delta@),
        shipments_by_key_keys@ == rescan_shipments_by_key_wanted(*s, customer_delta@, lineitem_delta@, orders_delta@),
        rescan_orders_by_key_ok(db2, orders_by_key_keys@, orders_by_key_rows@),
        rescan_shipments_by_key_ok(db2, shipments_by_key_keys@, shipments_by_key_rows@),
    ensures
        r matches Ok(s2) ==> s2.db() == db2,
        envelope(s.db()) && envelope(db2) ==> r is Ok,
{
    crate::maintenance::maintain(s, customer_delta, lineitem_delta, orders_delta,
        orders_by_key_keys, orders_by_key_rows, shipments_by_key_keys, shipments_by_key_rows,
        Ghost(lineitem_full), Ghost(orders_full), Ghost(db2))
}

} // verus!
