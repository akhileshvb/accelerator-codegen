// Kernel entry points: the functions gen_obligations.rs calls, with exactly
// its contracts.  Each turns the generated contracts into the predicates the
// implementation is proved against (build_contract, od_status_ok / od_view,
// db_step / dims_step) and calls the implementation.
use vstd::prelude::*;
use verus_common::bag::*;
use verus_common::sums::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::bridge::*;
use crate::orderspec::*;
use crate::state::*;
use crate::build::build_contract;

pub use crate::state::State;

verus! {

/// The scan's EXISTS (an 'F' order with the line's key) is fm(db, key) > 0.
pub proof fn lemma_exists_is_fm(db: Db, l: LineitemRow)
    ensures
        scan_flineitem_exists_1(db, l) <==> fm(db, l.l_orderkey) > 0,
{
    let ok = l.l_orderkey;
    let h = |o: OrdersRow| ind(o.o_orderkey == ok && is_f(o));
    assert(fm(db, ok) == sum(db.orders, h));
    if scan_flineitem_exists_1(db, l) {
        let i = choose|i_o: int| #![trigger db.orders[i_o]]
            0 <= i_o < db.orders.len() && scan_flineitem_exists_1_where(l, db.orders[i_o]);
        assert(h(db.orders[i]) > 0);
        lemma_sum_pos(db.orders, h, i);
    }
    if fm(db, ok) > 0 {
        let i = lemma_sum_nonzero_witness(db.orders, h);
        assert(scan_flineitem_exists_1_where(l, db.orders[i]));
    }
}

/// The generated scan contracts give what build is proved against.
pub proof fn lemma_build_inputs(sups: Seq<SupE>, nats: Seq<NatE>, fkeys: Seq<i64>, lis: Seq<LiRow>, db: Db)
    requires
        scan_supplier_ok(db, sups),
        scan_nation_ok(db, nats),
        scan_forders_ok(db, fkeys),
        scan_flineitem_ok(db, lis),
    ensures
        build_contract(sups, nats, fkeys, lis, db),
{
    lemma_sums_eq_multiset(sup_e_seq_view(sups), db.supplier);
    lemma_sums_eq_multiset(nat_e_seq_view(nats), db.nation);
    assert forall|f: spec_fn(int) -> int| #[trigger]
        sum(seq_view_i64(fkeys), f) == sum(db.orders, |o: OrdersRow| if is_f(o) { f(o.o_orderkey) } else { 0 }) by {
        let g = |o: OrdersRow| if o.o_orderstatus == seq![b'F'] { f(o.o_orderkey) } else { 0 };
        let k = |o: OrdersRow| if is_f(o) { f(o.o_orderkey) } else { 0 };
        assert(sum(seq_view_i64(fkeys), f) == sum(db.orders, g));
        lemma_sum_ext_all(db.orders, g, k);
    }
    assert forall|f: spec_fn(LineitemRow) -> int| #[trigger]
        sum(li_row_seq_view(lis), f) == sum(db.lineitem, |l: LineitemRow| if fm(db, l.l_orderkey) > 0 { f(l) } else { 0 }) by {
        let g = |l: LineitemRow| if scan_flineitem_exists_1(db, l) { f(l) } else { 0 };
        let k = |l: LineitemRow| if fm(db, l.l_orderkey) > 0 { f(l) } else { 0 };
        assert(sum(li_row_seq_view(lis), f) == sum(db.lineitem, g));
        assert forall|l: LineitemRow| #[trigger] g(l) == k(l) by {
            lemma_exists_is_fm(db, l);
        }
        lemma_sum_ext_all(db.lineitem, g, k);
    }
}

pub fn build(
    supplier: Vec<SupE>,
    nation: Vec<NatE>,
    forders: &[i64],
    flineitem: &[LiRow],
    Ghost(db): Ghost<Db>,
) -> (r: Option<State>)
    requires
        scan_supplier_ok(db, supplier@),
        scan_nation_ok(db, nation@),
        scan_forders_ok(db, forders@),
        scan_flineitem_ok(db, flineitem@),
    ensures
        r matches Some(s) ==> s.db() == db,
{
    proof {
        lemma_build_inputs(supplier@, nation@, forders@, flineitem@, db);
    }
    crate::build::build(supplier, nation, forders, flineitem, Ghost(db))
}

pub fn execute(s: &State, p1: &[u8]) -> (r: Option<Vec<(Vec<u8>, i64)>>)
    ensures
        r matches Some(out) ==> query_result(s.db(), params(p1@), out_view(out@)),
{
    crate::execute::execute(s, p1)
}

/// Orderkeys newly 'F' that the state does not track (their lineitems are
/// rescanned); only the orders deltas matter.
pub fn rescan_lineitem_keys(
    s: &State,
    lineitem_delta: &[LiDelta],
    nation_delta: &Vec<(NatE, i64)>,
    orders_delta: &[OrdDelta],
    supplier_delta: &Vec<(SupE, i64)>,
) -> (r: Vec<i64>) {
    crate::maint::needs_scan(s, orders_delta)
}

pub fn apply(
    s: &State,
    lineitem_delta: &[LiDelta],
    nation_delta: &Vec<(NatE, i64)>,
    orders_delta: &[OrdDelta],
    supplier_delta: &Vec<(SupE, i64)>,
    lineitem_keys: &[i64],
    lineitem_rows: &[LiRow],
    Ghost(orders_full): Ghost<Seq<(OrdersRow, int)>>,
    Ghost(db2): Ghost<Db>,
) -> (r: Result<State, u32>)
    requires
        orders_delta_ok(orders_delta@, orders_full),
        capture_ok(
            s.db(),
            db2,
            li_delta_seq_view(lineitem_delta@),
            nat_e_delta_seq_view(nation_delta@),
            orders_full,
            sup_e_delta_seq_view(supplier_delta@),
        ),
        rescan_lineitem_ok(db2, lineitem_keys@, lineitem_rows@),
    ensures
        r matches Ok(s2) ==> s2.db() == db2,
{
    // The kernel reads an orders delta as (key, flag, sign) plus the status
    // bytes as ghost; those bytes are the full rows' o_orderstatus.
    let ghost ost = orders_full.map_values(|x: (OrdersRow, int)| x.0.o_orderstatus);
    proof {
        assert(od_status_ok(orders_delta@, ost));
        assert(od_view(orders_delta@, ost) =~= orders_full);
        assert(dview(supplier_delta@) =~= sup_e_delta_seq_view(supplier_delta@));
        assert(dview(nation_delta@) =~= nat_e_delta_seq_view(nation_delta@));
    }
    crate::maint::apply(
        s,
        orders_delta,
        lineitem_delta,
        supplier_delta,
        nation_delta,
        lineitem_keys,
        lineitem_rows,
        Ghost(ost),
        Ghost(db2),
    )
}

} // verus!
