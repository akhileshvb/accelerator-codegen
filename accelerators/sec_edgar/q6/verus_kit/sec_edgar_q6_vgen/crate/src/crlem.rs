// Each record update keeps the record well formed and exact, and the groups it did not touch accounted for.
#![allow(unused_imports)]
use vstd::prelude::*;
use std::sync::Arc;
use verus_common::rel::*;
use verus_common::bytes::*;
use verus_prooflib::ordered::{Index, Key, KeyView, key_lt, lemma_key_order, lemma_key_trans};
use verus_prooflib::wide::*;
use verus_prooflib::float::*;
use crate::gen_spec::*;
use crate::model::*;
use crate::esc::*;
use crate::accmap::*;
use crate::f64bits::*;
use crate::grp::*;
use crate::inv::*;
use crate::crops::*;

verus! {

/// Every group accounted for in c is accounted for in n.
pub open spec fn cov_mono(c: ClassRec, n: ClassRec) -> bool {
    forall|k: GK, s: int, m: int| #![trigger covered_in(n, k, s, m)] covered_in(c, k, s, m) ==> covered_in(n, k, s, m)
}

pub proof fn lemma_r_put_ok(db: Db, c: ClassRec, n: ClassRec, g: Grp)
    requires
        crec_struct(c), crec_sem(db, c), crec_head(c, n), n.r.wf(),
        forall|q: KeyView| #![trigger n.r.at(q)] n.r.at(q) == if q == rkey(g) { Some(g) } else { c.r.at(q) },
        ix_same(n.f, c.f), ix_same(n.fb, c.fb), ix_same(n.hp, c.hp), ix_same(n.hn, c.hn),
        exact_g(db, c, g),
        forall|q: KeyView| #![trigger c.f.at(q)] c.f.at(q) is Some ==> key_lt(rkey(g), q) && c.f.at(q)->0.ik() != g.ik(),
        forall|q: KeyView| #![trigger c.r.at(q)] c.r.at(q) is Some ==> c.r.at(q)->0.ik() != g.ik(),
    ensures
        crec_struct(n), crec_sem(db, n), r_has(n, cls_gk(c, g)), cov_mono(c, n),
{
    assert(cls_gk(n, g) == cls_gk(c, g));
    assert forall|q: KeyView| n.r.at(q) is Some implies q == rkey(n.r.at(q)->0) by {
        if q != rkey(g) { assert(c.r.at(q) is Some); }
    }
    assert forall|r1: KeyView, r2: KeyView| n.r.at(r1) is Some && n.f.at(r2) is Some implies key_lt(r1, r2) by {
        if r1 == rkey(g) {} else { assert(c.r.at(r1) is Some); }
    }
    assert forall|r1: KeyView, r2: KeyView| n.r.at(r1) is Some && n.r.at(r2) is Some && n.r.at(r1)->0.ik() == n.r.at(r2)->0.ik() implies r1 == r2 by {
        if r1 == rkey(g) && r2 == rkey(g) {}
        else if r1 == rkey(g) { assert(c.r.at(r2) is Some); assert(c.r.at(r2)->0.ik() != g.ik()); }
        else if r2 == rkey(g) { assert(c.r.at(r1) is Some); assert(c.r.at(r1)->0.ik() != g.ik()); }
    }
    assert forall|q: KeyView| n.r.at(q) is Some implies exact_g(db, n, n.r.at(q)->0) by {
        if q != rkey(g) { assert(c.r.at(q) is Some); }
    }
    assert forall|r1: KeyView, r2: KeyView| n.r.at(r1) is Some && n.f.at(r2) is Some implies n.r.at(r1)->0.ik() != n.f.at(r2)->0.ik() by {
        if r1 != rkey(g) { assert(c.r.at(r1) is Some); }
    }
    assert(r_has(n, cls_gk(c, g))) by { assert(n.r.at(rkey(g)) == Some(g)); }
    assert forall|k: GK, s: int, m: int| covered_in(c, k, s, m) implies #[trigger] covered_in(n, k, s, m) by {
        if s <= max_val() && s >= -max_val() && r_has(c, k) {
            let rk = choose|rk: KeyView| #![trigger c.r.at(rk)] c.r.at(rk) is Some && cls_gk(c, c.r.at(rk)->0) == k;
            assert(n.r.at(rk) is Some);
            assert(cls_gk(n, n.r.at(rk)->0) == k);
        }
    }
}

pub proof fn lemma_f_put_ok(db: Db, c: ClassRec, n: ClassRec, g: Grp)
    requires
        crec_struct(c), crec_sem(db, c), crec_head(c, n), n.f.wf(), n.fb.wf(),
        forall|q: KeyView| #![trigger n.f.at(q)] n.f.at(q) == if q == rkey(g) { Some(g) } else { c.f.at(q) },
        forall|q: KeyView| #![trigger n.fb.at(q)] n.fb.at(q) == if q == nkey_v(ov(g.name)) { Some(g) } else { c.fb.at(q) },
        ix_same(n.r, c.r), ix_same(n.hp, c.hp), ix_same(n.hn, c.hn),
        exact_g(db, c, g),
        c.fb.at(nkey_v(ov(g.name))) is None,
        forall|q: KeyView| #![trigger c.r.at(q)] c.r.at(q) is Some ==> key_lt(q, rkey(g)) && c.r.at(q)->0.ik() != g.ik(),
    ensures
        crec_struct(n), crec_sem(db, n), cov_mono(c, n),
        covered_in(n, cls_gk(c, g), g.sum@, g.cnt@),
{
    let k0 = cls_gk(c, g);
    assert forall|q: KeyView| n.f.at(q) is Some implies {
        &&& q == rkey(n.f.at(q)->0)
        &&& n.fb.at(nkey_v(ov(n.f.at(q)->0.name))) == n.f.at(q)
    } by {
        if q != rkey(g) {
            let h = c.f.at(q)->0;
            assert(c.fb.at(nkey_v(ov(h.name))) == c.f.at(q));
            assert(nkey_v(ov(h.name)) != nkey_v(ov(g.name))) by {
                if nkey_v(ov(h.name)) == nkey_v(ov(g.name)) { assert(c.fb.at(nkey_v(ov(g.name))) is Some); }
            }
        }
    }
    assert forall|nk: KeyView| n.fb.at(nk) is Some implies {
        &&& nk == nkey_v(ov(n.fb.at(nk)->0.name))
        &&& n.f.at(rkey(n.fb.at(nk)->0)) == n.fb.at(nk)
    } by {
        if nk != nkey_v(ov(g.name)) {
            let h = c.fb.at(nk)->0;
            assert(c.f.at(rkey(h)) == c.fb.at(nk));
            assert(rkey(h) != rkey(g)) by {
                if rkey(h) == rkey(g) {
                    assert(c.f.at(rkey(h)) == c.fb.at(nk));
                    lemma_rkey_fields(h, g);
                    assert(nk == nkey_v(ov(h.name)));
                    assert(c.fb.at(nkey_v(ov(g.name))) is Some);
                }
            }
        }
    }
    assert forall|r1: KeyView, r2: KeyView| n.r.at(r1) is Some && n.f.at(r2) is Some implies key_lt(r1, r2) by {
        assert(c.r.at(r1) is Some);
        if r2 != rkey(g) { assert(c.f.at(r2) is Some); }
    }
    assert forall|q: KeyView| n.f.at(q) is Some implies exact_g(db, n, n.f.at(q)->0) by {
        if q != rkey(g) { assert(c.f.at(q) is Some); }
    }
    assert forall|r1: KeyView, r2: KeyView| n.r.at(r1) is Some && n.f.at(r2) is Some implies n.r.at(r1)->0.ik() != n.f.at(r2)->0.ik() by {
        assert(c.r.at(r1) is Some);
        if r2 != rkey(g) { assert(c.f.at(r2) is Some); }
    }
    assert(cls_gk(n, g) == k0);
    assert forall|k: GK, s: int, m: int| covered_in(c, k, s, m) implies #[trigger] covered_in(n, k, s, m) by {
        if s <= max_val() && s >= -max_val() {
            if r_has(c, k) {
                let rk = choose|rk: KeyView| #![trigger c.r.at(rk)] c.r.at(rk) is Some && cls_gk(c, c.r.at(rk)->0) == k;
                assert(n.r.at(rk) is Some);
                assert(cls_gk(n, n.r.at(rk)->0) == k);
            } else {
                let b = choose|b: u64| #![trigger okbits(b, s, m)] okbits(b, s, m) && c.fb.at(nkey_v(k.name)) is Some && !key_lt(rkey_gk(b, k), rkey(c.fb.at(nkey_v(k.name))->0));
                assert(nkey_v(k.name) != nkey_v(ov(g.name)));
                assert(n.fb.at(nkey_v(k.name)) == c.fb.at(nkey_v(k.name)));
            }
        }
    }
    assert(okbits(g.bits, g.sum@, g.cnt@));
    assert(rkey_gk(g.bits, k0) == rkey(g));
    lemma_key_order(rkey(g), rkey(g));
}

pub proof fn lemma_f_del_ok(db: Db, c: ClassRec, n: ClassRec, nk: KeyView)
    requires
        crec_struct(c), crec_sem(db, c), crec_head(c, n), n.f.wf(), n.fb.wf(),
        c.fb.at(nk) is None ==> ix_same(n.f, c.f) && ix_same(n.fb, c.fb),
        c.fb.at(nk) is Some ==> {
            let b = c.fb.at(nk)->0;
            &&& forall|q: KeyView| #![trigger n.f.at(q)] n.f.at(q) == if q == rkey(b) { None } else { c.f.at(q) }
            &&& forall|q: KeyView| #![trigger n.fb.at(q)] n.fb.at(q) == if q == nk { None } else { c.fb.at(q) }
        },
        ix_same(n.r, c.r), ix_same(n.hp, c.hp), ix_same(n.hn, c.hn),
    ensures
        crec_struct(n), crec_sem(db, n),
        n.fb.at(nk) is None,
        forall|k: GK, s: int, m: int| #![trigger covered_in(n, k, s, m)] covered_in(c, k, s, m) && nkey_v(k.name) != nk ==> covered_in(n, k, s, m),
{
    if c.fb.at(nk) is Some {
        let b = c.fb.at(nk)->0;
        assert(c.f.at(rkey(b)) == c.fb.at(nk));
        assert forall|q: KeyView| n.f.at(q) is Some implies {
            &&& q == rkey(n.f.at(q)->0)
            &&& n.fb.at(nkey_v(ov(n.f.at(q)->0.name))) == n.f.at(q)
        } by {
            let h = c.f.at(q)->0;
            assert(q != rkey(b));
            assert(c.fb.at(nkey_v(ov(h.name))) == c.f.at(q));
            assert(nkey_v(ov(h.name)) != nk) by { if nkey_v(ov(h.name)) == nk { assert(c.f.at(q) == c.fb.at(nk)); assert(q == rkey(b)); } }
        }
        assert forall|q: KeyView| n.fb.at(q) is Some implies {
            &&& q == nkey_v(ov(n.fb.at(q)->0.name))
            &&& n.f.at(rkey(n.fb.at(q)->0)) == n.fb.at(q)
        } by {
            let h = c.fb.at(q)->0;
            assert(q != nk);
            assert(c.f.at(rkey(h)) == c.fb.at(q));
            assert(rkey(h) != rkey(b)) by { if rkey(h) == rkey(b) { assert(c.f.at(rkey(b)) == c.fb.at(q)); assert(c.f.at(rkey(b)) == c.fb.at(nk)); assert(q == nkey_v(ov(h.name)) && nk == nkey_v(ov(b.name))); } }
        }
        assert forall|r1: KeyView, r2: KeyView| n.r.at(r1) is Some && n.f.at(r2) is Some implies key_lt(r1, r2) && n.r.at(r1)->0.ik() != n.f.at(r2)->0.ik() by {
            assert(c.r.at(r1) is Some); assert(c.f.at(r2) is Some);
        }
        assert forall|q: KeyView| n.f.at(q) is Some implies exact_g(db, n, n.f.at(q)->0) by { assert(c.f.at(q) is Some); }
    }
    assert forall|k: GK, s: int, m: int| covered_in(c, k, s, m) && nkey_v(k.name) != nk implies #[trigger] covered_in(n, k, s, m) by {
        if s <= max_val() && s >= -max_val() {
            if r_has(c, k) {
                let rk = choose|rk: KeyView| #![trigger c.r.at(rk)] c.r.at(rk) is Some && cls_gk(c, c.r.at(rk)->0) == k;
                assert(n.r.at(rk) is Some);
                assert(cls_gk(n, n.r.at(rk)->0) == k);
            } else {
                assert(n.fb.at(nkey_v(k.name)) == c.fb.at(nkey_v(k.name)));
            }
        }
    }
}

pub proof fn lemma_r_del_ok(db: Db, c: ClassRec, n: ClassRec, rk: KeyView)
    requires
        crec_struct(c), crec_sem(db, c), crec_head(c, n), n.r.wf(),
        forall|q: KeyView| #![trigger n.r.at(q)] n.r.at(q) == if q == rk { None } else { c.r.at(q) },
        ix_same(n.f, c.f), ix_same(n.fb, c.fb), ix_same(n.hp, c.hp), ix_same(n.hn, c.hn),
    ensures
        crec_struct(n), crec_sem(db, n),
        forall|k: GK, s: int, m: int| #![trigger covered_in(n, k, s, m)]
            covered_in(c, k, s, m) && (c.r.at(rk) is Some ==> k != cls_gk(c, c.r.at(rk)->0)) ==> covered_in(n, k, s, m),
{
    assert forall|q: KeyView| n.r.at(q) is Some implies q == rkey(n.r.at(q)->0) by { assert(c.r.at(q) is Some); }
    assert forall|r1: KeyView, r2: KeyView| n.r.at(r1) is Some && n.f.at(r2) is Some implies key_lt(r1, r2) && n.r.at(r1)->0.ik() != n.f.at(r2)->0.ik() by { assert(c.r.at(r1) is Some); }
    assert forall|r1: KeyView, r2: KeyView| n.r.at(r1) is Some && n.r.at(r2) is Some && n.r.at(r1)->0.ik() == n.r.at(r2)->0.ik() implies r1 == r2 by {
        assert(c.r.at(r1) is Some && c.r.at(r2) is Some);
    }
    assert forall|q: KeyView| n.r.at(q) is Some implies exact_g(db, n, n.r.at(q)->0) by { assert(c.r.at(q) is Some); }
    assert forall|k: GK, s: int, m: int| covered_in(c, k, s, m) && (c.r.at(rk) is Some ==> k != cls_gk(c, c.r.at(rk)->0)) implies #[trigger] covered_in(n, k, s, m) by {
        if s <= max_val() && s >= -max_val() && r_has(c, k) {
            let q = choose|q: KeyView| #![trigger c.r.at(q)] c.r.at(q) is Some && cls_gk(c, c.r.at(q)->0) == k;
            assert(q != rk);
            assert(n.r.at(q) == c.r.at(q));
            assert(cls_gk(n, n.r.at(q)->0) == k);
        }
    }
}

pub proof fn lemma_h_del_ok(db: Db, c: ClassRec, n: ClassRec, nk: KeyView)
    requires
        crec_struct(c), crec_sem(db, c), crec_head(c, n), n.hp.wf(), n.hn.wf(),
        forall|q: KeyView| #![trigger n.hp.at(q)] n.hp.at(q) == if q == nk { None } else { c.hp.at(q) },
        forall|q: KeyView| #![trigger n.hn.at(q)] n.hn.at(q) == if q == nk { None } else { c.hn.at(q) },
        ix_same(n.r, c.r), ix_same(n.f, c.f), ix_same(n.fb, c.fb),
    ensures
        crec_struct(n), crec_sem(db, n),
        forall|k: GK, s: int, m: int| #![trigger covered_in(n, k, s, m)] covered_in(c, k, s, m) && nkey_v(k.name) != nk ==> covered_in(n, k, s, m),
{
    assert forall|q: KeyView| n.hp.at(q) is Some implies has_cand(db, n, q, true) by { assert(c.hp.at(q) is Some); }
    assert forall|q: KeyView| n.hn.at(q) is Some implies has_cand(db, n, q, false) by { assert(c.hn.at(q) is Some); }
    assert forall|k: GK, s: int, m: int| covered_in(c, k, s, m) && nkey_v(k.name) != nk implies #[trigger] covered_in(n, k, s, m) by {
        if s <= max_val() && s >= -max_val() && r_has(c, k) {
            let q = choose|q: KeyView| #![trigger c.r.at(q)] c.r.at(q) is Some && cls_gk(c, c.r.at(q)->0) == k;
            assert(n.r.at(q) is Some);
            assert(cls_gk(n, n.r.at(q)->0) == k);
        }
    }
}

#[verifier::rlimit(60)]
#[verifier::spinoff_prover]
pub proof fn lemma_hp_put_ok(db: Db, c: ClassRec, n: ClassRec, nk: KeyView)
    requires
        crec_struct(c), crec_sem(db, c), crec_head(c, n), n.hp.wf(),
        forall|q: KeyView| #![trigger n.hp.at(q)] n.hp.at(q) == if q == nk { Some(()) } else { c.hp.at(q) },
        ix_same(n.r, c.r), ix_same(n.f, c.f), ix_same(n.fb, c.fb), ix_same(n.hn, c.hn),
        has_cand(db, c, nk, true), nk.0 == 0,
    ensures
        crec_struct(n), crec_sem(db, n), cov_mono(c, n),
        forall|k: GK, s: int, m: int| #![trigger covered_in(n, k, s, m)] s > max_val() && nkey_v(k.name) == nk ==> covered_in(n, k, s, m),
{
    assert forall|q: KeyView| n.hp.at(q) is Some implies has_cand(db, n, q, true) && q.0 == 0 by {
        if q != nk { assert(c.hp.at(q) is Some); }
    }
    assert forall|k: GK, s: int, m: int| s > max_val() && nkey_v(k.name) == nk implies #[trigger] covered_in(n, k, s, m) by {
        assert(n.hp.at(nkey_v(k.name)) is Some);
    }
    assert forall|k: GK, s: int, m: int| covered_in(c, k, s, m) implies #[trigger] covered_in(n, k, s, m) by {
        if s <= max_val() && s >= -max_val() && r_has(c, k) {
            let q = choose|q: KeyView| #![trigger c.r.at(q)] c.r.at(q) is Some && cls_gk(c, c.r.at(q)->0) == k;
            assert(n.r.at(q) is Some);
            assert(cls_gk(n, n.r.at(q)->0) == k);
        }
    }
}

#[verifier::rlimit(60)]
#[verifier::spinoff_prover]
pub proof fn lemma_hn_put_ok(db: Db, c: ClassRec, n: ClassRec, nk: KeyView)
    requires
        crec_struct(c), crec_sem(db, c), crec_head(c, n), n.hn.wf(),
        forall|q: KeyView| #![trigger n.hn.at(q)] n.hn.at(q) == if q == nk { Some(()) } else { c.hn.at(q) },
        ix_same(n.r, c.r), ix_same(n.f, c.f), ix_same(n.fb, c.fb), ix_same(n.hp, c.hp),
        has_cand(db, c, nk, false), nk.0 == 0,
    ensures
        crec_struct(n), crec_sem(db, n), cov_mono(c, n),
        forall|k: GK, s: int, m: int| #![trigger covered_in(n, k, s, m)] s < -max_val() && nkey_v(k.name) == nk ==> covered_in(n, k, s, m),
{
    assert forall|q: KeyView| n.hn.at(q) is Some implies has_cand(db, n, q, false) && q.0 == 0 by {
        if q != nk { assert(c.hn.at(q) is Some); }
    }
    lemma_max_pos();
    assert forall|k: GK, s: int, m: int| s < -max_val() && nkey_v(k.name) == nk implies #[trigger] covered_in(n, k, s, m) by {
        assert(n.hn.at(nkey_v(k.name)) is Some);
    }
    assert forall|k: GK, s: int, m: int| covered_in(c, k, s, m) implies #[trigger] covered_in(n, k, s, m) by {
        if s <= max_val() && s >= -max_val() && r_has(c, k) {
            let q = choose|q: KeyView| #![trigger c.r.at(q)] c.r.at(q) is Some && cls_gk(c, c.r.at(q)->0) == k;
            assert(n.r.at(q) is Some);
            assert(cls_gk(n, n.r.at(q)->0) == k);
        }
    }
}

} // verus!

verus! {

/// A unit's frontier group replaced by a better group of the unit keeps the record well formed and every group accounted for.
#[verifier::rlimit(100)]
#[verifier::spinoff_prover]
pub proof fn lemma_f_swap_ok(db: Db, c: ClassRec, n: ClassRec, b: Grp, g: Grp)
    requires
        crec_struct(c), crec_sem(db, c), crec_head(c, n), n.f.wf(), n.fb.wf(),
        c.fb.at(nkey_v(ov(b.name))) == Some(b), nkey_v(ov(g.name)) == nkey_v(ov(b.name)),
        forall|q: KeyView| #![trigger n.f.at(q)] n.f.at(q) == if q == rkey(g) { Some(g) } else if q == rkey(b) { None } else { c.f.at(q) },
        forall|q: KeyView| #![trigger n.fb.at(q)] n.fb.at(q) == if q == nkey_v(ov(g.name)) { Some(g) } else { c.fb.at(q) },
        ix_same(n.r, c.r), ix_same(n.hp, c.hp), ix_same(n.hn, c.hn),
        exact_g(db, c, g), key_lt(rkey(g), rkey(b)),
        forall|q: KeyView| #![trigger c.r.at(q)] c.r.at(q) is Some ==> key_lt(q, rkey(g)) && c.r.at(q)->0.ik() != g.ik(),
    ensures
        crec_struct(n), crec_sem(db, n), cov_mono(c, n),
        covered_in(n, cls_gk(c, g), g.sum@, g.cnt@),
{
    let k0 = cls_gk(c, g);
    lemma_key_order(rkey(g), rkey(b));
    assert(c.f.at(rkey(b)) == c.fb.at(nkey_v(ov(b.name))));
    assert(rkey(g) != rkey(b));
    assert forall|q: KeyView| n.f.at(q) is Some implies {
        &&& q == rkey(n.f.at(q)->0)
        &&& n.fb.at(nkey_v(ov(n.f.at(q)->0.name))) == n.f.at(q)
    } by {
        if q != rkey(g) {
            let h = c.f.at(q)->0;
            assert(c.fb.at(nkey_v(ov(h.name))) == c.f.at(q));
            assert(nkey_v(ov(h.name)) != nkey_v(ov(g.name))) by {
                if nkey_v(ov(h.name)) == nkey_v(ov(g.name)) { assert(c.f.at(q) == c.fb.at(nkey_v(ov(b.name)))); assert(q == rkey(b)); }
            }
        }
    }
    assert forall|nk: KeyView| n.fb.at(nk) is Some implies {
        &&& nk == nkey_v(ov(n.fb.at(nk)->0.name))
        &&& n.f.at(rkey(n.fb.at(nk)->0)) == n.fb.at(nk)
    } by {
        if nk != nkey_v(ov(g.name)) {
            let h = c.fb.at(nk)->0;
            assert(c.f.at(rkey(h)) == c.fb.at(nk));
            assert(rkey(h) != rkey(g)) by {
                if rkey(h) == rkey(g) { lemma_rkey_fields(h, g); assert(nk == nkey_v(ov(h.name))); assert(c.fb.at(nkey_v(ov(g.name))) is Some); }
            }
            assert(rkey(h) != rkey(b)) by {
                if rkey(h) == rkey(b) { lemma_rkey_fields(h, b); assert(nk == nkey_v(ov(h.name))); assert(nk == nkey_v(ov(b.name))); }
            }
        }
    }
    assert forall|r1: KeyView, r2: KeyView| n.r.at(r1) is Some && n.f.at(r2) is Some implies key_lt(r1, r2) && n.r.at(r1)->0.ik() != n.f.at(r2)->0.ik() by {
        assert(c.r.at(r1) is Some);
        if r2 != rkey(g) { assert(c.f.at(r2) is Some); } else { assert(key_lt(r1, rkey(g))); assert(c.r.at(r1)->0.ik() != g.ik()); }
    }
    assert forall|q: KeyView| n.f.at(q) is Some implies exact_g(db, n, n.f.at(q)->0) by {
        if q != rkey(g) { assert(c.f.at(q) is Some); }
    }
    assert(cls_gk(n, g) == k0);
    assert forall|k: GK, s: int, m: int| covered_in(c, k, s, m) implies #[trigger] covered_in(n, k, s, m) by {
        if s <= max_val() && s >= -max_val() {
            if r_has(c, k) {
                let rk = choose|rk: KeyView| #![trigger c.r.at(rk)] c.r.at(rk) is Some && cls_gk(c, c.r.at(rk)->0) == k;
                assert(n.r.at(rk) is Some);
                assert(cls_gk(n, n.r.at(rk)->0) == k);
            } else {
                let b2 = choose|b2: u64| #![trigger okbits(b2, s, m)] okbits(b2, s, m) && c.fb.at(nkey_v(k.name)) is Some && !key_lt(rkey_gk(b2, k), rkey(c.fb.at(nkey_v(k.name))->0));
                if nkey_v(k.name) == nkey_v(ov(g.name)) {
                    assert(c.fb.at(nkey_v(k.name))->0 == b);
                    assert(!key_lt(rkey_gk(b2, k), rkey(b)));
                    lemma_key_order(rkey_gk(b2, k), rkey(b));
                    if rkey_gk(b2, k) != rkey(b) { lemma_key_trans(rkey(g), rkey(b), rkey_gk(b2, k)); }
                    lemma_key_order(rkey_gk(b2, k), rkey(g));
                    assert(n.fb.at(nkey_v(k.name)) == Some(g));
                } else {
                    assert(n.fb.at(nkey_v(k.name)) == c.fb.at(nkey_v(k.name)));
                }
            }
        }
    }
    assert(okbits(g.bits, g.sum@, g.cnt@));
    assert(rkey_gk(g.bits, k0) == rkey(g));
    lemma_key_order(rkey(g), rkey(g));
}

} // verus!
