// The join: one record per (num, sub) combination of a parameter set.  Summing a group aggregate over
// the records is summing it over the query's tuples.
use vstd::prelude::*;
use std::sync::Arc;
use verus_common::bag::*;
use verus_common::bytes::*;
use verus_common::rel::*;
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
    pub name: Option<Vec<u8>>,
    pub cik: Option<i64>,
    pub v: Wide,
    pub vb: u64,
}

/// A record's value is the double of its bits.
pub open spec fn rec_ok(r: Rec) -> bool {
    r.v@ == f64_val(r.vb) && f64_finite(r.vb)
}

pub open spec fn all_rec_ok(l: Seq<Rec>) -> bool {
    forall|i: int| 0 <= i < l.len() ==> rec_ok(#[trigger] l[i])
}

pub open spec fn opt_int(x: Option<i64>) -> Option<int> {
    match x { Some(a) => Some(a as int), None => None }
}

pub open spec fn opt_bytes(x: Option<Vec<u8>>) -> Option<Seq<u8>> {
    match x { Some(a) => Some(a@), None => None }
}

pub open spec fn rec_jk(r: Rec) -> JK {
    JK { name: opt_bytes(r.name), cik: opt_int(r.cik), v: r.v@, vs: true }
}

pub open spec fn recsum(l: Seq<Rec>, h: spec_fn(JK) -> int) -> int {
    sum(l, |r: Rec| h(rec_jk(r)))
}

pub open spec fn sub_sum(subs: Seq<SubRow>, p: Params, n: NumRow, h: spec_fn(JK) -> int) -> int {
    sum(subs, |s: SubRow| if c_s(p, s) && m_s(n, s) { h(jk(n, s)) } else { 0 })
}

fn emit_subs(out: &mut Vec<Rec>, subs: &Vec<SubRt>, n: &NumRt, v: &Wide, p1: i64, Ghost(p): Ghost<Params>)
    requires
        n.rv().value is Some,
        v@ == n.rv().value->0,
        n.adsh__null == 0,
        p.p1 == p1 as int,
        f64_finite(n.value),
        all_rec_ok(old(out)@),
    ensures
        all_rec_ok(final(out)@),
        forall|h: spec_fn(JK) -> int| #[trigger] recsum(final(out)@, h)
            == recsum(old(out)@, h) + sub_sum(rows_v(subs@), p, n.rv(), h),
{
    let ghost start = out@;
    let mut i: usize = 0;
    proof {
        assert forall|h: spec_fn(JK) -> int| #[trigger] recsum(out@, h)
            == recsum(start, h) + sub_sum(rows_v(subs@.take(0)), p, n.rv(), h) by {
            assert(rows_v(subs@.take(0)) =~= Seq::<SubRow>::empty());
        }
    }
    while i < subs.len()
        invariant
            i <= subs.len(),
            n.rv().value is Some,
            v@ == n.rv().value->0,
            n.adsh__null == 0,
            p.p1 == p1 as int,
            f64_finite(n.value),
            all_rec_ok(out@),
            forall|h: spec_fn(JK) -> int| #[trigger] recsum(out@, h)
                == recsum(start, h) + sub_sum(rows_v(subs@.take(i as int)), p, n.rv(), h),
        decreases subs.len() - i,
    {
        let s = &subs[i];
        let hit = s.fy__null == 0 && s.fy == p1 && bytes_eq_exec(s.adsh.as_slice(), n.adsh.as_slice());
        let ghost before = out@;
        proof {
            assert(hit == (c_s(p, s.rv()) && m_s(n.rv(), s.rv())));
            assert forall|h: spec_fn(JK) -> int| #[trigger] recsum(before, h)
                == recsum(start, h) + sub_sum(rows_v(subs@.take(i as int)), p, n.rv(), h) by {}
        }
        if hit {
            let r = Rec {
                name: if s.name__null != 0 { None } else { Some(copy_bytes(s.name.as_slice())) },
                cik: if s.cik__null != 0 { None } else { Some(s.cik) },
                v: v.dup(),
                vb: n.value,
            };
            proof { assert(rec_jk(r) == jk(n.rv(), s.rv())); assert(rec_ok(r)); }
            out.push(r);
            proof {
                assert forall|k: int| 0 <= k < out@.len() implies rec_ok(#[trigger] out@[k]) by {
                    if k < before.len() { assert(out@[k] == before[k]); }
                }
            }
        }
        proof {
            let sv = subs@.take(i as int);
            assert(subs@.take(i as int + 1) =~= sv.push(subs@[i as int]));
            assert(rows_v(subs@.take(i as int + 1)) =~= rows_v(sv).push(s.rv()));
            assert forall|h: spec_fn(JK) -> int| #[trigger] recsum(out@, h)
                == recsum(start, h) + sub_sum(rows_v(subs@.take(i as int + 1)), p, n.rv(), h) by {
                let f = |s: SubRow| if c_s(p, s) && m_s(n.rv(), s) { h(jk(n.rv(), s)) } else { 0 };
                let rt = |r: Rec| h(rec_jk(r));
                lemma_sum_push(rows_v(sv), s.rv(), f);
                assert(sub_sum(rows_v(sv), p, n.rv(), h) == sum(rows_v(sv), f));
                assert(sub_sum(rows_v(subs@.take(i as int + 1)), p, n.rv(), h) == sum(rows_v(sv).push(s.rv()), f));
                if hit {
                    let last = out@.last();
                    assert(out@ =~= before.push(last));
                    lemma_sum_push(before, last, rt);
                    assert(recsum(out@, h) == recsum(before, h) + rt(last));
                    assert(rt(last) == f(s.rv()));
                    assert(recsum(before, h) == recsum(start, h) + sub_sum(rows_v(sv), p, n.rv(), h));
                } else {
                    assert(out@ =~= before);
                    assert(f(s.rv()) == 0);
                    assert(recsum(before, h) == recsum(start, h) + sub_sum(rows_v(sv), p, n.rv(), h));
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

/// The records of one num row.
pub fn emit_num(
    out: &mut Vec<Rec>,
    sub_bag: &Index<Vec<SubRt>>,
    n: &NumRt,
    p1: i64, p2: &[u8],
    pows: &Vec<Wide>,
    Ghost(db): Ghost<Db>,
)
    requires
        bag_holds::<SubRt>(*sub_bag, db.sub),
        pows_ok(pows@),
        n.value__null != 0 || f64_finite(n.value),
        all_rec_ok(old(out)@),
    ensures
        all_rec_ok(final(out)@),
        forall|h: spec_fn(JK) -> int| #[trigger] recsum(final(out)@, h)
            == recsum(old(out)@, h) + num_term(db, params(p1 as int, p2@), h, n.rv()),
{
    let ghost p = params(p1 as int, p2@);
    let cn = n.uom__null == 0 && bytes_eq_exec(n.uom.as_slice(), p2) && n.value__null == 0 && n.adsh__null == 0;
    proof { assert(cn == c_n(p, n.rv())); }
    if !cn {
        return;
    }
    let v = wide_of_bits(n.value, pows);
    let ghost nv = n.rv();
    let ks = make_key1(n.adsh.as_slice());
    let sv = bucket_of::<SubRt>(sub_bag, &ks);
    let ghost start = out@;
    emit_subs(out, &*sv, n, &v, p1, Ghost(p));
    proof {
        assert forall|h: spec_fn(JK) -> int| #[trigger] recsum(out@, h) == recsum(start, h) + num_term(db, p, h, nv) by {
            let f = |s: SubRow| if c_s(p, s) && m_s(nv, s) { h(jk(nv, s)) } else { 0 };
            assert forall|s: SubRow| #[trigger] f(s) != 0 ==> <SubRt as Keyed>::vkey(s) == Some(ks.view()) by {
                if f(s) != 0 { assert(m_s(nv, s)); }
            }
            lemma_bucket_sum::<SubRt>(*sub_bag, db.sub, ks.view(), f);
            assert(sub_sum(rows_v(sv@), p, nv, h) == sum(rows_at::<SubRt>(*sub_bag, ks.view()), f));
            assert(sub_term(db, p, h, nv) == sum(db.sub, f));
        }
    }
}

}
