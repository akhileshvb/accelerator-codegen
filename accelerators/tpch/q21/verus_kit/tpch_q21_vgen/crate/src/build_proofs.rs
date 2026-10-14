use vstd::prelude::*;
use verus_common::sums::*;
use crate::aggregates::*;
use crate::gen_obligations::*;
use crate::gen_spec::*;
use crate::index::*;

verus! {

pub open spec fn finished_weight(key: KeyView) -> spec_fn(FinishedOrder) -> int {
    |row: FinishedOrder| if key == (row.o_orderkey as int, Seq::<u8>::empty()) { 1 } else { 0 }
}

pub open spec fn waiting_weight(finished: Counts, key: KeyView) -> spec_fn(WaitingLine) -> int {
    |row: WaitingLine| if key == (row.l_suppkey as int, Seq::<u8>::empty()) {
        count(finished, (row.l_orderkey as int, Seq::<u8>::empty()))
    } else { 0 }
}

pub open spec fn supplier_weight(outer: KeyView, inner: KeyView) -> spec_fn(SupplierScan) -> int {
    |row: SupplierScan| if outer == (row.s_suppkey as int, Seq::<u8>::empty()) && inner == (row.s_nationkey as int, row.s_name@) { 1 } else { 0 }
}

pub open spec fn bucket_weight(waiting: Counts, outer: KeyView, inner: KeyView) -> spec_fn(SupplierScan) -> int {
    |row: SupplierScan| if outer == (row.s_nationkey as int, Seq::<u8>::empty()) && inner == (0int, row.s_name@) {
        count(waiting, (row.s_suppkey as int, Seq::<u8>::empty()))
    } else { 0 }
}

pub open spec fn supplier_name_weight(key: KeyView) -> spec_fn(SupplierScan) -> int {
    |row: SupplierScan| if key == (0int, row.s_name@) { 1 } else { 0 }
}

pub open spec fn nation_weight(outer: KeyView, inner: KeyView) -> spec_fn(NationScan) -> int {
    |row: NationScan| if outer == (0int, row.n_name@) && inner == (row.n_nationkey as int, Seq::<u8>::empty()) { 1 } else { 0 }
}

pub open spec fn nation_name_weight(key: KeyView) -> spec_fn(NationScan) -> int {
    |row: NationScan| if key == (0int, row.n_name@) { 1 } else { 0 }
}

pub proof fn lemma_finished_line(db: Db, rows: Seq<FinishedOrder>, line_position: int)
    requires
        scan_finished_orders_ok(db, rows),
        0 <= line_position < db.lineitem.len(),
        db.lineitem[line_position].l_receiptdate > db.lineitem[line_position].l_commitdate,
        exists_1(db, db.lineitem[line_position]),
        !exists_2(db, db.lineitem[line_position]),
    ensures sum(rows, finished_weight((db.lineitem[line_position].l_orderkey, Seq::<u8>::empty()))) == finished(db, db.lineitem[line_position].l_orderkey),
{
    let line = db.lineitem[line_position];
    let weight = |order: int| if order == line.l_orderkey { 1int } else { 0int };
    let raw = finished_weight((line.l_orderkey, Seq::<u8>::empty()));
    lemma_sum_map(rows, |row: FinishedOrder| finished_order_view(row), weight, raw);
    assert(sum(finished_order_seq_view(rows), weight) == sum(db.orders, |order: OrdersRow|
        if order.o_orderstatus == seq![b'F'] && scan_finished_orders_exists_1(db, order) { weight(order.o_orderkey) } else { 0 }));
    let scanned = |order: OrdersRow| if order.o_orderstatus == seq![b'F'] && scan_finished_orders_exists_1(db, order) { weight(order.o_orderkey) } else { 0 };
    let full = |order: OrdersRow| if order.o_orderkey == line.l_orderkey && order.o_orderstatus == seq![b'F'] { 1int } else { 0int };
    assert forall|position: int| #![trigger db.orders[position]] 0 <= position < db.orders.len() implies scanned(db.orders[position]) == full(db.orders[position]) by {
        let order = db.orders[position];
        if order.o_orderkey == line.l_orderkey {
            assert(scan_finished_orders_exists_1_where(db, order, line));
            assert(scan_finished_orders_exists_1(db, order));
        }
    }
    lemma_sum_ext(db.orders, scanned, full);
    lemma_sum_ext(db.orders, full, |order: OrdersRow| if order.o_orderkey == line.l_orderkey && order.o_orderstatus == seq![b'F'] { 1 } else { 0 });
}

pub proof fn lemma_waiting_scan(db: Db, rows: Seq<WaitingLine>, finished_rows: Seq<FinishedOrder>, finished_index: &Counts, key: KeyView)
    requires
        scan_waiting_lines_ok(db, rows), scan_finished_orders_ok(db, finished_rows),
        forall|query: KeyView| #![trigger count(*finished_index, query)] count(*finished_index, query) == sum(finished_rows, finished_weight(query)),
    ensures sum(rows, waiting_weight(*finished_index, key)) == if key.1 == Seq::<u8>::empty() { waiting(db, key.0) } else { 0 },
{
    let weight = |row: (int, int)| if key == (row.1, Seq::<u8>::empty()) { count(*finished_index, (row.0, Seq::<u8>::empty())) } else { 0 };
    lemma_sum_map(rows, |row: WaitingLine| waiting_line_view(row), weight, waiting_weight(*finished_index, key));
    let scanned = |line: LineitemRow| if line.l_receiptdate > line.l_commitdate && scan_waiting_lines_exists_1(db, line)
        && scan_waiting_lines_exists_2(db, line) && !scan_waiting_lines_exists_3(db, line) { weight((line.l_orderkey, line.l_suppkey)) } else { 0 };
    assert(sum(waiting_line_seq_view(rows), weight) == sum(db.lineitem, scanned));
    let full = |line: LineitemRow| if key.1 == Seq::<u8>::empty() && line.l_suppkey == key.0
        && line.l_receiptdate > line.l_commitdate && exists_1(db, line) && !exists_2(db, line) { finished(db, line.l_orderkey) } else { 0 };
    assert forall|position: int| #![trigger db.lineitem[position]] 0 <= position < db.lineitem.len() implies scanned(db.lineitem[position]) == full(db.lineitem[position]) by {
        let line = db.lineitem[position];
        if line.l_receiptdate > line.l_commitdate && exists_1(db, line) && !exists_2(db, line) {
            lemma_finished_line(db, finished_rows, position);
            if !scan_waiting_lines_exists_1(db, line) {
                let term = |order: OrdersRow| if order.o_orderkey == line.l_orderkey && order.o_orderstatus == seq![b'F'] { 1int } else { 0int };
                assert forall|order_position: int| #![trigger db.orders[order_position]] 0 <= order_position < db.orders.len() implies term(db.orders[order_position]) == 0 by {}
                lemma_sum_zero(db.orders, term);
            }
        }
    }
    lemma_sum_ext(db.lineitem, scanned, full);
    if key.1 == Seq::<u8>::empty() {
        lemma_sum_ext(db.lineitem, full, |line: LineitemRow| if line.l_suppkey == key.0
            && line.l_receiptdate > line.l_commitdate && exists_1(db, line) && !exists_2(db, line) { finished(db, line.l_orderkey) } else { 0 });
    } else {
        lemma_sum_zero(db.lineitem, full);
    }
}

pub proof fn lemma_supplier_scan(db: Db, rows: Seq<SupplierScan>, waiting_index: &Counts, outer: KeyView, inner: KeyView)
    requires scan_suppliers_ok(db, rows),
        forall|key: KeyView| #![trigger count(*waiting_index, key)] count(*waiting_index, key) == if key.1 == Seq::<u8>::empty() { waiting(db, key.0) } else { 0 },
    ensures
        sum(rows, supplier_weight(outer, inner)) == if outer.1 == Seq::<u8>::empty() { supplier_multiplicity(db, outer.0, inner.0, inner.1) } else { 0 },
        sum(rows, bucket_weight(*waiting_index, outer, inner)) == if outer.1 == Seq::<u8>::empty() && inner.0 == 0 { bucket(db, outer.0, inner.1) } else { 0 },
{
    let supplier_term = |row: SupplierRow| if outer == (row.s_suppkey, Seq::<u8>::empty()) && inner == (row.s_nationkey, row.s_name) { 1int } else { 0int };
    let bucket_term = |row: SupplierRow| if outer == (row.s_nationkey, Seq::<u8>::empty()) && inner == (0int, row.s_name) { waiting(db, row.s_suppkey) } else { 0 };
    lemma_sum_map(rows, |row: SupplierScan| supplier_scan_view(row), supplier_term, supplier_weight(outer, inner));
    lemma_sum_map(rows, |row: SupplierScan| supplier_scan_view(row), bucket_term, bucket_weight(*waiting_index, outer, inner));
    assert(sum(supplier_scan_seq_view(rows), supplier_term) == sum(db.supplier, supplier_term));
    assert(sum(supplier_scan_seq_view(rows), bucket_term) == sum(db.supplier, bucket_term));
    if outer.1 == Seq::<u8>::empty() {
        lemma_sum_ext(db.supplier, supplier_term, |row: SupplierRow| if row.s_suppkey == outer.0 && row.s_nationkey == inner.0 && row.s_name == inner.1 { 1 } else { 0 });
    } else { lemma_sum_zero(db.supplier, supplier_term); }
    if outer.1 == Seq::<u8>::empty() && inner.0 == 0 {
        lemma_sum_ext(db.supplier, bucket_term, |row: SupplierRow| if row.s_nationkey == outer.0 && row.s_name == inner.1 { waiting(db, row.s_suppkey) } else { 0 });
    } else { lemma_sum_zero(db.supplier, bucket_term); }
}

pub proof fn lemma_nation_scan(db: Db, rows: Seq<NationScan>, outer: KeyView, inner: KeyView)
    requires scan_nations_ok(db, rows),
    ensures sum(rows, nation_weight(outer, inner)) == if outer.0 == 0 && inner.1 == Seq::<u8>::empty() { nation_multiplicity(db, inner.0, outer.1) } else { 0 },
{
    let term = |row: NationRow| if outer == (0int, row.n_name) && inner == (row.n_nationkey, Seq::<u8>::empty()) { 1int } else { 0int };
    lemma_sum_map(rows, |row: NationScan| nation_scan_view(row), term, nation_weight(outer, inner));
    assert(sum(nation_scan_seq_view(rows), term) == sum(db.nation, term));
    if outer.0 == 0 && inner.1 == Seq::<u8>::empty() {
        lemma_sum_ext(db.nation, term, |row: NationRow| if row.n_nationkey == inner.0 && row.n_name == outer.1 { 1 } else { 0 });
    } else { lemma_sum_zero(db.nation, term); }
}

pub proof fn lemma_name_scan(db: Db, suppliers: Seq<SupplierScan>, nations: Seq<NationScan>, key: KeyView)
    requires scan_suppliers_ok(db, suppliers), scan_nations_ok(db, nations),
    ensures sum(suppliers, supplier_name_weight(key)) + sum(nations, nation_name_weight(key)) == if key.0 == 0 { name_references(db, key.1) } else { 0 },
{
    let supplier_term = |row: SupplierRow| if key == (0int, row.s_name) { 1int } else { 0int };
    let nation_term = |row: NationRow| if key == (0int, row.n_name) { 1int } else { 0int };
    lemma_sum_map(suppliers, |row: SupplierScan| supplier_scan_view(row), supplier_term, supplier_name_weight(key));
    lemma_sum_map(nations, |row: NationScan| nation_scan_view(row), nation_term, nation_name_weight(key));
    assert(sum(supplier_scan_seq_view(suppliers), supplier_term) == sum(db.supplier, supplier_term));
    assert(sum(nation_scan_seq_view(nations), nation_term) == sum(db.nation, nation_term));
    if key.0 == 0 {
        lemma_sum_ext(db.supplier, supplier_term, |row: SupplierRow| if row.s_name == key.1 { 1 } else { 0 });
        lemma_sum_ext(db.nation, nation_term, |row: NationRow| if row.n_name == key.1 { 1 } else { 0 });
    } else {
        lemma_sum_zero(db.supplier, supplier_term);
        lemma_sum_zero(db.nation, nation_term);
    }
}

}
