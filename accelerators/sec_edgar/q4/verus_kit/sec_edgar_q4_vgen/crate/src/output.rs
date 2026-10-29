// The answer rows: a group's rounded sum and average, the rows ordered and cut.
use vstd::prelude::*;
use verus_common::rel::*;
use verus_common::sums::*;
use verus_prooflib::float::*;
use verus_prooflib::wide::Wide;
use crate::bridge::*;
use crate::f64bits::*;
use crate::gen_spec::*;
use crate::groups::*;
use crate::join::*;
use crate::join_spec::*;
use crate::numeric::*;
use std::sync::Arc;
use verus_prooflib::ordered::{Index, Key, KeyView};
use verus_prooflib::ordered::lemma_ordered_unique;

verus! {

pub struct Cand {
    pub sic: Option<i64>,
    pub tlabel: Option<Vec<u8>>,
    pub nciks: usize,
    pub cv_t: Wide,
    pub bits_t: Option<u64>,
    pub cv_a: Wide,
    pub bits_a: Option<u64>,
}

pub open spec fn cand_row(p: Params, c: Cand) -> OutRow {
    OutRow {
        sic: opt_int(c.sic),
        tlabel: opt_bytes(c.tlabel),
        stmt: Some(p.p3),
        num_companies: c.nciks as int,
        total_value: Some(c.cv_t@),
        avg_value: Some(c.cv_a@),
    }
}

/// cv is the double `bits` that nearest-rounds x, or x itself when x is no double.
pub open spec fn rounded_as(x: int, cv: int, bits: Option<u64>) -> bool {
    match bits {
        Some(b) => f64_rounded(b, x) && f64_finite(b) && cv == f64_val(b),
        None => !f64_in_range(x) && cv == x,
    }
}

/// The candidate for cell c: its sum and its average (the sum over the count, to an integer) rounded.
pub open spec fn cand_of(c: Cell, q: int, k: Cand) -> bool {
    &&& k.sic == c.sic
    &&& opt_bytes(k.tlabel) == opt_bytes(c.tlabel)
    &&& k.nciks == c.nciks
    &&& rounded_as(c.sum@, k.cv_t@, k.bits_t)
    &&& rounded_as(q, k.cv_a@, k.bits_a)
}

proof fn lemma_abs_gsum(l: Seq<Rec>, p: Params, gs: Option<int>, gt: Option<Seq<u8>>)
    requires all_nonneg(l),
    ensures abs(gsum(l, p, gs, gt, value())) <= gsum(l, p, gs, gt, magnitude()),
    decreases l.len(),
{
    if l.len() > 0 {
        let rest = l.drop_last();
        let last = l.last();
        assert(l =~= rest.push(last));
        assert forall|i: int| 0 <= i < rest.len() implies (#[trigger] rest[i]).w >= 0 by { assert(rest[i] == l[i]); }
        assert(last.w >= 0) by { assert(l[l.len() - 1] == last); }
        lemma_abs_gsum(rest, p, gs, gt);
        lemma_gsum_push(rest, last, p, gs, gt, value());
        lemma_gsum_push(rest, last, p, gs, gt, magnitude());
        if in_group(p, last, gs, gt) {
            let v = rec_jk(p, last).v;
            assert(abs(last.w as int * v) == last.w as int * abs(v)) by (nonlinear_arith) requires last.w >= 0;
        }
    } else {
        assert(l =~= Seq::<Rec>::empty());
    }
}

proof fn lemma_count_ge_ciks(l: Seq<Rec>, p: Params, gs: Option<int>, gt: Option<Seq<u8>>)
    requires all_nonneg(l),
    ensures
        cikseq(l, p, gs, gt).len() <= gsum(l, p, gs, gt, one()),
        0 <= gsum(l, p, gs, gt, one()),
    decreases l.len(),
{
    if l.len() > 0 {
        let rest = l.drop_last();
        let last = l.last();
        assert(l =~= rest.push(last));
        assert forall|i: int| 0 <= i < rest.len() implies (#[trigger] rest[i]).w >= 0 by { assert(rest[i] == l[i]); }
        assert(last.w >= 0) by { assert(l[l.len() - 1] == last); }
        lemma_count_ge_ciks(rest, p, gs, gt);
        lemma_gsum_push(rest, last, p, gs, gt, one());
        lemma_cikseq_push(rest, last, p, gs, gt);
        assert(one()(rec_jk(p, last)) == 1int);
        if in_group(p, last, gs, gt) {
            assert(last.w as int * one()(rec_jk(p, last)) == last.w as int);
        }
    } else {
        assert(l =~= Seq::<Rec>::empty());
    }
}

/// A rounded quotient is within the bound of float_within_div.
proof fn lemma_avg_within(v: int, q: int, e: int, a: int, k: int)
    requires
        k >= 1,
        2 * abs(q * k - e) <= k,
        abs(q) <= abs(e),
        abs(e) <= a,
        v == q || pow2(53) * abs(v - q) <= abs(q),
    ensures
        float_within_div(v * k, e, a, k, k),
{
    let big = pow2(53);
    if v * k == e || k >= big {
    } else {
        let d = abs(v - q);
        let t = abs(q * k - e);
        assert(d >= 0 && t >= 0);
        assert(big * d <= abs(q)) by {
            if v == q {
                assert(d == 0);
                assert(big * d == 0) by (nonlinear_arith) requires d == 0;
            }
        }
        assert(abs(v * k - e) <= k * d + t) by (nonlinear_arith)
            requires d == abs(v - q), t == abs(q * k - e), k >= 1;
        assert(2 * (big - k) * abs(v * k - e) <= 2 * k * a + (big - k) * abs(k)) by (nonlinear_arith)
            requires
                d >= 0, t >= 0, k >= 1, k < big,
                big * d <= abs(q), abs(q) <= a, 2 * t <= k,
                abs(v * k - e) <= k * d + t, abs(k) == k;
    }
}


/// A candidate built from a cell is a row of the answer.
pub proof fn lemma_cand_ok(db: Db, p: Params, l: Seq<Rec>, c: Cell, q: int, k: Cand)
    requires
        records_ok(db, p, l),
        cell_ok(c, l, p, opt_int(c.sic), opt_bytes(c.tlabel)),
        c.nciks >= 2,
        cand_of(c, q, k),
        2 * abs(q * c.count@ - c.sum@) <= c.count@,
        abs(q) <= abs(c.sum@),
    ensures
        row_ok(db, p, cand_row(p, k)),
{
    let (gs, gt) = (opt_int(c.sic), opt_bytes(c.tlabel));
    lemma_group_quantities(db, p, l, gs, gt);
    lemma_group_companies(db, p, l, gs, gt);
    lemma_count_ge_ciks(l, p, gs, gt);
    lemma_dedup_len(cikseq(l, p, gs, gt));
    lemma_abs_gsum(l, p, gs, gt);
    let (cnt, e, a) = (c.count@, c.sum@, c.abs@);
    assert(cnt >= 2);
    let r = cand_row(p, k);
    assert(r.sic == gs && r.tlabel == gt && r.stmt == Some(p.p3));
    assert(group_count(db, p, gs, gt, Some(p.p3)) == cnt);
    assert(group_sum(db, p, gs, gt, Some(p.p3), tuple_one()) == cnt);
    assert(group_sum(db, p, gs, gt, Some(p.p3), tuple_value()) == e);
    assert(group_sum(db, p, gs, gt, Some(p.p3), tuple_magnitude()) == a);
    assert(abs(e) <= a);
    match k.bits_t {
        Some(b) => {
            lemma_rounded_within(b, e, a, cnt - 1);
        },
        None => {},
    }
    assert(float_within(k.cv_t@, e, a, cnt - 1));
    lemma_avg_within(k.cv_a@, q, e, a, cnt);
    match k.bits_a {
        Some(b) => {
            lemma_avg_within(f64_val(b), q, e, a, cnt);
        },
        None => {
            lemma_avg_within(q, q, e, a, cnt);
        },
    }
    assert(float_within_div(k.cv_a@ * cnt, e, a, cnt - 1 + 1, cnt));
    assert(row_ok(db, p, r));
}


pub open spec fn row_cell_ok(l: Seq<Rec>, p: Params, row: (Key, Arc<Cell>)) -> bool {
    let cell = *row.1;
    cell_ok(cell, l, p, opt_int(cell.sic), opt_bytes(cell.tlabel)) && row.0.view() == gk(opt_int(cell.sic), opt_bytes(cell.tlabel))
}

/// The candidate rows of all groups with at least two companies.
pub fn make_cands(cells: &Index<Cell>, c: &Consts, Ghost(db): Ghost<Db>, Ghost(p): Ghost<Params>, Ghost(l): Ghost<Seq<Rec>>)
    -> (cands: Vec<Cand>)
    requires
        groups_ok(*cells, l, p),
        records_ok(db, p, l),
        consts_ok(*c),
    ensures
        forall|j: int| 0 <= j < cands.len() ==> row_ok(db, p, cand_row(p, #[trigger] cands[j])),
        forall|j: int, m: int| 0 <= j < m < cands.len() ==>
            !(cands[j].sic == cands[m].sic && opt_bytes(cands[j].tlabel) == opt_bytes(cands[m].tlabel)),
        forall|gs: Option<int>, gt: Option<Seq<u8>>|
            group_count(db, p, gs, gt, Some(p.p3)) > 0 && #[trigger] having(db, p, gs, gt, Some(p.p3))
                ==> exists|j: int| 0 <= j < cands.len() && opt_int(cands[j].sic) == gs && opt_bytes(cands[j].tlabel) == gt,
{
    let rows = cells.rows();
    let ghost entries = cells.entries();
    proof {
        cells.model();
        lemma_ordered_unique(entries);
        assert forall|m: int| 0 <= m < rows.len() implies row_cell_ok(l, p, #[trigger] rows@[m]) by {
            assert(entries[m] == (rows@[m].0.view(), *rows@[m].1));
            assert(cells.at(entries[m].0) == Some(entries[m].1));
            let cell = *rows@[m].1;
            let (gs, gt) = (opt_int(cell.sic), opt_bytes(cell.tlabel));
            assert(entries[m].0 == gk(gs, gt));
            assert(cells.at(gk(gs, gt)) is Some);
        }
    }
    let mut cands: Vec<Cand> = Vec::new();
    let ghost mut src: Seq<int> = Seq::empty();
    let mut i: usize = 0;
    while i < rows.len()
        invariant
            i <= rows.len(),
            groups_ok(*cells, l, p),
            records_ok(db, p, l),
            consts_ok(*c),
            rows@.map_values(|row: (Key, Arc<Cell>)| (row.0.view(), *row.1)) == entries,
            forall|a: int, b: int| 0 <= a < b < entries.len() ==> entries[a].0 != entries[b].0,
            forall|m: int| 0 <= m < rows.len() ==> row_cell_ok(l, p, #[trigger] rows@[m]),
            src.len() == cands.len(),
            forall|j: int| 0 <= j < cands.len() ==> row_ok(db, p, cand_row(p, #[trigger] cands[j])),
            forall|j: int| 0 <= j < cands.len() ==> 0 <= #[trigger] src[j] < i
                && cands[j].sic == rows@[src[j]].1.sic
                && opt_bytes(cands[j].tlabel) == opt_bytes(rows@[src[j]].1.tlabel),
            forall|j: int, m: int| 0 <= j < m < cands.len() ==> src[j] < src[m],
            forall|m: int| 0 <= m < i && rows@[m].1.nciks >= 2 ==> exists|j: int| 0 <= j < cands.len() && src[j] == m,
        decreases rows.len() - i,
    {
        let cell = &*rows[i].1;
        proof { assert(entries[i as int] == (rows@[i as int].0.view(), *rows@[i as int].1)); }
        if cell.nciks >= 2 {
            let ghost (gs, gt) = (opt_int(cell.sic), opt_bytes(cell.tlabel));
            proof {
                lemma_count_ge_ciks(l, p, gs, gt);
                lemma_dedup_len(cikseq(l, p, gs, gt));
                assert(cell.count@ >= 2);
            }
            let (cvt, bt) = round_value(&cell.sum, c);
            let q = div_round(&cell.sum, &cell.count);
            let (cva, ba) = round_value(&q, c);
            let k = Cand {
                sic: cell.sic,
                tlabel: match &cell.tlabel { Some(b) => Some(crate::rows::copy_bytes(b.as_slice())), None => None },
                nciks: cell.nciks,
                cv_t: cvt, bits_t: bt, cv_a: cva, bits_a: ba,
            };
            proof { lemma_cand_ok(db, p, l, *cell, q@, k); }
            let ghost old_len = cands.len();
            cands.push(k);
            proof {
                let src_old = src;
                src = src.push(i as int);
                assert forall|j: int| 0 <= j < cands.len() implies #[trigger] src[j] < i + 1 by {}
                assert forall|m: int| 0 <= m < i + 1 && rows@[m].1.nciks >= 2 implies exists|j: int| 0 <= j < cands.len() && src[j] == m by {
                    if m == i as int {
                        assert(src[old_len as int] == m);
                    } else {
                        let j = choose|j: int| 0 <= j < old_len && src_old[j] == m;
                        assert(src[j] == src_old[j]);
                    }
                }
            }
        } else {
            proof {
                assert forall|m: int| 0 <= m < i + 1 && rows@[m].1.nciks >= 2 implies exists|j: int| 0 <= j < cands.len() && src[j] == m by {
                    if m == i as int { assert(rows@[m].1.nciks == cell.nciks); assert(false); }
                    assert(m < i);
                    assert(exists|j: int| 0 <= j < cands.len() && src[j] == m);
                }
            }
        }
        i += 1;
    }
    proof {
        assert forall|j: int, m: int| 0 <= j < m < cands.len() implies
            !(cands[j].sic == cands[m].sic && opt_bytes(cands[j].tlabel) == opt_bytes(cands[m].tlabel)) by {
            let (a, b) = (src[j], src[m]);
            assert(entries[a].0 != entries[b].0);
            if cands[j].sic == cands[m].sic && opt_bytes(cands[j].tlabel) == opt_bytes(cands[m].tlabel) {
                assert(rows@[a].1.sic == rows@[b].1.sic);
                assert(entries[a].0 == gk(opt_int(rows@[a].1.sic), opt_bytes(rows@[a].1.tlabel)));
                assert(entries[b].0 == gk(opt_int(rows@[b].1.sic), opt_bytes(rows@[b].1.tlabel)));
            }
        }
        assert forall|gs: Option<int>, gt: Option<Seq<u8>>|
            group_count(db, p, gs, gt, Some(p.p3)) > 0 && #[trigger] having(db, p, gs, gt, Some(p.p3))
                implies exists|j: int| 0 <= j < cands.len() && opt_int(cands[j].sic) == gs && opt_bytes(cands[j].tlabel) == gt by {
            lemma_group_quantities(db, p, l, gs, gt);
            if cells.at(gk(gs, gt)) is None {
                assert(gsum(l, p, gs, gt, one()) == 0);
            }
            let cell = cells.at(gk(gs, gt))->0;
            lemma_group_companies(db, p, l, gs, gt);
            let m = choose|m: int| 0 <= m < entries.len() && entries[m].0 == gk(gs, gt);
            assert(entries[m] == (rows@[m].0.view(), *rows@[m].1));
            assert(cells.at(entries[m].0) == Some(entries[m].1));
            assert(*rows@[m].1 == cell);
            assert(cell_ok(cell, l, p, gs, gt));
            assert(rows@[m].1.nciks >= 2);
            let j = choose|j: int| 0 <= j < cands.len() && src[j] == m;
            assert(opt_int(cands[j].sic) == gs);
            assert(opt_bytes(cands[j].tlabel) == gt);
        }
    }
    cands
}

}
