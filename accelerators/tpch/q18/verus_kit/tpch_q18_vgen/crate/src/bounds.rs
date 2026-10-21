use vstd::prelude::*;
use verus_common::sums::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::model::*;

verus! {

/// Summands within [-b, b] add up to within [-len * b, len * b].
pub proof fn lemma_sum_bounded<A>(s: Seq<A>, f: spec_fn(A) -> int, b: int)
    requires b >= 0, forall|i: int| 0 <= i < s.len() ==> -b <= f(#[trigger] s[i]) <= b,
    ensures -(s.len() as int * b) <= sum(s, f) <= s.len() as int * b,
    decreases s.len(),
{
    if s.len() > 0 {
        let t = s.drop_last();
        assert forall|i: int| 0 <= i < t.len() implies -b <= f(#[trigger] t[i]) <= b by { assert(t[i] == s[i]); }
        lemma_sum_bounded(t, f, b);
        assert(s.len() as int * b == t.len() as int * b + b) by (nonlinear_arith) requires s.len() == t.len() + 1;
        assert(s =~= t.push(s.last()));
        lemma_sum_push(t, s.last(), f);
    }
}

/// Within the envelope an order's total quantity fits an i128.
pub proof fn lemma_quantity_fits(db: Db, key: int)
    requires envelope(db),
    ensures i128::MIN <= sum(db.lineitem, qw(key)) <= i128::MAX,
{
    assert forall|i: int| 0 <= i < db.lineitem.len() implies -999999999999999 <= qw(key)(#[trigger] db.lineitem[i]) <= 999999999999999 by {
        assert(lineitem_in_type(db.lineitem[i]));
    }
    lemma_sum_bounded(db.lineitem, qw(key), 999999999999999);
    let n = db.lineitem.len() as int;
    assert(n * 999999999999999 <= 1099511627776int * 999999999999999) by (nonlinear_arith) requires 0 <= n <= 1099511627776;
}

}
