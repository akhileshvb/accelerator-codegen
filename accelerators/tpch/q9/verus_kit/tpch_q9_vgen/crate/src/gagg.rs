// PartAgg: per part, (supplier, year) -> (lines weighted by the order's year count, sum of
// extendedprice*(1-discount), sum of quantity).  Updated in O(1) per lineitem delta.
use vstd::prelude::*;
use verus_common::sums::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::meaning::*;
use crate::storage::*;
use verus_prooflib::wide::Wide;
use crate::pmap::*;
use crate::bagsig::signed;

verus! {

broadcast use vstd::seq_lib::group_seq_properties;

pub open spec fn agg_state(m: PMap, db: Db) -> bool {
    m.valid() && pm_unique(m) && forall|k: i64, s: int, y: int, f: int| 0 <= f <= 2 ==> #[trigger] pm_at(m, k, s, y, f) == moments(db, k as int, s, y, f)
}

pub open spec fn lwf(rev: int, qty: int, f: int) -> int {
    if f == 0 { 1int } else if f == 1 { rev } else { qty }
}

/// agg[(pk; sk, year)] += w * (the year counts in cells) * (1, rev, qty).
fn add_year_weights(abase: &PMap, aov: &mut Ov, cells: &Vec<Cell>, pk: i64, sk: i64, w: &Wide, rev: &Wide, qty: &Wide)
    requires abase.valid(), old(aov).valid(), pm_unique(*abase), ov_unique(*abase, *old(aov)),
    ensures final(aov).valid(), ov_unique(*abase, *final(aov)),
        forall|j1: i64, j2: int, j3: int, f: int| 0 <= f <= 2 ==> #[trigger] ov_at(*abase, *final(aov), j1, j2, j3, f)
            == ov_at(*abase, *old(aov), j1, j2, j3, f) + if j1 == pk && j2 == sk as int { w@ * pcell_at(cells@, j3, 0, 0) * lwf(rev@, qty@, f) } else { 0 },
{
    let ghost start = *aov;
    let mut i = 0;
    proof {
        assert forall|j1: i64, j2: int, j3: int, f: int| 0 <= f <= 2 implies #[trigger] ov_at(*abase, *aov, j1, j2, j3, f)
            == ov_at(*abase, start, j1, j2, j3, f) + if j1 == pk && j2 == sk as int { w@ * pcell_at(cells@.take(0), j3, 0, 0) * lwf(rev@, qty@, f) } else { 0 } by {
            lemma_sum_empty(cells@.take(0), pcell_w(j3, 0, 0));
            assert(w@ * 0int * lwf(rev@, qty@, f) == 0) by (nonlinear_arith);
        }
    }
    while i < cells.len()
        invariant i <= cells.len(), abase.valid(), aov.valid(), pm_unique(*abase), ov_unique(*abase, *aov), start == *old(aov),
            forall|j1: i64, j2: int, j3: int, f: int| 0 <= f <= 2 ==> #[trigger] ov_at(*abase, *aov, j1, j2, j3, f)
                == ov_at(*abase, start, j1, j2, j3, f) + if j1 == pk && j2 == sk as int { w@ * pcell_at(cells@.take(i as int), j3, 0, 0) * lwf(rev@, qty@, f) } else { 0 },
        decreases cells.len() - i,
    {
        if cells[i].k3 == 0 {
            let wt = w.mul(&cells[i].a);
            let dr = wt.mul(rev);
            let dq = wt.mul(qty);
            ov_add(abase, aov, pk, sk, cells[i].k2, &wt, &dr, &dq);
        }
        proof {
            assert forall|j1: i64, j2: int, j3: int, f: int| 0 <= f <= 2 implies #[trigger] ov_at(*abase, *aov, j1, j2, j3, f)
                == ov_at(*abase, start, j1, j2, j3, f) + if j1 == pk && j2 == sk as int { w@ * pcell_at(cells@.take(i as int + 1), j3, 0, 0) * lwf(rev@, qty@, f) } else { 0 } by {
                lemma_sum_take(cells@, i as int, pcell_w(j3, 0, 0));
                let prev = pcell_at(cells@.take(i as int), j3, 0, 0);
                let c = cells@[i as int];
                let l = lwf(rev@, qty@, f);
                if j1 == pk && j2 == sk as int && j3 == c.k2 as int && c.k3 == 0 {
                    assert(w@ * (prev + c.a@) * l == w@ * prev * l + (w@ * c.a@) * l) by (nonlinear_arith);
                } else if j1 == pk && j2 == sk as int {
                    assert(pcell_w(j3, 0, 0)(c) == 0);
                    assert(w@ * (prev + 0) * l == w@ * prev * l);
                }
            }
        }
        i += 1;
    }
    proof { assert(cells@.take(cells.len() as int) =~= cells@); }
}

pub open spec fn lw_of(row: LineIn, f: int) -> int {
    line_weight(line_in_view(row), f)
}

fn rev_qty(row: &LineIn) -> (r: (Wide, Wide))
    ensures r.0@ == lw_of(*row, 1), r.1@ == lw_of(*row, 2),
{
    let rev = Wide::from_i128(row.l_extendedprice as i128).mul(&Wide::from_i128(100i128 - row.l_discount as i128));
    let qty = Wide::from_i128(row.l_quantity as i128);
    (rev, qty)
}

/// The dates bucket of an order in a base (empty when absent).
fn dates_bucket(dbase: &PMap, ok: i64) -> (r: Vec<Cell>)
    requires dbase.valid(),
    ensures forall|y: int| #[trigger] pcell_at(r@, y, 0, 0) == pm_at(*dbase, ok, y, 0, 0),
{
    match dbase.get(ok) {
        Some(rows) => copy_cells(&rows),
        None => {
            let r: Vec<Cell> = Vec::new();
            proof { assert forall|y: int| #[trigger] pcell_at(r@, y, 0, 0) == pm_at(*dbase, ok, y, 0, 0) by { lemma_sum_empty(r@, pcell_w(y, 0, 0)); } }
            r
        }
    }
}

pub open spec fn line_old_term(dbase: PMap, pk: int, s: int, y: int, f: int) -> spec_fn(LineDelta) -> int {
    |ld: LineDelta| if ld.row.l_partkey == pk && ld.row.l_suppkey == s {
        ld.sign as int * pm_at(dbase, ld.row.l_orderkey, y, 0, 0) * lw_of(ld.row, f)
    } else { 0int }
}

/// agg += each delta line, weighted by the order's year counts in the base (before the transaction).
pub fn agg_lines(dbase: &PMap, abase: &PMap, aov: &mut Ov, chunk: &[LineDelta])
    requires dbase.valid(), abase.valid(), old(aov).valid(), pm_unique(*abase), ov_unique(*abase, *old(aov)),
    ensures final(aov).valid(), ov_unique(*abase, *final(aov)),
        forall|k: i64, s: int, y: int, f: int| 0 <= f <= 2 ==> #[trigger] ov_at(*abase, *final(aov), k, s, y, f)
            == ov_at(*abase, *old(aov), k, s, y, f) + sum(chunk@, line_old_term(*dbase, k as int, s, y, f)),
{
    let ghost start = *aov;
    let mut i = 0;
    proof {
        assert forall|k: i64, s: int, y: int, f: int| 0 <= f <= 2 implies #[trigger] ov_at(*abase, *aov, k, s, y, f) == ov_at(*abase, start, k, s, y, f) + sum(chunk@.take(0), line_old_term(*dbase, k as int, s, y, f)) by {
            lemma_sum_empty(chunk@.take(0), line_old_term(*dbase, k as int, s, y, f));
        }
    }
    while i < chunk.len()
        invariant i <= chunk.len(), dbase.valid(), abase.valid(), aov.valid(), pm_unique(*abase), ov_unique(*abase, *aov), start == *old(aov),
            forall|k: i64, s: int, y: int, f: int| 0 <= f <= 2 ==> #[trigger] ov_at(*abase, *aov, k, s, y, f) == ov_at(*abase, start, k, s, y, f) + sum(chunk@.take(i as int), line_old_term(*dbase, k as int, s, y, f)),
        decreases chunk.len() - i,
    {
        let ld = chunk[i];
        let cells = dates_bucket(dbase, ld.row.l_orderkey);
        let (rev, qty) = rev_qty(&ld.row);
        let sgn = Wide::from_i128(ld.sign as i128);
        add_year_weights(abase, aov, &cells, ld.row.l_partkey, ld.row.l_suppkey, &sgn, &rev, &qty);
        proof {
            assert forall|k: i64, s: int, y: int, f: int| 0 <= f <= 2 implies #[trigger] ov_at(*abase, *aov, k, s, y, f) == ov_at(*abase, start, k, s, y, f) + sum(chunk@.take(i as int + 1), line_old_term(*dbase, k as int, s, y, f)) by {
                lemma_sum_take(chunk@, i as int, line_old_term(*dbase, k as int, s, y, f));
                if k == ld.row.l_partkey && s == ld.row.l_suppkey as int {
                    assert(pcell_at(cells@, y, 0, 0) == pm_at(*dbase, ld.row.l_orderkey, y, 0, 0));
                    assert(lwf(rev@, qty@, f) == lw_of(ld.row, f));
                }
            }
        }
        i += 1;
    }
    proof { assert(chunk@.take(chunk@.len() as int) =~= chunk@); }
}

pub open spec fn line_move_term(dbase: PMap, dov: Ov, pk: int, s: int, y: int, f: int) -> spec_fn(LineIn) -> int {
    |r: LineIn| if r.l_partkey == pk && r.l_suppkey == s {
        (ov_at(dbase, dov, r.l_orderkey, y, 0, 0) - pm_at(dbase, r.l_orderkey, y, 0, 0)) * lw_of(r, f)
    } else { 0int }
}

/// agg += each rescanned line weighted by its order's change of year counts.
pub fn agg_rows(dbase: &PMap, dov: &Ov, abase: &PMap, aov: &mut Ov, rows: &[LineIn])
    requires dbase.valid(), abase.valid(), old(aov).valid(), pm_unique(*abase), ov_unique(*abase, *old(aov)),
    ensures final(aov).valid(), ov_unique(*abase, *final(aov)),
        forall|k: i64, s: int, y: int, f: int| 0 <= f <= 2 ==> #[trigger] ov_at(*abase, *final(aov), k, s, y, f)
            == ov_at(*abase, *old(aov), k, s, y, f) + sum(rows@, line_move_term(*dbase, *dov, k as int, s, y, f)),
{
    let ghost start = *aov;
    let mut i = 0;
    proof {
        assert forall|k: i64, s: int, y: int, f: int| 0 <= f <= 2 implies #[trigger] ov_at(*abase, *aov, k, s, y, f) == ov_at(*abase, start, k, s, y, f) + sum(rows@.take(0), line_move_term(*dbase, *dov, k as int, s, y, f)) by {
            lemma_sum_empty(rows@.take(0), line_move_term(*dbase, *dov, k as int, s, y, f));
        }
    }
    while i < rows.len()
        invariant i <= rows.len(), dbase.valid(), abase.valid(), aov.valid(), pm_unique(*abase), ov_unique(*abase, *aov), start == *old(aov),
            forall|k: i64, s: int, y: int, f: int| 0 <= f <= 2 ==> #[trigger] ov_at(*abase, *aov, k, s, y, f) == ov_at(*abase, start, k, s, y, f) + sum(rows@.take(i as int), line_move_term(*dbase, *dov, k as int, s, y, f)),
        decreases rows.len() - i,
    {
        let r = rows[i];
        let fresh = ov_bucket(dbase, dov, r.l_orderkey);
        let stale = dates_bucket(dbase, r.l_orderkey);
        let (rev, qty) = rev_qty(&r);
        let one = Wide::from_i128(1);
        let minus = Wide::from_i128(-1);
        let ghost mid;
        add_year_weights(abase, aov, &fresh, r.l_partkey, r.l_suppkey, &one, &rev, &qty);
        proof { mid = *aov; }
        add_year_weights(abase, aov, &stale, r.l_partkey, r.l_suppkey, &minus, &rev, &qty);
        proof {
            assert forall|k: i64, s: int, y: int, f: int| 0 <= f <= 2 implies #[trigger] ov_at(*abase, *aov, k, s, y, f) == ov_at(*abase, start, k, s, y, f) + sum(rows@.take(i as int + 1), line_move_term(*dbase, *dov, k as int, s, y, f)) by {
                lemma_sum_take(rows@, i as int, line_move_term(*dbase, *dov, k as int, s, y, f));
                if k == r.l_partkey && s == r.l_suppkey as int {
                    let nw = ov_at(*dbase, *dov, r.l_orderkey, y, 0, 0);
                    let od = pm_at(*dbase, r.l_orderkey, y, 0, 0);
                    assert(pcell_at(fresh@, y, 0, 0) == nw);
                    assert(pcell_at(stale@, y, 0, 0) == od);
                    let l = lwf(rev@, qty@, f);
                    assert(l == lw_of(r, f));
                    assert(1int * nw * l + (-1int) * od * l == (nw - od) * l) by (nonlinear_arith);
                }
            }
        }
        i += 1;
    }
    proof { assert(rows@.take(rows.len() as int) =~= rows@); }
}

}
