// The answer rows: the groups whose rounded total exceeds the rounded average of the company totals,
// ordered by total and cut at 100, from a class's entries.
use vstd::prelude::*;
use std::sync::Arc;
use verus_common::rel::*;
use verus_common::sums::*;
use verus_prooflib::lemma_sum_ones;
use verus_prooflib::ordered::{Index, Key, KeyView, lemma_ordered_unique};
use verus_prooflib::wide::Wide;
use crate::choice::*;
use crate::ent::*;
use crate::f64bits::*;
use crate::fin::*;
use crate::gen_obligations::*;
use crate::gen_spec::*;
use crate::gkey::*;
use crate::numeric::*;
use crate::qty::*;
use crate::rows::*;

verus! {

pub type Out = (Option<Vec<u8>>, Option<i64>, Option<u64>);

pub open spec fn out_row(g: Ent) -> OutRow {
    OutRow { name: opt_bytes(g.name), cik: opt_int(g.cik), total_value: Some(g.cv@) }
}

pub open spec fn entry_fits(g: Ent) -> bool {
    g.bits is Some
}

pub open spec fn sorted_rows(cands: Seq<Ent>, sorted: Seq<usize>) -> Seq<OutRow> {
    sorted.map_values(|i: usize| out_row(cands[i as int]))
}

/// A candidate is a group of the outer block whose total is above the scalar.
pub open spec fn cand_of(os: Seq<Ent>, cvs: int, k: Ent) -> bool {
    exists|i: int| 0 <= i < os.len() && opt_bytes(os[i].name) == opt_bytes(k.name) && os[i].cik == k.cik
        && os[i].cv@ == k.cv@ && os[i].bits == k.bits && os[i].cv@ > cvs
}

pub open spec fn cands_ok(os: Seq<Ent>, cvs: int, cands: Seq<Ent>) -> bool {
    &&& forall|j: int| 0 <= j < cands.len() ==> cand_of(os, cvs, #[trigger] cands[j])
    &&& forall|j: int, m: int| 0 <= j < m < cands.len() ==>
        !(opt_bytes((#[trigger] cands[j]).name) == opt_bytes((#[trigger] cands[m]).name) && cands[j].cik == cands[m].cik)
    &&& forall|i: int| 0 <= i < os.len() && (#[trigger] os[i]).cv@ > cvs ==>
        exists|j: int| 0 <= j < cands.len() && opt_bytes((#[trigger] cands[j]).name) == opt_bytes(os[i].name) && cands[j].cik == os[i].cik
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

pub open spec fn ch_a_of(es: Seq<Ent>) -> spec_fn(Option<int>) -> int {
    |k: Option<int>| entry_cv(es, k)
}

pub open spec fn ch_1_of(os: Seq<Ent>) -> spec_fn(Option<Seq<u8>>, Option<int>) -> int {
    |gn: Option<Seq<u8>>, gc: Option<int>| ocv(os, gn, gc)
}

proof fn lemma_ch_a_entries(es: Seq<Ent>)
    requires forall|a: int, b: int| 0 <= a < b < es.len() ==> (#[trigger] es[a]).cik != (#[trigger] es[b]).cik,
    ensures forall|i: int| 0 <= i < es.len() ==> ch_a_of(es)(opt_int((#[trigger] es[i]).cik)) == es[i].cv@,
{
    assert forall|i: int| 0 <= i < es.len() implies ch_a_of(es)(opt_int((#[trigger] es[i]).cik)) == es[i].cv@ by {
        lemma_entry_cv(es, i);
    }
}

/// The scalar of the subquery is present when it has a row.
proof fn lemma_scalar_value(db: Db, p: Params, es: Seq<Ent>, cvs: int)
    requires ients_ok(db, p, es),
    ensures scalar_1_value(db, p, ch_a_of(es), cvs) == if es.len() > 0 { Some(cvs) } else { None::<int> },
{
    lemma_ch_a_entries(es);
    lemma_scalar(db, p, es, ch_a_of(es));
}

/// A candidate's row is a row of the answer.
proof fn lemma_cand_row(db: Db, p: Params, es: Seq<Ent>, os: Seq<Ent>, cvs: int, k: Ent)
    requires
        ients_ok(db, p, es),
        gents_ok(db, p, os),
        cand_of(os, cvs, k),
    ensures
        row_ok(db, p, ch_a_of(es), cvs, ch_1_of(os), out_row(k)),
{
    lemma_scalar_value(db, p, es, cvs);
    let i = choose|i: int| 0 <= i < os.len() && opt_bytes(os[i].name) == opt_bytes(k.name) && os[i].cik == k.cik
        && os[i].cv@ == k.cv@ && os[i].bits == k.bits && os[i].cv@ > cvs;
    let (gn, gc) = (opt_bytes(k.name), opt_int(k.cik));
    lemma_count_vs(db, p, gn, gc);
    assert(gent_ok(db, p, os[i]));
    assert(opt_int(os[i].cik) == gc);
    assert forall|a: int, b: int| 0 <= a < b < os.len() implies
        !(opt_bytes((#[trigger] os[a]).name) == opt_bytes((#[trigger] os[b]).name) && os[a].cik == os[b].cik) by {}
    lemma_ocv(os, i);
    lemma_group_company(db, p, es, gn, gc);
}

proof fn lemma_cand_order(cands: Seq<Ent>, sorted: Seq<usize>)
    requires
        cands_distinct(cands),
        sorted.no_duplicates(),
        forall|k: int| 0 <= k < sorted.len() ==> (#[trigger] sorted[k]) < cands.len(),
        forall|a: int, b: int| 0 <= a < b < sorted.len() ==> cands[sorted[a] as int].cv@ >= cands[sorted[b] as int].cv@,
    ensures
        forall|a: int, b: int| 0 <= a < b < sorted_rows(cands, sorted).len() ==>
            !same_group(#[trigger] sorted_rows(cands, sorted)[a], #[trigger] sorted_rows(cands, sorted)[b])
            && !row_lt(sorted_rows(cands, sorted)[b], sorted_rows(cands, sorted)[a]),
{
    let full = sorted_rows(cands, sorted);
    assert forall|a: int, b: int| 0 <= a < b < full.len() implies
        !same_group(#[trigger] full[a], #[trigger] full[b]) && !row_lt(full[b], full[a]) by {
        let (x, y) = (sorted[a] as int, sorted[b] as int);
        assert(full[a] == out_row(cands[x]));
        assert(full[b] == out_row(cands[y]));
        assert(x != y);
        if x < y { assert(!(opt_bytes(cands[x].name) == opt_bytes(cands[y].name) && cands[x].cik == cands[y].cik)); }
        else { assert(!(opt_bytes(cands[y].name) == opt_bytes(cands[x].name) && cands[y].cik == cands[x].cik)); }
        assert(cands[x].cv@ >= cands[y].cv@);
    }
}

pub open spec fn cands_distinct(cands: Seq<Ent>) -> bool {
    forall|j: int, m: int| 0 <= j < m < cands.len() ==>
        !(opt_bytes((#[trigger] cands[j]).name) == opt_bytes((#[trigger] cands[m]).name) && cands[j].cik == cands[m].cik)
}

#[verifier::rlimit(100)]
proof fn lemma_cover(db: Db, p: Params, es: Seq<Ent>, os: Seq<Ent>, cvs: int, cands: Seq<Ent>, sorted: Seq<usize>)
    requires
        gents_ok(db, p, os),
        cands.len() <= usize::MAX,
        cands_ok(os, cvs, cands),
        forall|index: usize| index < cands.len() ==> #[trigger] sorted.contains(index),
        forall|k: int| 0 <= k < sorted.len() ==> (#[trigger] sorted[k]) < cands.len(),
    ensures
        forall|gn: Option<Seq<u8>>, gc: Option<int>|
            #[trigger] group_count(db, p, gn, gc) > 0 && having(db, p, ch_a_of(es), cvs, ch_1_of(os), gn, gc)
                ==> exists|i: int| 0 <= i < sorted_rows(cands, sorted).len()
                    && (#[trigger] sorted_rows(cands, sorted)[i]).name == gn && sorted_rows(cands, sorted)[i].cik == gc,
{
    let full = sorted_rows(cands, sorted);
    assert forall|gn: Option<Seq<u8>>, gc: Option<int>|
        #[trigger] group_count(db, p, gn, gc) > 0 && having(db, p, ch_a_of(es), cvs, ch_1_of(os), gn, gc)
            implies exists|i: int| 0 <= i < full.len() && (#[trigger] full[i]).name == gn && full[i].cik == gc by {
        lemma_count_vs(db, p, gn, gc);
        let i = choose|i: int| 0 <= i < os.len() && opt_bytes((#[trigger] os[i]).name) == gn && opt_int(os[i].cik) == gc;
        assert forall|a: int, b: int| 0 <= a < b < os.len() implies
            !(opt_bytes((#[trigger] os[a]).name) == opt_bytes((#[trigger] os[b]).name) && os[a].cik == os[b].cik) by {}
        lemma_ocv(os, i);
        assert(os[i].cv@ > cvs);
        let j = choose|j: int| 0 <= j < cands.len() && opt_bytes((#[trigger] cands[j]).name) == opt_bytes(os[i].name) && cands[j].cik == os[i].cik;
        assert(j < cands.len() && 0 <= j);
        let ju = j as usize;
        assert(ju as int == j);
        assert(sorted.contains(ju));
        let pos = choose|pos: int| 0 <= pos < sorted.len() && sorted[pos] == ju;
        assert(full[pos] == out_row(cands[sorted[pos] as int]));
    }
}

proof fn lemma_rows_ok(db: Db, p: Params, es: Seq<Ent>, os: Seq<Ent>, cvs: int, cands: Seq<Ent>, sorted: Seq<usize>)
    requires
        ients_ok(db, p, es),
        gents_ok(db, p, os),
        cands_ok(os, cvs, cands),
        forall|k: int| 0 <= k < sorted.len() ==> (#[trigger] sorted[k]) < cands.len(),
    ensures
        forall|i: int| 0 <= i < sorted_rows(cands, sorted).len() ==>
            row_ok(db, p, ch_a_of(es), cvs, ch_1_of(os), #[trigger] sorted_rows(cands, sorted)[i]),
{
    let full = sorted_rows(cands, sorted);
    assert forall|i: int| 0 <= i < full.len() implies row_ok(db, p, ch_a_of(es), cvs, ch_1_of(os), #[trigger] full[i]) by {
        assert(full[i] == out_row(cands[sorted[i] as int]));
        lemma_cand_row(db, p, es, os, cvs, cands[sorted[i] as int]);
    }
}

/// The candidates, sorted by their total descending, are a full answer for the engine's choices.
proof fn lemma_full3(db: Db, p: Params, es: Seq<Ent>, os: Seq<Ent>,
    q: int, cvs: int, bits_s: Option<u64>, cands: Seq<Ent>, sorted: Seq<usize>)
    requires
        ients_ok(db, p, es),
        gents_ok(db, p, os),
        cands.len() <= usize::MAX,
        es.len() >= 1 ==> 2 * abs(q * es.len() as int - sum(es, |e: Ent| e.cv@)) <= es.len() as int,
        es.len() >= 1 ==> abs(q) <= abs(sum(es, |e: Ent| e.cv@)),
        es.len() >= 1 ==> rounded_as(q, cvs, bits_s),
        cands_ok(os, cvs, cands),
        sorted.no_duplicates(),
        forall|index: usize| index < cands.len() ==> #[trigger] sorted.contains(index),
        forall|k: int| 0 <= k < sorted.len() ==> (#[trigger] sorted[k]) < cands.len(),
        forall|a: int, b: int| 0 <= a < b < sorted.len() ==> cands[sorted[a] as int].cv@ >= cands[sorted[b] as int].cv@,
    ensures
        full_result(db, p, sorted_rows(cands, sorted)),
{
    lemma_chosen(db, p, es, os, q, cvs, bits_s);
    lemma_rows_ok(db, p, es, os, cvs, cands, sorted);
    lemma_cand_order(cands, sorted);
    lemma_cover(db, p, es, os, cvs, cands, sorted);
    let full = sorted_rows(cands, sorted);
    assert(cands_distinct(cands));
    assert(chosen(db, p, ch_a_of(es), cvs, ch_1_of(os)));
    assert(full_result_with(db, p, ch_a_of(es), cvs, ch_1_of(os), full));
}

/// A group belongs to a company.
proof fn lemma_group_company(db: Db, p: Params, es: Seq<Ent>, gn: Option<Seq<u8>>, gc: Option<int>)
    requires
        ients_ok(db, p, es),
        group_sum(db, p, gn, gc, tup_vs()) > 0,
    ensures
        es.len() > 0,
{
    lemma_outer_join(db, p, gn, gc);
    lemma_group_le_company(db, p, gn, gc);
    lemma_inner_join(db, p, gc);
    assert(avg_sub_group_sum(db, p, gc, avg_vs()) > 0);
    let k = choose|k: int| 0 <= k < es.len() && opt_int((#[trigger] es[k]).cik) == gc;
}

/// The candidate's value and its bits agree.
pub open spec fn cand_sane(k: Ent) -> bool {
    match k.bits {
        Some(b) => f64_in_range(k.cv@) && k.cv@ == f64_val(b),
        None => !f64_in_range(k.cv@),
    }
}

proof fn lemma_cand_sane(os: Seq<Ent>, cvs: int, k: Ent)
    requires gentries_ok_entries(os), cand_of(os, cvs, k),
    ensures cand_sane(k),
{
    let i = choose|i: int| 0 <= i < os.len() && opt_bytes(os[i].name) == opt_bytes(k.name) && os[i].cik == k.cik
        && os[i].cv@ == k.cv@ && os[i].bits == k.bits && os[i].cv@ > cvs;
    if let Some(b) = k.bits { lemma_f64_in_range(b); }
}

/// Every entry's value is its bits' (the part of gentry_ok that cand_sane needs).
pub open spec fn gentries_ok_entries(os: Seq<Ent>) -> bool {
    forall|i: int| 0 <= i < os.len() ==> rounded_as((#[trigger] os[i]).sum@, os[i].cv@, os[i].bits)
}


pub proof fn lemma_gents_rounded(db: Db, p: Params, os: Seq<Ent>)
    requires gents_ok(db, p, os),
    ensures gentries_ok_entries(os),
{
    assert forall|i: int| 0 <= i < os.len() implies rounded_as((#[trigger] os[i]).sum@, os[i].cv@, os[i].bits) by {
        assert(gent_ok(db, p, os[i]));
    }
}


/// The groups whose rounded total is above the scalar.
pub fn cands_of_rows(rows: &Vec<(Key, Arc<Ent>)>, cvs: &Wide, Ghost(os): Ghost<Seq<Ent>>) -> (cands: Vec<Ent>)
    requires
        os == rows@.map_values(|row: (Key, Arc<Ent>)| *row.1),
        forall|a: int, b: int| 0 <= a < b < os.len() ==>
            !(opt_bytes((#[trigger] os[a]).name) == opt_bytes((#[trigger] os[b]).name) && os[a].cik == os[b].cik),
    ensures cands_ok(os, cvs@, cands@),
{
    let mut cands: Vec<Ent> = Vec::new();
    let ghost mut src: Seq<int> = Seq::empty();
    let mut i: usize = 0;
    while i < rows.len()
        invariant
            i <= rows.len(),
            os == rows@.map_values(|row: (Key, Arc<Ent>)| *row.1),
            src.len() == cands.len(),
            forall|j: int| 0 <= j < cands.len() ==> cand_of(os, cvs@, #[trigger] cands[j]),
            forall|j: int| 0 <= j < cands.len() ==> 0 <= #[trigger] src[j] < i
                && opt_bytes(cands[j].name) == opt_bytes(os[src[j]].name) && cands[j].cik == os[src[j]].cik,
            forall|j: int, m: int| 0 <= j < m < cands.len() ==> src[j] < src[m],
            forall|m: int| 0 <= m < i && os[m].cv@ > cvs@ ==> exists|j: int| 0 <= j < cands.len() && src[j] == m,
            forall|a: int, b: int| 0 <= a < b < os.len() ==>
                !(opt_bytes((#[trigger] os[a]).name) == opt_bytes((#[trigger] os[b]).name) && os[a].cik == os[b].cik),
        decreases rows.len() - i,
    {
        let g = &*rows[i].1;
        proof { assert(os[i as int] == *g); }
        let ghost old_len = cands.len();
        if cvs.lt(&g.cv) {
            let k = Ent {
                name: copy_name(&g.name),
                cik: g.cik, cv: g.cv.dup(), bits: g.bits, count: g.count.dup(), sum: g.sum.dup(), abs: g.abs.dup(),
            };
            cands.push(k);
            proof {
                let src_old = src;
                src = src.push(i as int);
                assert(cand_of(os, cvs@, cands[old_len as int]));
                assert forall|m: int| 0 <= m < i + 1 && os[m].cv@ > cvs@ implies exists|j: int| 0 <= j < cands.len() && src[j] == m by {
                    if m == i as int { assert(src[old_len as int] == m); }
                    else {
                        let j = choose|j: int| 0 <= j < old_len && src_old[j] == m;
                        assert(src[j] == src_old[j]);
                    }
                }
            }
        } else {
            proof {
                assert forall|m: int| 0 <= m < i + 1 && os[m].cv@ > cvs@ implies exists|j: int| 0 <= j < cands.len() && src[j] == m by {
                    if m == i as int { assert(false); }
                    assert(exists|j: int| 0 <= j < cands.len() && src[j] == m);
                }
            }
        }
        i += 1;
    }
    proof {
        assert forall|j: int, m: int| 0 <= j < m < cands.len() implies
            !(opt_bytes((#[trigger] cands[j]).name) == opt_bytes((#[trigger] cands[m]).name) && cands[j].cik == cands[m].cik) by {
            assert(src[j] < src[m]);
            assert(!(opt_bytes(os[src[j]].name) == opt_bytes(os[src[m]].name) && os[src[j]].cik == os[src[m]].cik));
        }
        assert forall|i2: int| 0 <= i2 < os.len() && (#[trigger] os[i2]).cv@ > cvs@ implies
            exists|j: int| 0 <= j < cands.len() && opt_bytes((#[trigger] cands[j]).name) == opt_bytes(os[i2].name) && cands[j].cik == os[i2].cik by {
            let j = choose|j: int| 0 <= j < cands.len() && src[j] == i2;
            assert(opt_bytes(cands[j].name) == opt_bytes(os[i2].name) && cands[j].cik == os[i2].cik);
        }
    }
    cands
}



/// The answer of a class: the first 100 groups above the scalar in order of their totals, or None when one of
/// them has a total that is no double.
#[verifier::rlimit(200)]
pub fn finish(cls: &Class, c: &Consts, Ghost(db): Ghost<Db>, Ghost(p): Ghost<Params>)
    -> (r: Option<Vec<Out>>)
    requires
        cls_ok(*cls, db, p),
        nums_dbl(db),
        consts_ok(*c),
    ensures
        r matches Some(out) ==> query_result(db, p, out_view(out@)),
        r matches Some(out) ==> forall|i: int| 0 <= i < out.len() ==> out_fits(#[trigger] out_view(out@)[i]),
        r is None ==> !answer_fits(db, p),
{
    let ghost es = ents_of(cls.cmap);
    let ghost os = ents_of(cls.omap);
    proof {
        lemma_ients_of(db, p, cls.cmap);
        lemma_gents_of(db, p, cls.omap);
        lemma_sum_ones(cls.cmap.entries());
        lemma_sum_map(cls.cmap.entries(), |row: (KeyView, Ent)| row.1, |e: Ent| e.cv@, cvw());
        assert(cls.k@ == es.len() as int);
        assert(cls.sr@ == sum(es, |e: Ent| e.cv@));
    }
    // the scalar: the average of the companies' totals, rounded
    let (q, cvs, bits_s) = if cls.k.is_pos() {
        let q = div_round(&cls.sr, &cls.k);
        let (cvs, b) = round_value(&q, c);
        (q, cvs, b)
    } else {
        (Wide::zero(), Wide::zero(), None)
    };
    proof {
        if es.len() > 0 { lemma_abs_sum_cv(es); }
    }
    let rows = cls.omap.rows();
    proof {
        cls.omap.model();
        lemma_ordered_unique(cls.omap.entries());
        assert(os == rows@.map_values(|row: (Key, Arc<Ent>)| *row.1)) by {
            assert(rows@.map_values(|row: (Key, Arc<Ent>)| (row.0.view(), *row.1)) == cls.omap.entries());
            assert(os =~= cls.omap.entries().map_values(|row: (KeyView, Ent)| row.1));
            assert(rows@.map_values(|row: (Key, Arc<Ent>)| *row.1) =~= cls.omap.entries().map_values(|row: (KeyView, Ent)| row.1));
        }
        assert(gents_ok(db, p, os));
    }
    let cands = cands_of_rows(&rows, &cvs, Ghost(os));
    proof {
        lemma_gents_rounded(db, p, os);
        assert forall|j: int| 0 <= j < cands.len() implies cand_sane(#[trigger] cands@[j]) by {
            lemma_cand_sane(os, cvs@, cands@[j]);
        }
    }
    let n = cands.len();
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
    let ghost le = |a: usize, b: usize| cands@[a as int].cv@ >= cands@[b as int].cv@;
    let cands_ref = &cands;
    let compare = |a: usize, b: usize| -> (result: bool)
        requires a < cands_ref.len(), b < cands_ref.len(),
        ensures result == (cands_ref@[a as int].cv@ >= cands_ref@[b as int].cv@),
    { !cands_ref[a].cv.lt(&cands_ref[b].cv) };
    proof {
        assert forall|a: usize, b: usize, cc: usize| valid(a) && valid(b) && valid(cc)
            && #[trigger] le(a, b) && #[trigger] le(b, cc) implies le(a, cc) by {}
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
            cands@[sorted@[a] as int].cv@ >= cands@[sorted@[b] as int].cv@ by {
            assert(le(sorted@[a], sorted@[b]));
        }
        lemma_full3(db, p, es, os, q@, cvs@, bits_s, cands@, sorted@);
    }
    let ghost full = sorted_rows(cands@, sorted@);
    let ghost limited = if full.len() <= 100 { full } else { full.take(100) };
    proof { assert(query_result(db, p, limited)); }
    let m = if sorted.len() < 100 { sorted.len() } else { 100 };
    let mut out: Vec<Out> = Vec::new();
    let mut k: usize = 0;
    proof {
        assert(limited.len() == m);
        assert forall|j: int| 0 <= j < m implies limited[j] == out_row(cands@[sorted@[j] as int]) by {
            assert(limited[j] == full[j]);
        }
    }
    while k < m
        invariant
            k <= m, m <= sorted.len(), m <= 100, m == limited.len(),
            sorted.len() == cands.len(),
            forall|j: int| 0 <= j < cands.len() ==> cand_sane(#[trigger] cands@[j]),
            forall|j: int| 0 <= j < sorted.len() ==> (#[trigger] sorted@[j]) < cands.len(),
            forall|j: int| 0 <= j < m ==> limited[j] == out_row(cands@[sorted@[j] as int]),
            out.len() == k,
            forall|j: int| 0 <= j < k ==> out_view(out@)[j] == limited[j] && out_fits(limited[j]),
            query_result(db, p, limited),
            out_view(out@).len() == k,
        decreases m - k,
    {
        let cand = &cands[sorted[k]];
        proof {
            assert(limited[k as int] == out_row(*cand));
            assert(cands@[sorted@[k as int] as int] == *cand);
            let idx = sorted@[k as int] as int;
            assert(0 <= idx < cands.len());
            assert(cand_sane(cands@[idx]));
        }
        if cand.bits.is_none() {
            proof {
                assert(!out_fits(limited[k as int]));
                assert(query_result(db, p, limited));
                assert(!answer_fits(db, p));
            }
            return None;
        }
        let row: Out = (
            match &cand.name { Some(b) => Some(copy_bytes(b.as_slice())), None => None },
            cand.cik,
            cand.bits,
        );
        proof {
            assert(out_fits(out_row(*cand)));
            let ov = out_view(out@);
            assert(out_view(out@.push(row)) =~= ov.push(out_row(*cand))) by {
                assert(out_view(out@.push(row)).len() == k + 1);
                assert forall|j: int| 0 <= j < k + 1 implies out_view(out@.push(row))[j] == ov.push(out_row(*cand))[j] by {
                    if j == k as int {
                        assert(out@.push(row)[j] == row);
                        assert(opt_bytes(cand.name) == (match row.0 { Some(v) => Some(v@), None => None }));
                        assert(out_view(out@.push(row))[j] == out_row(*cand));
                    } else {
                        assert(out@.push(row)[j] == out@[j]);
                    }
                }
            }
        }
        out.push(row);
        k += 1;
    }
    proof { assert(out_view(out@) =~= limited); }
    Some(out)
}

}
