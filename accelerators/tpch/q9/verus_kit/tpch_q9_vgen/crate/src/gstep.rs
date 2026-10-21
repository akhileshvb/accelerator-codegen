// PartAgg after a transaction: the moments of db2 from those of db, the delta lines and the
// rescanned lines of the changed orders.
use vstd::prelude::*;
use verus_common::sums::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::meaning::*;
use crate::storage::*;
use crate::pmap::*;
use crate::gagg::*;
use crate::gidx::*;
use crate::bagsig::signed;
use crate::applyspec::*;

verus! {

broadcast use vstd::seq_lib::group_seq_properties;

pub open spec fn in_keys(keys: Seq<i64>) -> spec_fn(LineitemRow) -> bool {
    |l: LineitemRow| seq_view_i64(keys).contains(l.l_orderkey)
}

pub open spec fn masked_in(keys: Seq<i64>, g: spec_fn(LineitemRow) -> int) -> spec_fn(LineitemRow) -> int {
    |l: LineitemRow| if seq_view_i64(keys).contains(l.l_orderkey) { g(l) } else { 0int }
}

pub open spec fn masked_out(keys: Seq<i64>, g: spec_fn(LineitemRow) -> int) -> spec_fn(LineitemRow) -> int {
    |l: LineitemRow| if seq_view_i64(keys).contains(l.l_orderkey) { 0int } else { g(l) }
}

pub open spec fn pair_w(k: int, s: int, d: spec_fn(int) -> int, f: int) -> spec_fn(LineitemRow) -> int {
    |l: LineitemRow| if l.l_partkey == k && l.l_suppkey == s { d(l.l_orderkey) * line_weight(l, f) } else { 0int }
}

pub open spec fn dates_fn(db: Db, y: int) -> spec_fn(int) -> int {
    |ok: int| dates(db, ok, y)
}

pub proof fn lemma_bag_f(db: Db, db2: Db, deltas: Seq<LineDelta>, f: spec_fn(LineitemRow) -> int)
    requires bag_step(db.lineitem, db2.lineitem, line_delta_seq_view(deltas)),
    ensures sum(db2.lineitem, f) == sum(db.lineitem, f) + sum(line_delta_seq_view(deltas), signed(f)),
{
    assert(signed(f) =~= (|d: (LineitemRow, int)| d.1 * f(d.0)));
}

pub proof fn lemma_rescan_g(db2: Db, keys: Seq<i64>, rows: Seq<LineIn>, g: spec_fn(LineitemRow) -> int)
    requires rescan_lines_for_orders_ok(db2, keys, rows),
    ensures sum(line_in_seq_view(rows), g) == sum(db2.lineitem, masked_in(keys, g)),
{
    assert(masked_in(keys, g) =~= (|l: LineitemRow| if seq_view_i64(keys).contains(l.l_orderkey) { g(l) } else { 0int }));
}

pub proof fn lemma_moments_alg(m1: int, m2: int, aout: int, ain: int, dout: int, din: int, d1: int, r1: int, r2: int, sout2: int, sin2_h2: int, sin2_h1: int)
    requires m2 == sout2 + sin2_h2, sout2 == aout + dout, m1 == aout + ain, sin2_h1 == ain + din, r1 == sin2_h1, r2 == sin2_h2, d1 == dout + din,
    ensures m2 == m1 + d1 + (r2 - r1),
{
}

#[verifier::spinoff_prover]
pub proof fn lemma_moments_step(db: Db, db2: Db, deltas: Seq<LineDelta>, orders: Seq<OrderDelta>, rows: Seq<LineIn>, keys: Seq<i64>,
    k: int, s: int, y: int, f: int)
    requires
        bag_step(db.lineitem, db2.lineitem, line_delta_seq_view(deltas)),
        bag_step(db.orders, db2.orders, order_delta_seq_view(orders)),
        rescan_lines_for_orders_ok(db2, keys, rows),
        keys == okeys_of(orders),
        0 <= f <= 2,
    ensures
        moments(db2, k, s, y, f) == moments(db, k, s, y, f)
            + sum(deltas, |ld: LineDelta| if ld.row.l_partkey == k && ld.row.l_suppkey == s { ld.sign as int * dates(db, ld.row.l_orderkey as int, y) * line_weight(line_in_view(ld.row), f) } else { 0int })
            + sum(rows, |r: LineIn| if r.l_partkey == k && r.l_suppkey == s { (dates(db2, r.l_orderkey as int, y) - dates(db, r.l_orderkey as int, y)) * line_weight(line_in_view(r), f) } else { 0int }),
{
    let tl = |ld: LineDelta| if ld.row.l_partkey == k && ld.row.l_suppkey == s { ld.sign as int * dates(db, ld.row.l_orderkey as int, y) * line_weight(line_in_view(ld.row), f) } else { 0int };
    let rl = |r: LineIn| if r.l_partkey == k && r.l_suppkey == s { (dates(db2, r.l_orderkey as int, y) - dates(db, r.l_orderkey as int, y)) * line_weight(line_in_view(r), f) } else { 0int };
    let h1 = pair_w(k, s, dates_fn(db, y), f);
    let h2 = pair_w(k, s, dates_fn(db2, y), f);
    let out1 = masked_out(keys, h1);
    let in1 = masked_in(keys, h1);
    let in2 = masked_in(keys, h2);
    let dl = line_delta_seq_view(deltas);
    let rv = line_in_seq_view(rows);
    let dd = |l: LineitemRow| h2(l) - h1(l);
    assert forall|l: LineitemRow| #[trigger] h2(l) == out1(l) + in2(l) by {
        if !seq_view_i64(keys).contains(l.l_orderkey) {
            lemma_dates_frame(db, db2, orders, l.l_orderkey, y);
        }
    }
    assert forall|l: LineitemRow| #[trigger] h1(l) == out1(l) + in1(l) by {}
    lemma_sum_add(db2.lineitem, out1, in2, h2);
    lemma_sum_add(db.lineitem, out1, in1, h1);
    lemma_sum_add(dl, signed(out1), signed(in1), signed(h1));
    lemma_bag_f(db, db2, deltas, out1);
    lemma_bag_f(db, db2, deltas, in1);
    lemma_rescan_g(db2, keys, rows, h2);
    lemma_rescan_g(db2, keys, rows, h1);
    lemma_sum_sub(rv, h2, h1, dd);
    lemma_moments_alg(sum(db.lineitem, h1), sum(db2.lineitem, h2), sum(db.lineitem, out1), sum(db.lineitem, in1),
        sum(dl, signed(out1)), sum(dl, signed(in1)), sum(dl, signed(h1)), sum(rv, h1), sum(rv, h2),
        sum(db2.lineitem, out1), sum(db2.lineitem, in2), sum(db2.lineitem, in1));
    assert forall|ld: LineDelta| #[trigger] tl(ld) == signed(h1)((line_in_view(ld.row), ld.sign as int)) by {
        let l = line_in_view(ld.row);
        if l.l_partkey == k && l.l_suppkey == s {
            assert(ld.sign as int * dates(db, l.l_orderkey, y) * line_weight(l, f) == ld.sign as int * (dates(db, l.l_orderkey, y) * line_weight(l, f))) by (nonlinear_arith);
        }
    }
    lemma_sum_map(deltas, |ld: LineDelta| (line_in_view(ld.row), ld.sign as int), signed(h1), tl);
    assert(line_delta_seq_view(deltas) =~= deltas.map_values(|ld: LineDelta| (line_in_view(ld.row), ld.sign as int)));
    assert forall|r: LineIn| #[trigger] rl(r) == dd(line_in_view(r)) by {
        let l = line_in_view(r);
        if l.l_partkey == k && l.l_suppkey == s {
            assert((dates(db2, l.l_orderkey, y) - dates(db, l.l_orderkey, y)) * line_weight(l, f)
                == dates(db2, l.l_orderkey, y) * line_weight(l, f) - dates(db, l.l_orderkey, y) * line_weight(l, f)) by (nonlinear_arith);
        }
    }
    lemma_sum_map(rows, |r: LineIn| line_in_view(r), dd, rl);
    lemma_sum_ext_all(db.lineitem, h1, |row: LineitemRow| if row.l_partkey == k && row.l_suppkey == s { dates(db, row.l_orderkey, y) * line_weight(row, f) } else { 0int });
    lemma_sum_ext_all(db2.lineitem, h2, |row: LineitemRow| if row.l_partkey == k && row.l_suppkey == s { dates(db2, row.l_orderkey, y) * line_weight(row, f) } else { 0int });
}

}
