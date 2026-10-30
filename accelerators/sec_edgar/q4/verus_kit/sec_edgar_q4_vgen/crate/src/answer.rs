// The answer rows: a group's rounded sum and average, the rows ordered and cut.
use vstd::prelude::*;
use verus_common::rel::*;
use verus_common::sums::*;
use verus_prooflib::float::*;
use verus_prooflib::wide::Wide;
use verus_prooflib::lemma_sum_ones;
use crate::cells::*;
use crate::f64bits::*;
use crate::fsafe::*;
use crate::facts::*;
use crate::gen_obligations::*;
use crate::gen_spec::*;
use crate::numeric::*;
use crate::rec::*;
use crate::walk::*;
use std::sync::Arc;
use verus_prooflib::ordered::{Index, Key, KeyView, key_lt, lemma_key_order};
use crate::keys::*;
use verus_prooflib::ordered::lemma_ordered_unique;

verus! {

pub struct Cand {
    pub sic: Option<i64>,
    pub tlabel: Option<Vec<u8>>,
    pub nciks: u128,
    pub count: Wide,
    pub sum: Wide,
    pub abs: Wide,
    pub cv_t: Wide,
    pub bits_t: Option<u64>,
    pub avg_done: bool,
    pub cv_a: Wide,
    pub bits_a: Option<u64>,
    pub q: Ghost<int>,
}

/// The average a candidate stands for: the rounded one once computed, else its integer quotient.
pub open spec fn cand_avg(c: Cand) -> int {
    if c.avg_done { c.cv_a@ } else { c.q@ }
}

pub open spec fn cand_row(p: Params, c: Cand) -> OutRow {
    OutRow {
        sic: opt_int(c.sic),
        tlabel: opt_bytes(c.tlabel),
        stmt: Some(p.p3),
        num_companies: c.nciks as int,
        total_value: Some(c.cv_t@),
        avg_value: Some(cand_avg(c)),
    }
}

/// cv is the double `bits` that nearest-rounds x, or x itself when x is no double.
pub open spec fn rounded_as(x: int, cv: int, bits: Option<u64>) -> bool {
    match bits {
        Some(b) => f64_rounded(b, x) && f64_finite(b) && cv == f64_val(b),
        None => !f64_in_range(x) && cv == x,
    }
}

/// The candidate is a group of the query's answer: its aggregates are the group's, its total is rounded,
/// and its average is rounded from the integer quotient q once computed.
pub open spec fn cand_sound(db: Db, p: Params, k: Cand) -> bool {
    &&& k.sic is Some
    &&& cell_facts(db, p, k.sic->0 as int, opt_bytes(k.tlabel), k.count@, k.sum@, k.abs@, k.nciks as int)
    &&& k.nciks >= 2
    &&& rounded_as(k.sum@, k.cv_t@, k.bits_t)
    &&& 2 * abs(k.q@ * k.count@ - k.sum@) <= k.count@
    &&& abs(k.q@) <= abs(k.sum@)
    &&& (k.avg_done ==> rounded_as(k.q@, k.cv_a@, k.bits_a))
}

pub open spec fn quot_ok(e: int, k: int, q: int) -> bool {
    2 * abs(q * k - e) <= k && abs(q) <= abs(e)
}

/// Some integer is the quotient rounded.
proof fn lemma_exists_quot(e: int, k: int)
    requires k >= 1,
    ensures exists|q: int| #[trigger] quot_ok(e, k, q),
{
    let d = 2 * k;
    let n = 2 * e + k;
    let q = n / d;
    let r = n % d;
    vstd::arithmetic::div_mod::lemma_fundamental_div_mod(n, d);
    vstd::arithmetic::div_mod::lemma_mod_bound(n, d);
    assert(n == d * q + r);
    assert(2 * (q * k - e) == k - r) by (nonlinear_arith) requires n == d * q + r, d == 2 * k, n == 2 * e + k;
    assert(2 * abs(q * k - e) <= k);
    assert(abs(q) <= abs(e)) by (nonlinear_arith)
        requires 2 * abs(q * k - e) <= k, k >= 1;
    assert(quot_ok(e, k, q));
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


/// A sound candidate is a row of the answer.
pub proof fn lemma_cand_ok(db: Db, p: Params, k: Cand)
    requires cand_sound(db, p, k),
    ensures row_ok(db, p, cand_row(p, k)),
{
    let (gs, gt) = (Some(k.sic->0 as int), opt_bytes(k.tlabel));
    let (cnt, e, a, q) = (k.count@, k.sum@, k.abs@, k.q@);
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
    if k.avg_done {
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
    } else {
        lemma_avg_within(q, q, e, a, cnt);
        assert(float_within_div(q * cnt, e, a, cnt - 1 + 1, cnt));
    }
    assert(row_ok(db, p, r));
}

/// The candidate rows of all groups with at least two companies.
pub fn make_cands(rows: &Vec<(Key, Arc<Cell>)>, c: &Consts, Ghost(db): Ghost<Db>, Ghost(p): Ghost<Params>, Ghost(g): Ghost<Index<Index<Cell>>>)
    -> (cands: Vec<Cand>)
    requires
        rep(g, tot_db(db)),
        consts_ok(*c),
        forall|j: int| 0 <= j < rows.len() ==> p.p1 <= (#[trigger] rows[j]).1.sic as int <= p.p2
            && cell_at(g, p.p4, p.p3, rows[j].1.sic as int, opt_bytes(rows[j].1.tlabel)) == Some(*rows[j].1),
        forall|j: int, m: int| 0 <= j < m < rows.len() ==>
            !(rows[j].1.sic == rows[m].1.sic && opt_bytes(rows[j].1.tlabel) == opt_bytes(rows[m].1.tlabel)),
        forall|sc: int, tl: Option<Seq<u8>>| p.p1 <= sc <= p.p2 && #[trigger] cell_at(g, p.p4, p.p3, sc, tl) is Some ==>
            exists|j: int| 0 <= j < rows.len() && rows[j].1.sic as int == sc && opt_bytes(rows[j].1.tlabel) == tl,
    ensures
        forall|j: int| 0 <= j < cands.len() ==> cand_sound(db, p, #[trigger] cands[j]) && !cands[j].avg_done,
        forall|j: int, m: int| 0 <= j < m < cands.len() ==>
            !(cands[j].sic == cands[m].sic && opt_bytes(cands[j].tlabel) == opt_bytes(cands[m].tlabel)),
        forall|gs: Option<int>, gt: Option<Seq<u8>>|
            group_count(db, p, gs, gt, Some(p.p3)) > 0 && #[trigger] having(db, p, gs, gt, Some(p.p3))
                ==> exists|j: int| 0 <= j < cands.len() && opt_int(cands[j].sic) == gs && opt_bytes(cands[j].tlabel) == gt,
{
    let mut cands: Vec<Cand> = Vec::new();
    let ghost mut src: Seq<int> = Seq::empty();
    let mut i: usize = 0;
    while i < rows.len()
        invariant
            i <= rows.len(),
            rep(g, tot_db(db)),
            consts_ok(*c),
            forall|j: int| 0 <= j < rows.len() ==> p.p1 <= (#[trigger] rows[j]).1.sic as int <= p.p2
                && cell_at(g, p.p4, p.p3, rows[j].1.sic as int, opt_bytes(rows[j].1.tlabel)) == Some(*rows[j].1),
            src.len() == cands.len(),
            forall|j: int| 0 <= j < cands.len() ==> cand_sound(db, p, #[trigger] cands[j]) && !cands[j].avg_done,
            forall|j: int| 0 <= j < cands.len() ==> 0 <= #[trigger] src[j] < i
                && cands[j].sic == Some(rows[src[j]].1.sic)
                && opt_bytes(cands[j].tlabel) == opt_bytes(rows[src[j]].1.tlabel),
            forall|j: int, m: int| 0 <= j < m < cands.len() ==> src[j] < src[m],
            forall|m: int| 0 <= m < i && rows[m].1.nciks >= 2 ==> exists|j: int| 0 <= j < cands.len() && src[j] == m,
        decreases rows.len() - i,
    {
        let cell = &*rows[i].1;
        if cell.nciks >= 2 {
            proof {
                lemma_cell_sound(db, p, g, *cell);
                assert(cell.count@ >= 2);
            }
            let (cvt, bt) = round_value(&cell.sum, c);
            proof { lemma_exists_quot(cell.sum@, cell.count@); }
            let ghost q = choose|q: int| quot_ok(cell.sum@, cell.count@, q);
            let k = Cand {
                sic: Some(cell.sic),
                tlabel: match &cell.tlabel { Some(b) => Some(crate::rows::copy_bytes(b.as_slice())), None => None },
                nciks: cell.nciks,
                count: cell.count.dup(), sum: cell.sum.dup(), abs: cell.abs.dup(),
                cv_t: cvt, bits_t: bt,
                avg_done: false, cv_a: Wide::zero(), bits_a: None, q: Ghost(q),
            };
            proof { assert(cand_sound(db, p, k)); }
            let ghost old_len = cands.len();
            cands.push(k);
            proof {
                let src_old = src;
                src = src.push(i as int);
                assert forall|j: int| 0 <= j < cands.len() implies #[trigger] src[j] < i + 1 by {}
                assert forall|m: int| 0 <= m < i + 1 && rows[m].1.nciks >= 2 implies exists|j: int| 0 <= j < cands.len() && src[j] == m by {
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
                assert forall|m: int| 0 <= m < i + 1 && rows[m].1.nciks >= 2 implies exists|j: int| 0 <= j < cands.len() && src[j] == m by {
                    if m == i as int { assert(rows[m].1.nciks == cell.nciks); assert(false); }
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
            assert(a < b);
        }
        assert forall|gs: Option<int>, gt: Option<Seq<u8>>|
            group_count(db, p, gs, gt, Some(p.p3)) > 0 && #[trigger] having(db, p, gs, gt, Some(p.p3))
                implies exists|j: int| 0 <= j < cands.len() && opt_int(cands[j].sic) == gs && opt_bytes(cands[j].tlabel) == gt by {
            lemma_count_as_group_sum(db, p, gs, gt, Some(p.p3));
            if !(gs is Some && p.p1 <= gs->0 <= p.p2) {
                lemma_group_outside(db, p, gs, gt, Some(p.p3), |t: Tuple| 1int);
            }
            let sc = gs->0;
            if cell_at(g, p.p4, p.p3, sc, gt) is None {
                lemma_cell_absent(db, p, g, sc, gt);
            }
            let m = choose|m: int| 0 <= m < rows.len() && rows[m].1.sic as int == sc && opt_bytes(rows[m].1.tlabel) == gt;
            let cell = rows[m].1;
            assert(cell_at(g, p.p4, p.p3, sc, gt) == Some(*cell));
            lemma_cell_sound(db, p, g, *cell);
            assert(having(db, p, gs, gt, Some(p.p3)) == (distinct_count(comp_seq(db, p, sc, gt)) >= 2));
            assert(cell.nciks >= 2);
            let j = choose|j: int| 0 <= j < cands.len() && src[j] == m;
            assert(cands[j].sic == Some(cell.sic));
        }
    }
    cands
}


proof fn lemma_index_permutation(sorted: Seq<usize>, original: Seq<usize>, length: nat)
    requires verus_common::sort::same_bag(sorted, original), original.len() == length,
        forall|index: int| 0 <= index < original.len() ==> (#[trigger] original[index]) as int == index,
        forall|index: int| 0 <= index < sorted.len() ==> (#[trigger] sorted[index]) < length,
    ensures sorted.no_duplicates(),
        forall|index: usize| index < length ==> #[trigger] sorted.contains(index),
{
    assert forall|left: int, right: int| 0 <= left < right < sorted.len() implies
        (#[trigger] sorted[left]) != (#[trigger] sorted[right]) by {
        if sorted[left] == sorted[right] {
            let target = sorted[left];
            let weight = |item: usize| if item == target { 1int } else { 0int };
            lemma_sum_single_match(original, weight, target as int);
            assert(sum(sorted, weight) == sum(original, weight));
            lemma_sum_remove(sorted, left, weight);
            let smaller = sorted.remove(left);
            assert(smaller[right - 1] == target);
            lemma_sum_ge_term(smaller, weight, right - 1);
        }
    }
    assert forall|index: usize| index < length implies #[trigger] sorted.contains(index) by {
        let weight = |item: usize| if item == index { 1int } else { 0int };
        lemma_sum_single_match(original, weight, index as int);
        assert(sum(sorted, weight) == 1);
        if !sorted.contains(index) {
            assert forall|position: int| 0 <= position < sorted.len() implies weight(#[trigger] sorted[position]) == 0 by {}
            lemma_sum_zero(sorted, weight);
        }
    }
}

/// The candidates in the sorted order, as answer rows.
pub open spec fn sorted_rows(p: Params, cands: Seq<Cand>, sorted: Seq<usize>) -> Seq<OutRow> {
    sorted.map_values(|i: usize| cand_row(p, cands[i as int]))
}

/// The candidates, sorted by their total descending, are a full answer.
proof fn lemma_sorted_full(db: Db, p: Params, cands: Seq<Cand>, sorted: Seq<usize>)
    requires
        engine_safe(db, p),
        cands.len() <= usize::MAX,
        forall|j: int| 0 <= j < cands.len() ==> row_ok(db, p, cand_row(p, #[trigger] cands[j])),
        forall|j: int, m: int| 0 <= j < m < cands.len() ==>
            !(cands[j].sic == cands[m].sic && opt_bytes(cands[j].tlabel) == opt_bytes(cands[m].tlabel)),
        forall|gs: Option<int>, gt: Option<Seq<u8>>|
            group_count(db, p, gs, gt, Some(p.p3)) > 0 && #[trigger] having(db, p, gs, gt, Some(p.p3))
                ==> exists|j: int| 0 <= j < cands.len() && opt_int(cands[j].sic) == gs && opt_bytes(cands[j].tlabel) == gt,
        sorted.no_duplicates(),
        forall|index: usize| index < cands.len() ==> #[trigger] sorted.contains(index),
        forall|k: int| 0 <= k < sorted.len() ==> (#[trigger] sorted[k]) < cands.len(),
        forall|a: int, b: int| 0 <= a < b < sorted.len() ==> cands[sorted[a] as int].cv_t@ >= cands[sorted[b] as int].cv_t@,
    ensures
        full_result(db, p, sorted_rows(p, cands, sorted)),
{
    let full = sorted_rows(p, cands, sorted);
    assert(full.len() == sorted.len());
    assert forall|i: int| 0 <= i < full.len() implies row_ok(db, p, #[trigger] full[i]) by {
        assert(full[i] == cand_row(p, cands[sorted[i] as int]));
    }
    assert forall|i: int, j: int| 0 <= i < j < full.len() implies
        !same_group(#[trigger] full[i], #[trigger] full[j]) && !row_lt(full[j], full[i]) by {
        let (x, y) = (sorted[i] as int, sorted[j] as int);
        assert(full[i] == cand_row(p, cands[x]));
        assert(full[j] == cand_row(p, cands[y]));
        assert(x != y);
        if x < y { assert(!(cands[x].sic == cands[y].sic && opt_bytes(cands[x].tlabel) == opt_bytes(cands[y].tlabel))); }
        else { assert(!(cands[y].sic == cands[x].sic && opt_bytes(cands[y].tlabel) == opt_bytes(cands[x].tlabel))); }
        assert(cands[x].cv_t@ >= cands[y].cv_t@);
    }
    assert forall|gs: Option<int>, gt: Option<Seq<u8>>, gst: Option<Seq<u8>>|
        #[trigger] group_count(db, p, gs, gt, gst) > 0 && having(db, p, gs, gt, gst)
            implies exists|i: int| 0 <= i < full.len() && (#[trigger] full[i]).sic == gs && full[i].tlabel == gt && full[i].stmt == gst by {
        if gst != Some(p.p3) {
            lemma_count_as_group_sum(db, p, gs, gt, gst);
            lemma_group_outside(db, p, gs, gt, gst, |t: Tuple| 1int);
        }
        let j = choose|j: int| 0 <= j < cands.len() && opt_int(cands[j].sic) == gs && opt_bytes(cands[j].tlabel) == gt;
        assert(j < cands.len() && 0 <= j);
        let ju = j as usize;
        assert(ju as int == j);
        assert(sorted.contains(ju));
        let i = choose|i: int| 0 <= i < sorted.len() && sorted[i] == ju;
        assert(full[i] == cand_row(p, cands[sorted[i] as int]));
    }
}


pub type Out = (Option<i64>, Option<Vec<u8>>, Option<Vec<u8>>, i64, Option<u64>, Option<u64>);

pub open spec fn cand_fits(k: Cand) -> bool {
    k.bits_t is Some && k.avg_done && k.bits_a is Some && k.nciks <= i64::MAX as u128
}

/// The candidate's values and its bits agree.
pub open spec fn cand_sane(k: Cand) -> bool {
    &&& match k.bits_t {
        Some(b) => f64_in_range(k.cv_t@) && k.cv_t@ == f64_val(b),
        None => !f64_in_range(k.cv_t@),
    }
    &&& (k.avg_done ==> match k.bits_a {
        Some(b) => f64_in_range(k.cv_a@) && k.cv_a@ == f64_val(b),
        None => !f64_in_range(k.cv_a@),
    })
}

proof fn lemma_cand_sane(db: Db, p: Params, k: Cand)
    requires cand_sound(db, p, k),
    ensures cand_sane(k),
{
    if let Some(b) = k.bits_t { lemma_f64_in_range(b); }
    if k.avg_done { if let Some(b) = k.bits_a { lemma_f64_in_range(b); } }
}

/// A candidate that does not fit the result's types is in no answer that fits; one that does is a fitting row.
proof fn lemma_fits_row(p: Params, k: Cand)
    requires cand_sane(k), k.avg_done,
    ensures out_fits(cand_row(p, k)) == cand_fits(k),
{
}

/// Two candidates of the same group with the same total.
pub open spec fn same_key(a: Cand, b: Cand) -> bool {
    a.sic == b.sic && opt_bytes(a.tlabel) == opt_bytes(b.tlabel) && a.cv_t@ == b.cv_t@
}

/// The candidate with its average rounded from the integer quotient.
fn complete_avg(k: &Cand, c: &Consts, Ghost(db): Ghost<Db>, Ghost(p): Ghost<Params>) -> (r: Cand)
    requires consts_ok(*c), cand_sound(db, p, *k), !k.avg_done,
    ensures cand_sound(db, p, r), r.avg_done, same_key(*k, r),
{
    let q = crate::divfast::div_round_fast(&k.sum, &k.count, c);
    let (cva, ba) = round_value(&q, c);
    Cand {
        sic: k.sic,
        tlabel: match &k.tlabel { Some(b) => Some(crate::rows::copy_bytes(b.as_slice())), None => None },
        nciks: k.nciks,
        count: k.count.dup(), sum: k.sum.dup(), abs: k.abs.dup(),
        cv_t: k.cv_t.dup(), bits_t: k.bits_t,
        avg_done: true, cv_a: cva, bits_a: ba, q: Ghost(q@),
    }
}

/// The answer: the first 500 candidates in order (their averages rounded now), or None when one of them
/// has a value that is no double.
pub fn finish(mut cands: Vec<Cand>, stmt: &[u8], c: &Consts, Ghost(db): Ghost<Db>, Ghost(p): Ghost<Params>)
    -> (r: Option<Vec<Out>>)
    requires
        p.p3 == stmt@,
        engine_safe(db, p),
        consts_ok(*c),
        forall|j: int| 0 <= j < cands.len() ==> cand_sound(db, p, #[trigger] cands[j]) && !cands[j].avg_done,
        forall|j: int, m: int| 0 <= j < m < cands.len() ==>
            !(cands[j].sic == cands[m].sic && opt_bytes(cands[j].tlabel) == opt_bytes(cands[m].tlabel)),
        forall|gs: Option<int>, gt: Option<Seq<u8>>|
            group_count(db, p, gs, gt, Some(p.p3)) > 0 && #[trigger] having(db, p, gs, gt, Some(p.p3))
                ==> exists|j: int| 0 <= j < cands.len() && opt_int(cands[j].sic) == gs && opt_bytes(cands[j].tlabel) == gt,
    ensures
        r matches Some(out) ==> query_result(db, p, out_view(out@)),
        r matches Some(out) ==> forall|i: int| 0 <= i < out.len() ==> out_fits(#[trigger] out_view(out@)[i]),
        r is None ==> !answer_fits(db, p),
{
    let n = cands.len();
    let ghost orig = cands@;
    let mut indices: Vec<usize> = Vec::with_capacity(n);
    let mut i: usize = 0;
    while i < n
        invariant i <= n, indices.len() == i, n == cands.len(),
            forall|position: int| 0 <= position < indices.len() ==> (#[trigger] indices@[position]) as int == position,
        decreases n - i,
    {
        indices.push(i);
        i += 1;
    }
    let ghost valid = |x: usize| x < cands.len();
    let ghost le = |a: usize, b: usize| cands@[a as int].cv_t@ >= cands@[b as int].cv_t@;
    let cands_ref = &cands;
    let compare = |a: usize, b: usize| -> (result: bool)
        requires a < cands_ref.len(), b < cands_ref.len(),
        ensures result == (cands_ref@[a as int].cv_t@ >= cands_ref@[b as int].cv_t@),
    { !cands_ref[a].cv_t.lt(&cands_ref[b].cv_t) };
    proof {
        assert forall|a: usize, b: usize, c: usize| valid(a) && valid(b) && valid(c)
            && #[trigger] le(a, b) && #[trigger] le(b, c) implies le(a, c) by {}
        assert forall|a: usize, b: usize| valid(a) && valid(b) implies #[trigger] le(a, b) || le(b, a) by {}
    }
    let sorted = verus_common::sort::sort_idx(&indices, &compare, Ghost(le), Ghost(valid));
    proof {
        assert(sorted@.len() == n) by {
            lemma_sum_ones(sorted@);
            lemma_sum_ones(indices@);
            assert(sum(sorted@, |x: usize| 1int) == sum(indices@, |x: usize| 1int));
        }
        lemma_index_permutation(sorted@, indices@, n as nat);
        assert forall|a: int, b: int| 0 <= a < b < sorted.len() implies
            orig[sorted@[a] as int].cv_t@ >= orig[sorted@[b] as int].cv_t@ by {
            assert(le(sorted@[a], sorted@[b]));
        }
    }
    let m = if sorted.len() < 500 { sorted.len() } else { 500 };
    // the first m candidates get their averages rounded (the rest are cut)
    let mut r: usize = 0;
    while r < m
        invariant
            r <= m, m <= sorted.len(), m <= 500, cands.len() == n, sorted.len() == n,
            consts_ok(*c),
            forall|j: int| 0 <= j < sorted.len() ==> (#[trigger] sorted@[j]) < n,
            sorted@.no_duplicates(),
            forall|j: int| 0 <= j < n ==> cand_sound(db, p, #[trigger] cands[j]) && same_key(cands[j], orig[j]),
            forall|j: int| 0 <= j < r ==> cands[sorted@[j] as int].avg_done,
            forall|j: int| r <= j < n ==> !cands[sorted@[j] as int].avg_done,
        decreases m - r,
    {
        let idx = sorted[r];
        let done = complete_avg(&cands[idx], c, Ghost(db), Ghost(p));
        cands.set(idx, done);
        proof {
            assert forall|j: int| r + 1 <= j < n implies !cands[sorted@[j] as int].avg_done by {
                assert(sorted@[j] != sorted@[r as int]);
            }
        }
        r += 1;
    }
    proof {
        assert forall|j: int| 0 <= j < cands.len() implies row_ok(db, p, cand_row(p, #[trigger] cands[j])) by {
            lemma_cand_ok(db, p, cands[j]);
        }
        assert forall|j: int| 0 <= j < cands.len() implies cand_sane(#[trigger] cands[j]) by {
            lemma_cand_sane(db, p, cands[j]);
        }
        assert forall|j: int, k2: int| 0 <= j < k2 < cands.len() implies
            !(cands[j].sic == cands[k2].sic && opt_bytes(cands[j].tlabel) == opt_bytes(cands[k2].tlabel)) by {
            assert(same_key(cands[j], orig[j]) && same_key(cands[k2], orig[k2]));
        }
        assert forall|gs: Option<int>, gt: Option<Seq<u8>>|
            group_count(db, p, gs, gt, Some(p.p3)) > 0 && #[trigger] having(db, p, gs, gt, Some(p.p3))
                implies exists|j: int| 0 <= j < cands.len() && opt_int(cands[j].sic) == gs && opt_bytes(cands[j].tlabel) == gt by {
            let j = choose|j: int| 0 <= j < orig.len() && opt_int(orig[j].sic) == gs && opt_bytes(orig[j].tlabel) == gt;
            assert(same_key(cands[j], orig[j]));
        }
        assert forall|a: int, b: int| 0 <= a < b < sorted.len() implies
            cands@[sorted@[a] as int].cv_t@ >= cands@[sorted@[b] as int].cv_t@ by {
            assert(same_key(cands[sorted@[a] as int], orig[sorted@[a] as int]));
            assert(same_key(cands[sorted@[b] as int], orig[sorted@[b] as int]));
        }
        lemma_sorted_full(db, p, cands@, sorted@);
    }
    let ghost full = sorted_rows(p, cands@, sorted@);
    let ghost limited = if full.len() <= 500 { full } else { full.take(500) };
    proof { assert(query_result(db, p, limited)); }
    let mut out: Vec<Out> = Vec::new();
    let mut k: usize = 0;
    proof {
        assert(limited.len() == m);
        assert forall|j: int| 0 <= j < m implies limited[j] == cand_row(p, cands@[sorted@[j] as int]) by {
            assert(limited[j] == full[j]);
        }
    }
    while k < m
        invariant
            k <= m, m <= sorted.len(), m <= 500, m == limited.len(),
            sorted.len() == cands.len(),
            forall|j: int| 0 <= j < cands.len() ==> cand_sane(#[trigger] cands[j]),
            forall|j: int| 0 <= j < m ==> cands[sorted@[j] as int].avg_done,
            p.p3 == stmt@,
            forall|j: int| 0 <= j < sorted.len() ==> (#[trigger] sorted@[j]) < cands.len(),
            forall|j: int| 0 <= j < m ==> limited[j] == cand_row(p, cands@[sorted@[j] as int]),
            out.len() == k,
            forall|j: int| 0 <= j < k ==> out_view(out@)[j] == limited[j] && out_fits(limited[j]),
            query_result(db, p, limited),
            out_view(out@).len() == k,
        decreases m - k,
    {
        let cand = &cands[sorted[k]];
        proof {
            assert(limited[k as int] == cand_row(p, *cand));
            assert(cands@[sorted@[k as int] as int] == *cand);
            let idx = sorted@[k as int] as int;
            assert(0 <= idx < cands.len());
            assert(cand_sane(cands[idx]));
            assert(cand.avg_done);
            lemma_fits_row(p, *cand);
        }
        if cand.bits_t.is_none() || cand.bits_a.is_none() || cand.nciks > 0x7fff_ffff_ffff_ffffu128 {
            proof {
                assert(!cand_fits(*cand));
                assert(!out_fits(limited[k as int]));
                assert(query_result(db, p, limited));
                assert(limited.len() <= max_rows());
                assert(!answer_fits(db, p));
            }
            return None;
        }
        let row: Out = (
            cand.sic,
            match &cand.tlabel { Some(b) => Some(crate::rows::copy_bytes(b.as_slice())), None => None },
            Some(crate::rows::copy_bytes(stmt)),
            cand.nciks as i64,
            cand.bits_t,
            cand.bits_a,
        );
        proof {
            assert(cand_fits(*cand));
            assert(out_fits(cand_row(p, *cand)));
            let ov = out_view(out@);
            assert(out_view(out@.push(row)) =~= ov.push(cand_row(p, *cand))) by {
                assert(out_view(out@.push(row)).len() == k + 1);
                assert forall|j: int| 0 <= j < k + 1 implies out_view(out@.push(row))[j] == ov.push(cand_row(p, *cand))[j] by {
                    if j == k as int {
                        assert(out@.push(row)[j] == row);
                        assert(row.0 == cand.sic);
                        assert(row.4 == cand.bits_t && row.5 == cand.bits_a);
                        assert(row.3 as int == cand.nciks as int);
                        assert(opt_bytes(cand.tlabel) == (match row.1 { Some(v) => Some(v@), None => None }));
                        assert(row.2 is Some && row.2->0@ == p.p3);
                        assert(out_view(out@.push(row))[j] == cand_row(p, *cand));
                    } else {
                        assert(out@.push(row)[j] == out@[j]);
                    }
                }
            }
        }
        out.push(row);
        k += 1;
    }
    proof {
        assert(out_view(out@) =~= limited);
    }
    Some(out)
}

/// The answer to the query for a parameter set, from the stored groups.
pub fn answer(groups: &Index<Index<Cell>>, p1: i64, p2: i64, p3: &[u8], p4: &[u8], c: &Consts, Ghost(db): Ghost<Db>) -> (r: Option<Vec<Out>>)
    requires
        rep(*groups, tot_db(db)),
        consts_ok(*c),
    ensures
        r matches Some(out) ==> query_result(db, params(p1 as int, p2 as int, p3@, p4@), out_view(out@)),
        r matches Some(out) ==> forall|i: int| 0 <= i < out.len() ==> out_fits(#[trigger] out_view(out@)[i]),
        r is None ==> !answer_fits(db, params(p1 as int, p2 as int, p3@, p4@)),
{
    let ghost p = params(p1 as int, p2 as int, p3@, p4@);
    let ghost g = *groups;
    let ok = make_key2(p4, p3);
    let rows: Vec<(Key, Arc<Cell>)> = match groups.get(&ok) {
        Some(a) => {
            let inner = a;
            let rows = range_rows(&*inner, p1, p2);
            proof {
                assert(g.at(key2(p4@, p3@)) == Some(*inner));
                assert(inner_ok(*inner));
                let ii = *inner;
                ii.model();
                let es = entries_of(rows@);
                lemma_ordered_unique(ii.entries());
                assert forall|j: int| 0 <= j < rows.len() implies p.p1 <= (#[trigger] rows[j]).1.sic as int <= p.p2
                    && cell_at(g, p.p4, p.p3, rows[j].1.sic as int, opt_bytes(rows[j].1.tlabel)) == Some(*rows[j].1) by {
                    lemma_entries_at(rows@, j);
                    assert(ii.entries().contains(es[j]));
                    let m = choose|m: int| 0 <= m < ii.entries().len() && ii.entries()[m] == es[j];
                    assert(ii.at(es[j].0) == Some(es[j].1));
                    assert(es[j] == (rows[j].0.view(), *rows[j].1));
                    assert(es[j].0 == gkey(rows[j].1.sic as int, opt_bytes(rows[j].1.tlabel)));
                }
                assert forall|j: int, m: int| 0 <= j < m < rows.len() implies
                    !(rows[j].1.sic == rows[m].1.sic && opt_bytes(rows[j].1.tlabel) == opt_bytes(rows[m].1.tlabel)) by {
                    lemma_entries_at(rows@, j);
                    lemma_entries_at(rows@, m);
                    assert(key_lt(es[j].0, es[m].0));
                    lemma_key_order(es[j].0, es[m].0);
                    assert(es[j].0 == gkey(rows[j].1.sic as int, opt_bytes(rows[j].1.tlabel)));
                    assert(es[m].0 == gkey(rows[m].1.sic as int, opt_bytes(rows[m].1.tlabel)));
                }
                assert forall|sc: int, tl: Option<Seq<u8>>| p.p1 <= sc <= p.p2 && #[trigger] cell_at(g, p.p4, p.p3, sc, tl) is Some implies
                    exists|j: int| 0 <= j < rows.len() && rows[j].1.sic as int == sc && opt_bytes(rows[j].1.tlabel) == tl by {
                    let cell = cell_at(g, p.p4, p.p3, sc, tl)->0;
                    assert(ii.at(gkey(sc, tl)) == Some(cell));
                    let m = choose|m: int| 0 <= m < ii.entries().len() && ii.entries()[m].0 == gkey(sc, tl);
                    let e = ii.entries()[m];
                    assert(e == (gkey(sc, tl), cell));
                    assert(ii.entries().contains(e));
                    assert(e.0.0 == sc);
                    assert(exists|j: int| 0 <= j < rows.len() && es[j] == e);
                    let j = choose|j: int| 0 <= j < rows.len() && es[j] == e;
                    lemma_entries_at(rows@, j);
                    assert(rows[j].1.sic as int == sc) by {
                        assert(es[j].0 == gkey(rows[j].1.sic as int, opt_bytes(rows[j].1.tlabel)));
                        lemma_gkey_inj(sc, tl, rows[j].1.sic as int, opt_bytes(rows[j].1.tlabel));
                    }
                    lemma_gkey_inj(sc, tl, rows[j].1.sic as int, opt_bytes(rows[j].1.tlabel));
                }
            }
            rows
        },
        None => {
            proof {
                assert forall|sc: int, tl: Option<Seq<u8>>| #[trigger] cell_at(g, p.p4, p.p3, sc, tl) is None by {
                    assert(g.at(key2(p4@, p3@)).is_none());
                }
            }
            Vec::new()
        },
    };
    proof { assert(rows_cover(rows@, g, p)); }
    if !rows_safe(&rows, c, Ghost(db), Ghost(p), Ghost(g)) {
        proof { lemma_unsafe_unanswerable(db, p); }
        return None;
    }
    let cands = make_cands(&rows, c, Ghost(db), Ghost(p), Ghost(g));
    finish(cands, p3, c, Ghost(db), Ghost(p))
}

}
