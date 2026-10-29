// The answer from the kept groups of a class: a full listing that starts with them.
#![allow(unused_imports)]
use vstd::prelude::*;
use verus_common::rel::*;
use verus_common::sums::*;
use verus_common::bytes::*;
use verus_prooflib::ordered::{Index, Key, KeyView, key_lt, lemma_key_order, lemma_key_trans};
use verus_prooflib::float::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::model::*;
use crate::answer::*;
use crate::ordering::*;
use crate::esc::*;
use crate::accmap::*;
use crate::f64bits::*;
use crate::grp::*;
use crate::inv::*;
use crate::ordx::*;
use crate::sortx::*;
use crate::cands::*;

verus! {

pub open spec fn g_row(p2: Seq<u8>, g: Grp) -> OutRow {
    OutRow { name: ov(g.name), stmt: Some(p2), tag: Some(g.tag@), plabel: ov(g.plabel), total_value: Some(f64_val(g.bits)), cnt: g.cnt@ }
}

pub open spec fn r_rows(p2: Seq<u8>, es: Seq<(KeyView, Grp)>) -> Seq<OutRow> {
    es.map_values(|e: (KeyView, Grp)| g_row(p2, e.1))
}

/// A double that is the exact sum or a rounding of it is an admissible total.
pub proof fn lemma_okbits_within(db: Db, k: GK, b: u64)
    requires okbits(b, sumk(db, k), cntk(db, k)), cntk(db, k) > 0,
    ensures float_within(f64_val(b), sumk(db, k), absk(db, k), cntk(db, k) - 1),
{
    if cntk(db, k) != 1 {
        lemma_abs_le(db, k);
        lemma_rounded_within(b, sumk(db, k), absk(db, k), cntk(db, k) - 1);
    }
}

pub proof fn lemma_g_row_ok(db: Db, p: Params, c: ClassRec, g: Grp)
    requires exact_g(db, c, g), k_params(cls_gk(c, g)) == p,
    ensures
        row_ok(db, p, g_row(p.p2, g)),
        g_row(p.p2, g) == brow(p.p2, g.bits, cls_gk(c, g), g.cnt@),
        crow(g_row(p.p2, g)),
{
    let k = cls_gk(c, g);
    lemma_okbits_within(db, k, g.bits);
    lemma_row_ok(db, p, k, f64_val(g.bits));
}

/// The kept groups, in order, are answer rows in the answer's order, each group once.
pub proof fn lemma_r_rows(db: Db, p: Params, c: ClassRec)
    requires
        crec_struct(c), crec_sem(db, c), cls_key(c) == okey(p.p1, p.p2, p.p3),
    ensures
        all_crow(r_rows(p.p2, c.r.entries())),
        forall|i: int| 0 <= i < c.r.entries().len() ==> row_ok(db, p, #[trigger] r_rows(p.p2, c.r.entries())[i]),
        rows_sorted(r_rows(p.p2, c.r.entries())),
        rows_distinct(r_rows(p.p2, c.r.entries())),
{
    lemma_okey_inj(c.fy as int, c.stmt@, c.uom@, p.p1, p.p2, p.p3);
    let es = c.r.entries();
    let rr = r_rows(p.p2, es);
    c.r.model();
    assert forall|i: int| 0 <= i < es.len() implies {
        &&& c.r.at(#[trigger] es[i].0) == Some(es[i].1)
        &&& es[i].0 == rkey(es[i].1)
        &&& exact_g(db, c, es[i].1)
    } by {
        assert(c.r.at(es[i].0) == Some(es[i].1));
    }
    assert forall|i: int| 0 <= i < es.len() implies row_ok(db, p, #[trigger] rr[i]) && crow(rr[i]) by {
        assert(k_params(cls_gk(c, es[i].1)) == p);
        lemma_g_row_ok(db, p, c, es[i].1);
    }
    assert forall|i: int, j: int| 0 <= i < j < es.len() implies !row_lt(#[trigger] rr[j], #[trigger] rr[i]) && !same_group(rr[i], rr[j]) by {
        let (gi, gj) = (es[i].1, es[j].1);
        assert(key_lt(es[i].0, es[j].0));
        lemma_g_row_ok(db, p, c, gi);
        lemma_g_row_ok(db, p, c, gj);
        lemma_rk_row(p.p2, gi.bits, cls_gk(c, gi), gi.cnt@, gj.bits, cls_gk(c, gj), gj.cnt@);
        if same_group(rr[i], rr[j]) {
            assert(gi.ik() == gj.ik());
            assert(es[i].0 == es[j].0);
            lemma_key_order(es[i].0, es[j].0);
        }
    }
}

pub open spec fn fb_grp(c: ClassRec, k: GK) -> Grp {
    c.fb.at(nkey_v(k.name))->0
}

pub open spec fn bits_for(c: ClassRec, k: GK, s: int, n: int, b: u64) -> bool {
    okbits(b, s, n) && c.fb.at(nkey_v(k.name)) is Some && !key_lt(rkey_gk(b, k), rkey(fb_grp(c, k)))
}

/// The double the proof gives a group that is not kept: below its unit's frontier group.
pub open spec fn rest_bits(c: ClassRec, k: GK, s: int, n: int) -> u64 {
    choose|b: u64| bits_for(c, k, s, n, b)
}

/// The total the proof gives a group that is not kept: that double, or the sum itself outside the double range.
pub open spec fn rest_val(c: ClassRec, k: GK, s: int, n: int) -> int {
    if s > max_val() || s < -max_val() { s } else { f64_val(rest_bits(c, k, s, n)) }
}

pub open spec fn rest_row(db: Db, p: Params, c: ClassRec, k: GK) -> OutRow {
    row_of_group(p, k, rest_val(c, k, sumk(db, k), cntk(db, k)), cntk(db, k))
}

/// A group that is not kept, and not above the double range, is an answer row that sorts after every kept row.
pub proof fn lemma_rest_row(db: Db, p: Params, c: ClassRec, k: GK)
    requires
        crec_struct(c), crec_sem(db, c), cls_key(c) == okey(p.p1, p.p2, p.p3),
        is_cand(db, p, k), covered_in(c, k, sumk(db, k), cntk(db, k)), !r_has(c, k),
    ensures
        row_ok(db, p, rest_row(db, p, c, k)),
        crow(rest_row(db, p, c, k)),
        forall|rk: KeyView| #![trigger c.r.at(rk)] c.r.at(rk) is Some ==> !same_group(rest_row(db, p, c, k), g_row(p.p2, c.r.at(rk)->0)),
        sumk(db, k) <= max_val() ==> forall|rk: KeyView| #![trigger c.r.at(rk)] c.r.at(rk) is Some ==> !row_lt(rest_row(db, p, c, k), g_row(p.p2, c.r.at(rk)->0)),
{
    lemma_okey_inj(c.fy as int, c.stmt@, c.uom@, p.p1, p.p2, p.p3);
    let (s, n) = (sumk(db, k), cntk(db, k));
    let row = rest_row(db, p, c, k);
    assert(k_params(k) == p);
    if s > max_val() {
        lemma_row_ok(db, p, k, s);
    } else if s < -max_val() {
        lemma_row_ok(db, p, k, s);
        assert forall|rk: KeyView| c.r.at(rk) is Some implies !row_lt(row, g_row(p.p2, c.r.at(rk)->0)) by {
            let g = c.r.at(rk)->0;
            lemma_g_row_ok(db, p, c, g);
            lemma_beyond_row(p.p2, g.bits, cls_gk(c, g), g.cnt@, row);
        }
    } else {
        assert(s <= max_val() && s >= -max_val());
        assert(exists|b: u64| okbits(b, s, n) && c.fb.at(nkey_v(k.name)) is Some && !key_lt(rkey_gk(b, k), rkey(c.fb.at(nkey_v(k.name))->0)));
        let b0 = choose|b: u64| okbits(b, s, n) && c.fb.at(nkey_v(k.name)) is Some && !key_lt(rkey_gk(b, k), rkey(c.fb.at(nkey_v(k.name))->0));
        assert(bits_for(c, k, s, n, b0));
        let b = rest_bits(c, k, s, n);
        assert(bits_for(c, k, s, n, b));
        lemma_okbits_within(db, k, b);
        lemma_row_ok(db, p, k, f64_val(b));
        let fg = fb_grp(c, k);
        assert(c.f.at(rkey(fg)) == Some(fg));
        assert(c.f.at(rkey(fg)) is Some);
        assert forall|rk: KeyView| c.r.at(rk) is Some implies !row_lt(row, g_row(p.p2, c.r.at(rk)->0)) by {
            let g = c.r.at(rk)->0;
            lemma_g_row_ok(db, p, c, g);
            assert(key_lt(rk, rkey(fg)));
            assert(rk == rkey(g));
            lemma_key_order(rkey_gk(b, k), rkey(fg));
            if rkey_gk(b, k) != rkey(fg) { assert(key_lt(rkey(fg), rkey_gk(b, k))); lemma_key_trans(rkey(g), rkey(fg), rkey_gk(b, k)); }
            lemma_rk_row(p.p2, g.bits, cls_gk(c, g), g.cnt@, b, k, n);
            assert(row == brow(p.p2, b, k, n));
        }
    }
    assert forall|rk: KeyView| c.r.at(rk) is Some implies !same_group(row, g_row(p.p2, c.r.at(rk)->0)) by {
        let g = c.r.at(rk)->0;
        if same_group(row, g_row(p.p2, g)) {
            assert(cls_gk(c, g) == k);
            assert(r_has(c, k));
        }
    }
}

proof fn lemma_rest_keys(c: ClassRec, kl: Seq<GK>) -> (rk: Seq<GK>)
    requires no_dups(kl),
    ensures
        no_dups(rk),
        forall|i: int| 0 <= i < rk.len() ==> kl.contains(#[trigger] rk[i]) && !r_has(c, rk[i]),
        forall|k: GK| kl.contains(k) && !r_has(c, k) ==> rk.contains(k),
    decreases kl.len(),
{
    if kl.len() == 0 {
        Seq::empty()
    } else {
        let t = kl.drop_last();
        assert(no_dups(t));
        let r0 = lemma_rest_keys(c, t);
        let x = kl[kl.len() - 1];
        assert forall|i: int| 0 <= i < r0.len() implies r0[i] != x by {
            assert(t.contains(r0[i]));
            let q = choose|q: int| 0 <= q < t.len() && t[q] == r0[i];
            assert(kl[q] == r0[i]);
        }
        let rk = if r_has(c, x) { r0 } else { r0.push(x) };
        assert forall|k: GK| kl.contains(k) && !r_has(c, k) implies rk.contains(k) by {
            if k == x {
                assert(rk[rk.len() - 1] == x);
            } else {
                let q = choose|q: int| 0 <= q < kl.len() && kl[q] == k;
                assert(q < kl.len() - 1);
                assert(t[q] == k);
                assert(t.contains(k));
                assert(r0.contains(k));
                let z = choose|z: int| 0 <= z < r0.len() && r0[z] == k;
                assert(rk[z] == k);
            }
        }
        assert forall|i: int| 0 <= i < rk.len() implies kl.contains(#[trigger] rk[i]) && !r_has(c, rk[i]) by {
            if i < r0.len() {
                assert(rk[i] == r0[i]);
                assert(t.contains(r0[i]));
                let q = choose|q: int| 0 <= q < t.len() && t[q] == r0[i];
                assert(kl[q] == rk[i]);
            } else {
                assert(rk[i] == x);
                assert(kl[kl.len() - 1] == x);
            }
        }
        rk
    }
}

/// A listing that holds every group once, in order, with admissible rows, is a full answer.
pub proof fn lemma_full_from(db: Db, p: Params, full: Seq<OutRow>)
    requires
        forall|i: int| 0 <= i < full.len() ==> row_ok(db, p, #[trigger] full[i]),
        rows_sorted(full), rows_distinct(full),
        forall|k: GK| is_cand(db, p, k) ==> exists|i: int| 0 <= i < full.len() && (#[trigger] full[i]).name == k.name
            && full[i].stmt == Some(p.p2) && full[i].tag == Some(k.tag) && full[i].plabel == k.plabel,
    ensures full_result(db, p, full),
{
    assert forall|g_name: Option<Seq<u8>>, g_stmt: Option<Seq<u8>>, g_tag: Option<Seq<u8>>, g_plabel: Option<Seq<u8>>|
        #[trigger] group_count(db, p, g_name, g_stmt, g_tag, g_plabel) > 0 && having(db, p, g_name, g_stmt, g_tag, g_plabel) implies
        exists|i: int| 0 <= i < full.len() && (#[trigger] full[i]).name == g_name && full[i].stmt == g_stmt && full[i].tag == g_tag && full[i].plabel == g_plabel by {
        lemma_group_nonempty(db, p, g_name, g_stmt, g_tag, g_plabel);
        let k = GK { fy: p.p1, stmt: p.p2, uom: p.p3, name: g_name, tag: g_tag->0, plabel: g_plabel };
        assert(is_cand(db, p, k));
    }
}

proof fn lemma_nonempty<V>(ix: Index<V>, q: KeyView)
    requires ix.wf(), ix.at(q) is Some,
    ensures ix.entries().len() > 0,
{
    ix.model();
    let pos = choose|pos: int| #![auto] 0 <= pos < ix.entries().len() && ix.entries()[pos].0 == q;
}

proof fn lemma_some_entry<V>(ix: Index<V>) -> (q: KeyView)
    requires ix.wf(), ix.entries().len() > 0,
    ensures ix.at(q) is Some,
{
    ix.model();
    ix.entries()[0].0
}

proof fn lemma_r_has_entry(c: ClassRec, k: GK) -> (i: int)
    requires crec_struct(c), r_has(c, k),
    ensures 0 <= i < c.r.entries().len(), cls_gk(c, c.r.entries()[i].1) == k,
{
    c.r.model();
    let rk = choose|rk: KeyView| #![trigger c.r.at(rk)] c.r.at(rk) is Some && cls_gk(c, c.r.at(rk)->0) == k;
    let pos = choose|pos: int| #![auto] 0 <= pos < c.r.entries().len() && c.r.entries()[pos].0 == rk;
    assert(c.r.at(c.r.entries()[pos].0) == Some(c.r.entries()[pos].1));
    pos
}

pub open spec fn rest_rows(db: Db, p: Params, c: ClassRec, rk: Seq<GK>) -> Seq<OutRow> {
    rk.map_values(|k: GK| rest_row(db, p, c, k))
}

/// The classes of the state cover every group; none is above the double range when hp is empty.
proof fn lemma_cover_all(db: Db, cls: Index<ClassRec>, p: Params, c: ClassRec)
    requires cls_ok(db, cls), cls.at(okey(p.p1, p.p2, p.p3)) == Some(c), c.hp.entries().len() == 0,
    ensures forall|k: GK| is_cand(db, p, k) ==> covered_in(c, k, sumk(db, k), cntk(db, k)) && sumk(db, k) <= max_val(),
{
    let ok = okey(p.p1, p.p2, p.p3);
    assert forall|k: GK| is_cand(db, p, k) implies covered_in(c, k, sumk(db, k), cntk(db, k)) && sumk(db, k) <= max_val() by {
        assert(okey(k.fy, k.stmt, k.uom) == ok);
        assert(covered(db, cls, k));
        if sumk(db, k) > max_val() { lemma_nonempty(c.hp, nkey_v(k.name)); }
    }
}

/// The groups that are not kept give answer rows of distinct groups.
proof fn lemma_rest_ok(db: Db, p: Params, c: ClassRec, kl: Seq<GK>, rk: Seq<GK>)
    requires
        crec_struct(c), crec_sem(db, c), cls_key(c) == okey(p.p1, p.p2, p.p3),
        no_dups(rk),
        forall|i: int| 0 <= i < rk.len() ==> kl.contains(#[trigger] rk[i]) && !r_has(c, rk[i]),
        forall|i: int| 0 <= i < kl.len() ==> is_cand(db, p, #[trigger] kl[i]),
        forall|k: GK| is_cand(db, p, k) ==> covered_in(c, k, sumk(db, k), cntk(db, k)) && sumk(db, k) <= max_val(),
    ensures
        all_crow(rest_rows(db, p, c, rk)),
        forall|i: int| 0 <= i < rk.len() ==> row_ok(db, p, #[trigger] rest_rows(db, p, c, rk)[i]),
        rows_distinct(rest_rows(db, p, c, rk)),
{
    let rest = rest_rows(db, p, c, rk);
    assert forall|i: int| 0 <= i < rest.len() implies row_ok(db, p, #[trigger] rest[i]) && crow(rest[i]) by {
        let k = rk[i];
        assert(kl.contains(k));
        let q = choose|q: int| 0 <= q < kl.len() && kl[q] == k;
        assert(is_cand(db, p, k));
        lemma_rest_row(db, p, c, k);
    }
    assert forall|i: int, j: int| 0 <= i < j < rest.len() implies !same_group(#[trigger] rest[i], #[trigger] rest[j]) by {
        if same_group(rest[i], rest[j]) { assert(rk[i] == rk[j]); }
    }
}

/// The kept rows followed by any sorted listing of the rest are in the answer's order.
proof fn lemma_assemble(db: Db, p: Params, c: ClassRec, kl: Seq<GK>, rk: Seq<GK>, t: Seq<OutRow>)
    requires
        crec_struct(c), crec_sem(db, c), cls_key(c) == okey(p.p1, p.p2, p.p3),
        no_dups(rk),
        forall|i: int| 0 <= i < rk.len() ==> kl.contains(#[trigger] rk[i]) && !r_has(c, rk[i]),
        forall|i: int| 0 <= i < kl.len() ==> is_cand(db, p, #[trigger] kl[i]),
        forall|k: GK| is_cand(db, p, k) ==> covered_in(c, k, sumk(db, k), cntk(db, k)) && sumk(db, k) <= max_val(),
        rows_sorted(t), rows_distinct(t),
        forall|i: int| 0 <= i < t.len() ==> rest_rows(db, p, c, rk).contains(#[trigger] t[i]),
    ensures
        rows_sorted(r_rows(p.p2, c.r.entries()) + t),
        rows_distinct(r_rows(p.p2, c.r.entries()) + t),
        forall|i: int| 0 <= i < t.len() ==> row_ok(db, p, #[trigger] t[i]),
{
    let es = c.r.entries();
    let rr = r_rows(p.p2, es);
    let full = rr + t;
    let rest = rest_rows(db, p, c, rk);
    lemma_r_rows(db, p, c);
    c.r.model();
    lemma_rest_ok(db, p, c, kl, rk);
    assert forall|i: int| 0 <= i < t.len() implies row_ok(db, p, #[trigger] t[i]) by {
        assert(rest.contains(t[i]));
        let q = choose|q: int| 0 <= q < rest.len() && rest[q] == t[i];
    }
    assert forall|i: int, j: int| 0 <= i < rr.len() && 0 <= j < t.len() implies
        !row_lt(#[trigger] t[j], #[trigger] rr[i]) && !same_group(rr[i], t[j]) by {
        let x = t[j];
        assert(rest.contains(x));
        let q = choose|q: int| 0 <= q < rest.len() && rest[q] == x;
        assert(kl.contains(rk[q]));
        let q2 = choose|q2: int| 0 <= q2 < kl.len() && kl[q2] == rk[q];
        lemma_rest_row(db, p, c, rk[q]);
        assert(c.r.at(es[i].0) == Some(es[i].1));
        assert(rr[i] == g_row(p.p2, es[i].1));
        assert(sumk(db, rk[q]) <= max_val());
    }
    assert(rows_sorted(full)) by {
        assert forall|i: int, j: int| 0 <= i < j < full.len() implies !row_lt(#[trigger] full[j], #[trigger] full[i]) by {
            if j < rr.len() {
            } else if i < rr.len() {
                assert(full[j] == t[j - rr.len()] && full[i] == rr[i]);
            } else {
                assert(full[i] == t[i - rr.len()] && full[j] == t[j - rr.len()]);
            }
        }
    }
    assert(rows_distinct(full)) by {
        assert forall|i: int, j: int| 0 <= i < j < full.len() implies !same_group(#[trigger] full[i], #[trigger] full[j]) by {
            if j < rr.len() {
            } else if i < rr.len() {
                assert(full[j] == t[j - rr.len()] && full[i] == rr[i]);
                assert(same_group(rr[i], t[j - rr.len()]) == same_group(t[j - rr.len()], rr[i]));
            } else {
                assert(full[i] == t[i - rr.len()] && full[j] == t[j - rr.len()]);
            }
        }
    }
}

/// The full answer that starts with the kept groups, when no unit has a group above the double range.
pub proof fn lemma_read_full(db: Db, cls: Index<ClassRec>, p: Params, c: ClassRec) -> (full: Seq<OutRow>)
    requires
        cls_ok(db, cls), cls.at(okey(p.p1, p.p2, p.p3)) == Some(c), c.hp.entries().len() == 0,
    ensures
        full_result(db, p, full),
        full.len() >= c.r.entries().len(),
        full.take(c.r.entries().len() as int) =~= r_rows(p.p2, c.r.entries()),
        c.f.entries().len() == 0 ==> forall|i: int| c.r.entries().len() <= i < full.len() ==> !out_fits(#[trigger] full[i]),
        c.f.entries().len() == 0 && c.hn.entries().len() == 0 ==> full.len() == c.r.entries().len(),
        c.f.entries().len() == 0 && c.hn.entries().len() > 0 ==> full.len() > c.r.entries().len(),
{
    let ok = okey(p.p1, p.p2, p.p3);
    assert(cls.at(ok) is Some);
    assert(ok == cls_key(c) && crec_struct(c) && crec_sem(db, c));
    let es = c.r.entries();
    let rr = r_rows(p.p2, es);
    c.r.model();
    let kl = lemma_cand_list(db, p);
    let rk = lemma_rest_keys(c, kl);
    lemma_cover_all(db, cls, p, c);
    lemma_rest_ok(db, p, c, kl, rk);
    let rest = rest_rows(db, p, c, rk);
    let t = lemma_sort_rows(rest);
    let full = rr + t;
    lemma_assemble(db, p, c, kl, rk, t);
    lemma_r_rows(db, p, c);
    assert forall|i: int| 0 <= i < full.len() implies row_ok(db, p, #[trigger] full[i]) by {
        if i >= rr.len() { assert(full[i] == t[i - rr.len()]); }
    }
    assert forall|k: GK| is_cand(db, p, k) implies exists|i: int| 0 <= i < full.len() && (#[trigger] full[i]).name == k.name
            && full[i].stmt == Some(p.p2) && full[i].tag == Some(k.tag) && full[i].plabel == k.plabel by {
        assert(kl.contains(k));
        if r_has(c, k) {
            let i = lemma_r_has_entry(c, k);
            assert(full[i] == rr[i]);
            assert(rr[i] == g_row(p.p2, es[i].1));
        } else {
            assert(rk.contains(k));
            let z = choose|z: int| 0 <= z < rk.len() && rk[z] == k;
            assert(rest[z] == rest_row(db, p, c, k));
            assert(t.contains(rest[z]));
            let w = choose|w: int| 0 <= w < t.len() && t[w] == rest[z];
            assert(full[rr.len() + w] == t[w]);
        }
    }
    lemma_full_from(db, p, full);
    assert(full.take(es.len() as int) =~= rr);
    if c.f.entries().len() == 0 {
        lemma_rest_below(db, cls, p, c, kl, rk);
        assert forall|i: int| es.len() <= i < full.len() implies !out_fits(#[trigger] full[i]) by {
            let x = t[i - rr.len()];
            assert(full[i] == x);
            assert(rest.contains(x));
            let q = choose|q: int| 0 <= q < rest.len() && rest[q] == x;
            assert(rest[q] == rest_row(db, p, c, rk[q]));
        }
        if c.hn.entries().len() == 0 {
            lemma_rest_empty(db, cls, p, c, kl, rk);
        } else {
            lemma_rest_some(db, p, c, kl, rk);
        }
    }
    full
}

/// With nothing in the frontier, every group that is not kept is below the double range.
proof fn lemma_rest_below(db: Db, cls: Index<ClassRec>, p: Params, c: ClassRec, kl: Seq<GK>, rk: Seq<GK>)
    requires
        cls_ok(db, cls), cls.at(okey(p.p1, p.p2, p.p3)) == Some(c), c.hp.entries().len() == 0, c.f.entries().len() == 0,
        forall|i: int| 0 <= i < rk.len() ==> kl.contains(#[trigger] rk[i]) && !r_has(c, rk[i]),
        forall|i: int| 0 <= i < kl.len() ==> is_cand(db, p, #[trigger] kl[i]),
    ensures
        forall|i: int| 0 <= i < rk.len() ==> sumk(db, #[trigger] rk[i]) < -max_val(),
{
    let ok = okey(p.p1, p.p2, p.p3);
    lemma_cover_all(db, cls, p, c);
    assert forall|i: int| 0 <= i < rk.len() implies sumk(db, #[trigger] rk[i]) < -max_val() by {
        let k = rk[i];
        assert(kl.contains(k));
        let q = choose|q: int| 0 <= q < kl.len() && kl[q] == k;
        assert(is_cand(db, p, k));
        if sumk(db, k) >= -max_val() {
            assert(covered_in(c, k, sumk(db, k), cntk(db, k)));
            assert(c.fb.at(nkey_v(k.name)) is Some);
            let g = c.fb.at(nkey_v(k.name))->0;
            assert(c.f.at(rkey(g)) == c.fb.at(nkey_v(k.name)));
            lemma_nonempty(c.f, rkey(g));
        }
    }
}

proof fn lemma_rest_empty(db: Db, cls: Index<ClassRec>, p: Params, c: ClassRec, kl: Seq<GK>, rk: Seq<GK>)
    requires
        cls_ok(db, cls), cls.at(okey(p.p1, p.p2, p.p3)) == Some(c), c.hp.entries().len() == 0, c.f.entries().len() == 0, c.hn.entries().len() == 0,
        forall|i: int| 0 <= i < rk.len() ==> kl.contains(#[trigger] rk[i]) && !r_has(c, rk[i]),
        forall|i: int| 0 <= i < kl.len() ==> is_cand(db, p, #[trigger] kl[i]),
    ensures rk.len() == 0,
{
    let ok = okey(p.p1, p.p2, p.p3);
    lemma_rest_below(db, cls, p, c, kl, rk);
    if rk.len() > 0 {
        let k = rk[0];
        assert(kl.contains(k));
        let q = choose|q: int| 0 <= q < kl.len() && kl[q] == k;
        assert(is_cand(db, p, k));
        assert(okey(k.fy, k.stmt, k.uom) == ok);
        assert(covered(db, cls, k));
        assert(sumk(db, k) < -max_val());
        lemma_nonempty(c.hn, nkey_v(k.name));
    }
}

proof fn lemma_rest_some(db: Db, p: Params, c: ClassRec, kl: Seq<GK>, rk: Seq<GK>)
    requires
        crec_struct(c), crec_sem(db, c), cls_key(c) == okey(p.p1, p.p2, p.p3), c.hn.entries().len() > 0,
        forall|k: GK| kl.contains(k) && !r_has(c, k) ==> rk.contains(k),
        forall|k: GK| is_cand(db, p, k) ==> kl.contains(k),
    ensures rk.len() > 0,
{
    lemma_okey_inj(c.fy as int, c.stmt@, c.uom@, p.p1, p.p2, p.p3);
    let nk = lemma_some_entry(c.hn);
    assert(has_cand(db, c, nk, false));
    let k = choose|k: GK| #![trigger cntk(db, k)] cntk(db, k) > 0 && in_class(c, k) && nkey_v(k.name) == nk && sumk(db, k) < -max_val();
    assert(k_params(k) == p);
    assert(is_cand(db, p, k));
    assert(kl.contains(k));
    if r_has(c, k) {
        let i = lemma_r_has_entry(c, k);
        c.r.model();
        assert(c.r.at(c.r.entries()[i].0) == Some(c.r.entries()[i].1));
        assert(exact_g(db, c, c.r.entries()[i].1));
    }
    assert(rk.contains(k));
}

pub proof fn lemma_not_fits(db: Db, p: Params, full: Seq<OutRow>, t: int)
    requires full_result(db, p, full), 0 <= t < full.len(), t < 200, !out_fits(full[t]),
    ensures !answer_fits(db, p),
{
    let lim = limit_rows(full, 200);
    assert(query_result(db, p, lim));
    if full.len() <= 200 { assert(lim == full); } else { assert(lim == full.take(200)); }
    assert(lim[t] == full[t]);
}

/// A class without a record has no groups, so the answer is empty.
pub proof fn lemma_read_none(db: Db, cls: Index<ClassRec>, p: Params)
    requires cls_ok(db, cls), cls.at(okey(p.p1, p.p2, p.p3)) is None,
    ensures query_result(db, p, Seq::empty()),
{
    let full = Seq::<OutRow>::empty();
    assert forall|k: GK| is_cand(db, p, k) implies false by {
        assert(okey(k.fy, k.stmt, k.uom) == okey(p.p1, p.p2, p.p3));
        assert(covered(db, cls, k));
    }
    lemma_full_from(db, p, full);
    assert(limit_rows(full, 200) =~= Seq::<OutRow>::empty());
}

/// A group above the double range sorts first and does not fit: the answer is not total.
pub proof fn lemma_read_unfit_p(db: Db, cls: Index<ClassRec>, p: Params, c: ClassRec)
    requires cls_ok(db, cls), cls.at(okey(p.p1, p.p2, p.p3)) == Some(c), c.hp.entries().len() > 0,
    ensures !answer_fits(db, p),
{
    let ok = okey(p.p1, p.p2, p.p3);
    assert(ok == cls_key(c) && crec_struct(c) && crec_sem(db, c));
    lemma_okey_inj(c.fy as int, c.stmt@, c.uom@, p.p1, p.p2, p.p3);
    let es = c.r.entries();
    let rr = r_rows(p.p2, es);
    c.r.model();
    lemma_r_rows(db, p, c);
    let kl = lemma_cand_list(db, p);
    let rk = lemma_rest_keys(c, kl);
    assert forall|k: GK| is_cand(db, p, k) implies covered_in(c, k, sumk(db, k), cntk(db, k)) by {
        assert(okey(k.fy, k.stmt, k.uom) == ok);
        assert(covered(db, cls, k));
    }
    let rest = rest_rows(db, p, c, rk);
    assert forall|i: int| 0 <= i < rest.len() implies row_ok(db, p, #[trigger] rest[i]) && crow(rest[i]) by {
        let k = rk[i];
        assert(kl.contains(k));
        let q = choose|q: int| 0 <= q < kl.len() && kl[q] == k;
        assert(is_cand(db, p, k));
        lemma_rest_row(db, p, c, k);
    }
    let all = rr + rest;
    assert(all_crow(all)) by {
        assert forall|i: int| 0 <= i < all.len() implies crow(#[trigger] all[i]) by {
            if i < rr.len() { assert(all[i] == rr[i]); } else { assert(all[i] == rest[i - rr.len()]); }
        }
    }
    assert(rows_distinct(all)) by {
        assert forall|i: int, j: int| 0 <= i < j < all.len() implies !same_group(#[trigger] all[i], #[trigger] all[j]) by {
            if j < rr.len() {
            } else if i < rr.len() {
                let x = rest[j - rr.len()];
                assert(all[j] == x && all[i] == rr[i]);
                let z = j - rr.len();
                assert(kl.contains(rk[z]));
                let q = choose|q: int| 0 <= q < kl.len() && kl[q] == rk[z];
                assert(is_cand(db, p, rk[z]));
                lemma_rest_row(db, p, c, rk[z]);
                assert(c.r.at(es[i].0) == Some(es[i].1));
                assert(rr[i] == g_row(p.p2, es[i].1));
                assert(same_group(rr[i], x) == same_group(x, rr[i]));
            } else {
                let (a, b) = (i - rr.len(), j - rr.len());
                assert(all[i] == rest[a] && all[j] == rest[b]);
                if same_group(rest[a], rest[b]) { assert(rk[a] == rk[b]); }
            }
        }
    }
    let t = lemma_sort_rows(all);
    assert forall|i: int| 0 <= i < t.len() implies row_ok(db, p, #[trigger] t[i]) by {
        assert(all.contains(t[i]));
        let q = choose|q: int| 0 <= q < all.len() && all[q] == t[i];
        if q < rr.len() { assert(all[q] == rr[q]); } else { assert(all[q] == rest[q - rr.len()]); }
    }
    assert forall|k: GK| is_cand(db, p, k) implies exists|i: int| 0 <= i < t.len() && (#[trigger] t[i]).name == k.name
            && t[i].stmt == Some(p.p2) && t[i].tag == Some(k.tag) && t[i].plabel == k.plabel by {
        assert(kl.contains(k));
        let x = if r_has(c, k) {
            let i = lemma_r_has_entry(c, k);
            assert(all[i] == rr[i]);
            assert(rr[i] == g_row(p.p2, es[i].1));
            all[i]
        } else {
            assert(rk.contains(k));
            let z = choose|z: int| 0 <= z < rk.len() && rk[z] == k;
            assert(all[rr.len() + z] == rest[z]);
            assert(rest[z] == rest_row(db, p, c, k));
            all[rr.len() + z as int]
        };
        assert(all.contains(x));
        let w = choose|w: int| 0 <= w < t.len() && t[w] == x;
    }
    lemma_full_from(db, p, t);
    // the group above the range
    let nk = lemma_some_entry(c.hp);
    assert(has_cand(db, c, nk, true));
    let k0 = choose|k: GK| #![trigger cntk(db, k)] cntk(db, k) > 0 && in_class(c, k) && nkey_v(k.name) == nk && sumk(db, k) > max_val();
    assert(k_params(k0) == p);
    assert(is_cand(db, p, k0));
    assert(kl.contains(k0));
    if r_has(c, k0) {
        let i = lemma_r_has_entry(c, k0);
        assert(c.r.at(es[i].0) == Some(es[i].1));
        assert(exact_g(db, c, es[i].1));
    }
    assert(rk.contains(k0));
    let z = choose|z: int| 0 <= z < rk.len() && rk[z] == k0;
    assert(rest[z] == rest_row(db, p, c, k0));
    assert(all[rr.len() + z] == rest[z]);
    assert(all.contains(rest[z]));
    let j = choose|j: int| 0 <= j < t.len() && t[j] == rest[z];
    assert(t.len() > 0);
    assert(crow(t[0])) by { assert(all.contains(t[0])); let q = choose|q: int| 0 <= q < all.len() && all[q] == t[0]; }
    if j > 0 {
        assert(!row_lt(t[j], t[0]));
    }
    assert(!out_fits(t[0]));
    lemma_not_fits(db, p, t, 0);
}

} // verus!
