use vstd::prelude::*;
use vstd::arithmetic::div_mod::*;
use verus_common::rel::*;

// IEEE binary64 bits against the spec's exact value (f64_val): which bit patterns are
// in range, and when two have the same value.

verus! {

pub proof fn lemma_pow2_pos(n: nat)
    ensures pow2(n) > 0,
    decreases n,
{
    if n > 0 { lemma_pow2_pos((n - 1) as nat); }
}

pub proof fn lemma_pow2_add(a: nat, b: nat)
    ensures pow2(a + b) == pow2(a) * pow2(b),
    decreases a,
{
    if a > 0 {
        lemma_pow2_add((a - 1) as nat, b);
        let x = pow2((a - 1) as nat);
        let y = pow2(b);
        assert(2 * (x * y) == (2 * x) * y) by (nonlinear_arith);
    }
}

pub proof fn lemma_pow2_mono(a: nat, b: nat)
    requires a <= b,
    ensures pow2(a) <= pow2(b),
{
    lemma_pow2_add(a, (b - a) as nat);
    lemma_pow2_pos((b - a) as nat);
    lemma_pow2_pos(a);
    let x = pow2(a);
    let y = pow2((b - a) as nat);
    assert(x * y >= x) by (nonlinear_arith) requires x > 0, y >= 1;
}

pub proof fn lemma_pow2_consts()
    ensures pow2(52) == 4503599627370496, pow2(53) == 9007199254740992, pow2(63) == 9223372036854775808,
{
    assert(pow2(52) == 4503599627370496) by (compute_only);
    assert(pow2(53) == 9007199254740992) by (compute_only);
    assert(pow2(63) == 9223372036854775808) by (compute_only);
}

/// The biased exponent field of IEEE binary64 bits.
pub open spec fn f64_exp(b: u64) -> int {
    (b as int / pow2(52)) % 2048
}

pub open spec fn f64_frac(b: u64) -> int {
    b as int % pow2(52)
}

/// Exponent and fraction without the sign.
pub open spec fn f64_mag_bits(b: u64) -> int {
    b as int % pow2(63)
}

pub open spec fn f64_finite(b: u64) -> bool {
    f64_exp(b) != 2047
}

/// The magnitude of a finite double, at the spec's scale.
pub open spec fn f64_mag(b: u64) -> int {
    if f64_exp(b) == 0 { f64_frac(b) } else { (f64_frac(b) + pow2(52)) * pow2((f64_exp(b) - 1) as nat) }
}

/// A double's bits are its sign, exponent and fraction fields.
pub proof fn lemma_decompose(b: u64)
    ensures
        b as int == (b as int / pow2(63)) * pow2(63) + f64_mag_bits(b),
        f64_mag_bits(b) == f64_exp(b) * pow2(52) + f64_frac(b),
        0 <= b as int / pow2(63) <= 1,
{
    lemma_pow2_consts();
    let p52: int = 4503599627370496;
    let x = b as int;
    lemma_fundamental_div_mod(x, p52);
    let q = x / p52;
    lemma_fundamental_div_mod(q, 2048);
    lemma_div_denominator(x, p52, 2048);
    assert(pow2(63) == p52 * 2048);
    let s = q / 2048;
    assert(x == p52 * (2048 * s + q % 2048) + x % p52) by (nonlinear_arith)
        requires x == p52 * q + x % p52, q == 2048 * s + q % 2048;
    assert(x == (p52 * 2048) * s + ((q % 2048) * p52 + x % p52)) by (nonlinear_arith)
        requires x == p52 * (2048 * s + q % 2048) + x % p52;
    lemma_mod_bound(x, p52);
    lemma_mod_bound(q, 2048);
    assert(0 <= (q % 2048) * p52 + x % p52 < p52 * 2048) by (nonlinear_arith)
        requires 0 <= q % 2048 < 2048, 0 <= x % p52 < p52;
    lemma_fundamental_div_mod_converse(x, p52 * 2048, s, (q % 2048) * p52 + x % p52);
    assert(x / pow2(63) == s);
}

proof fn lemma_mag_bound(b: u64)
    requires
        f64_finite(b),
    ensures
        0 <= f64_mag(b) <= (pow2(53) - 1) * pow2(2045),
{
    lemma_pow2_consts();
    lemma_decompose(b);
    lemma_pow2_pos(2045);
    let e = f64_exp(b);
    let m = f64_frac(b) + pow2(52);
    if e != 0 {
        lemma_pow2_mono((e - 1) as nat, 2045);
        lemma_pow2_pos((e - 1) as nat);
        let w = pow2((e - 1) as nat);
        let top = pow2(2045);
        assert(m * w <= (pow2(53) - 1) * top) by (nonlinear_arith)
            requires 0 < m <= pow2(53) - 1, 0 < w <= top;
    } else {
        assert(f64_frac(b) <= (pow2(53) - 1) * pow2(2045)) by (nonlinear_arith)
            requires 0 <= f64_frac(b) < pow2(52), pow2(2045) >= 1, pow2(53) == 2 * pow2(52);
    }
}

/// Finite doubles are in range; NaN and infinity are not.
pub proof fn lemma_f64_in_range(b: u64)
    ensures
        f64_in_range(f64_val(b)) <==> f64_finite(b),
{
    lemma_pow2_pos(2045);
    if f64_finite(b) {
        lemma_mag_bound(b);
    } else {
        lemma_pow2_add(53, 2045);
        assert(pow2(2098) > (pow2(53) - 1) * pow2(2045)) by (nonlinear_arith)
            requires pow2(2098) == pow2(53) * pow2(2045), pow2(2045) > 0;
    }
}

/// Two normal numbers of different exponents differ.
proof fn lemma_exp_order(e1: nat, e2: nat, m1: int, m2: int)
    requires
        e1 < e2,
        pow2(52) <= m1 < pow2(53),
        pow2(52) <= m2 < pow2(53),
    ensures
        m1 * pow2(e1) != m2 * pow2(e2),
{
    lemma_pow2_consts();
    lemma_pow2_add(e1, (e2 - e1) as nat);
    assert(e1 + (e2 - e1) as nat == e2);
    lemma_pow2_pos(e1);
    lemma_pow2_mono(1, (e2 - e1) as nat);
    reveal_with_fuel(pow2, 2);
    let w = pow2(e1);
    let k = pow2((e2 - e1) as nat);
    assert(m2 * k >= 9007199254740992) by (nonlinear_arith)
        requires m2 >= 4503599627370496, k >= 2;
    assert(m1 * w < (m2 * k) * w) by (nonlinear_arith)
        requires m1 < m2 * k, w > 0;
    assert((m2 * k) * w == m2 * (k * w)) by (nonlinear_arith);
}

/// Distinct finite doubles of the same sign and magnitude fields are the same double.
proof fn lemma_mag_inj(a: u64, b: u64)
    requires
        f64_finite(a),
        f64_finite(b),
        f64_mag(a) == f64_mag(b),
    ensures
        f64_mag_bits(a) == f64_mag_bits(b),
{
    lemma_pow2_consts();
    lemma_decompose(a);
    lemma_decompose(b);
    let (ea, eb) = (f64_exp(a), f64_exp(b));
    let (ma, mb) = (f64_frac(a) + pow2(52), f64_frac(b) + pow2(52));
    if ea != 0 && eb != 0 {
        if ea < eb {
            lemma_exp_order((ea - 1) as nat, (eb - 1) as nat, ma, mb);
        } else if eb < ea {
            lemma_exp_order((eb - 1) as nat, (ea - 1) as nat, mb, ma);
        } else {
            lemma_pow2_pos((ea - 1) as nat);
            let w = pow2((ea - 1) as nat);
            assert(ma == mb) by (nonlinear_arith)
                requires ma * w == mb * w, w > 0;
        }
    } else if ea != 0 {
        lemma_pow2_mono(0, (ea - 1) as nat);
        assert(ma * pow2((ea - 1) as nat) >= ma) by (nonlinear_arith)
            requires pow2((ea - 1) as nat) >= 1, ma > 0;
    } else if eb != 0 {
        lemma_pow2_mono(0, (eb - 1) as nat);
        assert(mb * pow2((eb - 1) as nat) >= mb) by (nonlinear_arith)
            requires pow2((eb - 1) as nat) >= 1, mb > 0;
    }
}

/// The finite double's value is zero exactly when its exponent and fraction are.
proof fn lemma_mag_zero(b: u64)
    requires
        f64_finite(b),
    ensures
        (f64_mag(b) == 0) <==> (f64_mag_bits(b) == 0),
{
    lemma_pow2_consts();
    lemma_decompose(b);
    if f64_exp(b) != 0 {
        lemma_pow2_pos((f64_exp(b) - 1) as nat);
        let w = pow2((f64_exp(b) - 1) as nat);
        let m = f64_frac(b) + pow2(52);
        assert(m * w > 0) by (nonlinear_arith)
            requires m > 0, w > 0;
    }
}

/// Finite doubles have the same value exactly when their bits agree, or both are a zero.
pub proof fn lemma_f64_val_eq(a: u64, b: u64)
    requires
        f64_finite(a),
        f64_finite(b),
    ensures
        (f64_val(a) == f64_val(b)) <==> (a == b || (f64_mag_bits(a) == 0 && f64_mag_bits(b) == 0)),
{
    lemma_decompose(a);
    lemma_decompose(b);
    lemma_mag_zero(a);
    lemma_mag_zero(b);
    lemma_mag_bound(a);
    lemma_mag_bound(b);
    if f64_val(a) == f64_val(b) && f64_mag(a) != 0 {
        assert(a as int / pow2(63) == b as int / pow2(63));
        lemma_mag_inj(a, b);
    }
}

}
