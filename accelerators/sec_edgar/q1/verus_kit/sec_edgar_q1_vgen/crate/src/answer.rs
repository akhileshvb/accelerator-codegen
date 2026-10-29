// The spec's group aggregates, in terms of the plain table rows.
use vstd::prelude::*;
use verus_common::sums::*;
use verus_common::rel::*;
use verus_common::order::*;
use crate::gen_spec::*;
use verus_prooflib::ordered::{Index, KeyView, lemma_key_contribution, contribution};
use crate::model::*;
use crate::steps::*;

verus! {

pub open spec fn p_where(params: Params, r: PreRow) -> bool { r.stmt == Some(params.p1) }

pub proof fn lemma_tuples_sum(db: Db, params: Params, f: spec_fn(Tuple) -> int)
    ensures
        sum(tuples(db, params), f) == sum(db.pre, |r: PreRow| if p_where(params, r) { f(Tuple { pre: r }) } else { 0 }),
{
    let g = |pre: PreRow| tuples_1(db, params, pre);
    lemma_sum_flat_map(db.pre, g, f);
    assert(tuples(db, params) == flat(db.pre.map_values(g)));
    let per = |a: PreRow| sum(g(a), f);
    let target = |r: PreRow| if p_where(params, r) { f(Tuple { pre: r }) } else { 0 };
    assert forall|a: PreRow| #[trigger] per(a) == target(a) by {
        lemma_sum_opt(where_clause(db, params, a), Tuple { pre: a }, f);
        assert(where_clause(db, params, a) == p_where(params, a));
    }
    lemma_sum_ext_all(db.pre, per, target);
}

/// The group's COUNT(*) is its row count in the table.
pub proof fn lemma_group_count_is(db: Db, params: Params, gr: Option<Seq<u8>>)
    ensures
        group_count(db, params, Some(params.p1), gr) == sum(db.pre, w_cnt(params.p1, gr)),
{
    let f = |t: Tuple| if t.pre.stmt == Some(params.p1) && t.pre.rfile == gr { 1int } else { 0 };
    lemma_tuples_sum(db, params, f);
    lemma_sum_ext_all(db.pre,
        |r: PreRow| if p_where(params, r) { f(Tuple { pre: r }) } else { 0 }, w_cnt(params.p1, gr));
}

pub proof fn lemma_group_count_other(db: Db, params: Params, gs: Option<Seq<u8>>, gr: Option<Seq<u8>>)
    requires
        gs != Some(params.p1),
    ensures
        group_count(db, params, gs, gr) == 0,
{
    let f = |t: Tuple| if t.pre.stmt == gs && t.pre.rfile == gr { 1int } else { 0 };
    lemma_tuples_sum(db, params, f);
    let target = |r: PreRow| if p_where(params, r) { f(Tuple { pre: r }) } else { 0 };
    assert forall|i: int| 0 <= i < db.pre.len() implies target(#[trigger] db.pre[i]) == 0 by {}
    lemma_sum_zero(db.pre, target);
}

pub open spec fn line_present(t: Tuple) -> int { if t.pre.line is Some { 1int } else { 0 } }
pub open spec fn line_value(t: Tuple) -> int { if t.pre.line is Some { t.pre.line->0 } else { 0 } }

pub proof fn lemma_group_lines(db: Db, params: Params, gr: Option<Seq<u8>>)
    ensures
        group_sum(db, params, Some(params.p1), gr, |t: Tuple| if t.pre.line is Some { 1int } else { 0 })
            == sum(db.pre, w_lcnt(params.p1, gr)),
        group_sum(db, params, Some(params.p1), gr, |t: Tuple| if t.pre.line is Some { t.pre.line->0 } else { 0 })
            == sum(db.pre, w_lsum(params.p1, gr)),
{
    let f1 = |t: Tuple| if t.pre.line is Some { 1int } else { 0 };
    let g1 = |t: Tuple| if t.pre.stmt == Some(params.p1) && t.pre.rfile == gr { f1(t) } else { 0 };
    lemma_tuples_sum(db, params, g1);
    lemma_sum_ext_all(db.pre,
        |r: PreRow| if p_where(params, r) { g1(Tuple { pre: r }) } else { 0 }, w_lcnt(params.p1, gr));
    let f2 = |t: Tuple| if t.pre.line is Some { t.pre.line->0 } else { 0 };
    let g2 = |t: Tuple| if t.pre.stmt == Some(params.p1) && t.pre.rfile == gr { f2(t) } else { 0 };
    lemma_tuples_sum(db, params, g2);
    lemma_sum_ext_all(db.pre,
        |r: PreRow| if p_where(params, r) { g2(Tuple { pre: r }) } else { 0 }, w_lsum(params.p1, gr));
}

pub open spec fn vals_of(db: Db, params: Params, gr: Option<Seq<u8>>) -> Seq<Seq<u8>> {
    group_vals_where(db, params, Some(params.p1), gr, |t: Tuple| t.pre.adsh is Some, |t: Tuple| t.pre.adsh->0)
}

/// The values of group_vals_where, in terms of the table's rows.
pub proof fn lemma_gvw_contains(
    db: Db, params: Params, gs: Option<Seq<u8>>, gr: Option<Seq<u8>>,
    keep: spec_fn(Tuple) -> bool, f: spec_fn(Tuple) -> Seq<u8>, y: Seq<u8>,
)
    ensures
        group_vals_where(db, params, gs, gr, keep, f).contains(y) <==> exists|i: int|
            0 <= i < db.pre.len() && p_where(params, #[trigger] db.pre[i]) && db.pre[i].stmt == gs && db.pre[i].rfile == gr
                && keep(Tuple { pre: db.pre[i] }) && f(Tuple { pre: db.pre[i] }) == y,
{
    let ts = tuples(db, params);
    let g = |pre: PreRow| tuples_1(db, params, pre);
    let pred = |t: Tuple| t.pre.stmt == gs && t.pre.rfile == gr && keep(t);
    lemma_map_where(ts, pred, f);
    assert(ts == flat(db.pre.map_values(g)));
    if group_vals_where(db, params, gs, gr, keep, f).contains(y) {
        let k = choose|k: int| 0 <= k < ts.len() && pred(ts[k]) && f(ts[k]) == y;
        assert(0 <= k < ts.len() && ts[k] == ts[k]);
        assert(ts.contains(ts[k]));
        lemma_flat_contains(db.pre.map_values(g), ts[k]);
        let i = choose|i: int| 0 <= i < db.pre.map_values(g).len() && (#[trigger] db.pre.map_values(g)[i]).contains(ts[k]);
        assert(db.pre.map_values(g)[i] == g(db.pre[i]));
        assert(where_clause(db, params, db.pre[i]));
        lemma_singleton_contains(Tuple { pre: db.pre[i] }, ts[k]);
        assert(ts[k] == Tuple { pre: db.pre[i] });
        assert(p_where(params, db.pre[i]) && db.pre[i].stmt == gs && db.pre[i].rfile == gr
            && keep(Tuple { pre: db.pre[i] }) && f(Tuple { pre: db.pre[i] }) == y);
    }
    if exists|i: int| 0 <= i < db.pre.len() && p_where(params, #[trigger] db.pre[i]) && db.pre[i].stmt == gs && db.pre[i].rfile == gr
            && keep(Tuple { pre: db.pre[i] }) && f(Tuple { pre: db.pre[i] }) == y {
        let i = choose|i: int| 0 <= i < db.pre.len() && p_where(params, #[trigger] db.pre[i]) && db.pre[i].stmt == gs && db.pre[i].rfile == gr
            && keep(Tuple { pre: db.pre[i] }) && f(Tuple { pre: db.pre[i] }) == y;
        let t = Tuple { pre: db.pre[i] };
        assert(g(db.pre[i]) == seq![t]);
        assert(db.pre.map_values(g)[i] == seq![t]);
        assert(seq![t].contains(t)) by { assert(seq![t][0] == t); }
        lemma_flat_contains(db.pre.map_values(g), t);
        let k = choose|k: int| 0 <= k < ts.len() && ts[k] == t;
        assert(pred(ts[k]) && f(ts[k]) == y);
    }
}

/// A filing is among the group's filings exactly when the group holds a row of it.
pub proof fn lemma_vals_contains(db: Db, params: Params, gr: Option<Seq<u8>>, a: Seq<u8>)
    ensures
        vals_of(db, params, gr).contains(a) <==> sum(db.pre, w_cell(params.p1, gr, a)) > 0,
{
    lemma_gvw_contains(db, params, Some(params.p1), gr, |t: Tuple| t.pre.adsh is Some, |t: Tuple| t.pre.adsh->0, a);
    let cell = w_cell(params.p1, gr, a);
    assert forall|i: int| 0 <= i < db.pre.len() implies cell(#[trigger] db.pre[i]) >= 0 by {}
    if vals_of(db, params, gr).contains(a) {
        let i = choose|i: int| 0 <= i < db.pre.len() && p_where(params, #[trigger] db.pre[i]) && db.pre[i].stmt == Some(params.p1)
            && db.pre[i].rfile == gr && db.pre[i].adsh is Some && db.pre[i].adsh->0 == a;
        assert(db.pre[i].adsh == Some(a));
        assert(cell(db.pre[i]) == 1);
        lemma_sum_pos(db.pre, cell, i);
    }
    if sum(db.pre, cell) > 0 {
        let i = lemma_sum_nonzero_witness(db.pre, cell);
        assert(p_where(params, db.pre[i]));
    }
}

pub proof fn lemma_cell_contribution(cells: Index<i128>, k: KeyView, f: spec_fn((KeyView, i128)) -> int)
    requires
        cells.wf(),
    ensures
        sum(cells.entries(), |e: (KeyView, i128)| if e.0 == k { f(e) } else { 0 }) == contribution(cells, k, f),
{
    lemma_key_contribution(&cells, k, f);
}

pub proof fn lemma_cell_counts_one(
    cells: Index<i128>, i: int, k: KeyView, f: spec_fn((KeyView, i128)) -> int, g: spec_fn((KeyView, i128)) -> int,
)
    requires
        f == filing_w(i),
        cells.wf(),
        cells.at(k) is Some,
        k.0 == gkey(i),
        forall|e: (KeyView, i128)| #[trigger] g(e) == if e.0 == k { f(e) } else { 0int },
    ensures
        sum(cells.entries(), g) == 1,
{
    lemma_cell_contribution(cells, k, f);
    let v = cells.at(k)->0;
    assert(contribution(cells, k, f) == f((k, v)));
    lemma_sum_ext_all(cells.entries(), g, |e: (KeyView, i128)| if e.0 == k { f(e) } else { 0 });
}

pub proof fn lemma_singleton_contains<A>(t: A, x: A)
    requires
        seq![t].contains(x),
    ensures
        x == t,
{
    let j = choose|j: int| 0 <= j < 1 && seq![t][j] == x;
}

pub proof fn lemma_sum_ones<A>(s: Seq<A>)
    ensures
        sum(s, |x: A| 1int) == s.len(),
    decreases s.len(),
{
    if s.len() > 0 { lemma_sum_ones(s.drop_last()); }
}

/// The group's filing total is its COUNT(DISTINCT adsh).
pub proof fn lemma_filings_distinct_with(db: Db, params: Params, aggs: Seq<Agg>, cells: Index<i128>, i: int, f: spec_fn((KeyView, i128)) -> int)
    requires
        f == filing_w(i),
        cells_ok(aggs, cells, db.pre, Seq::empty()),
        filings_ok(aggs, cells),
        0 <= i < aggs.len(),
        aggs[i].s() == params.p1,
    ensures
        aggs[i].filings as int == distinct_count(vals_of(db, params, aggs[i].rf())),
{
    let gr = aggs[i].rf();
    let vals = vals_of(db, params, gr);
    lemma_dedup(vals);
    let d = dedup(vals);
    let ks = d.map_values(|a: Seq<u8>| (gkey(i), a));
    let entries = cells.entries();
    cells.model();
    assert forall|x: int, y: int| 0 <= x < ks.len() && 0 <= y < ks.len() && x != y implies ks[x] != ks[y] by {
        assert(d[x] != d[y]);
    }
    let key = |e: (KeyView, i128)| e.0;
    let per = |k: KeyView| sum(entries, |e: (KeyView, i128)| if key(e) == k { f(e) } else { 0 });
    assert forall|p: int| 0 <= p < entries.len() && f(#[trigger] entries[p]) != 0 implies ks.contains(key(entries[p])) by {
        let e = entries[p];
        let t = e.0.1;
        assert(cells.at(e.0) == Some(e.1));
        assert(e.0 == (gkey(i), t));
        let c = vsum(db.pre, Seq::empty(), w_cell(aggs[i].s(), aggs[i].rf(), t));
        assert(cell_ok(cells.at((gkey(i), t)), c));
        lemma_sum_empty(Seq::<(PreRow, int)>::empty(), |d: (PreRow, int)| d.1 * w_cell(aggs[i].s(), aggs[i].rf(), t)(d.0));
        assert(c == sum(db.pre, w_cell(params.p1, gr, t)));
        let cell = w_cell(params.p1, gr, t);
        assert forall|q: int| 0 <= q < db.pre.len() implies cell(#[trigger] db.pre[q]) >= 0 by {}
        if sum(db.pre, cell) <= 0 { lemma_sum_nonneg(db.pre, cell); }
        lemma_vals_contains(db, params, gr, t);
        assert(d.contains(t));
        let j = choose|j: int| 0 <= j < d.len() && d[j] == t;
        assert(ks[j] == e.0);
    }
    lemma_sum_group(entries, ks, key, f, per);
    assert forall|k: KeyView| ks.contains(k) implies #[trigger] per(k) == 1 by {
        let j = choose|j: int| 0 <= j < ks.len() && ks[j] == k;
        let a = d[j];
        assert(k == (gkey(i), a));
        lemma_vals_contains(db, params, gr, a);
        assert(d[j] == a);
        assert(d.contains(a));
        assert(vals.contains(a));
        lemma_sum_empty(Seq::<(PreRow, int)>::empty(), |d: (PreRow, int)| d.1 * w_cell(aggs[i].s(), aggs[i].rf(), a)(d.0));
        let c = vsum(db.pre, Seq::empty(), w_cell(aggs[i].s(), aggs[i].rf(), a));
        assert(cell_ok(cells.at((gkey(i), a)), c));
        assert(c == sum(db.pre, w_cell(params.p1, gr, a)));
        assert(c > 0);
        assert(cells.at(k) is Some);
        let g = |e: (KeyView, i128)| if key(e) == k { f(e) } else { 0 };
        assert(forall|e: (KeyView, i128)| #[trigger] g(e) == if e.0 == k { f(e) } else { 0int });
        lemma_cell_counts_one(cells, i, k, f, g);
        assert(per(k) == sum(entries, g));
    }
    lemma_sum_ext(ks, per, |k: KeyView| 1int);
    lemma_sum_ones(ks);
}

pub proof fn lemma_filings_distinct(db: Db, params: Params, aggs: Seq<Agg>, cells: Index<i128>, i: int)
    requires
        cells_ok(aggs, cells, db.pre, Seq::empty()),
        filings_ok(aggs, cells),
        0 <= i < aggs.len(),
        aggs[i].s() == params.p1,
    ensures
        aggs[i].filings as int == distinct_count(vals_of(db, params, aggs[i].rf())),
{
    lemma_filings_distinct_with(db, params, aggs, cells, i, filing_w(i));
}

} // verus!
