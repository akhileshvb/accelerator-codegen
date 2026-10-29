// Trimming: when a class stores more than KMAX cells within its threshold, lower the threshold to the TRIMTO-th cell's
// value key; each filing keeps only its first cell beyond it.
use vstd::prelude::*;
use std::sync::Arc;
use verus_common::sums::*;
use verus_common::bytes::*;
use verus_common::rel::*;
use verus_prooflib::ordered::{Index, Key, KeyView, key_lt, ordered_rows};
use crate::gen_spec::*;
use crate::model::*;
use crate::cells::*;
use crate::classes::*;
use crate::maint::*;
use crate::f64bits::*;
use crate::group::copy_vec;
use crate::thr::*;
use crate::cnt::*;
use crate::plan::*;
use crate::step::*;
use crate::plan::{lemma_filter_step, pick_view};
use verus_prooflib::ordered::{lemma_lower_suffix, lower_position};
use crate::filing::lemma_cover_congr;

verus! {

/// Cells stored within the threshold above which a class is trimmed.
pub const KMAX: usize = 1024;
/// ... and the number of cells it keeps within the new threshold.
pub const TRIMTO: usize = 512;

/// The filing's threshold in one class lowered to dn.
pub open spec fn th_dem(th: Th, u: Seq<u8>, fy: int, uom: Seq<u8>, dn: int) -> Th {
    |b: Seq<u8>, f2: int, u2: Seq<u8>| if b == u && f2 == fy && u2 == uom { dn } else { th(b, f2, u2) }
}

/// A stored, listed cell of the filing in the class beyond dn.
pub open spec fn in_tail(c: Cells, h: Index<Vec<CK>>, u: Seq<u8>, fy: int, uom: Seq<u8>, dn: int, d: int) -> bool {
    d > dn && listed(dir_of(h, u), CellId { fy, uom, d, a: u }) && cell_at(c, CellId { fy, uom, d, a: u }) is Some
}

/// With no stored tail, or none below m, the family has nothing between dn and m.
pub proof fn lemma_dem_zero(c: Cells, h: Index<Vec<CK>>, w: Fam, th: Th, u: Seq<u8>, fy: int, uom: Seq<u8>, dn: int, m: int, d: int)
    requires
        cells_rep(c, sfam(w, th)),
        cover(h, sfam(w, th)),
        th(u, fy, uom) >= dn,
        dn < d < m,
        forall|x: int| in_tail(c, h, u, fy, uom, dn, x) ==> m <= x,
    ensures
        zero_fam(w, CellId { fy, uom, d, a: u }),
{
    let id = CellId { fy, uom, d, a: u };
    let dd = th(u, fy, uom);
    if !zero_fam(w, id) {
        let f = lemma_first_nonzero(w, id, dn, d);
        let idf = at_d(id, f);
        let thh = th;
        assert(th_id(thh, idf) == dd);
        if f > dd {
            assert(isb(w, thh, idf)) by {
                assert forall|x: int| dd < x < f implies zero_fam(w, #[trigger] at_d(idf, x)) by { assert(at_d(idf, x) == at_d(id, x)); }
            }
        }
        assert(keep(w, thh, idf));
        let g = choose|g: spec_fn(OutRow) -> int| fam_w(w, idf, g) != 0;
        assert(fam_w(sfam(w, thh), idf, g) != 0);
        lemma_nonzero_stored(c, sfam(w, thh), idf);
        assert(listed(dir_of(h, u), idf)) by { assert(!zero_fam(sfam(w, thh), idf)); }
        assert(in_tail(c, h, u, fy, uom, dn, f));
        assert(m <= f);
    }
}

/// After lowering the threshold of one filing in one class: the family is what is left of the old one.
pub proof fn lemma_dem_family(c: Cells, h: Index<Vec<CK>>, w: Fam, th: Th, u: Seq<u8>, fy: int, uom: Seq<u8>, dn: int, m: int, id: CellId)
    requires
        cells_rep(c, sfam(w, th)),
        cover(h, sfam(w, th)),
        th(u, fy, uom) >= dn,
        in_tail(c, h, u, fy, uom, dn, m),
        forall|x: int| in_tail(c, h, u, fy, uom, dn, x) ==> m <= x,
        id.a == u && id.fy == fy && id.uom == uom,
    ensures
        id.d <= dn ==> forall|g: spec_fn(OutRow) -> int| #[trigger] fam_w(sfam(w, th_dem(th, u, fy, uom, dn)), id, g) == fam_w(sfam(w, th), id, g),
        dn < id.d < m ==> zero_fam(sfam(w, th), id) && zero_fam(sfam(w, th_dem(th, u, fy, uom, dn)), id),
        id.d == m ==> forall|g: spec_fn(OutRow) -> int| #[trigger] fam_w(sfam(w, th_dem(th, u, fy, uom, dn)), id, g) == fam_w(sfam(w, th), id, g),
        id.d > m ==> zero_fam(sfam(w, th_dem(th, u, fy, uom, dn)), id),
{
    let th2 = th_dem(th, u, fy, uom, dn);
    assert(th_id(th2, id) == dn);
    assert(th_id(th, id) >= dn);
    if id.d <= dn {
        assert(keep(w, th, id) && keep(w, th2, id));
    }
    if dn < id.d && id.d < m {
        lemma_dem_zero(c, h, w, th, u, fy, uom, dn, m, id.d);
        lemma_sfam_zero(w, th, id);
        lemma_sfam_zero(w, th2, id);
    }
    if id.d == m {
        // the first cell beyond the new threshold: nothing nonzero between
        assert forall|x: int| dn < x < m implies zero_fam(w, #[trigger] at_d(id, x)) by {
            lemma_dem_zero(c, h, w, th, u, fy, uom, dn, m, x);
            assert(at_d(id, x) == CellId { fy, uom, d: x, a: u });
        }
        let idm = CellId { fy, uom, d: m, a: u };
        assert(id == idm);
        lemma_stored_nonzero(c, sfam(w, th), id);
        let g0 = choose|g: spec_fn(OutRow) -> int| fam_w(sfam(w, th), id, g) != 0;
        assert(keep(w, th, id) && fam_w(w, id, g0) != 0);
        assert(!zero_fam(w, id));
        assert(isb(w, th2, id));
        assert(keep(w, th2, id));
        assert forall|g: spec_fn(OutRow) -> int| #[trigger] fam_w(sfam(w, th2), id, g) == fam_w(sfam(w, th), id, g) by {
            assert(keep(w, th, id));
        }
    }
    if id.d > m {
        assert(!isb(w, th2, id)) by {
            let idm = at_d(id, m);
            let idm_c = CellId { fy, uom, d: m, a: u };
            assert(idm == idm_c);
            lemma_stored_nonzero(c, sfam(w, th), idm_c);
            let g0 = choose|g: spec_fn(OutRow) -> int| fam_w(sfam(w, th), idm_c, g) != 0;
            assert(fam_w(w, idm_c, g0) != 0);
            assert(!zero_fam(w, at_d(id, m)));
        }
        assert(!keep(w, th2, id));
        assert forall|g: spec_fn(OutRow) -> int| #[trigger] fam_w(sfam(w, th2), id, g) == 0 by {}
    }
}

pub open spec fn ck_in(ck: CK, fy: int, uom: Seq<u8>) -> bool {
    ck.fy as int == fy && (*ck.uom)@ == uom
}

/// The first of the filing's stored cells in the class beyond dn.
fn first_tail(c: &Cells, h: &Index<Vec<CK>>, u: &Arc<Vec<u8>>, fy: i64, uom: &Arc<Vec<u8>>, dn: i64) -> (mo: Option<i64>)
    requires
        cells_wf(*c),
        h.wf(),
    ensures
        match mo {
            Some(m) => in_tail(*c, *h, (**u)@, fy as int, (**uom)@, dn as int, m as int)
                && forall|x: int| in_tail(*c, *h, (**u)@, fy as int, (**uom)@, dn as int, x) ==> m <= x,
            None => forall|x: int| !in_tail(*c, *h, (**u)@, fy as int, (**uom)@, dn as int, x),
        },
{
    let l = match h.get(&kkey(0, u)) {
        Some(l) => l,
        None => {
            proof { assert(dir_of(*h, (**u)@) == Seq::<CK>::empty()); }
            return None;
        }
    };
    let ghost lv = l@;
    proof { assert(dir_of(*h, (**u)@) == lv); }
    let mut best: Option<i64> = None;
    let mut j: usize = 0;
    while j < l.len()
        invariant
            j <= l@.len(),
            l@ == lv,
            cells_wf(*c),
            match best {
                Some(m) => (m as int) > dn as int
                    && (exists|q: int| 0 <= q < j && ck_matches(#[trigger] lv[q], CellId { fy: fy as int, uom: (**uom)@, d: m as int, a: (**u)@ }))
                    && cell_at(*c, CellId { fy: fy as int, uom: (**uom)@, d: m as int, a: (**u)@ }) is Some,
                None => true,
            },
            forall|x: int| x > dn as int && (exists|q: int| 0 <= q < j && ck_matches(#[trigger] lv[q], CellId { fy: fy as int, uom: (**uom)@, d: x, a: (**u)@ }))
                && cell_at(*c, CellId { fy: fy as int, uom: (**uom)@, d: x, a: (**u)@ }) is Some
                ==> (best is Some && best->0 as int <= x),
        decreases l.len() - j,
    {
        let ck = &l[j];
        let ghost old_best = best;
        if ck.fy == fy && bytes_eq_exec(ck.uom.as_slice(), uom.as_slice()) && ck.d > dn {
            let stored = cells_get(c, fy, uom, ck.d, u);
            if stored.is_some() {
                let better = match best { Some(m) => ck.d < m, None => true };
                if better {
                    best = Some(ck.d);
                }
            }
        }
        proof {
            let jj = j as int;
            assert forall|x: int| x > dn as int && (exists|q: int| 0 <= q < jj + 1 && ck_matches(#[trigger] lv[q], CellId { fy: fy as int, uom: (**uom)@, d: x, a: (**u)@ }))
                && cell_at(*c, CellId { fy: fy as int, uom: (**uom)@, d: x, a: (**u)@ }) is Some
                implies (best is Some && best->0 as int <= x) by {
                let q = choose|q: int| 0 <= q < jj + 1 && ck_matches(#[trigger] lv[q], CellId { fy: fy as int, uom: (**uom)@, d: x, a: (**u)@ });
                if q == jj { assert(ck.d as int == x); }
            }
            if best != old_best {
                assert(ck.fy == fy && (*ck.uom)@ == (**uom)@ && best == Some(ck.d));
                assert(lv[jj] == *ck);
                assert(ck_matches(lv[jj], CellId { fy: fy as int, uom: (**uom)@, d: ck.d as int, a: (**u)@ }));
            } else if let Some(m) = best {
                let q = choose|q: int| 0 <= q < jj && ck_matches(#[trigger] lv[q], CellId { fy: fy as int, uom: (**uom)@, d: m as int, a: (**u)@ });
            }
        }
        j += 1;
    }
    proof {
        assert forall|x: int| in_tail(*c, *h, (**u)@, fy as int, (**uom)@, dn as int, x) implies (best is Some && best->0 as int <= x) by {
            let q = choose|q: int| 0 <= q < lv.len() && ck_matches(#[trigger] lv[q], CellId { fy: fy as int, uom: (**uom)@, d: x, a: (**u)@ });
        }
        if let Some(m) = best {
            assert(listed(lv, CellId { fy: fy as int, uom: (**uom)@, d: m as int, a: (**u)@ }));
        }
    }
    best
}

pub open spec fn dropck(ck: CK, fy: int, uom: Seq<u8>, dn: int, m: int) -> bool {
    ck_in(ck, fy, uom) && ck.d as int > dn && ck.d as int != m
}

/// The family restricted to one filing's class after lowering: the same function wherever it can be nonzero.
proof fn lemma_dem_other(w: Fam, th: Th, u: Seq<u8>, fy: int, uom: Seq<u8>, dn: int, id: CellId)
    requires
        !(id.a == u && id.fy == fy && id.uom == uom),
    ensures
        forall|g: spec_fn(OutRow) -> int| #[trigger] fam_w(sfam(w, th_dem(th, u, fy, uom, dn)), id, g) == fam_w(sfam(w, th), id, g),
{
    let th2 = th_dem(th, u, fy, uom, dn);
    assert(th_id(th, id) == th_id(th2, id));
    assert forall|d: int, g: spec_fn(OutRow) -> int| #[trigger] fam_w(w, at_d(id, d), g) == fam_w(w, at_d(id, d), g) by {}
    lemma_sfam_unit(w, w, th, th2, id);
}

/// Lower the threshold of one filing in one class: keep only its first stored cell beyond dn.
pub fn demote_unit(c: &mut Cells, h: &mut Index<Vec<CK>>, u: &Arc<Vec<u8>>, fy: i64, uom: &Arc<Vec<u8>>, dn: i64, Ghost(w): Ghost<Fam>, Ghost(th): Ghost<Th>)
    requires
        cells_rep(*old(c), sfam(w, th)),
        cover(*old(h), sfam(w, th)),
        th((**u)@, fy as int, (**uom)@) >= dn as int,
    ensures
        cells_rep(*final(c), sfam(w, th_dem(th, (**u)@, fy as int, (**uom)@, dn as int))),
        cover(*final(h), sfam(w, th_dem(th, (**u)@, fy as int, (**uom)@, dn as int))),
        forall|id: CellId| !(id.fy == fy as int && id.uom == (**uom)@) ==> #[trigger] cell_at(*final(c), id) == cell_at(*old(c), id),
{
    let ghost (uv, fyv, uomv, dnv) = ((**u)@, fy as int, (**uom)@, dn as int);
    let ghost th2 = th_dem(th, uv, fyv, uomv, dnv);
    let ghost w0 = sfam(w, th);
    let ghost w2 = sfam(w, th2);
    let ghost c0 = *old(c);
    let ghost h0 = *old(h);
    let mo = first_tail(c, h, u, fy, uom, dn);
    match mo {
        None => {
            proof {
                assert forall|id: CellId, g: spec_fn(OutRow) -> int| #[trigger] fam_w(w2, id, g) == fam_w(w0, id, g) by {
                    if id.a == uv && id.fy == fyv && id.uom == uomv {
                        if id.d <= dnv {
                            assert(keep(w, th, id) && keep(w, th2, id));
                        } else {
                            lemma_dem_zero(c0, h0, w, th, uv, fyv, uomv, dnv, id.d + 1, id.d);
                            assert(id == CellId { fy: fyv, uom: uomv, d: id.d, a: uv });
                            lemma_sfam_zero(w, th, id);
                            lemma_sfam_zero(w, th2, id);
                        }
                    } else {
                        lemma_dem_other(w, th, uv, fyv, uomv, dnv, id);
                    }
                }
                lemma_rep_congr(c0, w0, w2);
                lemma_cover_congr(h0, w0, w2);
            }
        }
        Some(m) => {
            let ghost mv = m as int;
            let key = kkey(0, u);
            let l = h.get(&key).unwrap();
            let ghost lv = l@;
            proof { assert(dir_of(h0, uv) == lv); }
            let mut newl: Vec<CK> = Vec::new();
            let ghost dp = |ck: CK| dropck(ck, fyv, uomv, dnv, mv);
            let ghost kp = |ck: CK| !dropck(ck, fyv, uomv, dnv, mv);
            let mut j: usize = 0;
            proof { assert(lv.take(0) =~= Seq::<CK>::empty()); }
            while j < l.len()
                invariant
                    j <= l@.len(),
                    l@ == lv,
                    uv == (**u)@,
                    fyv == fy as int, uomv == (**uom)@, mv == m as int, dnv == dn as int,
                    kp == (|ck: CK| !dropck(ck, fyv, uomv, dnv, mv)),
                    newl@ == lv.take(j as int).filter(kp),
                    cells_wf(*c),
                    forall|id: CellId| #[trigger] cell_at(*c, id) == if id.a == uv && (exists|q: int| 0 <= q < j && ck_matches(#[trigger] lv[q], id) && dropck(lv[q], fyv, uomv, dnv, mv)) { None } else { cell_at(c0, id) },
                decreases l.len() - j,
            {
                let ck = &l[j];
                let same_uom = bytes_eq_exec(ck.uom.as_slice(), uom.as_slice());
                let drop = ck.fy == fy && same_uom && ck.d > dn && ck.d != m;
                proof {
                    lemma_filter_step(lv, j as int, kp);
                    assert(lv[j as int] == *ck);
                    assert(same_uom == ((*ck.uom)@ == (**uom)@));
                    assert(drop == dropck(lv[j as int], fyv, uomv, dnv, mv));
                }
                if drop {
                    let ghost before = *c;
                    cells_remove(c, ck.fy, &ck.uom, ck.d, u);
                    proof {
                        assert forall|id: CellId| #[trigger] cell_at(*c, id) == if id.a == uv && (exists|q: int| 0 <= q < j + 1 && ck_matches(#[trigger] lv[q], id) && dropck(lv[q], fyv, uomv, dnv, mv)) { None } else { cell_at(c0, id) } by {
                            let me = CellId { fy: ck.fy as int, uom: (*ck.uom)@, d: ck.d as int, a: uv };
                            if id == me {
                                assert(ck_matches(lv[j as int], id));
                                assert(dropck(lv[j as int], fyv, uomv, dnv, mv));
                            } else {
                                if id.a == uv && (exists|q: int| 0 <= q < j + 1 && ck_matches(#[trigger] lv[q], id) && dropck(lv[q], fyv, uomv, dnv, mv)) {
                                    let q = choose|q: int| 0 <= q < j + 1 && ck_matches(#[trigger] lv[q], id) && dropck(lv[q], fyv, uomv, dnv, mv);
                                    if q == j as int { assert(ck_matches(lv[q], id) ==> id == me); }
                                }
                            }
                        }
                    }
                } else {
                    newl.push(copy_ck(ck));
                    proof {
                        assert(newl@ =~= lv.take(j as int).filter(kp).push(lv[j as int]));
                        assert forall|id: CellId| #[trigger] cell_at(*c, id) == if id.a == uv && (exists|q: int| 0 <= q < j + 1 && ck_matches(#[trigger] lv[q], id) && dropck(lv[q], fyv, uomv, dnv, mv)) { None } else { cell_at(c0, id) } by {
                            if id.a == uv && (exists|q: int| 0 <= q < j + 1 && ck_matches(#[trigger] lv[q], id) && dropck(lv[q], fyv, uomv, dnv, mv)) {
                                let q = choose|q: int| 0 <= q < j + 1 && ck_matches(#[trigger] lv[q], id) && dropck(lv[q], fyv, uomv, dnv, mv);
                                if q == j as int { assert(!dropck(lv[q], fyv, uomv, dnv, mv)); }
                            }
                        }
                    }
                }
                j += 1;
            }
            proof { assert(lv.take(lv.len() as int) =~= lv); }
            h.put(key, Arc::new(newl));
            proof {
                assert forall|id: CellId| !(id.fy == fy as int && id.uom == (**uom)@) implies #[trigger] cell_at(*c, id) == cell_at(c0, id) by {
                    if id.a == uv && (exists|q: int| 0 <= q < lv.len() && ck_matches(#[trigger] lv[q], id) && dropck(lv[q], fyv, uomv, dnv, mv)) {
                        let q = choose|q: int| 0 <= q < lv.len() && ck_matches(#[trigger] lv[q], id) && dropck(lv[q], fyv, uomv, dnv, mv);
                        assert(false);
                    }
                }
                assert(dir_of(*h, uv) == lv.filter(kp));
                assert forall|a: Seq<u8>| a != uv implies dir_of(*h, a) == dir_of(h0, a) by {}
                // cells
                assert forall|id: CellId| #[trigger] cell_ok(cell_at(*c, id), w2, id) by {
                    let dropped = id.a == uv && (exists|q: int| 0 <= q < lv.len() && ck_matches(#[trigger] lv[q], id) && dropck(lv[q], fyv, uomv, dnv, mv));
                    assert(cell_ok(cell_at(c0, id), w0, id));
                    if id.a == uv && id.fy == fyv && id.uom == uomv {
                        lemma_dem_family(c0, h0, w, th, uv, fyv, uomv, dnv, mv, id);
                        if id.d <= dnv || id.d == mv {
                            assert(!dropped) by {
                                if dropped { let q = choose|q: int| 0 <= q < lv.len() && ck_matches(#[trigger] lv[q], id) && dropck(lv[q], fyv, uomv, dnv, mv); }
                            }
                            lemma_cell_ok_congr(cell_at(c0, id), w0, w2, id);
                        } else if id.d < mv {
                            assert(cell_at(c0, id) is None) by { if cell_at(c0, id) is Some { lemma_stored_nonzero(c0, w0, id); } }
                        } else {
                            if listed(lv, id) {
                                let q = choose|q: int| 0 <= q < lv.len() && ck_matches(#[trigger] lv[q], id);
                                assert(dropck(lv[q], fyv, uomv, dnv, mv));
                                assert(dropped);
                            } else {
                                assert(cell_at(c0, id) is None) by {
                                    if cell_at(c0, id) is Some { lemma_stored_nonzero(c0, w0, id); assert(listed(dir_of(h0, uv), id)); }
                                }
                            }
                        }
                    } else {
                        lemma_dem_other(w, th, uv, fyv, uomv, dnv, id);
                        assert(!dropped) by {
                            if dropped { let q = choose|q: int| 0 <= q < lv.len() && ck_matches(#[trigger] lv[q], id) && dropck(lv[q], fyv, uomv, dnv, mv); }
                        }
                        lemma_cell_ok_congr(cell_at(c0, id), w0, w2, id);
                    }
                }
                // cover
                assert forall|id: CellId| !(#[trigger] zero_fam(w2, id)) implies listed(dir_of(*h, id.a), id) by {
                    if id.a == uv {
                        if id.fy == fyv && id.uom == uomv {
                            lemma_dem_family(c0, h0, w, th, uv, fyv, uomv, dnv, mv, id);
                            assert(id.d <= dnv || id.d == mv) by {
                                if !(id.d <= dnv || id.d == mv) {
                                    if id.d < mv { lemma_sfam_zero(w, th2, id); }
                                    assert(zero_fam(w2, id));
                                }
                            }
                            assert(!zero_fam(w0, id)) by {
                                let g = choose|g: spec_fn(OutRow) -> int| fam_w(w2, id, g) != 0;
                                assert(fam_w(w0, id, g) != 0);
                            }
                            let q = choose|q: int| 0 <= q < lv.len() && ck_matches(#[trigger] lv[q], id);
                            assert(kp(lv[q]));
                            lv.lemma_filter_contains(kp, q);
                            assert(listed(lv.filter(kp), id)) by {
                                let q2 = choose|q2: int| 0 <= q2 < lv.filter(kp).len() && lv.filter(kp)[q2] == lv[q];
                                assert(ck_matches(lv.filter(kp)[q2], id));
                            }
                        } else {
                            lemma_dem_other(w, th, uv, fyv, uomv, dnv, id);
                            assert(!zero_fam(w0, id)) by {
                                let g = choose|g: spec_fn(OutRow) -> int| fam_w(w2, id, g) != 0;
                                assert(fam_w(w0, id, g) != 0);
                            }
                            let q = choose|q: int| 0 <= q < lv.len() && ck_matches(#[trigger] lv[q], id);
                            assert(kp(lv[q])) by { assert(!ck_in(lv[q], fyv, uomv) || true); }
                            lv.lemma_filter_contains(kp, q);
                            assert(listed(lv.filter(kp), id)) by {
                                let q2 = choose|q2: int| 0 <= q2 < lv.filter(kp).len() && lv.filter(kp)[q2] == lv[q];
                                assert(ck_matches(lv.filter(kp)[q2], id));
                            }
                        }
                    } else {
                        lemma_dem_other(w, th, uv, fyv, uomv, dnv, id);
                        assert(!zero_fam(w0, id)) by {
                            let g = choose|g: spec_fn(OutRow) -> int| fam_w(w2, id, g) != 0;
                            assert(fam_w(w0, id, g) != 0);
                        }
                        assert(dir_of(*h, id.a) == dir_of(h0, id.a));
                    }
                }
            }
        }
    }
}

/// The thresholds of a class lowered to dn for the listed filings.
pub open spec fn th_acc(t: Thr, fy: int, uom: Seq<u8>, us: Seq<Seq<u8>>, dn: int) -> Th {
    |b: Seq<u8>, f2: int, u2: Seq<u8>| if f2 == fy && u2 == uom && us.contains(b) { dn } else { thr_of(t, f2, u2) }
}

/// The filings with a stored cell of the class whose value key is in (dn, D].
fn collect_units(inner: &Index<Vec<Row>>, dn: i64, d: i64) -> (us: Vec<Arc<Vec<u8>>>)
    requires
        inner.wf(),
        dn < D_INF,
    ensures
        forall|e: (KeyView, Vec<Row>)| inner.entries().contains(e) && e.0.0 > dn as int && e.0.0 <= d as int ==> pick_view(us@).contains(e.0.1),
{
    let ghost es = inner.entries();
    proof { inner.model(); lemma_lead(es, dn as int); lemma_lower_suffix(es, bound_of(dn as int)); }
    let ghost lp = lead(es, dn as int);
    let ghost tl = es.skip(lp);
    let bkey = Key::number(dn + 1);
    let mut cur = inner.cursor_from(&bkey);
    proof { assert(bkey.view() == bound_of(dn as int)); assert(cur.remaining() == tl); }
    let mut us: Vec<Arc<Vec<u8>>> = Vec::new();
    let ghost mut j: int = 0;
    proof { assert(tl.skip(0) =~= tl); }
    loop
        invariant_except_break
            cur.remaining() == tl.skip(j),
        invariant
            inner.wf(),
            es == inner.entries(),
            tl == es.skip(lp),
            0 <= lp <= es.len(),
            ordered_rows(es),
            forall|i: int| 0 <= i < lp ==> (#[trigger] es[i]).0.0 <= dn as int,
            forall|i: int| lp <= i < es.len() ==> (#[trigger] es[i]).0.0 > dn as int,
            0 <= j <= tl.len(),
            cur.work() >= 0,
            forall|q: int| 0 <= q < j && (#[trigger] tl[q]).0.0 <= d as int ==> pick_view(us@).contains(tl[q].0.1),
        ensures
            0 <= j <= tl.len(),
            forall|q: int| 0 <= q < j && (#[trigger] tl[q]).0.0 <= d as int ==> pick_view(us@).contains(tl[q].0.1),
            forall|q: int| j <= q < tl.len() ==> (#[trigger] tl[q]).0.0 > d as int,
        decreases cur.work(),
    {
        let ghost before = cur.remaining();
        match cur.next() {
            None => {
                proof { assert(j == tl.len()); }
                break;
            }
            Some((k, cell)) => {
                proof {
                    assert(before[0] == tl[j]);
                    assert(cur.remaining() == tl.skip(j).skip(1));
                    assert(tl.skip(j).skip(1) =~= tl.skip(j + 1));
                    assert((k.view(), *cell) == tl[j]);
                }
                if k.number > d {
                    proof {
                        assert forall|q: int| j <= q < tl.len() implies (#[trigger] tl[q]).0.0 > d as int by {
                            if q > j { assert(key_lt(tl[j].0, tl[q].0)); }
                        }
                    }
                    break;
                }
                let ghost old = us@;
                us.push(k.text.clone());
                proof {
                    assert(pick_view(us@) =~= pick_view(old).push((*k.text)@));
                    assert(tl[j].0.1 == (*k.text)@);
                    assert forall|q: int| 0 <= q < j + 1 && (#[trigger] tl[q]).0.0 <= d as int implies pick_view(us@).contains(tl[q].0.1) by {
                        if q == j { assert(pick_view(us@)[pick_view(us@).len() - 1] == tl[j].0.1); }
                        else { assert(pick_view(old).contains(tl[q].0.1)); let p = choose|p: int| 0 <= p < pick_view(old).len() && pick_view(old)[p] == tl[q].0.1; assert(pick_view(us@)[p] == pick_view(old)[p]); }
                    }
                    j = j + 1;
                }
            }
        }
    }
    proof {
        assert forall|e: (KeyView, Vec<Row>)| es.contains(e) && e.0.0 > dn as int && e.0.0 <= d as int implies pick_view(us@).contains(e.0.1) by {
            let p = choose|p: int| 0 <= p < es.len() && es[p] == e;
            if p < lp { assert(es[p].0.0 <= dn as int); }
            else {
                assert(tl[p - lp] == e);
                if p - lp >= j { assert(tl[p - lp].0.0 > d as int); }
            }
        }
    }
    us
}

/// The threshold to lower a class to, when it stores more than KMAX cells within d.
fn trim_point(inner: &Index<Vec<Row>>, d: i64) -> (r: Option<i64>)
    requires
        inner.wf(),
    ensures
        match r {
            Some(dn) => inner.entries().len() > KMAX && inner.entries()[KMAX as int].0.0 <= d as int
                && dn as int == inner.entries()[TRIMTO as int - 1].0.0 && (dn as int) < d as int,
            None => true,
        },
{
    let ghost es = inner.entries();
    proof { inner.model(); }
    let mut cur = inner.cursor();
    let mut i: usize = 0;
    let mut a: i64 = 0;
    let mut more = false;
    proof { assert(es.skip(0) =~= es); }
    loop
        invariant_except_break
            cur.remaining() == es.skip(i as int),
            !more,
        invariant
            i <= KMAX,
            i <= es.len(),
            cur.work() >= 0,
            i >= TRIMTO ==> a as int == es[TRIMTO as int - 1].0.0,
        ensures
            i <= KMAX,
            i <= es.len(),
            i >= TRIMTO ==> a as int == es[TRIMTO as int - 1].0.0,
            more ==> (i == KMAX && es.len() > KMAX),
        decreases cur.work(),
    {
        let ghost before = cur.remaining();
        if i == KMAX {
            proof { assert(es.len() > KMAX || es.len() == i); }
            if true {
                match cur.next() {
                    None => { proof { assert(es.skip(i as int).len() == 0); } break; }
                    Some(_) => {
                        proof { assert(before.len() > 0); assert(es.len() > KMAX); }
                        more = true;
                        break;
                    }
                }
            }
        }
        match cur.next() {
            None => { proof { assert(es.skip(i as int).len() == 0); } break; }
            Some((k, cell)) => {
                proof {
                    assert(before.len() > 0);
                    assert(before[0] == es[i as int]);
                    assert(cur.remaining() == es.skip(i as int).skip(1));
                    assert(es.skip(i as int).skip(1) =~= es.skip(i as int + 1));
                    assert((k.view(), *cell) == es[i as int]);
                }
                if i == TRIMTO - 1 {
                    a = k.number;
                }
                i += 1;
            }
        }
    }
    if more && a < d {
        // the entry at KMAX: its value key is at least a's; read it again by a fresh cursor
        let mut cur2 = inner.cursor();
        let mut q: usize = 0;
        proof { assert(es.skip(0) =~= es); }
        let mut b: i64 = 0;
        while q <= KMAX
            invariant
                q <= KMAX + 1,
                KMAX < es.len(),
                cur2.remaining() == es.skip(q as int),
                cur2.work() >= 0,
                q > KMAX ==> b as int == es[KMAX as int].0.0,
            decreases KMAX + 1 - q,
        {
            let ghost before = cur2.remaining();
            match cur2.next() {
                None => { proof { assert(false); } }
                Some((k, cell)) => {
                    proof {
                        assert(before[0] == es[q as int]);
                        assert(cur2.remaining() == es.skip(q as int).skip(1));
                        assert(es.skip(q as int).skip(1) =~= es.skip(q as int + 1));
                        assert((k.view(), *cell) == es[q as int]);
                    }
                    if q == KMAX { b = k.number; }
                    q += 1;
                }
            }
        }
        if b <= d { return Some(a); }
    }
    None
}

/// Lower one class's threshold when it stores more than KMAX cells within it.
#[verifier::rlimit(200)]
pub fn trim_class(c: &mut Cells, h: &mut Index<Vec<CK>>, thr: &mut Thr, fy: i64, uom: &Arc<Vec<u8>>, Ghost(w): Ghost<Fam>)
    requires
        cells_rep(*old(c), sfam(w, th_of(*old(thr)))),
        cover(*old(h), sfam(w, th_of(*old(thr)))),
        old(thr).wf(),
        thr_ok(*old(c), *old(thr)),
    ensures
        cells_rep(*final(c), sfam(w, th_of(*final(thr)))),
        cover(*final(h), sfam(w, th_of(*final(thr)))),
        final(thr).wf(),
        thr_ok(*final(c), *final(thr)),
{
    let ghost c0 = *old(c);
    let ghost thr0 = *old(thr);
    let ghost fyv = fy as int;
    let ghost uomv = (**uom)@;
    proof { lemma_cells_range(c0, sfam(w, th_of(thr0))); }
    let inner = match c.get(&kkey(fy, uom)) {
        Some(i) => i,
        None => { return; }
    };
    proof { assert(c0.at((fyv, uomv)) == Some(*inner)); assert(class_entries(c0, fyv, uomv) == inner.entries()); inner.model(); }
    let d = thr_get(thr, fy, uom);
    let dn = match trim_point(&*inner, d) {
        Some(x) => x,
        None => { return; }
    };
    let ghost dnv = dn as int;
    let ghost es = class_entries(c0, fyv, uomv);
    proof { assert(dn < D_INF); }
    let us = collect_units(&*inner, dn, d);
    let ghost uv_all = pick_view(us@);
    let mut i: usize = 0;
    proof {
        assert forall|id: CellId, g: spec_fn(OutRow) -> int| #[trigger] fam_w(sfam(w, th_acc(thr0, fyv, uomv, uv_all.take(0), dnv)), id, g) == fam_w(sfam(w, th_of(thr0)), id, g) by {
            assert(!uv_all.take(0).contains(id.a));
            assert(th_id(th_acc(thr0, fyv, uomv, uv_all.take(0), dnv), id) == th_id(th_of(thr0), id));
            lemma_sfam_unit(w, w, th_acc(thr0, fyv, uomv, uv_all.take(0), dnv), th_of(thr0), id);
        }
        lemma_rep_congr(*c, sfam(w, th_of(thr0)), sfam(w, th_acc(thr0, fyv, uomv, uv_all.take(0), dnv)));
        lemma_cover_congr(*h, sfam(w, th_of(thr0)), sfam(w, th_acc(thr0, fyv, uomv, uv_all.take(0), dnv)));
    }
    while i < us.len()
        invariant
            i <= us@.len(),
            uv_all == pick_view(us@),
            fyv == fy as int, uomv == (**uom)@, dnv == dn as int,
            dn < d,
            d as int == thr_of(thr0, fyv, uomv),
            cells_rep(*c, sfam(w, th_acc(thr0, fyv, uomv, uv_all.take(i as int), dnv))),
            cover(*h, sfam(w, th_acc(thr0, fyv, uomv, uv_all.take(i as int), dnv))),
            forall|id: CellId| !(id.fy == fyv && id.uom == uomv) ==> #[trigger] cell_at(*c, id) == cell_at(c0, id),
        decreases us.len() - i,
    {
        let ghost thi = th_acc(thr0, fyv, uomv, uv_all.take(i as int), dnv);
        let ghost cb = *c;
        proof { assert(thi((*us@[i as int])@, fyv, uomv) >= dnv); }
        demote_unit(c, h, &us[i], fy, uom, dn, Ghost(w), Ghost(thi));
        proof {
            let u = (*us@[i as int])@;
            let th2 = th_dem(thi, u, fyv, uomv, dnv);
            let thn = th_acc(thr0, fyv, uomv, uv_all.take(i as int + 1), dnv);
            assert(uv_all.take(i as int + 1) =~= uv_all.take(i as int).push(uv_all[i as int]));
            assert(uv_all[i as int] == u);
            assert forall|id: CellId, g: spec_fn(OutRow) -> int| #[trigger] fam_w(sfam(w, thn), id, g) == fam_w(sfam(w, th2), id, g) by {
                crate::filing::lemma_push_contains2(uv_all.take(i as int), u, id.a);
                assert(th_id(thn, id) == th_id(th2, id));
                lemma_sfam_unit(w, w, thn, th2, id);
            }
            lemma_rep_congr(*c, sfam(w, th2), sfam(w, thn));
            lemma_cover_congr(*h, sfam(w, th2), sfam(w, thn));
        }
        i += 1;
    }
    proof { assert(uv_all.take(uv_all.len() as int) =~= uv_all); }
    thr.put(kkey(fy, uom), Arc::new(dn));
    proof {
        let thf = th_acc(thr0, fyv, uomv, uv_all, dnv);
        let thn = th_of(*thr);
        assert forall|id: CellId, g: spec_fn(OutRow) -> int| #[trigger] fam_w(sfam(w, thn), id, g) == fam_w(sfam(w, thf), id, g) by {
            if id.fy == fyv && id.uom == uomv {
                if uv_all.contains(id.a) {
                    assert(th_id(thn, id) == th_id(thf, id));
                    lemma_sfam_unit(w, w, thn, thf, id);
                } else {
                    assert(th_id(thn, id) == dnv && th_id(thf, id) == d as int);
                    assert forall|x: int| th_id(thn, id) < x <= th_id(thf, id) implies zero_fam(w, #[trigger] at_d(id, x)) by {
                        let idx = at_d(id, x);
                        assert(cell_at(c0, idx) is None) by {
                            if cell_at(c0, idx) is Some {
                                let row = (( x, id.a), cell_at(c0, idx)->0);
                                assert(es.contains(row));
                                assert(uv_all.contains(id.a));
                            }
                        }
                        assert(cell_ok(cell_at(c0, idx), sfam(w, th_of(thr0)), idx));
                        assert(keep(w, th_of(thr0), idx));
                        assert forall|g2: spec_fn(OutRow) -> int| fam_w(w, idx, g2) == 0 by { assert(fam_w(sfam(w, th_of(thr0)), idx, g2) == 0); }
                    }
                    lemma_keep_eq(w, thn, thf, id);
                }
            } else {
                assert(th_id(thn, id) == th_id(thf, id));
                lemma_sfam_unit(w, w, thn, thf, id);
            }
        }
        lemma_rep_congr(*c, sfam(w, thf), sfam(w, thn));
        lemma_cover_congr(*h, sfam(w, thf), sfam(w, thn));
        // a hundred cells within the new threshold stay stored
        assert forall|f2: int, u2: Seq<u8>| #[trigger] thr_of(*thr, f2, u2) == D_INF as int || exists|ks: Seq<KeyView>| witnessed(*c, f2, u2, thr_of(*thr, f2, u2), ks) by {
            if (f2, u2) != (fyv, uomv) {
                assert(thr.at((f2, u2)) == thr0.at((f2, u2)));
                if thr_of(*thr, f2, u2) != D_INF as int {
                    let ks = choose|ks: Seq<KeyView>| witnessed(c0, f2, u2, thr_of(thr0, f2, u2), ks);
                    assert forall|q: int| 0 <= q < ks.len() implies cell_at(*c, CellId { fy: f2, uom: u2, d: ks[q].0, a: ks[q].1 }) is Some by {
                        let idq = CellId { fy: f2, uom: u2, d: ks[q].0, a: ks[q].1 };
                        assert(cell_at(c0, idq) is Some);
                        assert(cell_at(*c, idq) == cell_at(c0, idq));
                    }
                    assert(witnessed(*c, f2, u2, thr_of(*thr, f2, u2), ks));
                }
            } else {
                let ks = es.take(TRIMTO as int).map_values(|e: (KeyView, Vec<Row>)| e.0);
                assert(ks.len() == TRIMTO);
                assert forall|p: int, q: int| 0 <= p < q < ks.len() implies ks[p] != ks[q] by {
                    assert(key_lt(es[p].0, es[q].0));
                    verus_prooflib::ordered::lemma_key_order(es[p].0, es[q].0);
                }
                assert(ks.no_duplicates());
                assert forall|q: int| 0 <= q < ks.len() implies (#[trigger] ks[q]).0 <= dnv && cell_at(*c, CellId { fy: fyv, uom: uomv, d: ks[q].0, a: ks[q].1 }) is Some by {
                    let idq = CellId { fy: fyv, uom: uomv, d: es[q].0.0, a: es[q].0.1 };
                    assert(ks[q] == es[q].0);
                    if q < TRIMTO as int - 1 { assert(key_lt(es[q].0, es[TRIMTO as int - 1].0)); }
                    assert(inner.at(es[q].0) == Some(es[q].1));
                    assert(cell_at(c0, idq) is Some);
                    lemma_stored_nonzero(c0, sfam(w, th_of(thr0)), idq);
                    let g0 = choose|g0: spec_fn(OutRow) -> int| fam_w(sfam(w, th_of(thr0)), idq, g0) != 0;
                    assert(keep(w, th_of(thr0), idq) && fam_w(w, idq, g0) != 0);
                    assert(keep(w, thn, idq));
                    assert(fam_w(sfam(w, thn), idq, g0) != 0);
                    lemma_nonzero_stored(*c, sfam(w, thn), idq);
                }
                assert(thr_of(*thr, fyv, uomv) == dnv);
                assert(witnessed(*c, fyv, uomv, dnv, ks));
            }
        }
    }
}

/// Trim every class.
pub fn trim_all(c: &mut Cells, h: &mut Index<Vec<CK>>, thr: &mut Thr, Ghost(w): Ghost<Fam>)
    requires
        cells_rep(*old(c), sfam(w, th_of(*old(thr)))),
        cover(*old(h), sfam(w, th_of(*old(thr)))),
        old(thr).wf(),
        thr_ok(*old(c), *old(thr)),
    ensures
        cells_rep(*final(c), sfam(w, th_of(*final(thr)))),
        cover(*final(h), sfam(w, th_of(*final(thr)))),
        final(thr).wf(),
        thr_ok(*final(c), *final(thr)),
{
    let rows = c.rows();
    let mut i: usize = 0;
    while i < rows.len()
        invariant
            i <= rows@.len(),
            cells_rep(*c, sfam(w, th_of(*thr))),
            cover(*h, sfam(w, th_of(*thr))),
            thr.wf(),
            thr_ok(*c, *thr),
        decreases rows.len() - i,
    {
        let key = rows[i].0.share();
        let uom = key.text.clone();
        trim_class(c, h, thr, key.number, &uom, Ghost(w));
        i += 1;
    }
}

} // verus!
