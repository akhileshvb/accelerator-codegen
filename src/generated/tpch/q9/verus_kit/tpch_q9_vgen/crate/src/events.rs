// A line's contribution to the rollups of the tracked patterns, in one database "world":
// the dimension pages and the order-year weights of that world.
use vstd::prelude::*;
use verus_common::sums::*;
use verus_common::sqlfns::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::meaning::*;
use crate::values::*;
use crate::rows::*;
use crate::storage::*;
use crate::dimensions::*;
use crate::rollup::*;

verus! {

broadcast use vstd::seq_lib::group_seq_properties;

pub open spec fn pat_views_of(pats: Seq<Vec<u8>>) -> Seq<Seq<u8>> {
    pats.map_values(|p: Vec<u8>| p@)
}

pub open spec fn page_mask_color(pages: Pages<Vec<MaskRow>>, key: i64, c: int) -> int {
    match pages.at(key) { Some(rows) => mask_color(rows@, c), None => 0int }
}

pub open spec fn masks_ready(pages: Pages<Vec<MaskRow>>, db: Db, pats: Seq<Seq<u8>>) -> bool {
    pages.valid()
        && (forall|key: i64, c: int| 0 <= c < pats.len() ==> #[trigger] page_mask_color(pages, key, c) == green(filter_db(db, pats[c]), key as int))
        && forall|key: i64| #[trigger] pages.at(key).is_some() ==> {
            let rows = pages.at(key).unwrap();
            rows.len() > 0 && masks_unique(rows@) && masks_positive(rows@)
        }
}

pub open spec fn page_pair(pages: Pages<Vec<Pair>>, key: i64, supplier: int, field: int) -> int {
    match pages.at(key) { Some(rows) => pair_at(rows@, supplier, field), None => 0int }
}

pub open spec fn pairs_ready(pages: Pages<Vec<Pair>>, db: Db) -> bool {
    pages.valid()
        && (forall|key: i64, supplier: int, field: int| 0 <= field <= 1 ==> #[trigger] page_pair(pages, key, supplier, field)
            == if field == 0 { pair_count(db, key as int, supplier) } else { pair_cost(db, key as int, supplier) })
        && forall|key: i64| #[trigger] pages.at(key).is_some() ==> {
            let rows = pages.at(key).unwrap();
            rows.len() > 0 && pairs_unique(rows@) && pairs_positive(rows@)
        }
}

pub open spec fn dval(delta: Seq<Vec<RCell>>, c: int, nk: int, year: int, field: int) -> int {
    if 0 <= c < delta.len() { cell_at(delta[c]@, nk, year, field) } else { 0int }
}

pub open spec fn ysum(ws: Seq<YearW>, year: int) -> int {
    sum(ws, |w: YearW| if w.year == year { w.weight as int } else { 0int })
}

pub open spec fn kc(kc0: i128, kc1: i128, field: int) -> int {
    if field == 0 { kc0 as int } else { kc1 as int }
}

/// Adds weight w of one year, over the nation rows of a supplier.
#[verifier::spinoff_prover]
fn add_year(v: &mut Vec<RCell>, srows: &Vec<Number>, year: i64, w: i128, kc0: i128, kc1: i128) -> (r: Option<()>)
    ensures r.is_some() ==> forall|nk: int, y: int, f: int| 0 <= f <= 1 ==> #[trigger] cell_at(final(v)@, nk, y, f)
        == cell_at(old(v)@, nk, y, f) + if y == year as int { w as int * number_at(srows@, nk) * kc(kc0, kc1, f) } else { 0 },
{
    let ghost base = v@;
    let mut k = 0;
    proof {
        assert forall|nk: int, y: int, f: int| 0 <= f <= 1 implies cell_at(v@, nk, y, f)
            == cell_at(base, nk, y, f) + if y == year as int { w as int * number_at(srows@.take(0), nk) * kc(kc0, kc1, f) } else { 0 } by {
            lemma_sum_empty(srows@.take(0), |row: Number| if row.key as int == nk { row.count as int } else { 0int });
            assert(w as int * 0int * kc(kc0, kc1, f) == 0) by (nonlinear_arith);
        }
    }
    while k < srows.len()
        invariant k <= srows.len(), base == old(v)@,
            forall|nk: int, y: int, f: int| 0 <= f <= 1 ==> #[trigger] cell_at(v@, nk, y, f)
                == cell_at(base, nk, y, f) + if y == year as int { w as int * number_at(srows@.take(k as int), nk) * kc(kc0, kc1, f) } else { 0 },
        decreases srows.len() - k,
    {
        let row = srows[k];
        let cnt = w.checked_mul(row.count)?;
        let dc = cnt.checked_mul(kc0)?;
        let dp = cnt.checked_mul(kc1)?;
        let ghost before = v@;
        bump_cell(v, row.key, year, dc, dp)?;
        proof {
            assert forall|nk: int, y: int, f: int| 0 <= f <= 1 implies #[trigger] cell_at(v@, nk, y, f)
                == cell_at(base, nk, y, f) + if y == year as int { w as int * number_at(srows@.take(k as int + 1), nk) * kc(kc0, kc1, f) } else { 0 } by {
                lemma_sum_take(srows@, k as int, |r: Number| if r.key as int == nk { r.count as int } else { 0int });
                let prev = number_at(srows@.take(k as int), nk);
                let kf = kc(kc0, kc1, f);
                if y == year as int && nk == row.key as int {
                    assert(cell_at(v@, nk, y, f) == cell_at(before, nk, y, f) + if f == 0 { dc as int } else { dp as int });
                    assert(w as int * (prev + row.count as int) * kf == w as int * prev * kf + (w as int * row.count as int) * kf) by (nonlinear_arith);
                } else if y == year as int {
                    assert(w as int * prev * kf == w as int * prev * kf);
                }
            }
        }
        k += 1;
    }
    proof { assert(srows@.take(srows.len() as int) =~= srows@); }
    Some(())
}

#[verifier::spinoff_prover]
fn add_color(v: &mut Vec<RCell>, srows: &Vec<Number>, ws: &Vec<YearW>, kc0: i128, kc1: i128) -> (r: Option<()>)
    ensures r.is_some() ==> forall|nk: int, y: int, f: int| 0 <= f <= 1 ==> #[trigger] cell_at(final(v)@, nk, y, f)
        == cell_at(old(v)@, nk, y, f) + ysum(ws@, y) * number_at(srows@, nk) * kc(kc0, kc1, f),
{
    let ghost base = v@;
    let mut j = 0;
    proof {
        assert forall|nk: int, y: int, f: int| 0 <= f <= 1 implies cell_at(v@, nk, y, f)
            == cell_at(base, nk, y, f) + ysum(ws@.take(0), y) * number_at(srows@, nk) * kc(kc0, kc1, f) by {
            lemma_sum_empty(ws@.take(0), |w: YearW| if w.year as int == y { w.weight as int } else { 0int });
            assert(0int * number_at(srows@, nk) * kc(kc0, kc1, f) == 0) by (nonlinear_arith);
        }
    }
    while j < ws.len()
        invariant j <= ws.len(), base == old(v)@,
            forall|nk: int, y: int, f: int| 0 <= f <= 1 ==> #[trigger] cell_at(v@, nk, y, f)
                == cell_at(base, nk, y, f) + ysum(ws@.take(j as int), y) * number_at(srows@, nk) * kc(kc0, kc1, f),
        decreases ws.len() - j,
    {
        let ghost before = v@;
        add_year(v, srows, ws[j].year, ws[j].weight, kc0, kc1)?;
        proof {
            assert forall|nk: int, y: int, f: int| 0 <= f <= 1 implies #[trigger] cell_at(v@, nk, y, f)
                == cell_at(base, nk, y, f) + ysum(ws@.take(j as int + 1), y) * number_at(srows@, nk) * kc(kc0, kc1, f) by {
                lemma_sum_take(ws@, j as int, |w: YearW| if w.year as int == y { w.weight as int } else { 0int });
                let prev = ysum(ws@.take(j as int), y);
                let n = number_at(srows@, nk);
                let kf = kc(kc0, kc1, f);
                if y == ws[j as int].year as int {
                    assert((prev + ws[j as int].weight as int) * n * kf == prev * n * kf + ws[j as int].weight as int * n * kf) by (nonlinear_arith);
                }
            }
        }
        j += 1;
    }
    proof { assert(ws@.take(ws.len() as int) =~= ws@); }
    Some(())
}

proof fn lemma_hline_zero_green(db: Db, p: Seq<u8>, l: LineitemRow, nk: int, y: int, f: int)
    requires green(filter_db(db, p), l.l_partkey) == 0,
    ensures hline(db, p, l, nk, y, f) == 0,
{
    let s = supplier_count(db, l.l_suppkey, nk);
    let q = pf(db, l, f);
    assert(dates(db, l.l_orderkey, y) * (s * 0int * q) == 0) by (nonlinear_arith);
}

proof fn lemma_hline_zero_pair(db: Db, p: Seq<u8>, l: LineitemRow, nk: int, y: int, f: int)
    requires pair_count(db, l.l_partkey, l.l_suppkey) == 0, pair_cost(db, l.l_partkey, l.l_suppkey) == 0, 0 <= f <= 1,
    ensures hline(db, p, l, nk, y, f) == 0,
{
    let s = supplier_count(db, l.l_suppkey, nk);
    let g = green(filter_db(db, p), l.l_partkey);
    assert(pf(db, l, f) == 0) by (nonlinear_arith) requires pair_count(db, l.l_partkey, l.l_suppkey) == 0, pair_cost(db, l.l_partkey, l.l_suppkey) == 0, pf(db, l, f) == if f == 0 { pair_count(db, l.l_partkey, l.l_suppkey) } else { pair_count(db, l.l_partkey, l.l_suppkey) * line_weight(l, 1) - pair_cost(db, l.l_partkey, l.l_suppkey) * l.l_quantity };
    assert(dates(db, l.l_orderkey, y) * (s * g * 0int) == 0) by (nonlinear_arith);
}

proof fn lemma_hline_zero_supplier(db: Db, p: Seq<u8>, l: LineitemRow, nk: int, y: int, f: int)
    requires supplier_count(db, l.l_suppkey, nk) == 0,
    ensures hline(db, p, l, nk, y, f) == 0,
{
    let g = green(filter_db(db, p), l.l_partkey);
    let q = pf(db, l, f);
    assert(dates(db, l.l_orderkey, y) * (0int * g * q) == 0) by (nonlinear_arith);
}

pub open spec fn line_post(db: Db, pats: Seq<Seq<u8>>, line: LineIn, scale: int, old_delta: Seq<Vec<RCell>>, new_delta: Seq<Vec<RCell>>) -> bool {
    new_delta.len() == old_delta.len()
        && forall|c: int, nk: int, y: int, f: int| 0 <= c < pats.len() && 0 <= f <= 1 ==> #[trigger] dval(new_delta, c, nk, y, f)
            == dval(old_delta, c, nk, y, f) + scale * hline(db, pats[c], line_in_view(line), nk, y, f)
}

fn find_pair(rows: &Vec<Pair>, supplier: i64) -> (r: (i128, i128))
    requires pairs_unique(rows@),
    ensures r.0 as int == pair_at(rows@, supplier as int, 0), r.1 as int == pair_at(rows@, supplier as int, 1),
{
    let mut pi = 0;
    while pi < rows.len() && rows[pi].supplier != supplier
        invariant pi <= rows.len(), forall|q: int| 0 <= q < pi ==> (#[trigger] rows@[q]).supplier != supplier,
        decreases rows.len() - pi,
    { pi += 1; }
    let ghost w0 = |row: Pair| if row.supplier == supplier as int { row.count as int } else { 0int };
    let ghost w1 = |row: Pair| if row.supplier == supplier as int { row.cost as int } else { 0int };
    if pi == rows.len() {
        proof {
            lemma_sum_zero(rows@, w0);
            lemma_sum_zero(rows@, w1);
            lemma_sum_ext_all(rows@, w0, |row: Pair| if row.supplier == supplier as int { if 0int == 0 { row.count as int } else { row.cost as int } } else { 0int });
            lemma_sum_ext_all(rows@, w1, |row: Pair| if row.supplier == supplier as int { if 1int == 0 { row.count as int } else { row.cost as int } } else { 0int });
        }
        return (0, 0);
    }
    proof {
        assert forall|q: int| 0 <= q < rows@.len() && q != pi as int implies w0(#[trigger] rows@[q]) == 0 by {
            assert(rows@.map_values(|row: Pair| row.supplier)[q] != rows@.map_values(|row: Pair| row.supplier)[pi as int]);
        }
        assert forall|q: int| 0 <= q < rows@.len() && q != pi as int implies w1(#[trigger] rows@[q]) == 0 by {
            assert(rows@.map_values(|row: Pair| row.supplier)[q] != rows@.map_values(|row: Pair| row.supplier)[pi as int]);
        }
        lemma_sum_single_match(rows@, w0, pi as int);
        lemma_sum_single_match(rows@, w1, pi as int);
        lemma_sum_ext_all(rows@, w0, |row: Pair| if row.supplier == supplier as int { if 0int == 0 { row.count as int } else { row.cost as int } } else { 0int });
        lemma_sum_ext_all(rows@, w1, |row: Pair| if row.supplier == supplier as int { if 1int == 0 { row.count as int } else { row.cost as int } } else { 0int });
    }
    (rows[pi].count, rows[pi].cost)
}

#[verifier::spinoff_prover]
fn add_colors(mrows: &Vec<MaskRow>, srows: &Vec<Number>, ws: &Vec<YearW>, n: usize, scale: i128, pc: i128, pprofit: i128,
    delta: &mut Vec<Vec<RCell>>, Ghost(db): Ghost<Db>, Ghost(pats): Ghost<Seq<Seq<u8>>>, Ghost(lv): Ghost<LineitemRow>) -> (r: Option<()>)
    requires n == pats.len(), n <= 127, old(delta).len() == n,
        forall|c: int| 0 <= c < pats.len() ==> #[trigger] mask_color(mrows@, c) == green(filter_db(db, pats[c]), lv.l_partkey),
        forall|nk: int| #[trigger] number_at(srows@, nk) == supplier_count(db, lv.l_suppkey, nk),
        pc as int == pf(db, lv, 0), pprofit as int == pf(db, lv, 1),
        forall|y: int| #[trigger] ysum(ws@, y) == dates(db, lv.l_orderkey, y),
    ensures r.is_some() ==> final(delta).len() == n && forall|c: int, nk: int, y: int, f: int| 0 <= c < pats.len() && 0 <= f <= 1 ==> #[trigger] dval(final(delta)@, c, nk, y, f)
        == dval(old(delta)@, c, nk, y, f) + scale as int * hline(db, pats[c], lv, nk, y, f),
{
    let ghost old_delta = delta@;
    let mut c: usize = 0;
    proof {
        assert forall|c2: int, nk: int, y: int, f: int| 0 <= c2 < pats.len() && 0 <= f <= 1 implies dval(delta@, c2, nk, y, f)
            == dval(old_delta, c2, nk, y, f) + if c2 < 0int { scale as int * hline(db, pats[c2], lv, nk, y, f) } else { 0int } by {}
    }
    while c < n
        invariant c <= n, n == pats.len(), n <= 127, delta.len() == n, old_delta.len() == n,
            forall|c2: int| 0 <= c2 < pats.len() ==> #[trigger] mask_color(mrows@, c2) == green(filter_db(db, pats[c2]), lv.l_partkey),
            forall|nk: int| #[trigger] number_at(srows@, nk) == supplier_count(db, lv.l_suppkey, nk),
            pc as int == pf(db, lv, 0), pprofit as int == pf(db, lv, 1),
            forall|y: int| #[trigger] ysum(ws@, y) == dates(db, lv.l_orderkey, y),
            forall|c2: int, nk: int, y: int, f: int| 0 <= c2 < pats.len() && 0 <= f <= 1 ==> #[trigger] dval(delta@, c2, nk, y, f)
                == dval(old_delta, c2, nk, y, f) + if c2 < c as int { scale as int * hline(db, pats[c2], lv, nk, y, f) } else { 0int },
        decreases n - c,
    {
        let g = mask_color_exec(mrows, c as u32)?;
        if g != 0 {
            let k0 = scale.checked_mul(g)?;
            let kc0 = k0.checked_mul(pc)?;
            let kc1 = k0.checked_mul(pprofit)?;
            let ghost before = delta@;
            let mut v = delta.remove(c);
            let ghost vbefore = v@;
            add_color(&mut v, srows, ws, kc0, kc1)?;
            delta.insert(c, v);
            proof {
                assert forall|c2: int, nk: int, y: int, f: int| 0 <= c2 < pats.len() && 0 <= f <= 1 implies #[trigger] dval(delta@, c2, nk, y, f)
                    == dval(old_delta, c2, nk, y, f) + if c2 < c as int + 1 { scale as int * hline(db, pats[c2], lv, nk, y, f) } else { 0int } by {
                    if c2 == c as int {
                        assert(dval(delta@, c2, nk, y, f) == cell_at(v@, nk, y, f));
                        assert(dval(before, c2, nk, y, f) == cell_at(vbefore, nk, y, f));
                        let d = ysum(ws@, y);
                        let s = number_at(srows@, nk);
                        let gg = g as int;
                        let sc = scale as int;
                        let q = pf(db, lv, f);
                        assert(g as int == green(filter_db(db, pats[c as int]), lv.l_partkey));
                        assert(k0 as int == sc * gg);
                        assert(kc(kc0, kc1, f) == sc * gg * q);
                        assert(d * s * (sc * gg * q) == sc * (d * (s * gg * q))) by (nonlinear_arith);
                    } else {
                        assert(dval(delta@, c2, nk, y, f) == dval(before, c2, nk, y, f));
                    }
                }
            }
        } else {
            proof {
                assert forall|c2: int, nk: int, y: int, f: int| 0 <= c2 < pats.len() && 0 <= f <= 1 implies #[trigger] dval(delta@, c2, nk, y, f)
                    == dval(old_delta, c2, nk, y, f) + if c2 < c as int + 1 { scale as int * hline(db, pats[c2], lv, nk, y, f) } else { 0int } by {
                    if c2 == c as int {
                        assert(g as int == green(filter_db(db, pats[c as int]), lv.l_partkey));
                        lemma_hline_zero_green(db, pats[c2], lv, nk, y, f);
                        assert(scale as int * 0int == 0) by (nonlinear_arith);
                    }
                }
            }
        }
        c += 1;
    }
    Some(())
}

/// The rollup change of one line (times scale) in the world of `db`.
#[verifier::spinoff_prover]
pub fn line_world(masks: &Pages<Vec<MaskRow>>, pairs: &Pages<Vec<Pair>>, sups: &Pages<Vec<Number>>, n: usize, line: LineIn, ws: &Vec<YearW>, scale: i128,
    delta: &mut Vec<Vec<RCell>>, Ghost(db): Ghost<Db>, Ghost(pats): Ghost<Seq<Seq<u8>>>) -> (r: Option<()>)
    requires masks_ready(*masks, db, pats), pairs_ready(*pairs, db), supplier_pages_ready(*sups, db),
        n == pats.len(), n <= 127, old(delta).len() == n,
        forall|y: int| #[trigger] ysum(ws@, y) == dates(db, line.l_orderkey as int, y),
    ensures r.is_some() ==> line_post(db, pats, line, scale as int, old(delta)@, final(delta)@),
{
    let ghost lv = line_in_view(line);
    let ghost old_delta = delta@;
    let mrec = masks.get(line.l_partkey);
    let prec = pairs.get(line.l_partkey);
    let srec = sups.get(line.l_suppkey);
    if mrec.is_none() || prec.is_none() || srec.is_none() {
        proof {
            assert forall|c: int, nk: int, y: int, f: int| 0 <= c < pats.len() && 0 <= f <= 1 implies #[trigger] dval(delta@, c, nk, y, f)
                == dval(old_delta, c, nk, y, f) + scale as int * hline(db, pats[c], lv, nk, y, f) by {
                if mrec.is_none() {
                    assert(page_mask_color(*masks, line.l_partkey, c) == green(filter_db(db, pats[c]), line.l_partkey as int));
                    lemma_hline_zero_green(db, pats[c], lv, nk, y, f);
                } else if prec.is_none() {
                    assert(page_pair(*pairs, line.l_partkey, line.l_suppkey as int, 0) == pair_count(db, line.l_partkey as int, line.l_suppkey as int));
                    assert(page_pair(*pairs, line.l_partkey, line.l_suppkey as int, 1) == pair_cost(db, line.l_partkey as int, line.l_suppkey as int));
                    lemma_hline_zero_pair(db, pats[c], lv, nk, y, f);
                } else {
                    assert(number_page(*sups, line.l_suppkey, nk) == supplier_count(db, line.l_suppkey as int, nk));
                    lemma_hline_zero_supplier(db, pats[c], lv, nk, y, f);
                }
                assert(scale as int * 0int == 0) by (nonlinear_arith);
            }
        }
        return Some(());
    }
    let mrows = mrec.unwrap();
    let prows = prec.unwrap();
    let srows = srec.unwrap();
    let (pc, pcost) = find_pair(&prows, line.l_suppkey);
    let rev: i128 = (line.l_extendedprice as i128).checked_mul(100i128 - line.l_discount as i128)?;
    let qty: i128 = line.l_quantity as i128;
    let a = pc.checked_mul(rev)?;
    let b = pcost.checked_mul(qty)?;
    let pprofit = a.checked_sub(b)?;
    proof {
        assert(page_pair(*pairs, line.l_partkey, line.l_suppkey as int, 0) == pair_count(db, line.l_partkey as int, line.l_suppkey as int));
        assert(page_pair(*pairs, line.l_partkey, line.l_suppkey as int, 1) == pair_cost(db, line.l_partkey as int, line.l_suppkey as int));
        assert(pc as int == pair_count(db, lv.l_partkey, lv.l_suppkey));
        assert(pcost as int == pair_cost(db, lv.l_partkey, lv.l_suppkey));
        assert(rev as int == line_weight(lv, 1));
        assert(pprofit as int == pf(db, lv, 1));
        assert(pc as int == pf(db, lv, 0));
        assert forall|c: int| 0 <= c < pats.len() implies #[trigger] mask_color(mrows@, c) == green(filter_db(db, pats[c]), lv.l_partkey) by {
            assert(page_mask_color(*masks, line.l_partkey, c) == green(filter_db(db, pats[c]), line.l_partkey as int));
        }
        assert forall|nk: int| #[trigger] number_at(srows@, nk) == supplier_count(db, lv.l_suppkey, nk) by {
            assert(number_page(*sups, line.l_suppkey, nk) == supplier_count(db, line.l_suppkey as int, nk));
        }
    }
    add_colors(&mrows, &srows, ws, n, scale, pc, pprofit, delta, Ghost(db), Ghost(pats), Ghost(lv))?;
    Some(())
}

}
