// Algebra between the generated spec (gen_spec.rs) and the kernel's aggregates.
//
//   numwait(db, p, g) = group_count(db, {p1: p}, g)
//                     = Σ_{s ∈ supplier, s_name = g} nm(s_nationkey) · W(s_suppkey)
//
// where nm counts nation rows with that key and name p, fm counts
// orders rows with that key and status 'F', and
//
//   W(sk) = Σ_{l ∈ lineitem, l_suppkey = sk, l qualifies} fm(l_orderkey)
//
// "l qualifies" is the l1 part of the WHERE clause (late, EXISTS, NOT EXISTS).
// Everything here is proof vocabulary (not trusted): each name is defined
// from, or proved equal to, the generated definitions.
use vstd::prelude::*;
use verus_common::bytes::*;
use verus_common::sums::*;
use verus_common::rel::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;

verus! {

/// l_receiptdate > l_commitdate
pub open spec fn late(l: LineitemRow) -> bool {
    l.l_receiptdate > l.l_commitdate
}

/// o_orderstatus = 'F' (the conjunct of where_clause), named for the proofs.
pub open spec fn is_f(o: OrdersRow) -> bool {
    o.o_orderstatus == seq![b'F']
}

/// COUNT(*) of group g for :nation = p.
pub open spec fn numwait(db: Db, p: Seq<u8>, g: Seq<u8>) -> int {
    group_count(db, params(p), g)
}

/// ORDER BY numwait DESC, s_name as a strict order on (s_name, numwait) rows.
pub open spec fn row_before(a: (Seq<u8>, int), b: (Seq<u8>, int)) -> bool {
    a.1 > b.1 || (a.1 == b.1 && bytes_lt(a.0, b.0))
}

/// The answer before LIMIT, in the strict form the kernel establishes:
/// strictly sorted (so one row per group), each row a group with its COUNT(*),
/// every group present.  Equivalent to gen_spec::full_result (lemmas below).
pub open spec fn strict_full(db: Db, p: Seq<u8>, out: Seq<(Seq<u8>, int)>) -> bool {
    &&& forall|i: int, j: int|
        0 <= i < j < out.len() ==> row_before(#[trigger] out[i], #[trigger] out[j])
    &&& forall|i: int|
        0 <= i < out.len() ==> (#[trigger] out[i]).1 == numwait(db, p, out[i].0)
            && out[i].1 > 0
    &&& forall|g: Seq<u8>|
        #[trigger] numwait(db, p, g) > 0 ==> exists|i: int|
            0 <= i < out.len() && (#[trigger] out[i]).0 == g
}

/// A (s_name, numwait) pair as an output row.
pub open spec fn out_row(r: (Seq<u8>, int)) -> OutRow {
    OutRow { s_name: r.0, numwait: r.1 }
}

pub open spec fn out_rows(rs: Seq<(Seq<u8>, int)>) -> Seq<OutRow> {
    rs.map_values(|r: (Seq<u8>, int)| out_row(r))
}

pub proof fn lemma_strict_full_is_full(db: Db, p: Seq<u8>, out: Seq<(Seq<u8>, int)>)
    requires
        strict_full(db, p, out),
    ensures
        full_result(db, params(p), out_rows(out)),
{
    let rs = out_rows(out);
    assert forall|i: int, j: int| 0 <= i < j < rs.len() implies
        !same_group(#[trigger] rs[i], #[trigger] rs[j]) && !row_lt(rs[j], rs[i]) by {
        assert(row_before(out[i], out[j]));
        lemma_bytes_lt_irrefl(out[i].0);
        lemma_bytes_lt_asym(out[i].0, out[j].0);
    }
    assert forall|g: Seq<u8>| #[trigger] group_count(db, params(p), g) > 0 implies
        exists|i: int| 0 <= i < rs.len() && (#[trigger] rs[i]).s_name == g by {
        assert(numwait(db, p, g) > 0);
        let i = choose|i: int| 0 <= i < out.len() && (#[trigger] out[i]).0 == g;
        assert(rs[i].s_name == g);
    }
}

/// Number of orders rows with o_orderkey = ok and o_orderstatus = 'F'.
pub open spec fn fm(db: Db, ok: int) -> int {
    sum(db.orders, |o: OrdersRow| ind(o.o_orderkey == ok && is_f(o)))
}

/// Number of nation rows with n_nationkey = nk and n_name = p.
pub open spec fn nm(db: Db, nk: int, p: Seq<u8>) -> int {
    sum(db.nation, |n: NationRow| ind(n.n_nationkey == nk && n.n_name == p))
}

/// The conditions of the WHERE clause that mention only l1 (and lineitem).
pub open spec fn qual(db: Db, l: LineitemRow) -> bool {
    late(l) && exists_1(db, l) && !exists_2(db, l)
}

// A single lineitem's share of a supplier's waiting-order total: the number of matching 'F' orders if it is that supplier's qualifying late line, else 0.
pub open spec fn w_term(db: Db, sk: int, l: LineitemRow) -> int {
    if l.l_suppkey == sk && qual(db, l) {
        fm(db, l.l_orderkey)
    } else {
        0
    }
}

/// Per-supplier-key count: qualifying l1 rows of supplier sk, each weighted
/// by how many 'F' orders rows it joins.
pub open spec fn w(db: Db, sk: int) -> int {
    sum(db.lineitem, |l: LineitemRow| w_term(db, sk, l))
}

// One supplier's share of the answer for a given nation-named group: (matching nations) x (its waiting-order total), or 0 if its name differs.
// Summing these over all suppliers gives numwait; the executor computes the same per-supplier product.
pub open spec fn sup_term(db: Db, p: Seq<u8>, g: Seq<u8>, s: SupplierRow) -> int {
    if s.s_name == g {
        nm(db, s.s_nationkey, p) * w(db, s.s_suppkey)
    } else {
        0
    }
}

// The count of group g, one FROM table at a time.
// Innermost layer (over nations): reduces to a 0/1 match on the supplier's nation and name.
pub open spec fn nat_level(db: Db, p: Seq<u8>, g: Seq<u8>, supplier: SupplierRow, l1: LineitemRow, orders: OrdersRow) -> int {
    sum(db.nation, |nation: NationRow| if where_clause(db, params(p), supplier, l1, orders, nation) && supplier.s_name == g { 1int } else { 0 })
}

// Next layer out (over orders): pairs the lineitem with each order row and folds in the nation layer.
pub open spec fn ord_level(db: Db, p: Seq<u8>, g: Seq<u8>, s: SupplierRow, l1: LineitemRow) -> int {
    sum(db.orders, |o: OrdersRow| nat_level(db, p, g, s, l1, o))
}

// Third layer (over lineitems for one supplier): the lineitem-level part of the join, before the supplier layer.
pub open spec fn li_level(db: Db, p: Seq<u8>, g: Seq<u8>, s: SupplierRow) -> int {
    sum(db.lineitem, |l1: LineitemRow| ord_level(db, p, g, s, l1))
}

// Sum over nation rows collapses to the count of nations with the supplier's key and the target name, provided the lineitem conditions hold (else 0).
pub proof fn lemma_nat_level(db: Db, p: Seq<u8>, g: Seq<u8>, s: SupplierRow, l1: LineitemRow, o: OrdersRow)
    ensures
        nat_level(db, p, g, s, l1, o) == if s.s_suppkey == l1.l_suppkey && o.o_orderkey == l1.l_orderkey
            && is_f(o) && qual(db, l1) && s.s_name == g {
            nm(db, s.s_nationkey, p)
        } else {
            0
        },
{
    let fl = |nation: NationRow| if where_clause(db, params(p), s, l1, o, nation) && s.s_name == g { 1int } else { 0 };
    if s.s_suppkey == l1.l_suppkey && o.o_orderkey == l1.l_orderkey && is_f(o) && qual(db, l1)
        && s.s_name == g {
        lemma_sum_ext_all(db.nation, fl, |n: NationRow| ind(n.n_nationkey == s.s_nationkey && n.n_name == p));
    } else {
        lemma_sum_ext_all(db.nation, fl, |n: NationRow| 0int);
        lemma_sum_zero(db.nation, |n: NationRow| 0int);
    }
}

// Sum over orders collapses to (nation matches) x (number of 'F' rows for the lineitem's order) when the lineitem qualifies for this supplier, else 0.
pub proof fn lemma_ord_level(db: Db, p: Seq<u8>, g: Seq<u8>, s: SupplierRow, l1: LineitemRow)
    ensures
        ord_level(db, p, g, s, l1) == if s.s_suppkey == l1.l_suppkey && qual(db, l1) && s.s_name == g {
            nm(db, s.s_nationkey, p) * fm(db, l1.l_orderkey)
        } else {
            0
        },
{
    let fo = |o: OrdersRow| nat_level(db, p, g, s, l1, o);
    if s.s_suppkey == l1.l_suppkey && qual(db, l1) && s.s_name == g {
        let c = nm(db, s.s_nationkey, p);
        let ind_o = |o: OrdersRow| ind(o.o_orderkey == l1.l_orderkey && is_f(o));
        assert forall|o: OrdersRow| #[trigger] fo(o) == c * ind_o(o) by {
            lemma_nat_level(db, p, g, s, l1, o);
        }
        lemma_sum_scale(db.orders, ind_o, fo, c);
    } else {
        assert forall|o: OrdersRow| #[trigger] fo(o) == 0int by {
            lemma_nat_level(db, p, g, s, l1, o);
        }
        lemma_sum_ext_all(db.orders, fo, |o: OrdersRow| 0int);
        lemma_sum_zero(db.orders, |o: OrdersRow| 0int);
    }
}

// Sum over lineitems collapses to the supplier's whole contribution: nation matches times the supplier's waiting-order total.
pub proof fn lemma_li_level(db: Db, p: Seq<u8>, g: Seq<u8>, s: SupplierRow)
    ensures
        li_level(db, p, g, s) == sup_term(db, p, g, s),
{
    let fl = |l1: LineitemRow| ord_level(db, p, g, s, l1);
    if s.s_name == g {
        let c = nm(db, s.s_nationkey, p);
        let wt = |l: LineitemRow| w_term(db, s.s_suppkey, l);
        assert forall|l1: LineitemRow| #[trigger] fl(l1) == c * wt(l1) by {
            lemma_ord_level(db, p, g, s, l1);
        }
        lemma_sum_scale(db.lineitem, wt, fl, c);
    } else {
        assert forall|l1: LineitemRow| #[trigger] fl(l1) == 0int by {
            lemma_ord_level(db, p, g, s, l1);
        }
        lemma_sum_ext_all(db.lineitem, fl, |l: LineitemRow| 0int);
        lemma_sum_zero(db.lineitem, |l: LineitemRow| 0int);
    }
}

/// Σ over the generated tuples of [supplier name = g]
///   = Σ_s [s_name = g] · nm(s_nationkey, p) · W(s_suppkey).
proof fn lemma_tuples_by_supplier(db: Db, p: Seq<u8>, g: Seq<u8>, ff: spec_fn(Tuple) -> int)
    requires
        forall|t: Tuple| #[trigger] ff(t) == if t.supplier.s_name == g { 1int } else { 0 },
    ensures
        sum(tuples(db, params(p)), ff) == sum(db.supplier, |s: SupplierRow| sup_term(db, p, g, s)),
{
    // tuples is flat over supplier, then lineitem, orders, nation (gen_spec::tuples_1..4)
    let g1 = |supplier: SupplierRow| tuples_1(db, params(p), supplier);
    lemma_sum_flat_map(db.supplier, g1, ff);
    assert forall|s: SupplierRow| #[trigger] sum(g1(s), ff) == sup_term(db, p, g, s) by {
        let g2 = |l1: LineitemRow| tuples_2(db, params(p), s, l1);
        lemma_sum_flat_map(db.lineitem, g2, ff);
        assert forall|l1: LineitemRow| #[trigger] sum(g2(l1), ff) == ord_level(db, p, g, s, l1) by {
            let g3 = |orders: OrdersRow| tuples_3(db, params(p), s, l1, orders);
            lemma_sum_flat_map(db.orders, g3, ff);
            assert forall|o: OrdersRow| #[trigger] sum(g3(o), ff) == nat_level(db, p, g, s, l1, o) by {
                let g4 = |nation: NationRow| tuples_4(db, params(p), s, l1, o, nation);
                lemma_sum_flat_map(db.nation, g4, ff);
                let fl = |nation: NationRow| if where_clause(db, params(p), s, l1, o, nation) && s.s_name == g { 1int } else { 0 };
                assert forall|n: NationRow| #[trigger] sum(g4(n), ff) == fl(n) by {
                    lemma_sum_opt(where_clause(db, params(p), s, l1, o, n), Tuple { supplier: s, l1, orders: o, nation: n }, ff);
                }
                lemma_sum_ext_all(db.nation, |a: NationRow| sum(g4(a), ff), fl);
            }
            lemma_sum_ext_all(db.orders, |a: OrdersRow| sum(g3(a), ff), |o: OrdersRow| nat_level(db, p, g, s, l1, o));
        }
        lemma_sum_ext_all(db.lineitem, |a: LineitemRow| sum(g2(a), ff), |l1: LineitemRow| ord_level(db, p, g, s, l1));
        lemma_li_level(db, p, g, s);
    }
    lemma_sum_ext_all(db.supplier, |a: SupplierRow| sum(g1(a), ff), |s: SupplierRow| sup_term(db, p, g, s));
}

/// numwait = Σ_s [s_name = g] · nm(s_nationkey, p) · W(s_suppkey).
pub proof fn lemma_numwait_by_supplier(db: Db, p: Seq<u8>, g: Seq<u8>)
    ensures
        numwait(db, p, g) == sum(db.supplier, |s: SupplierRow| sup_term(db, p, g, s)),
{
    lemma_tuples_by_supplier(db, p, g, |t: Tuple| if t.supplier.s_name == g { 1int } else { 0 });
}

// The number of 'F' order rows for a key is a count, so it is never negative; needed for bounds and no-overflow arguments.
pub proof fn lemma_fm_nonneg(db: Db, ok: int)
    ensures
        fm(db, ok) >= 0,
{
    lemma_sum_nonneg(db.orders, |o: OrdersRow| ind(o.o_orderkey == ok && is_f(o)));
}

// The number of nation rows with a given key and name is a count, so never negative.
pub proof fn lemma_nm_nonneg(db: Db, nk: int, p: Seq<u8>)
    ensures
        nm(db, nk, p) >= 0,
{
    lemma_sum_nonneg(db.nation, |n: NationRow| ind(n.n_nationkey == nk && n.n_name == p));
}

// A supplier's waiting-order total adds up nonnegative terms, so it is never negative; this bounds the final numwait counts.
pub proof fn lemma_w_nonneg(db: Db, sk: int)
    ensures
        w(db, sk) >= 0,
{
    assert forall|i: int| 0 <= i < db.lineitem.len() implies w_term(
        db,
        sk,
        #[trigger] db.lineitem[i],
    ) >= 0 by {
        lemma_fm_nonneg(db, db.lineitem[i].l_orderkey);
    }
    lemma_sum_nonneg(db.lineitem, |l: LineitemRow| w_term(db, sk, l));
}

} // verus!
