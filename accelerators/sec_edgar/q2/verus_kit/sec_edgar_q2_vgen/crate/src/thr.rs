// Per class, a threshold on the value key: below it every cell is stored, beyond it a filing stores only its first cell.
use vstd::prelude::*;
use std::sync::Arc;
use verus_common::sums::*;
use verus_prooflib::ordered::{Index, Key, KeyView};
use crate::gen_spec::*;
use crate::model::*;
use crate::cells::*;
use crate::maint::*;
use crate::f64bits::*;

verus! {

pub const D_INF: i64 = 9223372036854775807;

/// The threshold of every class: absent is unbounded.
pub type Thr = Index<i64>;

/// A threshold per (filing, class): while a batch of filings is refreshed, they differ.
pub type Th = spec_fn(Seq<u8>, int, Seq<u8>) -> int;

pub open spec fn thr_of(t: Thr, fy: int, uom: Seq<u8>) -> int {
    match t.at((fy, uom)) { Some(d) => d as int, None => D_INF as int }
}

pub open spec fn th_of(t: Thr) -> Th {
    |a: Seq<u8>, fy: int, uom: Seq<u8>| thr_of(t, fy, uom)
}

pub open spec fn th_upd(th: Th, a: Seq<u8>, t: Thr) -> Th {
    |b: Seq<u8>, fy: int, uom: Seq<u8>| if b == a { thr_of(t, fy, uom) } else { th(b, fy, uom) }
}

pub open spec fn th_id(th: Th, id: CellId) -> int {
    th(id.a, id.fy, id.uom)
}

pub open spec fn at_d(id: CellId, d: int) -> CellId {
    CellId { fy: id.fy, uom: id.uom, d, a: id.a }
}

/// id is the first cell of its filing beyond the threshold.
pub open spec fn isb(w: Fam, th: Th, id: CellId) -> bool {
    &&& th_id(th, id) < id.d
    &&& !zero_fam(w, id)
    &&& forall|d: int| th_id(th, id) < d < id.d ==> zero_fam(w, #[trigger] at_d(id, d))
}

pub open spec fn keep(w: Fam, th: Th, id: CellId) -> bool {
    id.d <= th_id(th, id) || isb(w, th, id)
}

/// What is stored of the family w under the thresholds th.
pub open spec fn sfam(w: Fam, th: Th) -> Fam {
    |id: CellId, g: spec_fn(OutRow) -> int| if keep(w, th, id) { w(id, g) } else { 0int }
}

pub proof fn lemma_sfam_unit(w1: Fam, w2: Fam, th1: Th, th2: Th, id: CellId)
    requires
        th_id(th1, id) == th_id(th2, id),
        forall|d: int, g: spec_fn(OutRow) -> int| #[trigger] fam_w(w1, at_d(id, d), g) == fam_w(w2, at_d(id, d), g),
        forall|g: spec_fn(OutRow) -> int| #[trigger] fam_w(w1, id, g) == fam_w(w2, id, g),
    ensures
        forall|g: spec_fn(OutRow) -> int| #[trigger] fam_w(sfam(w1, th1), id, g) == fam_w(sfam(w2, th2), id, g),
{
    assert forall|d: int| zero_fam(w1, at_d(id, d)) == zero_fam(w2, at_d(id, d)) by {
        if zero_fam(w1, at_d(id, d)) {
            assert forall|g: spec_fn(OutRow) -> int| #[trigger] fam_w(w2, at_d(id, d), g) == 0 by { assert(fam_w(w1, at_d(id, d), g) == 0); }
        }
        if zero_fam(w2, at_d(id, d)) {
            assert forall|g: spec_fn(OutRow) -> int| #[trigger] fam_w(w1, at_d(id, d), g) == 0 by { assert(fam_w(w2, at_d(id, d), g) == 0); }
        }
    }
    lemma_zero_congr(w1, w2, id);
    assert(isb(w1, th1, id) == isb(w2, th2, id)) by {
        if isb(w1, th1, id) {
            assert forall|d: int| th_id(th2, id) < d < id.d implies zero_fam(w2, #[trigger] at_d(id, d)) by {
                assert(zero_fam(w1, at_d(id, d)) == zero_fam(w2, at_d(id, d)));
            }
        }
        if isb(w2, th2, id) {
            assert forall|d: int| th_id(th1, id) < d < id.d implies zero_fam(w1, #[trigger] at_d(id, d)) by {
                assert(zero_fam(w1, at_d(id, d)) == zero_fam(w2, at_d(id, d)));
            }
        }
    }
    assert(keep(w1, th1, id) == keep(w2, th2, id));
    assert forall|g: spec_fn(OutRow) -> int| #[trigger] fam_w(sfam(w1, th1), id, g) == fam_w(sfam(w2, th2), id, g) by {
        assert(fam_w(w1, id, g) == fam_w(w2, id, g));
        assert(fam_w(sfam(w1, th1), id, g) == if keep(w1, th1, id) { fam_w(w1, id, g) } else { 0int });
        assert(fam_w(sfam(w2, th2), id, g) == if keep(w2, th2, id) { fam_w(w2, id, g) } else { 0int });
    }
}

pub fn thr_get(t: &Thr, fy: i64, uom: &Arc<Vec<u8>>) -> (d: i64)
    requires
        t.wf(),
    ensures
        d as int == thr_of(*t, fy as int, (**uom)@),
{
    match t.get(&kkey(fy, uom)) {
        Some(v) => *v,
        None => D_INF,
    }
}

// ---- the stored entries of one filing ----

pub open spec fn cls_d(t: Thr, ck: CK) -> int {
    thr_of(t, ck.fy as int, (*ck.uom)@)
}

pub open spec fn same_cls(a: CK, b: CK) -> bool {
    a.fy == b.fy && (*a.uom)@ == (*b.uom)@
}

/// e is within the threshold of its class, or is the first entry of its class beyond it.
pub open spec fn kept_ent(ents: Seq<Ent>, t: Thr, e: Ent) -> bool {
    e.ck.d as int <= cls_d(t, e.ck)
    || forall|x: Ent| ents.contains(x) && same_cls(x.ck, e.ck) && x.ck.d as int > cls_d(t, e.ck) ==> e.ck.d <= #[trigger] x.ck.d
}

pub open spec fn kept_ents(ents: Seq<Ent>, t: Thr) -> Seq<Ent> {
    ents.filter(|e: Ent| kept_ent(ents, t, e))
}

pub open spec fn ent_fam(ents: Seq<Ent>) -> Fam {
    |id: CellId, g: spec_fn(OutRow) -> int| ent_sum(ents, id, g)
}

pub proof fn lemma_ent_fam_zero(ents: Seq<Ent>, id: CellId)
    ensures
        zero_fam(ent_fam(ents), id) == !exists|x: Ent| ents.contains(x) && ck_matches(x.ck, id),
{
    let one = |o: OutRow| 1int;
    if exists|x: Ent| ents.contains(x) && ck_matches(x.ck, id) {
        let x = choose|x: Ent| ents.contains(x) && ck_matches(x.ck, id);
        let i = choose|i: int| 0 <= i < ents.len() && ents[i] == x;
        let phi = |e: Ent| if ck_matches(e.ck, id) { one(e.row.view()) } else { 0 };
        assert forall|k: int| 0 <= k < ents.len() implies phi(#[trigger] ents[k]) >= 0 by {}
        assert(phi(ents[i]) == 1);
        lemma_sum_pos(ents, phi, i);
        assert(fam_w(ent_fam(ents), id, one) > 0);
        assert(!zero_fam(ent_fam(ents), id));
    } else {
        assert forall|g: spec_fn(OutRow) -> int| #[trigger] fam_w(ent_fam(ents), id, g) == 0 by {
            let phi = |e: Ent| if ck_matches(e.ck, id) { g(e.row.view()) } else { 0 };
            assert forall|k: int| 0 <= k < ents.len() implies phi(#[trigger] ents[k]) == 0 by {
                assert(ents.contains(ents[k]));
            }
            lemma_sum_zero(ents, phi);
        }
    }
}

/// The kept entries of a filing weigh what is stored of its cells.
pub proof fn lemma_kept_sum(ents: Seq<Ent>, t: Thr, id: CellId, g: spec_fn(OutRow) -> int)
    ensures
        ent_sum(kept_ents(ents, t), id, g) == fam_w(sfam(ent_fam(ents), th_of(t)), id, g),
{
    let p = |e: Ent| kept_ent(ents, t, e);
    let phi = |e: Ent| if ck_matches(e.ck, id) { g(e.row.view()) } else { 0 };
    let psi = |e: Ent| if p(e) { phi(e) } else { 0 };
    lemma_sum_filter(ents, p, phi, psi);
    let fe = ent_fam(ents);
    let th = th_of(t);
    lemma_ent_fam_zero(ents, id);
    if keep(fe, th, id) {
        assert forall|k: int| 0 <= k < ents.len() implies psi(#[trigger] ents[k]) == phi(ents[k]) by {
            let e = ents[k];
            assert(ents.contains(e));
            if ck_matches(e.ck, id) {
                if id.d > th_id(th, id) {
                    assert(isb(fe, th, id));
                    assert forall|x: Ent| ents.contains(x) && same_cls(x.ck, e.ck) && x.ck.d as int > cls_d(t, e.ck) implies e.ck.d <= #[trigger] x.ck.d by {
                        if (x.ck.d as int) < id.d {
                            let d2 = x.ck.d as int;
                            assert(zero_fam(fe, at_d(id, d2)));
                            lemma_ent_fam_zero(ents, at_d(id, d2));
                            assert(ck_matches(x.ck, at_d(id, d2)));
                        }
                    }
                }
            }
        }
        lemma_sum_ext(ents, psi, phi);
    } else {
        assert forall|k: int| 0 <= k < ents.len() implies psi(#[trigger] ents[k]) == 0 by {
            let e = ents[k];
            assert(ents.contains(e));
            if ck_matches(e.ck, id) && p(e) {
                // e is an entry of id's cell: id is not first beyond the threshold, so a smaller beyond cell exists
                assert(id.d > th_id(th, id));
                assert(!zero_fam(fe, id));
                if !forall|d: int| th_id(th, id) < d < id.d ==> zero_fam(fe, #[trigger] at_d(id, d)) {
                    let d2 = choose|d: int| th_id(th, id) < d < id.d && !zero_fam(fe, at_d(id, d));
                    lemma_ent_fam_zero(ents, at_d(id, d2));
                    let x = choose|x: Ent| ents.contains(x) && ck_matches(x.ck, at_d(id, d2));
                    assert(same_cls(x.ck, e.ck));
                    assert(x.ck.d as int > cls_d(t, e.ck));
                    assert(false);
                }
                assert(isb(fe, th, id));
            }
        }
        lemma_sum_zero(ents, psi);
    }
}

pub fn copy_ent(e: &Ent) -> (c: Ent)
    ensures c == *e,
{
    Ent { ck: copy_ck(&e.ck), row: copy_row(&e.row) }
}

/// The entries of a filing that are stored under the thresholds t.
pub fn keep_ents(ents: &Vec<Ent>, t: &Thr) -> (kept: Vec<Ent>)
    requires
        t.wf(),
    ensures
        kept@ == kept_ents(ents@, *t),
        forall|j: int| 0 <= j < kept@.len() ==> ents@.contains(#[trigger] kept@[j]),
{
    let ghost ev = ents@;
    let mut bmin: Index<i64> = Index::new();
    let mut i: usize = 0;
    while i < ents.len()
        invariant
            i <= ents@.len(),
            ev == ents@,
            t.wf(),
            bmin.wf(),
            forall|c: KeyView| #![trigger bmin.at(c)] bmin.at(c) is Some ==> exists|j: int| 0 <= j < i && (ev[j].ck.fy as int, (*ev[j].ck.uom)@) == c && ev[j].ck.d as int > cls_d(*t, ev[j].ck) && ev[j].ck.d == bmin.at(c)->0,
            forall|j: int| 0 <= j < i && ev[j].ck.d as int > cls_d(*t, ev[j].ck) ==> {
                &&& #[trigger] bmin.at((ev[j].ck.fy as int, (*ev[j].ck.uom)@)) is Some
                &&& bmin.at((ev[j].ck.fy as int, (*ev[j].ck.uom)@))->0 <= ev[j].ck.d
            },
        decreases ents.len() - i,
    {
        let e = &ents[i];
        let dt = thr_get(t, e.ck.fy, &e.ck.uom);
        if e.ck.d > dt {
            let key = kkey(e.ck.fy, &e.ck.uom);
            let ghost old = bmin;
            let better = match bmin.get(&key) {
                Some(m) => e.ck.d < *m,
                None => true,
            };
            if better {
                bmin.put(key, Arc::new(e.ck.d));
            }
            proof {
                assert forall|c: KeyView| #![trigger bmin.at(c)] bmin.at(c) is Some implies exists|j: int| 0 <= j < i + 1 && (ev[j].ck.fy as int, (*ev[j].ck.uom)@) == c && ev[j].ck.d as int > cls_d(*t, ev[j].ck) && ev[j].ck.d == bmin.at(c)->0 by {
                    if better && c == (e.ck.fy as int, (*e.ck.uom)@) {
                        assert(ev[i as int] == *e);
                    } else if bmin.at(c) is Some {
                        let j = choose|j: int| 0 <= j < i && (ev[j].ck.fy as int, (*ev[j].ck.uom)@) == c && ev[j].ck.d as int > cls_d(*t, ev[j].ck) && ev[j].ck.d == old.at(c)->0;
                    }
                }
                assert forall|j: int| 0 <= j < i + 1 && ev[j].ck.d as int > cls_d(*t, ev[j].ck) implies {
                    &&& #[trigger] bmin.at((ev[j].ck.fy as int, (*ev[j].ck.uom)@)) is Some
                    &&& bmin.at((ev[j].ck.fy as int, (*ev[j].ck.uom)@))->0 <= ev[j].ck.d
                } by {
                    if j == i as int {
                        assert(ev[j] == *e);
                    } else {
                        assert(0 <= j < i);
                        if better && (ev[j].ck.fy as int, (*ev[j].ck.uom)@) == (e.ck.fy as int, (*e.ck.uom)@) {
                            assert(bmin.at((e.ck.fy as int, (*e.ck.uom)@))->0 == e.ck.d);
                            assert(old.at((ev[j].ck.fy as int, (*ev[j].ck.uom)@))->0 <= ev[j].ck.d);
                        }
                    }
                }
            }
        } else {
            proof {
                assert forall|j: int| 0 <= j < i + 1 && ev[j].ck.d as int > cls_d(*t, ev[j].ck) implies {
                    &&& #[trigger] bmin.at((ev[j].ck.fy as int, (*ev[j].ck.uom)@)) is Some
                    &&& bmin.at((ev[j].ck.fy as int, (*ev[j].ck.uom)@))->0 <= ev[j].ck.d
                } by {
                    if j == i as int { assert(ev[j] == *e); assert(false); }
                }
                assert forall|c: KeyView| #![trigger bmin.at(c)] bmin.at(c) is Some implies exists|j: int| 0 <= j < i + 1 && (ev[j].ck.fy as int, (*ev[j].ck.uom)@) == c && ev[j].ck.d as int > cls_d(*t, ev[j].ck) && ev[j].ck.d == bmin.at(c)->0 by {
                    let j = choose|j: int| 0 <= j < i && (ev[j].ck.fy as int, (*ev[j].ck.uom)@) == c && ev[j].ck.d as int > cls_d(*t, ev[j].ck) && ev[j].ck.d == bmin.at(c)->0;
                }
            }
        }
        i += 1;
    }
    let mut kept: Vec<Ent> = Vec::new();
    let ghost p = |e: Ent| kept_ent(ev, *t, e);
    let mut i: usize = 0;
    proof { assert(ev.take(0).filter(p) =~= Seq::<Ent>::empty()); }
    while i < ents.len()
        invariant
            i <= ents@.len(),
            ev == ents@,
            t.wf(),
            bmin.wf(),
            p == (|e: Ent| kept_ent(ev, *t, e)),
            forall|c: KeyView| #![trigger bmin.at(c)] bmin.at(c) is Some ==> exists|j: int| 0 <= j < ev.len() && (ev[j].ck.fy as int, (*ev[j].ck.uom)@) == c && ev[j].ck.d as int > cls_d(*t, ev[j].ck) && ev[j].ck.d == bmin.at(c)->0,
            forall|j: int| 0 <= j < ev.len() && ev[j].ck.d as int > cls_d(*t, ev[j].ck) ==> {
                &&& #[trigger] bmin.at((ev[j].ck.fy as int, (*ev[j].ck.uom)@)) is Some
                &&& bmin.at((ev[j].ck.fy as int, (*ev[j].ck.uom)@))->0 <= ev[j].ck.d
            },
            kept@ == ev.take(i as int).filter(p),
            forall|j: int| 0 <= j < kept@.len() ==> ev.contains(#[trigger] kept@[j]),
        decreases ents.len() - i,
    {
        let e = &ents[i];
        let dt = thr_get(t, e.ck.fy, &e.ck.uom);
        let keep_it = if e.ck.d <= dt {
            true
        } else {
            match bmin.get(&kkey(e.ck.fy, &e.ck.uom)) {
                Some(m) => *m == e.ck.d,
                None => false,
            }
        };
        proof {
            assert(ev[i as int] == *e);
            // kept_ent(ev, t, *e) <==> keep_it
            if !(e.ck.d as int <= cls_d(*t, e.ck)) {
                let c = (e.ck.fy as int, (*e.ck.uom)@);
                assert(bmin.at(c) is Some);
                let m = bmin.at(c)->0;
                let j = choose|j: int| 0 <= j < ev.len() && (ev[j].ck.fy as int, (*ev[j].ck.uom)@) == c && ev[j].ck.d as int > cls_d(*t, ev[j].ck) && ev[j].ck.d == m;
                assert(m <= e.ck.d);
                if keep_it {
                    assert forall|x: Ent| ev.contains(x) && same_cls(x.ck, e.ck) && x.ck.d as int > cls_d(*t, e.ck) implies e.ck.d <= #[trigger] x.ck.d by {
                        let jx = choose|jx: int| 0 <= jx < ev.len() && ev[jx] == x;
                        assert(bmin.at((x.ck.fy as int, (*x.ck.uom)@)) == bmin.at(c));
                        assert(m <= x.ck.d);
                    }
                    assert(kept_ent(ev, *t, *e));
                } else {
                    assert(ev.contains(ev[j]));
                    assert(same_cls(ev[j].ck, e.ck));
                    assert(!(e.ck.d <= ev[j].ck.d));
                    assert(!kept_ent(ev, *t, *e));
                }
            }
            assert(keep_it == kept_ent(ev, *t, *e));
            reveal_with_fuel(Seq::filter, 2);
            assert(ev.take(i as int + 1) =~= ev.take(i as int).push(ev[i as int]));
        }
        if keep_it {
            kept.push(copy_ent(e));
        }
        proof {
            let s1 = ev.take(i as int + 1);
            assert(s1.drop_last() =~= ev.take(i as int));
            assert(s1.last() == ev[i as int]);
            assert(kept@ =~= if p(ev[i as int]) { ev.take(i as int).filter(p).push(ev[i as int]) } else { ev.take(i as int).filter(p) });
            assert forall|j: int| 0 <= j < kept@.len() implies ev.contains(#[trigger] kept@[j]) by {
                if keep_it && j == kept@.len() - 1 { assert(kept@[j] == ev[i as int]); }
            }
        }
        i += 1;
    }
    proof { assert(ev.take(ev.len() as int) =~= ev); }
    kept
}

} // verus!
