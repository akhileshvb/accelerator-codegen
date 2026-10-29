// The lists apply builds from a transaction's rescans and deltas.
use vstd::prelude::*;
use verus_common::bag::*;
use verus_common::sums::*;
use verus_prooflib::ordered::Index;
use crate::algebra::*;
use crate::bucket::*;
use crate::emit::*;
use crate::gen_obligations::*;
use crate::gen_spec::*;
use crate::keyset::*;
use crate::keys::*;
use crate::rows::*;

verus! {

/// A row with an adsh, the key of a filing.
pub trait AdshRow: BagRow {
    spec fn adsh_of(v: Self::V) -> Option<Seq<u8>>;

    fn adsh_exec(&self) -> (r: Option<&[u8]>)
        ensures
            match r {
                Some(x) => Self::adsh_of(self.rv()) == Some(x@),
                None => Self::adsh_of(self.rv()) is None,
            },
    ;
}

impl AdshRow for NumRt {
    open spec fn adsh_of(v: NumRow) -> Option<Seq<u8>> {
        v.adsh
    }

    fn adsh_exec(&self) -> (r: Option<&[u8]>) {
        if self.adsh__null != 0 { None } else { Some(self.adsh.as_slice()) }
    }
}

impl AdshRow for PreRt {
    open spec fn adsh_of(v: PreRow) -> Option<Seq<u8>> {
        v.adsh
    }

    fn adsh_exec(&self) -> (r: Option<&[u8]>) {
        if self.adsh__null != 0 { None } else { Some(self.adsh.as_slice()) }
    }
}

/// The rows of a filing rescan, then the rows of a tag rescan that are not of those filings.
pub fn merge_rows<R: AdshRow>(r1: &Vec<R>, r2: &Vec<R>, aset: &Index<()>, Ghost(a): Ghost<Seq<Seq<u8>>>) -> (out: Vec<R>)
    requires set_holds(*aset, a),
    ensures
        forall|f: spec_fn(R::V) -> int| #[trigger] sum(rows_v(out@), f)
            == sum(rows_v(r1@), f) + sum(rows_v(r2@), |x: R::V| if !kin(a, R::adsh_of(x)) { f(x) } else { 0 }),
        forall|i: int| 0 <= i < out.len() ==> rows_v(r1@).contains((#[trigger] out@[i]).rv())
            || (rows_v(r2@).contains(out@[i].rv()) && !kin(a, R::adsh_of(out@[i].rv()))),
{
    let mut out = dup_rows_of(r1);
    let mut i: usize = 0;
    proof {
        assert forall|q: int| 0 <= q < out.len() implies rows_v(r1@).contains((#[trigger] out@[q]).rv()) by {
            assert(rows_v(out@)[q] == out@[q].rv());
        }
        assert forall|f: spec_fn(R::V) -> int| #[trigger] sum(rows_v(out@), f)
            == sum(rows_v(r1@), f) + sum(rows_v(r2@.take(0)), |x: R::V| if !kin(a, R::adsh_of(x)) { f(x) } else { 0 }) by {
            assert(rows_v(r2@.take(0)) =~= Seq::<R::V>::empty());
        }
    }
    while i < r2.len()
        invariant
            i <= r2.len(),
            set_holds(*aset, a),
            forall|f: spec_fn(R::V) -> int| #[trigger] sum(rows_v(out@), f)
                == sum(rows_v(r1@), f) + sum(rows_v(r2@.take(i as int)), |x: R::V| if !kin(a, R::adsh_of(x)) { f(x) } else { 0 }),
            forall|q: int| 0 <= q < out.len() ==> rows_v(r1@).contains((#[trigger] out@[q]).rv())
                || (rows_v(r2@).contains(out@[q].rv()) && !kin(a, R::adsh_of(out@[q].rv()))),
        decreases r2.len() - i,
    {
        let ghost before = out@;
        let keep = match r2[i].adsh_exec() {
            None => true,
            Some(x) => !in_set(aset, x, Ghost(a)),
        };
        proof {
            assert(r2@.take(i as int + 1) =~= r2@.take(i as int).push(r2@[i as int]));
            assert(rows_v(r2@.take(i as int + 1)) =~= rows_v(r2@.take(i as int)).push(r2@[i as int].rv()));
            assert(keep == !kin(a, R::adsh_of(r2@[i as int].rv())));
        }
        if keep {
            let y = r2[i].dup();
            out.push(y);
            proof {
                assert(rows_v(out@) =~= rows_v(before).push(y.rv()));
                assert(rows_v(r2@)[i as int] == r2@[i as int].rv());
                assert(rows_v(r2@).contains(r2@[i as int].rv()));
                assert forall|q: int| 0 <= q < out.len() implies rows_v(r1@).contains((#[trigger] out@[q]).rv())
                    || (rows_v(r2@).contains(out@[q].rv()) && !kin(a, R::adsh_of(out@[q].rv()))) by {
                    if q < before.len() { assert(out@[q] == before[q]); }
                    else { assert(out@[q] == y); }
                }
                assert forall|f: spec_fn(R::V) -> int| #[trigger] sum(rows_v(out@), f)
                    == sum(rows_v(r1@), f) + sum(rows_v(r2@.take(i as int + 1)), |x: R::V| if !kin(a, R::adsh_of(x)) { f(x) } else { 0 }) by {
                    lemma_sum_push(rows_v(before), y.rv(), f);
                    lemma_sum_push(rows_v(r2@.take(i as int)), r2@[i as int].rv(), |x: R::V| if !kin(a, R::adsh_of(x)) { f(x) } else { 0 });
                }
            }
        } else {
            proof {
                assert forall|f: spec_fn(R::V) -> int| #[trigger] sum(rows_v(out@), f)
                    == sum(rows_v(r1@), f) + sum(rows_v(r2@.take(i as int + 1)), |x: R::V| if !kin(a, R::adsh_of(x)) { f(x) } else { 0 }) by {
                    lemma_sum_push(rows_v(r2@.take(i as int)), r2@[i as int].rv(), |x: R::V| if !kin(a, R::adsh_of(x)) { f(x) } else { 0 });
                }
            }
        }
        i += 1;
    }
    proof { assert(r2@.take(r2.len() as int) =~= r2@); }
    out
}

fn dup_rows_of<R: BagRow>(v: &Vec<R>) -> (out: Vec<R>)
    ensures rows_v(out@) == rows_v(v@),
{
    let mut out: Vec<R> = Vec::new();
    let mut i: usize = 0;
    while i < v.len()
        invariant i <= v.len(), rows_v(out@) == rows_v(v@.take(i as int)),
        decreases v.len() - i,
    {
        let r = v[i].dup();
        proof {
            assert(v@.take(i as int + 1) =~= v@.take(i as int).push(v@[i as int]));
            assert(rows_v(v@.take(i as int + 1)) =~= rows_v(v@.take(i as int)).push(v@[i as int].rv()));
        }
        out.push(r);
        proof { assert(rows_v(out@) =~= rows_v(v@.take(i as int + 1))); }
        i += 1;
    }
    proof { assert(v@.take(v.len() as int) =~= v@); }
    out
}

pub open spec fn ones(rows: Seq<NumRow>) -> Seq<(NumRow, int)> {
    rows.map_values(|x: NumRow| (x, 1int))
}

/// The rows with sign +1 on each, as a signed list.
pub fn with_ones(rows: &Vec<NumRt>) -> (nl: Vec<(NumRt, i64)>)
    ensures nl_view(nl@) == ones(rows_v(rows@)), nl_ok(nl@),
{
    let mut nl: Vec<(NumRt, i64)> = Vec::new();
    let mut i: usize = 0;
    while i < rows.len()
        invariant i <= rows.len(), nl_view(nl@) == ones(rows_v(rows@.take(i as int))), nl_ok(nl@),
        decreases rows.len() - i,
    {
        let r = rows[i].dup();
        proof {
            assert(rows@.take(i as int + 1) =~= rows@.take(i as int).push(rows@[i as int]));
            assert(rows_v(rows@.take(i as int + 1)) =~= rows_v(rows@.take(i as int)).push(rows@[i as int].rv()));
        }
        let ghost before = nl_view(nl@);
        let ghost rv_ = r.rv();
        nl.push((r, 1));
        proof {
            assert(nl_view(nl@) =~= before.push((rv_, 1int)));
            assert(ones(rows_v(rows@.take(i as int + 1))) =~= ones(rows_v(rows@.take(i as int))).push((rv_, 1int)));
        }
        i += 1;
    }
    proof { assert(rows@.take(rows.len() as int) =~= rows@); }
    nl
}

/// Σ of a weight-1 list is the sum over the rows.
pub proof fn lemma_ones_wsum(rows: Seq<NumRow>, f: spec_fn(NumRow) -> int)
    ensures wsum(ones(rows), f) == sum(rows, f),
    decreases rows.len(),
{
    if rows.len() > 0 {
        let r0 = rows.drop_last();
        lemma_ones_wsum(r0, f);
        assert(ones(rows).drop_last() =~= ones(r0));
    }
}

/// The inserted rows and the deleted rows of a signed list.
pub fn split_signs<T: BagRow>(d: &Vec<(T, i64)>) -> (r: (Vec<T>, Vec<T>))
    requires unit_signs(dview(d@)),
    ensures
        rows_v(r.0@) == plus_rows(dview(d@)),
        rows_v(r.1@) == minus_rows(dview(d@)),
{
    let mut plus: Vec<T> = Vec::new();
    let mut minus: Vec<T> = Vec::new();
    let mut t: usize = 0;
    proof {
        assert(dview(d@.take(0)).len() == 0);
        assert(plus_rows(dview(d@.take(0))) =~= Seq::<T::V>::empty());
        assert(minus_rows(dview(d@.take(0))) =~= Seq::<T::V>::empty());
    }
    while t < d.len()
        invariant
            t <= d.len(),
            unit_signs(dview(d@)),
            rows_v(plus@) == plus_rows(dview(d@.take(t as int))),
            rows_v(minus@) == minus_rows(dview(d@.take(t as int))),
        decreases d.len() - t,
    {
        proof {
            lemma_dview_step(d@, t as int);
            assert(dview(d@)[t as int].1 == 1 || dview(d@)[t as int].1 == -1);
        }
        let ghost (pp, mm) = (rows_v(plus@), rows_v(minus@));
        if d[t].1 == 1 {
            let y = d[t].0.dup();
            plus.push(y);
            proof { assert(rows_v(plus@) =~= pp.push(y.rv())); }
        } else {
            let y = d[t].0.dup();
            minus.push(y);
            proof { assert(rows_v(minus@) =~= mm.push(y.rv())); }
        }
        t += 1;
    }
    proof { assert(d@.take(d.len() as int) =~= d@); }
    (plus, minus)
}

}
