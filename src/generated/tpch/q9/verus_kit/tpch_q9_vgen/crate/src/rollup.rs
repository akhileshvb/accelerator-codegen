// The line-centric form of the nation totals of a pattern, and its frames under table changes.
use vstd::prelude::*;
use verus_common::sums::*;
use verus_common::sqlfns::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::meaning::*;
use crate::query_proofs::*;

verus! {

broadcast use vstd::seq_lib::group_seq_properties;

/// The pair factor of a line: field 0 counts the (part, supplier) rows of partsupp; field 1 is
/// their count times the line's revenue minus their cost sum times its quantity.
pub open spec fn pf(db: Db, l: LineitemRow, field: int) -> int {
    if field == 0 { pair_count(db, l.l_partkey, l.l_suppkey) }
    else { pair_count(db, l.l_partkey, l.l_suppkey) * line_weight(l, 1) - pair_cost(db, l.l_partkey, l.l_suppkey) * l.l_quantity }
}

/// Copies of a line in the join with the parts matching `p`, the supplier (in nation nk) and partsupp.
pub open spec fn kappa(db: Db, p: Seq<u8>, l: LineitemRow, nk: int, field: int) -> int {
    supplier_count(db, l.l_suppkey, nk) * green(filter_db(db, p), l.l_partkey) * pf(db, l, field)
}

/// The contribution of a line to (nation nk, year), times the copies of its order of that year.
pub open spec fn hline(db: Db, p: Seq<u8>, l: LineitemRow, nk: int, year: int, field: int) -> int {
    dates(db, l.l_orderkey, year) * kappa(db, p, l, nk, field)
}

/// nation_total, line by line.
pub open spec fn tl(db: Db, p: Seq<u8>, nk: int, year: int, field: int) -> int {
    sum(db.lineitem, |l: LineitemRow| hline(db, p, l, nk, year, field))
}

pub open spec fn line_term(db: Db, p: Seq<u8>, s: int, l: LineitemRow, year: int, field: int) -> int {
    if l.l_suppkey == s { dates(db, l.l_orderkey, year) * green(filter_db(db, p), l.l_partkey) * pf(db, l, field) } else { 0int }
}

#[verifier::spinoff_prover]
pub proof fn lemma_supplier_lines(db: Db, p: Seq<u8>, s: int, year: int, field: int)
    requires 0 <= field <= 1,
    ensures supplier_total(filter_db(db, p), s, year, field) == sum(db.lineitem, |l: LineitemRow| line_term(db, p, s, l, year, field)),
{
    let dbp = filter_db(db, p);
    lemma_supplier_join(db, p, s, year, field);
    let cross = |part: PartRow, l: LineitemRow| if like_match(part.p_name, p) { dates(db, l.l_orderkey, year) * line_join(db, part.p_partkey, s, l, field) } else { 0int };
    let outer = |part: PartRow| if like_match(part.p_name, p) { part_join(db, part.p_partkey, s, year, field) } else { 0int };
    let inner = |l: LineitemRow| line_term(db, p, s, l, year, field);
    assert forall|part: PartRow| #[trigger] outer(part) == sum(db.lineitem, |l: LineitemRow| cross(part, l)) by {
        if like_match(part.p_name, p) {
            lemma_line_join(db, part.p_partkey, s, year, field);
            lemma_sum_ext_all(db.lineitem, |row: LineitemRow| dates(db, row.l_orderkey, year) * line_join(db, part.p_partkey, s, row, field), |l: LineitemRow| cross(part, l));
        } else {
            lemma_sum_zero(db.lineitem, |l: LineitemRow| cross(part, l));
        }
    }
    assert forall|l: LineitemRow| #[trigger] inner(l) == sum(db.part, |part: PartRow| cross(part, l)) by {
        if l.l_suppkey == s {
            let amount = dates(db, l.l_orderkey, year) * pf(db, l, field);
            assert forall|part: PartRow| #[trigger] cross(part, l) == amount * matched_weight(p, l.l_partkey)(part) by {
                if like_match(part.p_name, p) && part.p_partkey == l.l_partkey {
                    assert(line_join(db, part.p_partkey, s, l, field) == pf(db, l, field));
                } else {
                    assert(amount * 0int == 0) by (nonlinear_arith);
                }
            }
            lemma_sum_scale(db.part, matched_weight(p, l.l_partkey), |part: PartRow| cross(part, l), amount);
            lemma_matched_green(db, p, l.l_partkey);
            let g = green(dbp, l.l_partkey);
            let d = dates(db, l.l_orderkey, year);
            let q = pf(db, l, field);
            assert(amount * g == d * g * q) by (nonlinear_arith) requires amount == d * q;
        } else {
            lemma_sum_zero(db.part, |part: PartRow| cross(part, l));
        }
    }
    lemma_sum_swap(db.part, db.lineitem, cross, outer, inner);
}

#[verifier::spinoff_prover]
pub proof fn lemma_tl_bridge(db: Db, p: Seq<u8>, nk: int, year: int, field: int)
    requires 0 <= field <= 1,
    ensures tl(db, p, nk, year, field) == nation_total(filter_db(db, p), nk, year, field),
{
    let dbp = filter_db(db, p);
    lemma_filter_same(db, p);
    let cross = |sup: SupplierRow, l: LineitemRow| if sup.s_nationkey == nk && l.l_suppkey == sup.s_suppkey {
        dates(db, l.l_orderkey, year) * green(dbp, l.l_partkey) * pf(db, l, field)
    } else { 0int };
    let outer = |sup: SupplierRow| if sup.s_nationkey == nk { supplier_total(dbp, sup.s_suppkey, year, field) } else { 0int };
    let inner = |l: LineitemRow| hline(db, p, l, nk, year, field);
    assert forall|sup: SupplierRow| #[trigger] outer(sup) == sum(db.lineitem, |l: LineitemRow| cross(sup, l)) by {
        if sup.s_nationkey == nk {
            lemma_supplier_lines(db, p, sup.s_suppkey, year, field);
            lemma_sum_ext_all(db.lineitem, |l: LineitemRow| line_term(db, p, sup.s_suppkey, l, year, field), |l: LineitemRow| cross(sup, l));
        } else {
            lemma_sum_zero(db.lineitem, |l: LineitemRow| cross(sup, l));
        }
    }
    assert forall|l: LineitemRow| #[trigger] inner(l) == sum(db.supplier, |sup: SupplierRow| cross(sup, l)) by {
        let amount = dates(db, l.l_orderkey, year) * green(dbp, l.l_partkey) * pf(db, l, field);
        let count = |sup: SupplierRow| if sup.s_suppkey == l.l_suppkey && sup.s_nationkey == nk { 1int } else { 0int };
        assert forall|sup: SupplierRow| #[trigger] cross(sup, l) == amount * count(sup) by {
            if !(sup.s_nationkey == nk && l.l_suppkey == sup.s_suppkey) { assert(amount * 0int == 0) by (nonlinear_arith); }
        }
        lemma_sum_scale(db.supplier, count, |sup: SupplierRow| cross(sup, l), amount);
        let d = dates(db, l.l_orderkey, year);
        let g = green(dbp, l.l_partkey);
        let q = pf(db, l, field);
        let c = supplier_count(db, l.l_suppkey, nk);
        assert(supplier_count(db, l.l_suppkey, nk) == sum(db.supplier, count));
        assert(amount * c == d * (c * g * q)) by (nonlinear_arith) requires amount == d * g * q;
    }
    lemma_sum_swap(db.supplier, db.lineitem, cross, outer, inner);
    assert(dbp.supplier == db.supplier);
    lemma_sum_ext_all(db.supplier, outer, |sup: SupplierRow| if sup.s_nationkey == nk { supplier_total(dbp, sup.s_suppkey, year, field) } else { 0int });
}

/// The change of a sum of per-line weights when lines change by `dl` and the weight changes from h1 to h2.
pub proof fn lemma_master<A>(before: Seq<A>, after: Seq<A>, dl: Seq<(A, int)>, h1: spec_fn(A) -> int, h2: spec_fn(A) -> int)
    requires bag_step(before, after, dl),
    ensures sum(after, h2) - sum(before, h1) == sum(after, |l: A| h2(l) - h1(l)) + sum(dl, |d: (A, int)| d.1 * h1(d.0)),
{
    assert(sum(after, h1) == sum(before, h1) + sum(dl, |d: (A, int)| d.1 * h1(d.0)));
    lemma_sum_sub(after, h2, h1, |l: A| h2(l) - h1(l));
}

}
