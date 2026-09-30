// Algebra between q_ref and the kernel's aggregates.
//
//   numwait(db, p, g) = Σ_{s ∈ supplier, s_name = g} nm(s_nationkey) · W(s_suppkey)
//
// where nm counts nation rows with that key and name p, fm counts
// orders rows with that key and status 'F', and
//
//   W(sk) = Σ_{l ∈ lineitem, l_suppkey = sk, l qualifies} fm(l_orderkey)
//
// "l qualifies" is the l1 part of the WHERE clause (late, EXISTS, NOT EXISTS).
use vstd::prelude::*;
use verus_common::sums::*;
use crate::spec::*;

verus! {

/// Number of orders rows with o_orderkey = ok and o_orderstatus = 'F'.
pub open spec fn fm(db: Db, ok: int) -> int {
    sum(db.orders, |o: Order| ind(o.orderkey == ok && o.status_is_f))
}

/// Number of nation rows with n_nationkey = nk and n_name = p.
pub open spec fn nm(db: Db, nk: int, p: Seq<u8>) -> int {
    sum(db.nation, |n: Nation| ind(n.nationkey == nk && n.name == p))
}

/// The conditions of the WHERE clause that mention only l1 (and lineitem).
pub open spec fn qual(db: Db, l: Lineitem) -> bool {
    late(l) && exists_l2(db, l) && !exists_l3(db, l)
}

// A single lineitem's share of a supplier's waiting-order total: the number of matching 'F' orders if it is that supplier's qualifying late line, else 0.
pub open spec fn w_term(db: Db, sk: int, l: Lineitem) -> int {
    if l.suppkey == sk && qual(db, l) {
        fm(db, l.orderkey)
    } else {
        0
    }
}

/// Per-supplier-key count: qualifying l1 rows of supplier sk, each weighted
/// by how many 'F' orders rows it joins.
pub open spec fn w(db: Db, sk: int) -> int {
    sum(db.lineitem, |l: Lineitem| w_term(db, sk, l))
}

// One supplier's share of the answer for a given nation-named group: (matching nations) x (its waiting-order total), or 0 if its name differs.
// Summing these over all suppliers gives numwait; the executor computes the same per-supplier product.
pub open spec fn sup_term(db: Db, p: Seq<u8>, g: Seq<u8>, s: Supplier) -> int {
    if s.name == g {
        nm(db, s.nationkey, p) * w(db, s.suppkey)
    } else {
        0
    }
}

// Named levels of the numwait nest.
// Innermost layer of the query's nested sum (over nations): reduces to a 0/1 match on the supplier's nation and name.
pub open spec fn nat_level(db: Db, p: Seq<u8>, g: Seq<u8>, s: Supplier, l1: Lineitem, o: Order) -> int {
    sum(db.nation, |n: Nation| ind(where_clause(db, p, s, l1, o, n) && s.name == g))
}

// Next layer out (over orders): pairs the lineitem with each order row and folds in the nation layer.
pub open spec fn ord_level(db: Db, p: Seq<u8>, g: Seq<u8>, s: Supplier, l1: Lineitem) -> int {
    sum(db.orders, |o: Order| nat_level(db, p, g, s, l1, o))
}

// Third layer (over lineitems for one supplier): the lineitem-level part of the join, before the supplier layer.
pub open spec fn li_level(db: Db, p: Seq<u8>, g: Seq<u8>, s: Supplier) -> int {
    sum(db.lineitem, |l1: Lineitem| ord_level(db, p, g, s, l1))
}

// Sum over nation rows collapses to the count of nations with the supplier's key and the target name, provided the lineitem conditions hold (else 0).
pub proof fn lemma_nat_level(db: Db, p: Seq<u8>, g: Seq<u8>, s: Supplier, l1: Lineitem, o: Order)
    ensures
        nat_level(db, p, g, s, l1, o) == if s.suppkey == l1.suppkey && o.orderkey == l1.orderkey
            && o.status_is_f && qual(db, l1) && s.name == g {
            nm(db, s.nationkey, p)
        } else {
            0
        },
{
    let f = |n: Nation| ind(where_clause(db, p, s, l1, o, n) && s.name == g);
    if s.suppkey == l1.suppkey && o.orderkey == l1.orderkey && o.status_is_f && qual(db, l1)
        && s.name == g {
        lemma_sum_ext_all(
            db.nation,
            f,
            |n: Nation| ind(n.nationkey == s.nationkey && n.name == p),
        );
    } else {
        lemma_sum_ext_all(db.nation, f, |n: Nation| 0int);
        lemma_sum_zero(db.nation, |n: Nation| 0int);
    }
}

// Sum over orders collapses to (nation matches) x (number of 'F' rows for the lineitem's order) when the lineitem qualifies for this supplier, else 0.
pub proof fn lemma_ord_level(db: Db, p: Seq<u8>, g: Seq<u8>, s: Supplier, l1: Lineitem)
    ensures
        ord_level(db, p, g, s, l1) == if s.suppkey == l1.suppkey && qual(db, l1) && s.name == g {
            nm(db, s.nationkey, p) * fm(db, l1.orderkey)
        } else {
            0
        },
{
    let f = |o: Order| nat_level(db, p, g, s, l1, o);
    assert forall|o: Order| #[trigger] f(o) == nat_level(db, p, g, s, l1, o) by {}
    if s.suppkey == l1.suppkey && qual(db, l1) && s.name == g {
        let c = nm(db, s.nationkey, p);
        let ind_o = |o: Order| ind(o.orderkey == l1.orderkey && o.status_is_f);
        assert forall|o: Order| #[trigger] f(o) == c * ind_o(o) by {
            lemma_nat_level(db, p, g, s, l1, o);
        }
        lemma_sum_scale(db.orders, ind_o, f, c);
    } else {
        assert forall|o: Order| #[trigger] f(o) == 0int by {
            lemma_nat_level(db, p, g, s, l1, o);
        }
        lemma_sum_ext_all(db.orders, f, |o: Order| 0int);
        lemma_sum_zero(db.orders, |o: Order| 0int);
    }
}

// Sum over lineitems collapses to the supplier's whole contribution: nation matches times the supplier's waiting-order total.
pub proof fn lemma_li_level(db: Db, p: Seq<u8>, g: Seq<u8>, s: Supplier)
    ensures
        li_level(db, p, g, s) == sup_term(db, p, g, s),
{
    let f = |l1: Lineitem| ord_level(db, p, g, s, l1);
    if s.name == g {
        let c = nm(db, s.nationkey, p);
        let wt = |l: Lineitem| w_term(db, s.suppkey, l);
        assert forall|l1: Lineitem| #[trigger] f(l1) == c * wt(l1) by {
            lemma_ord_level(db, p, g, s, l1);
        }
        lemma_sum_scale(db.lineitem, wt, f, c);
    } else {
        assert forall|l1: Lineitem| #[trigger] f(l1) == 0int by {
            lemma_ord_level(db, p, g, s, l1);
        }
        lemma_sum_ext_all(db.lineitem, f, |l: Lineitem| 0int);
        lemma_sum_zero(db.lineitem, |l: Lineitem| 0int);
    }
}

/// numwait = Σ_s [s_name = g] · nm(s_nationkey, p) · W(s_suppkey).
pub proof fn lemma_numwait_by_supplier(db: Db, p: Seq<u8>, g: Seq<u8>)
    ensures
        numwait(db, p, g) == sum(db.supplier, |s: Supplier| sup_term(db, p, g, s)),
{
    // Unfold the literal nest of numwait into the named levels.
    let lam_s = |s: Supplier|
        sum(
            db.lineitem,
            |l1: Lineitem|
                sum(
                    db.orders,
                    |o: Order|
                        sum(
                            db.nation,
                            |n: Nation| ind(where_clause(db, p, s, l1, o, n) && s.name == g),
                        ),
                ),
        );
    assert forall|s: Supplier| #[trigger] lam_s(s) == sup_term(db, p, g, s) by {
        let lam_l = |l1: Lineitem|
            sum(
                db.orders,
                |o: Order|
                    sum(
                        db.nation,
                        |n: Nation| ind(where_clause(db, p, s, l1, o, n) && s.name == g),
                    ),
            );
        assert forall|l1: Lineitem| #[trigger] lam_l(l1) == ord_level(db, p, g, s, l1) by {
            let lam_o = |o: Order|
                sum(db.nation, |n: Nation| ind(where_clause(db, p, s, l1, o, n) && s.name == g));
            lemma_sum_ext_all(db.orders, lam_o, |o: Order| nat_level(db, p, g, s, l1, o));
        }
        lemma_sum_ext_all(db.lineitem, lam_l, |l1: Lineitem| ord_level(db, p, g, s, l1));
        lemma_li_level(db, p, g, s);
    }
    lemma_sum_ext_all(db.supplier, lam_s, |s: Supplier| sup_term(db, p, g, s));
}

// The number of 'F' order rows for a key is a count, so it is never negative; needed for bounds and no-overflow arguments.
pub proof fn lemma_fm_nonneg(db: Db, ok: int)
    ensures
        fm(db, ok) >= 0,
{
    lemma_sum_nonneg(db.orders, |o: Order| ind(o.orderkey == ok && o.status_is_f));
}

// The number of nation rows with a given key and name is a count, so never negative.
pub proof fn lemma_nm_nonneg(db: Db, nk: int, p: Seq<u8>)
    ensures
        nm(db, nk, p) >= 0,
{
    lemma_sum_nonneg(db.nation, |n: Nation| ind(n.nationkey == nk && n.name == p));
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
        lemma_fm_nonneg(db, db.lineitem[i].orderkey);
    }
    lemma_sum_nonneg(db.lineitem, |l: Lineitem| w_term(db, sk, l));
}

} // verus!
