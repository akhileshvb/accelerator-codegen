// The spec's group counts and sums as nested sums over the three tables, and signed row lists.
#![allow(unused_imports)]
use vstd::prelude::*;
use verus_common::rel::*;
use verus_common::sums::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;

verus! {

/// A group of the query, with the parameters it belongs to.
pub struct GK {
    pub fy: int,
    pub stmt: Seq<u8>,
    pub uom: Seq<u8>,
    pub name: Option<Seq<u8>>,
    pub tag: Seq<u8>,
    pub plabel: Option<Seq<u8>>,
}

/// The triple (n, s, p) joins and passes WHERE for the parameters its row fixes.
pub open spec fn jk(n: NumRow, s: SubRow, p: PreRow) -> bool {
    &&& n.adsh == Some(s.adsh)
    &&& p.adsh == n.adsh
    &&& n.tag is Some
    &&& p.tag == n.tag
    &&& n.version is Some
    &&& p.version == n.version
    &&& n.value is Some
    &&& n.uom is Some
    &&& s.fy is Some
    &&& p.stmt is Some
}

pub open spec fn key_of(n: NumRow, s: SubRow, p: PreRow) -> GK {
    GK { fy: s.fy->0, stmt: p.stmt->0, uom: n.uom->0, name: s.name, tag: n.tag->0, plabel: p.plabel }
}

pub type H3 = spec_fn(NumRow, SubRow, PreRow) -> int;

pub open spec fn icnt(k: GK) -> H3 {
    |n: NumRow, s: SubRow, p: PreRow| if jk(n, s, p) && key_of(n, s, p) == k { 1int } else { 0 }
}

pub open spec fn ival(k: GK) -> H3 {
    |n: NumRow, s: SubRow, p: PreRow| if jk(n, s, p) && key_of(n, s, p) == k { n.value->0 } else { 0 }
}

pub open spec fn iabs(k: GK) -> H3 {
    |n: NumRow, s: SubRow, p: PreRow| if jk(n, s, p) && key_of(n, s, p) == k { abs(n.value->0) } else { 0 }
}

/// Σ h over the join of the three tables.
pub open spec fn gdb(db: Db, h: H3) -> int {
    sum(db.num, |n: NumRow| sum(db.sub, |s: SubRow| sum(db.pre, |p: PreRow| h(n, s, p))))
}

pub open spec fn cntk(db: Db, k: GK) -> int {
    gdb(db, icnt(k))
}

pub open spec fn sumk(db: Db, k: GK) -> int {
    gdb(db, ival(k))
}

pub open spec fn absk(db: Db, k: GK) -> int {
    gdb(db, iabs(k))
}

pub open spec fn k_params(k: GK) -> Params {
    params(k.fy, k.stmt, k.uom)
}

/// The spec's group fits key k exactly when stmt and tag are the ones the key carries.
pub open spec fn is_key(p: Params, g_stmt: Option<Seq<u8>>, g_tag: Option<Seq<u8>>) -> bool {
    g_stmt == Some(p.p2) && g_tag is Some
}

pub open spec fn k_of(p: Params, g_name: Option<Seq<u8>>, g_tag: Option<Seq<u8>>, g_plabel: Option<Seq<u8>>) -> GK {
    GK { fy: p.p1, stmt: p.p2, uom: p.p3, name: g_name, tag: g_tag->0, plabel: g_plabel }
}


proof fn lemma_where_key(db: Db, p: Params, n: NumRow, s: SubRow, pp: PreRow, g_name: Option<Seq<u8>>, g_stmt: Option<Seq<u8>>, g_tag: Option<Seq<u8>>, g_plabel: Option<Seq<u8>>)
    ensures
        (where_clause(db, p, n, s, pp) && s.name == g_name && pp.stmt == g_stmt && n.tag == g_tag && pp.plabel == g_plabel)
            <==> (is_key(p, g_stmt, g_tag) && jk(n, s, pp) && key_of(n, s, pp) == k_of(p, g_name, g_tag, g_plabel)),
        (is_key(p, g_stmt, g_tag) && jk(n, s, pp) && key_of(n, s, pp) == k_of(p, g_name, g_tag, g_plabel)) ==> where_clause(db, p, n, s, pp),
{
    reveal(where_clause);
}

/// The group aggregate's integrand over a triple.
pub open spec fn b3(p: Params, g_name: Option<Seq<u8>>, g_stmt: Option<Seq<u8>>, g_tag: Option<Seq<u8>>, g_plabel: Option<Seq<u8>>, f: spec_fn(Tuple) -> int, n: NumRow, s: SubRow, pp: PreRow) -> int {
    if is_key(p, g_stmt, g_tag) && jk(n, s, pp) && key_of(n, s, pp) == k_of(p, g_name, g_tag, g_plabel) { f(Tuple { n, s_: s, p: pp }) } else { 0 }
}

pub open spec fn pre_b(db: Db, p: Params, g_name: Option<Seq<u8>>, g_stmt: Option<Seq<u8>>, g_tag: Option<Seq<u8>>, g_plabel: Option<Seq<u8>>, f: spec_fn(Tuple) -> int, n: NumRow, s: SubRow) -> int {
    sum(db.pre, |pp: PreRow| b3(p, g_name, g_stmt, g_tag, g_plabel, f, n, s, pp))
}

/// The spec's group aggregate for weight f is the nested sum of f over the triples of the group.
pub proof fn lemma_group_sum_nested(db: Db, p: Params, g_name: Option<Seq<u8>>, g_stmt: Option<Seq<u8>>, g_tag: Option<Seq<u8>>, g_plabel: Option<Seq<u8>>, f: spec_fn(Tuple) -> int)
    ensures
        group_sum(db, p, g_name, g_stmt, g_tag, g_plabel, f) == if is_key(p, g_stmt, g_tag) {
            gdb(db, |n: NumRow, s: SubRow, pp: PreRow| if jk(n, s, pp) && key_of(n, s, pp) == k_of(p, g_name, g_tag, g_plabel) { f(Tuple { n, s_: s, p: pp }) } else { 0 })
        } else { 0 },
{
    let w = |t: Tuple| if t.s_.name == g_name && t.p.stmt == g_stmt && t.n.tag == g_tag && t.p.plabel == g_plabel { f(t) } else { 0 };
    reveal(group_sum);
    reveal(tuples);
    let g0 = |n: NumRow| tuples_1(db, p, n);
    lemma_sum_flat_map(db.num, g0, w);
    assert(tuples(db, p) == flat(db.num.map_values(g0)));
    let level2 = |n: NumRow| sum(db.sub, |s: SubRow| pre_b(db, p, g_name, g_stmt, g_tag, g_plabel, f, n, s));
    assert forall|n: NumRow| #[trigger] sum(tuples_1(db, p, n), w) == level2(n) by {
        reveal(tuples_1);
        let g1 = |s: SubRow| tuples_2(db, p, n, s);
        lemma_sum_flat_map(db.sub, g1, w);
        assert(tuples_1(db, p, n) == flat(db.sub.map_values(g1)));
        assert forall|s: SubRow| #[trigger] sum(tuples_2(db, p, n, s), w) == pre_b(db, p, g_name, g_stmt, g_tag, g_plabel, f, n, s) by {
            reveal(tuples_2);
            let g2 = |pp: PreRow| tuples_3(db, p, n, s, pp);
            lemma_sum_flat_map(db.pre, g2, w);
            assert(tuples_2(db, p, n, s) == flat(db.pre.map_values(g2)));
            let a = |pp: PreRow| sum(g2(pp), w);
            let b = |pp: PreRow| b3(p, g_name, g_stmt, g_tag, g_plabel, f, n, s, pp);
            assert forall|pp: PreRow| #[trigger] a(pp) == b(pp) by {
                reveal(tuples_3);
                lemma_where_key(db, p, n, s, pp, g_name, g_stmt, g_tag, g_plabel);
                let t = Tuple { n, s_: s, p: pp };
                if where_clause(db, p, n, s, pp) {
                    lemma_sum_singleton(t, w);
                } else {
                    lemma_sum_empty(Seq::<Tuple>::empty(), w);
                }
            }
            lemma_sum_ext_all(db.pre, a, b);
        }
        let c1 = |s: SubRow| sum(g1(s), w);
        assert forall|s: SubRow| #[trigger] c1(s) == pre_b(db, p, g_name, g_stmt, g_tag, g_plabel, f, n, s) by {
            assert(g1(s) == tuples_2(db, p, n, s));
        }
        lemma_sum_ext_all(db.sub, c1, |s: SubRow| pre_b(db, p, g_name, g_stmt, g_tag, g_plabel, f, n, s));
    }
    let c0 = |n: NumRow| sum(g0(n), w);
    assert forall|n: NumRow| #[trigger] c0(n) == level2(n) by { assert(g0(n) == tuples_1(db, p, n)); }
    lemma_sum_ext_all(db.num, c0, level2);
    if is_key(p, g_stmt, g_tag) {
        let h = |n: NumRow, s: SubRow, pp: PreRow| if jk(n, s, pp) && key_of(n, s, pp) == k_of(p, g_name, g_tag, g_plabel) { f(Tuple { n, s_: s, p: pp }) } else { 0 };
        assert forall|n: NumRow, s: SubRow, pp: PreRow| #[trigger] b3(p, g_name, g_stmt, g_tag, g_plabel, f, n, s, pp) == h(n, s, pp) by {}
        assert forall|n: NumRow| #[trigger] level2(n) == sum(db.sub, |s: SubRow| sum(db.pre, |pp: PreRow| h(n, s, pp))) by {
            assert forall|s: SubRow| #[trigger] pre_b(db, p, g_name, g_stmt, g_tag, g_plabel, f, n, s) == sum(db.pre, |pp: PreRow| h(n, s, pp)) by {
                lemma_sum_ext_all(db.pre, |pp: PreRow| b3(p, g_name, g_stmt, g_tag, g_plabel, f, n, s, pp), |pp: PreRow| h(n, s, pp));
            }
            lemma_sum_ext_all(db.sub, |s: SubRow| pre_b(db, p, g_name, g_stmt, g_tag, g_plabel, f, n, s), |s: SubRow| sum(db.pre, |pp: PreRow| h(n, s, pp)));
        }
        lemma_sum_ext_all(db.num, level2, |n: NumRow| sum(db.sub, |s: SubRow| sum(db.pre, |pp: PreRow| h(n, s, pp))));
    } else {
        let z = |x: int| 0int;
        assert forall|n: NumRow| #[trigger] level2(n) == 0 by {
            assert forall|s: SubRow| #[trigger] pre_b(db, p, g_name, g_stmt, g_tag, g_plabel, f, n, s) == 0 by {
                lemma_sum_zero(db.pre, |pp: PreRow| b3(p, g_name, g_stmt, g_tag, g_plabel, f, n, s, pp));
            }
            lemma_sum_zero(db.sub, |s: SubRow| pre_b(db, p, g_name, g_stmt, g_tag, g_plabel, f, n, s));
        }
        lemma_sum_zero(db.num, level2);
    }
}

} // verus!
