use vstd::prelude::*;
use verus_common::sums::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::model::*;
use crate::maintenance::Scratch;

verus! {
pub proof fn lemma_empty_revenue_bags(bags: Seq<Seq<Revenue0Tuple>>)
    requires forall|position: int| 0 <= position < bags.len() ==> (#[trigger] bags[position]).len() == 0,
    ensures verus_common::rel::flat(bags).len() == 0,
    decreases bags.len(),
{
    if bags.len() > 0 {
        assert forall|position: int| 0 <= position < bags.drop_last().len() implies
            (#[trigger] bags.drop_last()[position]).len() == 0 by { assert(bags.drop_last()[position] == bags[position]); }
        lemma_empty_revenue_bags(bags.drop_last());
    }
}
pub proof fn lemma_empty_query_bags(bags: Seq<Seq<Tuple>>)
    requires forall|position: int| 0 <= position < bags.len() ==> (#[trigger] bags[position]).len() == 0,
    ensures verus_common::rel::flat(bags).len() == 0,
    decreases bags.len(),
{
    if bags.len() > 0 {
        assert forall|position: int| 0 <= position < bags.drop_last().len() implies
            (#[trigger] bags.drop_last()[position]).len() == 0 by { assert(bags.drop_last()[position] == bags[position]); }
        lemma_empty_query_bags(bags.drop_last());
    }
}
pub proof fn lemma_empty_range(db: Db, parameters: Params)
    requires parameters.p1 >= parameters.p2,
    ensures query_result(db, parameters, Seq::<OutRow>::empty()),
{
    let bags = db.lineitem.map_values(|row: LineitemRow| revenue0_tuples_1(db, parameters, row));
    assert forall|position: int| 0 <= position < bags.len() implies (#[trigger] bags[position]).len() == 0 by {
        assert(bags[position] == revenue0_tuples_1(db, parameters, db.lineitem[position]));
        assert(!revenue0_where_clause(db, parameters, db.lineitem[position]));
    }
    lemma_empty_revenue_bags(bags);
    assert(revenue0_tuples(db, parameters).len() == 0);
    assert(rel_revenue0(db, parameters).len() == 0) by { reveal(verus_common::rel::dedup); reveal(verus_common::order::map_where); }
    let query_bags = db.supplier.map_values(|supplier: SupplierRow| tuples_1(db, parameters, supplier));
    assert forall|position: int| 0 <= position < query_bags.len() implies (#[trigger] query_bags[position]).len() == 0 by {
        assert(query_bags[position] == tuples_1(db, parameters, db.supplier[position]));
    }
    lemma_empty_query_bags(query_bags);
    assert(rows(db, parameters).len() == 0);
    assert(rows(db, parameters) =~= Seq::<OutRow>::empty());
}
pub proof fn lemma_scratch_lines(scratch: Scratch, inputs: Seq<LineInput>)
    requires scratch.matches_lines(line_input_seq_view(inputs)),
    ensures forall|supplier: int, date: int| #[trigger] scratch.count_int(supplier, date) == day_count(line_input_seq_view(inputs), supplier, date),
        forall|supplier: int, date: int| #[trigger] scratch.revenue_int(supplier, date) == day_revenue(line_input_seq_view(inputs), supplier, date),
{
    let rows = line_input_seq_view(inputs);
    assert forall|supplier: int, date: int| #[trigger] scratch.count_int(supplier, date) == day_count(rows, supplier, date) by {
        if i64::MIN <= supplier <= i64::MAX && i32::MIN <= date <= i32::MAX {
            assert(scratch.count(supplier as i64, date as i32) == day_count(rows, supplier, date));
        } else {
            assert forall|position: int| 0 <= position < rows.len() implies
                (#[trigger] rows[position]).l_shipdate != date || rows[position].l_suppkey != supplier by {
                assert(rows[position] == line_input_view(inputs[position]));
            }
            lemma_sum_zero(rows, |row: LineitemRow| if row.l_shipdate == date && row.l_suppkey == supplier { 1int } else { 0int });
        }
    }
    assert forall|supplier: int, date: int| #[trigger] scratch.revenue_int(supplier, date) == day_revenue(rows, supplier, date) by {
        if i64::MIN <= supplier <= i64::MAX && i32::MIN <= date <= i32::MAX {
            assert(scratch.revenue(supplier as i64, date as i32) == day_revenue(rows, supplier, date));
        } else {
            assert forall|position: int| 0 <= position < rows.len() implies
                (#[trigger] rows[position]).l_shipdate != date || rows[position].l_suppkey != supplier by {
                assert(rows[position] == line_input_view(inputs[position]));
            }
            lemma_sum_zero(rows, |row: LineitemRow| if row.l_shipdate == date && row.l_suppkey == supplier { weight(row) } else { 0int });
        }
    }
}
pub proof fn lemma_scan_scratch(facts: Facts, scratch: Scratch, inputs: Seq<LineInput>)
    requires facts_shape(facts), scratch.matches_lines(line_input_seq_view(inputs)),
        forall|supplier: int, date: int| #[trigger] stored_day_count(facts, supplier, date) == scratch.count_int(supplier, date),
        forall|supplier: int, date: int| #[trigger] stored_day_revenue(facts, supplier, date) == scratch.revenue_int(supplier, date),
    ensures facts_match(facts, line_input_seq_view(inputs)),
{
    lemma_scratch_lines(scratch, inputs);
}
pub proof fn lemma_scratch_deltas(scratch: Scratch, inputs: Seq<LineDelta>)
    requires scratch.matches_delta(line_delta_seq_view(inputs)),
    ensures forall|supplier: int, date: int| #[trigger] scratch.count_int(supplier, date) ==
            sum(line_delta_seq_view(inputs), |change: (LineitemRow, int)| if change.0.l_shipdate == date && change.0.l_suppkey == supplier { change.1 } else { 0int }),
        forall|supplier: int, date: int| #[trigger] scratch.revenue_int(supplier, date) ==
            sum(line_delta_seq_view(inputs), |change: (LineitemRow, int)| if change.0.l_shipdate == date && change.0.l_suppkey == supplier { change.1 * weight(change.0) } else { 0int }),
{
    let rows = line_delta_seq_view(inputs);
    assert forall|supplier: int, date: int| #[trigger] scratch.count_int(supplier, date) ==
        sum(rows, |change: (LineitemRow, int)| if change.0.l_shipdate == date && change.0.l_suppkey == supplier { change.1 } else { 0int }) by {
        if i64::MIN <= supplier <= i64::MAX && i32::MIN <= date <= i32::MAX {
            assert(scratch.count(supplier as i64, date as i32) == sum(rows, |change: (LineitemRow, int)| if change.0.l_suppkey == supplier && change.0.l_shipdate == date { change.1 } else { 0int }));
            lemma_sum_ext_all(rows, |change: (LineitemRow, int)| if change.0.l_suppkey == supplier && change.0.l_shipdate == date { change.1 } else { 0int },
                |change: (LineitemRow, int)| if change.0.l_shipdate == date && change.0.l_suppkey == supplier { change.1 } else { 0int });
        } else {
            assert forall|position: int| 0 <= position < rows.len() implies
                (#[trigger] rows[position]).0.l_shipdate != date || rows[position].0.l_suppkey != supplier by {
                assert(rows[position].0 == line_input_view(inputs[position].row));
            }
            lemma_sum_zero(rows, |change: (LineitemRow, int)| if change.0.l_shipdate == date && change.0.l_suppkey == supplier { change.1 } else { 0int });
        }
    }
    assert forall|supplier: int, date: int| #[trigger] scratch.revenue_int(supplier, date) ==
        sum(rows, |change: (LineitemRow, int)| if change.0.l_shipdate == date && change.0.l_suppkey == supplier { change.1 * weight(change.0) } else { 0int }) by {
        if i64::MIN <= supplier <= i64::MAX && i32::MIN <= date <= i32::MAX {
            assert(scratch.revenue(supplier as i64, date as i32) == sum(rows, |change: (LineitemRow, int)| if change.0.l_suppkey == supplier && change.0.l_shipdate == date { change.1 * weight(change.0) } else { 0int }));
            lemma_sum_ext_all(rows, |change: (LineitemRow, int)| if change.0.l_suppkey == supplier && change.0.l_shipdate == date { change.1 * weight(change.0) } else { 0int },
                |change: (LineitemRow, int)| if change.0.l_shipdate == date && change.0.l_suppkey == supplier { change.1 * weight(change.0) } else { 0int });
        } else {
            assert forall|position: int| 0 <= position < rows.len() implies
                (#[trigger] rows[position]).0.l_shipdate != date || rows[position].0.l_suppkey != supplier by {
                assert(rows[position].0 == line_input_view(inputs[position].row));
            }
            lemma_sum_zero(rows, |change: (LineitemRow, int)| if change.0.l_shipdate == date && change.0.l_suppkey == supplier { change.1 * weight(change.0) } else { 0int });
        }
    }
}
pub proof fn lemma_delta_scratch(before: Facts, after: Facts, scratch: Scratch, inputs: Seq<LineDelta>)
    requires facts_shape(after), scratch.matches_delta(line_delta_seq_view(inputs)),
        forall|supplier: int, date: int| #[trigger] stored_day_count(after, supplier, date) == stored_day_count(before, supplier, date) + scratch.count_int(supplier, date),
        forall|supplier: int, date: int| #[trigger] stored_day_revenue(after, supplier, date) == stored_day_revenue(before, supplier, date) + scratch.revenue_int(supplier, date),
    ensures facts_after(before, after, line_delta_seq_view(inputs)),
{
    lemma_scratch_deltas(scratch, inputs);
}
pub proof fn lemma_build_ok(facts: Facts, dimensions: Dimensions, lines: Seq<LineInput>, suppliers: Seq<SupplierInput>, db: Db)
    requires scan_lines_ok(db, lines), scan_suppliers_ok(db, suppliers),
        facts_match(facts, line_input_seq_view(lines)), dimensions_match(dimensions, supplier_input_seq_view(suppliers)),
    ensures facts_match(facts, db.lineitem), dimensions_match(dimensions, db.supplier),
{
    assert forall|supplier: int, date: int| #[trigger] stored_day_count(facts, supplier, date) == day_count(db.lineitem, supplier, date) by {
        let count = |row: LineitemRow| if row.l_shipdate == date && row.l_suppkey == supplier { 1int } else { 0int };
        assert(sum(line_input_seq_view(lines), count) == sum(db.lineitem, count));
    }
    assert forall|supplier: int, date: int| #[trigger] stored_day_revenue(facts, supplier, date) == day_revenue(db.lineitem, supplier, date) by {
        let revenue = |row: LineitemRow| if row.l_shipdate == date && row.l_suppkey == supplier { weight(row) } else { 0int };
        assert(sum(line_input_seq_view(lines), revenue) == sum(db.lineitem, revenue));
    }
    assert forall|supplier: int, key: verus_prooflib::ordered::KeyView| #[trigger] stored_supplier_count(dimensions, supplier, key) == supplier_count(db.supplier, supplier, key) by {
        let count = |row: SupplierRow| if row.s_suppkey == supplier && variant_key(row.s_name, row.s_address, row.s_phone) == key { 1int } else { 0int };
        assert(sum(supplier_input_seq_view(suppliers), count) == sum(db.supplier, count));
    }
}
pub open spec fn facts_after(before: Facts, after: Facts, delta: Seq<(LineitemRow, int)>) -> bool {
    &&& facts_shape(after)
    &&& forall|supplier: int, date: int| #[trigger] stored_day_count(after, supplier, date) == stored_day_count(before, supplier, date) +
        sum(delta, |change: (LineitemRow, int)| if change.0.l_shipdate == date && change.0.l_suppkey == supplier { change.1 } else { 0int })
    &&& forall|supplier: int, date: int| #[trigger] stored_day_revenue(after, supplier, date) == stored_day_revenue(before, supplier, date) +
        sum(delta, |change: (LineitemRow, int)| if change.0.l_shipdate == date && change.0.l_suppkey == supplier { change.1 * weight(change.0) } else { 0int })
}
pub open spec fn dimensions_after(before: Dimensions, after: Dimensions, delta: Seq<(SupplierRow, int)>) -> bool {
    dimensions_shape(after) && forall|supplier: int, key: verus_prooflib::ordered::KeyView| #[trigger] stored_supplier_count(after, supplier, key) ==
        stored_supplier_count(before, supplier, key) + sum(delta, |change: (SupplierRow, int)|
            if change.0.s_suppkey == supplier && variant_key(change.0.s_name, change.0.s_address, change.0.s_phone) == key { change.1 } else { 0int })
}
pub proof fn lemma_apply_ok(before: Facts, after: Facts, before_dimensions: Dimensions, after_dimensions: Dimensions,
    db: Db, db2: Db, lines: Seq<(LineitemRow, int)>, suppliers: Seq<(SupplierRow, int)>)
    requires facts_match(before, db.lineitem), dimensions_match(before_dimensions, db.supplier),
        capture_ok(db, db2, lines, suppliers), facts_after(before, after, lines), dimensions_after(before_dimensions, after_dimensions, suppliers),
    ensures facts_match(after, db2.lineitem), dimensions_match(after_dimensions, db2.supplier),
{
    assert forall|supplier: int, date: int| #[trigger] stored_day_count(after, supplier, date) == day_count(db2.lineitem, supplier, date) by {
        let count = |row: LineitemRow| if row.l_shipdate == date && row.l_suppkey == supplier { 1int } else { 0int };
        assert(sum(db2.lineitem, count) == sum(db.lineitem, count) + sum(lines, |change: (LineitemRow, int)| change.1 * count(change.0)));
        lemma_sum_ext_all(lines, |change: (LineitemRow, int)| change.1 * count(change.0),
            |change: (LineitemRow, int)| if change.0.l_shipdate == date && change.0.l_suppkey == supplier { change.1 } else { 0int });
    }
    assert forall|supplier: int, date: int| #[trigger] stored_day_revenue(after, supplier, date) == day_revenue(db2.lineitem, supplier, date) by {
        let revenue = |row: LineitemRow| if row.l_shipdate == date && row.l_suppkey == supplier { weight(row) } else { 0int };
        assert(sum(db2.lineitem, revenue) == sum(db.lineitem, revenue) + sum(lines, |change: (LineitemRow, int)| change.1 * revenue(change.0)));
        lemma_sum_ext_all(lines, |change: (LineitemRow, int)| change.1 * revenue(change.0),
            |change: (LineitemRow, int)| if change.0.l_shipdate == date && change.0.l_suppkey == supplier { change.1 * weight(change.0) } else { 0int });
    }
    assert forall|supplier: int, key: verus_prooflib::ordered::KeyView| #[trigger] stored_supplier_count(after_dimensions, supplier, key) == supplier_count(db2.supplier, supplier, key) by {
        let count = |row: SupplierRow| if row.s_suppkey == supplier && variant_key(row.s_name, row.s_address, row.s_phone) == key { 1int } else { 0int };
        assert(sum(db2.supplier, count) == sum(db.supplier, count) + sum(suppliers, |change: (SupplierRow, int)| change.1 * count(change.0)));
        lemma_sum_ext_all(suppliers, |change: (SupplierRow, int)| change.1 * count(change.0),
            |change: (SupplierRow, int)| if change.0.s_suppkey == supplier && variant_key(change.0.s_name, change.0.s_address, change.0.s_phone) == key { change.1 } else { 0int });
    }
}
pub proof fn lemma_delta_target(db: Db, db2: Db, lines: Seq<(LineitemRow, int)>, suppliers: Seq<(SupplierRow, int)>)
    requires capture_ok(db, db2, lines, suppliers),
    ensures forall|supplier: int, date: int| #[trigger] day_count(db2.lineitem, supplier, date) == day_count(db.lineitem, supplier, date) +
            sum(lines, |change: (LineitemRow, int)| if change.0.l_shipdate == date && change.0.l_suppkey == supplier { change.1 } else { 0int }),
        forall|supplier: int, date: int| #[trigger] day_revenue(db2.lineitem, supplier, date) == day_revenue(db.lineitem, supplier, date) +
            sum(lines, |change: (LineitemRow, int)| if change.0.l_shipdate == date && change.0.l_suppkey == supplier { change.1 * weight(change.0) } else { 0int }),
        forall|supplier: int, key: verus_prooflib::ordered::KeyView| #[trigger] supplier_count(db2.supplier, supplier, key) == supplier_count(db.supplier, supplier, key) +
            sum(suppliers, |change: (SupplierRow, int)| if change.0.s_suppkey == supplier && variant_key(change.0.s_name, change.0.s_address, change.0.s_phone) == key { change.1 } else { 0int }),
{
    assert forall|supplier: int, date: int| #[trigger] day_count(db2.lineitem, supplier, date) == day_count(db.lineitem, supplier, date) +
        sum(lines, |change: (LineitemRow, int)| if change.0.l_shipdate == date && change.0.l_suppkey == supplier { change.1 } else { 0int }) by {
        let count = |row: LineitemRow| if row.l_shipdate == date && row.l_suppkey == supplier { 1int } else { 0int };
        assert(sum(db2.lineitem, count) == sum(db.lineitem, count) + sum(lines, |change: (LineitemRow, int)| change.1 * count(change.0)));
        lemma_sum_ext_all(lines, |change: (LineitemRow, int)| change.1 * count(change.0),
            |change: (LineitemRow, int)| if change.0.l_shipdate == date && change.0.l_suppkey == supplier { change.1 } else { 0int });
    }
    assert forall|supplier: int, date: int| #[trigger] day_revenue(db2.lineitem, supplier, date) == day_revenue(db.lineitem, supplier, date) +
        sum(lines, |change: (LineitemRow, int)| if change.0.l_shipdate == date && change.0.l_suppkey == supplier { change.1 * weight(change.0) } else { 0int }) by {
        let revenue = |row: LineitemRow| if row.l_shipdate == date && row.l_suppkey == supplier { weight(row) } else { 0int };
        assert(sum(db2.lineitem, revenue) == sum(db.lineitem, revenue) + sum(lines, |change: (LineitemRow, int)| change.1 * revenue(change.0)));
        lemma_sum_ext_all(lines, |change: (LineitemRow, int)| change.1 * revenue(change.0),
            |change: (LineitemRow, int)| if change.0.l_shipdate == date && change.0.l_suppkey == supplier { change.1 * weight(change.0) } else { 0int });
    }
    assert forall|supplier: int, key: verus_prooflib::ordered::KeyView| #[trigger] supplier_count(db2.supplier, supplier, key) == supplier_count(db.supplier, supplier, key) +
        sum(suppliers, |change: (SupplierRow, int)| if change.0.s_suppkey == supplier && variant_key(change.0.s_name, change.0.s_address, change.0.s_phone) == key { change.1 } else { 0int }) by {
        let count = |row: SupplierRow| if row.s_suppkey == supplier && variant_key(row.s_name, row.s_address, row.s_phone) == key { 1int } else { 0int };
        assert(sum(db2.supplier, count) == sum(db.supplier, count) + sum(suppliers, |change: (SupplierRow, int)| change.1 * count(change.0)));
        lemma_sum_ext_all(suppliers, |change: (SupplierRow, int)| change.1 * count(change.0),
            |change: (SupplierRow, int)| if change.0.s_suppkey == supplier && variant_key(change.0.s_name, change.0.s_address, change.0.s_phone) == key { change.1 } else { 0int });
    }
}
pub proof fn lemma_execute_ok(db: Db, parameters: Params, output: Seq<OutRow>)
    requires output.to_multiset() == rows(db, parameters).to_multiset(),
        forall|left: int, right: int| #![trigger output[left], output[right]]
            0 <= left < right < output.len() ==> output[left].s_suppkey <= output[right].s_suppkey,
    ensures query_result(db, parameters, output),
{
}
}
