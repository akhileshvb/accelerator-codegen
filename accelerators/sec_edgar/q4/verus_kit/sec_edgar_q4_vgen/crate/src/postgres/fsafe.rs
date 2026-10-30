// The engine's float SUM / AVG overflow check.  Postgres raises on overflow, so a group answers only
// when float_safe holds (gen_spec.rs); the stored sum of magnitudes and count of each group decide it.
use vstd::prelude::*;
use std::sync::Arc;
use verus_common::rel::*;
use verus_prooflib::ordered::{Index, Key};
use verus_prooflib::wide::Wide;
use crate::cells::*;
use crate::facts::*;
use crate::gen_obligations::*;
use crate::gen_spec::*;
use crate::numeric::*;
use crate::rec::opt_bytes;
use crate::walk::*;

verus! {

/// No float SUM / AVG of the query errors on this engine.
pub open spec fn engine_safe(db: Db, p: Params) -> bool {
    float_safe(db, p)
}

pub open spec fn cell_ok(abs_sum: int, count: int) -> bool {
    float8_sum_safe(abs_sum) && float8_avg_safe(abs_sum, count)
}

/// lim = the largest double times 2^1074 (float8_avg_safe's bound).
fn cell_safe(abs_sum: &Wide, count: &Wide, lim: &Wide, c: &Consts) -> (r: bool)
    requires
        consts_ok(*c),
        lim@ == (pow2(53) - 1) * pow2(2045) * pow2(1074),
    ensures
        r == cell_ok(abs_sum@, count@),
{
    if c.top.lt(&abs_sum.add(abs_sum)) {
        return false;
    }
    let n1 = count.add(&Wide::from_i128(1));
    let t = Wide::from_i128(4).mul(&n1).mul(&n1).mul(&n1).mul(abs_sum).mul(abs_sum);
    !lim.lt(&t)
}

/// A stored group is safe exactly when its stored aggregates are.
proof fn lemma_group_safe_cell(db: Db, p: Params, c: Cell)
    requires
        cell_facts(db, p, c.sic as int, opt_bytes(c.tlabel), c.count@, c.sum@, c.abs@, c.nciks as int),
    ensures
        group_float_safe(db, p, Some(c.sic as int), opt_bytes(c.tlabel), Some(p.p3)) == cell_ok(c.abs@, c.count@),
{
    let (gs, gt, gst) = (Some(c.sic as int), opt_bytes(c.tlabel), Some(p.p3));
    assert(group_sum(db, p, gs, gt, gst, |t: Tuple| if t.n.value is Some { abs(t.n.value->0) } else { 0 }) == c.abs@);
    assert(group_sum(db, p, gs, gt, gst, |t: Tuple| if t.n.value is Some { 1int } else { 0 }) == c.count@);
}

/// Whether the engine can aggregate every group of the rows (those a HAVING drops included).
pub fn rows_safe(rows: &Vec<(Key, Arc<Cell>)>, c: &Consts, Ghost(db): Ghost<Db>, Ghost(p): Ghost<Params>, Ghost(g): Ghost<Index<Index<Cell>>>) -> (ok: bool)
    requires
        rep(g, tot_db(db)),
        consts_ok(*c),
        rows_cover(rows@, g, p),
    ensures
        ok == engine_safe(db, p),
{
    let lim = c.top.mul(&c.pows[1074]);
    proof { assert(c.pows@[1074]@ == pow2(1074)); }
    let mut i: usize = 0;
    while i < rows.len()
        invariant
            i <= rows.len(),
            rep(g, tot_db(db)),
            consts_ok(*c),
            rows_cover(rows@, g, p),
            lim@ == (pow2(53) - 1) * pow2(2045) * pow2(1074),
            forall|j: int| 0 <= j < i ==> (#[trigger] rows[j]).1.count@ == 0 || cell_ok(rows[j].1.abs@, rows[j].1.count@),
        decreases rows.len() - i,
    {
        let cell = &*rows[i].1;
        if !cell.count.is_zero() && !cell_safe(&cell.abs, &cell.count, &lim, c) {
            proof {
                let (sc, tl) = (cell.sic as int, opt_bytes(cell.tlabel));
                lemma_cell_sound(db, p, g, *cell);
                lemma_count_nonneg(db, p, Some(sc), tl, Some(p.p3));
                lemma_group_safe_cell(db, p, *cell);
                assert(group_count(db, p, Some(sc), tl, Some(p.p3)) > 0);
                assert(!engine_safe(db, p)) by {
                    reveal(float_safe);
                    reveal(groups_float_safe);
                }
            }
            return false;
        }
        i += 1;
    }
    proof {
        assert(engine_safe(db, p)) by {
            reveal(float_safe);
            reveal(groups_float_safe);
            assert forall|gs: Option<int>, gt: Option<Seq<u8>>, gst: Option<Seq<u8>>|
                #[trigger] group_count(db, p, gs, gt, gst) > 0 implies group_float_safe(db, p, gs, gt, gst) by {
                lemma_count_as_group_sum(db, p, gs, gt, gst);
                if !(gs is Some && p.p1 <= gs->0 <= p.p2 && gst == Some(p.p3)) {
                    lemma_group_outside(db, p, gs, gt, gst, |t: Tuple| 1int);
                }
                let sc = gs->0;
                if cell_at(g, p.p4, p.p3, sc, gt) is None {
                    lemma_cell_absent(db, p, g, sc, gt);
                }
                let m = choose|m: int| 0 <= m < rows.len() && rows[m].1.sic as int == sc && opt_bytes(rows[m].1.tlabel) == gt;
                let cell = rows[m].1;
                lemma_cell_sound(db, p, g, *cell);
                lemma_group_safe_cell(db, p, *cell);
            }
        }
    }
    true
}

/// No answer exists when the engine cannot aggregate.
pub proof fn lemma_unsafe_unanswerable(db: Db, p: Params)
    requires !engine_safe(db, p),
    ensures !answer_fits(db, p),
{
}

}
