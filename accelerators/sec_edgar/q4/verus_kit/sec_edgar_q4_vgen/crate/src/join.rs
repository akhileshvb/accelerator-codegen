// The join: one record per (num, sub, tag) combination of a parameter set, weighted by the number of
// matching pre rows.  Summing a group aggregate over the records is summing it over the query's tuples.
use vstd::prelude::*;
use std::sync::Arc;
use verus_common::bag::*;
use verus_common::bytes::*;
use verus_common::sums::*;
use verus_prooflib::ordered::{Index, Key, KeyView};
use verus_prooflib::wide::Wide;
use crate::bucket::*;
use crate::f64bits::*;
use crate::gen_obligations::*;
use crate::gen_spec::*;
use crate::join_spec::*;
use crate::keys::*;
use crate::numeric::*;
use crate::rows::*;

verus! {

pub struct Rec {
    pub sic: Option<i64>,
    pub tlabel: Option<Vec<u8>>,
    pub cik: Option<i64>,
    pub v: Wide,
    pub w: i128,
}

pub open spec fn all_nonneg(l: Seq<Rec>) -> bool {
    forall|i: int| 0 <= i < l.len() ==> (#[trigger] l[i]).w >= 0
}

pub open spec fn opt_int(x: Option<i64>) -> Option<int> {
    match x { Some(a) => Some(a as int), None => None }
}

pub open spec fn opt_bytes(x: Option<Vec<u8>>) -> Option<Seq<u8>> {
    match x { Some(a) => Some(a@), None => None }
}

pub open spec fn rec_jk(p: Params, r: Rec) -> JK {
    JK { sic: opt_int(r.sic), tlabel: opt_bytes(r.tlabel), cik: opt_int(r.cik), v: r.v@, vs: true, stmt: Some(p.p3) }
}

pub open spec fn recsum(l: Seq<Rec>, p: Params, h: spec_fn(JK) -> int) -> int {
    sum(l, |r: Rec| r.w as int * h(rec_jk(p, r)))
}

pub open spec fn tag_sum(tags: Seq<TagRow>, p: Params, n: NumRow, s: SubRow, w: int, h: spec_fn(JK) -> int) -> int {
    sum(tags, |t: TagRow| if c_t(t) && m_t(n, t) { w * h(jk(p, n, s, t)) } else { 0 })
}

pub open spec fn sub_sum(subs: Seq<SubRow>, p: Params, tags: Seq<TagRow>, n: NumRow, w: int, h: spec_fn(JK) -> int) -> int {
    sum(subs, |s: SubRow| if c_s(p, s) && m_s(n, s) { tag_sum(tags, p, n, s, w, h) } else { 0 })
}

fn opt_of(x: i64, is_null: u8) -> (r: Option<i64>)
    ensures opt_int(r) == (if is_null != 0 { None } else { Some(x as int) }),
{
    if is_null != 0 { None } else { Some(x) }
}

fn emit_tags(out: &mut Vec<Rec>, tags: &Vec<TagRt>, n: &NumRt, s: &SubRt, v: &Wide, w: i128, Ghost(p): Ghost<Params>)
    requires
        n.rv().value is Some,
        v@ == n.rv().value->0,
        n.tag__null == 0,
        n.version__null == 0,
        w >= 0,
        all_nonneg(old(out)@),
    ensures
        all_nonneg(final(out)@),
        forall|h: spec_fn(JK) -> int| #[trigger] recsum(final(out)@, p, h)
            == recsum(old(out)@, p, h) + tag_sum(rows_v(tags@), p, n.rv(), s.rv(), w as int, h),
{
    let ghost start = out@;
    let mut i: usize = 0;
    proof {
        assert forall|h: spec_fn(JK) -> int| #[trigger] recsum(out@, p, h)
            == recsum(start, p, h) + tag_sum(rows_v(tags@.take(0)), p, n.rv(), s.rv(), w as int, h) by {
            assert(rows_v(tags@.take(0)) =~= Seq::<TagRow>::empty());
        }
    }
    while i < tags.len()
        invariant
            i <= tags.len(),
            n.rv().value is Some,
            v@ == n.rv().value->0,
            n.tag__null == 0,
            n.version__null == 0,
            w >= 0,
            all_nonneg(out@),
            forall|h: spec_fn(JK) -> int| #[trigger] recsum(out@, p, h)
                == recsum(start, p, h) + tag_sum(rows_v(tags@.take(i as int)), p, n.rv(), s.rv(), w as int, h),
        decreases tags.len() - i,
    {
        let t = &tags[i];
        let hit = t.abstract___null == 0 && t.abstract_ == 0
            && bytes_eq_exec(t.tag.as_slice(), n.tag.as_slice())
            && bytes_eq_exec(t.version.as_slice(), n.version.as_slice());
        let ghost before = out@;
        proof {
            assert(hit == (c_t(t.rv()) && m_t(n.rv(), t.rv())));
            assert forall|h: spec_fn(JK) -> int| #[trigger] recsum(before, p, h)
                == recsum(start, p, h) + tag_sum(rows_v(tags@.take(i as int)), p, n.rv(), s.rv(), w as int, h) by {}
        }
        if hit {
            let r = Rec {
                sic: opt_of(s.sic, s.sic__null),
                tlabel: if t.tlabel__null != 0 { None } else { Some(copy_bytes(t.tlabel.as_slice())) },
                cik: opt_of(s.cik, s.cik__null),
                v: v.dup(),
                w,
            };
            proof { assert(rec_jk(p, r) == jk(p, n.rv(), s.rv(), t.rv())); }
            out.push(r);
            proof {
                assert forall|k: int| 0 <= k < out@.len() implies (#[trigger] out@[k]).w >= 0 by {
                    if k < before.len() { assert(out@[k] == before[k]); }
                }
            }
        }
        proof {
            let tv = tags@.take(i as int);
            let tv1 = tags@.take(i as int + 1);
            assert(tv1 =~= tv.push(tags@[i as int]));
            assert(rows_v(tv1) =~= rows_v(tv).push(t.rv()));
            assert forall|h: spec_fn(JK) -> int| #[trigger] recsum(out@, p, h)
                == recsum(start, p, h) + tag_sum(rows_v(tags@.take(i as int + 1)), p, n.rv(), s.rv(), w as int, h) by {
                let f = |t: TagRow| if c_t(t) && m_t(n.rv(), t) { w as int * h(jk(p, n.rv(), s.rv(), t)) } else { 0 };
                let rt = |r: Rec| r.w as int * h(rec_jk(p, r));
                lemma_sum_push(rows_v(tv), t.rv(), f);
                assert(tag_sum(rows_v(tv), p, n.rv(), s.rv(), w as int, h) == sum(rows_v(tv), f));
                assert(tag_sum(rows_v(tv1), p, n.rv(), s.rv(), w as int, h) == sum(rows_v(tv).push(t.rv()), f));
                if hit {
                    let last = out@.last();
                    assert(out@ =~= before.push(last));
                    lemma_sum_push(before, last, rt);
                    assert(recsum(out@, p, h) == recsum(before, p, h) + rt(last));
                    assert(rt(last) == f(t.rv()));
                } else {
                    assert(out@ =~= before);
                    assert(f(t.rv()) == 0);
                }
            }
        }
        i += 1;
    }
    proof { assert(tags@.take(tags.len() as int) =~= tags@); }
}

fn emit_subs(out: &mut Vec<Rec>, subs: &Vec<SubRt>, tags: &Vec<TagRt>, n: &NumRt, v: &Wide, w: i128, p1: i64, p2: i64,
    Ghost(p): Ghost<Params>)
    requires
        n.rv().value is Some,
        v@ == n.rv().value->0,
        n.adsh__null == 0,
        n.tag__null == 0,
        n.version__null == 0,
        p.p1 == p1 as int,
        p.p2 == p2 as int,
        w >= 0,
        all_nonneg(old(out)@),
    ensures
        all_nonneg(final(out)@),
        forall|h: spec_fn(JK) -> int| #[trigger] recsum(final(out)@, p, h)
            == recsum(old(out)@, p, h) + sub_sum(rows_v(subs@), p, rows_v(tags@), n.rv(), w as int, h),
{
    let ghost start = out@;
    let mut i: usize = 0;
    proof {
        assert forall|h: spec_fn(JK) -> int| #[trigger] recsum(out@, p, h)
            == recsum(start, p, h) + sub_sum(rows_v(subs@.take(0)), p, rows_v(tags@), n.rv(), w as int, h) by {
            assert(rows_v(subs@.take(0)) =~= Seq::<SubRow>::empty());
        }
    }
    while i < subs.len()
        invariant
            i <= subs.len(),
            n.rv().value is Some,
            v@ == n.rv().value->0,
            n.adsh__null == 0,
            n.tag__null == 0,
            n.version__null == 0,
            p.p1 == p1 as int,
            p.p2 == p2 as int,
            w >= 0,
            all_nonneg(out@),
            forall|h: spec_fn(JK) -> int| #[trigger] recsum(out@, p, h)
                == recsum(start, p, h) + sub_sum(rows_v(subs@.take(i as int)), p, rows_v(tags@), n.rv(), w as int, h),
        decreases subs.len() - i,
    {
        let s = &subs[i];
        let hit = s.sic__null == 0 && p1 <= s.sic && s.sic <= p2 && bytes_eq_exec(s.adsh.as_slice(), n.adsh.as_slice());
        let ghost before = out@;
        proof {
            assert(hit == (c_s(p, s.rv()) && m_s(n.rv(), s.rv())));
            assert forall|h: spec_fn(JK) -> int| #[trigger] recsum(before, p, h)
                == recsum(start, p, h) + sub_sum(rows_v(subs@.take(i as int)), p, rows_v(tags@), n.rv(), w as int, h) by {}
        }
        if hit {
            emit_tags(out, tags, n, s, v, w, Ghost(p));
        }
        proof {
            let sv = subs@.take(i as int);
            assert(subs@.take(i as int + 1) =~= sv.push(subs@[i as int]));
            assert(rows_v(subs@.take(i as int + 1)) =~= rows_v(sv).push(s.rv()));
            assert forall|h: spec_fn(JK) -> int| #[trigger] recsum(out@, p, h)
                == recsum(start, p, h) + sub_sum(rows_v(subs@.take(i as int + 1)), p, rows_v(tags@), n.rv(), w as int, h) by {
                let f = |s: SubRow| if c_s(p, s) && m_s(n.rv(), s) { tag_sum(rows_v(tags@), p, n.rv(), s, w as int, h) } else { 0 };
                lemma_sum_push(rows_v(sv), s.rv(), f);
                if !hit { assert(out@ =~= before); }
            }
        }
        i += 1;
    }
    proof { assert(subs@.take(subs.len() as int) =~= subs@); }
}

/// How many pre rows of a bucket match the num row (and the statement).
fn pre_weight(pres: &Vec<PreRt>, n: &NumRt, stmt: &[u8], Ghost(p): Ghost<Params>) -> (w: usize)
    requires p.p3 == stmt@,
    ensures w as int == sum(rows_v(pres@), |r: PreRow| if c_p(p, n.rv(), r) { 1int } else { 0int }),
{
    let mut w: usize = 0;
    let mut i: usize = 0;
    while i < pres.len()
        invariant
            i <= pres.len(),
            p.p3 == stmt@,
            w <= i,
            w as int == sum(rows_v(pres@.take(i as int)), |r: PreRow| if c_p(p, n.rv(), r) { 1int } else { 0int }),
        decreases pres.len() - i,
    {
        let r = &pres[i];
        let hit = r.stmt__null == 0 && bytes_eq_exec(r.stmt.as_slice(), stmt)
            && same_text(&r.adsh, r.adsh__null, &n.adsh, n.adsh__null)
            && same_text(&r.tag, r.tag__null, &n.tag, n.tag__null)
            && same_text(&r.version, r.version__null, &n.version, n.version__null);
        proof {
            assert(hit == c_p(p, n.rv(), r.rv()));
            let sv = pres@.take(i as int);
            assert(pres@.take(i as int + 1) =~= sv.push(pres@[i as int]));
            assert(rows_v(pres@.take(i as int + 1)) =~= rows_v(sv).push(r.rv()));
            lemma_sum_push(rows_v(sv), r.rv(), |r: PreRow| if c_p(p, n.rv(), r) { 1int } else { 0int });
        }
        if hit { w += 1; }
        i += 1;
    }
    proof { assert(pres@.take(pres.len() as int) =~= pres@); }
    w
}

fn bucket_of<R: Keyed>(b: &Index<Vec<R>>, k: &Key) -> (v: Arc<Vec<R>>)
    ensures rows_v(v@) == rows_at::<R>(*b, k.view()),
{
    match b.get(k) {
        Some(a) => a,
        None => Arc::new(Vec::new()),
    }
}

/// The records of one num row.
pub fn emit_num(
    out: &mut Vec<Rec>,
    sub_bag: &Index<Vec<SubRt>>,
    tag_bag: &Index<Vec<TagRt>>,
    pre_bag: &Index<Vec<PreRt>>,
    n: &NumRt,
    p1: i64, p2: i64, p3: &[u8], p4: &[u8],
    pows: &Vec<Wide>,
    Ghost(db): Ghost<Db>,
)
    requires
        bag_holds::<SubRt>(*sub_bag, db.sub),
        bag_holds::<TagRt>(*tag_bag, db.tag),
        bag_holds::<PreRt>(*pre_bag, db.pre),
        pows_ok(pows@),
        n.value__null != 0 || f64_finite(n.value),
        all_nonneg(old(out)@),
    ensures
        all_nonneg(final(out)@),
        forall|h: spec_fn(JK) -> int| #[trigger] recsum(final(out)@, params(p1 as int, p2 as int, p3@, p4@), h)
            == recsum(old(out)@, params(p1 as int, p2 as int, p3@, p4@), h) + num_term(db, params(p1 as int, p2 as int, p3@, p4@), h, n.rv()),
{
    let ghost p = params(p1 as int, p2 as int, p3@, p4@);
    let cn = n.uom__null == 0 && bytes_eq_exec(n.uom.as_slice(), p4) && n.value__null == 0
        && n.adsh__null == 0 && n.tag__null == 0 && n.version__null == 0;
    proof { assert(cn == c_n(p, n.rv())); }
    if !cn {
        return;
    }
    let v = wide_of_bits(n.value, pows);
    let ghost nv = n.rv();
    let ks = make_key1(n.adsh.as_slice());
    let kt = make_key2(n.tag.as_slice(), n.version.as_slice());
    let kp = make_key3(n.adsh.as_slice(), n.tag.as_slice(), n.version.as_slice());
    let sv = bucket_of::<SubRt>(sub_bag, &ks);
    let tv = bucket_of::<TagRt>(tag_bag, &kt);
    let pv = bucket_of::<PreRt>(pre_bag, &kp);
    let w = pre_weight(&*pv, n, p3, Ghost(p));
    proof {
        // the table's pre / tag / sub sums are those of the buckets
        let ind = |r: PreRow| if c_p(p, nv, r) { 1int } else { 0int };
        assert forall|r: PreRow| #[trigger] ind(r) != 0 ==> <PreRt as Keyed>::vkey(r) == Some(kp.view()) by {
            if ind(r) != 0 { assert(c_p(p, nv, r)); }
        }
        lemma_bucket_sum::<PreRt>(*pre_bag, db.pre, kp.view(), ind);
        assert(prew(db, p, nv) == w as int);
        assert forall|h: spec_fn(JK) -> int, s: SubRow| #[trigger] tag_term(db, p, h, nv, s)
            == tag_sum(rows_v(tv@), p, nv, s, w as int, h) by {
            let f = |t: TagRow| if c_t(t) && m_t(nv, t) { prew(db, p, nv) * h(jk(p, nv, s, t)) } else { 0 };
            assert forall|t: TagRow| #[trigger] f(t) != 0 ==> <TagRt as Keyed>::vkey(t) == Some(kt.view()) by {
                if f(t) != 0 { assert(m_t(nv, t)); }
            }
            lemma_bucket_sum::<TagRt>(*tag_bag, db.tag, kt.view(), f);
        }
    }
    let ghost start = out@;
    emit_subs(out, &*sv, &*tv, n, &v, w as i128, p1, p2, Ghost(p));
    proof {
        assert forall|h: spec_fn(JK) -> int| #[trigger] recsum(out@, p, h)
            == recsum(start, p, h) + num_term(db, p, h, nv) by {
            let f = |s: SubRow| if c_s(p, s) && m_s(nv, s) { tag_term(db, p, h, nv, s) } else { 0 };
            let g = |s: SubRow| if c_s(p, s) && m_s(nv, s) { tag_sum(rows_v(tv@), p, nv, s, w as int, h) } else { 0 };
            assert forall|s: SubRow| #[trigger] f(s) == g(s) by {}
            lemma_sum_ext_all(db.sub, f, g);
            assert forall|s: SubRow| #[trigger] f(s) != 0 ==> <SubRt as Keyed>::vkey(s) == Some(ks.view()) by {
                if f(s) != 0 { assert(m_s(nv, s)); }
            }
            lemma_bucket_sum::<SubRt>(*sub_bag, db.sub, ks.view(), f);
            lemma_sum_ext_all(rows_at::<SubRt>(*sub_bag, ks.view()), f, g);
        }
    }
}

}
