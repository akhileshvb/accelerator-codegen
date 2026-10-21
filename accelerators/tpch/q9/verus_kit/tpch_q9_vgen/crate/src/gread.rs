// The general read: nation-year totals for any pattern from PartAgg, the part names, the
// supplier nations and the partsupp costs.
use vstd::prelude::*;
use verus_common::sums::*;
use verus_common::sqlfns::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::storage::*;
use verus_prooflib::wide::Wide;
use crate::pmap::*;
use crate::gnames::*;
use crate::gapply::*;
use crate::wmap::*;

verus! {

broadcast use vstd::seq_lib::group_seq_properties;

pub open spec fn like_ind(p: Seq<u8>) -> spec_fn((int, Seq<u8>)) -> int {
    |kn: (int, Seq<u8>)| if like_match(kn.1, p) { 1int } else { 0int }
}

/// The weighted count of the part rows of a key whose name matches p.
pub fn green_of(rows: &Vec<NCell>, p: &[u8]) -> (r: Wide)
    ensures r@ == ncell_sum(rows@, like_ind(p@)),
{
    let mut total = Wide::zero();
    let mut i = 0;
    proof { lemma_sum_empty(rows@.take(0), ncell_w(like_ind(p@))); }
    while i < rows.len()
        invariant i <= rows.len(), total@ == ncell_sum(rows@.take(i as int), like_ind(p@)),
        decreases rows.len() - i,
    {
        proof { lemma_sum_take(rows@, i as int, ncell_w(like_ind(p@))); }
        if verus_prooflib::like_match_exec(rows[i].name.as_slice(), p) {
            total = total.add(&rows[i].count);
        }
        i += 1;
    }
    proof { assert(rows@.take(rows.len() as int) =~= rows@); }
    total
}

/// The sum of one field over the cells with keys (k2, k3).
pub fn cell_total(rows: &Vec<Cell>, k2: i64, k3: i64, field: u8) -> (r: Wide)
    requires field <= 2,
    ensures r@ == pcell_at(rows@, k2 as int, k3 as int, field as int),
{
    let mut total = Wide::zero();
    let mut i = 0;
    proof { lemma_sum_empty(rows@.take(0), pcell_w(k2 as int, k3 as int, field as int)); }
    while i < rows.len()
        invariant i <= rows.len(), field <= 2, total@ == pcell_at(rows@.take(i as int), k2 as int, k3 as int, field as int),
        decreases rows.len() - i,
    {
        proof { lemma_sum_take(rows@, i as int, pcell_w(k2 as int, k3 as int, field as int)); }
        if rows[i].k2 == k2 && rows[i].k3 == k3 {
            total = if field == 0 { total.add(&rows[i].a) } else if field == 1 { total.add(&rows[i].b) } else { total.add(&rows[i].c) };
        }
        i += 1;
    }
    proof { assert(rows@.take(rows.len() as int) =~= rows@); }
    total
}

pub open spec fn bucket_seq(m: PMap, q: i64) -> Seq<Cell> {
    match m.at(q) { Some(rows) => rows@, None => Seq::empty() }
}

/// A part's pair factor for one PartAgg cell.
pub open spec fn pf_cell(prows: Seq<Cell>, e: Cell, f: int) -> int {
    if f == 0 { pcell_at(prows, e.k2 as int, 0, 0) * e.a@ }
    else { pcell_at(prows, e.k2 as int, 0, 0) * e.b@ - pcell_at(prows, e.k2 as int, 0, 1) * e.c@ }
}

/// What one PartAgg cell adds to the nation-year totals, given its part's matched count.
pub open spec fn cell_term(sup: PMap, prows: Seq<Cell>, gq: int, nk: int, y: int, f: int) -> spec_fn(Cell) -> int {
    |e: Cell| if e.k3 == y { pm_at(sup, e.k2, nk, 0, 0) * (gq * pf_cell(prows, e, f)) } else { 0int }
}

pub open spec fn green_q(g: General, pat: Seq<u8>, q: i64) -> int {
    pn_at(g.pnames, q, like_ind(pat))
}

/// What a part adds to the nation-year totals.
pub open spec fn contrib(g: General, pat: Seq<u8>, q: i64, nk: int, y: int, f: int) -> int {
    sum(bucket_seq(g.agg, q), cell_term(g.sup, bucket_seq(g.pairs, q), green_q(g, pat, q), nk, y, f))
}

/// t[(nk, year)] += sup rows of s at nk * (cnt0, cnt1), for every nation of the supplier.
fn add_suppliers(t: &mut WMap, srows: &Vec<Cell>, year: i64, cnt0: &Wide, cnt1: &Wide)
    requires old(t).valid(),
    ensures final(t).valid(),
        forall|nk: i64, y: int, f: int| 0 <= f <= 1 ==> #[trigger] wm_at(*final(t), nk, y, f)
            == wm_at(*old(t), nk, y, f) + if y == year as int { pcell_at(srows@, nk as int, 0, 0) * kf(cnt0@, cnt1@, f) } else { 0 },
{
    let ghost base = *t;
    let mut i = 0;
    proof {
        assert forall|nk: i64, y: int, f: int| 0 <= f <= 1 implies #[trigger] wm_at(*t, nk, y, f)
            == wm_at(base, nk, y, f) + if y == year as int { pcell_at(srows@.take(0), nk as int, 0, 0) * kf(cnt0@, cnt1@, f) } else { 0 } by {
            lemma_sum_empty(srows@.take(0), pcell_w(nk as int, 0, 0));
            assert(0int * kf(cnt0@, cnt1@, f) == 0) by (nonlinear_arith);
        }
    }
    while i < srows.len()
        invariant i <= srows.len(), t.valid(), base == *old(t),
            forall|nk: i64, y: int, f: int| 0 <= f <= 1 ==> #[trigger] wm_at(*t, nk, y, f)
                == wm_at(base, nk, y, f) + if y == year as int { pcell_at(srows@.take(i as int), nk as int, 0, 0) * kf(cnt0@, cnt1@, f) } else { 0 },
        decreases srows.len() - i,
    {
        if srows[i].k3 == 0 {
            let d0 = srows[i].a.mul(cnt0);
            let d1 = srows[i].a.mul(cnt1);
            wm_add(t, srows[i].k2, year, &d0, &d1);
        }
        proof {
            assert forall|nk: i64, y: int, f: int| 0 <= f <= 1 implies #[trigger] wm_at(*t, nk, y, f)
                == wm_at(base, nk, y, f) + if y == year as int { pcell_at(srows@.take(i as int + 1), nk as int, 0, 0) * kf(cnt0@, cnt1@, f) } else { 0 } by {
                lemma_sum_take(srows@, i as int, pcell_w(nk as int, 0, 0));
                let prev = pcell_at(srows@.take(i as int), nk as int, 0, 0);
                let c = srows@[i as int];
                let kk = kf(cnt0@, cnt1@, f);
                if y == year as int && srows[i as int].k3 == 0 && nk == srows[i as int].k2 {
                    assert((prev + c.a@) * kk == prev * kk + c.a@ * kk) by (nonlinear_arith);
                } else if y == year as int {
                    assert(pcell_w(nk as int, 0, 0)(c) == 0);
                    assert((prev + 0) * kk == prev * kk);
                }
            }
        }
        i += 1;
    }
    proof { assert(srows@.take(srows.len() as int) =~= srows@); }
}


/// Same as add_suppliers for the supplier's cells found in the map.
fn add_suppliers_of(t: &mut WMap, sup: &PMap, s: i64, year: i64, cnt0: &Wide, cnt1: &Wide)
    requires old(t).valid(), sup.valid(),
    ensures final(t).valid(),
        forall|nk: i64, y: int, f: int| 0 <= f <= 1 ==> #[trigger] wm_at(*final(t), nk, y, f)
            == wm_at(*old(t), nk, y, f) + if y == year as int { pm_at(*sup, s, nk as int, 0, 0) * kf(cnt0@, cnt1@, f) } else { 0 },
{
    match sup.get(s) {
        Some(rows) => { add_suppliers(t, &rows, year, cnt0, cnt1); }
        None => {
            let empty: Vec<Cell> = Vec::new();
            let ghost base = *t;
            add_suppliers(t, &empty, year, cnt0, cnt1);
            proof {
                assert forall|nk: i64, y: int, f: int| 0 <= f <= 1 implies #[trigger] wm_at(*t, nk, y, f)
                    == wm_at(base, nk, y, f) + if y == year as int { pm_at(*sup, s, nk as int, 0, 0) * kf(cnt0@, cnt1@, f) } else { 0 } by {
                    lemma_sum_empty(empty@, pcell_w(nk as int, 0, 0));
                    assert(0int * kf(cnt0@, cnt1@, f) == 0) by (nonlinear_arith);
                }
            }
        }
    }
}

/// t += the contribution of every PartAgg cell of a part with matched count gq.
fn add_part_rows(sup: &PMap, t: &mut WMap, arows: &Vec<Cell>, prows: &Vec<Cell>, gq: &Wide)
    requires old(t).valid(), sup.valid(),
    ensures final(t).valid(),
        forall|nk: i64, y: int, f: int| 0 <= f <= 1 ==> #[trigger] wm_at(*final(t), nk, y, f)
            == wm_at(*old(t), nk, y, f) + sum(arows@, cell_term(*sup, prows@, gq@, nk as int, y, f)),
{
    let ghost base = *t;
    let mut i = 0;
    proof {
        assert forall|nk: i64, y: int, f: int| 0 <= f <= 1 implies #[trigger] wm_at(*t, nk, y, f)
            == wm_at(base, nk, y, f) + sum(arows@.take(0), cell_term(*sup, prows@, gq@, nk as int, y, f)) by {
            lemma_sum_empty(arows@.take(0), cell_term(*sup, prows@, gq@, nk as int, y, f));
        }
    }
    while i < arows.len()
        invariant i <= arows.len(), t.valid(), sup.valid(), base == *old(t),
            forall|nk: i64, y: int, f: int| 0 <= f <= 1 ==> #[trigger] wm_at(*t, nk, y, f)
                == wm_at(base, nk, y, f) + sum(arows@.take(i as int), cell_term(*sup, prows@, gq@, nk as int, y, f)),
        decreases arows.len() - i,
    {
        let e = &arows[i];
        let pc = cell_total(prows, e.k2, 0, 0);
        let pcost = cell_total(prows, e.k2, 0, 1);
        let cnt0 = gq.mul(&pc.mul(&e.a));
        let cnt1 = gq.mul(&pc.mul(&e.b).sub(&pcost.mul(&e.c)));
        let ghost before = *t;
        add_suppliers_of(t, sup, e.k2, e.k3, &cnt0, &cnt1);
        proof {
            assert(cnt0@ == gq@ * pf_cell(prows@, *e, 0));
            assert(cnt1@ == gq@ * pf_cell(prows@, *e, 1));
            assert forall|nk: i64, y: int, f: int| 0 <= f <= 1 implies #[trigger] wm_at(*t, nk, y, f)
                == wm_at(base, nk, y, f) + sum(arows@.take(i as int + 1), cell_term(*sup, prows@, gq@, nk as int, y, f)) by {
                lemma_sum_take(arows@, i as int, cell_term(*sup, prows@, gq@, nk as int, y, f));
                assert(kf(cnt0@, cnt1@, f) == gq@ * pf_cell(prows@, *e, f));
            }
        }
        i += 1;
    }
    proof { assert(arows@.take(arows.len() as int) =~= arows@); }
}

/// t += part q's contribution.
fn add_part(g: &General, t: &mut WMap, q: i64, gq: &Wide)
    requires old(t).valid(), g.sup.valid(), g.agg.valid(), g.pairs.valid(),
    ensures final(t).valid(),
        forall|nk: i64, y: int, f: int| 0 <= f <= 1 ==> #[trigger] wm_at(*final(t), nk, y, f)
            == wm_at(*old(t), nk, y, f) + sum(bucket_seq(g.agg, q), cell_term(g.sup, bucket_seq(g.pairs, q), gq@, nk as int, y, f)),
{
    let empty: Vec<Cell> = Vec::new();
    let ghost base = *t;
    match g.agg.get(q) {
        Some(arows) => match g.pairs.get(q) {
            Some(prows) => { add_part_rows(&g.sup, t, &arows, &prows, gq); }
            None => { add_part_rows(&g.sup, t, &arows, &empty, gq); proof { assert(empty@ =~= Seq::<Cell>::empty()); } }
        },
        None => {
            proof {
                assert forall|nk: i64, y: int, f: int| 0 <= f <= 1 implies #[trigger] wm_at(*t, nk, y, f)
                    == wm_at(base, nk, y, f) + sum(bucket_seq(g.agg, q), cell_term(g.sup, bucket_seq(g.pairs, q), gq@, nk as int, y, f)) by {
                    assert(bucket_seq(g.agg, q) =~= Seq::<Cell>::empty());
                    lemma_sum_empty(Seq::<Cell>::empty(), cell_term(g.sup, bucket_seq(g.pairs, q), gq@, nk as int, y, f));
                }
            }
        }
    }
}


proof fn lemma_unmatched_part(g: General, pat: Seq<u8>, q: i64, nk: int, y: int, f: int)
    requires green_q(g, pat, q) == 0,
    ensures contrib(g, pat, q, nk, y, f) == 0,
{
    let rows = bucket_seq(g.agg, q);
    let term = cell_term(g.sup, bucket_seq(g.pairs, q), 0, nk, y, f);
    assert forall|e: Cell| #[trigger] term(e) == 0 by {
        let pf = pf_cell(bucket_seq(g.pairs, q), e, f);
        assert(0int * pf == 0) by (nonlinear_arith);
        if e.k3 == y { assert(pm_at(g.sup, e.k2, nk, 0, 0) * 0int == 0) by (nonlinear_arith); }
    }
    lemma_sum_zero(rows, term);
}

pub open spec fn read_wf(g: General) -> bool {
    g.sup.valid() && g.agg.valid() && g.pairs.valid() && g.pnames.valid()
}

/// The nation-year totals of the part rows matching pat, summed over the part keys.
pub fn read_t(g: &General, pat: &[u8]) -> (t: WMap)
    requires read_wf(*g),
    ensures t.valid(),
        exists|keys: Seq<i64>| keys.no_duplicates() && (forall|k: i64| #[trigger] g.pnames.at(k).is_some() <==> keys.contains(k))
            && forall|nk: i64, y: int, f: int| 0 <= f <= 1 ==> #[trigger] wm_at(t, nk, y, f) == sum(keys, |q: i64| contrib(*g, pat@, q, nk as int, y, f)),
{
    let keys = g.pnames.keys();
    let mut t: WMap = Work::new();
    let mut i = 0;
    proof {
        assert forall|nk: i64, y: int, f: int| 0 <= f <= 1 implies #[trigger] wm_at(t, nk, y, f) == sum(keys@.take(0), |q: i64| contrib(*g, pat@, q, nk as int, y, f)) by {
            lemma_sum_empty(keys@.take(0), |q: i64| contrib(*g, pat@, q, nk as int, y, f));
        }
    }
    while i < keys.len()
        invariant i <= keys.len(), t.valid(), read_wf(*g),
            forall|nk: i64, y: int, f: int| 0 <= f <= 1 ==> #[trigger] wm_at(t, nk, y, f) == sum(keys@.take(i as int), |q: i64| contrib(*g, pat@, q, nk as int, y, f)),
        decreases keys.len() - i,
    {
        let q = keys[i];
        let gq = match g.pnames.get(q) {
            Some(rows) => green_of(&rows, pat),
            None => Wide::zero(),
        };
        proof { assert(gq@ == green_q(*g, pat@, q)); }
        let ghost before = t;
        if !gq.is_zero() {
            add_part(g, &mut t, q, &gq);
        }
        proof {
            assert forall|nk: i64, y: int, f: int| 0 <= f <= 1 implies #[trigger] wm_at(t, nk, y, f) == sum(keys@.take(i as int + 1), |q2: i64| contrib(*g, pat@, q2, nk as int, y, f)) by {
                lemma_sum_take(keys@, i as int, |q2: i64| contrib(*g, pat@, q2, nk as int, y, f));
                if gq@ == 0 { lemma_unmatched_part(*g, pat@, q, nk as int, y, f); }
            }
        }
        i += 1;
    }
    proof { assert(keys@.take(keys.len() as int) =~= keys@); }
    t
}

}
