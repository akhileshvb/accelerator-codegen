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
use crate::rank::*;
use verus_prooflib::ordered::{key_lt, lemma_key_order};
use crate::rows::*;

verus! {

pub type Out = (Option<Vec<u8>>, Option<i64>, Option<u64>);

pub open spec fn out_row(g: Ent) -> OutRow {
    OutRow { name: opt_bytes(g.name), cik: opt_int(g.cik), total_value: Some(g.cv@) }
}

pub open spec fn entry_fits(g: Ent) -> bool {
    g.bits is Some
}

pub open spec fn sorted_rows(cands: Seq<Ent>, sorted: Seq<int>) -> Seq<OutRow> {
    sorted.map_values(|i: int| out_row(cands[i]))
}

pub open spec fn sorted_int(s: Seq<usize>) -> Seq<int> {
    s.map_values(|x: usize| x as int)
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

proof fn lemma_cand_order(cands: Seq<Ent>, sorted: Seq<int>)
    requires
        cands_distinct(cands),
        sorted.no_duplicates(),
        forall|k: int| 0 <= k < sorted.len() ==> 0 <= (#[trigger] sorted[k]) < cands.len(),
        forall|a: int, b: int| 0 <= a < b < sorted.len() ==> cands[sorted[a]].cv@ >= cands[sorted[b]].cv@,
    ensures
        forall|a: int, b: int| 0 <= a < b < sorted_rows(cands, sorted).len() ==>
            !same_group(#[trigger] sorted_rows(cands, sorted)[a], #[trigger] sorted_rows(cands, sorted)[b])
            && !row_lt(sorted_rows(cands, sorted)[b], sorted_rows(cands, sorted)[a]),
{
    let full = sorted_rows(cands, sorted);
    assert forall|a: int, b: int| 0 <= a < b < full.len() implies
        !same_group(#[trigger] full[a], #[trigger] full[b]) && !row_lt(full[b], full[a]) by {
        let (x, y) = (sorted[a], sorted[b]);
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
proof fn lemma_cover(db: Db, p: Params, es: Seq<Ent>, os: Seq<Ent>, cvs: int, cands: Seq<Ent>, sorted: Seq<int>)
    requires
        gents_ok(db, p, os),
        cands_ok(os, cvs, cands),
        forall|index: int| 0 <= index < cands.len() ==> #[trigger] sorted.contains(index),
        forall|k: int| 0 <= k < sorted.len() ==> 0 <= (#[trigger] sorted[k]) < cands.len(),
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
        assert(sorted.contains(j));
        let pos = choose|pos: int| 0 <= pos < sorted.len() && sorted[pos] == j;
        assert(full[pos] == out_row(cands[sorted[pos]]));
    }
}

proof fn lemma_rows_ok(db: Db, p: Params, es: Seq<Ent>, os: Seq<Ent>, cvs: int, cands: Seq<Ent>, sorted: Seq<int>)
    requires
        ients_ok(db, p, es),
        gents_ok(db, p, os),
        cands_ok(os, cvs, cands),
        forall|k: int| 0 <= k < sorted.len() ==> 0 <= (#[trigger] sorted[k]) < cands.len(),
    ensures
        forall|i: int| 0 <= i < sorted_rows(cands, sorted).len() ==>
            row_ok(db, p, ch_a_of(es), cvs, ch_1_of(os), #[trigger] sorted_rows(cands, sorted)[i]),
{
    let full = sorted_rows(cands, sorted);
    assert forall|i: int| 0 <= i < full.len() implies row_ok(db, p, ch_a_of(es), cvs, ch_1_of(os), #[trigger] full[i]) by {
        assert(full[i] == out_row(cands[sorted[i]]));
        lemma_cand_row(db, p, es, os, cvs, cands[sorted[i]]);
    }
}

/// The candidates, sorted by their total descending, are a full answer for the engine's choices.
pub proof fn lemma_full3(db: Db, p: Params, es: Seq<Ent>, os: Seq<Ent>,
    q: int, cvs: int, bits_s: Option<u64>, cands: Seq<Ent>, sorted: Seq<int>)
    requires
        ients_ok(db, p, es),
        gents_ok(db, p, os),
        es.len() >= 1 ==> 2 * abs(q * es.len() as int - sum(es, |e: Ent| e.cv@)) <= es.len() as int,
        es.len() >= 1 ==> abs(q) <= abs(sum(es, |e: Ent| e.cv@)),
        es.len() >= 1 ==> rounded_as(q, cvs, bits_s),
        cands_ok(os, cvs, cands),
        sorted.no_duplicates(),
        forall|index: int| 0 <= index < cands.len() ==> #[trigger] sorted.contains(index),
        forall|k: int| 0 <= k < sorted.len() ==> 0 <= (#[trigger] sorted[k]) < cands.len(),
        forall|a: int, b: int| 0 <= a < b < sorted.len() ==> cands[sorted[a]].cv@ >= cands[sorted[b]].cv@,
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
pub fn finish_scan(cls: &Class, c: &Consts, Ghost(db): Ghost<Db>, Ghost(p): Ghost<Params>)
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
        let si = sorted_int(sorted@);
        assert forall|k: int| 0 <= k < si.len() implies 0 <= (#[trigger] si[k]) < cands.len() by { assert(sorted@[k] < cands.len()); }
        assert forall|a: int, b: int| 0 <= a < b < si.len() implies cands@[si[a]].cv@ >= cands@[si[b]].cv@ by {
            assert(cands@[sorted@[a] as int].cv@ >= cands@[sorted@[b] as int].cv@);
        }
        assert(si.no_duplicates()) by {
            assert forall|a: int, b: int| 0 <= a < b < si.len() implies si[a] != si[b] by { assert(sorted@[a] != sorted@[b]); }
        }
        assert forall|index: int| 0 <= index < cands.len() implies #[trigger] si.contains(index) by {
            assert(sorted@.contains(index as usize));
            let pos = choose|pos: int| 0 <= pos < sorted@.len() && sorted@[pos] == index as usize;
            assert(si[pos] == index);
        }
        lemma_full3(db, p, es, os, q@, cvs@, bits_s, cands@, si);
    }
    let ghost full = sorted_rows(cands@, sorted_int(sorted@));
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


// ── the read through the order index ──

/// The number of leading entries above the scalar.
pub open spec fn cut(eo: Seq<Ent>, cvs: int) -> int
    decreases eo.len(),
{
    if eo.len() == 0 { 0 } else if eo[0].cv@ <= cvs { 0 } else { 1 + cut(eo.skip(1), cvs) }
}

proof fn lemma_cut(eo: Seq<Ent>, cvs: int)
    ensures
        0 <= cut(eo, cvs) <= eo.len(),
        forall|i: int| 0 <= i < cut(eo, cvs) ==> (#[trigger] eo[i]).cv@ > cvs,
        cut(eo, cvs) < eo.len() ==> eo[cut(eo, cvs)].cv@ <= cvs,
    decreases eo.len(),
{
    if eo.len() > 0 && eo[0].cv@ > cvs {
        lemma_cut(eo.skip(1), cvs);
        assert forall|i: int| 0 <= i < cut(eo, cvs) implies (#[trigger] eo[i]).cv@ > cvs by {
            if i > 0 { assert(eo.skip(1)[i - 1] == eo[i]); }
        }
        if cut(eo, cvs) < eo.len() { assert(eo.skip(1)[cut(eo, cvs) - 1] == eo[cut(eo, cvs)]); }
    }
}

/// The entries of the order index in key order: every group once, each with the bits of its total, totals not increasing.
proof fn lemma_ord_entries(db: Db, p: Params, cls: Class)
    requires cls_ok(cls, db, p), cls.oor@ == 0,
    ensures ({
        let eo = ents_of(cls.ord);
        &&& forall|i: int| 0 <= i < eo.len() ==> (#[trigger] eo[i]).bits is Some && f64_finite(eo[i].bits->0) && eo[i].cv@ == f64_val(eo[i].bits->0)
        &&& forall|i: int, j: int| 0 <= i < j < eo.len() ==> (#[trigger] eo[i]).cv@ >= (#[trigger] eo[j]).cv@
        &&& forall|i: int| 0 <= i < eo.len() ==> cls.omap.at(gk(opt_bytes((#[trigger] eo[i]).name), opt_int(eo[i].cik))) == Some(eo[i])
        &&& forall|i: int, j: int| 0 <= i < j < eo.len() ==>
            !(opt_bytes((#[trigger] eo[i]).name) == opt_bytes((#[trigger] eo[j]).name) && eo[i].cik == eo[j].cik)
    }),
{
    let ents = cls.ord.entries();
    let eo = ents_of(cls.ord);
    cls.ord.model();
    cls.omap.model();
    // no entry of the map has an out-of-range total
    let oe = cls.omap.entries();
    assert forall|i: int| 0 <= i < oe.len() implies (#[trigger] oorw()(oe[i])) >= 0 by {}
    assert forall|i: int| 0 <= i < eo.len() implies (#[trigger] eo[i]).bits is Some by {
        let x = eo[i];
        assert(ents[i].1 == x);
        assert(cls.ord.at(ents[i].0) == Some(x));
        let g = gk(opt_bytes(x.name), opt_int(x.cik));
        assert(cls.omap.at(g) == Some(x));
        let pos = choose|pos: int| 0 <= pos < oe.len() && oe[pos].0 == g;
        assert(cls.omap.at(oe[pos].0) == Some(oe[pos].1));
        assert(oe[pos].1 == x);
        if x.bits is None { lemma_sum_pos(oe, oorw(), pos); }
    }
    assert forall|i: int| 0 <= i < eo.len() implies ent_ok_of(cls, #[trigger] eo[i]) by {
        let x = eo[i];
        assert(ents[i].1 == x);
        let g = gk(opt_bytes(x.name), opt_int(x.cik));
        assert(cls.ord.at(ents[i].0) == Some(x));
        assert(cls.omap.at(g) == Some(x));
        let gn = opt_bytes(x.name);
        let gc = opt_int(x.cik);
        assert(ek(false, gn, gc) == g);
    }
    assert forall|i: int| 0 <= i < eo.len() implies (#[trigger] eo[i]).bits is Some && f64_finite(eo[i].bits->0) && eo[i].cv@ == f64_val(eo[i].bits->0) by {}
    assert forall|i: int| 0 <= i < eo.len() implies cls.omap.at(gk(opt_bytes((#[trigger] eo[i]).name), opt_int(eo[i].cik))) == Some(eo[i]) by {
        assert(ents[i].1 == eo[i]);
        assert(cls.ord.at(ents[i].0) == Some(eo[i]));
    }
    assert forall|i: int, j: int| 0 <= i < j < eo.len() implies (#[trigger] eo[i]).cv@ >= (#[trigger] eo[j]).cv@ by {
        assert(ents[i].1 == eo[i] && ents[j].1 == eo[j]);
        assert(cls.ord.at(ents[i].0) == Some(eo[i]) && cls.ord.at(ents[j].0) == Some(eo[j]));
        assert(key_lt(ents[i].0, ents[j].0));
        assert(ents[i].0 == okey(eo[i]) && ents[j].0 == okey(eo[j]));
        if eo[i].cv@ < eo[j].cv@ {
            lemma_rank_order(eo[j].bits->0, eo[i].bits->0);
            assert(key_lt(okey(eo[j]), okey(eo[i])));
            lemma_key_order(okey(eo[i]), okey(eo[j]));
        }
    }
    assert forall|i: int, j: int| 0 <= i < j < eo.len() implies
        !(opt_bytes((#[trigger] eo[i]).name) == opt_bytes((#[trigger] eo[j]).name) && eo[i].cik == eo[j].cik) by {
        assert(ents[i].0 != ents[j].0) by { lemma_ordered_unique(ents); }
        assert(ents[i].0 == okey(eo[i]) && ents[j].0 == okey(eo[j]));
        assert(ents[i].1 == eo[i] && ents[j].1 == eo[j]);
        assert(cls.ord.at(ents[i].0) == Some(eo[i]) && cls.ord.at(ents[j].0) == Some(eo[j]));
        if opt_bytes(eo[i].name) == opt_bytes(eo[j].name) && eo[i].cik == eo[j].cik {
            assert(false) by { lemma_ordered_unique(ents); }
        }
    }
}

/// The entry is one of the map's, with its rounding facts.
pub open spec fn ent_ok_of(cls: Class, x: Ent) -> bool {
    rounded_as(x.sum@, x.cv@, x.bits)
}


pub open spec fn rows_of_ents(es: Seq<Ent>) -> Seq<OutRow> {
    es.map_values(|e: Ent| out_row(e))
}

/// The leading entries of the order index above the scalar are candidates, distinct, and all of them.
proof fn lemma_cands_prefix(omap: Index<Ent>, ord: Index<Ent>, cvs: int)
    requires
        omap.wf(),
        ord_ok(omap, ord),
        forall|k: KeyView| #[trigger] omap.at(k) is Some ==> k == gk(opt_bytes(omap.at(k)->0.name), opt_int(omap.at(k)->0.cik)),
        ({ let eo = ents_of(ord);
           &&& forall|i: int, j: int| 0 <= i < j < eo.len() ==> (#[trigger] eo[i]).cv@ >= (#[trigger] eo[j]).cv@
           &&& forall|i: int, j: int| 0 <= i < j < eo.len() ==>
               !(opt_bytes((#[trigger] eo[i]).name) == opt_bytes((#[trigger] eo[j]).name) && eo[i].cik == eo[j].cik) }),
    ensures
        cands_ok(ents_of(omap), cvs, ents_of(ord).take(cut(ents_of(ord), cvs))),
{
    let eo = ents_of(ord);
    let os = ents_of(omap);
    lemma_cut(eo, cvs);
    let t = cut(eo, cvs);
    let cands = eo.take(t);
    let ents = ord.entries();
    let oe = omap.entries();
    ord.model();
    omap.model();
    assert forall|j: int| 0 <= j < cands.len() implies cand_of(os, cvs, #[trigger] cands[j]) by {
        let x = cands[j];
        assert(x == eo[j]);
        assert(ents[j].1 == x);
        assert(ord.at(ents[j].0) == Some(x));
        let g = gk(opt_bytes(x.name), opt_int(x.cik));
        assert(omap.at(g) == Some(x));
        let pos = choose|pos: int| 0 <= pos < oe.len() && oe[pos].0 == g;
        assert(omap.at(oe[pos].0) == Some(oe[pos].1));
        assert(os[pos] == x);
    }
    assert forall|a: int, b: int| 0 <= a < b < cands.len() implies
        !(opt_bytes((#[trigger] cands[a]).name) == opt_bytes((#[trigger] cands[b]).name) && cands[a].cik == cands[b].cik) by {
        assert(cands[a] == eo[a] && cands[b] == eo[b]);
    }
    assert forall|i: int| 0 <= i < os.len() && (#[trigger] os[i]).cv@ > cvs implies
        exists|j: int| 0 <= j < cands.len() && opt_bytes((#[trigger] cands[j]).name) == opt_bytes(os[i].name) && cands[j].cik == os[i].cik by {
        let x = os[i];
        assert(oe[i].1 == x);
        assert(omap.at(oe[i].0) == Some(x));
        assert(oe[i].0 == gk(opt_bytes(x.name), opt_int(x.cik)));
        let pp = choose|pp: int| 0 <= pp < ents.len() && ents[pp].0 == okey(x);
        assert(ord.at(ents[pp].0) == Some(ents[pp].1));
        assert(ord.at(okey(x)) == Some(x));
        assert(eo[pp] == x);
        if pp >= t {
            assert(t < eo.len());
            if pp > t { assert(eo[t].cv@ >= eo[pp].cv@); }
            assert(false);
        }
        assert(cands[pp] == x);
    }
}

proof fn lemma_identity_sorted(cands: Seq<Ent>)
    requires forall|a: int, b: int| 0 <= a < b < cands.len() ==> (#[trigger] cands[a]).cv@ >= (#[trigger] cands[b]).cv@,
    ensures ({
        let id = Seq::new(cands.len(), |i: int| i);
        &&& id.no_duplicates()
        &&& forall|index: int| 0 <= index < cands.len() ==> #[trigger] id.contains(index)
        &&& forall|k: int| 0 <= k < id.len() ==> (#[trigger] id[k]) < cands.len()
        &&& forall|a: int, b: int| 0 <= a < b < id.len() ==> cands[id[a]].cv@ >= cands[id[b]].cv@
        &&& sorted_rows(cands, id) =~= rows_of_ents(cands)
    }),
{
    let id = Seq::new(cands.len(), |i: int| i);
    assert forall|a: int, b: int| 0 <= a < b < id.len() implies id[a] != id[b] by {}
    assert forall|index: int| 0 <= index < cands.len() implies #[trigger] id.contains(index) by {
        assert(id[index] == index);
    }
}

/// The leading entries above the scalar, in the order index's order, are a full answer.
#[verifier::rlimit(100)]
pub proof fn lemma_fast_full(db: Db, p: Params, cls: Class, q: int, cvs: int, bits_s: Option<u64>)
    requires
        cls_ok(cls, db, p),
        nums_dbl(db),
        cls.oor@ == 0,
        ({ let es = ents_of(cls.cmap);
           &&& es.len() >= 1 ==> 2 * abs(q * es.len() as int - sum(es, |e: Ent| e.cv@)) <= es.len() as int
           &&& es.len() >= 1 ==> abs(q) <= abs(sum(es, |e: Ent| e.cv@))
           &&& es.len() >= 1 ==> rounded_as(q, cvs, bits_s) }),
    ensures
        full_result(db, p, rows_of_ents(ents_of(cls.ord).take(cut(ents_of(cls.ord), cvs)))),
{
    let eo = ents_of(cls.ord);
    let es = ents_of(cls.cmap);
    let os = ents_of(cls.omap);
    lemma_ients_of(db, p, cls.cmap);
    lemma_gents_of(db, p, cls.omap);
    lemma_ord_entries(db, p, cls);
    lemma_cut(eo, cvs);
    let t = cut(eo, cvs);
    let cands = eo.take(t);
    cls.omap.map_model();
    assert(forall|k: KeyView| #[trigger] cls.omap.at(k) is Some ==> k == gk(opt_bytes(cls.omap.at(k)->0.name), opt_int(cls.omap.at(k)->0.cik)));
    lemma_cands_prefix(cls.omap, cls.ord, cvs);
    assert forall|a: int, b: int| 0 <= a < b < cands.len() implies (#[trigger] cands[a]).cv@ >= (#[trigger] cands[b]).cv@ by {
        assert(cands[a] == eo[a] && cands[b] == eo[b]);
    }
    lemma_identity_sorted(cands);
    let id = Seq::new(cands.len(), |i: int| i);
    lemma_full3(db, p, es, os, q, cvs, bits_s, cands, id);
}


/// The answer of a class whose totals are all doubles: the order index walked from the top.
#[verifier::rlimit(100)]
pub fn finish_fast(cls: &Class, c: &Consts, Ghost(db): Ghost<Db>, Ghost(p): Ghost<Params>) -> (r: Option<Vec<Out>>)
    requires
        cls_ok(*cls, db, p),
        nums_dbl(db),
        consts_ok(*c),
        cls.oor@ == 0,
    ensures
        r matches Some(out) ==> query_result(db, p, out_view(out@)),
        r matches Some(out) ==> forall|i: int| 0 <= i < out.len() ==> out_fits(#[trigger] out_view(out@)[i]),
        r is Some,
{
    let ghost es = ents_of(cls.cmap);
    proof {
        lemma_ients_of(db, p, cls.cmap);
        lemma_sum_ones(cls.cmap.entries());
        lemma_sum_map(cls.cmap.entries(), |row: (KeyView, Ent)| row.1, |e: Ent| e.cv@, cvw());
        assert(cls.k@ == es.len() as int);
        assert(cls.sr@ == sum(es, |e: Ent| e.cv@));
    }
    let (q, cvs, bits_s) = if cls.k.is_pos() {
        let q = div_round(&cls.sr, &cls.k);
        let (cvs, b) = round_value(&q, c);
        (q, cvs, b)
    } else {
        (Wide::zero(), Wide::zero(), None)
    };
    let ghost eo = ents_of(cls.ord);
    let ghost ents = cls.ord.entries();
    proof {
        if es.len() > 0 { lemma_abs_sum_cv(es); }
        lemma_ord_entries(db, p, *cls);
        lemma_cut(eo, cvs@);
        lemma_fast_full(db, p, *cls, q@, cvs@, bits_s);
        cls.ord.model();
    }
    let ghost full = rows_of_ents(eo.take(cut(eo, cvs@)));
    let mut cur = cls.ord.cursor();
    let mut out: Vec<Out> = Vec::new();
    let mut j: usize = 0;
    let mut stop = false;
    proof { assert(eo.take(0) =~= Seq::<Ent>::empty()); }
    while !stop && j < 100
        invariant
            j <= 100,
            out.len() == j,
            eo == ents_of(cls.ord),
            ents == cls.ord.entries(),
            j <= ents.len(),
            !stop ==> cur.remaining() == ents.skip(j as int),
            forall|i: int| 0 <= i < j ==> (#[trigger] eo[i]).cv@ > cvs@,
            out_view(out@) =~= rows_of_ents(eo.take(j as int)),
            stop ==> (j == eo.len() || eo[j as int].cv@ <= cvs@),
            forall|i: int| 0 <= i < eo.len() ==> (#[trigger] eo[i]).bits is Some && f64_finite(eo[i].bits->0) && eo[i].cv@ == f64_val(eo[i].bits->0),
        decreases 100 - j + if stop { 0int } else { 1int },
    {
        match cur.next() {
            None => {
                proof { assert(ents.skip(j as int).len() == 0); assert(j == eo.len()); }
                stop = true;
            },
            Some((k, e)) => {
                proof { assert((k.view(), *e) == ents.skip(j as int)[0]); assert(ents[j as int] == (k.view(), *e)); assert(eo[j as int] == *e); }
                if cvs.lt(&e.cv) {
                    let b = match e.bits { Some(b) => b, None => { proof { assert(false); } 0u64 } };
                    let row: Out = (copy_name(&e.name), e.cik, Some(b));
                    proof {
                        assert(eo.take(j as int + 1) =~= eo.take(j as int).push(eo[j as int]));
                        assert(out_view(out@.push(row)) =~= out_view(out@).push(out_row(eo[j as int])));
                        assert(rows_of_ents(eo.take(j as int + 1)) =~= rows_of_ents(eo.take(j as int)).push(out_row(eo[j as int])));
                    }
                    out.push(row);
                    j += 1;
                } else {
                    stop = true;
                }
            },
        }
    }
    proof {
        let t = cut(eo, cvs@);
        // the walk stopped at the cut, or at 100 rows inside the candidates
        if stop {
            assert(j == t);
        } else {
            assert(j == 100);
            assert(100 <= t) by { if t < 100 { assert(eo[t].cv@ > cvs@ ); assert(t < eo.len() ==> eo[t].cv@ <= cvs@); assert(false); } }
        }
        let limited = limit_rows(full, 100);
        assert(out_view(out@) =~= limited) by {
            if full.len() <= 100 { assert(full =~= rows_of_ents(eo.take(j as int))); }
            else { assert(full.take(100) =~= rows_of_ents(eo.take(j as int))); }
        }
        assert(query_result(db, p, limited));
        assert forall|i: int| 0 <= i < out.len() implies out_fits(#[trigger] out_view(out@)[i]) by {
            assert(out_view(out@)[i] == out_row(eo[i]));
            lemma_f64_in_range(eo[i].bits->0);
        }
    }
    Some(out)
}

/// The answer of a class: from the order index when every total is a double, else by scanning the groups.
pub fn finish(cls: &Class, c: &Consts, Ghost(db): Ghost<Db>, Ghost(p): Ghost<Params>) -> (r: Option<Vec<Out>>)
    requires
        cls_ok(*cls, db, p),
        nums_dbl(db),
        consts_ok(*c),
    ensures
        r matches Some(out) ==> query_result(db, p, out_view(out@)),
        r matches Some(out) ==> forall|i: int| 0 <= i < out.len() ==> out_fits(#[trigger] out_view(out@)[i]),
        r is None ==> !answer_fits(db, p),
{
    if cls.oor.is_zero() { finish_fast(cls, c, Ghost(db), Ghost(p)) } else { finish_scan(cls, c, Ghost(db), Ghost(p)) }
}

}
