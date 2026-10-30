// Stage D of an update: a class keeps at most K groups; the worst go back to their units' frontier.
#![allow(unused_imports)]
use vstd::prelude::*;
use std::sync::Arc;
use verus_common::rel::*;
use verus_prooflib::ordered::{Index, Key, KeyView, key_lt, lemma_key_order, lemma_key_trans, compare};
use crate::gen_spec::*;
use crate::model::*;
use crate::esc::*;
use crate::accmap::*;
use crate::grp::*;
use crate::inv::*;
use crate::crops::*;
use crate::crlem::*;
use crate::cnt::*;
use crate::tbl::*;
use crate::place::*;
use crate::plan::*;
use crate::units::*;

verus! {

/// Every group of db accounted for in c is accounted for in n.
pub open spec fn cov_db(db: Db, c: ClassRec, n: ClassRec) -> bool {
    forall|k: GK| #![trigger covered_in(n, k, sumk(db, k), cntk(db, k), absk(db, k))]
        cntk(db, k) > 0 && in_class(c, k) && covered_in(c, k, sumk(db, k), cntk(db, k), absk(db, k)) ==> covered_in(n, k, sumk(db, k), cntk(db, k), absk(db, k))
}

proof fn lemma_pos<V>(ix: Index<V>, q: KeyView) -> (pos: int)
    requires ix.wf(), ix.at(q) is Some,
    ensures 0 <= pos < ix.entries().len(), ix.entries()[pos].0 == q,
{
    ix.model();
    choose|pos: int| #![auto] 0 <= pos < ix.entries().len() && ix.entries()[pos].0 == q
}

proof fn lemma_some_len<V>(ix: Index<V>, q: KeyView)
    requires ix.wf(), ix.at(q) is Some,
    ensures ix.entries().len() > 0,
{
    ix.model();
    let pos = choose|pos: int| #![auto] 0 <= pos < ix.entries().len() && ix.entries()[pos].0 == q;
}

/// The worst kept group g (stored under rk) goes back to the frontier of its unit.
#[verifier::rlimit(200)]
#[verifier::spinoff_prover]
pub fn trim_step(c: &ClassRec, rk: &Key, g: &Arc<Grp>, Ghost(db): Ghost<Db>) -> (n: ClassRec)
    requires
        crec_struct(*c), crec_sem(db, *c),
        c.r.at(rk.view()) == Some(**g),
        forall|q: KeyView| #![trigger c.r.at(q)] c.r.at(q) is Some ==> q == rk.view() || key_lt(q, rk.view()),
    ensures
        crec_struct(n), crec_sem(db, n), crec_head(*c, n),
        forall|q: KeyView| #![trigger n.r.at(q)] n.r.at(q) == if q == rk.view() { None } else { c.r.at(q) },
        cov_db(db, *c, n),
        n.f.entries().len() > 0,
{
    let ghost gv = arc_val(g);
    let n1 = crec_r_del(c, rk);
    proof {
        lemma_r_del_ok(db, *c, n1, rk.view());
        assert(exact_g(db, *c, gv));
        assert(rk.view() == rkey(gv));
    }
    let nk = nkey_of(&g.name);
    let ghost k0 = cls_gk(*c, gv);
    match n1.fb.get(&nk) {
        None => {
            let n = crec_f_put(&n1, g.clone());
            proof {
                assert(forall|q: KeyView| #![trigger n1.r.at(q)] n1.r.at(q) is Some ==> key_lt(q, rkey(gv)) && n1.r.at(q)->0.ik() != gv.ik()) by {
                    assert forall|q: KeyView| n1.r.at(q) is Some implies key_lt(q, rkey(gv)) && n1.r.at(q)->0.ik() != gv.ik() by {
                        assert(c.r.at(q) is Some);
                        assert(q != rkey(gv));
                        assert(q == rkey(c.r.at(q)->0));
                    }
                }
                assert(exact_g(db, n1, gv));
                lemma_f_put_ok(db, n1, n, gv);
                assert(n.f.at(rkey(gv)) is Some);
                lemma_some_len(n.f, rkey(gv));
                assert forall|k: GK| cntk(db, k) > 0 && in_class(*c, k) && covered_in(*c, k, sumk(db, k), cntk(db, k), absk(db, k)) implies covered_in(n, k, sumk(db, k), cntk(db, k), absk(db, k)) by {
                    if k == k0 { assert(sumk(db, k) == gv.sum@ && cntk(db, k) == gv.cnt@); assert(cls_gk(n, gv) == k0); } else { assert(covered_in(n1, k, sumk(db, k), cntk(db, k), absk(db, k))); }
                }
            }
            n
        }
        Some(b) => {
            let ikb = ikey_of(&b.name, b.tag.as_slice(), &b.plabel);
            let rb = rkey_of(b.bits, &ikb);
            let ikg = ikey_of(&g.name, g.tag.as_slice(), &g.plabel);
            let rg = rkey_of(g.bits, &ikg);
            if compare(&rg, &rb) < 0 {
                let n2 = crec_f_del(&n1, &nk);
                let n = crec_f_put(&n2, g.clone());
                proof {
                    let bv = arc_val(&b);
                    assert(n1.fb.at(nkey_v(ov(gv.name))) == Some(bv));
                    assert(rb.view() == rkey(bv) && rg.view() == rkey(gv));
                    assert(forall|q: KeyView| #![trigger n1.r.at(q)] n1.r.at(q) is Some ==> key_lt(q, rkey(gv)) && n1.r.at(q)->0.ik() != gv.ik()) by {
                        assert forall|q: KeyView| n1.r.at(q) is Some implies key_lt(q, rkey(gv)) && n1.r.at(q)->0.ik() != gv.ik() by {
                            assert(c.r.at(q) is Some); assert(q != rkey(gv)); assert(q == rkey(c.r.at(q)->0));
                        }
                    }
                    assert(exact_g(db, n1, gv));
                    lemma_ikey_name_eq2(gv, bv);
                    assert(n1.fb.at(nkey_v(ov(bv.name))) == Some(bv));
                    assert(n.f.at(rkey(gv)) == Some(gv));
                    assert forall|q: KeyView| #![trigger n.f.at(q)] n.f.at(q) == if q == rkey(gv) { Some(gv) } else if q == rkey(bv) { None } else { n1.f.at(q) } by {
                        lemma_key_order(rkey(gv), rkey(bv));
                    }
                    lemma_f_swap_ok(db, n1, n, bv, gv);
                    lemma_some_len(n.f, rkey(gv));
                    assert forall|k: GK| cntk(db, k) > 0 && in_class(*c, k) && covered_in(*c, k, sumk(db, k), cntk(db, k), absk(db, k)) implies covered_in(n, k, sumk(db, k), cntk(db, k), absk(db, k)) by {
                        if k == k0 { assert(sumk(db, k) == gv.sum@ && cntk(db, k) == gv.cnt@); assert(cls_gk(n, gv) == k0); } else { assert(covered_in(n1, k, sumk(db, k), cntk(db, k), absk(db, k))); }
                    }
                }
                n
            } else {
                proof {
                    let bv = arc_val(&b);
                    assert(rb.view() == rkey(bv) && rg.view() == rkey(gv));
                    assert(n1.fb.at(nkey_v(ov(gv.name))) == Some(bv));
                    assert(n1.f.at(rkey(bv)) == n1.fb.at(nkey_v(ov(bv.name))));
                    lemma_some_len(n1.f, rkey(bv));
                    assert(okbits(gv.bits, gv.sum@, gv.cnt@));
                    assert(rkey_gk(gv.bits, k0) == rkey(gv));
                    assert(covered_in(n1, k0, gv.sum@, gv.cnt@, absk(db, k0)));
                    assert forall|k: GK| cntk(db, k) > 0 && in_class(*c, k) && covered_in(*c, k, sumk(db, k), cntk(db, k), absk(db, k)) implies covered_in(n1, k, sumk(db, k), cntk(db, k), absk(db, k)) by {
                        if k == k0 { assert(sumk(db, k) == gv.sum@ && cntk(db, k) == gv.cnt@); } else { assert(covered_in(n1, k, sumk(db, k), cntk(db, k), absk(db, k))); }
                    }
                }
                n1
            }
        }
    }
}

proof fn lemma_ikey_name_eq2(a: Grp, b: Grp)
    requires nkey_v(ov(a.name)) == nkey_v(ov(b.name)),
    ensures ov(a.name) == ov(b.name),
{
    lemma_esc_opt_split(ov(a.name), Seq::empty(), ov(b.name), Seq::empty());
}

/// A class keeps at most K_MAX groups: the rest go back to the frontier.
#[verifier::rlimit(300)]
#[verifier::spinoff_prover]
pub fn trim_class(c: &ClassRec, Ghost(db): Ghost<Db>) -> (r: Option<ClassRec>)
    requires crec_struct(*c), crec_sem(db, *c), crec_full(*c),
    ensures
        r matches Some(n) ==> crec_struct(n) && crec_sem(db, n) && crec_full(n) && crec_head(*c, n) && cov_db(db, *c, n),
        r is None ==> c.r.entries().len() <= K_MAX,
{
    let ents = c.r.rows();
    let ghost es = c.r.entries();
    proof { c.r.model(); }
    if ents.len() <= K_MAX {
        return None;
    }
    let ghost len = ents.len() as int;
    // the worst group
    let last = ents.len() - 1;
    proof {
        assert(es[last as int] == (ents@[last as int].0.view(), arc_val(&ents[last as int].1)));
        assert(c.r.at(es[last as int].0) == Some(es[last as int].1));
        assert forall|a: int, b: int| 0 <= a < b < es.len() implies key_lt((#[trigger] es[a]).0, (#[trigger] es[b]).0) by {}
        assert forall|q: KeyView| c.r.at(q) is Some implies q == ents@[last as int].0.view() || key_lt(q, ents@[last as int].0.view()) by {
            let pos = lemma_pos(c.r, q);
            if pos < last as int { assert(key_lt(es[pos].0, es[last as int].0)); }
            else { assert(pos == last as int); }
        }
    }
    let mut n = trim_step(c, &ents[last].0, &ents[last].1, Ghost(db));
    let mut i = last;
    while i > K_MAX
        invariant
            K_MAX <= i < ents.len(), crec_struct(*c), crec_sem(db, *c), crec_struct(n), crec_sem(db, n), crec_head(*c, n),
            ents@.map_values(|row: (Key, Arc<Grp>)| (row.0.view(), *row.1)) == es, es == c.r.entries(), len == ents.len() as int,
            forall|pos: int| #![trigger es[pos]] 0 <= pos < es.len() ==> c.r.at(es[pos].0) == Some(es[pos].1),
            forall|a: int, b: int| 0 <= a < b < es.len() ==> key_lt((#[trigger] es[a]).0, (#[trigger] es[b]).0),
            forall|q: KeyView| #![trigger n.r.at(q)] n.r.at(q) == if (exists|j: int| i as int <= j < len && #[trigger] es[j].0 == q) { None } else { c.r.at(q) },
            cov_db(db, *c, n), n.f.entries().len() > 0,
        decreases i,
    {
        let k = i - 1;
        proof {
            assert(es[k as int] == (ents@[k as int].0.view(), arc_val(&ents@[k as int].1)));
            assert(c.r.at(es[k as int].0) == Some(es[k as int].1));
            assert(n.r.at(es[k as int].0) == Some(es[k as int].1)) by {
                assert(!(exists|j: int| i as int <= j < len && es[j].0 == es[k as int].0)) by {
                    if exists|j: int| i as int <= j < len && es[j].0 == es[k as int].0 { let j = choose|j: int| i as int <= j < len && es[j].0 == es[k as int].0; assert(key_lt(es[k as int].0, es[j].0)); lemma_key_order(es[k as int].0, es[j].0); }
                }
            }
            assert forall|q: KeyView| n.r.at(q) is Some implies q == ents@[k as int].0.view() || key_lt(q, ents@[k as int].0.view()) by {
                assert(c.r.at(q) is Some);
                let pos = lemma_pos(c.r, q);
                assert(!(exists|j: int| i as int <= j < len && es[j].0 == q));
                if pos >= i as int { assert(es[pos].0 == q); assert(exists|j: int| i as int <= j < len && es[j].0 == q); }
                assert(pos < i as int);
                if pos < k as int { assert(key_lt(es[pos].0, es[k as int].0)); } else { assert(pos == k as int); }
            }
        }
        let ghost n0 = n;
        n = trim_step(&n, &ents[k].0, &ents[k].1, Ghost(db));
        proof {
            assert forall|q: KeyView| n.r.at(q) == if (exists|j: int| k as int <= j < len && es[j].0 == q) { None } else { c.r.at(q) } by {
                if q == es[k as int].0 {} else {}
            }
            assert(cov_db(db, *c, n)) by {
                assert forall|kk: GK| cntk(db, kk) > 0 && in_class(*c, kk) && covered_in(*c, kk, sumk(db, kk), cntk(db, kk), absk(db, kk)) implies covered_in(n, kk, sumk(db, kk), cntk(db, kk), absk(db, kk)) by {
                    assert(covered_in(n0, kk, sumk(db, kk), cntk(db, kk), absk(db, kk)));
                    assert(in_class(n0, kk));
                }
            }
        }
        i = k;
    }
    proof {
        // exactly the first K_MAX groups remain
        let keys = Seq::new(K_MAX as nat, |j: int| es[j].0);
        assert(keys.no_duplicates()) by {
            assert forall|a: int, b: int| 0 <= a < b < keys.len() implies keys[a] != keys[b] by { assert(key_lt(es[a].0, es[b].0)); lemma_key_order(es[a].0, es[b].0); }
        }
        assert forall|j: int| 0 <= j < keys.len() implies n.r.at(#[trigger] keys[j]) is Some by {
            assert(c.r.at(es[j].0) is Some);
            assert(!(exists|jj: int| K_MAX <= jj < len && es[jj].0 == es[j].0)) by {
                if exists|jj: int| K_MAX <= jj < len && es[jj].0 == es[j].0 { let jj = choose|jj: int| K_MAX <= jj < len && es[jj].0 == es[j].0; assert(key_lt(es[j].0, es[jj].0)); lemma_key_order(es[j].0, es[jj].0); }
            }
        }
        lemma_len_ge(keys, n.r);
        assert(crec_full(n));
    }
    Some(n)
}

/// Trims every class of the set.
#[verifier::rlimit(200)]
#[verifier::spinoff_prover]
pub fn trim_all(t: &mut Index<ClassRec>, tcls: &Index<ClassRef>, Ghost(db2): Ghost<Db>, Ghost(p): Ghost<spec_fn(GK) -> bool>)
    requires tbl_wf(db2, *old(t)), full_all(*old(t)), cov_where(db2, *old(t), p), cset_ok(*tcls),
    ensures tbl_wf(db2, *final(t)), full_all(*final(t)), cov_where(db2, *final(t), p),
{
    let ents = tcls.rows();
    let ghost es = tcls.entries();
    proof { tcls.model(); }
    let mut i = 0;
    while i < ents.len()
        invariant
            i <= ents.len(), cset_ok(*tcls), tcls.wf(),
            ents@.map_values(|row: (Key, Arc<ClassRef>)| (row.0.view(), *row.1)) == es, es == tcls.entries(),
            tbl_wf(db2, *t), full_all(*t), cov_where(db2, *t, p),
        decreases ents.len() - i,
    {
        let key = &ents[i].0;
        match t.get(key) {
            None => {}
            Some(c) => {
                let ghost t0 = *t;
                let ghost cv = arc_val(&c);
                proof { assert(t0.at(key.view()) == Some(cv)); assert(cls_key(cv) == key.view()); }
                match trim_class(&c, Ghost(db2)) {
                    None => {}
                    Some(n) => {
                        let ghost nv = n;
                        cls_store(t, n);
                        proof {
                            assert(cls_key(nv) == cls_key(cv));
                            assert forall|ok: KeyView| t.at(ok) is Some implies ({ let c2 = t.at(ok)->0; ok == cls_key(c2) && crec_struct(c2) && crec_sem(db2, c2) && crec_full(c2) }) by {
                                if ok != key.view() { assert(t0.at(ok) is Some); }
                            }
                            assert forall|k: GK| cntk(db2, k) > 0 && p(k) implies #[trigger] covered(db2, *t, k) by {
                                let ok2 = okey(k.fy, k.stmt, k.uom);
                                assert(covered(db2, t0, k));
                                if ok2 == key.view() {
                                    lemma_okey_inj(k.fy, k.stmt, k.uom, cv.fy as int, cv.stmt@, cv.uom@);
                                    assert(in_class(cv, k));
                                    assert(covered_in(cv, k, sumk(db2, k), cntk(db2, k), absk(db2, k)));
                                }
                            }
                        }
                    }
                }
            }
        }
        i += 1;
    }
}

} // verus!
