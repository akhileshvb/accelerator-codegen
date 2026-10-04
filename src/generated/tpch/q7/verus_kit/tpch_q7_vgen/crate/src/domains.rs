use vstd::prelude::*;
use verus_common::sums::*;
use verus_prooflib::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;

verus! {

pub open spec fn key_range(value: int) -> bool { i64::MIN as int <= value <= i64::MAX as int }
pub open spec fn supplier_domain(row: SupplierRow) -> bool { key_range(row.s_suppkey) && key_range(row.s_nationkey) }
pub open spec fn customer_domain(row: CustomerRow) -> bool { key_range(row.c_custkey) && key_range(row.c_nationkey) }
pub open spec fn nation_domain(row: NationRow) -> bool { key_range(row.n_nationkey) }
pub open spec fn line_domain(row: LineitemRow) -> bool {
    !(9131 <= row.l_shipdate <= 9861) || (key_range(row.l_orderkey) && key_range(row.l_suppkey)
        && key_range(row.l_extendedprice) && key_range(row.l_discount))
}

pub open spec fn database_domain(db: Db) -> bool {
    &&& forall|index: int| #![trigger db.supplier[index]] 0 <= index < db.supplier.len() ==> supplier_domain(db.supplier[index])
    &&& forall|index: int| #![trigger db.customer[index]] 0 <= index < db.customer.len() ==> customer_domain(db.customer[index])
    &&& forall|index: int| #![trigger db.nation[index]] 0 <= index < db.nation.len() ==> nation_domain(db.nation[index])
    &&& forall|index: int| #![trigger db.lineitem[index]] 0 <= index < db.lineitem.len() ==> line_domain(db.lineitem[index])
}

pub proof fn lemma_build_domain(db: Db, suppliers: Seq<SupplierInput>, customers: Seq<CustomerInput>,
    nations: Seq<NationInput>, lines: Seq<LineInput>)
    requires scan_suppliers_ok(db, suppliers), scan_customers_ok(db, customers), scan_nations_ok(db, nations), scan_window_lines_ok(db, lines),
    ensures database_domain(db),
{
    lemma_scan_property(supplier_input_seq_view(suppliers), db.supplier, |row: SupplierRow| supplier_domain(row));
    lemma_scan_property(customer_input_seq_view(customers), db.customer, |row: CustomerRow| customer_domain(row));
    lemma_scan_property(nation_input_seq_view(nations), db.nation, |row: NationRow| nation_domain(row));
    let bad = |row: LineitemRow| if line_domain(row) { 0int } else { 1int };
    lemma_sum_zero(line_input_seq_view(lines), bad);
    let filtered = |row: LineitemRow| if 9131 <= row.l_shipdate && row.l_shipdate <= 9861 { bad(row) } else { 0int };
    assert(sum(line_input_seq_view(lines), bad) == sum(db.lineitem, filtered));
    assert forall|index: int| #![trigger db.lineitem[index]] 0 <= index < db.lineitem.len() implies line_domain(db.lineitem[index]) by {
        if !line_domain(db.lineitem[index]) { lemma_sum_pos(db.lineitem, filtered, index); }
    }
}

pub proof fn lemma_capture_domain(db: Db, db2: Db, suppliers: Seq<SupplierDelta>, customers: Seq<CustomerDelta>,
    nations: Seq<NationDelta>, lines: Seq<LineDelta>, full_lines: Seq<(LineitemRow, int)>, orders: Seq<(OrdersRow, int)>)
    requires database_domain(db), lineitem_delta_ok(lines, full_lines),
        capture_ok(db, db2, customer_delta_seq_view(customers), full_lines, nation_delta_seq_view(nations), orders, supplier_delta_seq_view(suppliers)),
    ensures database_domain(db2),
{
    lemma_bag_step_preserves_predicate(db.supplier, db2.supplier, supplier_delta_seq_view(suppliers), |row: SupplierRow| supplier_domain(row));
    lemma_bag_step_preserves_predicate(db.customer, db2.customer, customer_delta_seq_view(customers), |row: CustomerRow| customer_domain(row));
    lemma_bag_step_preserves_predicate(db.nation, db2.nation, nation_delta_seq_view(nations), |row: NationRow| nation_domain(row));
    assert forall|index: int| #![trigger full_lines[index]] 0 <= index < full_lines.len() implies line_domain(full_lines[index].0) by {
        assert(0 <= index < lines.len());
        assert(full_lines[index].0.l_orderkey == lines[index].l_orderkey as int);
        assert(full_lines[index].0.l_suppkey == lines[index].l_suppkey as int);
        assert(full_lines[index].0.l_extendedprice == lines[index].l_extendedprice as int);
        assert(full_lines[index].0.l_discount == lines[index].l_discount as int);
    }
    lemma_bag_step_preserves_predicate(db.lineitem, db2.lineitem, full_lines, |row: LineitemRow| line_domain(row));
}

}
