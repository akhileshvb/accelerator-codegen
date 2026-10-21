// The general read computes the nation totals: regrouping the lines by (part, supplier).
use vstd::prelude::*;
use verus_common::sums::*;
use verus_common::sqlfns::*;
use verus_common::rel::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::meaning::*;
use crate::rollup::*;
use crate::query_proofs::*;
use crate::pmap::*;
use crate::gnames::*;
use crate::gapply::*;
use crate::gread::*;
use crate::gidx::*;
use crate::gagg::*;
use crate::storage::*;

verus! {

broadcast use vstd::seq_lib::group_seq_properties;

pub open spec fn pfdb(db: Db, q: int, s: int, y: int, f: int) -> int {
    if f == 0 { pair_count(db, q, s) * moments(db, q, s, y, 0) }
    else { pair_count(db, q, s) * moments(db, q, s, y, 1) - pair_cost(db, q, s) * moments(db, q, s, y, 2) }
}

/// What the (part, supplier) pair q, s adds to the nation-year totals of pattern pat.
pub open spec fn vdb(db: Db, pat: Seq<u8>, nk: int, y: int, f: int, q: int, s: int) -> int {
    supplier_count(db, s, nk) * green(filter_db(db, pat), q) * pfdb(db, q, s, y, f)
}

pub open spec fn keyed(q: int, s: int, g: spec_fn(LineitemRow) -> int) -> spec_fn(LineitemRow) -> int {
    |l: LineitemRow| if l.l_partkey == q && l.l_suppkey == s { g(l) } else { 0int }
}

pub open spec fn hl(db: Db, pat: Seq<u8>, nk: int, y: int, f: int) -> spec_fn(LineitemRow) -> int {
    |l: LineitemRow| hline(db, pat, l, nk, y, f)
}

/// The lines of one (part, supplier) pair add to the totals what the pair's moments say.
#[verifier::spinoff_prover]
pub proof fn lemma_pair_lines(db: Db, pat: Seq<u8>, nk: int, y: int, f: int, q: int, s: int)
    requires 0 <= f <= 1,
    ensures sum(db.lineitem, keyed(q, s, hl(db, pat, nk, y, f))) == vdb(db, pat, nk, y, f, q, s),
{
    let ce = supplier_count(db, s, nk) * green(filter_db(db, pat), q);
    let pc = pair_count(db, q, s);
    let pcost = pair_cost(db, q, s);
    let m = |f2: int| keyed(q, s, |l: LineitemRow| dates(db, l.l_orderkey, y) * line_weight(l, f2));
    let dpf = keyed(q, s, |l: LineitemRow| dates(db, l.l_orderkey, y) * pf(db, l, f));
    assert forall|l: LineitemRow| #[trigger] keyed(q, s, hl(db, pat, nk, y, f))(l) == ce * dpf(l) by {
        if l.l_partkey == q && l.l_suppkey == s {
            let d = dates(db, l.l_orderkey, y);
            let p = pf(db, l, f);
            assert(d * (supplier_count(db, s, nk) * green(filter_db(db, pat), q) * p) == ce * (d * p)) by (nonlinear_arith)
                requires ce == supplier_count(db, s, nk) * green(filter_db(db, pat), q);
        }
    }
    lemma_sum_scale(db.lineitem, dpf, keyed(q, s, hl(db, pat, nk, y, f)), ce);
    if f == 0 {
        assert forall|l: LineitemRow| #[trigger] dpf(l) == pc * m(0)(l) by {
            if l.l_partkey == q && l.l_suppkey == s {
                let d = dates(db, l.l_orderkey, y);
                assert(line_weight(l, 0) == 1);
                assert(pf(db, l, 0) == pc);
                assert(d * pc == pc * (d * 1)) by (nonlinear_arith);
            }
        }
        lemma_sum_scale(db.lineitem, m(0), dpf, pc);
        lemma_sum_ext_all(db.lineitem, m(0), |row: LineitemRow| if row.l_partkey == q && row.l_suppkey == s { dates(db, row.l_orderkey, y) * line_weight(row, 0) } else { 0int });
    } else {
        let a = |l: LineitemRow| pc * m(1)(l);
        let b = |l: LineitemRow| pcost * m(2)(l);
        assert forall|l: LineitemRow| #[trigger] dpf(l) == a(l) - b(l) by {
            if l.l_partkey == q && l.l_suppkey == s {
                let d = dates(db, l.l_orderkey, y);
                assert(pf(db, l, 1) == pc * line_weight(l, 1) - pcost * l.l_quantity);
                assert(line_weight(l, 2) == l.l_quantity);
                assert(d * (pc * line_weight(l, 1) - pcost * l.l_quantity) == pc * (d * line_weight(l, 1)) - pcost * (d * l.l_quantity)) by (nonlinear_arith);
            }
        }
        lemma_sum_sub(db.lineitem, a, b, dpf);
        lemma_sum_scale(db.lineitem, m(1), a, pc);
        lemma_sum_scale(db.lineitem, m(2), b, pcost);
        lemma_sum_ext_all(db.lineitem, m(1), |row: LineitemRow| if row.l_partkey == q && row.l_suppkey == s { dates(db, row.l_orderkey, y) * line_weight(row, 1) } else { 0int });
        lemma_sum_ext_all(db.lineitem, m(2), |row: LineitemRow| if row.l_partkey == q && row.l_suppkey == s { dates(db, row.l_orderkey, y) * line_weight(row, 2) } else { 0int });
    }
    assert(sum(db.lineitem, dpf) == pfdb(db, q, s, y, f));
    assert(ce * pfdb(db, q, s, y, f) == vdb(db, pat, nk, y, f, q, s)) by (nonlinear_arith)
        requires ce == supplier_count(db, s, nk) * green(filter_db(db, pat), q);
}


pub open spec fn pair_key(l: LineitemRow) -> (int, int) {
    (l.l_partkey, l.l_suppkey)
}

/// With the pairs of ks listed once and covering every line that adds anything, the totals are the pairs' sums.
pub proof fn lemma_tl_grouped(db: Db, pat: Seq<u8>, nk: int, y: int, f: int, ks: Seq<(int, int)>)
    requires 0 <= f <= 1, distinct(ks),
        forall|i: int| 0 <= i < db.lineitem.len() && hline(db, pat, #[trigger] db.lineitem[i], nk, y, f) != 0
            ==> ks.contains(pair_key(db.lineitem[i])),
    ensures tl(db, pat, nk, y, f) == sum(ks, |k: (int, int)| vdb(db, pat, nk, y, f, k.0, k.1)),
{
    let per = |k: (int, int)| vdb(db, pat, nk, y, f, k.0, k.1);
    let key = |x: LineitemRow| pair_key(x);
    let fh = hl(db, pat, nk, y, f);
    assert forall|k: (int, int)| #[trigger] per(k) == sum(db.lineitem, |x: LineitemRow| if key(x) == k { fh(x) } else { 0int }) by {
        lemma_pair_lines(db, pat, nk, y, f, k.0, k.1);
        lemma_sum_ext_all(db.lineitem, keyed(k.0, k.1, fh), |x: LineitemRow| if key(x) == k { fh(x) } else { 0int });
    }
    assert forall|i: int| 0 <= i < db.lineitem.len() && hl(db, pat, nk, y, f)(db.lineitem[i]) != 0 implies ks.contains(key(#[trigger] db.lineitem[i])) by {
        assert(hline(db, pat, db.lineitem[i], nk, y, f) != 0);
    }
    lemma_sum_group(db.lineitem, ks, key, hl(db, pat, nk, y, f), per);
    lemma_sum_ext_all(db.lineitem, hl(db, pat, nk, y, f), |l: LineitemRow| hline(db, pat, l, nk, y, f));
}

/// Cells with distinct keys: a cell's entry is its key's total.
pub proof fn lemma_cell_value(rows: Seq<Cell>, i: int, f: int)
    requires pcell_keys(rows).no_duplicates(), 0 <= i < rows.len(), 0 <= f <= 2,
    ensures pcell_at(rows, rows[i].k2 as int, rows[i].k3 as int, f) == if f == 0 { rows[i].a@ } else if f == 1 { rows[i].b@ } else { rows[i].c@ },
{
    let w = pcell_w(rows[i].k2 as int, rows[i].k3 as int, f);
    assert forall|j: int| 0 <= j < rows.len() && j != i implies w(#[trigger] rows[j]) == 0 by {
        assert(pcell_keys(rows)[j] != pcell_keys(rows)[i]);
    }
    lemma_sum_single_match(rows, w, i);
}

pub open spec fn year_cells(a: Seq<Cell>, y: int) -> Seq<Cell> {
    a.filter(|e: Cell| e.k3 == y)
}

pub open spec fn pairs_of(g: General, q: i64, y: int) -> Seq<(int, int)> {
    year_cells(bucket_seq(g.agg, q), y).map_values(|e: Cell| (q as int, e.k2 as int))
}

pub open spec fn ks_of(g: General, keys: Seq<i64>, y: int) -> Seq<(int, int)> {
    flat(keys.map_values(|q: i64| pairs_of(g, q, y)))
}

pub proof fn lemma_bucket_at(m: PMap, q: i64, k2: int, k3: int, f: int)
    ensures pcell_at(bucket_seq(m, q), k2, k3, f) == pm_at(m, q, k2, k3, f),
{
    if m.at(q).is_none() { lemma_sum_empty(Seq::<Cell>::empty(), pcell_w(k2, k3, f)); }
}

/// One PartAgg cell of the part q adds what its (part, supplier) pair adds.
pub proof fn lemma_term_value(g: General, db: Db, pat: Seq<u8>, q: i64, i: int, nk: int, y: int, f: int)
    requires general_of(g, db), 0 <= f <= 1, 0 <= i < bucket_seq(g.agg, q).len(), bucket_seq(g.agg, q)[i].k3 == y,
    ensures cell_term(g.sup, bucket_seq(g.pairs, q), green_q(g, pat, q), nk, y, f)(bucket_seq(g.agg, q)[i])
        == vdb(db, pat, nk, y, f, q as int, bucket_seq(g.agg, q)[i].k2 as int),
{
    let a = bucket_seq(g.agg, q);
    let p = bucket_seq(g.pairs, q);
    let e = a[i];
    let s = e.k2 as int;
    assert(pm_unique(g.agg));
    if g.agg.at(q).is_some() { assert(pcell_keys(g.agg.at(q).unwrap()@).no_duplicates()); }
    lemma_cell_value(a, i, 0);
    lemma_cell_value(a, i, 1);
    lemma_cell_value(a, i, 2);
    lemma_bucket_at(g.agg, q, e.k2 as int, y, 0);
    lemma_bucket_at(g.agg, q, e.k2 as int, y, 1);
    lemma_bucket_at(g.agg, q, e.k2 as int, y, 2);
    lemma_bucket_at(g.pairs, q, s, 0, 0);
    lemma_bucket_at(g.pairs, q, s, 0, 1);
    assert(pm_at(g.agg, q, s, y, 0) == moments(db, q as int, s, y, 0));
    assert(pm_at(g.agg, q, s, y, 1) == moments(db, q as int, s, y, 1));
    assert(pm_at(g.agg, q, s, y, 2) == moments(db, q as int, s, y, 2));
    assert(pm_at(g.pairs, q, s, 0, 0) == pair_count(db, q as int, s));
    assert(pm_at(g.pairs, q, s, 0, 1) == pair_cost(db, q as int, s));
    assert(pm_at(g.sup, e.k2, nk, 0, 0) == supplier_count(db, s, nk));
    let dbp = filter_db(db, pat);
    lemma_matched_green(db, pat, q as int);
    lemma_sum_ext_all(db.part, part_w(q as int, like_ind(pat)), matched_weight(pat, q as int));
    assert(green_q(g, pat, q) == green(dbp, q as int));
    let c = supplier_count(db, s, nk);
    let gr = green(dbp, q as int);
    let pf = pf_cell(p, e, f);
    assert(pf == pfdb(db, q as int, s, y, f));
    assert(c * (gr * pf) == c * gr * pf) by (nonlinear_arith);
}


#[verifier::spinoff_prover]
pub proof fn lemma_contrib_pairs(g: General, db: Db, pat: Seq<u8>, q: i64, nk: int, y: int, f: int)
    requires general_of(g, db), 0 <= f <= 1,
    ensures contrib(g, pat, q, nk, y, f) == sum(pairs_of(g, q, y), |k: (int, int)| vdb(db, pat, nk, y, f, k.0, k.1)),
{
    let a = bucket_seq(g.agg, q);
    let vfun = |k: (int, int)| vdb(db, pat, nk, y, f, k.0, k.1);
    let p = |e: Cell| e.k3 == y;
    let m = |e: Cell| (q as int, e.k2 as int);
    let h = |e: Cell| vdb(db, pat, nk, y, f, q as int, e.k2 as int);
    let gz = |e: Cell| if e.k3 == y { vdb(db, pat, nk, y, f, q as int, e.k2 as int) } else { 0int };
    assert forall|e: Cell| #[trigger] vfun(m(e)) == h(e) by {}
    lemma_sum_map(a.filter(p), m, vfun, h);
    assert forall|e: Cell| #[trigger] gz(e) == if p(e) { h(e) } else { 0int } by {}
    lemma_sum_filter(a, p, h, gz);
    let term = cell_term(g.sup, bucket_seq(g.pairs, q), green_q(g, pat, q), nk, y, f);
    assert forall|i: int| 0 <= i < a.len() implies term(#[trigger] a[i]) == gz(a[i]) by {
        if a[i].k3 == y { lemma_term_value(g, db, pat, q, i, nk, y, f); }
    }
    lemma_sum_ext(a, term, gz);
}

pub proof fn lemma_ks_distinct(g: General, keys: Seq<i64>, y: int)
    requires keys.no_duplicates(), pm_unique(g.agg),
    ensures distinct(ks_of(g, keys, y)),
{
    let pages = keys.map_values(|q: i64| pairs_of(g, q, y));
    assert forall|pos: int| 0 <= pos < pages.len() implies (#[trigger] pages[pos]).no_duplicates() by {
        let q = keys[pos];
        let a = bucket_seq(g.agg, q);
        if g.agg.at(q).is_some() { assert(pcell_keys(g.agg.at(q).unwrap()@).no_duplicates()); }
        let fl = year_cells(a, y);
        lemma_year_cells(a, y);
        assert forall|l: int, r: int| 0 <= l < r < pages[pos].len() implies (#[trigger] pages[pos][l]) != (#[trigger] pages[pos][r]) by {
            assert(fl[l].k2 != fl[r].k2);
        }
    }
    assert forall|l: int, r: int, v: (int, int)| #![trigger pages[l].contains(v), pages[r].contains(v)]
        0 <= l < r < pages.len() implies !(pages[l].contains(v) && pages[r].contains(v)) by {
        if pages[l].contains(v) && pages[r].contains(v) {
            let il = choose|i: int| 0 <= i < pages[l].len() && pages[l][i] == v;
            let ir = choose|i: int| 0 <= i < pages[r].len() && pages[r][i] == v;
            assert(v.0 == keys[l] as int && v.0 == keys[r] as int);
            assert(keys[l] != keys[r]);
        }
    }
    verus_prooflib::lemma_flat_unique(pages);
    let ks = ks_of(g, keys, y);
    assert forall|i: int, j: int| 0 <= i < ks.len() && 0 <= j < ks.len() && i != j implies ks[i] != ks[j] by {
        if i < j { assert(ks[i] != ks[j]); } else { assert(ks[j] != ks[i]); }
    }
}

/// The cells of one year: members of the bucket, with distinct suppliers when the cell keys are distinct.
proof fn lemma_year_cells(a: Seq<Cell>, y: int)
    requires pcell_keys(a).no_duplicates(),
    ensures
        forall|j: int| 0 <= j < year_cells(a, y).len() ==> (#[trigger] year_cells(a, y)[j]).k3 == y,
        forall|l: int, r: int| 0 <= l < r < year_cells(a, y).len() ==> year_cells(a, y)[l].k2 != year_cells(a, y)[r].k2,
    decreases a.len(),
{
    reveal(Seq::filter);
    let p = |e: Cell| e.k3 == y;
    if a.len() > 0 {
        let pre = a.drop_last();
        assert(pcell_keys(pre) =~= pcell_keys(a).drop_last());
        lemma_year_cells(pre, y);
        let sub = pre.filter(p);
        if p(a.last()) {
            let fl = sub.push(a.last());
            assert(a.filter(p) == fl);
            assert forall|l: int, r: int| 0 <= l < r < fl.len() implies fl[l].k2 != fl[r].k2 by {
                if r == fl.len() - 1 {
                    // an earlier cell of the year with the same supplier would repeat the last cell's key
                    assert(sub[l].k3 == y);
                    lemma_member_of_prefix(pre, y, l);
                    let i = choose|i: int| 0 <= i < pre.len() && pre[i] == sub[l];
                    assert(pcell_keys(a)[i] == (sub[l].k2, sub[l].k3));
                    assert(pcell_keys(a)[a.len() - 1] == (a.last().k2, a.last().k3));
                    if sub[l].k2 == a.last().k2 { assert(pcell_keys(a)[i] == pcell_keys(a)[a.len() - 1]); }
                } else {
                    assert(fl[l] == sub[l] && fl[r] == sub[r]);
                }
            }
        }
    }
}

proof fn lemma_member_of_prefix(a: Seq<Cell>, y: int, j: int)
    requires 0 <= j < year_cells(a, y).len(),
    ensures exists|i: int| 0 <= i < a.len() && a[i] == year_cells(a, y)[j],
    decreases a.len(),
{
    reveal(Seq::filter);
    let p = |e: Cell| e.k3 == y;
    let pre = a.drop_last();
    let sub = pre.filter(p);
    if p(a.last()) {
        if j == sub.len() { assert(a[a.len() - 1] == a.filter(p)[j]); }
        else { lemma_member_of_prefix(pre, y, j); let i = choose|i: int| 0 <= i < pre.len() && pre[i] == sub[j]; assert(a[i] == pre[i]); }
    } else {
        lemma_member_of_prefix(pre, y, j);
        let i = choose|i: int| 0 <= i < pre.len() && pre[i] == sub[j];
        assert(a[i] == pre[i]);
    }
}


pub open spec fn lines_fit(db: Db) -> bool {
    forall|i: int| 0 <= i < db.lineitem.len() ==> {
        let l = #[trigger] db.lineitem[i];
        i64::MIN <= l.l_partkey <= i64::MAX && i64::MIN <= l.l_suppkey <= i64::MAX
    }
}

pub proof fn lemma_dates_nonneg(db: Db, k: int, y: int)
    ensures dates(db, k, y) >= 0,
{
    lemma_sum_nonneg(db.orders, |row: OrdersRow| if row.o_orderkey == k && civil_year(row.o_orderdate) == y { 1int } else { 0int });
}

pub proof fn lemma_green_state(g: General, db: Db, pat: Seq<u8>, q: i64)
    requires general_of(g, db),
    ensures green_q(g, pat, q) == green(filter_db(db, pat), q as int),
{
    lemma_matched_green(db, pat, q as int);
    lemma_sum_ext_all(db.part, part_w(q as int, like_ind(pat)), matched_weight(pat, q as int));
}

/// Lines that add anything are listed in ks_of: their part has matching rows and their pair has year cells.
#[verifier::spinoff_prover]
pub proof fn lemma_coverage(g: General, db: Db, pat: Seq<u8>, keys: Seq<i64>, nk: int, y: int, f: int)
    requires general_of(g, db), lines_fit(db), 0 <= f <= 1,
        forall|k: i64| #[trigger] g.pnames.at(k).is_some() <==> keys.contains(k),
    ensures forall|i: int| 0 <= i < db.lineitem.len() && hline(db, pat, #[trigger] db.lineitem[i], nk, y, f) != 0
        ==> ks_of(g, keys, y).contains(pair_key(db.lineitem[i])),
{
    assert forall|i: int| 0 <= i < db.lineitem.len() && hline(db, pat, #[trigger] db.lineitem[i], nk, y, f) != 0
        implies ks_of(g, keys, y).contains(pair_key(db.lineitem[i])) by {
        let l = db.lineitem[i];
        let q = l.l_partkey as i64;
        let s = l.l_suppkey as i64;
        assert(q as int == l.l_partkey && s as int == l.l_suppkey);
        let d = dates(db, l.l_orderkey, y);
        let gr = green(filter_db(db, pat), l.l_partkey);
        let rest = supplier_count(db, l.l_suppkey, nk) * gr * pf(db, l, f);
        assert(d != 0 && rest != 0) by { if d == 0 { assert(d * rest == 0) by (nonlinear_arith) requires d == 0; } if rest == 0 { assert(d * rest == 0) by (nonlinear_arith) requires rest == 0; } }
        assert(gr != 0) by { if gr == 0 { assert(supplier_count(db, l.l_suppkey, nk) * gr * pf(db, l, f) == 0) by (nonlinear_arith) requires gr == 0; } }
        lemma_green_state(g, db, pat, q);
        assert(pn_at(g.pnames, q, like_ind(pat)) != 0);
        assert(g.pnames.at(q).is_some());
        assert(keys.contains(q));
        lemma_dates_nonneg(db, l.l_orderkey, y);
        // the moment of the pair counts this line
        let w = |x: LineitemRow| if x.l_partkey == l.l_partkey && x.l_suppkey == l.l_suppkey { dates(db, x.l_orderkey, y) * line_weight(x, 0) } else { 0int };
        assert forall|j: int| 0 <= j < db.lineitem.len() implies w(#[trigger] db.lineitem[j]) >= 0 by {
            lemma_dates_nonneg(db, db.lineitem[j].l_orderkey, y);
        }
        lemma_sum_ge_term(db.lineitem, w, i);
        assert(moments(db, l.l_partkey, l.l_suppkey, y, 0) >= d);
        assert(pm_at(g.agg, q, s as int, y, 0) == moments(db, q as int, s as int, y, 0));
        let a = bucket_seq(g.agg, q);
        lemma_bucket_at(g.agg, q, s as int, y, 0);
        assert(pcell_at(a, s as int, y, 0) != 0);
        let j = lemma_sum_nonzero_witness(a, pcell_w(s as int, y, 0));
        let e = a[j];
        assert(e.k2 == s && e.k3 == y);
        lemma_year_member(a, y, j);
        let t = choose|t: int| 0 <= t < year_cells(a, y).len() && year_cells(a, y)[t] == e;
        let ps = pairs_of(g, q, y);
        assert(ps[t] == (q as int, s as int));
        assert(ps.contains((q as int, s as int)));
        let pages = keys.map_values(|q2: i64| pairs_of(g, q2, y));
        let pos = choose|pos: int| 0 <= pos < keys.len() && keys[pos] == q;
        assert(pages[pos] == ps);
        lemma_flat_contains(pages, (q as int, s as int));
    }
}

/// A cell of the bucket with the year is among the year's cells.
proof fn lemma_year_member(a: Seq<Cell>, y: int, j: int)
    requires 0 <= j < a.len(), a[j].k3 == y,
    ensures exists|t: int| 0 <= t < year_cells(a, y).len() && year_cells(a, y)[t] == a[j],
{
    broadcast use vstd::seq_lib::group_filter_ensures;
    assert(year_cells(a, y).contains(a[j]));
}


pub proof fn lemma_ks_sum(g: General, keys: Seq<i64>, y: int, vfun: spec_fn((int, int)) -> int)
    ensures sum(ks_of(g, keys, y), vfun) == sum(keys, |q: i64| sum(pairs_of(g, q, y), vfun)),
{
    let gp = |q: i64| pairs_of(g, q, y);
    lemma_sum_flat_map(keys, gp, vfun);
    assert(ks_of(g, keys, y) == flat(keys.map_values(gp)));
    lemma_sum_ext_all(keys, |q: i64| sum(gp(q), vfun), |q: i64| sum(pairs_of(g, q, y), vfun));
}

pub proof fn lemma_contrib_sum(g: General, db: Db, pat: Seq<u8>, keys: Seq<i64>, nk: int, y: int, f: int, vfun: spec_fn((int, int)) -> int)
    requires general_of(g, db), 0 <= f <= 1, vfun == (|k: (int, int)| vdb(db, pat, nk, y, f, k.0, k.1)),
    ensures sum(keys, |q: i64| contrib(g, pat, q, nk, y, f)) == sum(keys, |q: i64| sum(pairs_of(g, q, y), vfun)),
{
    assert forall|q: i64| #[trigger] contrib(g, pat, q, nk, y, f) == sum(pairs_of(g, q, y), vfun) by {
        lemma_contrib_pairs(g, db, pat, q, nk, y, f);
    }
    lemma_sum_ext_all(keys, |q: i64| contrib(g, pat, q, nk, y, f), |q: i64| sum(pairs_of(g, q, y), vfun));
}

/// The read's totals are the nation totals of the filtered database.
#[verifier::spinoff_prover]
pub proof fn lemma_read_is_nation_total(g: General, db: Db, pat: Seq<u8>, keys: Seq<i64>, nk: int, y: int, f: int)
    requires general_of(g, db), lines_fit(db), 0 <= f <= 1, keys.no_duplicates(),
        forall|k: i64| #[trigger] g.pnames.at(k).is_some() <==> keys.contains(k),
    ensures sum(keys, |q: i64| contrib(g, pat, q, nk, y, f)) == nation_total(filter_db(db, pat), nk, y, f),
{
    let vfun = |k: (int, int)| vdb(db, pat, nk, y, f, k.0, k.1);
    let ks = ks_of(g, keys, y);
    lemma_ks_distinct(g, keys, y);
    lemma_coverage(g, db, pat, keys, nk, y, f);
    lemma_tl_grouped(db, pat, nk, y, f, ks);
    lemma_ks_sum(g, keys, y, vfun);
    lemma_contrib_sum(g, db, pat, keys, nk, y, f, vfun);
    lemma_tl_bridge(db, pat, nk, y, f);
}

}
