// A pass of signed num rows through the join: for each num row, its sub rows (by adsh), tag rows
// (by tag and version) and signed pre rows (by adsh, tag and version) give one signed record per
// combination.  The records' sum is the pass's sum over the join (algebra.rs, `passv`).
use vstd::prelude::*;
use std::sync::Arc;
use verus_common::bag::*;
use verus_common::bytes::*;
use verus_common::sums::*;
use verus_prooflib::ordered::{Index, Key, KeyView};
use verus_prooflib::wide::Wide;
use crate::algebra::*;
use crate::bucket::*;
use crate::f64bits::*;
use crate::gen_obligations::*;
use crate::gen_spec::*;
use crate::join_spec::*;
use crate::jspec::*;
use crate::keys::*;
use crate::numeric::*;
use crate::rec::*;
use crate::rows::*;

verus! {

/// Pre rows of a pass, bucketed by their join key, with a sign each.
pub struct Side {
    pub idx: Index<Vec<PreRt>>,
    pub b: i64,
    pub tab: Ghost<Seq<PreRow>>,
}

pub open spec fn sides_spec(sides: Seq<Side>) -> Sides {
    sides.map_values(|sd: Side| (sd.tab@, sd.b as int))
}

pub open spec fn sides_ok(sides: Seq<Side>) -> bool {
    forall|i: int| 0 <= i < sides.len() ==>
        bag_holds::<PreRt>((#[trigger] sides[i]).idx, sides[i].tab@) && (sides[i].b == 1 || sides[i].b == -1)
}

/// The rows of a side under one key.
pub struct SideRows {
    pub rows: Arc<Vec<PreRt>>,
    pub b: i64,
}

pub open spec fn bs_ok(bs: Seq<SideRows>) -> bool {
    forall|j: int| 0 <= j < bs.len() ==> (#[trigger] bs[j]).b == 1 || bs[j].b == -1
}

pub open spec fn rows_spec(bs: Seq<SideRows>) -> Sides {
    bs.map_values(|x: SideRows| (rows_v(x.rows@), x.b as int))
}

/// The join of a num row, a sub row and a tag row is possible for these rows.
pub open spec fn elig(n: NumRow, s: SubRow, t: TagRow) -> bool {
    &&& n.uom is Some && n.value is Some && n.adsh is Some && n.tag is Some && n.version is Some
    &&& s.sic is Some
    &&& t.abstract_ == Some(0int)
    &&& m_s(n, s)
    &&& m_t(n, t)
}

pub open spec fn hit_pre(n: NumRow, r: PreRow) -> bool {
    r.stmt is Some && r.adsh == n.adsh && r.tag == n.tag && r.version == n.version
}

pub open spec fn rec_of(c: i128, n: NumRow, s: SubRow, t: TagRow, r: PreRow, x: Rec) -> bool {
    &&& x.uom@ == n.uom->0
    &&& x.stmt@ == r.stmt->0
    &&& x.sic as int == s.sic->0
    &&& opt_bytes(x.tlabel) == t.tlabel
    &&& opt_int(x.cik) == s.cik
    &&& x.v@ == n.value->0
    &&& x.w == c
}

proof fn lemma_rterm(p: Params, h: spec_fn(JK) -> int, c: i128, n: NumRow, s: SubRow, t: TagRow, r: PreRow, x: Rec)
    requires elig(n, s, t), hit_pre(n, r), rec_of(c, n, s, t, r, x),
    ensures rterm(p, h, x) == c as int * psi(p, h, n, s, t, r),
{
    if wc(p, n, s, t, r) {
        assert(rec_in(p, x));
        assert(h(rec_jk(p, x)) == h(pj(tup(n, s, t, r))));
    } else {
        assert(!rec_in(p, x));
    }
}

proof fn lemma_psi_zero_pre(p: Params, h: spec_fn(JK) -> int, n: NumRow, s: SubRow, t: TagRow, r: PreRow)
    requires !hit_pre(n, r),
    ensures psi(p, h, n, s, t, r) == 0,
{
}

proof fn lemma_psi_zero_elig(p: Params, h: spec_fn(JK) -> int, n: NumRow, s: SubRow, t: TagRow, r: PreRow)
    requires !c_t(t) || !m_t(n, t),
    ensures psi(p, h, n, s, t, r) == 0,
{
}

proof fn lemma_rows_take<R: BagRow>(s: Seq<R>, i: int)
    requires 0 <= i < s.len(),
    ensures
        s.take(i + 1) =~= s.take(i).push(s[i]),
        rows_v(s.take(i + 1)) =~= rows_v(s.take(i)).push(s[i].rv()),
{
    assert(s.take(i + 1) =~= s.take(i).push(s[i]));
    assert(rows_v(s.take(i + 1)) =~= rows_v(s.take(i)).push(s[i].rv()));
}

proof fn lemma_mul_assoc(a: int, b: int, x: int)
    ensures (a * b) * x == a * (b * x),
{
    assert((a * b) * x == a * (b * x)) by (nonlinear_arith);
}

proof fn lemma_mul_zero(a: int, x: int)
    requires a == 0,
    ensures a * x == 0,
{
    assert(a * x == 0) by (nonlinear_arith) requires a == 0;
}

pub fn opt_of(x: i64, is_null: u8) -> (r: Option<i64>)
    ensures opt_int(r) == (if is_null != 0 { None } else { Some(x as int) }),
{
    if is_null != 0 { None } else { Some(x) }
}

/// The records of one num, sub and tag row with the pre rows of a bucket.
fn emit_pre(out: &mut Vec<Rec>, c: i128, n: &NumRt, vw: &Wide, s: &SubRt, t: &TagRt, rows: &Vec<PreRt>)
    requires
        elig(n.rv(), s.rv(), t.rv()),
        vw@ == n.rv().value->0,
    ensures
        forall|p: Params, h: spec_fn(JK) -> int| #[trigger] rsum(final(out)@, p, h)
            == rsum(old(out)@, p, h) + c as int * pre_sum(rows_v(rows@), p, h, n.rv(), s.rv(), t.rv()),
{
    let ghost start = out@;
    let ghost (nv, sv, tv) = (n.rv(), s.rv(), t.rv());
    let mut i: usize = 0;
    proof {
        assert forall|p: Params, h: spec_fn(JK) -> int| #[trigger] rsum(out@, p, h)
            == rsum(start, p, h) + c as int * pre_sum(rows_v(rows@.take(0)), p, h, nv, sv, tv) by {
            assert(rows_v(rows@.take(0)) =~= Seq::<PreRow>::empty());
            assert(pre_sum(rows_v(rows@.take(0)), p, h, nv, sv, tv) == 0);
        }
    }
    while i < rows.len()
        invariant
            i <= rows.len(),
            nv == n.rv(), sv == s.rv(), tv == t.rv(),
            elig(nv, sv, tv),
            vw@ == nv.value->0,
            forall|p: Params, h: spec_fn(JK) -> int| #[trigger] rsum(out@, p, h)
                == rsum(start, p, h) + c as int * pre_sum(rows_v(rows@.take(i as int)), p, h, nv, sv, tv),
        decreases rows.len() - i,
    {
        let r = &rows[i];
        let hit = r.stmt__null == 0
            && same_text(&r.adsh, r.adsh__null, &n.adsh, n.adsh__null)
            && same_text(&r.tag, r.tag__null, &n.tag, n.tag__null)
            && same_text(&r.version, r.version__null, &n.version, n.version__null);
        proof {
            assert(hit == hit_pre(nv, r.rv()));
            lemma_rows_take(rows@, i as int);
        }
        let ghost before = out@;
        if hit {
            let x = Rec {
                uom: copy_bytes(n.uom.as_slice()),
                stmt: copy_bytes(r.stmt.as_slice()),
                sic: s.sic,
                tlabel: if t.tlabel__null != 0 { None } else { Some(copy_bytes(t.tlabel.as_slice())) },
                cik: opt_of(s.cik, s.cik__null),
                v: vw.dup(),
                w: c,
            };
            proof {
                assert(rec_of(c, nv, sv, tv, r.rv(), x));
                assert forall|p: Params, h: spec_fn(JK) -> int| #[trigger] rsum(before.push(x), p, h)
                    == rsum(start, p, h) + c as int * pre_sum(rows_v(rows@.take(i as int + 1)), p, h, nv, sv, tv) by {
                    lemma_rsum_push(before, x, p, h);
                    lemma_rterm(p, h, c, nv, sv, tv, r.rv(), x);
                    lemma_sum_push(rows_v(rows@.take(i as int)), r.rv(), |q: PreRow| psi(p, h, nv, sv, tv, q));
                    assert(c as int * (pre_sum(rows_v(rows@.take(i as int)), p, h, nv, sv, tv) + psi(p, h, nv, sv, tv, r.rv()))
                        == c as int * pre_sum(rows_v(rows@.take(i as int)), p, h, nv, sv, tv) + c as int * psi(p, h, nv, sv, tv, r.rv())) by (nonlinear_arith);
                }
            }
            out.push(x);
        } else {
            proof {
                assert forall|p: Params, h: spec_fn(JK) -> int| #[trigger] rsum(before, p, h)
                    == rsum(start, p, h) + c as int * pre_sum(rows_v(rows@.take(i as int + 1)), p, h, nv, sv, tv) by {
                    lemma_psi_zero_pre(p, h, nv, sv, tv, r.rv());
                    lemma_sum_push(rows_v(rows@.take(i as int)), r.rv(), |q: PreRow| psi(p, h, nv, sv, tv, q));
                }
            }
        }
        i += 1;
    }
    proof { assert(rows@.take(rows.len() as int) =~= rows@); }
}

proof fn lemma_sides_push(sd: Seq<SideRows>, j: int)
    requires 0 <= j < sd.len(),
    ensures rows_spec(sd.take(j + 1)) =~= rows_spec(sd.take(j)).push((rows_v(sd[j].rows@), sd[j].b as int)),
{
    assert(rows_spec(sd.take(j + 1)) =~= rows_spec(sd.take(j)).push((rows_v(sd[j].rows@), sd[j].b as int)));
}

/// The records of one num, sub and tag row with all sides.
fn emit_sides(out: &mut Vec<Rec>, c: i128, n: &NumRt, vw: &Wide, s: &SubRt, t: &TagRt, bs: &Vec<SideRows>)
    requires
        -1 <= c <= 1,
        bs_ok(bs@),
        elig(n.rv(), s.rv(), t.rv()),
        vw@ == n.rv().value->0,
    ensures
        forall|p: Params, h: spec_fn(JK) -> int| #[trigger] rsum(final(out)@, p, h)
            == rsum(old(out)@, p, h) + c as int * psum(rows_spec(bs@), p, h, n.rv(), s.rv(), t.rv()),
{
    let ghost start = out@;
    let ghost (nv, sv, tv) = (n.rv(), s.rv(), t.rv());
    let mut j: usize = 0;
    proof {
        assert forall|p: Params, h: spec_fn(JK) -> int| #[trigger] rsum(out@, p, h)
            == rsum(start, p, h) + c as int * psum(rows_spec(bs@.take(0)), p, h, nv, sv, tv) by {
            assert(rows_spec(bs@.take(0)) =~= Seq::<(Seq<PreRow>, int)>::empty());
            assert(psum(rows_spec(bs@.take(0)), p, h, nv, sv, tv) == 0);
        }
    }
    while j < bs.len()
        invariant
            j <= bs.len(),
            nv == n.rv(), sv == s.rv(), tv == t.rv(),
            -1 <= c <= 1,
            bs_ok(bs@),
            elig(nv, sv, tv),
            vw@ == nv.value->0,
            forall|p: Params, h: spec_fn(JK) -> int| #[trigger] rsum(out@, p, h)
                == rsum(start, p, h) + c as int * psum(rows_spec(bs@.take(j as int)), p, h, nv, sv, tv),
        decreases bs.len() - j,
    {
        proof { assert(bs@[j as int].b == 1 || bs@[j as int].b == -1); }
        let bj_exec: i64 = bs[j].b;
        assert(bj_exec == 1 || bj_exec == -1);
        let cb: i128 = if bj_exec == 1 { c } else { -c };
        let ghost before = out@;
        emit_pre(out, cb, n, vw, s, t, &*bs[j].rows);
        proof {
            lemma_sides_push(bs@, j as int);
            assert forall|p: Params, h: spec_fn(JK) -> int| #[trigger] rsum(out@, p, h)
                == rsum(start, p, h) + c as int * psum(rows_spec(bs@.take(j as int + 1)), p, h, nv, sv, tv) by {
                let sdf = |x: (Seq<PreRow>, int)| x.1 * pre_sum(x.0, p, h, nv, sv, tv);
                let prefix = rows_spec(bs@.take(j as int));
                lemma_sum_push(prefix, (rows_v(bs@[j as int].rows@), bs@[j as int].b as int), sdf);
                let pj_ = pre_sum(rows_v(bs@[j as int].rows@), p, h, nv, sv, tv);
                let bj = bs@[j as int].b as int;
                assert(cb as int == c as int * bj);
                assert(cb as int * pj_ == c as int * (bj * pj_)) by (nonlinear_arith) requires cb as int == c as int * bj;
                assert(c as int * (psum(prefix, p, h, nv, sv, tv) + bj * pj_)
                    == c as int * psum(prefix, p, h, nv, sv, tv) + c as int * (bj * pj_)) by (nonlinear_arith);
            }
        }
        j += 1;
    }
    proof { assert(bs@.take(bs.len() as int) =~= bs@); }
}

/// The records of one num and sub row with the tag rows of a bucket.
fn emit_tags(out: &mut Vec<Rec>, c: i128, n: &NumRt, vw: &Wide, s: &SubRt, tags: &Vec<TagRt>, bs: &Vec<SideRows>)
    requires
        -1 <= c <= 1,
        bs_ok(bs@),
        n.rv().uom is Some && n.rv().value is Some && n.rv().adsh is Some && n.rv().tag is Some && n.rv().version is Some,
        s.rv().sic is Some,
        m_s(n.rv(), s.rv()),
        vw@ == n.rv().value->0,
    ensures
        forall|p: Params, h: spec_fn(JK) -> int| #[trigger] rsum(final(out)@, p, h)
            == rsum(old(out)@, p, h) + c as int * sum(rows_v(tags@), |t: TagRow| psum(rows_spec(bs@), p, h, n.rv(), s.rv(), t)),
{
    let ghost start = out@;
    let ghost (nv, sv) = (n.rv(), s.rv());
    let mut i: usize = 0;
    proof {
        assert forall|p: Params, h: spec_fn(JK) -> int| #[trigger] rsum(out@, p, h)
            == rsum(start, p, h) + c as int * sum(rows_v(tags@.take(0)), |t: TagRow| psum(rows_spec(bs@), p, h, nv, sv, t)) by {
            assert(rows_v(tags@.take(0)) =~= Seq::<TagRow>::empty());
        }
    }
    while i < tags.len()
        invariant
            i <= tags.len(),
            nv == n.rv(), sv == s.rv(),
            -1 <= c <= 1,
            bs_ok(bs@),
            nv.uom is Some && nv.value is Some && nv.adsh is Some && nv.tag is Some && nv.version is Some,
            sv.sic is Some,
            m_s(nv, sv),
            vw@ == nv.value->0,
            forall|p: Params, h: spec_fn(JK) -> int| #[trigger] rsum(out@, p, h)
                == rsum(start, p, h) + c as int * sum(rows_v(tags@.take(i as int)), |t: TagRow| psum(rows_spec(bs@), p, h, nv, sv, t)),
        decreases tags.len() - i,
    {
        let t = &tags[i];
        let hit = t.abstract___null == 0 && t.abstract_ == 0
            && bytes_eq_exec(t.tag.as_slice(), n.tag.as_slice())
            && bytes_eq_exec(t.version.as_slice(), n.version.as_slice());
        proof {
            assert(hit == (c_t(t.rv()) && m_t(nv, t.rv())));
            lemma_rows_take(tags@, i as int);
        }
        let ghost before = out@;
        if hit {
            emit_sides(out, c, n, vw, s, t, bs);
        }
        proof {
            assert forall|p: Params, h: spec_fn(JK) -> int| #[trigger] rsum(out@, p, h)
                == rsum(start, p, h) + c as int * sum(rows_v(tags@.take(i as int + 1)), |t2: TagRow| psum(rows_spec(bs@), p, h, nv, sv, t2)) by {
                let f = |t2: TagRow| psum(rows_spec(bs@), p, h, nv, sv, t2);
                lemma_sum_push(rows_v(tags@.take(i as int)), t.rv(), f);
                if hit {
                    assert(elig(nv, sv, t.rv()));
                    assert(c as int * (sum(rows_v(tags@.take(i as int)), f) + f(t.rv()))
                        == c as int * sum(rows_v(tags@.take(i as int)), f) + c as int * f(t.rv())) by (nonlinear_arith);
                } else {
                    assert(out@ =~= before);
                    assert forall|r: PreRow| #[trigger] psi(p, h, nv, sv, t.rv(), r) == 0 by {
                        lemma_psi_zero_elig(p, h, nv, sv, t.rv(), r);
                    }
                    assert(f(t.rv()) == 0) by {
                        let sd = |x: (Seq<PreRow>, int)| x.1 * pre_sum(x.0, p, h, nv, sv, t.rv());
                        assert forall|x: (Seq<PreRow>, int)| #[trigger] sd(x) == 0 by {
                            lemma_sum_all_zero_pre(x.0, p, h, nv, sv, t.rv());
                            lemma_mul_zero(0, 0);
                        }
                        lemma_sum_zero(rows_spec(bs@), sd);
                    }
                    assert(c as int * (sum(rows_v(tags@.take(i as int)), f) + 0)
                        == c as int * sum(rows_v(tags@.take(i as int)), f)) by (nonlinear_arith);
                }
            }
        }
        i += 1;
    }
    proof { assert(tags@.take(tags.len() as int) =~= tags@); }
}

proof fn lemma_sum_all_zero_pre(rows: Seq<PreRow>, p: Params, h: spec_fn(JK) -> int, n: NumRow, s: SubRow, t: TagRow)
    requires forall|r: PreRow| #[trigger] psi(p, h, n, s, t, r) == 0,
    ensures pre_sum(rows, p, h, n, s, t) == 0,
{
    lemma_sum_zero(rows, |r: PreRow| psi(p, h, n, s, t, r));
}


proof fn lemma_psi_zero_sub(p: Params, h: spec_fn(JK) -> int, n: NumRow, s: SubRow, t: TagRow, r: PreRow)
    requires s.sic is None || !m_s(n, s),
    ensures psi(p, h, n, s, t, r) == 0,
{
}

proof fn lemma_psi_zero_num(p: Params, h: spec_fn(JK) -> int, n: NumRow, s: SubRow, t: TagRow, r: PreRow)
    requires !(n.uom is Some && n.value is Some && n.adsh is Some && n.tag is Some && n.version is Some),
    ensures psi(p, h, n, s, t, r) == 0,
{
}

/// Nothing joins when the pre rows or the tag rows or the sub rows contribute nothing.
proof fn lemma_big_psi_zero(sub: Seq<SubRow>, tag: Seq<TagRow>, sides: Sides, p: Params, h: spec_fn(JK) -> int, n: NumRow)
    requires forall|s: SubRow, t: TagRow, r: PreRow| #[trigger] psi(p, h, n, s, t, r) == 0,
    ensures big_psi(sub, tag, sides, p, h, n) == 0,
{
    assert forall|s: SubRow| #[trigger] tsum(tag, sides, p, h, n, s) == 0 by {
        assert forall|t: TagRow| #[trigger] psum(sides, p, h, n, s, t) == 0 by {
            lemma_psum_zero(sides, p, h, n, s, t);
        }
        lemma_sum_all_zero_gen(tag, |t: TagRow| psum(sides, p, h, n, s, t));
    }
    lemma_sum_all_zero_gen(sub, |s: SubRow| tsum(tag, sides, p, h, n, s));
}

proof fn lemma_sum_all_zero_gen<A>(s: Seq<A>, f: spec_fn(A) -> int)
    requires forall|x: A| #[trigger] f(x) == 0,
    ensures sum(s, f) == 0,
{
    lemma_sum_zero(s, f);
}

proof fn lemma_psum_zero(sides: Sides, p: Params, h: spec_fn(JK) -> int, n: NumRow, s: SubRow, t: TagRow)
    requires forall|r: PreRow| #[trigger] psi(p, h, n, s, t, r) == 0,
    ensures psum(sides, p, h, n, s, t) == 0,
{
    let sd = |x: (Seq<PreRow>, int)| x.1 * pre_sum(x.0, p, h, n, s, t);
    assert forall|x: (Seq<PreRow>, int)| #[trigger] sd(x) == 0 by {
        lemma_sum_all_zero_pre(x.0, p, h, n, s, t);
        lemma_mul_zero_r(x.1, pre_sum(x.0, p, h, n, s, t));
    }
    lemma_sum_zero(sides, sd);
}

proof fn lemma_mul_zero_r(a: int, x: int)
    requires x == 0,
    ensures a * x == 0,
{
    assert(a * x == 0) by (nonlinear_arith) requires x == 0;
}

/// The records of one num row with the sub rows of a bucket.
fn emit_subs(out: &mut Vec<Rec>, c: i128, n: &NumRt, vw: &Wide, subs: &Vec<SubRt>, tags: &Vec<TagRt>, bs: &Vec<SideRows>)
    requires
        -1 <= c <= 1,
        bs_ok(bs@),
        n.rv().uom is Some && n.rv().value is Some && n.rv().adsh is Some && n.rv().tag is Some && n.rv().version is Some,
        vw@ == n.rv().value->0,
    ensures
        forall|p: Params, h: spec_fn(JK) -> int| #[trigger] rsum(final(out)@, p, h)
            == rsum(old(out)@, p, h) + c as int * sum(rows_v(subs@), |s: SubRow|
                sum(rows_v(tags@), |t: TagRow| psum(rows_spec(bs@), p, h, n.rv(), s, t))),
{
    let ghost start = out@;
    let ghost nv = n.rv();
    let mut i: usize = 0;
    proof {
        assert forall|p: Params, h: spec_fn(JK) -> int| #[trigger] rsum(out@, p, h)
            == rsum(start, p, h) + c as int * sum(rows_v(subs@.take(0)), |s: SubRow|
                sum(rows_v(tags@), |t: TagRow| psum(rows_spec(bs@), p, h, nv, s, t))) by {
            assert(rows_v(subs@.take(0)) =~= Seq::<SubRow>::empty());
        }
    }
    while i < subs.len()
        invariant
            i <= subs.len(),
            nv == n.rv(),
            -1 <= c <= 1,
            bs_ok(bs@),
            nv.uom is Some && nv.value is Some && nv.adsh is Some && nv.tag is Some && nv.version is Some,
            vw@ == nv.value->0,
            forall|p: Params, h: spec_fn(JK) -> int| #[trigger] rsum(out@, p, h)
                == rsum(start, p, h) + c as int * sum(rows_v(subs@.take(i as int)), |s: SubRow|
                    sum(rows_v(tags@), |t: TagRow| psum(rows_spec(bs@), p, h, nv, s, t))),
        decreases subs.len() - i,
    {
        let s = &subs[i];
        let hit = s.sic__null == 0 && bytes_eq_exec(s.adsh.as_slice(), n.adsh.as_slice());
        proof {
            assert(hit == (s.rv().sic is Some && m_s(nv, s.rv())));
            lemma_rows_take(subs@, i as int);
        }
        let ghost before = out@;
        if hit {
            emit_tags(out, c, n, vw, s, tags, bs);
        }
        proof {
            assert forall|p: Params, h: spec_fn(JK) -> int| #[trigger] rsum(out@, p, h)
                == rsum(start, p, h) + c as int * sum(rows_v(subs@.take(i as int + 1)), |s2: SubRow|
                    sum(rows_v(tags@), |t: TagRow| psum(rows_spec(bs@), p, h, nv, s2, t))) by {
                let f = |s2: SubRow| sum(rows_v(tags@), |t: TagRow| psum(rows_spec(bs@), p, h, nv, s2, t));
                lemma_sum_push(rows_v(subs@.take(i as int)), s.rv(), f);
                if hit {
                    assert(c as int * (sum(rows_v(subs@.take(i as int)), f) + f(s.rv()))
                        == c as int * sum(rows_v(subs@.take(i as int)), f) + c as int * f(s.rv())) by (nonlinear_arith);
                } else {
                    assert(out@ =~= before);
                    assert forall|t: TagRow| #[trigger] psum(rows_spec(bs@), p, h, nv, s.rv(), t) == 0 by {
                        assert forall|r: PreRow| #[trigger] psi(p, h, nv, s.rv(), t, r) == 0 by {
                            lemma_psi_zero_sub(p, h, nv, s.rv(), t, r);
                        }
                        lemma_psum_zero(rows_spec(bs@), p, h, nv, s.rv(), t);
                    }
                    lemma_sum_all_zero_gen(rows_v(tags@), |t: TagRow| psum(rows_spec(bs@), p, h, nv, s.rv(), t));
                    assert(f(s.rv()) == 0);
                    assert(c as int * (sum(rows_v(subs@.take(i as int)), f) + 0)
                        == c as int * sum(rows_v(subs@.take(i as int)), f)) by (nonlinear_arith);
                }
            }
        }
        i += 1;
    }
    proof { assert(subs@.take(subs.len() as int) =~= subs@); }
}


fn bucket_of<R: Keyed>(b: &Index<Vec<R>>, k: &Key) -> (v: Arc<Vec<R>>)
    ensures rows_v(v@) == rows_at::<R>(*b, k.view()),
{
    match b.get(k) {
        Some(a) => a,
        None => Arc::new(Vec::new()),
    }
}

proof fn lemma_sum_pointwise<A>(a: Seq<A>, b: Seq<A>, f: spec_fn(A) -> int)
    requires a.len() == b.len(), forall|j: int| 0 <= j < a.len() ==> f(#[trigger] a[j]) == f(b[j]),
    ensures sum(a, f) == sum(b, f),
    decreases a.len(),
{
    if a.len() > 0 {
        let (a0, b0) = (a.drop_last(), b.drop_last());
        assert forall|j: int| 0 <= j < a0.len() implies f(#[trigger] a0[j]) == f(b0[j]) by {
            assert(a0[j] == a[j] && b0[j] == b[j]);
        }
        lemma_sum_pointwise(a0, b0, f);
        assert(a.last() == a[a.len() - 1] && b.last() == b[b.len() - 1]);
    }
}

/// The sides' pre rows under the key of a num row, in place of the whole sides.
proof fn lemma_sides_bucket(sides: Seq<Side>, bs: Seq<SideRows>, kp: KeyView, p: Params, h: spec_fn(JK) -> int, n: NumRow, s: SubRow, t: TagRow)
    requires
        sides_ok(sides),
        bs.len() == sides.len(),
        forall|j: int| 0 <= j < bs.len() ==> rows_v((#[trigger] bs[j]).rows@) == rows_at::<PreRt>(sides[j].idx, kp) && bs[j].b == sides[j].b,
        n.adsh is Some && n.tag is Some && n.version is Some,
        kp == key3(n.adsh->0, n.tag->0, n.version->0),
    ensures
        psum(sides_spec(sides), p, h, n, s, t) == psum(rows_spec(bs), p, h, n, s, t),
{
    let sd = |x: (Seq<PreRow>, int)| x.1 * pre_sum(x.0, p, h, n, s, t);
    assert(sides_spec(sides).len() == rows_spec(bs).len());
    assert forall|j: int| 0 <= j < sides_spec(sides).len() implies sd(#[trigger] sides_spec(sides)[j]) == sd(rows_spec(bs)[j]) by {
        let f = |r: PreRow| psi(p, h, n, s, t, r);
        assert forall|r: PreRow| #[trigger] f(r) != 0 implies <PreRt as Keyed>::vkey(r) == Some(kp) by {
            assert(wc(p, n, s, t, r));
        }
        lemma_bucket_sum::<PreRt>(sides[j].idx, sides[j].tab@, kp, f);
    }
    lemma_sum_pointwise(sides_spec(sides), rows_spec(bs), sd);
}

/// A num row's join over the tables is its join over the buckets of its keys.
proof fn lemma_bridge(st: Seq<SubRow>, tt: Seq<TagRow>, sub_bag: Index<Vec<SubRt>>, tag_bag: Index<Vec<TagRt>>,
    sides: Seq<Side>, bs: Seq<SideRows>, sv: Seq<SubRow>, tv: Seq<TagRow>,
    ks: KeyView, kt: KeyView, kp: KeyView, p: Params, h: spec_fn(JK) -> int, n: NumRow)
    requires
        bag_holds::<SubRt>(sub_bag, st),
        bag_holds::<TagRt>(tag_bag, tt),
        sides_ok(sides),
        bs.len() == sides.len(),
        forall|j: int| 0 <= j < bs.len() ==> rows_v((#[trigger] bs[j]).rows@) == rows_at::<PreRt>(sides[j].idx, kp) && bs[j].b == sides[j].b,
        sv == rows_at::<SubRt>(sub_bag, ks),
        tv == rows_at::<TagRt>(tag_bag, kt),
        n.adsh is Some && n.tag is Some && n.version is Some,
        ks == key1(n.adsh->0), kt == key2(n.tag->0, n.version->0), kp == key3(n.adsh->0, n.tag->0, n.version->0),
    ensures
        big_psi(st, tt, sides_spec(sides), p, h, n)
            == sum(sv, |s: SubRow| sum(tv, |t: TagRow| psum(rows_spec(bs), p, h, n, s, t))),
{
    let ss = sides_spec(sides);
    let rs = rows_spec(bs);
    // per (sub, tag): the whole sides or their buckets
    assert forall|s: SubRow, t: TagRow| #[trigger] psum(ss, p, h, n, s, t) == psum(rs, p, h, n, s, t) by {
        lemma_sides_bucket(sides, bs, kp, p, h, n, s, t);
    }
    // per sub: the tag table or its bucket
    assert forall|s: SubRow| #[trigger] tsum(tt, ss, p, h, n, s) == sum(tv, |t: TagRow| psum(rs, p, h, n, s, t)) by {
        let y = |t: TagRow| psum(rs, p, h, n, s, t);
        lemma_sum_ext_all(tt, |t: TagRow| psum(ss, p, h, n, s, t), y);
        assert forall|t: TagRow| #[trigger] y(t) != 0 implies <TagRt as Keyed>::vkey(t) == Some(kt) by {
            if !(c_t(t) && m_t(n, t)) {
                assert forall|r: PreRow| #[trigger] psi(p, h, n, s, t, r) == 0 by { lemma_psi_zero_elig(p, h, n, s, t, r); }
                lemma_psum_zero(rs, p, h, n, s, t);
            }
        }
        lemma_bucket_sum::<TagRt>(tag_bag, tt, kt, y);
    }
    let z = |s: SubRow| sum(tv, |t: TagRow| psum(rs, p, h, n, s, t));
    lemma_sum_ext_all(st, |s: SubRow| tsum(tt, ss, p, h, n, s), z);
    assert forall|s: SubRow| #[trigger] z(s) != 0 implies <SubRt as Keyed>::vkey(s) == Some(ks) by {
        if !(s.sic is Some && m_s(n, s)) {
            assert forall|t: TagRow| #[trigger] psum(rs, p, h, n, s, t) == 0 by {
                assert forall|r: PreRow| #[trigger] psi(p, h, n, s, t, r) == 0 by { lemma_psi_zero_sub(p, h, n, s, t, r); }
                lemma_psum_zero(rs, p, h, n, s, t);
            }
            lemma_sum_all_zero_gen(tv, |t: TagRow| psum(rs, p, h, n, s, t));
        }
    }
    lemma_bucket_sum::<SubRt>(sub_bag, st, ks, z);
}

proof fn lemma_zero_bs(p: Params, h: spec_fn(JK) -> int, n: NumRow, st: Seq<SubRow>, tt: Seq<TagRow>, sides: Sides)
    requires !(n.uom is Some && n.value is Some && n.adsh is Some && n.tag is Some && n.version is Some),
    ensures big_psi(st, tt, sides, p, h, n) == 0,
{
    assert forall|s: SubRow, t: TagRow, r: PreRow| #[trigger] psi(p, h, n, s, t, r) == 0 by {
        lemma_psi_zero_num(p, h, n, s, t, r);
    }
    lemma_big_psi_zero(st, tt, sides, p, h, n);
}

/// The records of one num row, against the sub and tag tables and the sides, times c.
pub fn emit_num(out: &mut Vec<Rec>, c: i128, n: &NumRt, sub_bag: &Index<Vec<SubRt>>, tag_bag: &Index<Vec<TagRt>>,
    sides: &Vec<Side>, pows: &Vec<Wide>, Ghost(st): Ghost<Seq<SubRow>>, Ghost(tt): Ghost<Seq<TagRow>>)
    requires
        -1 <= c <= 1,
        bag_holds::<SubRt>(*sub_bag, st),
        bag_holds::<TagRt>(*tag_bag, tt),
        sides_ok(sides@),
        pows_ok(pows@),
    ensures
        forall|p: Params, h: spec_fn(JK) -> int| #[trigger] rsum(final(out)@, p, h)
            == rsum(old(out)@, p, h) + c as int * big_psi(st, tt, sides_spec(sides@), p, h, n.rv()),
{
    let ghost nv = n.rv();
    let cn = n.uom__null == 0 && n.value__null == 0 && n.adsh__null == 0 && n.tag__null == 0 && n.version__null == 0;
    if !cn {
        proof {
            assert forall|p: Params, h: spec_fn(JK) -> int| #[trigger] big_psi(st, tt, sides_spec(sides@), p, h, nv) == 0 by {
                lemma_zero_bs(p, h, nv, st, tt, sides_spec(sides@));
            }
        }
        return;
    }
    let vw = wide_of_bits(n.value, pows);
    let ks = make_key1(n.adsh.as_slice());
    let kt = make_key2(n.tag.as_slice(), n.version.as_slice());
    let kp = make_key3(n.adsh.as_slice(), n.tag.as_slice(), n.version.as_slice());
    let sv = bucket_of::<SubRt>(sub_bag, &ks);
    let tv = bucket_of::<TagRt>(tag_bag, &kt);
    let mut bs: Vec<SideRows> = Vec::new();
    let mut j: usize = 0;
    while j < sides.len()
        invariant
            j <= sides.len(),
            sides_ok(sides@),
            bs.len() == j,
            forall|q: int| 0 <= q < bs.len() ==> rows_v((#[trigger] bs[q]).rows@) == rows_at::<PreRt>(sides[q].idx, kp.view()) && bs[q].b == sides[q].b,
            bs_ok(bs@),
        decreases sides.len() - j,
    {
        proof { assert(sides[j as int].b == 1 || sides[j as int].b == -1); }
        let rows = bucket_of::<PreRt>(&sides[j].idx, &kp);
        bs.push(SideRows { rows, b: sides[j].b });
        j += 1;
    }
    emit_subs(out, c, n, &vw, &*sv, &*tv, &bs);
    proof {
        assert forall|p: Params, h: spec_fn(JK) -> int| #[trigger] big_psi(st, tt, sides_spec(sides@), p, h, nv)
            == sum(rows_v(sv@), |s: SubRow| sum(rows_v(tv@), |t: TagRow| psum(rows_spec(bs@), p, h, nv, s, t))) by {
            lemma_bridge(st, tt, *sub_bag, *tag_bag, sides@, bs@, rows_v(sv@), rows_v(tv@), ks.view(), kt.view(), kp.view(), p, h, nv);
        }
    }
}


pub open spec fn nl_view(nl: Seq<(NumRt, i64)>) -> Seq<(NumRow, int)> {
    nl.map_values(|x: (NumRt, i64)| (x.0.rv(), x.1 as int))
}

pub open spec fn nl_ok(nl: Seq<(NumRt, i64)>) -> bool {
    forall|i: int| 0 <= i < nl.len() ==> (#[trigger] nl[i]).1 == 1 || nl[i].1 == -1
}

/// The records of a pass of signed num rows, times mult.
pub fn emit_pass(out: &mut Vec<Rec>, nl: &Vec<(NumRt, i64)>, mult: i64, sub_bag: &Index<Vec<SubRt>>, tag_bag: &Index<Vec<TagRt>>,
    sides: &Vec<Side>, pows: &Vec<Wide>, Ghost(st): Ghost<Seq<SubRow>>, Ghost(tt): Ghost<Seq<TagRow>>)
    requires
        -1 <= mult <= 1,
        bag_holds::<SubRt>(*sub_bag, st),
        bag_holds::<TagRt>(*tag_bag, tt),
        sides_ok(sides@),
        pows_ok(pows@),
        nl_ok(nl@),
    ensures
        forall|p: Params, h: spec_fn(JK) -> int| #[trigger] rsum(final(out)@, p, h)
            == rsum(old(out)@, p, h) + mult as int * passv(nl_view(nl@), st, tt, sides_spec(sides@), p, h),
{
    let ghost start = out@;
    let mut i: usize = 0;
    proof {
        assert forall|p: Params, h: spec_fn(JK) -> int| #[trigger] rsum(out@, p, h)
            == rsum(start, p, h) + mult as int * passv(nl_view(nl@.take(0)), st, tt, sides_spec(sides@), p, h) by {
            assert(nl_view(nl@.take(0)) =~= Seq::<(NumRow, int)>::empty());
        }
    }
    while i < nl.len()
        invariant
            i <= nl.len(),
            -1 <= mult <= 1,
            bag_holds::<SubRt>(*sub_bag, st),
            bag_holds::<TagRt>(*tag_bag, tt),
            sides_ok(sides@),
            pows_ok(pows@),
            nl_ok(nl@),
            forall|p: Params, h: spec_fn(JK) -> int| #[trigger] rsum(out@, p, h)
                == rsum(start, p, h) + mult as int * passv(nl_view(nl@.take(i as int)), st, tt, sides_spec(sides@), p, h),
        decreases nl.len() - i,
    {
        proof { assert(nl@[i as int].1 == 1 || nl@[i as int].1 == -1); }
        let a = nl[i].1;
        let c: i128 = if a == 1 { mult as i128 } else { -(mult as i128) };
        emit_num(out, c, &nl[i].0, sub_bag, tag_bag, sides, pows, Ghost(st), Ghost(tt));
        proof {
            assert(nl@.take(i as int + 1) =~= nl@.take(i as int).push(nl@[i as int]));
            assert(nl_view(nl@.take(i as int + 1)) =~= nl_view(nl@.take(i as int)).push((nl@[i as int].0.rv(), a as int)));
            assert forall|p: Params, h: spec_fn(JK) -> int| #[trigger] rsum(out@, p, h)
                == rsum(start, p, h) + mult as int * passv(nl_view(nl@.take(i as int + 1)), st, tt, sides_spec(sides@), p, h) by {
                let g = |x: (NumRow, int)| x.1 * big_psi(st, tt, sides_spec(sides@), p, h, x.0);
                let prefix = nl_view(nl@.take(i as int));
                lemma_sum_push(prefix, (nl@[i as int].0.rv(), a as int), g);
                let bp = big_psi(st, tt, sides_spec(sides@), p, h, nl@[i as int].0.rv());
                assert(c as int == mult as int * a as int);
                assert(c as int * bp == mult as int * (a as int * bp)) by (nonlinear_arith) requires c as int == mult as int * a as int;
                assert(mult as int * (passv(prefix, st, tt, sides_spec(sides@), p, h) + a as int * bp)
                    == mult as int * passv(prefix, st, tt, sides_spec(sides@), p, h) + mult as int * (a as int * bp)) by (nonlinear_arith);
            }
        }
        i += 1;
    }
    proof { assert(nl@.take(nl.len() as int) =~= nl@); }
}

}
