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
use verus_common::sort::*;
use crate::gen_spec::*;
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

// Reads a raw input lineitem row as a logical lineitem, widening its fixed-width numbers to unbounded integers.
// This is how host-supplied rows are related to the abstract query definition.
pub open spec fn li_view(r: LiRow) -> LineitemRow {
    LineitemRow {
        l_orderkey: r.orderkey as int,
        l_suppkey: r.suppkey as int,
        l_commitdate: r.commitdate as int,
        l_receiptdate: r.receiptdate as int,
    }
}

// Reads a batch of raw lineitem rows as the logical lineitem table, i.e. the input data a scan or build is specified against.
// The contracts of build and maintenance are stated in terms of it.
pub open spec fn lis_view(s: Seq<LiRow>) -> Seq<LineitemRow> {
    s.map_values(|r: LiRow| li_view(r))
}

// Decides whether a lineitem was received after its commit date (late), and is proved to agree with the logical definition of late.
// Lateness is the basis for deciding which suppliers kept orders waiting.
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

// Reads a batch of lineitem changes as (row, +1/-1) pairs (+1 inserts a copy, -1 deletes one): the logical form of an update batch.
pub open spec fn ld_view(s: Seq<LiDelta>) -> Seq<(LineitemRow, int)> {
    s.map_values(|d: LiDelta| (li_view(d.row), d.sign as int))
}

// Reads a batch of order changes as (order, +1/-1) pairs.  The executable delta carries only the
// flag (o_orderstatus = 'F'); `st` holds each changed row's actual o_orderstatus bytes (ghost).
pub open spec fn od_view(s: Seq<OrdDelta>, st: Seq<Seq<u8>>) -> Seq<(OrdersRow, int)> {
    Seq::new(s.len(), |i: int| (OrdersRow { o_orderkey: s[i].orderkey as int, o_orderstatus: st[i] }, s[i].sign as int))
}

/// Capture contract for the status flag: one status per delta, and the host's
/// flag is nonzero exactly when that status is "F".
pub open spec fn od_status_ok(s: Seq<OrdDelta>, st: Seq<Seq<u8>>) -> bool {
    &&& st.len() == s.len()
    &&& forall|i: int| 0 <= i < s.len() ==> ((#[trigger] s[i]).status_is_f != 0) == (st[i] == seq![b'F'])
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

// Reads a stored supplier entry as a logical supplier row (with its name as a byte string).
pub open spec fn sup_view(s: SupE) -> SupplierRow {
    SupplierRow { s_suppkey: s.sk as int, s_name: s.name@, s_nationkey: s.nk as int }
}

// Reads a stored nation entry as a logical nation row.
pub open spec fn nat_view(n: NatE) -> NationRow {
    NationRow { n_nationkey: n.nk as int, n_name: n.name@ }
}

// Reads a list of stored supplier entries as a logical supplier table; the state is required to hold the same bag of rows as the database.
pub open spec fn sups_view(s: Seq<SupE>) -> Seq<SupplierRow> {
    s.map_values(|x: SupE| sup_view(x))
}

// Reads a list of stored nation entries as a logical nation table; the state is required to hold the same bag of rows as the database.
pub open spec fn nats_view(s: Seq<NatE>) -> Seq<NationRow> {
    s.map_values(|x: NatE| nat_view(x))
}

// Lets shared multiset-of-rows machinery (insert/delete/compare) work on supplier entries, using their logical row as the meaning.
impl BagRow for SupE {
    type V = SupplierRow;

    open spec fn rv(&self) -> SupplierRow {
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

// Lets shared multiset-of-rows machinery (insert/delete/compare) work on nation entries, using their logical row as the meaning.
impl BagRow for NatE {
    type V = NationRow;

    open spec fn rv(&self) -> NationRow {
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

// The generic bag-view of a supplier entry list is the same as the supplier-table view, so bag-level facts apply to supplier updates.
pub proof fn lemma_rows_v_sups(s: Seq<SupE>)
    ensures
        rows_v(s) == sups_view(s),
{
    assert(rows_v(s) =~= sups_view(s));
}

// The generic bag-view of a nation entry list is the same as the nation-table view, so bag-level facts apply to nation updates.
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

// Copying an order record preserves its meaning (same F count and per-supplier counts); the built-in clone does not promise this.
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

// A stored per-order record is correct: it has at least one 'F' row, its F count equals the true count for that order,
// and its per-supplier line counts match the lineitems in the database. This is the fact incremental maintenance preserves.
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

// Ordering of suppliers by name (ORDER BY s_name ascending):
// a comes no later than b if b's name is not smaller.
pub open spec fn name_le(sups: Seq<SupE>) -> spec_fn(usize, usize) -> bool {
    |a: usize, b: usize| !bytes_lt(sups[b as int].name@, sups[a as int].name@)
}

// A supplier position is legal when it lies within the supplier table.
pub open spec fn valid_sup(sups: Seq<SupE>) -> spec_fn(usize) -> bool {
    |i: usize| (i as int) < sups.len()
}

// Name ordering is a proper sort order (any two are comparable, and it is
// transitive), which a sort routine requires to be correct.
pub proof fn lemma_name_le_preorder(sups: Seq<SupE>)
    ensures
        preorder_on(name_le(sups), valid_sup(sups)),
{
    let le = name_le(sups);
    assert forall|a: usize, b: usize| #![trigger le(a, b)] valid_sup(sups)(a) && valid_sup(sups)(b) implies le(a, b) || le(b, a) by {
        lemma_bytes_lt_asym(sups[a as int].name@, sups[b as int].name@);
    }
    assert forall|a: usize, b: usize, c: usize|
        valid_sup(sups)(a) && valid_sup(sups)(b) && valid_sup(sups)(c) && #[trigger] le(a, b) && #[trigger] le(b, c) implies le(a, c) by {
        let na = sups[a as int].name@;
        let nb = sups[b as int].name@;
        let nc = sups[c as int].name@;
        if bytes_lt(nc, na) {
            if na == nb {
            } else {
                lemma_bytes_lt_total(na, nb);
                lemma_bytes_lt_trans(nc, na, nb);
            }
        }
    }
}

/// A nation's supplier list: the suppliers with nationkey nk (the same bag
/// as nat_filter), sorted by name so that a nation's suppliers come out of
/// gather already in GROUP BY s_name order.
pub open spec fn nat_list_ok(sups: Seq<SupE>, nk: i32, l: Seq<usize>) -> bool {
    &&& same_bag(l, nat_filter(sups, nk, sups.len() as int))
    &&& all_valid(l, valid_sup(sups))
    &&& sorted_le(l, name_le(sups))
}

/// Supplier / nation rows are the tables as bags (row order is irrelevant).
pub open spec fn static_ok(st: Static, db: Db) -> bool {
    &&& sups_view(st.sups@).to_multiset() == db.supplier.to_multiset()
    &&& nats_view(st.nats@).to_multiset() == db.nation.to_multiset()
    &&& st.sups@.len() < usize::MAX
    &&& st.nat_sups@.len() == st.nats@.len()
    &&& forall|j: int|
        0 <= j < st.nats@.len() ==> nat_list_ok(st.sups@, st.nats@[j].nk, (#[trigger] st.nat_sups@[j])@)
}

// ── Order map: persistent base shared across versions + small overlay ──────
pub struct OrderMap {
    pub base: Arc<HashMap<i64, OrdE>>,
    pub overlay: HashMap<i64, Option<OrdE>>,
    pub okeys: Vec<i64>,
}

// Finds the current record for an order: pending changes take precedence (a recorded deletion means none), then the base table, else absent.
pub open spec fn om_get(m: OrderMap, ok: i64) -> Option<OrdE> {
    if m.overlay@.contains_key(ok) {
        m.overlay@[ok]
    } else if m.base@.contains_key(ok) {
        Some(m.base@[ok])
    } else {
        None
    }
}

// Every order is either stored with a correct record or has no 'F' rows and is absent; stored counts fit in 64 bits,
// and every pending change is tracked in the key list. Ensures the order table faithfully describes the database.
pub open spec fn orders_inv(m: OrderMap, db: Db) -> bool {
    &&& forall|ok: i64|
        match #[trigger] om_get(m, ok) {
            Some(o) => ord_ok(o, db, ok as int),
            None => fm(db, ok as int) == 0,
        }
    &&& forall|ok: int| #[trigger] fm(db, ok) > 0 ==> i64::MIN <= ok <= i64::MAX
    &&& forall|k: i64| #[trigger] m.overlay@.contains_key(k) ==> m.okeys@.contains(k)
}

// Executable lookup of an order's record, guaranteed to return exactly what the logical lookup says.
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
// Reads a supplier-total table as a function, treating a missing supplier as total 0 (no waiting orders).
pub open spec fn wget(m: Map<i64, i64>, sk: i64) -> int {
    if m.contains_key(sk) {
        m[sk] as int
    } else {
        0
    }
}

// The supplier-total table is correct: each supplier's entry equals its true waiting-order total, so answers need no rescan.
pub open spec fn w_ok(m: Map<i64, i64>, db: Db) -> bool {
    forall|sk: i64| #[trigger] wget(m, sk) == w(db, sk as int)
}

// Executable lookup of a supplier's waiting-order total, defaulting to 0 when absent, matching the logical lookup.
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

// The maintained state is correct as a whole: supplier/nation tables, per-order records and supplier totals all match the current database.
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

    // Builds a state from correct parts; the invariant is what makes the resulting state usable and maintainable.
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

    // Exposes the state's pieces to read from while retaining the guarantee that they jointly describe the current database.
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

    // Cheaply shares the supplier/nation tables, which never change under updates, so a new state can reuse them without copying.
    pub fn static_arc(&self) -> (r: Arc<Static>)
        ensures
            *r == self.st(),
    {
        self.st.clone()
    }
}

} // verus!
