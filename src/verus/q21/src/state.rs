// Kernel state and its invariant.
//
// A `State` carries (as ghost) the database it answers for.  Its type
// invariant ties the executable structures to that database:
//   * supplier / nation rows are the tables as bags,
//   * the order map holds, for every orderkey with 'F' rows, the 'F'
//     multiplicity and the per-supplier (count, late count) entries,
//   * the W map holds W(sk) for every suppkey (absent = 0).
use vstd::prelude::*;
use std::collections::HashMap;
use std::sync::Arc;
use verus_common::sums::*;
use verus_common::bag::*;
use verus_common::bytes::*;
use crate::spec::*;
use crate::bridge::*;
use crate::orderspec::*;
use crate::ents::*;

verus! {

broadcast use vstd::std_specs::hash::group_hash_axioms;

// ── Rows crossing the FFI boundary ─────────────────────────────────────────
#[repr(C)]
#[derive(Clone, Copy)]
pub struct LiRow {
    pub orderkey: i64,
    pub suppkey: i64,
    pub commitdate: i32,
    pub receiptdate: i32,
}

pub open spec fn li_view(r: LiRow) -> Lineitem {
    Lineitem {
        orderkey: r.orderkey as int,
        suppkey: r.suppkey as int,
        commitdate: r.commitdate as int,
        receiptdate: r.receiptdate as int,
    }
}

pub open spec fn lis_view(s: Seq<LiRow>) -> Seq<Lineitem> {
    s.map_values(|r: LiRow| li_view(r))
}

pub fn is_late(r: &LiRow) -> (b: bool)
    ensures
        b == late(li_view(*r)),
{
    r.receiptdate > r.commitdate
}

/// A captured lineitem change: the projected row and +1 (insert) / -1 (delete).
#[repr(C)]
#[derive(Clone, Copy)]
pub struct LiDelta {
    pub row: LiRow,
    pub sign: i64,
}

/// A captured orders change: orderkey, (o_orderstatus = 'F'), and the sign.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct OrdDelta {
    pub orderkey: i64,
    pub status_is_f: u8,
    pub sign: i64,
}

pub open spec fn ld_view(s: Seq<LiDelta>) -> Seq<(Lineitem, int)> {
    s.map_values(|d: LiDelta| (li_view(d.row), d.sign as int))
}

pub open spec fn od_view(s: Seq<OrdDelta>) -> Seq<(Order, int)> {
    s.map_values(
        |d: OrdDelta| (Order { orderkey: d.orderkey as int, status_is_f: d.status_is_f != 0 }, d.sign as int),
    )
}

pub struct SupE {
    pub sk: i64,
    pub name: Vec<u8>,
    pub nk: i32,
}

pub struct NatE {
    pub nk: i32,
    pub name: Vec<u8>,
}

pub open spec fn sup_view(s: SupE) -> Supplier {
    Supplier { suppkey: s.sk as int, name: s.name@, nationkey: s.nk as int }
}

pub open spec fn nat_view(n: NatE) -> Nation {
    Nation { nationkey: n.nk as int, name: n.name@ }
}

pub open spec fn sups_view(s: Seq<SupE>) -> Seq<Supplier> {
    s.map_values(|x: SupE| sup_view(x))
}

pub open spec fn nats_view(s: Seq<NatE>) -> Seq<Nation> {
    s.map_values(|x: NatE| nat_view(x))
}

impl BagRow for SupE {
    type V = Supplier;

    open spec fn rv(&self) -> Supplier {
        sup_view(*self)
    }

    fn same(&self, o: &Self) -> (r: bool) {
        self.sk == o.sk && self.nk == o.nk && bytes_eq_exec(self.name.as_slice(), o.name.as_slice())
    }

    fn dup(&self) -> (r: Self) {
        let name = self.name.clone();
        proof {
            assert(name@ =~= self.name@);
        }
        SupE { sk: self.sk, name, nk: self.nk }
    }
}

impl BagRow for NatE {
    type V = Nation;

    open spec fn rv(&self) -> Nation {
        nat_view(*self)
    }

    fn same(&self, o: &Self) -> (r: bool) {
        self.nk == o.nk && bytes_eq_exec(self.name.as_slice(), o.name.as_slice())
    }

    fn dup(&self) -> (r: Self) {
        let name = self.name.clone();
        proof {
            assert(name@ =~= self.name@);
        }
        NatE { nk: self.nk, name }
    }
}

pub proof fn lemma_rows_v_sups(s: Seq<SupE>)
    ensures
        rows_v(s) == sups_view(s),
{
    assert(rows_v(s) =~= sups_view(s));
}

pub proof fn lemma_rows_v_nats(s: Seq<NatE>)
    ensures
        rows_v(s) == nats_view(s),
{
    assert(rows_v(s) =~= nats_view(s));
}

// ── Per-order record ───────────────────────────────────────────────────────
pub struct OrdE {
    pub fm: i64,
    pub ents: Vec<Ent>,
}

impl Clone for OrdE {
    fn clone(&self) -> (r: Self)
        ensures
            r.fm == self.fm,
            r.ents@ == self.ents@,
    {
        let ents = self.ents.clone();
        proof {
            assert(ents@ =~= self.ents@);
        }
        OrdE { fm: self.fm, ents }
    }
}

pub open spec fn ord_ok(o: OrdE, db: Db, ok: int) -> bool {
    &&& o.fm > 0
    &&& o.fm as int == fm(db, ok)
    &&& ents_ok(o.ents@, db, ok)
}

// ── Static part (suppliers / nations; unchanged by incremental maintenance) ─
pub struct Static {
    pub sups: Vec<SupE>,
    pub nats: Vec<NatE>,
    pub nat_sups: Vec<Vec<usize>>,
}

/// Indices 0..n of supplier rows whose nationkey is nk, in order.
pub open spec fn nat_filter(sups: Seq<SupE>, nk: i32, n: int) -> Seq<usize> {
    Seq::new(n as nat, |i: int| i as usize).filter(|i: usize| sups[i as int].nk == nk)
}

/// Supplier / nation rows are the tables as bags (row order is irrelevant).
pub open spec fn static_ok(st: Static, db: Db) -> bool {
    &&& sups_view(st.sups@).to_multiset() == db.supplier.to_multiset()
    &&& nats_view(st.nats@).to_multiset() == db.nation.to_multiset()
    &&& st.sups@.len() < usize::MAX
    &&& st.nat_sups@.len() == st.nats@.len()
    &&& forall|j: int|
        0 <= j < st.nats@.len() ==> (#[trigger] st.nat_sups@[j])@ == nat_filter(
            st.sups@,
            st.nats@[j].nk,
            st.sups@.len() as int,
        )
}

// ── Order map: persistent base shared across versions + small overlay ──────
pub struct OrderMap {
    pub base: Arc<HashMap<i64, OrdE>>,
    pub overlay: HashMap<i64, Option<OrdE>>,
    pub okeys: Vec<i64>,
}

pub open spec fn om_get(m: OrderMap, ok: i64) -> Option<OrdE> {
    if m.overlay@.contains_key(ok) {
        m.overlay@[ok]
    } else if m.base@.contains_key(ok) {
        Some(m.base@[ok])
    } else {
        None
    }
}

pub open spec fn orders_inv(m: OrderMap, db: Db) -> bool {
    &&& forall|ok: i64|
        match #[trigger] om_get(m, ok) {
            Some(o) => ord_ok(o, db, ok as int),
            None => fm(db, ok as int) == 0,
        }
    &&& forall|ok: int| #[trigger] fm(db, ok) > 0 ==> i64::MIN <= ok <= i64::MAX
    &&& forall|k: i64| #[trigger] m.overlay@.contains_key(k) ==> m.okeys@.contains(k)
}

pub fn om_get_exec(m: &OrderMap, ok: i64) -> (r: Option<&OrdE>)
    ensures
        r.is_some() == om_get(*m, ok).is_some(),
        r.is_some() ==> *r.unwrap() == om_get(*m, ok).unwrap(),
{
    assert(vstd::std_specs::hash::obeys_key_model::<i64>());
    match m.overlay.get(&ok) {
        Some(v) => match v {
            Some(o) => Some(o),
            None => None,
        },
        None => m.base.get(&ok),
    }
}

/// W per suppkey (absent = 0).  W depends only on lineitem and orders.
pub open spec fn wget(m: Map<i64, i64>, sk: i64) -> int {
    if m.contains_key(sk) {
        m[sk] as int
    } else {
        0
    }
}

pub open spec fn w_ok(m: Map<i64, i64>, db: Db) -> bool {
    forall|sk: i64| #[trigger] wget(m, sk) == w(db, sk as int)
}

pub fn wget_exec(m: &HashMap<i64, i64>, sk: i64) -> (r: i64)
    ensures
        r as int == wget(m@, sk),
{
    assert(vstd::std_specs::hash::obeys_key_model::<i64>());
    match m.get(&sk) {
        Some(v) => *v,
        None => 0,
    }
}

// ── The state ──────────────────────────────────────────────────────────────
pub struct State {
    st: Arc<Static>,
    orders: OrderMap,
    wm: HashMap<i64, i64>,
    db: Ghost<Db>,
}

pub open spec fn state_ok(st: Static, orders: OrderMap, wm: Map<i64, i64>, db: Db) -> bool {
    &&& static_ok(st, db)
    &&& orders_inv(orders, db)
    &&& w_ok(wm, db)
}

impl State {
    /// The database this state answers for.
    pub closed spec fn db(self) -> Db {
        self.db@
    }

    #[verifier::type_invariant]
    pub closed spec fn wf(self) -> bool {
        state_ok(*self.st, self.orders, self.wm@, self.db@)
    }

    pub closed spec fn st(self) -> Static {
        *self.st
    }

    pub closed spec fn orders(self) -> OrderMap {
        self.orders
    }

    pub closed spec fn wm(self) -> Map<i64, i64> {
        self.wm@
    }

    pub fn new(st: Arc<Static>, orders: OrderMap, wm: HashMap<i64, i64>, Ghost(db): Ghost<Db>) -> (r: State)
        requires
            state_ok(*st, orders, wm@, db),
        ensures
            r.db() == db,
            r.st() == *st,
            r.orders() == orders,
            r.wm() == wm@,
    {
        State { st, orders, wm, db: Ghost(db) }
    }

    pub fn parts(&self) -> (r: (&Static, &OrderMap, &HashMap<i64, i64>))
        ensures
            *r.0 == self.st(),
            *r.1 == self.orders(),
            r.2@ == self.wm(),
            state_ok(*r.0, *r.1, r.2@, self.db()),
    {
        proof {
            use_type_invariant(self);
        }
        (&*self.st, &self.orders, &self.wm)
    }

    pub fn static_arc(&self) -> (r: Arc<Static>)
        ensures
            *r == self.st(),
    {
        self.st.clone()
    }
}

} // verus!
