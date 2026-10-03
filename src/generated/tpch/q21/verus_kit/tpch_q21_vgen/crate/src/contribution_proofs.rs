use vstd::prelude::*;
use verus_common::sums::*;
use crate::gen_spec::*;
use crate::aggregates::*;
use crate::index::*;
use crate::maintenance::*;
use crate::neighborhood_proofs::*;

verus! {

pub open spec fn summary_db(summary: OrderSummary, db: Db, order: int) -> bool {
    &&& summary.lines.wf()
    &&& summary.finished as int == finished(db, order)
    &&& forall|supplier: KeyView| #![trigger line_total(summary, supplier)] #![trigger line_late(summary, supplier)] {
        &&& line_total(summary, supplier) == if supplier.1 == Seq::<u8>::empty() { total_lines(db, order, supplier.0) } else { 0 }
        &&& line_late(summary, supplier) == if supplier.1 == Seq::<u8>::empty() { late_lines(db, order, supplier.0) } else { 0 }
    }
}

pub proof fn lemma_line_counts(db: Db, order: int, supplier: int)
    ensures 0 <= late_lines(db, order, supplier) <= total_lines(db, order, supplier), finished(db, order) >= 0,
{
    lemma_sum_nonneg(db.lineitem, late_weight(order, supplier));
    lemma_sum_le(db.lineitem, late_weight(order, supplier), total_weight(order, supplier));
    lemma_sum_nonneg(db.orders, |row: OrdersRow| if row.o_orderkey == order && row.o_orderstatus == seq![b'F'] { 1int } else { 0int });
}

pub proof fn lemma_summary_row(summary: &OrderSummary, db: Db, order: int, position: int)
    requires summary_db(*summary, db, order), 0 <= position < summary.lines.entries().len(),
    ensures summary.lines.entries()[position].1.total >= 0,
        0 <= summary.lines.entries()[position].1.late <= summary.lines.entries()[position].1.total,
        summary.lines.entries()[position].1.late > 0 || summary.lines.entries()[position].1.total > 0 ==> summary.lines.entries()[position].0.1 == Seq::<u8>::empty(),
{
    summary.lines.model();
    let key = summary.lines.entries()[position].0;
    lemma_line_counts(db, order, key.0);
    assert(line_total(*summary, key) == summary.lines.entries()[position].1.total as int);
    assert(line_late(*summary, key) == summary.lines.entries()[position].1.late as int);
}

#[verifier::rlimit(20)]
pub proof fn lemma_summary_others(summary: &OrderSummary, db: Db, order: int, supplier: int)
    requires summary_db(*summary, db, order),
    ensures other_late(db, order, supplier) >= 0,
        (other_total(db, order, supplier) > 0) == (exists|position: int| #![trigger summary.lines.entries()[position]]
            0 <= position < summary.lines.entries().len() && summary.lines.entries()[position].0.0 != supplier && summary.lines.entries()[position].1.total > 0),
        (other_late(db, order, supplier) > 0) == (exists|position: int| #![trigger summary.lines.entries()[position]]
            0 <= position < summary.lines.entries().len() && summary.lines.entries()[position].0.0 != supplier && summary.lines.entries()[position].1.late > 0),
{
    summary.lines.model();
    let total_predicate = |row: LineitemRow| row.l_orderkey == order && row.l_suppkey != supplier;
    let late_predicate = |row: LineitemRow| row.l_orderkey == order && row.l_suppkey != supplier && row.l_receiptdate > row.l_commitdate;
    lemma_indicator_exists(db.lineitem, total_predicate);
    lemma_indicator_exists(db.lineitem, late_predicate);
    lemma_sum_ext(db.lineitem, |row: LineitemRow| if total_predicate(row) { 1int } else { 0int },
        |row: LineitemRow| if row.l_orderkey == order && row.l_suppkey != supplier { 1int } else { 0int });
    lemma_sum_ext(db.lineitem, |row: LineitemRow| if late_predicate(row) { 1int } else { 0int },
        |row: LineitemRow| if row.l_orderkey == order && row.l_suppkey != supplier && row.l_receiptdate > row.l_commitdate { 1int } else { 0int });
    if other_total(db, order, supplier) > 0 {
        let source = choose|source: int| #![trigger db.lineitem[source]] 0 <= source < db.lineitem.len() && total_predicate(db.lineitem[source]);
        let other = db.lineitem[source].l_suppkey;
        lemma_sum_ge_term(db.lineitem, total_weight(order, other), source);
        let key = (other, Seq::<u8>::empty());
        assert(line_total(*summary, key) > 0);
        assert(summary.lines.at(key).is_some());
        let position = choose|position: int| #![auto] 0 <= position < summary.lines.entries().len() && summary.lines.entries()[position].0 == key;
        assert(summary.lines.entries()[position].1.total > 0);
    }
    if other_late(db, order, supplier) > 0 {
        let source = choose|source: int| #![trigger db.lineitem[source]] 0 <= source < db.lineitem.len() && late_predicate(db.lineitem[source]);
        let other = db.lineitem[source].l_suppkey;
        lemma_sum_ge_term(db.lineitem, late_weight(order, other), source);
        let key = (other, Seq::<u8>::empty());
        assert(line_late(*summary, key) > 0);
        assert(summary.lines.at(key).is_some());
        let position = choose|position: int| #![auto] 0 <= position < summary.lines.entries().len() && summary.lines.entries()[position].0 == key;
        assert(summary.lines.entries()[position].1.late > 0);
    }
    assert forall|position: int| #![trigger summary.lines.entries()[position]] 0 <= position < summary.lines.entries().len()
        && summary.lines.entries()[position].0.0 != supplier && summary.lines.entries()[position].1.total > 0 implies other_total(db, order, supplier) > 0 by {
        lemma_summary_row(summary, db, order, position);
        let other = summary.lines.entries()[position].0.0;
        let key = summary.lines.entries()[position].0;
        assert(key.1 == Seq::<u8>::empty());
        assert(line_total(*summary, key) == summary.lines.entries()[position].1.total as int);
        assert(total_lines(db, order, other) > 0);
        lemma_sum_le(db.lineitem, total_weight(order, other), |row: LineitemRow| if total_predicate(row) { 1int } else { 0int });
    }
    assert forall|position: int| #![trigger summary.lines.entries()[position]] 0 <= position < summary.lines.entries().len()
        && summary.lines.entries()[position].0.0 != supplier && summary.lines.entries()[position].1.late > 0 implies other_late(db, order, supplier) > 0 by {
        lemma_summary_row(summary, db, order, position);
        let other = summary.lines.entries()[position].0.0;
        let key = summary.lines.entries()[position].0;
        assert(key.1 == Seq::<u8>::empty());
        assert(line_late(*summary, key) == summary.lines.entries()[position].1.late as int);
        assert(late_lines(db, order, other) > 0);
        lemma_sum_le(db.lineitem, late_weight(order, other), |row: LineitemRow| if late_predicate(row) { 1int } else { 0int });
    }
}

pub open spec fn contribution_value(value: Option<(i64, i128)>, supplier: KeyView) -> int {
    match value { Some((key, count)) => if supplier == (key as int, Seq::<u8>::empty()) { count as int } else { 0 }, None => 0 }
}

}
