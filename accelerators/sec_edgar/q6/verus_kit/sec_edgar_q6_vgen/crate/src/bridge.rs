// From the old state and the plan to the state stage C starts from.
#![allow(unused_imports)]
use vstd::prelude::*;
use std::sync::Arc;
use verus_common::rel::*;
use verus_prooflib::ordered::{Index, Key, KeyView, key_lt, lemma_key_order};
use crate::gen_spec::*;
use crate::model::*;
use crate::esc::*;
use crate::accmap::*;
use crate::grp::*;
use crate::inv::*;
use crate::tbl::*;
use crate::units::*;
use crate::kept::*;
use crate::plan::*;
use crate::evict::*;
use crate::place::*;
use crate::cnt::*;
use crate::ucls::*;

verus! {

/// Units outside u keep their groups: db2 has the groups db has.
pub open spec fn frame_ok(db: Db, db2: Db, u: Index<UnitRef>) -> bool {
    forall|k: GK| #![trigger cntk(db2, k)] !u_has(u, k.fy, k.name) ==> cntk(db2, k) == cntk(db, k) && sumk(db2, k) == sumk(db, k) && absk(db2, k) == absk(db, k)
}

/// Every record's groups are of units outside u.
pub open spec fn notu_rec(c: ClassRec, u: Index<UnitRef>) -> bool {
    &&& forall|q: KeyView| #![trigger c.r.at(q)] c.r.at(q) is Some ==> !wn(u, c.fy as int, nkey_v(ov(c.r.at(q)->0.name)))
    &&& forall|q: KeyView| #![trigger c.f.at(q)] c.f.at(q) is Some ==> !wn(u, c.fy as int, nkey_v(ov(c.f.at(q)->0.name)))
    &&& forall|q: KeyView| #![trigger c.hp.at(q)] c.hp.at(q) is Some ==> !wn(u, c.fy as int, q)
    &&& forall|q: KeyView| #![trigger c.hn.at(q)] c.hn.at(q) is Some ==> !wn(u, c.fy as int, q)
}

/// A record whose groups are of units outside u is exact for db2 when it is for db.
pub proof fn lemma_sem_switch(db: Db, db2: Db, c: ClassRec, u: Index<UnitRef>)
    requires crec_sem(db, c), notu_rec(c, u), frame_ok(db, db2, u),
    ensures crec_sem(db2, c),
{
    assert forall|q: KeyView| c.r.at(q) is Some implies exact_g(db2, c, c.r.at(q)->0) by {
        let g = c.r.at(q)->0;
        assert(exact_g(db, c, g));
        let k = cls_gk(c, g);
        assert(!u_has(u, k.fy, k.name));
    }
    assert forall|q: KeyView| c.f.at(q) is Some implies exact_g(db2, c, c.f.at(q)->0) by {
        let g = c.f.at(q)->0;
        assert(exact_g(db, c, g));
        let k = cls_gk(c, g);
        assert(!u_has(u, k.fy, k.name));
    }
    assert forall|nk: KeyView| c.hp.at(nk) is Some implies has_cand(db2, c, nk, true) by {
        assert(has_cand(db, c, nk, true));
        let k = choose|k: GK| #![trigger cntk(db, k)] cntk(db, k) > 0 && in_class(c, k) && nkey_v(k.name) == nk && bad_g(sumk(db, k), absk(db, k));
        assert(k.fy == c.fy as int);
        assert(!u_has(u, k.fy, k.name)) by { assert(wn(u, c.fy as int, nk) == u_has(u, k.fy, k.name)); }
        assert(!u_has(u, k.fy, k.name) ==> cntk(db2, k) == cntk(db, k) && sumk(db2, k) == sumk(db, k));
        assert(cntk(db2, k) > 0 && bad_g(sumk(db2, k), absk(db2, k)));
    }
    assert forall|nk: KeyView| c.hn.at(nk) is Some implies has_cand(db2, c, nk, false) by {
        assert(has_cand(db, c, nk, false));
        let k = choose|k: GK| #![trigger cntk(db, k)] cntk(db, k) > 0 && in_class(c, k) && nkey_v(k.name) == nk && sumk(db, k) < -max_val();
        assert(k.fy == c.fy as int);
        assert(!u_has(u, k.fy, k.name)) by { assert(wn(u, c.fy as int, nk) == u_has(u, k.fy, k.name)); }
        assert(!u_has(u, k.fy, k.name) ==> cntk(db2, k) == cntk(db, k) && sumk(db2, k) == sumk(db, k));
        assert(cntk(db2, k) > 0 && sumk(db2, k) < -max_val());
    }
}

/// Every class some unit of the set has an entry in is in the class set.
pub open spec fn class_cover_u(cls: Index<ClassRec>, w: Index<UnitRef>, cw: Index<ClassRef>) -> bool {
    forall|ok: KeyView, nk: KeyView| #![trigger cls.at(ok), unit_in(cls.at(ok)->0, nk)]
        cls.at(ok) is Some && unit_in(cls.at(ok)->0, nk) && wn(w, cls.at(ok)->0.fy as int, nk) ==> cw.at(ok) is Some
}

/// The stored groups of a class outside the class set are of units outside the set.
pub proof fn lemma_non_cw(cls0: Index<ClassRec>, w: Index<UnitRef>, cw: Index<ClassRef>, ok: KeyView)
    requires class_cover_u(cls0, w, cw), cls0.at(ok) is Some, cw.at(ok) is None,
    ensures ({
        let c = cls0.at(ok)->0;
        &&& forall|q: KeyView| #![trigger c.r.at(q)] c.r.at(q) is Some ==> !wn(w, c.fy as int, nkey_v(ov(c.r.at(q)->0.name)))
        &&& forall|q: KeyView| #![trigger c.f.at(q)] c.f.at(q) is Some ==> !wn(w, c.fy as int, nkey_v(ov(c.f.at(q)->0.name)))
        &&& forall|q: KeyView| #![trigger c.hp.at(q)] c.hp.at(q) is Some ==> !wn(w, c.fy as int, q)
        &&& forall|q: KeyView| #![trigger c.hn.at(q)] c.hn.at(q) is Some ==> !wn(w, c.fy as int, q)
    }),
{
    let c = cls0.at(ok)->0;
    assert forall|q: KeyView| c.r.at(q) is Some implies !wn(w, c.fy as int, nkey_v(ov(c.r.at(q)->0.name))) by { assert(unit_in(c, nkey_v(ov(c.r.at(q)->0.name)))); }
    assert forall|q: KeyView| c.f.at(q) is Some implies !wn(w, c.fy as int, nkey_v(ov(c.f.at(q)->0.name))) by { assert(unit_in(c, nkey_v(ov(c.f.at(q)->0.name)))); }
    assert forall|q: KeyView| c.hp.at(q) is Some implies !wn(w, c.fy as int, q) by { assert(unit_in(c, q)); }
    assert forall|q: KeyView| c.hn.at(q) is Some implies !wn(w, c.fy as int, q) by { assert(unit_in(c, q)); }
}

pub proof fn lemma_subset_key(u: Index<UnitRef>, w: Index<UnitRef>, q: KeyView)
    requires uset_ok(u), subset_u(u, w), u.at(q) is Some,
    ensures w.at(q) is Some,
{
    let ur = u.at(q)->0;
    assert(q == ukey(ur.fy as int, ov(ur.name)));
    assert(u_has(u, ur.fy as int, ov(ur.name)));
    assert(u_has(w, ur.fy as int, ov(ur.name)));
}

proof fn lemma_empty_entries<V>(ix: Index<V>)
    requires ix.wf(), forall|q: KeyView| #![trigger ix.at(q)] ix.at(q) is None,
    ensures ix.entries().len() == 0,
{
    ix.model();
    if ix.entries().len() > 0 { assert(ix.at(ix.entries()[0].0) is Some); }
}

/// After the eviction a class keeps LIMIT groups or has an empty frontier.
#[verifier::rlimit(200)]
#[verifier::spinoff_prover]
pub proof fn lemma_full_evicted(c0: ClassRec, n: ClassRec, u: Index<UnitRef>, w: Index<UnitRef>, refill: bool)
    requires
        crec_struct(c0), crec_full(c0), uset_ok(u), subset_u(u, w),
        evict_rel(c0, n, u, w, pick_of(c0, u, refill)),
        refill && notu_keys(c0.r.entries(), c0.fy as int, u).len() < k_limit() ==>
            forall|k: KeyView| vsel(c0.f.entries(), c0.fy as int, u).contains(k) ==> wn(w, c0.fy as int, nkey_v(ov(c0.f.at(k)->0.name))),
        !refill ==> forall|q: KeyView| #![trigger c0.r.at(q)] c0.r.at(q) is Some ==> !wn(u, c0.fy as int, nkey_v(ov(c0.r.at(q)->0.name))),
    ensures crec_full(n),
{
    c0.r.model();
    let es = c0.r.entries();
    assert(ordered_es(es));
    let pk = pick_of(c0, u, refill);
    let nu = notu_keys(es, c0.fy as int, u);
    lemma_notu_sorted(es, c0.fy as int, u);
    if nu.len() >= k_limit() {
        assert forall|i: int, j: int| 0 <= i < j < nu.len() implies nu[i] != nu[j] by { lemma_key_order(nu[i], nu[j]); }
        assert(nu.no_duplicates());
        assert forall|i: int| 0 <= i < nu.len() implies n.r.at(#[trigger] nu[i]) is Some by {
            assert(nu.contains(nu[i]));
            let j = choose|j: int| 0 <= j < es.len() && es[j].0 == nu[i] && !u_has(u, c0.fy as int, ov(es[j].1.name));
            assert(c0.r.at(es[j].0) == Some(es[j].1));
            lemma_pick_facts_ok(c0, u, w, refill);
            if mvd(pk, nu[i]) { let t = choose|t: int| 0 <= t < pk.len() && rkey(pk[t]) == nu[i]; assert(n.r.at(rkey(pk[t])) == Some(pk[t])); }
            else { assert(n.r.at(nu[i]) == c0.r.at(nu[i])); assert(wn(u, c0.fy as int, nkey_v(ov(es[j].1.name))) == u_has(u, c0.fy as int, ov(es[j].1.name))); }
        }
        lemma_len_ge(nu, n.r);
    } else if refill {
        lemma_pick_facts_ok(c0, u, w, refill);
        let vs = vsel(c0.f.entries(), c0.fy as int, u);
        c0.f.model();
        let fes = c0.f.entries();
        assert(ordered_es(fes));
        lemma_vsel_prefix(fes, c0.fy as int, u);
        if vs.len() >= k_refill() {
            assert(vs.no_duplicates()) by { assert forall|i: int, j: int| 0 <= i < j < vs.len() implies vs[i] != vs[j] by { lemma_key_order(vs[i], vs[j]); } }
            assert forall|i: int| 0 <= i < vs.len() implies n.r.at(#[trigger] vs[i]) is Some by {
                assert(pk[i] == c0.f.at(vs[i])->0);
                assert(rkey(pk[i]) == vs[i]);
                assert(n.r.at(rkey(pk[i])) == Some(pk[i]));
            }
            lemma_len_ge(vs, n.r);
        } else {
            lemma_notu_sorted(fes, c0.fy as int, u);
            let nk = notu_keys(fes, c0.fy as int, u);
            assert(vs =~= nk);
            assert forall|q: KeyView| n.f.at(q) is None by {
                if n.f.at(q) is Some {
                    let h = c0.f.at(q)->0;
                    assert(!wn(w, c0.fy as int, nkey_v(ov(h.name))));
                    assert(!u_has(u, c0.fy as int, ov(h.name)));
                    let pos = lemma_pos2(c0.f, q);
                    assert(nk.contains(q));
                    assert(vs.contains(q));
                }
            }
            lemma_empty_entries(n.f);
        }
    } else {
        // nothing of u was stored here and nothing moved: the kept groups stay
        assert forall|q: KeyView| c0.r.at(q) is Some implies n.r.at(q) is Some by {
            assert(pk =~= Seq::<Grp>::empty());
            assert(!mvd(pk, q));
        }
        lemma_len_le(c0.r, n.r);
        if c0.f.entries().len() == 0 {
            assert forall|q: KeyView| n.f.at(q) is None by { if n.f.at(q) is Some { assert(c0.f.at(q) is Some); lemma_some_len2(c0.f, q); } }
            lemma_empty_entries(n.f);
        }
    }
}

proof fn lemma_pos2<V>(ix: Index<V>, q: KeyView) -> (pos: int)
    requires ix.wf(), ix.at(q) is Some,
    ensures 0 <= pos < ix.entries().len(), ix.entries()[pos].0 == q,
{
    ix.model();
    choose|pos: int| #![auto] 0 <= pos < ix.entries().len() && ix.entries()[pos].0 == q
}

proof fn lemma_some_len2<V>(ix: Index<V>, q: KeyView)
    requires ix.wf(), ix.at(q) is Some,
    ensures ix.entries().len() > 0,
{
    let p = lemma_pos2(ix, q);
}

proof fn lemma_pick_facts_ok(c: ClassRec, u: Index<UnitRef>, w: Index<UnitRef>, refill: bool)
    requires
        crec_struct(c), uset_ok(u), subset_u(u, w),
        refill && notu_keys(c.r.entries(), c.fy as int, u).len() < k_limit() ==>
            forall|k: KeyView| vsel(c.f.entries(), c.fy as int, u).contains(k) ==> wn(w, c.fy as int, nkey_v(ov(c.f.at(k)->0.name))),
    ensures
        forall|i: int| 0 <= i < pick_of(c, u, refill).len() ==> c.f.at(rkey(#[trigger] pick_of(c, u, refill)[i])) == Some(pick_of(c, u, refill)[i]),
        refill && notu_keys(c.r.entries(), c.fy as int, u).len() < k_limit() ==> pick_of(c, u, refill).len() == vsel(c.f.entries(), c.fy as int, u).len()
            && forall|i: int| 0 <= i < pick_of(c, u, refill).len() ==> pick_of(c, u, refill)[i] == c.f.at(vsel(c.f.entries(), c.fy as int, u)[i])->0 && rkey(pick_of(c, u, refill)[i]) == vsel(c.f.entries(), c.fy as int, u)[i],
{
    lemma_pick_facts(c, u, w, refill);
    let pk = pick_of(c, u, refill);
    if refill && notu_keys(c.r.entries(), c.fy as int, u).len() < k_limit() {
        let vs = vsel(c.f.entries(), c.fy as int, u);
        assert(pk == vs.map_values(|k: KeyView| c.f.at(k)->0));
        assert(pk.len() == vs.len());
        assert forall|i: int| 0 <= i < pk.len() implies pk[i] == c.f.at(vs[i])->0 && rkey(pk[i]) == vs[i] by {
            assert(pk[i] == c.f.at(vs[i])->0);
        }
    }
}

/// The records of a class set after the eviction.
#[verifier::rlimit(400)]
#[verifier::spinoff_prover]
pub proof fn lemma_bridge(db: Db, db2: Db, cls0: Index<ClassRec>, t: Index<ClassRec>, kept: GMap,
    u: Index<UnitRef>, w: Index<UnitRef>, ct: Index<ClassRef>, cw: Index<ClassRef>)
    requires
        cls_ok(db, cls0), uset_ok(u), uset_ok(w), subset_u(u, w), frame_ok(db, db2, u),
        class_cover_u(cls0, w, cw), class_cover_u(cls0, u, ct), refill_ok(cls0, ct, u, w),
        evict_tbl(cls0, t, cw, ct, u, w), kept_tbl(cls0, t, cw, u, w, kept),
    ensures mid_ok(db2, t, kept, u, w, no_done()),
{
    assert(tbl_str(cls0)) by {
        assert forall|ok: KeyView| cls0.at(ok) is Some implies ok == cls_key(cls0.at(ok)->0) && crec_struct(cls0.at(ok)->0) by {}
    }
    // each record of t
    assert forall|ok: KeyView| t.at(ok) is Some implies ({
        let n = t.at(ok)->0;
        &&& cls0.at(ok) is Some
        &&& ok == cls_key(n) && crec_struct(n) && crec_sem(db2, n) && crec_full(n)
        &&& forall|q: KeyView| #![trigger n.f.at(q)] n.f.at(q) is Some ==> !wn(w, n.fy as int, nkey_v(ov(n.f.at(q)->0.name)))
        &&& forall|q: KeyView| #![trigger n.r.at(q)] n.r.at(q) is Some ==> !wn(u, n.fy as int, nkey_v(ov(n.r.at(q)->0.name)))
        &&& forall|q: KeyView| #![trigger n.r.at(q)] n.r.at(q) is Some && wn(w, n.fy as int, nkey_v(ov(n.r.at(q)->0.name))) ==> gm_at(kept, ok, n.r.at(q)->0.ik()) == n.r.at(q)
        &&& forall|k: GK| #![trigger covered_in(n, k, sumk(db, k), cntk(db, k), absk(db, k))]
            in_class(cls0.at(ok)->0, k) && cntk(db, k) > 0 && covered_in(cls0.at(ok)->0, k, sumk(db, k), cntk(db, k), absk(db, k)) && !u_has(w, k.fy, k.name) ==> covered_in(n, k, sumk(db, k), cntk(db, k), absk(db, k))
    }) by {
        assert(cls0.at(ok) is Some);
        let c0 = cls0.at(ok)->0;
        let n = t.at(ok)->0;
        assert(ok == cls_key(c0) && crec_struct(c0) && crec_sem(db, c0) && crec_full(c0));
        if cw.at(ok) is Some {
            let refill = ct.at(ok) is Some;
            assert(evict_rel(c0, n, u, w, pick_of(c0, u, refill)));
            assert(refill && notu_keys(c0.r.entries(), c0.fy as int, u).len() < k_limit() ==>
                forall|k: KeyView| vsel(c0.f.entries(), c0.fy as int, u).contains(k) ==> wn(w, c0.fy as int, nkey_v(ov(c0.f.at(k)->0.name)))) by {
                if refill && notu_keys(c0.r.entries(), c0.fy as int, u).len() < k_limit() {
                    assert forall|k: KeyView| vsel(c0.f.entries(), c0.fy as int, u).contains(k) implies wn(w, c0.fy as int, nkey_v(ov(c0.f.at(k)->0.name))) by {
                        assert(ct.at(ok) is Some && cls0.at(ok) is Some);
                    }
                }
            }
            lemma_evict_ok(db, c0, n, u, w, refill);
            lemma_pick_facts_ok(c0, u, w, refill);
            lemma_pick_facts(c0, u, w, refill);
            assert(cls_key(n) == cls_key(c0));
            let pk = pick_of(c0, u, refill);
            assert(notu_rec(n, u)) by {
                assert forall|q: KeyView| n.r.at(q) is Some implies !wn(u, n.fy as int, nkey_v(ov(n.r.at(q)->0.name))) by {
                    if mvd(pk, q) { let i = choose|i: int| 0 <= i < pk.len() && rkey(pk[i]) == q; assert(n.r.at(rkey(pk[i])) == Some(pk[i])); }
                    else { assert(c0.r.at(q) is Some); }
                }
                assert forall|q: KeyView| n.f.at(q) is Some implies !wn(u, n.fy as int, nkey_v(ov(n.f.at(q)->0.name))) by {
                    assert(!wn(w, c0.fy as int, nkey_v(ov(c0.f.at(q)->0.name))));
                    let nk = nkey_v(ov(c0.f.at(q)->0.name));
                    assert(wn(u, c0.fy as int, nk) ==> wn(w, c0.fy as int, nk)) by { if wn(u, c0.fy as int, nk) { assert(u_has(u, c0.fy as int, ov(c0.f.at(q)->0.name))); } }
                }
                assert forall|q: KeyView| n.hp.at(q) is Some implies !wn(u, n.fy as int, q) by {
                    assert(c0.hp.at(q) is Some && !wn(w, c0.fy as int, q));
                    if wn(u, c0.fy as int, q) { lemma_subset_key(u, w, (c0.fy as int, q.1)); }
                }
                assert forall|q: KeyView| n.hn.at(q) is Some implies !wn(u, n.fy as int, q) by {
                    assert(c0.hn.at(q) is Some && !wn(w, c0.fy as int, q));
                    if wn(u, c0.fy as int, q) { lemma_subset_key(u, w, (c0.fy as int, q.1)); }
                }
            }
            lemma_sem_switch(db, db2, n, u);
            assert(!refill ==> forall|q: KeyView| #![trigger c0.r.at(q)] c0.r.at(q) is Some ==> !wn(u, c0.fy as int, nkey_v(ov(c0.r.at(q)->0.name)))) by {
                if !refill {
                    assert forall|q: KeyView| c0.r.at(q) is Some implies !wn(u, c0.fy as int, nkey_v(ov(c0.r.at(q)->0.name))) by {
                        assert(unit_in(c0, nkey_v(ov(c0.r.at(q)->0.name))));
                    }
                }
            }
            lemma_full_evicted(c0, n, u, w, refill);
            assert forall|q: KeyView| n.r.at(q) is Some && wn(w, n.fy as int, nkey_v(ov(n.r.at(q)->0.name))) implies gm_at(kept, ok, n.r.at(q)->0.ik()) == n.r.at(q) by {
                assert(t.at(ok) is Some && cw.at(ok) is Some);
            }
        } else {
            assert(t.at(ok) == cls0.at(ok));
            lemma_non_cw(cls0, w, cw, ok);
            assert(forall|q: KeyView| c0.r.at(q) is Some ==> !wn(w, c0.fy as int, nkey_v(ov(c0.r.at(q)->0.name))));
            assert(notu_rec(c0, u)) by {
                assert forall|q: KeyView| c0.hp.at(q) is Some implies !wn(u, c0.fy as int, q) by { if wn(u, c0.fy as int, q) { lemma_subset_key(u, w, (c0.fy as int, q.1)); } }
                assert forall|q: KeyView| c0.r.at(q) is Some implies !wn(u, c0.fy as int, nkey_v(ov(c0.r.at(q)->0.name))) by {
                    let nk = nkey_v(ov(c0.r.at(q)->0.name));
                    if wn(u, c0.fy as int, nk) { assert(u_has(u, c0.fy as int, ov(c0.r.at(q)->0.name))); assert(u_has(w, c0.fy as int, ov(c0.r.at(q)->0.name))); assert(wn(w, c0.fy as int, nk)); }
                }
                assert forall|q: KeyView| c0.f.at(q) is Some implies !wn(u, c0.fy as int, nkey_v(ov(c0.f.at(q)->0.name))) by {
                    let nk = nkey_v(ov(c0.f.at(q)->0.name));
                    if wn(u, c0.fy as int, nk) { assert(u_has(u, c0.fy as int, ov(c0.f.at(q)->0.name))); assert(u_has(w, c0.fy as int, ov(c0.f.at(q)->0.name))); assert(wn(w, c0.fy as int, nk)); }
                }
                assert forall|q: KeyView| c0.hn.at(q) is Some implies !wn(u, c0.fy as int, q) by {
                    if wn(u, c0.fy as int, q) { lemma_subset_key(u, w, (c0.fy as int, q.1)); }
                }
            }
            lemma_sem_switch(db, db2, c0, u);
        }
    }
    assert(tbl_wf(db2, t)) by {
        assert forall|ok: KeyView| t.at(ok) is Some implies ({ let c = t.at(ok)->0; ok == cls_key(c) && crec_struct(c) && crec_sem(db2, c) }) by {}
    }
    assert(cov_where(db2, t, mid_cov(w, no_done()))) by {
        assert forall|k: GK| cntk(db2, k) > 0 && mid_cov(w, no_done())(k) implies #[trigger] covered(db2, t, k) by {
            assert(!u_has(w, k.fy, k.name));
            assert(!u_has(u, k.fy, k.name)) by { if u_has(u, k.fy, k.name) { assert(u_has(w, k.fy, k.name)); } }
            assert(cntk(db, k) > 0 && sumk(db2, k) == sumk(db, k) && cntk(db2, k) == cntk(db, k));
            assert(covered(db, cls0, k));
            let ok = okey(k.fy, k.stmt, k.uom);
            assert(cls0.at(ok) is Some);
            let c0 = cls0.at(ok)->0;
            lemma_okey_inj(k.fy, k.stmt, k.uom, c0.fy as int, c0.stmt@, c0.uom@);
            assert(in_class(c0, k));
            assert(t.at(ok) is Some);
        }
    }
    assert(ks_ok(kept, t)) by {
        assert forall|ok: KeyView, ik: KeyView| gm_at(kept, ok, ik) is Some implies ({
            let g = gm_at(kept, ok, ik)->0;
            t.at(ok) is Some && g.ik() == ik && t.at(ok)->0.r.at(rkey(g)) == Some(g)
        }) by {}
    }
    assert(purge_ok(t, kept, u, w, no_done())) by {
        assert forall|ok: KeyView| t.at(ok) is Some implies ({
            let c = t.at(ok)->0;
            &&& forall|q: KeyView| #![trigger c.f.at(q)] c.f.at(q) is Some && wn(w, c.fy as int, nkey_v(ov(c.f.at(q)->0.name))) ==> no_done()(cls_gk(c, c.f.at(q)->0))
            &&& forall|q: KeyView| #![trigger c.r.at(q)] c.r.at(q) is Some && wn(u, c.fy as int, nkey_v(ov(c.r.at(q)->0.name))) ==> no_done()(cls_gk(c, c.r.at(q)->0))
            &&& forall|q: KeyView| #![trigger c.r.at(q)] c.r.at(q) is Some && wn(w, c.fy as int, nkey_v(ov(c.r.at(q)->0.name))) && !wn(u, c.fy as int, nkey_v(ov(c.r.at(q)->0.name))) ==>
                no_done()(cls_gk(c, c.r.at(q)->0)) || gm_at(kept, ok, c.r.at(q)->0.ik()) == c.r.at(q)
        }) by {}
    }
    assert(full_all(t)) by {
        assert forall|ok: KeyView| t.at(ok) is Some implies crec_full(t.at(ok)->0) by {}
    }
}

} // verus!
