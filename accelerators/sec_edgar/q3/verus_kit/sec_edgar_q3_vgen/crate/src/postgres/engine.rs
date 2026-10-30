// The engine's float-overflow condition on the answer.  Postgres raises on float8 overflow:
// the answer exists only when float_safe, which one check on the class's totals decides.
use vstd::prelude::*;
use verus_common::rel::*;
use verus_common::sums::*;
use verus_prooflib::wide::Wide;
use crate::gen_spec::*;
use crate::numeric::*;
use crate::qty::*;
use crate::tot::*;

verus! {

/// What the answer's proof needs besides the rows (full_result's overflow part).
#[verifier::opaque]
pub open spec fn engine_safe(db: Db, p: Params) -> bool {
    float_safe(db, p)
}

/// The check on a class's totals (companies' magnitude, tuple count): the scalar AVG's bound.
pub open spec fn tot_ok(ia: int, n: int) -> bool {
    float8_avg_safe(2 * ia, n)
}

pub fn tot_check(ia: &Wide, n: &Wide, c: &Consts) -> (r: bool)
    requires consts_ok(*c),
    ensures r == tot_ok(ia@, n@),
{
    let a2 = ia.add(ia);
    let n1 = n.add(&Wide::from_i128(1));
    let lhs = Wide::from_i128(4).mul(&n1).mul(&n1).mul(&n1).mul(&a2).mul(&a2);
    let rhs = c.top.mul(&c.pows[1074]);
    !rhs.lt(&lhs)
}

proof fn lemma_pow2_mono(a: nat, b: nat)
    requires a <= b,
    ensures pow2(a) <= pow2(b),
    decreases b,
{
    if a < b {
        lemma_pow2_mono(a, (b - 1) as nat);
        lemma_pow2_pos((b - 1) as nat);
    }
}

proof fn lemma_pow2_pos(a: nat)
    ensures pow2(a) >= 1,
    decreases a,
{
    if a > 0 { lemma_pow2_pos((a - 1) as nat); }
}

/// The scalar bound implies the magnitude itself is summable.
proof fn lemma_tot_sum_safe(ia: int, n: int)
    requires ia >= 0, n >= 0, tot_ok(ia, n),
    ensures float8_sum_safe(ia),
{
    let top = (pow2(53) - 1) * pow2(2045);
    let p = pow2(1074);
    lemma_pow2_mono(1074, 2045);
    lemma_pow2_mono(0, 53);
    lemma_pow2_pos(1074);
    assert(pow2(0) == 1);
    assert(pow2(53) >= 2) by { lemma_pow2_mono(1, 53); assert(pow2(1) == 2) by { reveal_with_fuel(pow2, 2); } }
    assert(top >= p) by (nonlinear_arith)
        requires top == (pow2(53) - 1) * pow2(2045), pow2(53) >= 2, pow2(2045) >= p, p >= 1;
    let x = 2 * ia;
    let c = (n + 1) * (n + 1) * (n + 1);
    assert(c >= 1) by (nonlinear_arith) requires n >= 0, c == (n + 1) * (n + 1) * (n + 1);
    assert(4 * (n + 1) * (n + 1) * (n + 1) * x * x == 4 * c * (x * x)) by (nonlinear_arith)
        requires c == (n + 1) * (n + 1) * (n + 1);
    assert(4 * c * (x * x) >= 4 * (x * x)) by (nonlinear_arith) requires c >= 1, x >= 0;
    if x > top {
        assert(x * x > top * top) by (nonlinear_arith) requires x > top, top >= 1;
        assert(top * p <= top * top) by (nonlinear_arith) requires top >= p, top >= 1, p >= 1;
        assert(false);
    }
}

pub proof fn lemma_tot_safe(db: Db, p: Params, ia: int, n: int)
    requires totals_bound(db, p, ia, n), tot_ok(ia, n),
    ensures engine_safe(db, p),
{
    reveal(engine_safe);
    lemma_tot_sum_safe(ia, n);
    assert(scalar_1_groups_float_safe(db, p)) by {
        lemma_sum_ext_all(avg_sub_tuples(db, p), |t: AvgSubTuple| if t.n2.value is Some { abs(t.n2.value->0) } else { 0 }, avg_mag());
        lemma_sum_ext_all(avg_sub_tuples(db, p), |t: AvgSubTuple| if t.n2.value is Some { 1int } else { 0 }, avg_vs());
    }
    assert(avg_sub_groups_float_safe(db, p)) by {
        assert forall|gc: Option<int>| #[trigger] avg_sub_group_count(db, p, gc) > 0 implies avg_sub_group_float_safe(db, p, gc) by {
            assert((|t: AvgSubTuple| if t.n2.value is Some { abs(t.n2.value->0) } else { 0 }) =~= avg_mag());
            lemma_float8_sum_safe_le(avg_sub_group_sum(db, p, gc, |t: AvgSubTuple| if t.n2.value is Some { abs(t.n2.value->0) } else { 0 }), ia);
        }
    }
    assert(groups_float_safe(db, p)) by {
        assert forall|gn: Option<Seq<u8>>, gc: Option<int>| #[trigger] group_count(db, p, gn, gc) > 0 implies group_float_safe(db, p, gn, gc) by {
            assert((|t: Tuple| if t.n.value is Some { abs(t.n.value->0) } else { 0 }) =~= tup_mag());
            lemma_float8_sum_safe_le(group_sum(db, p, gn, gc, |t: Tuple| if t.n.value is Some { abs(t.n.value->0) } else { 0 }), ia);
        }
    }
}

pub proof fn lemma_tot_unsafe(db: Db, p: Params, ia: int, n: int)
    requires totals_bound(db, p, ia, n), !tot_ok(ia, n),
    ensures !defined(db, p),
{
    lemma_sum_ext_all(avg_sub_tuples(db, p), |t: AvgSubTuple| if t.n2.value is Some { abs(t.n2.value->0) } else { 0 }, avg_mag());
    lemma_sum_ext_all(avg_sub_tuples(db, p), |t: AvgSubTuple| if t.n2.value is Some { 1int } else { 0 }, avg_vs());
}

}
