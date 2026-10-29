// Reading a class in value order until a hundred rows are in and the last value's block is complete.
use vstd::prelude::*;
use std::sync::Arc;
use verus_common::sums::*;
use verus_prooflib::ordered::{Index, Key, KeyView, key_lt, ordered_rows};
use crate::gen_spec::*;
use crate::cells::*;
use crate::classes::*;
use crate::finish::*;
use crate::sortout::rows_finite;
use crate::f64bits::f64_finite;
use crate::maint::lemma_views_push;
use crate::thr::*;
use crate::cnt::*;

verus! {

/// An unbounded class has every entry within its lead.
pub proof fn lemma_lower_all(es: Seq<(KeyView, Vec<Row>)>, d: int)
    requires
        ordered_rows(es),
        forall|i: int| 0 <= i < es.len() ==> (#[trigger] es[i]).0.0 <= d,
    ensures
        lead(es, d) == es.len(),
{
    lemma_lead(es, d);
    if lead(es, d) < es.len() {
        let i = lead(es, d);
        assert(es[i].0.0 > d);
    }
}

pub fn walk(inner: &Index<Vec<Row>>, dmax: i64) -> (res: Vec<Row>)
    requires
        inner.wf(),
        forall|i: int| 0 <= i < inner.entries().len() ==> rows_finite((#[trigger] inner.entries()[i]).1@),
        forall|i: int| 0 <= i < inner.entries().len() ==> (#[trigger] inner.entries()[i]).1@.len() > 0,
        forall|i: int| 0 <= i < inner.entries().len() ==> (#[trigger] inner.entries()[i]).0.0 <= D_INF as int,
        dmax == D_INF || lead(inner.entries(), dmax as int) >= 100,
    ensures
        rows_finite(res@),
        exists|m: int| {
            &&& 0 <= m <= inner.entries().len()
            &&& views(res@) == flat_rows(inner.entries().take(m))
            &&& (res@.len() >= 100 || (m == inner.entries().len() && dmax == D_INF))
            &&& (m == inner.entries().len() || (m > 0 && inner.entries()[m].0.0 > inner.entries()[m - 1].0.0))
            &&& (m == 0 || inner.entries()[m - 1].0.0 <= dmax as int)
        },
{
    let ghost es = inner.entries();
    proof { inner.model(); lemma_lead(es, dmax as int); }
    let ghost q = lead(es, dmax as int);
    proof { if dmax == D_INF { assert forall|i: int| 0 <= i < es.len() implies es[i].0.0 <= dmax as int by {} ; lemma_lower_all(es, dmax as int); } }
    let mut cur = inner.cursor();
    let mut res: Vec<Row> = Vec::new();
    let mut have = false;
    let mut last: i64 = 0;
    let ghost mut m: int = 0;
    proof { assert(es.take(0) =~= Seq::<(KeyView, Vec<Row>)>::empty()); assert(es.skip(0) =~= es); }
    loop
        invariant
            es == inner.entries(),
            ordered_rows(es),
            0 <= m <= es.len(),
            cur.remaining() == es.skip(m),
            cur.work() >= 0,
            views(res@) == flat_rows(es.take(m)),
            rows_finite(res@),
            forall|i: int| 0 <= i < es.len() ==> rows_finite((#[trigger] es[i]).1@),
            have == (m > 0),
            m > 0 ==> last as int == es[m - 1].0.0,
            q == lead(es, dmax as int),
            0 <= q <= es.len(),
            forall|i: int| 0 <= i < q ==> (#[trigger] es[i]).0.0 <= dmax as int,
            forall|i: int| q <= i < es.len() ==> (#[trigger] es[i]).0.0 > dmax as int,
            forall|i: int| 0 <= i < es.len() ==> (#[trigger] es[i]).1@.len() > 0,
            dmax == D_INF || q >= 100,
            dmax == D_INF ==> q == es.len(),
            m <= q,
            res@.len() >= m,
        decreases cur.work(),
    {
        let ghost before = cur.remaining();
        match cur.next() {
            None => {
                proof {
                    assert(es.skip(m).len() == 0);
                    assert(m == es.len());
                    assert(m == q);
                    assert(m == 0 || es[m - 1].0.0 <= dmax as int);
                }
                return res;
            }
            Some((k, cell)) => {
                proof {
                    assert(before.len() > 0);
                    assert(m < es.len());
                    assert(before[0] == es[m]);
                    assert(cur.remaining() == es.skip(m).skip(1));
                    assert(es.skip(m).skip(1) =~= es.skip(m + 1));
                }
                if res.len() >= 100 && have && k.number != last {
                    proof {
                        assert(res@.len() >= 100);
                        assert(m > 0);
                        assert(k.number as int != es[m - 1].0.0);
                        assert(key_lt(es[m - 1].0, es[m].0));
                        assert(es[m].0.0 > es[m - 1].0.0);
                        assert(m <= q);
                        assert(es[m - 1].0.0 <= dmax as int);
                    }
                    return res;
                }
                proof {
                    if m == q {
                        // the first entry beyond the threshold: a hundred rows are in, so the walk stops here
                        assert(dmax != D_INF);
                        assert(es[m].0.0 > dmax as int);
                        assert(res@.len() >= 100);
                        assert(have);
                        assert(es[m - 1].0.0 <= dmax as int);
                        assert(k.number as int != last as int);
                        assert(false);
                    }
                }
                let ghost base = res@;
                proof { assert(rows_finite(es[m].1@)); assert(*cell == es[m].1); }
                let mut j: usize = 0;
                proof { assert(cell@.take(0) =~= Seq::<Row>::empty()); }
                while j < cell.len()
                    invariant
                        j <= cell@.len(),
                        views(res@) == views(base) + views(cell@.take(j as int)),
                        rows_finite(res@),
                        rows_finite(cell@),
                    decreases cell.len() - j,
                {
                    let ghost before = res@;
                    res.push(copy_row(&cell[j]));
                    proof {
                        assert(rows_finite(res@)) by {
                            assert forall|k: int| 0 <= k < res@.len() implies f64_finite((#[trigger] res@[k]).val) by {
                                if k < before.len() { assert(res@[k] == before[k]); } else { assert(res@[k] == cell@[j as int]); }
                            }
                        }
                        lemma_views_push(before, cell@[j as int]);
                        assert(cell@.take(j as int + 1) =~= cell@.take(j as int).push(cell@[j as int]));
                        assert(views(cell@.take(j as int + 1)) =~= views(cell@.take(j as int)).push(cell@[j as int].view()));
                        assert(views(res@) =~= views(base) + views(cell@.take(j as int + 1)));
                    }
                    j += 1;
                }
                proof {
                    assert(cell@.take(cell@.len() as int) =~= cell@);
                    lemma_flat_rows_push(es, m);
                    assert(entry_views(es[m]) == views(cell@));
                    assert(res@.len() == base.len() + cell@.len());
                    assert(cell@.len() > 0);
                }
                last = k.number;
                have = true;
                proof { m = m + 1; }
            }
        }
    }
}

} // verus!
