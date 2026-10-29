// At build: per class, the threshold below which the stored prefix holds a thousand cells.
use vstd::prelude::*;
use std::sync::Arc;
use verus_common::bytes::*;
use verus_common::sums::*;
use verus_prooflib::ordered::{Index, Key, KeyView};
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::model::*;
use crate::cells::*;
use crate::maint::*;
use crate::f64bits::*;
use crate::group::*;
use crate::filing::*;
use crate::thr::*;
use crate::cnt::*;

verus! {

/// Cells kept per class: the stored prefix of a class holds at least this many (the smallest value keys seen).
pub const TOPK: usize = 1024;

/// A class's candidate cells: distinct (value key, filing) pairs, none above maxd.
pub struct Top {
    pub v: Vec<(i64, Arc<Vec<u8>>)>,
    pub maxd: i64,
}

pub open spec fn top_ok(t: Top, isreal: spec_fn(int, Seq<u8>) -> bool) -> bool {
    &&& t.v@.len() <= TOPK
    &&& forall|i: int| 0 <= i < t.v@.len() ==> isreal((#[trigger] t.v@[i]).0 as int, (*t.v@[i].1)@) && t.v@[i].0 <= t.maxd
    &&& forall|i: int, j: int| 0 <= i < t.v@.len() && 0 <= j < t.v@.len() && i != j
        ==> !((#[trigger] t.v@[i]).0 == (#[trigger] t.v@[j]).0 && (*t.v@[i].1)@ == (*t.v@[j].1)@)
}

pub fn top_insert(t: &mut Top, d: i64, a: &Arc<Vec<u8>>, Ghost(isreal): Ghost<spec_fn(int, Seq<u8>) -> bool>)
    requires
        top_ok(*old(t), isreal),
        isreal(d as int, (**a)@),
    ensures
        top_ok(*final(t), isreal),
        old(t).v@.len() <= final(t).v@.len(),
        old(t).v@.len() == TOPK ==> final(t).v@.len() == TOPK,
{
    if t.v.len() >= TOPK && d >= t.maxd {
        return;
    }
    let mut i: usize = 0;
    while i < t.v.len()
        invariant
            i <= t.v@.len(),
            top_ok(*t, isreal),
            *t == *old(t),
            forall|k: int| 0 <= k < i ==> !((#[trigger] t.v@[k]).0 == d && (*t.v@[k].1)@ == (**a)@),
        decreases t.v.len() - i,
    {
        if t.v[i].0 == d && bytes_eq_exec(t.v[i].1.as_slice(), a.as_slice()) {
            return;
        }
        i += 1;
    }
    if t.v.len() < TOPK {
        t.v.push((d, a.clone()));
        if d > t.maxd || t.v.len() == 1 {
            t.maxd = d;
        }
        proof {
            assert forall|k: int| 0 <= k < t.v@.len() implies isreal((#[trigger] t.v@[k]).0 as int, (*t.v@[k].1)@) && t.v@[k].0 <= t.maxd by {
                if k < t.v@.len() - 1 { assert(t.v@[k] == old(t).v@[k]); }
            }
            assert forall|p: int, q: int| 0 <= p < t.v@.len() && 0 <= q < t.v@.len() && p != q implies !((#[trigger] t.v@[p]).0 == (#[trigger] t.v@[q]).0 && (*t.v@[p].1)@ == (*t.v@[q].1)@) by {
                if p < t.v@.len() - 1 && q < t.v@.len() - 1 { assert(t.v@[p] == old(t).v@[p] && t.v@[q] == old(t).v@[q]); }
                else if p == t.v@.len() - 1 { assert(t.v@[q] == old(t).v@[q]); }
                else { assert(t.v@[p] == old(t).v@[p]); }
            }
        }
        return;
    }
    // full, and d is below the largest: replace the largest
    let mut im: usize = 0;
    let mut k: usize = 1;
    while k < t.v.len()
        invariant
            1 <= k <= t.v@.len(),
            im < k,
            top_ok(*t, isreal),
            *t == *old(t),
            forall|q: int| 0 <= q < k ==> (#[trigger] t.v@[q]).0 <= t.v@[im as int].0,
        decreases t.v.len() - k,
    {
        if t.v[k].0 > t.v[im].0 {
            im = k;
        }
        k += 1;
    }
    t.v.set(im, (d, a.clone()));
    proof {
        assert forall|q: int| 0 <= q < t.v@.len() implies isreal((#[trigger] t.v@[q]).0 as int, (*t.v@[q].1)@) by {
            if q != im as int { assert(t.v@[q] == old(t).v@[q]); }
        }
    }
    let mut mx: i64 = d;
    let mut k2: usize = 0;
    while k2 < t.v.len()
        invariant
            k2 <= t.v@.len(),
            t.v@.len() == TOPK,
            forall|q: int| 0 <= q < k2 ==> (#[trigger] t.v@[q]).0 <= mx,
            t.v@[im as int].0 == d,
            forall|q: int| 0 <= q < t.v@.len() && q != im as int ==> (#[trigger] t.v@[q]) == old(t).v@[q],
            im < t.v@.len(),
            forall|q: int| 0 <= q < t.v@.len() ==> isreal((#[trigger] t.v@[q]).0 as int, (*t.v@[q].1)@),
        decreases t.v.len() - k2,
    {
        if t.v[k2].0 > mx {
            mx = t.v[k2].0;
        }
        k2 += 1;
    }
    t.maxd = mx;
    proof {
        let n = t.v@.len();
        assert forall|q: int| 0 <= q < n implies isreal((#[trigger] t.v@[q]).0 as int, (*t.v@[q].1)@) && t.v@[q].0 <= t.maxd by {
            if q != im as int { assert(t.v@[q] == old(t).v@[q]); }
        }
        assert forall|p: int, q: int| 0 <= p < n && 0 <= q < n && p != q implies !((#[trigger] t.v@[p]).0 == (#[trigger] t.v@[q]).0 && (*t.v@[p].1)@ == (*t.v@[q].1)@) by {
            if p != im as int && q != im as int { assert(t.v@[p] == old(t).v@[p] && t.v@[q] == old(t).v@[q]); }
            else if p == im as int { assert(t.v@[q] == old(t).v@[q]); assert(!(old(t).v@[q].0 == d && (*old(t).v@[q].1)@ == (**a)@)); }
            else { assert(t.v@[p] == old(t).v@[p]); assert(!(old(t).v@[p].0 == d && (*old(t).v@[p].1)@ == (**a)@)); }
        }
    }
}

pub struct ClsTop {
    pub fy: i64,
    pub uom: Arc<Vec<u8>>,
    pub top: Top,
}

pub open spec fn real_in(db: Db, fy: int, uom: Seq<u8>) -> spec_fn(int, Seq<u8>) -> bool {
    |d: int, a: Seq<u8>| !zero_fam(cw(db), CellId { fy, uom, d, a })
}

pub open spec fn cls_ok(c: ClsTop, db: Db) -> bool {
    top_ok(c.top, real_in(db, c.fy as int, (*c.uom)@))
}

/// Per class, the largest of a thousand distinct cells of the filings, once there are that many.
pub fn pick_thresholds(
    keys: &Vec<Vec<u8>>,
    mri: &Index<Vec<MR>>,
    sri: &Index<Vec<SR>>,
    Ghost(db): Ghost<Db>,
) -> (thn: Thr)
    requires
        mri.wf(),
        sri.wf(),
        forall|a: Seq<u8>| seq_view_bytes(keys@).contains(a) ==> {
            &&& mr_sem(mr_at(*mri, a), a, db)
            &&& sr_sem(sr_at(*sri, a), a, db)
            &&& forall|k: int| 0 <= k < mr_at(*mri, a).len() ==> f64_finite((#[trigger] mr_at(*mri, a)[k]).val)
        },
    ensures
        thn.wf(),
        forall|fy: int, uom: Seq<u8>| #[trigger] thr_of(thn, fy, uom) == D_INF as int
            || exists|ks: Seq<KeyView>| ks.len() >= lim() && ks.no_duplicates()
                && forall|i: int| 0 <= i < ks.len() ==> (#[trigger] ks[i]).0 <= thr_of(thn, fy, uom)
                    && !zero_fam(cw(db), CellId { fy, uom, d: ks[i].0, a: ks[i].1 }),
{
    let mut cls: Vec<ClsTop> = Vec::new();
    let mut i: usize = 0;
    while i < keys.len()
        invariant
            i <= keys@.len(),
            mri.wf(),
            sri.wf(),
            forall|a: Seq<u8>| seq_view_bytes(keys@).contains(a) ==> {
                &&& mr_sem(mr_at(*mri, a), a, db)
                &&& sr_sem(sr_at(*sri, a), a, db)
                &&& forall|k: int| 0 <= k < mr_at(*mri, a).len() ==> f64_finite((#[trigger] mr_at(*mri, a)[k]).val)
            },
            forall|q: int| 0 <= q < cls.len() ==> cls_ok(#[trigger] cls@[q], db),
            forall|p: int, q: int| 0 <= p < q < cls.len() ==> !((#[trigger] cls@[p]).fy == (#[trigger] cls@[q]).fy && (*cls@[p].uom)@ == (*cls@[q].uom)@),
        decreases keys.len() - i,
    {
        let a = Arc::new(copy_vec(keys[i].as_slice()));
        let ghost av = (*a)@;
        proof { assert(seq_view_bytes(keys@).contains(av)) by { assert(seq_view_bytes(keys@)[i as int] == av); } }
        let mr = get_mrs(mri, &a);
        let sr = get_srs(sri, &a);
        let ents = build_ents(&mr, &sr);
        let mut e: usize = 0;
        while e < ents.len()
            invariant
                e <= ents@.len(),
                av == (*a)@,
                mr_sem(mr@, av, db),
                sr_sem(sr@, av, db),
                ents@ == pairs(mr@, sr@),
                forall|k: int| 0 <= k < mr@.len() ==> f64_finite((#[trigger] mr@[k]).val),
                forall|j: int| 0 <= j < ents@.len() ==> ent_ok(#[trigger] ents@[j]),
                forall|q: int| 0 <= q < cls.len() ==> cls_ok(#[trigger] cls@[q], db),
                forall|p: int, q: int| 0 <= p < q < cls.len() ==> !((#[trigger] cls@[p]).fy == (#[trigger] cls@[q]).fy && (*cls@[p].uom)@ == (*cls@[q].uom)@),
            decreases ents.len() - e,
        {
            let ck = &ents[e].ck;
            let ghost id = CellId { fy: ck.fy as int, uom: (*ck.uom)@, d: ck.d as int, a: av };
            proof {
                // the cell of this entry is not empty
                assert(ents@.contains(ents@[e as int]));
                lemma_ent_fam_zero(ents@, id);
                assert(ck_matches(ents@[e as int].ck, id));
                assert(!zero_fam(ent_fam(ents@), id));
                assert forall|g: spec_fn(OutRow) -> int| #[trigger] fam_w(ent_fam(ents@), id, g) == fam_w(cw(db), id, g) by {
                    lemma_ents_cell_w(mr@, sr@, av, db, id, g);
                }
                lemma_zero_congr(ent_fam(ents@), cw(db), id);
                assert(!zero_fam(cw(db), id));
            }
            let mut c: usize = 0;
            while c < cls.len() && !(cls[c].fy == ck.fy && bytes_eq_exec(cls[c].uom.as_slice(), ck.uom.as_slice()))
                invariant
                    c <= cls.len(),
                    forall|q: int| 0 <= q < c ==> !(cls@[q].fy == ck.fy && (*cls@[q].uom)@ == (*ck.uom)@),
                decreases cls.len() - c,
            {
                c += 1;
            }
            if c == cls.len() {
                cls.push(ClsTop { fy: ck.fy, uom: ck.uom.clone(), top: Top { v: Vec::new(), maxd: 0 } });
                proof {
                    assert forall|q: int| 0 <= q < cls.len() implies cls_ok(#[trigger] cls@[q], db) by {
                        if q < cls.len() - 1 {} else { assert(cls@[q].top.v@.len() == 0); }
                    }
                    assert forall|p: int, q: int| 0 <= p < q < cls.len() implies !((#[trigger] cls@[p]).fy == (#[trigger] cls@[q]).fy && (*cls@[p].uom)@ == (*cls@[q].uom)@) by {
                        if q == cls.len() - 1 {
                            assert(!(cls@[p].fy == ck.fy && (*cls@[p].uom)@ == (*ck.uom)@));
                        }
                    }
                }
            }
            proof { assert(c < cls.len()); }
            proof { assert(cls@[c as int].fy == ck.fy && (*cls@[c as int].uom)@ == (*ck.uom)@); }
            top_insert(&mut cls[c].top, ck.d, &a, Ghost(real_in(db, ck.fy as int, (*ck.uom)@)));
            e += 1;
        }
        i += 1;
    }
    // the thresholds
    let mut thn: Thr = Index::new();
    let mut c: usize = 0;
    while c < cls.len()
        invariant
            c <= cls.len(),
            thn.wf(),
            forall|q: int| 0 <= q < cls.len() ==> cls_ok(#[trigger] cls@[q], db),
            forall|p: int, q: int| 0 <= p < q < cls.len() ==> !((#[trigger] cls@[p]).fy == (#[trigger] cls@[q]).fy && (*cls@[p].uom)@ == (*cls@[q].uom)@),
            forall|fy: int, uom: Seq<u8>| #[trigger] thr_of(thn, fy, uom) == D_INF as int
                || exists|q: int| 0 <= q < c && cls@[q].fy as int == fy && (*cls@[q].uom)@ == uom && cls@[q].top.v@.len() == TOPK && thr_of(thn, fy, uom) == cls@[q].top.maxd as int,
        decreases cls.len() - c,
    {
        let ghost old = thn;
        if cls[c].top.v.len() >= TOPK {
            thn.put(kkey(cls[c].fy, &cls[c].uom), Arc::new(cls[c].top.maxd));
        }
        proof {
            assert forall|fy: int, uom: Seq<u8>| #[trigger] thr_of(thn, fy, uom) == D_INF as int
                || exists|q: int| 0 <= q < c + 1 && cls@[q].fy as int == fy && (*cls@[q].uom)@ == uom && cls@[q].top.v@.len() == TOPK && thr_of(thn, fy, uom) == cls@[q].top.maxd as int by {
                if cls@[c as int].top.v@.len() >= TOPK && (fy, uom) == (cls@[c as int].fy as int, (*cls@[c as int].uom)@) {
                    assert(thr_of(thn, fy, uom) == cls@[c as int].top.maxd as int);
                } else if thr_of(thn, fy, uom) != D_INF as int {
                    assert(thn.at((fy, uom)) == old.at((fy, uom)));
                    let q = choose|q: int| 0 <= q < c && cls@[q].fy as int == fy && (*cls@[q].uom)@ == uom && cls@[q].top.v@.len() == TOPK && thr_of(old, fy, uom) == cls@[q].top.maxd as int;
                    assert(thr_of(thn, fy, uom) == thr_of(old, fy, uom));
                }
            }
        }
        c += 1;
    }
    proof {
        assert forall|fy: int, uom: Seq<u8>| #[trigger] thr_of(thn, fy, uom) == D_INF as int
            || exists|ks: Seq<KeyView>| ks.len() >= lim() && ks.no_duplicates()
                && forall|i: int| 0 <= i < ks.len() ==> (#[trigger] ks[i]).0 <= thr_of(thn, fy, uom)
                    && !zero_fam(cw(db), CellId { fy, uom, d: ks[i].0, a: ks[i].1 }) by {
            if thr_of(thn, fy, uom) != D_INF as int {
                let q = choose|q: int| 0 <= q < cls.len() && cls@[q].fy as int == fy && (*cls@[q].uom)@ == uom && cls@[q].top.v@.len() == TOPK && thr_of(thn, fy, uom) == cls@[q].top.maxd as int;
                let t = cls@[q].top;
                let ks = t.v@.map_values(|e: (i64, Arc<Vec<u8>>)| (e.0 as int, (*e.1)@));
                assert(ks.len() == TOPK);
                assert forall|x: int, y: int| 0 <= x < ks.len() && 0 <= y < ks.len() && x != y implies ks[x] != ks[y] by {}
                assert(ks.no_duplicates());
                assert forall|x: int| 0 <= x < ks.len() implies (#[trigger] ks[x]).0 <= thr_of(thn, fy, uom) && !zero_fam(cw(db), CellId { fy, uom, d: ks[x].0, a: ks[x].1 }) by {
                    assert(real_in(db, fy, uom)(t.v@[x].0 as int, (*t.v@[x].1)@));
                }
            }
        }
    }
    thn
}

} // verus!
