// Per-order facts.  A lineitem's qualification (late, EXISTS l2, NOT EXISTS
// l3) depends only on the lineitems of its own order, so W decomposes into
// per-order contributions, and a batch of changes confined to a set of
// orderkeys changes W only through those orders.
use vstd::prelude::*;
use verus_common::sums::*;
use crate::spec::*;
use crate::bridge::*;

verus! {

/// Lineitem rows of order ok and supplier sk.
pub open spec fn cnt(db: Db, ok: int, sk: int) -> int {
    sum(db.lineitem, |l: Lineitem| ind(l.orderkey == ok && l.suppkey == sk))
}

/// Late lineitem rows of order ok and supplier sk.
pub open spec fn lcnt(db: Db, ok: int, sk: int) -> int {
    sum(db.lineitem, |l: Lineitem| ind(l.orderkey == ok && l.suppkey == sk && late(l)))
}

/// Lineitem rows of order ok with a supplier other than sk.
pub open spec fn other_cnt(db: Db, ok: int, sk: int) -> int {
    sum(db.lineitem, |l: Lineitem| ind(l.orderkey == ok && l.suppkey != sk))
}

/// Late lineitem rows of order ok with a supplier other than sk.
pub open spec fn other_late_cnt(db: Db, ok: int, sk: int) -> int {
    sum(db.lineitem, |l: Lineitem| ind(l.orderkey == ok && l.suppkey != sk && late(l)))
}

/// l1 with (ok, sk) passes EXISTS / NOT EXISTS.
pub open spec fn qk(db: Db, ok: int, sk: int) -> bool {
    other_cnt(db, ok, sk) > 0 && other_late_cnt(db, ok, sk) == 0
}

/// Per-order contribution to W(sk), before the 'F' multiplicity.
pub open spec fn contrib(db: Db, ok: int, sk: int) -> int {
    if qk(db, ok, sk) {
        lcnt(db, ok, sk)
    } else {
        0
    }
}

pub open spec fn per_order(db: Db, ok: int, sk: int) -> int {
    fm(db, ok) * contrib(db, ok, sk)
}

pub proof fn lemma_ind_sum_nonneg(s: Seq<Lineitem>, p: spec_fn(Lineitem) -> bool)
    ensures
        sum(s, |l: Lineitem| ind(p(l))) >= 0,
{
    lemma_sum_nonneg(s, |l: Lineitem| ind(p(l)));
}

pub proof fn lemma_cnt_nonneg(db: Db, ok: int, sk: int)
    ensures
        cnt(db, ok, sk) >= 0,
        lcnt(db, ok, sk) >= 0,
        lcnt(db, ok, sk) <= cnt(db, ok, sk),
        other_cnt(db, ok, sk) >= 0,
        other_late_cnt(db, ok, sk) >= 0,
{
    lemma_sum_nonneg(db.lineitem, |l: Lineitem| ind(l.orderkey == ok && l.suppkey == sk));
    lemma_sum_nonneg(
        db.lineitem,
        |l: Lineitem| ind(l.orderkey == ok && l.suppkey == sk && late(l)),
    );
    lemma_sum_le(
        db.lineitem,
        |l: Lineitem| ind(l.orderkey == ok && l.suppkey == sk && late(l)),
        |l: Lineitem| ind(l.orderkey == ok && l.suppkey == sk),
    );
    lemma_sum_nonneg(db.lineitem, |l: Lineitem| ind(l.orderkey == ok && l.suppkey != sk));
    lemma_sum_nonneg(
        db.lineitem,
        |l: Lineitem| ind(l.orderkey == ok && l.suppkey != sk && late(l)),
    );
}

/// An indicator count is positive iff a witness row exists.
pub proof fn lemma_ind_count_pos(s: Seq<Lineitem>, p: spec_fn(Lineitem) -> bool)
    ensures
        sum(s, |l: Lineitem| ind(p(l))) > 0 <==> exists|j: int| 0 <= j < s.len() && p(
            #[trigger] s[j],
        ),
{
    let f = |l: Lineitem| ind(p(l));
    lemma_sum_nonneg(s, f);
    if exists|j: int| 0 <= j < s.len() && p(#[trigger] s[j]) {
        let j = choose|j: int| 0 <= j < s.len() && p(#[trigger] s[j]);
        lemma_sum_pos(s, f, j);
    }
    if sum(s, f) > 0 {
        let j = lemma_sum_nonzero_witness(s, f);
        assert(p(s[j]));
    }
}

/// EXISTS l2 as a count.
pub proof fn lemma_exists_l2_count(db: Db, l: Lineitem)
    ensures
        exists_l2(db, l) == (other_cnt(db, l.orderkey, l.suppkey) > 0),
{
    let ok = l.orderkey;
    let sk = l.suppkey;
    let p = |x: Lineitem| x.orderkey == ok && x.suppkey != sk;
    lemma_ind_count_pos(db.lineitem, p);
    lemma_sum_ext_all(
        db.lineitem,
        |x: Lineitem| ind(p(x)),
        |x: Lineitem| ind(x.orderkey == ok && x.suppkey != sk),
    );
    if exists_l2(db, l) {
        let j = choose|j: int|
            0 <= j < db.lineitem.len() && (#[trigger] db.lineitem[j]).orderkey == l.orderkey
                && db.lineitem[j].suppkey != l.suppkey;
        assert(p(db.lineitem[j]));
    }
    if other_cnt(db, ok, sk) > 0 {
        let j = choose|j: int| 0 <= j < db.lineitem.len() && p(#[trigger] db.lineitem[j]);
        assert(db.lineitem[j].orderkey == l.orderkey && db.lineitem[j].suppkey != l.suppkey);
    }
}

/// EXISTS l3 as a count.
pub proof fn lemma_exists_l3_count(db: Db, l: Lineitem)
    ensures
        exists_l3(db, l) == (other_late_cnt(db, l.orderkey, l.suppkey) > 0),
{
    let ok = l.orderkey;
    let sk = l.suppkey;
    let p = |x: Lineitem| x.orderkey == ok && x.suppkey != sk && late(x);
    lemma_ind_count_pos(db.lineitem, p);
    lemma_sum_ext_all(
        db.lineitem,
        |x: Lineitem| ind(p(x)),
        |x: Lineitem| ind(x.orderkey == ok && x.suppkey != sk && late(x)),
    );
    if exists_l3(db, l) {
        let j = choose|j: int|
            0 <= j < db.lineitem.len() && (#[trigger] db.lineitem[j]).orderkey == l.orderkey
                && db.lineitem[j].suppkey != l.suppkey && late(db.lineitem[j]);
        assert(p(db.lineitem[j]));
    }
    if other_late_cnt(db, ok, sk) > 0 {
        let j = choose|j: int| 0 <= j < db.lineitem.len() && p(#[trigger] db.lineitem[j]);
        assert(db.lineitem[j].orderkey == l.orderkey && db.lineitem[j].suppkey != l.suppkey);
    }
}

/// EXISTS / NOT EXISTS of the query as per-(ok, sk) counts.
pub proof fn lemma_qual_key(db: Db, l: Lineitem)
    ensures
        qual(db, l) == (late(l) && qk(db, l.orderkey, l.suppkey)),
{
    lemma_exists_l2_count(db, l);
    lemma_exists_l3_count(db, l);
    lemma_cnt_nonneg(db, l.orderkey, l.suppkey);
}

/// The lineitems of one order contribute fm(ok) · contrib(ok, sk) to W(sk).
pub proof fn lemma_per_order(db: Db, ok: int, sk: int)
    ensures
        sum(db.lineitem, |l: Lineitem| if l.orderkey == ok { w_term(db, sk, l) } else { 0 })
            == per_order(db, ok, sk),
{
    let f = |l: Lineitem| if l.orderkey == ok { w_term(db, sk, l) } else { 0 };
    let c = fm(db, ok);
    if qk(db, ok, sk) {
        let g = |l: Lineitem| ind(l.orderkey == ok && l.suppkey == sk && late(l));
        assert forall|l: Lineitem| #[trigger] f(l) == c * g(l) by {
            lemma_qual_key(db, l);
        }
        lemma_sum_scale(db.lineitem, g, f, c);
    } else {
        assert forall|l: Lineitem| #[trigger] f(l) == 0int by {
            lemma_qual_key(db, l);
        }
        lemma_sum_ext_all(db.lineitem, f, |l: Lineitem| 0int);
        lemma_sum_zero(db.lineitem, |l: Lineitem| 0int);
        assert(c * 0 == 0) by (nonlinear_arith);
    }
}

/// W as a sum over a distinct list of orderkeys covering every 'F' order.
pub proof fn lemma_w_by_orders(db: Db, sk: int, ks: Seq<int>)
    requires
        distinct(ks),
        forall|ok: int| #[trigger] fm(db, ok) > 0 ==> ks.contains(ok),
    ensures
        w(db, sk) == sum(ks, |ok: int| per_order(db, ok, sk)),
{
    let f = |l: Lineitem| w_term(db, sk, l);
    let key = |l: Lineitem| l.orderkey;
    let per = |k: int| sum(db.lineitem, |x: Lineitem| if key(x) == k { f(x) } else { 0 });
    assert forall|i: int| 0 <= i < db.lineitem.len() && f(db.lineitem[i]) != 0 implies ks.contains(
        key(#[trigger] db.lineitem[i]),
    ) by {
        lemma_fm_nonneg(db, db.lineitem[i].orderkey);
    }
    assert forall|k: int| #[trigger] per(k) == sum(
        db.lineitem,
        |x: Lineitem| if key(x) == k { f(x) } else { 0 },
    ) by {}
    lemma_sum_group(db.lineitem, ks, key, f, per);
    assert forall|k: int| #[trigger] per(k) == per_order(db, k, sk) by {
        lemma_sum_ext_all(
            db.lineitem,
            |x: Lineitem| if key(x) == k { f(x) } else { 0 },
            |l: Lineitem| if l.orderkey == k { w_term(db, sk, l) } else { 0 },
        );
        lemma_per_order(db, k, sk);
    }
    lemma_sum_ext_all(ks, per, |ok: int| per_order(db, ok, sk));
}

// ── Changes confined to a set of orderkeys ─────────────────────────────────
/// The capture contract for one transaction, orders / lineitem part: the new
/// tables are the old ones plus the signed row deltas (bags).
pub open spec fn db_step(
    db: Db,
    db2: Db,
    od: Seq<(Order, int)>,
    ld: Seq<(Lineitem, int)>,
) -> bool {
    &&& bag_step(db.orders, db2.orders, od)
    &&& bag_step(db.lineitem, db2.lineitem, ld)
}

/// The capture contract, supplier / nation part.
pub open spec fn dims_step(
    db: Db,
    db2: Db,
    sd: Seq<(Supplier, int)>,
    nd: Seq<(Nation, int)>,
) -> bool {
    &&& bag_step(db.supplier, db2.supplier, sd)
    &&& bag_step(db.nation, db2.nation, nd)
}

/// Every lineitem delta, and every 'F' orders delta, has its orderkey in `a`.
/// (A delta of a non-'F' orders row cannot change any 'F' count.)
pub open spec fn deltas_within(a: Seq<int>, od: Seq<(Order, int)>, ld: Seq<(Lineitem, int)>) -> bool {
    &&& forall|i: int| 0 <= i < od.len() && (#[trigger] od[i]).0.status_is_f ==> a.contains(od[i].0.orderkey)
    &&& forall|i: int| 0 <= i < ld.len() ==> a.contains((#[trigger] ld[i]).0.orderkey)
}

/// Σ over deltas of sign·f(row) is 0 when f vanishes on every delta row.
pub proof fn lemma_delta_zero<A>(ds: Seq<(A, int)>, f: spec_fn(A) -> int)
    requires
        forall|i: int| 0 <= i < ds.len() ==> f((#[trigger] ds[i]).0) == 0,
    ensures
        sum(ds, |d: (A, int)| d.1 * f(d.0)) == 0,
{
    let g = |d: (A, int)| d.1 * f(d.0);
    assert forall|i: int| 0 <= i < ds.len() implies g(#[trigger] ds[i]) == 0 by {
        assert(f(ds[i].0) == 0);
        assert(ds[i].1 * 0 == 0) by (nonlinear_arith);
    }
    lemma_sum_zero(ds, g);
}

/// Any lineitem count restricted to one orderkey outside `a` is unchanged.
pub proof fn lemma_li_count_unchanged(
    db: Db,
    db2: Db,
    od: Seq<(Order, int)>,
    ld: Seq<(Lineitem, int)>,
    a: Seq<int>,
    ok: int,
    p: spec_fn(Lineitem) -> bool,
)
    requires
        db_step(db, db2, od, ld),
        deltas_within(a, od, ld),
        !a.contains(ok),
    ensures
        sum(db2.lineitem, |l: Lineitem| ind(l.orderkey == ok && p(l))) == sum(
            db.lineitem,
            |l: Lineitem| ind(l.orderkey == ok && p(l)),
        ),
{
    let f = |l: Lineitem| ind(l.orderkey == ok && p(l));
    assert(sum(db2.lineitem, f) == sum(db.lineitem, f) + sum(ld, |d: (Lineitem, int)| d.1 * f(d.0)));
    assert forall|i: int| 0 <= i < ld.len() implies f((#[trigger] ld[i]).0) == 0 by {
        assert(a.contains(ld[i].0.orderkey));
    }
    lemma_delta_zero(ld, f);
}

pub proof fn lemma_fm_unchanged(
    db: Db,
    db2: Db,
    od: Seq<(Order, int)>,
    ld: Seq<(Lineitem, int)>,
    a: Seq<int>,
    ok: int,
)
    requires
        db_step(db, db2, od, ld),
        deltas_within(a, od, ld),
        !a.contains(ok),
    ensures
        fm(db2, ok) == fm(db, ok),
{
    let f = |o: Order| ind(o.orderkey == ok && o.status_is_f);
    assert(sum(db2.orders, f) == sum(db.orders, f) + sum(od, |d: (Order, int)| d.1 * f(d.0)));
    assert forall|i: int| 0 <= i < od.len() implies f((#[trigger] od[i]).0) == 0 by {
        if od[i].0.status_is_f {
            assert(a.contains(od[i].0.orderkey));
        }
    }
    lemma_delta_zero(od, f);
}

/// w_term of a lineitem whose order is outside `a` is unchanged.
pub proof fn lemma_w_term_unchanged(
    db: Db,
    db2: Db,
    od: Seq<(Order, int)>,
    ld: Seq<(Lineitem, int)>,
    a: Seq<int>,
    sk: int,
    l: Lineitem,
)
    requires
        db_step(db, db2, od, ld),
        deltas_within(a, od, ld),
        !a.contains(l.orderkey),
    ensures
        w_term(db2, sk, l) == w_term(db, sk, l),
{
    let ok = l.orderkey;
    let s = l.suppkey;
    lemma_fm_unchanged(db, db2, od, ld, a, ok);
    lemma_li_count_unchanged(db, db2, od, ld, a, ok, |x: Lineitem| x.suppkey != s);
    lemma_li_count_unchanged(db, db2, od, ld, a, ok, |x: Lineitem| x.suppkey != s && late(x));
    lemma_sum_ext_all(
        db.lineitem,
        |x: Lineitem| ind(x.orderkey == ok && x.suppkey != s),
        |x: Lineitem| ind(x.orderkey == ok && (|y: Lineitem| y.suppkey != s)(x)),
    );
    lemma_sum_ext_all(
        db2.lineitem,
        |x: Lineitem| ind(x.orderkey == ok && x.suppkey != s),
        |x: Lineitem| ind(x.orderkey == ok && (|y: Lineitem| y.suppkey != s)(x)),
    );
    lemma_sum_ext_all(
        db.lineitem,
        |x: Lineitem| ind(x.orderkey == ok && x.suppkey != s && late(x)),
        |x: Lineitem| ind(x.orderkey == ok && (|y: Lineitem| y.suppkey != s && late(y))(x)),
    );
    lemma_sum_ext_all(
        db2.lineitem,
        |x: Lineitem| ind(x.orderkey == ok && x.suppkey != s && late(x)),
        |x: Lineitem| ind(x.orderkey == ok && (|y: Lineitem| y.suppkey != s && late(y))(x)),
    );
    lemma_qual_key(db, l);
    lemma_qual_key(db2, l);
}

pub open spec fn in_term(db: Db, a: Seq<int>, sk: int, l: Lineitem) -> int {
    if a.contains(l.orderkey) {
        w_term(db, sk, l)
    } else {
        0
    }
}

pub open spec fn out_term(db: Db, a: Seq<int>, sk: int, l: Lineitem) -> int {
    if a.contains(l.orderkey) {
        0
    } else {
        w_term(db, sk, l)
    }
}

pub proof fn lemma_in_part_key(db: Db, a: Seq<int>, sk: int, k: int)
    requires
        a.contains(k),
    ensures
        sum(
            db.lineitem,
            |x: Lineitem| if x.orderkey == k { in_term(db, a, sk, x) } else { 0 },
        ) == per_order(db, k, sk),
{
    lemma_sum_ext_all(
        db.lineitem,
        |x: Lineitem| if x.orderkey == k { in_term(db, a, sk, x) } else { 0 },
        |l: Lineitem| if l.orderkey == k { w_term(db, sk, l) } else { 0 },
    );
    lemma_per_order(db, k, sk);
}

/// Σ_l [l_orderkey ∈ a] w_term = Σ_{ok ∈ a} per_order(ok).
pub proof fn lemma_in_part(db: Db, a: Seq<int>, sk: int)
    requires
        distinct(a),
    ensures
        sum(db.lineitem, |l: Lineitem| in_term(db, a, sk, l)) == sum(
            a,
            |ok: int| per_order(db, ok, sk),
        ),
{
    let f = |l: Lineitem| in_term(db, a, sk, l);
    let key = |l: Lineitem| l.orderkey;
    let per = |k: int|
        sum(db.lineitem, |x: Lineitem| if x.orderkey == k { in_term(db, a, sk, x) } else { 0 });
    assert forall|k: int| #[trigger] per(k) == sum(
        db.lineitem,
        |x: Lineitem| if key(x) == k { f(x) } else { 0 },
    ) by {
        lemma_sum_ext_all(
            db.lineitem,
            |x: Lineitem| if key(x) == k { f(x) } else { 0 },
            |x: Lineitem| if x.orderkey == k { in_term(db, a, sk, x) } else { 0 },
        );
    }
    lemma_sum_group(db.lineitem, a, key, f, per);
    assert forall|i: int| 0 <= i < a.len() implies #[trigger] per(a[i]) == per_order(
        db,
        a[i],
        sk,
    ) by {
        assert(a.contains(a[i]));
        lemma_in_part_key(db, a, sk, a[i]);
    }
    lemma_sum_ext(a, per, |ok: int| per_order(db, ok, sk));
}

/// W(sk) = Σ out + Σ_{ok ∈ a} per_order.
pub proof fn lemma_w_split(db: Db, a: Seq<int>, sk: int)
    requires
        distinct(a),
    ensures
        w(db, sk) == sum(db.lineitem, |l: Lineitem| out_term(db, a, sk, l)) + sum(
            a,
            |ok: int| per_order(db, ok, sk),
        ),
{
    lemma_sum_add(
        db.lineitem,
        |l: Lineitem| out_term(db, a, sk, l),
        |l: Lineitem| in_term(db, a, sk, l),
        |l: Lineitem| w_term(db, sk, l),
    );
    lemma_in_part(db, a, sk);
}

/// Locality: a transaction whose deltas touch only orderkeys in `a` changes
/// W(sk) by Σ_{ok ∈ a} (new per-order contribution − old).
pub proof fn lemma_w_step(
    db: Db,
    db2: Db,
    od: Seq<(Order, int)>,
    ld: Seq<(Lineitem, int)>,
    a: Seq<int>,
    sk: int,
)
    requires
        db_step(db, db2, od, ld),
        deltas_within(a, od, ld),
        distinct(a),
    ensures
        w(db2, sk) == w(db, sk) + sum(
            a,
            |ok: int| per_order(db2, ok, sk) - per_order(db, ok, sk),
        ),
{
    lemma_w_split(db, a, sk);
    lemma_w_split(db2, a, sk);
    // Σ_{db2} out(db2) = Σ_{db2} out(db) = Σ_{db} out(db) + Σ_ld sign·out(db) = Σ_{db} out(db).
    let o1 = |l: Lineitem| out_term(db, a, sk, l);
    let o2 = |l: Lineitem| out_term(db2, a, sk, l);
    assert forall|l: Lineitem| #[trigger] o2(l) == o1(l) by {
        if !a.contains(l.orderkey) {
            lemma_w_term_unchanged(db, db2, od, ld, a, sk, l);
        }
    }
    lemma_sum_ext_all(db2.lineitem, o2, o1);
    assert(sum(db2.lineitem, o1) == sum(db.lineitem, o1) + sum(
        ld,
        |d: (Lineitem, int)| d.1 * o1(d.0),
    ));
    assert forall|i: int| 0 <= i < ld.len() implies o1((#[trigger] ld[i]).0) == 0 by {
        assert(a.contains(ld[i].0.orderkey));
    }
    lemma_delta_zero(ld, o1);
    lemma_sum_sub(
        a,
        |ok: int| per_order(db2, ok, sk),
        |ok: int| per_order(db, ok, sk),
        |ok: int| per_order(db2, ok, sk) - per_order(db, ok, sk),
    );
}

/// New counts after the step, per (ok, sk), from the deltas.
pub proof fn lemma_counts_step(
    db: Db,
    db2: Db,
    od: Seq<(Order, int)>,
    ld: Seq<(Lineitem, int)>,
    ok: int,
    sk: int,
)
    requires
        db_step(db, db2, od, ld),
    ensures
        fm(db2, ok) == fm(db, ok) + sum(
            od,
            |d: (Order, int)| d.1 * ind(d.0.orderkey == ok && d.0.status_is_f),
        ),
        cnt(db2, ok, sk) == cnt(db, ok, sk) + sum(
            ld,
            |d: (Lineitem, int)| d.1 * ind(d.0.orderkey == ok && d.0.suppkey == sk),
        ),
        lcnt(db2, ok, sk) == lcnt(db, ok, sk) + sum(
            ld,
            |d: (Lineitem, int)| d.1 * ind(d.0.orderkey == ok && d.0.suppkey == sk && late(d.0)),
        ),
{
    let fo = |o: Order| ind(o.orderkey == ok && o.status_is_f);
    let fc = |l: Lineitem| ind(l.orderkey == ok && l.suppkey == sk);
    let fl = |l: Lineitem| ind(l.orderkey == ok && l.suppkey == sk && late(l));
    assert(sum(db2.orders, fo) == sum(db.orders, fo) + sum(od, |d: (Order, int)| d.1 * fo(d.0)));
    assert(sum(db2.lineitem, fc) == sum(db.lineitem, fc) + sum(
        ld,
        |d: (Lineitem, int)| d.1 * fc(d.0),
    ));
    assert(sum(db2.lineitem, fl) == sum(db.lineitem, fl) + sum(
        ld,
        |d: (Lineitem, int)| d.1 * fl(d.0),
    ));
    lemma_sum_ext_all(
        od,
        |d: (Order, int)| d.1 * fo(d.0),
        |d: (Order, int)| d.1 * ind(d.0.orderkey == ok && d.0.status_is_f),
    );
    lemma_sum_ext_all(
        ld,
        |d: (Lineitem, int)| d.1 * fc(d.0),
        |d: (Lineitem, int)| d.1 * ind(d.0.orderkey == ok && d.0.suppkey == sk),
    );
    lemma_sum_ext_all(
        ld,
        |d: (Lineitem, int)| d.1 * fl(d.0),
        |d: (Lineitem, int)| d.1 * ind(d.0.orderkey == ok && d.0.suppkey == sk && late(d.0)),
    );
}

} // verus!
