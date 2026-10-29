// Stage A of a commit: the changed rows joined with the other table, as contributions to the groups.
// A changed num row meets the sub rows before the commit (each with its multiplicity); a changed sub row
// meets the num rows after it.  Their sum is the change of the join (`lemma_join_bag_step`).
use vstd::prelude::*;
use std::sync::Arc;
use verus_common::bag::*;
use verus_common::bytes::*;
use verus_common::rel::*;
use verus_common::sums::*;
use verus_prooflib::*;
use verus_prooflib::ordered::{Index, Key, KeyView};
use verus_prooflib::wide::Wide;
use crate::bucket::*;
use crate::dmap::*;
use crate::ent::*;
use crate::exact::*;
use crate::gen_obligations::*;
use crate::gen_spec::*;
use crate::gkey::*;
use crate::join_spec::*;
use crate::keys::*;
use crate::numeric::*;
use crate::qty::*;
use crate::rows::*;
use crate::wbag::*;

verus! {

pub open spec fn fsub(p: Params, h: spec_fn(JK) -> int, n: NumRow) -> spec_fn(SubRow) -> int {
    |s: SubRow| wt(p, h)(n, s)
}

pub open spec fn fnum(p: Params, h: spec_fn(JK) -> int, s: SubRow) -> spec_fn(NumRow) -> int {
    |n: NumRow| wt(p, h)(n, s)
}

pub fn copy_sub_name(s: &SubRt) -> (r: Option<Vec<u8>>)
    ensures opt_bytes(r) == sub_rt_view(*s).name,
{
    if s.name__null != 0 { None } else { Some(copy_bytes(s.name.as_slice())) }
}

pub fn sub_cik(s: &SubRt) -> (r: Option<i64>)
    ensures opt_int(r) == sub_rt_view(*s).cik,
{
    if s.cik__null != 0 { None } else { Some(s.cik) }
}

fn abs_wide(v: &Wide) -> (m: Wide)
    ensures m@ == abs(v@),
{
    if v.is_neg() { v.neg() } else { v.dup() }
}

pub open spec fn num_guard(n: NumRt) -> bool {
    n.uom__null == 0 && n.value__null == 0 && n.adsh__null == 0
}

/// A changed num row (sign sgn) against the sub rows of its filing.
#[verifier::spinoff_prover]
pub fn emit_nd(dm: &mut Index<DCls>, n: &NumRt, sgn: i64, sv: &Vec<WRow>, pows: &Vec<Wide>, Ghost(l): Ghost<Seq<Ct>>) -> (r: Ghost<Seq<Ct>>)
    requires
        ddm_ok(*old(dm), l),
        pows_ok(pows@),
        sgn == 1 || sgn == -1,
    ensures
        ddm_ok(*final(dm), r@),
        forall|p: Params, h: spec_fn(JK) -> int| #[trigger] lsum(r@, p, h)
            == lsum(l, p, h) + sgn as int * wsum(sv@, fsub(p, h, num_rt_view(*n))),
{
    let ghost nrow = num_rt_view(*n);
    let cn = n.uom__null == 0 && n.value__null == 0 && n.adsh__null == 0;
    if !cn {
        proof {
            assert forall|p: Params, h: spec_fn(JK) -> int| #[trigger] wsum(sv@, fsub(p, h, nrow)) == 0 by {
                assert forall|i: int| 0 <= i < sv@.len() implies (|w: WRow| w.m@ * fsub(p, h, nrow)(sub_rt_view(w.row)))(#[trigger] sv@[i]) == 0 by {
                    assert(!c_n(p, nrow));
                }
                lemma_sum_zero(sv@, |w: WRow| w.m@ * fsub(p, h, nrow)(sub_rt_view(w.row)));
            }
        }
        return Ghost(l);
    }
    let v = wide_of_row_bits(n.value, pows);
    let mag = abs_wide(&v);
    let sgnw = Wide::from_i128(sgn as i128);
    let ghost mut li = l;
    let mut i: usize = 0;
    proof {
        assert forall|p: Params, h: spec_fn(JK) -> int| #[trigger] lsum(li, p, h) == lsum(l, p, h) + sgn as int * wsum(sv@.take(0), fsub(p, h, nrow)) by {
            assert(sv@.take(0) =~= Seq::<WRow>::empty());
        }
    }
    while i < sv.len()
        invariant
            i <= sv.len(),
            ddm_ok(*dm, li),
            pows_ok(pows@),
            sgn == 1 || sgn == -1,
            cn,
            n.uom__null == 0 && n.value__null == 0 && n.adsh__null == 0,
            nrow == num_rt_view(*n),
            v@ == f64_val(n.value),
            mag@ == abs(v@),
            sgnw@ == sgn as int,
            forall|p: Params, h: spec_fn(JK) -> int| #[trigger] lsum(li, p, h)
                == lsum(l, p, h) + sgn as int * wsum(sv@.take(i as int), fsub(p, h, nrow)),
        decreases sv.len() - i,
    {
        let w = &sv[i];
        let s = &w.row;
        let hit = s.fy__null == 0 && bytes_eq_exec(s.adsh.as_slice(), n.adsh.as_slice());
        let ghost srow = sub_rt_view(*s);
        let ghost li0 = li;
        if hit {
            let sg = w.m.mul(&sgnw);
            let name = copy_sub_name(s);
            let cik = sub_cik(s);
            let ghost ct = Ct { fy: s.fy as int, uom: n.uom@, j: jk(nrow, srow), sign: w.m@ * sgn as int };
            proof { assert(nrow.value == Some(f64_val(n.value))); assert(ct.j.vs); assert(ct.j.v == f64_val(n.value)); }
            acc_ct(dm, s.fy, n.uom.as_slice(), &name, cik, &sg, &v, &mag, Ghost(li), Ghost(ct));
            proof {
                li = li.push(ct);
                assert(sg@ == w.m@ * sgn as int);
                assert(ct.j.v == f64_val(n.value));
            }
        }
        proof {
            let take_i = sv@.take(i as int);
            assert(sv@.take(i as int + 1) =~= take_i.push(sv@[i as int]));
            assert forall|p: Params, h: spec_fn(JK) -> int| #[trigger] lsum(li, p, h)
                == lsum(l, p, h) + sgn as int * wsum(sv@.take(i as int + 1), fsub(p, h, nrow)) by {
                let f = |x: WRow| x.m@ * fsub(p, h, nrow)(sub_rt_view(x.row));
                lemma_sum_push(take_i, sv@[i as int], f);
                let m = w.m@;
                let fx = wt(p, h)(nrow, srow);
                assert(f(sv@[i as int]) == m * fx);
                assert(lsum(li0, p, h) == lsum(l, p, h) + sgn as int * wsum(take_i, fsub(p, h, nrow)));
                if hit {
                    let ct = li.last();
                    assert(li =~= li0.push(ct));
                    lemma_sum_push(li0, ct, |c: Ct| if ct_in(c, p) { c.sign * h(c.j) } else { 0 });
                    assert(lsum(li, p, h) == lsum(li0, p, h) + (if ct_in(ct, p) { ct.sign * h(ct.j) } else { 0 }));
                    assert(c_n(p, nrow) == (n.uom@ == p.p2));
                    assert(c_s(p, srow) == (s.fy as int == p.p1));
                    assert(m_s(nrow, srow) == (n.adsh@ == s.adsh@));
                    if ct_in(ct, p) {
                        assert(fx == h(jk(nrow, srow)));
                        assert(sgn as int * (m * fx) == (m * sgn as int) * fx) by (nonlinear_arith);
                    } else {
                        assert(fx == 0);
                        assert(sgn as int * (m * fx) == 0) by (nonlinear_arith) requires fx == 0;
                    }
                } else {
                    assert(li =~= li0);
                    assert(fx == 0);
                    assert(sgn as int * (m * fx) == 0) by (nonlinear_arith) requires fx == 0;
                }
                assert(sgn as int * (wsum(take_i, fsub(p, h, nrow)) + m * fx)
                    == sgn as int * wsum(take_i, fsub(p, h, nrow)) + sgn as int * (m * fx)) by (nonlinear_arith);
            }
        }
        i += 1;
    }
    proof { assert(sv@.take(sv.len() as int) =~= sv@); }
    Ghost(li)
}

/// A changed sub row (sign sg) against the num rows of its filing.
#[verifier::spinoff_prover]
pub fn emit_sd(dm: &mut Index<DCls>, s: &SubRt, sg: i64, nr: &Vec<NumRt>, pows: &Vec<Wide>, Ghost(l): Ghost<Seq<Ct>>) -> (r: Ghost<Seq<Ct>>)
    requires
        ddm_ok(*old(dm), l),
        pows_ok(pows@),
        sg == 1 || sg == -1,
    ensures
        ddm_ok(*final(dm), r@),
        forall|p: Params, h: spec_fn(JK) -> int| #[trigger] lsum(r@, p, h)
            == lsum(l, p, h) + sg as int * sum(rows_v(nr@), fnum(p, h, sub_rt_view(*s))),
{
    let ghost srow = sub_rt_view(*s);
    let sgw = Wide::from_i128(sg as i128);
    let ghost mut li = l;
    let mut i: usize = 0;
    proof {
        assert forall|p: Params, h: spec_fn(JK) -> int| #[trigger] lsum(li, p, h)
            == lsum(l, p, h) + sg as int * sum(rows_v(nr@.take(0)), fnum(p, h, srow)) by {
            assert(rows_v(nr@.take(0)) =~= Seq::<NumRow>::empty());
        }
    }
    while i < nr.len()
        invariant
            i <= nr.len(),
            ddm_ok(*dm, li),
            pows_ok(pows@),
            sg == 1 || sg == -1,
            srow == sub_rt_view(*s),
            sgw@ == sg as int,
            forall|p: Params, h: spec_fn(JK) -> int| #[trigger] lsum(li, p, h)
                == lsum(l, p, h) + sg as int * sum(rows_v(nr@.take(i as int)), fnum(p, h, srow)),
        decreases nr.len() - i,
    {
        let n = &nr[i];
        let hit = s.fy__null == 0 && n.uom__null == 0 && n.value__null == 0 && n.adsh__null == 0
            && bytes_eq_exec(s.adsh.as_slice(), n.adsh.as_slice());
        let ghost nrow = num_rt_view(*n);
        let ghost li0 = li;
        if hit {
            let v = wide_of_row_bits(n.value, pows);
            let mag = abs_wide(&v);
            let name = copy_sub_name(s);
            let cik = sub_cik(s);
            let ghost ct = Ct { fy: s.fy as int, uom: n.uom@, j: jk(nrow, srow), sign: sg as int };
            proof { assert(nrow.value == Some(f64_val(n.value))); assert(ct.j.vs); assert(ct.j.v == f64_val(n.value)); }
            acc_ct(dm, s.fy, n.uom.as_slice(), &name, cik, &sgw, &v, &mag, Ghost(li), Ghost(ct));
            proof {
                li = li.push(ct);
                assert(ct.j.v == f64_val(n.value));
            }
        }
        proof {
            let prefix = rows_v(nr@.take(i as int));
            assert(nr@.take(i as int + 1) =~= nr@.take(i as int).push(nr@[i as int]));
            assert(rows_v(nr@.take(i as int + 1)) =~= prefix.push(nrow));
            assert forall|p: Params, h: spec_fn(JK) -> int| #[trigger] lsum(li, p, h)
                == lsum(l, p, h) + sg as int * sum(rows_v(nr@.take(i as int + 1)), fnum(p, h, srow)) by {
                let f = fnum(p, h, srow);
                lemma_sum_push(prefix, nrow, f);
                let fx = wt(p, h)(nrow, srow);
                assert(f(nrow) == fx);
                assert(lsum(li0, p, h) == lsum(l, p, h) + sg as int * sum(prefix, f));
                if hit {
                    let ct = li.last();
                    assert(li =~= li0.push(ct));
                    lemma_sum_push(li0, ct, |c: Ct| if ct_in(c, p) { c.sign * h(c.j) } else { 0 });
                    assert(lsum(li, p, h) == lsum(li0, p, h) + (if ct_in(ct, p) { ct.sign * h(ct.j) } else { 0 }));
                    assert(c_n(p, nrow) == (n.uom@ == p.p2));
                    assert(c_s(p, srow) == (s.fy as int == p.p1));
                    assert(m_s(nrow, srow) == (n.adsh@ == s.adsh@));
                    if ct_in(ct, p) { assert(fx == h(jk(nrow, srow))); } else { assert(fx == 0); }
                } else {
                    assert(li =~= li0);
                    assert(fx == 0);
                }
                assert(sg as int * (sum(prefix, f) + fx) == sg as int * sum(prefix, f) + sg as int * fx) by (nonlinear_arith);
            }
        }
        i += 1;
    }
    proof { assert(nr@.take(nr.len() as int) =~= nr@); }
    Ghost(li)
}


/// The sub rows of a filing, with multiplicities; none for a NULL filing.
fn wbucket(subs: &Index<Vec<WRow>>, n: &NumRt) -> (v: Arc<Vec<WRow>>)
    ensures
        v@ == (if num_rt_view(*n).adsh is Some { wrows_at(*subs, key1(num_rt_view(*n).adsh->0)) } else { Seq::empty() }),
{
    if n.adsh__null != 0 {
        Arc::new(Vec::new())
    } else {
        let k = make_key1(n.adsh.as_slice());
        match subs.get(&k) {
            Some(a) => a,
            None => Arc::new(Vec::new()),
        }
    }
}

/// A num row's sum over the sub rows of its filing, from the buckets.
proof fn lemma_n_term(subs: Index<Vec<WRow>>, dbsub: Seq<SubRow>, sv: Seq<WRow>, p: Params, h: spec_fn(JK) -> int, nrow: NumRow)
    requires
        wbag_as(subs, phi_tab(dbsub)),
        sv == (if nrow.adsh is Some { wrows_at(subs, key1(nrow.adsh->0)) } else { Seq::empty() }),
    ensures
        wsum(sv, fsub(p, h, nrow)) == sum(dbsub, |s: SubRow| wt(p, h)(nrow, s)),
{
    let f = fsub(p, h, nrow);
    if nrow.adsh is Some {
        let k = key1(nrow.adsh->0);
        assert(wsum(wrows_at(subs, k), f) == phi_tab(dbsub)(skw(k, f)));
        assert forall|s: SubRow| #[trigger] skw(k, f)(s) == f(s) by {
            if f(s) != 0 {
                assert(m_s(nrow, s));
                assert(skey(s) == k);
            }
        }
        lemma_sum_ext_all(dbsub, skw(k, f), f);
        assert(sum(dbsub, f) == sum(dbsub, |s: SubRow| wt(p, h)(nrow, s)));
    } else {
        assert(sv =~= Seq::<WRow>::empty());
        assert forall|s: SubRow| #[trigger] f(s) == 0 by {}
        lemma_sum_zero(dbsub, f);
        assert(wsum(sv, f) == 0);
        assert(sum(dbsub, |s: SubRow| wt(p, h)(nrow, s)) == sum(dbsub, f));
    }
}

/// The batch's changed num rows against the sub table before the commit.
pub fn stage_n(dm: &mut Index<DCls>, nd: &Vec<NumDelta>, subs: &Index<Vec<WRow>>, pows: &Vec<Wide>, Ghost(dbsub): Ghost<Seq<SubRow>>) -> (r: Ghost<Seq<Ct>>)
    requires
        ddm_ok(*old(dm), Seq::empty()),
        wbag_as(*subs, phi_tab(dbsub)),
        pows_ok(pows@),
        forall|i: int| 0 <= i < nd.len() ==> (#[trigger] nd[i]).sign == 1 || nd[i].sign == -1,
    ensures
        ddm_ok(*final(dm), r@),
        forall|p: Params, h: spec_fn(JK) -> int| #[trigger] lsum(r@, p, h) == left_join_delta(num_delta_seq_view(nd@), dbsub, wt(p, h)),
{
    let ghost view = num_delta_seq_view(nd@);
    let ghost mut li: Seq<Ct> = Seq::empty();
    let mut i: usize = 0;
    proof {
        assert forall|p: Params, h: spec_fn(JK) -> int| #[trigger] lsum(li, p, h) == left_join_delta(view.take(0), dbsub, wt(p, h)) by {
            assert(li =~= Seq::<Ct>::empty());
            assert(view.take(0) =~= Seq::<(NumRow, int)>::empty());
        }
    }
    while i < nd.len()
        invariant
            i <= nd.len(),
            view == num_delta_seq_view(nd@),
            ddm_ok(*dm, li),
            wbag_as(*subs, phi_tab(dbsub)),
            pows_ok(pows@),
            forall|j: int| 0 <= j < nd.len() ==> (#[trigger] nd[j]).sign == 1 || nd[j].sign == -1,
            forall|p: Params, h: spec_fn(JK) -> int| #[trigger] lsum(li, p, h) == left_join_delta(view.take(i as int), dbsub, wt(p, h)),
        decreases nd.len() - i,
    {
        let n = &nd[i].row;
        let sv = wbucket(subs, n);
        let li0 = Ghost(li);
        let Ghost(l2) = emit_nd(dm, n, nd[i].sign, &*sv, pows, li0);
        proof {
            let nrow = num_rt_view(*n);
            assert(view[i as int] == (nrow, nd[i as int].sign as int));
            assert(view.take(i as int + 1) =~= view.take(i as int).push(view[i as int]));
            assert forall|p: Params, h: spec_fn(JK) -> int| #[trigger] lsum(l2, p, h) == left_join_delta(view.take(i as int + 1), dbsub, wt(p, h)) by {
                lemma_n_term(*subs, dbsub, sv@, p, h, nrow);
                assert(lsum(li, p, h) == left_join_delta(view.take(i as int), dbsub, wt(p, h)));
                lemma_sum_push(view.take(i as int), view[i as int], |e: (NumRow, int)| e.1 * sum(dbsub, |o: SubRow| wt(p, h)(e.0, o)));
            }
            li = l2;
        }
        i += 1;
    }
    proof { assert(view.take(nd.len() as int) =~= view); }
    Ghost(li)
}

/// The nums bucketed by filing hold, for the filings of the changed sub rows, the sums of the table.
pub open spec fn nb_covers(nb: Index<Vec<NumRt>>, tab: Seq<NumRow>, sd: Seq<SubDelta>) -> bool {
    &&& nb.wf()
    &&& forall|i: int, f: spec_fn(NumRow) -> int| 0 <= i < sd.len() ==>
        #[trigger] sum(rows_at::<NumRt>(nb, key1(sd[i].row.adsh@)), f) == sum(tab, key_weight::<NumRt>(key1(sd[i].row.adsh@), f))
}

fn nbucket(nb: &Index<Vec<NumRt>>, s: &SubRt) -> (v: Arc<Vec<NumRt>>)
    ensures rows_v(v@) == rows_at::<NumRt>(*nb, key1(s.adsh@)),
{
    let k = make_key1(s.adsh.as_slice());
    match nb.get(&k) {
        Some(a) => a,
        None => Arc::new(Vec::new()),
    }
}

proof fn lemma_s_term(nr: Seq<NumRow>, tab: Seq<NumRow>, p: Params, h: spec_fn(JK) -> int, srow: SubRow, nb: Index<Vec<NumRt>>)
    requires
        nr == rows_at::<NumRt>(nb, key1(srow.adsh)),
        forall|f: spec_fn(NumRow) -> int| #[trigger] sum(rows_at::<NumRt>(nb, key1(srow.adsh)), f) == sum(tab, key_weight::<NumRt>(key1(srow.adsh), f)),
    ensures
        sum(nr, fnum(p, h, srow)) == sum(tab, |o: NumRow| wt(p, h)(o, srow)),
{
    let f = fnum(p, h, srow);
    let k = key1(srow.adsh);
    assert forall|o: NumRow| #[trigger] key_weight::<NumRt>(k, f)(o) == f(o) by {
        if f(o) != 0 { assert(m_s(o, srow)); assert(<NumRt as Keyed>::vkey(o) == Some(k)); }
    }
    lemma_sum_ext_all(tab, key_weight::<NumRt>(k, f), f);
    assert(sum(nr, f) == sum(tab, key_weight::<NumRt>(k, f)));
}

/// The batch's changed sub rows against the num table after the commit.
pub fn stage_s(dm: &mut Index<DCls>, sd: &Vec<SubDelta>, nb: &Index<Vec<NumRt>>, pows: &Vec<Wide>, Ghost(l0): Ghost<Seq<Ct>>, Ghost(tab): Ghost<Seq<NumRow>>) -> (r: Ghost<Seq<Ct>>)
    requires
        ddm_ok(*old(dm), l0),
        nb_covers(*nb, tab, sd@),
        pows_ok(pows@),
        forall|i: int| 0 <= i < sd.len() ==> (#[trigger] sd[i]).sign == 1 || sd[i].sign == -1,
    ensures
        ddm_ok(*final(dm), r@),
        forall|p: Params, h: spec_fn(JK) -> int| #[trigger] lsum(r@, p, h)
            == lsum(l0, p, h) + right_join_delta(tab, sub_delta_seq_view(sd@), wt(p, h)),
{
    let ghost view = sub_delta_seq_view(sd@);
    let ghost mut li = l0;
    let mut i: usize = 0;
    proof {
        assert forall|p: Params, h: spec_fn(JK) -> int| #[trigger] lsum(li, p, h) == lsum(l0, p, h) + right_join_delta(tab, view.take(0), wt(p, h)) by {
            assert(view.take(0) =~= Seq::<(SubRow, int)>::empty());
        }
    }
    while i < sd.len()
        invariant
            i <= sd.len(),
            view == sub_delta_seq_view(sd@),
            ddm_ok(*dm, li),
            nb_covers(*nb, tab, sd@),
            pows_ok(pows@),
            forall|j: int| 0 <= j < sd.len() ==> (#[trigger] sd[j]).sign == 1 || sd[j].sign == -1,
            forall|p: Params, h: spec_fn(JK) -> int| #[trigger] lsum(li, p, h)
                == lsum(l0, p, h) + right_join_delta(tab, view.take(i as int), wt(p, h)),
        decreases sd.len() - i,
    {
        let s = &sd[i].row;
        let nr = nbucket(nb, s);
        let Ghost(l2) = emit_sd(dm, s, sd[i].sign, &*nr, pows, Ghost(li));
        proof {
            let srow = sub_rt_view(*s);
            assert(view[i as int] == (srow, sd[i as int].sign as int));
            assert(view.take(i as int + 1) =~= view.take(i as int).push(view[i as int]));
            assert(sd[i as int].row.adsh@ == srow.adsh);
            assert forall|p: Params, h: spec_fn(JK) -> int| #[trigger] lsum(l2, p, h)
                == lsum(l0, p, h) + right_join_delta(tab, view.take(i as int + 1), wt(p, h)) by {
                assert forall|f: spec_fn(NumRow) -> int| #[trigger] sum(rows_at::<NumRt>(*nb, key1(srow.adsh)), f) == sum(tab, key_weight::<NumRt>(key1(srow.adsh), f)) by {
                    assert(sum(rows_at::<NumRt>(*nb, key1(sd[i as int].row.adsh@)), f) == sum(tab, key_weight::<NumRt>(key1(sd[i as int].row.adsh@), f)));
                }
                lemma_s_term(rows_v(nr@), tab, p, h, srow, *nb);
                assert(lsum(li, p, h) == lsum(l0, p, h) + right_join_delta(tab, view.take(i as int), wt(p, h)));
                lemma_sum_push(view.take(i as int), view[i as int], |e: (SubRow, int)| e.1 * sum(tab, |o: NumRow| wt(p, h)(o, e.0)));
            }
            li = l2;
        }
        i += 1;
    }
    proof { assert(view.take(sd.len() as int) =~= view); }
    Ghost(li)
}

/// The ledger of a commit is the difference of the joins.
pub proof fn lemma_ledger_from_deltas(l: Seq<Ct>, db: Db, db2: Db, nv: Seq<(NumRow, int)>, sv: Seq<(SubRow, int)>)
    requires
        bag_step(db.num, db2.num, nv),
        bag_step(db.sub, db2.sub, sv),
        forall|p: Params, h: spec_fn(JK) -> int| #[trigger] lsum(l, p, h)
            == left_join_delta(nv, db.sub, wt(p, h)) + right_join_delta(db2.num, sv, wt(p, h)),
    ensures ledger_ok(l, db, db2),
{
    assert forall|p: Params, h: spec_fn(JK) -> int| #[trigger] jq(db2, p, h) == jq(db, p, h) + lsum(l, p, h) by {
        lemma_join_bag_step(db.num, db2.num, nv, db.sub, db2.sub, sv, wt(p, h));
    }
}

}
