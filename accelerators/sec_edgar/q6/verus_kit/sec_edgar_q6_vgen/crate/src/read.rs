// Reading the answer: the first LIMIT kept groups of the asked class.
#![allow(unused_imports)]
use vstd::prelude::*;
use std::sync::Arc;
use verus_common::rel::*;
use verus_prooflib::ordered::{Index, Key, KeyView, Cursor};
use verus_prooflib::wide::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::esc::*;
use crate::accmap::*;
use crate::f64bits::*;
use crate::grp::*;
use crate::inv::*;
use crate::readp::*;

verus! {

pub fn read(cls: &Index<ClassRec>, Ghost(db): Ghost<Db>, p1: i64, p2: &[u8], p3: &[u8]) -> (r: Option<Vec<OutT>>)
    requires cls_ok(db, *cls),
    ensures
        r matches Some(out) ==> query_result(db, params(p1 as int, p2@, p3@), out_view(out@)),
        r matches Some(out) ==> forall|i: int| 0 <= i < out.len() ==> out_fits(#[trigger] out_view(out@)[i]),
        r is None ==> !answer_fits(db, params(p1 as int, p2@, p3@)),
{
    let ghost p = params(p1 as int, p2@, p3@);
    let ok = okey_of(p1, p2, p3);
    let mut out: Vec<OutT> = Vec::new();
    let c = match cls.get(&ok) {
        None => {
            proof {
                lemma_read_none(db, *cls, p);
                assert(out_view(out@) =~= Seq::<OutRow>::empty());
            }
            return Some(out);
        }
        Some(c) => c,
    };
    proof { assert(cls.at(ok.view()) == Some(arc_val(&c))); assert(ok.view() == okey(p.p1, p.p2, p.p3)); }
    if !c.hp.is_empty() {
        proof { lemma_read_unfit_p(db, *cls, p, arc_val(&c)); }
        return None;
    }
    let ghost cv = arc_val(&c);
    let ghost es = cv.r.entries();
    let ghost full = lemma_read_full(db, *cls, p, cv);
    let ghost rr = r_rows(p.p2, es);
    let mut cur = c.r.cursor();
    let mut t: usize = 0;
    let mut more = true;
    proof { assert(cur.remaining() =~= es.skip(0)); }
    while t < 200 && more
        invariant
            t <= 200, out.len() == t,
            cv == arc_val(&c), cv.r.wf(), es == cv.r.entries(), rr == r_rows(p.p2, es),
            p == params(p1 as int, p2@, p3@),
            cls_ok(db, *cls), cls.at(okey(p.p1, p.p2, p.p3)) == Some(cv), cv.hp.entries().len() == 0,
            full_result(db, p, full), full.len() >= es.len(), full.take(es.len() as int) =~= rr,
            cv.f.entries().len() == 0 ==> forall|i: int| es.len() <= i < full.len() ==> !out_fits(#[trigger] full[i]),
            cv.f.entries().len() == 0 && cv.hn.entries().len() == 0 ==> full.len() == es.len(),
            cv.f.entries().len() == 0 && cv.hn.entries().len() > 0 ==> full.len() > es.len(),
            crec_full(cv),
            t <= es.len(),
            cur.remaining() == es.skip(t as int),
            !more ==> t == es.len(),
            out_view(out@) == full.take(t as int),
            forall|j: int| 0 <= j < t ==> out_fits(full[j]),
        decreases (200 - t) * 2 + if more { 1int } else { 0int },
    {
        let ghost before = out@;
        match cur.next() {
            None => {
                more = false;
                proof { assert(es.skip(t as int).len() == 0); }
            }
            Some((key, g)) => {
                proof {
                    assert(es.skip(t as int)[0] == es[t as int]);
                    assert(es.skip(t as int).skip(1) =~= es.skip(t as int + 1));
                    cv.r.model();
                    assert(cv.r.at(es[t as int].0) == Some(es[t as int].1));
                    assert(full[t as int] == rr[t as int]);
                    assert(rr[t as int] == g_row(p.p2, arc_val(&g)));
                    assert(exact_g(db, cv, arc_val(&g)));
                }
                let cn = cnt_i64(&g.cnt);
                if cn.is_none() {
                    proof {
                        assert(!out_fits(full[t as int]));
                        lemma_not_fits(db, p, full, t as int);
                    }
                    return None;
                }
                let tup: OutT = (copy_opt(&g.name), Some(copy_bytes(p2)), Some(copy_bytes(g.tag.as_slice())), copy_opt(&g.plabel), Some(g.bits), cn.unwrap());
                out.push(tup);
                proof {
                    lemma_f64_in_range(g.bits);
                    assert(out_view(out@) =~= out_view(before).push(full[t as int]));
                    assert(full.take(t as int + 1) =~= full.take(t as int).push(full[t as int]));
                    assert(out_fits(full[t as int]));
                }
                t += 1;
            }
        }
    }
    proof {
        if t < 200 {
            assert(!more);
            assert(es.len() == t);
            if cv.f.entries().len() != 0 { assert(false); }
        }
    }
    let hn_some = !c.hn.is_empty();
    if t < 200 && hn_some {
        proof { assert(full.len() > es.len()); lemma_not_fits(db, p, full, es.len() as int); }
        return None;
    }
    proof {
        assert(out_view(out@) =~= limit_rows(full, 200)) by {
            if t < 200 { assert(full.len() == es.len()); assert(full.take(t as int) =~= full); }
            else { if full.len() <= 200 { assert(full.take(200) =~= full); } }
        }
        assert forall|j: int| 0 <= j < out.len() implies out_fits(#[trigger] out_view(out@)[j]) by {
            assert(out_view(out@)[j] == full[j]);
        }
    }
    Some(out)
}

} // verus!
