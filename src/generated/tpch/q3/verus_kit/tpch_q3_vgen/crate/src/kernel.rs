#![allow(unused_imports)]
use vstd::prelude::*;
use verus_common::rat::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use verus_prooflib::ordered::Index;
use crate::model::*;

verus! {

pub struct State {
    pub(crate) customers: Index<CustomerCell>,
    pub(crate) pages: Index<Page>,
    pub(crate) ranks: Index<Index<()>>,
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

    pub(crate) fn from_parts(customers: Index<CustomerCell>, pages: Index<Page>, ranks: Index<Index<()>>, Ghost(db): Ghost<Db>) -> (result: Self)
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
{
    crate::output::read(s, p1, p2)
}

pub fn rescan_orders_by_key_keys(
    s: &State,
    customer_delta: &Vec<CustomerDelta>,
    lineitem_delta: &[ShipmentKeyDelta],
    orders_delta: &[OrderKeyDelta],
) -> (r: Vec<i64>)
{
    crate::maintenance::fact_keys(lineitem_delta, orders_delta)
}

pub fn rescan_shipments_by_key_keys(
    s: &State,
    customer_delta: &Vec<CustomerDelta>,
    lineitem_delta: &[ShipmentKeyDelta],
    orders_delta: &[OrderKeyDelta],
) -> (r: Vec<i64>)
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
        rescan_orders_by_key_ok(db2, orders_by_key_keys@, orders_by_key_rows@),
        rescan_shipments_by_key_ok(db2, shipments_by_key_keys@, shipments_by_key_rows@),
    ensures
        r matches Ok(s2) ==> s2.db() == db2,
{
    crate::maintenance::maintain(s, customer_delta, lineitem_delta, orders_delta,
        orders_by_key_keys, orders_by_key_rows, shipments_by_key_keys, shipments_by_key_rows,
        Ghost(lineitem_full), Ghost(orders_full), Ghost(db2))
}

} // verus!
