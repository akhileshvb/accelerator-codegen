// Build: order years of the scan and the rollups of the tracked patterns.
use vstd::prelude::*;
use std::sync::Arc;
use verus_common::sums::*;
use verus_common::sqlfns::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::meaning::*;
use crate::storage::*;
use crate::values::*;
use crate::rows::*;
use crate::dimensions::*;
use crate::events::*;
use crate::orders::*;
use crate::rollup::*;
use crate::buildparts::*;

verus! {

broadcast use vstd::seq_lib::group_seq_properties;

/// The order years of the database: copies of each (order, year).
#[verifier::spinoff_prover]
pub fn build_dates(rows: &[OrderIn], Ghost(db): Ghost<Db>) -> (w: Work<Vec<Year>>)
    requires scan_all_orders_ok(db, rows@),
    ensures w.valid(),
        forall|key: i64, year: int| #[trigger] post_year(w, key, year) == dates(db, key as int, year),
        forall|key: i64| #[trigger] w.rows@.contains_key(key) ==> years_positive(w.rows@[key]@) && w.rows@[key]@.map_values(|row: Year| row.year).no_duplicates(),
{
    let mut keys: Vec<i64> = Vec::new();
    let mut index = 0;
    while index < rows.len()
        invariant index <= rows.len(),
        decreases rows.len() - index,
    { keys.push(rows[index].o_orderkey); index += 1; }
    let w = post_dates(&keys, rows);
    proof {
        assert forall|key: i64, year: int| #[trigger] post_year(w, key, year) == dates(db, key as int, year) by {
            let weight = |row: OrdersRow| if row.o_orderkey == key as int && civil_year(row.o_orderdate) == year { 1int } else { 0int };
            assert(sum(order_in_seq_view(rows@), weight) == sum(db.orders, weight));
            lemma_sum_map(rows@, |row: OrderIn| order_in_view(row), weight, order_input_weight(key, year));
        }
    }
    w
}

proof fn lemma_ysum_push(ws: Seq<YearW>, w: YearW, y: int)
    ensures ysum(ws.push(w), y) == ysum(ws, y) + if w.year as int == y { w.weight as int } else { 0int },
{
    lemma_sum_push(ws, w, |x: YearW| if x.year as int == y { x.weight as int } else { 0int });
}

proof fn lemma_year_at_take(years: Seq<Year>, i: int, y: int)
    requires 0 <= i < years.len(),
    ensures year_at(years.take(i + 1), y) == year_at(years.take(i), y) + if years[i].year as int == y { years[i].count as int } else { 0int },
{
    lemma_sum_take(years, i, |r: Year| if r.year as int == y { r.count as int } else { 0int });
}

/// Year weights of a list of (year, count) rows.
#[verifier::spinoff_prover]
pub fn years_to_ws(years: &Vec<Year>) -> (ws: Vec<YearW>)
    ensures forall|y: int| #[trigger] ysum(ws@, y) == year_at(years@, y),
{
    let mut ws: Vec<YearW> = Vec::new();
    let mut index = 0;
    proof {
        assert forall|y: int| #[trigger] ysum(ws@, y) == year_at(years@.take(0), y) by {
            lemma_sum_empty(ws@, |w: YearW| if w.year as int == y { w.weight as int } else { 0int });
            lemma_sum_empty(years@.take(0), |r: Year| if r.year as int == y { r.count as int } else { 0int });
        }
    }
    while index < years.len()
        invariant index <= years.len(),
            forall|y: int| #[trigger] ysum(ws@, y) == year_at(years@.take(index as int), y),
        decreases years.len() - index,
    {
        let ghost before = ws@;
        assert forall|y: int| #[trigger] ysum(before, y) == year_at(years@.take(index as int), y) by {}
        ws.push(YearW { year: years[index].year, weight: years[index].count });
        proof {
            assert forall|y: int| #[trigger] ysum(ws@, y) == year_at(years@.take(index as int + 1), y) by {
                assert(ws@ =~= before.push(YearW { year: years[index as int].year, weight: years[index as int].count }));
                lemma_ysum_push(before, YearW { year: years[index as int].year, weight: years[index as int].count }, y);
                lemma_year_at_take(years@, index as int, y);
            }
        }
        index += 1;
    }
    proof { assert(years@.take(index as int) =~= years@); }
    ws
}

pub open spec fn lines_weight(db: Db, p: Seq<u8>, nk: int, year: int, field: int) -> spec_fn(LineIn) -> int {
    |r: LineIn| hline(db, p, line_in_view(r), nk, year, field)
}

/// The rollups of the tracked patterns over all lines.
#[verifier::spinoff_prover]
pub fn build_rollup(all_lines: &[LineIn], dates_w: &Work<Vec<Year>>, masks: &Pages<Vec<MaskRow>>, pairs: &Pages<Vec<Pair>>, sups: &Pages<Vec<Number>>,
    n: usize, Ghost(db): Ghost<Db>, Ghost(pv): Ghost<Seq<Seq<u8>>>) -> (r: Option<Vec<Vec<RCell>>>)
    requires scan_all_lines_ok(db, all_lines@), dates_w.valid(),
        forall|key: i64, year: int| #[trigger] post_year(*dates_w, key, year) == dates(db, key as int, year),
        forall|key: i64| #[trigger] dates_w.rows@.contains_key(key) ==> years_positive(dates_w.rows@[key]@) && dates_w.rows@[key]@.map_values(|row: Year| row.year).no_duplicates(),
        masks_ready(*masks, db, pv), pairs_ready(*pairs, db), supplier_pages_ready(*sups, db), n == pv.len(), n <= 127,
    ensures r matches Some(delta) ==> delta.len() == n && forall|c: int, nk: int, y: int, f: int| 0 <= c < pv.len() && 0 <= f <= 1 ==>
        #[trigger] cell_at(delta[c]@, nk, y, f) == nation_total(filter_db(db, pv[c]), nk, y, f),
{
    let mut delta: Vec<Vec<RCell>> = Vec::new();
    let mut c = 0;
    while c < n
        invariant c <= n, delta.len() == c,
            forall|c2: int| 0 <= c2 < c ==> (#[trigger] delta@[c2])@.len() == 0,
        decreases n - c,
    { delta.push(Vec::new()); c += 1; }
    proof {
        assert forall|c2: int, nk: int, y: int, f: int| 0 <= c2 < pv.len() && 0 <= f <= 1 implies #[trigger] dval(delta@, c2, nk, y, f)
            == sum(all_lines@.take(0), lines_weight(db, pv[c2], nk, y, f)) by {
            lemma_sum_empty(all_lines@.take(0), lines_weight(db, pv[c2], nk, y, f));
            lemma_sum_empty(delta@[c2]@, |row: RCell| if row.nk as int == nk && row.year as int == y { if f == 0 { row.count as int } else { row.profit as int } } else { 0int });
        }
    }
    let mut index = 0;
    while index < all_lines.len()
        invariant index <= all_lines.len(), delta.len() == n, n == pv.len(), n <= 127,
            masks_ready(*masks, db, pv), pairs_ready(*pairs, db), supplier_pages_ready(*sups, db),
            dates_w.valid(),
            forall|key: i64, year: int| #[trigger] post_year(*dates_w, key, year) == dates(db, key as int, year),
            forall|key: i64| #[trigger] dates_w.rows@.contains_key(key) ==> years_positive(dates_w.rows@[key]@) && dates_w.rows@[key]@.map_values(|row: Year| row.year).no_duplicates(),
            forall|c2: int, nk: int, y: int, f: int| 0 <= c2 < pv.len() && 0 <= f <= 1 ==> #[trigger] dval(delta@, c2, nk, y, f)
                == sum(all_lines@.take(index as int), lines_weight(db, pv[c2], nk, y, f)),
        decreases all_lines.len() - index,
    {
        let line = all_lines[index];
        let ws = match dates_w.rows.get(&line.l_orderkey) { Some(years) => years_to_ws(years), None => Vec::new() };
        proof {
            assert forall|y: int| #[trigger] ysum(ws@, y) == dates(db, line.l_orderkey as int, y) by {
                assert(post_year(*dates_w, line.l_orderkey, y) == dates(db, line.l_orderkey as int, y));
                if dates_w.rows@.contains_key(line.l_orderkey) {
                    assert(post_year(*dates_w, line.l_orderkey, y) == year_at(dates_w.rows@[line.l_orderkey]@, y));
                }
                if !dates_w.rows@.contains_key(line.l_orderkey) {
                    lemma_sum_empty(ws@, |w: YearW| if w.year as int == y { w.weight as int } else { 0int });
                    lemma_sum_empty(Seq::<Year>::empty(), |r: Year| if r.year as int == y { r.count as int } else { 0int });
                }
            }
        }
        let ghost before = delta@;
        line_world(masks, pairs, sups, n, line, &ws, 1, &mut delta, Ghost(db), Ghost(pv))?;
        proof {
            assert forall|c2: int, nk: int, y: int, f: int| 0 <= c2 < pv.len() && 0 <= f <= 1 implies #[trigger] dval(delta@, c2, nk, y, f)
                == sum(all_lines@.take(index as int + 1), lines_weight(db, pv[c2], nk, y, f)) by {
                lemma_sum_take(all_lines@, index as int, lines_weight(db, pv[c2], nk, y, f));
            }
        }
        index += 1;
    }
    proof {
        assert(all_lines@.take(index as int) =~= all_lines@);
        assert forall|c2: int, nk: int, y: int, f: int| 0 <= c2 < pv.len() && 0 <= f <= 1 implies #[trigger] cell_at(delta[c2]@, nk, y, f)
            == nation_total(filter_db(db, pv[c2]), nk, y, f) by {
            let hl = |l: LineitemRow| hline(db, pv[c2], l, nk, y, f);
            assert(sum(line_in_seq_view(all_lines@), hl) == sum(db.lineitem, hl));
            lemma_sum_map(all_lines@, |r: LineIn| line_in_view(r), hl, lines_weight(db, pv[c2], nk, y, f));
            lemma_tl_bridge(db, pv[c2], nk, y, f);
            assert(dval(delta@, c2, nk, y, f) == cell_at(delta[c2]@, nk, y, f));
        }
    }
    Some(delta)
}

}
