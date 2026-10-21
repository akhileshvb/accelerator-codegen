// The general path's line pass: every line event adds its exact contribution to the nation
// totals T[(nation, year)] = (tuple count, profit).
use vstd::prelude::*;
use verus_common::sums::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::meaning::*;
use crate::storage::*;
use crate::wide::*;
use crate::wmap::*;
use crate::log::*;
use crate::ingest::*;
use crate::rollup::*;
use verus_common::rel::*;

verus! {

broadcast use vstd::seq_lib::group_seq_properties;

pub open spec fn kf(k0: int, k1: int, f: int) -> int {
    if f == 0 { k0 } else { k1 }
}

/// The sum of one field over the entries with key k2.
pub fn ent_total(rows: &Vec<Ent>, k2: i64, field: u8) -> (r: Wide)
    requires field <= 1,
    ensures r@ == ent_at(rows@, k2 as int, field as int),
{
    let mut total = Wide::zero();
    let mut i = 0;
    proof { lemma_sum_empty(rows@.take(0), ent_weight(k2 as int, field as int)); }
    while i < rows.len()
        invariant i <= rows.len(), total@ == ent_at(rows@.take(i as int), k2 as int, field as int),
        decreases rows.len() - i,
    {
        proof { lemma_sum_take(rows@, i as int, ent_weight(k2 as int, field as int)); }
        if rows[i].k2 == k2 {
            total = if field == 0 { total.add(&rows[i].a) } else { total.add(&rows[i].b) };
        }
        i += 1;
    }
    proof { assert(rows@.take(rows.len() as int) =~= rows@); }
    total
}

/// t[(nk, year)] += w * (the nation rows of srows at nk) * (k0, k1), for every nation in srows.
fn add_nations(t: &mut WMap, srows: &Vec<Ent>, year: i64, w: &Wide, k0: &Wide, k1: &Wide)
    requires old(t).valid(),
    ensures final(t).valid(),
        forall|nk: i64, y: int, f: int| 0 <= f <= 1 ==> #[trigger] wm_at(*final(t), nk, y, f)
            == wm_at(*old(t), nk, y, f) + if y == year as int { w@ * ent_at(srows@, nk as int, 0) * kf(k0@, k1@, f) } else { 0 },
{
    let ghost base = *t;
    let mut i = 0;
    proof {
        assert forall|nk: i64, y: int, f: int| 0 <= f <= 1 implies #[trigger] wm_at(*t, nk, y, f)
            == wm_at(base, nk, y, f) + if y == year as int { w@ * ent_at(srows@.take(0), nk as int, 0) * kf(k0@, k1@, f) } else { 0 } by {
            lemma_sum_empty(srows@.take(0), ent_weight(nk as int, 0));
            assert(w@ * 0int * kf(k0@, k1@, f) == 0) by (nonlinear_arith);
        }
    }
    while i < srows.len()
        invariant i <= srows.len(), t.valid(), base == *old(t),
            forall|nk: i64, y: int, f: int| 0 <= f <= 1 ==> #[trigger] wm_at(*t, nk, y, f)
                == wm_at(base, nk, y, f) + if y == year as int { w@ * ent_at(srows@.take(i as int), nk as int, 0) * kf(k0@, k1@, f) } else { 0 },
        decreases srows.len() - i,
    {
        let cnt = w.mul(&srows[i].a);
        let dc = cnt.mul(k0);
        let dp = cnt.mul(k1);
        let ghost before = *t;
        wm_add(t, srows[i].k2, year, &dc, &dp);
        proof {
            assert forall|nk: i64, y: int, f: int| 0 <= f <= 1 implies #[trigger] wm_at(*t, nk, y, f)
                == wm_at(base, nk, y, f) + if y == year as int { w@ * ent_at(srows@.take(i as int + 1), nk as int, 0) * kf(k0@, k1@, f) } else { 0 } by {
                lemma_sum_take(srows@, i as int, ent_weight(nk as int, 0));
                let prev = ent_at(srows@.take(i as int), nk as int, 0);
                let e = srows@[i as int];
                let kk = kf(k0@, k1@, f);
                if y == year as int && nk == srows[i as int].k2 {
                    assert(w@ * (prev + e.a@) * kk == w@ * prev * kk + (w@ * e.a@) * kk) by (nonlinear_arith);
                } else if y == year as int {
                    assert(w@ * (prev + 0) * kk == w@ * prev * kk);
                }
            }
        }
        i += 1;
    }
    proof { assert(srows@.take(srows.len() as int) =~= srows@); }
}

/// t[(nk, y)] += (the date rows of drows at y) * (the nation rows of srows at nk) * (k0, k1).
fn add_dates(t: &mut WMap, srows: &Vec<Ent>, drows: &Vec<Ent>, k0: &Wide, k1: &Wide)
    requires old(t).valid(),
    ensures final(t).valid(),
        forall|nk: i64, y: int, f: int| 0 <= f <= 1 ==> #[trigger] wm_at(*final(t), nk, y, f)
            == wm_at(*old(t), nk, y, f) + ent_at(drows@, y, 0) * ent_at(srows@, nk as int, 0) * kf(k0@, k1@, f),
{
    let ghost base = *t;
    let mut j = 0;
    proof {
        assert forall|nk: i64, y: int, f: int| 0 <= f <= 1 implies #[trigger] wm_at(*t, nk, y, f)
            == wm_at(base, nk, y, f) + ent_at(drows@.take(0), y, 0) * ent_at(srows@, nk as int, 0) * kf(k0@, k1@, f) by {
            lemma_sum_empty(drows@.take(0), ent_weight(y, 0));
            assert(0int * ent_at(srows@, nk as int, 0) * kf(k0@, k1@, f) == 0) by (nonlinear_arith);
        }
    }
    while j < drows.len()
        invariant j <= drows.len(), t.valid(), base == *old(t),
            forall|nk: i64, y: int, f: int| 0 <= f <= 1 ==> #[trigger] wm_at(*t, nk, y, f)
                == wm_at(base, nk, y, f) + ent_at(drows@.take(j as int), y, 0) * ent_at(srows@, nk as int, 0) * kf(k0@, k1@, f),
        decreases drows.len() - j,
    {
        add_nations(t, srows, drows[j].k2, &drows[j].a, k0, k1);
        proof {
            assert forall|nk: i64, y: int, f: int| 0 <= f <= 1 implies #[trigger] wm_at(*t, nk, y, f)
                == wm_at(base, nk, y, f) + ent_at(drows@.take(j as int + 1), y, 0) * ent_at(srows@, nk as int, 0) * kf(k0@, k1@, f) by {
                lemma_sum_take(drows@, j as int, ent_weight(y, 0));
                let prev = ent_at(drows@.take(j as int), y, 0);
                let n = ent_at(srows@, nk as int, 0);
                let kk = kf(k0@, k1@, f);
                if y == drows[j as int].k2 as int {
                    assert((prev + drows@[j as int].a@) * n * kk == prev * n * kk + drows@[j as int].a@ * n * kk) by (nonlinear_arith);
                }
            }
        }
        j += 1;
    }
    proof { assert(drows@.take(drows.len() as int) =~= drows@); }
}


/// The bucket of key k1 (an empty one when absent).
fn bucket<'a>(w: &'a WMap, k1: i64, empty: &'a Vec<Ent>) -> (r: &'a Vec<Ent>)
    requires empty@.len() == 0,
    ensures forall|k2: int, f: int| #[trigger] ent_at(r@, k2, f) == wm_at(*w, k1, k2, f),
{
    match w.rows.get(&k1) {
        Some(rows) => rows,
        None => {
            proof {
                assert forall|k2: int, f: int| #[trigger] ent_at(empty@, k2, f) == wm_at(*w, k1, k2, f) by {
                    lemma_sum_empty(empty@, ent_weight(k2, f));
                }
            }
            empty
        }
    }
}

/// The pair factor of a line in the index of partsupp rows.
pub open spec fn pff(p: WMap, line: LineIn, f: int) -> int {
    if f == 0 { wm_at(p, line.l_partkey, line.l_suppkey as int, 0) }
    else { wm_at(p, line.l_partkey, line.l_suppkey as int, 0) * line_weight(line_in_view(line), 1) - wm_at(p, line.l_partkey, line.l_suppkey as int, 1) * line.l_quantity as int }
}

/// t += sign * (the line's contribution) over every (nation, year), read from the indexes.
pub fn line_into_t(t: &mut WMap, d: &WMap, s: &WMap, p: &WMap, g: &WMap, line: LineIn, sign: i64)
    requires old(t).valid(),
    ensures final(t).valid(),
        forall|nk: i64, y: int, f: int| 0 <= f <= 1 ==> #[trigger] wm_at(*final(t), nk, y, f) == wm_at(*old(t), nk, y, f)
            + sign as int * (wm_at(*d, line.l_orderkey, y, 0) * (wm_at(*s, line.l_suppkey, nk as int, 0) * wm_at(*g, line.l_partkey, 0, 0) * pff(*p, line, f))),
{
    let empty: Vec<Ent> = Vec::new();
    let drows = bucket(d, line.l_orderkey, &empty);
    let srows = bucket(s, line.l_suppkey, &empty);
    let prows = bucket(p, line.l_partkey, &empty);
    let grows = bucket(g, line.l_partkey, &empty);
    let gv = ent_total(grows, 0, 0);
    let pc = ent_total(prows, line.l_suppkey, 0);
    let pcost = ent_total(prows, line.l_suppkey, 1);
    let rev = Wide::from_i128(line.l_extendedprice as i128).mul(&Wide::from_i128(100i128 - line.l_discount as i128));
    let qty = Wide::from_i128(line.l_quantity as i128);
    let pf1 = pc.mul(&rev).sub(&pcost.mul(&qty));
    let base = Wide::from_i128(sign as i128).mul(&gv);
    let k0 = base.mul(&pc);
    let k1 = base.mul(&pf1);
    let ghost before = *t;
    add_dates(t, srows, drows, &k0, &k1);
    proof {
        assert forall|nk: i64, y: int, f: int| 0 <= f <= 1 implies #[trigger] wm_at(*t, nk, y, f) == wm_at(before, nk, y, f)
            + sign as int * (wm_at(*d, line.l_orderkey, y, 0) * (wm_at(*s, line.l_suppkey, nk as int, 0) * wm_at(*g, line.l_partkey, 0, 0) * pff(*p, line, f))) by {
            let dd = wm_at(*d, line.l_orderkey, y, 0);
            let sc = wm_at(*s, line.l_suppkey, nk as int, 0);
            let gg = wm_at(*g, line.l_partkey, 0, 0);
            let q = pff(*p, line, f);
            let sg = sign as int;
            assert(ent_at(drows@, y, 0) == dd);
            assert(ent_at(srows@, nk as int, 0) == sc);
            assert(gv@ == gg);
            assert(rev@ == line_weight(line_in_view(line), 1));
            assert(kf(k0@, k1@, f) == sg * gg * q);
            assert(dd * sc * (sg * gg * q) == sg * (dd * (sc * gg * q))) by (nonlinear_arith);
        }
    }
}


/// The nation-year totals T (count, profit) of the lines of `events`, read from the indexes.
pub open spec fn line_term(d: WMap, s: WMap, p: WMap, g: WMap, nk: int, y: int, f: int) -> spec_fn(LineDelta) -> int {
    |ld: LineDelta| ld.sign as int * (wm_at(d, ld.row.l_orderkey, y, 0) * (wm_at(s, ld.row.l_suppkey, nk, 0) * wm_at(g, ld.row.l_partkey, 0, 0) * pff(p, ld.row, f)))
}

pub open spec fn t_index(t: WMap, d: WMap, s: WMap, p: WMap, g: WMap, events: Seq<LineDelta>) -> bool {
    t.valid() && forall|nk: i64, y: int, f: int| 0 <= f <= 1 ==> #[trigger] wm_at(t, nk, y, f) == sum(events, line_term(d, s, p, g, nk as int, y, f))
}

fn ingest_lines(t: &mut WMap, d: &WMap, s: &WMap, p: &WMap, g: &WMap, chunk: &[LineDelta])
    requires old(t).valid(),
    ensures final(t).valid(),
        forall|nk: i64, y: int, f: int| 0 <= f <= 1 ==> #[trigger] wm_at(*final(t), nk, y, f)
            == wm_at(*old(t), nk, y, f) + sum(chunk@, line_term(*d, *s, *p, *g, nk as int, y, f)),
{
    let ghost base = *t;
    let mut i = 0;
    proof {
        assert forall|nk: i64, y: int, f: int| 0 <= f <= 1 implies #[trigger] wm_at(*t, nk, y, f) == wm_at(base, nk, y, f) + sum(chunk@.take(0), line_term(*d, *s, *p, *g, nk as int, y, f)) by {
            lemma_sum_empty(chunk@.take(0), line_term(*d, *s, *p, *g, nk as int, y, f));
        }
    }
    while i < chunk.len()
        invariant i <= chunk.len(), t.valid(), base == *old(t),
            forall|nk: i64, y: int, f: int| 0 <= f <= 1 ==> #[trigger] wm_at(*t, nk, y, f) == wm_at(base, nk, y, f) + sum(chunk@.take(i as int), line_term(*d, *s, *p, *g, nk as int, y, f)),
        decreases chunk.len() - i,
    {
        line_into_t(t, d, s, p, g, chunk[i].row, chunk[i].sign);
        proof {
            assert forall|nk: i64, y: int, f: int| 0 <= f <= 1 implies #[trigger] wm_at(*t, nk, y, f) == wm_at(base, nk, y, f) + sum(chunk@.take(i as int + 1), line_term(*d, *s, *p, *g, nk as int, y, f)) by {
                lemma_sum_take(chunk@, i as int, line_term(*d, *s, *p, *g, nk as int, y, f));
            }
        }
        i += 1;
    }
    proof { assert(chunk@.take(chunk@.len() as int) =~= chunk@); }
}

pub fn t_from_log(log: &Log<LineDelta>, d: &WMap, s: &WMap, p: &WMap, g: &WMap) -> (t: WMap)
    ensures t_index(t, *d, *s, *p, *g, log.events()),
{
    let mut t: WMap = Work::new();
    let mut j = 0;
    proof {
        assert forall|nk: i64, y: int, f: int| 0 <= f <= 1 implies #[trigger] wm_at(t, nk, y, f) == sum(flat(log.parts().take(0)), line_term(*d, *s, *p, *g, nk as int, y, f)) by {
            assert(flat(log.parts().take(0)) =~= Seq::<LineDelta>::empty());
            lemma_sum_empty(Seq::<LineDelta>::empty(), line_term(*d, *s, *p, *g, nk as int, y, f));
        }
    }
    while j < log.chunks.len()
        invariant j <= log.chunks.len(), t.valid(),
            forall|nk: i64, y: int, f: int| 0 <= f <= 1 ==> #[trigger] wm_at(t, nk, y, f) == sum(flat(log.parts().take(j as int)), line_term(*d, *s, *p, *g, nk as int, y, f)),
        decreases log.chunks.len() - j,
    {
        ingest_lines(&mut t, d, s, p, g, log.chunks[j].as_slice());
        proof {
            let parts = log.parts();
            assert(parts.take(j as int + 1).drop_last() =~= parts.take(j as int));
            assert(flat(parts.take(j as int + 1)) == flat(parts.take(j as int)) + parts[j as int]);
            assert(parts[j as int] == (*log.chunks@[j as int])@);
            assert forall|nk: i64, y: int, f: int| 0 <= f <= 1 implies #[trigger] wm_at(t, nk, y, f) == sum(flat(parts.take(j as int + 1)), line_term(*d, *s, *p, *g, nk as int, y, f)) by {
                lemma_sum_concat(flat(parts.take(j as int)), parts[j as int], line_term(*d, *s, *p, *g, nk as int, y, f));
            }
        }
        j += 1;
    }
    proof { assert(log.parts().take(j as int) =~= log.parts()); }
    t
}

/// With indexes that agree with the database, T is the nation totals of the filtered database.
pub proof fn lemma_t_is_nation_total(t: WMap, d: WMap, s: WMap, p: WMap, g: WMap, events: Seq<LineDelta>, db: Db, pat: Seq<u8>)
    requires t_index(t, d, s, p, g, events), built_from(db.lineitem, line_delta_seq_view(events)),
        forall|k: i64, y: int| #[trigger] wm_at(d, k, y, 0) == dates(db, k as int, y),
        forall|k: i64, n: int| #[trigger] wm_at(s, k, n, 0) == supplier_count(db, k as int, n),
        forall|k: i64, n: int| #[trigger] wm_at(p, k, n, 0) == pair_count(db, k as int, n) && wm_at(p, k, n, 1) == pair_cost(db, k as int, n),
        forall|k: i64| #[trigger] wm_at(g, k, 0, 0) == green(filter_db(db, pat), k as int),
    ensures forall|nk: i64, y: int, f: int| 0 <= f <= 1 ==> #[trigger] wm_at(t, nk, y, f) == nation_total(filter_db(db, pat), nk as int, y, f),
{
    assert forall|nk: i64, y: int, f: int| 0 <= f <= 1 implies #[trigger] wm_at(t, nk, y, f) == nation_total(filter_db(db, pat), nk as int, y, f) by {
        let h = |l: LineitemRow| hline(db, pat, l, nk as int, y, f);
        lemma_built_sum(db.lineitem, line_delta_seq_view(events), h);
        assert forall|ld: LineDelta| #[trigger] line_term(d, s, p, g, nk as int, y, f)(ld) == signed(h)((line_in_view(ld.row), ld.sign as int)) by {
            let l = line_in_view(ld.row);
            assert(wm_at(d, ld.row.l_orderkey, y, 0) == dates(db, l.l_orderkey, y));
            assert(wm_at(s, ld.row.l_suppkey, nk as int, 0) == supplier_count(db, l.l_suppkey, nk as int));
            assert(wm_at(g, ld.row.l_partkey, 0, 0) == green(filter_db(db, pat), l.l_partkey));
            assert(pff(p, ld.row, f) == pf(db, l, f));
        }
        lemma_sum_map(events, |ld: LineDelta| (line_in_view(ld.row), ld.sign as int), signed(h), line_term(d, s, p, g, nk as int, y, f));
        assert(line_delta_seq_view(events) =~= events.map_values(|ld: LineDelta| (line_in_view(ld.row), ld.sign as int)));
        lemma_sum_ext_all(db.lineitem, h, |l: LineitemRow| hline(db, pat, l, nk as int, y, f));
        lemma_tl_bridge(db, pat, nk as int, y, f);
    }
}

}
