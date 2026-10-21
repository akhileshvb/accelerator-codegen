// The six tables as event logs: what the general path answers from, kept exact under every write.
use vstd::prelude::*;
use verus_common::sums::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::values::*;
use crate::log::*;
use crate::ingest::*;

verus! {

broadcast use vstd::seq_lib::group_seq_properties;

pub struct Raw {
    pub parts: Log<PartDelta>,
    pub orders: Log<OrderDelta>,
    pub suppliers: Log<SupplierDelta>,
    pub nations: Log<NationDelta>,
    pub partsupp: Log<PartsuppDelta>,
    pub lines: Log<LineDelta>,
}

/// Every table of db is the signed sum of its events.
pub open spec fn raw_of(raw: Raw, db: Db) -> bool {
    &&& built_from(db.part, part_delta_seq_view(raw.parts.events()))
    &&& built_from(db.orders, order_delta_seq_view(raw.orders.events()))
    &&& built_from(db.supplier, supplier_delta_seq_view(raw.suppliers.events()))
    &&& built_from(db.nation, nation_delta_seq_view(raw.nations.events()))
    &&& built_from(db.partsupp, partsupp_delta_seq_view(raw.partsupp.events()))
    &&& built_from(db.lineitem, line_delta_seq_view(raw.lines.events()))
}

pub proof fn lemma_unit_events<R>(rows: Seq<R>, f: spec_fn(R) -> int)
    ensures sum(rows.map_values(|x: R| (x, 1int)), signed(f)) == sum(rows, f),
{
    lemma_sum_map(rows, |x: R| (x, 1int), signed(f), f);
}

/// A scan is the table's events with sign +1.
pub proof fn lemma_scan_built<R>(table: Seq<R>, rows: Seq<R>, events: Seq<(R, int)>)
    requires forall|f: spec_fn(R) -> int| #[trigger] sum(rows, f) == sum(table, f), events =~= rows.map_values(|x: R| (x, 1int)),
    ensures built_from(table, events),
{
    assert forall|f: spec_fn(R) -> int| #[trigger] sum(table, f) == sum(Seq::<R>::empty(), f) + sum(events, |d: (R, int)| d.1 * f(d.0)) by {
        assert(signed(f) =~= (|d: (R, int)| d.1 * f(d.0)));
        lemma_sum_empty(Seq::<R>::empty(), f);
        lemma_unit_events(rows, f);
        assert(sum(rows, f) == sum(table, f));
        assert(sum(events, signed(f)) == sum(rows.map_values(|x: R| (x, 1int)), signed(f)));
    }
}

/// A table extended by a transaction's delta is the signed sum of the old events and the delta.
pub proof fn lemma_step_built<R>(before: Seq<R>, after: Seq<R>, old_events: Seq<(R, int)>, delta: Seq<(R, int)>)
    requires built_from(before, old_events), bag_step(before, after, delta),
    ensures built_from(after, old_events + delta),
{
    assert forall|f: spec_fn(R) -> int| #[trigger] sum(after, f) == sum(Seq::<R>::empty(), f) + sum(old_events + delta, |d: (R, int)| d.1 * f(d.0)) by {
        assert(signed(f) =~= (|d: (R, int)| d.1 * f(d.0)));
        lemma_sum_concat(old_events, delta, signed(f));
        assert(sum(after, f) == sum(before, f) + sum(delta, signed(f)));
        assert(sum(before, f) == sum(Seq::<R>::empty(), f) + sum(old_events, signed(f)));
        lemma_sum_empty(Seq::<R>::empty(), f);
    }
}

pub fn copy_part_in(r: &PartIn) -> (c: PartIn)
    ensures part_in_view(c) == part_in_view(*r),
{
    PartIn { p_partkey: r.p_partkey, p_name: copy_bytes(&r.p_name) }
}

pub fn copy_nation_in(r: &NationIn) -> (c: NationIn)
    ensures nation_in_view(c) == nation_in_view(*r),
{
    NationIn { n_nationkey: r.n_nationkey, n_name: copy_bytes(&r.n_name) }
}

pub fn part_events(rows: &Vec<PartIn>) -> (r: Vec<PartDelta>)
    ensures part_delta_seq_view(r@) =~= part_in_seq_view(rows@).map_values(|x: PartRow| (x, 1int)),
{
    let mut r: Vec<PartDelta> = Vec::new();
    let mut i = 0;
    while i < rows.len()
        invariant i <= rows.len(), r@.len() == i,
            forall|j: int| 0 <= j < i ==> part_in_view(r@[j].row) == part_in_view(rows@[j]) && r@[j].sign == 1,
        decreases rows.len() - i,
    {
        r.push(PartDelta { row: copy_part_in(&rows[i]), sign: 1 });
        i += 1;
    }
    r
}

pub fn nation_events(rows: &Vec<NationIn>) -> (r: Vec<NationDelta>)
    ensures nation_delta_seq_view(r@) =~= nation_in_seq_view(rows@).map_values(|x: NationRow| (x, 1int)),
{
    let mut r: Vec<NationDelta> = Vec::new();
    let mut i = 0;
    while i < rows.len()
        invariant i <= rows.len(), r@.len() == i,
            forall|j: int| 0 <= j < i ==> nation_in_view(r@[j].row) == nation_in_view(rows@[j]) && r@[j].sign == 1,
        decreases rows.len() - i,
    {
        r.push(NationDelta { row: copy_nation_in(&rows[i]), sign: 1 });
        i += 1;
    }
    r
}

pub fn order_events(rows: &[OrderIn]) -> (r: Vec<OrderDelta>)
    ensures order_delta_seq_view(r@) =~= order_in_seq_view(rows@).map_values(|x: OrdersRow| (x, 1int)),
{
    let mut r: Vec<OrderDelta> = Vec::new();
    let mut i = 0;
    while i < rows.len()
        invariant i <= rows.len(), r@.len() == i,
            forall|j: int| 0 <= j < i ==> r@[j].row == rows@[j] && r@[j].sign == 1,
        decreases rows.len() - i,
    {
        r.push(OrderDelta { row: rows[i], sign: 1 });
        i += 1;
    }
    r
}

pub fn supplier_events(rows: &[SupplierIn]) -> (r: Vec<SupplierDelta>)
    ensures supplier_delta_seq_view(r@) =~= supplier_in_seq_view(rows@).map_values(|x: SupplierRow| (x, 1int)),
{
    let mut r: Vec<SupplierDelta> = Vec::new();
    let mut i = 0;
    while i < rows.len()
        invariant i <= rows.len(), r@.len() == i,
            forall|j: int| 0 <= j < i ==> r@[j].row == rows@[j] && r@[j].sign == 1,
        decreases rows.len() - i,
    {
        r.push(SupplierDelta { row: rows[i], sign: 1 });
        i += 1;
    }
    r
}

pub fn partsupp_events(rows: &[PartsuppIn]) -> (r: Vec<PartsuppDelta>)
    ensures partsupp_delta_seq_view(r@) =~= partsupp_in_seq_view(rows@).map_values(|x: PartsuppRow| (x, 1int)),
{
    let mut r: Vec<PartsuppDelta> = Vec::new();
    let mut i = 0;
    while i < rows.len()
        invariant i <= rows.len(), r@.len() == i,
            forall|j: int| 0 <= j < i ==> r@[j].row == rows@[j] && r@[j].sign == 1,
        decreases rows.len() - i,
    {
        r.push(PartsuppDelta { row: rows[i], sign: 1 });
        i += 1;
    }
    r
}

pub fn line_events(rows: &[LineIn]) -> (r: Vec<LineDelta>)
    ensures line_delta_seq_view(r@) =~= line_in_seq_view(rows@).map_values(|x: LineitemRow| (x, 1int)),
{
    let mut r: Vec<LineDelta> = Vec::new();
    let mut i = 0;
    while i < rows.len()
        invariant i <= rows.len(), r@.len() == i,
            forall|j: int| 0 <= j < i ==> r@[j].row == rows@[j] && r@[j].sign == 1,
        decreases rows.len() - i,
    {
        r.push(LineDelta { row: rows[i], sign: 1 });
        i += 1;
    }
    r
}

pub fn copy_part_deltas(rows: &Vec<PartDelta>) -> (r: Vec<PartDelta>)
    ensures part_delta_seq_view(r@) =~= part_delta_seq_view(rows@),
{
    let mut r: Vec<PartDelta> = Vec::new();
    let mut i = 0;
    while i < rows.len()
        invariant i <= rows.len(), r@.len() == i,
            forall|j: int| 0 <= j < i ==> part_in_view(r@[j].row) == part_in_view(rows@[j].row) && r@[j].sign == rows@[j].sign,
        decreases rows.len() - i,
    {
        r.push(PartDelta { row: copy_part_in(&rows[i].row), sign: rows[i].sign });
        i += 1;
    }
    r
}

pub fn copy_nation_deltas(rows: &Vec<NationDelta>) -> (r: Vec<NationDelta>)
    ensures nation_delta_seq_view(r@) =~= nation_delta_seq_view(rows@),
{
    let mut r: Vec<NationDelta> = Vec::new();
    let mut i = 0;
    while i < rows.len()
        invariant i <= rows.len(), r@.len() == i,
            forall|j: int| 0 <= j < i ==> nation_in_view(r@[j].row) == nation_in_view(rows@[j].row) && r@[j].sign == rows@[j].sign,
        decreases rows.len() - i,
    {
        r.push(NationDelta { row: copy_nation_in(&rows[i].row), sign: rows[i].sign });
        i += 1;
    }
    r
}

pub fn copy_order_deltas(rows: &[OrderDelta]) -> (r: Vec<OrderDelta>)
    ensures r@ == rows@,
{
    let mut r: Vec<OrderDelta> = Vec::new();
    let mut i = 0;
    while i < rows.len()
        invariant i <= rows.len(), r@ == rows@.take(i as int),
        decreases rows.len() - i,
    {
        r.push(rows[i]);
        i += 1;
    }
    proof { assert(rows@.take(rows.len() as int) =~= rows@); }
    r
}

pub fn copy_supplier_deltas(rows: &[SupplierDelta]) -> (r: Vec<SupplierDelta>)
    ensures r@ == rows@,
{
    let mut r: Vec<SupplierDelta> = Vec::new();
    let mut i = 0;
    while i < rows.len()
        invariant i <= rows.len(), r@ == rows@.take(i as int),
        decreases rows.len() - i,
    {
        r.push(rows[i]);
        i += 1;
    }
    proof { assert(rows@.take(rows.len() as int) =~= rows@); }
    r
}

pub fn copy_partsupp_deltas(rows: &[PartsuppDelta]) -> (r: Vec<PartsuppDelta>)
    ensures r@ == rows@,
{
    let mut r: Vec<PartsuppDelta> = Vec::new();
    let mut i = 0;
    while i < rows.len()
        invariant i <= rows.len(), r@ == rows@.take(i as int),
        decreases rows.len() - i,
    {
        r.push(rows[i]);
        i += 1;
    }
    proof { assert(rows@.take(rows.len() as int) =~= rows@); }
    r
}

pub fn copy_line_deltas(rows: &[LineDelta]) -> (r: Vec<LineDelta>)
    ensures r@ == rows@,
{
    let mut r: Vec<LineDelta> = Vec::new();
    let mut i = 0;
    while i < rows.len()
        invariant i <= rows.len(), r@ == rows@.take(i as int),
        decreases rows.len() - i,
    {
        r.push(rows[i]);
        i += 1;
    }
    proof { assert(rows@.take(rows.len() as int) =~= rows@); }
    r
}

/// The logs of the scans of db.
pub fn raw_build(all_parts: &Vec<PartIn>, all_orders: &[OrderIn], supplier_nations: &[SupplierIn], nation_names: &Vec<NationIn>,
    all_partsupp: &[PartsuppIn], all_lines: &[LineIn], Ghost(db): Ghost<Db>) -> (r: Raw)
    requires
        scan_all_parts_ok(db, all_parts@), scan_all_orders_ok(db, all_orders@), scan_supplier_nations_ok(db, supplier_nations@),
        scan_nation_names_ok(db, nation_names@), scan_all_partsupp_ok(db, all_partsupp@), scan_all_lines_ok(db, all_lines@),
    ensures raw_of(r, db),
{
    let parts = Log::empty().extended(part_events(all_parts));
    let orders = Log::empty().extended(order_events(all_orders));
    let suppliers = Log::empty().extended(supplier_events(supplier_nations));
    let nations = Log::empty().extended(nation_events(nation_names));
    let partsupp = Log::empty().extended(partsupp_events(all_partsupp));
    let lines = Log::empty().extended(line_events(all_lines));
    proof {
        lemma_scan_built(db.part, part_in_seq_view(all_parts@), part_delta_seq_view(parts.events()));
        lemma_scan_built(db.orders, order_in_seq_view(all_orders@), order_delta_seq_view(orders.events()));
        lemma_scan_built(db.supplier, supplier_in_seq_view(supplier_nations@), supplier_delta_seq_view(suppliers.events()));
        lemma_scan_built(db.nation, nation_in_seq_view(nation_names@), nation_delta_seq_view(nations.events()));
        lemma_scan_built(db.partsupp, partsupp_in_seq_view(all_partsupp@), partsupp_delta_seq_view(partsupp.events()));
        lemma_scan_built(db.lineitem, line_in_seq_view(all_lines@), line_delta_seq_view(lines.events()));
    }
    Raw { parts, orders, suppliers, nations, partsupp, lines }
}

/// The logs after a transaction.
pub fn raw_apply(raw: &Raw, lineitem_delta: &[LineDelta], nation_delta: &Vec<NationDelta>, orders_delta: &[OrderDelta],
    part_delta: &Vec<PartDelta>, partsupp_delta: &[PartsuppDelta], supplier_delta: &[SupplierDelta], Ghost(db): Ghost<Db>, Ghost(db2): Ghost<Db>) -> (r: Raw)
    requires raw_of(*raw, db),
        bag_step(db.lineitem, db2.lineitem, line_delta_seq_view(lineitem_delta@)),
        bag_step(db.nation, db2.nation, nation_delta_seq_view(nation_delta@)),
        bag_step(db.orders, db2.orders, order_delta_seq_view(orders_delta@)),
        bag_step(db.part, db2.part, part_delta_seq_view(part_delta@)),
        bag_step(db.partsupp, db2.partsupp, partsupp_delta_seq_view(partsupp_delta@)),
        bag_step(db.supplier, db2.supplier, supplier_delta_seq_view(supplier_delta@)),
    ensures raw_of(r, db2),
{
    let parts = raw.parts.extended(copy_part_deltas(part_delta));
    let orders = raw.orders.extended(copy_order_deltas(orders_delta));
    let suppliers = raw.suppliers.extended(copy_supplier_deltas(supplier_delta));
    let nations = raw.nations.extended(copy_nation_deltas(nation_delta));
    let partsupp = raw.partsupp.extended(copy_partsupp_deltas(partsupp_delta));
    let lines = raw.lines.extended(copy_line_deltas(lineitem_delta));
    proof {
        lemma_step_built(db.part, db2.part, part_delta_seq_view(raw.parts.events()), part_delta_seq_view(part_delta@));
        lemma_step_built(db.orders, db2.orders, order_delta_seq_view(raw.orders.events()), order_delta_seq_view(orders_delta@));
        lemma_step_built(db.supplier, db2.supplier, supplier_delta_seq_view(raw.suppliers.events()), supplier_delta_seq_view(supplier_delta@));
        lemma_step_built(db.nation, db2.nation, nation_delta_seq_view(raw.nations.events()), nation_delta_seq_view(nation_delta@));
        lemma_step_built(db.partsupp, db2.partsupp, partsupp_delta_seq_view(raw.partsupp.events()), partsupp_delta_seq_view(partsupp_delta@));
        lemma_step_built(db.lineitem, db2.lineitem, line_delta_seq_view(raw.lines.events()), line_delta_seq_view(lineitem_delta@));
        assert(part_delta_seq_view(parts.events()) =~= part_delta_seq_view(raw.parts.events()) + part_delta_seq_view(part_delta@));
        assert(order_delta_seq_view(orders.events()) =~= order_delta_seq_view(raw.orders.events()) + order_delta_seq_view(orders_delta@));
        assert(supplier_delta_seq_view(suppliers.events()) =~= supplier_delta_seq_view(raw.suppliers.events()) + supplier_delta_seq_view(supplier_delta@));
        assert(nation_delta_seq_view(nations.events()) =~= nation_delta_seq_view(raw.nations.events()) + nation_delta_seq_view(nation_delta@));
        assert(partsupp_delta_seq_view(partsupp.events()) =~= partsupp_delta_seq_view(raw.partsupp.events()) + partsupp_delta_seq_view(partsupp_delta@));
        assert(line_delta_seq_view(lines.events()) =~= line_delta_seq_view(raw.lines.events()) + line_delta_seq_view(lineitem_delta@));
    }
    Raw { parts, orders, suppliers, nations, partsupp, lines }
}

}
