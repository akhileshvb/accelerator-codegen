// Exact integers (at the spec's float scale, x 2^1074) rounded to IEEE binary64 bits, rounding half up.
// `float_within` follows from a nearest rounding of the exact value (`lemma_rounded_within`).
use vstd::prelude::*;
use vstd::arithmetic::div_mod::*;
use verus_common::rel::*;
use crate::wide::*;

verus! {

pub open spec fn signed(neg: bool, m: int) -> int {
    if neg { -m } else { m }
}

/// bits decode to a finite double within half an ulp of e.
pub open spec fn f64_rounded(bits: u64, e: int) -> bool {
    &&& f64_in_range(f64_val(bits))
    &&& pow2(53) * abs(f64_val(bits) - e) <= abs(e)
}

/// A nearest rounding of an exact e is within the bound of any sum of absolute values a >= |e| of m >= 1 operations.
pub proof fn lemma_rounded_within(bits: u64, e: int, a: int, m: int)
    requires f64_rounded(bits, e), abs(e) <= a, m >= 1,
    ensures float_within(f64_val(bits), e, a, m),
{
    let (v, p) = (f64_val(bits), pow2(53));
    lemma_pow2_pos(53);
    if v != e && m < p {
        assert((p - m) * abs(v - e) <= m * a) by (nonlinear_arith)
            requires p * abs(v - e) <= abs(e), abs(e) <= a, m >= 1, m < p, abs(v - e) >= 0;
    }
}

proof fn lemma_pow2_consts()
    ensures
        pow2(52) == 0x10_0000_0000_0000,
        pow2(53) == 0x20_0000_0000_0000,
        pow2(63) == 0x8000_0000_0000_0000,
        pow2(64) == 0x1_0000_0000_0000_0000,
{
    reveal_with_fuel(pow2, 65);
}

proof fn lemma_pow2_pos(a: nat)
    ensures pow2(a) >= 1,
    decreases a,
{
    if a > 0 { lemma_pow2_pos((a - 1) as nat); }
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

proof fn lemma_pow2_add(a: nat, b: nat)
    ensures pow2(a + b) == pow2(a) * pow2(b),
    decreases a,
{
    if a > 0 {
        lemma_pow2_add((a - 1) as nat, b);
        let (x, y) = (pow2((a - 1) as nat), pow2(b));
        assert(2 * (x * y) == (2 * x) * y) by (nonlinear_arith);
    }
}

proof fn lemma_pw_pow2(k: nat)
    ensures pw(k) == pow2(64 * k),
    decreases k,
{
    if k > 0 {
        lemma_pw_pow2((k - 1) as nat);
        lemma_pow2_consts();
        lemma_pow2_add(64, (64 * (k - 1)) as nat);
    }
}

/// Decoding the encoding of sign, biased exponent e <= 2046 and fraction.
proof fn lemma_decode(sign: int, e: int, frac: int)
    requires 0 <= sign <= 1, 0 <= e <= 2046, 0 <= frac < 0x10_0000_0000_0000,
    ensures ({
        let bits = sign * 0x8000_0000_0000_0000 + e * 0x10_0000_0000_0000 + frac;
        &&& 0 <= bits < 0x1_0000_0000_0000_0000
        &&& f64_val(bits as u64) == signed(sign == 1, if e == 0 { frac } else { (frac + 0x10_0000_0000_0000) * pow2((e - 1) as nat) })
    }),
{
    lemma_pow2_consts();
    let bits = sign * 0x8000_0000_0000_0000 + e * 0x10_0000_0000_0000 + frac;
    let hi = sign * 2048 + e;
    let rest = e * 0x10_0000_0000_0000 + frac;
    assert(0 <= rest < 0x8000_0000_0000_0000 && bits == 0x10_0000_0000_0000 * hi + frac && 0 <= bits < 0x1_0000_0000_0000_0000)
        by (nonlinear_arith)
        requires 0 <= sign <= 1, 0 <= e <= 2046, 0 <= frac < 0x10_0000_0000_0000,
            hi == sign * 2048 + e, rest == e * 0x10_0000_0000_0000 + frac, bits == sign * 0x8000_0000_0000_0000 + rest;
    lemma_fundamental_div_mod_converse(bits, 0x10_0000_0000_0000, hi, frac);
    lemma_fundamental_div_mod_converse(hi, 2048, sign, e);
    lemma_fundamental_div_mod_converse(bits, 0x8000_0000_0000_0000, sign, rest);
}

/// Rounding half up the 53 bits above the low s >= 1 of m, whose top bit is at 52 + s: m = 2^s d + r, q = d + (r >= 2^(s-1)).
proof fn lemma_round(m: int, s: nat, d: int, r: int, q: int)
    requires
        pow2((52 + s) as nat) <= m < pow2((53 + s) as nat),
        s >= 1,
        m == pow2(s) * d + r,
        0 <= r < pow2(s),
        q == d + (if r >= pow2((s - 1) as nat) { 1int } else { 0int }),
    ensures
        pow2(52) <= q <= pow2(53),
        pow2(53) * abs(q * pow2(s) - m) <= m,
        q == pow2(53) ==> (pow2(53) - 1) * pow2(s) < m,
{
    lemma_pow2_add(52, s);
    lemma_pow2_add(53, s);
    lemma_pow2_add((s - 1) as nat, 1);
    lemma_pow2_consts();
    lemma_pow2_pos(s);
    lemma_pow2_pos((s - 1) as nat);
    let (ps, h, p52, p53) = (pow2(s), pow2((s - 1) as nat), pow2(52), pow2(53));
    assert(ps == 2 * h && p53 == 2 * p52);
    assert(p52 <= d && d < p53) by (nonlinear_arith)
        requires pow2((52 + s) as nat) == p52 * ps, pow2((53 + s) as nat) == p53 * ps,
            pow2((52 + s) as nat) <= m < pow2((53 + s) as nat), m == ps * d + r, 0 <= r < ps, ps >= 1;
    let err = abs(q * ps - m);
    if r >= h {
        assert(q * ps - m == ps - r) by (nonlinear_arith) requires q == d + 1, m == ps * d + r;
    } else {
        assert(q * ps - m == -r) by (nonlinear_arith) requires q == d, m == ps * d + r;
    }
    assert(err <= h && p53 * h == p52 * ps) by (nonlinear_arith) requires ps == 2 * h, p53 == 2 * p52, err == abs(q * ps - m), 0 <= r < ps, (q * ps - m == ps - r) || (q * ps - m == -r), (q * ps - m == ps - r) == (r >= h);
    assert(p53 * err <= p53 * h) by (nonlinear_arith) requires err <= h, p53 >= 1;
    assert(p53 * err <= m);
    if q == p53 {
        assert((p53 - 1) * ps < m) by (nonlinear_arith)
            requires d == p53 - 1, m == ps * d + r, r >= h, h >= 1;
    }
}

/// lemma_round for the top bits of m = w * 2^(64 lo) + rest, taken from the window w (so s >= 1 there).
proof fn lemma_window_round(w: int, rest: int, lo: nat, s: nat, q: int)
    requires
        pow2((52 + s) as nat) <= w < pow2((53 + s) as nat),
        s >= 1,
        0 <= rest < pw(lo),
        q == w / pow2(s) + (if w % pow2(s) >= pow2((s - 1) as nat) { 1int } else { 0int }),
    ensures ({
        let (m, t) = (w * pw(lo) + rest, (s + 64 * lo) as nat);
        &&& pow2((52 + t) as nat) <= m < pow2((53 + t) as nat)
        &&& pow2(52) <= q <= pow2(53)
        &&& pow2(53) * abs(q * pow2(t) - m) <= m
        &&& q == pow2(53) ==> (pow2(53) - 1) * pow2(t) < m
    }),
{
    lemma_pw_pow2(lo);
    lemma_pow2_add(s, (64 * lo) as nat);
    lemma_pow2_add((52 + s) as nat, (64 * lo) as nat);
    lemma_pow2_add((53 + s) as nat, (64 * lo) as nat);
    lemma_pow2_add((s - 1) as nat, (64 * lo) as nat);
    lemma_pow2_pos(s);
    lemma_pow2_pos((s - 1) as nat);
    lemma_fundamental_div_mod(w, pow2(s));
    let (ps, h, pl) = (pow2(s), pow2((s - 1) as nat), pw(lo));
    let (d, r) = (w / ps, w % ps);
    let rr = r * pl + rest;
    let t = (s + 64 * lo) as nat;
    assert(pow2(t) == ps * pl && pow2((t - 1) as nat) == h * pl);
    assert(w * pl <= w * pl + rest < (w + 1) * pl) by (nonlinear_arith) requires 0 <= rest < pl, w >= 0;
    assert(pow2((52 + s) as nat) * pl <= w * pl && (w + 1) * pl <= pow2((53 + s) as nat) * pl) by (nonlinear_arith)
        requires pow2((52 + s) as nat) <= w, w < pow2((53 + s) as nat), pl >= 1;
    assert(w * pl + rest == pow2(t) * d + rr && 0 <= rr < pow2(t)) by (nonlinear_arith)
        requires w == ps * d + r, 0 <= r < ps, 0 <= rest < pl, rr == r * pl + rest, pow2(t) == ps * pl, pl >= 1;
    assert((rr >= pow2((t - 1) as nat)) == (r >= h)) by (nonlinear_arith)
        requires 0 <= r, 0 <= rest < pl, rr == r * pl + rest, pow2((t - 1) as nat) == h * pl, pl >= 1;
    lemma_round(w * pl + rest, t, d, rr, q);
}

/// Magnitudes below 2^53 at an exponent t <= 2045 are finite doubles.
proof fn lemma_in_range(neg: bool, q: int, t: nat)
    requires 0 <= q < pow2(53), t <= 2045,
    ensures f64_in_range(signed(neg, q * pow2(t))),
{
    lemma_pow2_mono(t, 2045);
    lemma_pow2_pos(t);
    assert(q * pow2(t) <= (pow2(53) - 1) * pow2(2045)) by (nonlinear_arith)
        requires 0 <= q <= pow2(53) - 1, 1 <= pow2(t) <= pow2(2045);
}

/// w * 2^(64 lo) + rest, w the window of the top limbs (w >= 2^64 unless lo == 0, rest below the window),
/// rounded to the nearest double (ties up) as bits with the sign applied.
pub fn round_window(neg: bool, w: u128, lo: usize, rest: Ghost<int>) -> (bits: u64)
    requires
        lo == 0 || w >= 0x1_0000_0000_0000_0000u128,
        0 <= rest@ < pw(lo as nat),
        f64_in_range(w as int * pw(lo as nat) + rest@),
    ensures f64_rounded(bits, signed(neg, w as int * pw(lo as nat) + rest@)),
{
    proof { lemma_pow2_consts(); lemma_pw_pow2(lo as nat); lemma_pow2_add(53, 2045); lemma_pow2_pos(2045); }
    let ghost m = w as int * pw(lo as nat) + rest@;
    let ghost sg = if neg { 1int } else { 0int };
    let sign: u64 = if neg { 0x8000_0000_0000_0000u64 } else { 0u64 };
    if w < 0x10_0000_0000_0000u128 {
        proof {
            assert(lo == 0 && pw(0) == 1 && m == w as int);
            lemma_decode(sg, 0, w as int);
            lemma_in_range(neg, w as int, 0);
        }
        return sign + w as u64;
    }
    // k: 2^k <= w < 2^(k+1)
    let mut k: u32 = 0;
    let mut p: u128 = 1;
    proof { assert(pow2(0) == 1); }
    while p <= w / 2
        invariant p == pow2(k as nat), p <= w, k < 128,
        decreases w - p,
    {
        proof {
            lemma_pow2_pos(k as nat);
            lemma_pow2_consts();
            if k + 1 >= 128 {
                lemma_pow2_mono(128, (k + 1) as nat);
                lemma_pow2_add(64, 64);
                assert(pow2(128) == 0x1_0000_0000_0000_0000 * 0x1_0000_0000_0000_0000) by (nonlinear_arith)
                    requires pow2(128) == pow2(64) * pow2(64), pow2(64) == 0x1_0000_0000_0000_0000;
            }
        }
        p = p * 2;
        k = k + 1;
    }
    proof {
        assert(w < 2 * p && pow2((k + 1) as nat) == 2 * p);
        if k < 52 { lemma_pow2_mono((k + 1) as nat, 52); }
        if lo > 0 {
            assert(k >= 64) by { if k < 64 { lemma_pow2_mono((k + 1) as nat, 64); } }
            lemma_pow2_pos((64 * lo) as nat);
        }
        // m < 2^2098 bounds the window's position
        assert(m < pow2(2098) && pw(lo as nat) <= m) by (nonlinear_arith)
            requires f64_in_range(m), pow2(2098) == pow2(53) * pow2(2045), pow2(2045) >= 1, pow2(53) >= 2,
                m == w as int * pw(lo as nat) + rest@, w >= 1, pw(lo as nat) >= 1, rest@ >= 0;
        if 64 * lo >= 2098 { lemma_pow2_mono(2098, (64 * lo) as nat); }
    }
    let s: u32 = k - 52;
    let t: usize = s as usize + 64 * lo;
    proof { lemma_pow2_add(52, s as nat); lemma_pow2_add(53, s as nat); }
    let q: u128 = if s == 0 {
        w
    } else {
        let ps: u128 = p / 0x10_0000_0000_0000u128;
        let (d, r, half) = (w / ps, w % ps, ps / 2);
        proof {
            assert(ps == pow2(s as nat));
            lemma_pow2_add((s - 1) as nat, 1);
            lemma_window_round(w as int, rest@, lo as nat, s as nat, d as int + if r >= half { 1int } else { 0int });
        }
        if r >= half { d + 1 } else { d }
    };
    let carry = q == 0x20_0000_0000_0000u128;
    let (q3, t3): (u128, usize) = if carry { (0x10_0000_0000_0000u128, t + 1) } else { (q, t) };
    proof {
        if s == 0 {
            assert(lo == 0 && t == 0 && q as int == m);
            lemma_pow2_add(52, 0);
        } else {
            if 52 + t >= 2098 { lemma_pow2_mono(2098, (52 + t) as nat); }
            if carry && t == 2045 { assert((pow2(53) - 1) * pow2(2045) < m); }
        }
        lemma_pow2_add(1, t as nat);
    }
    let frac = (q3 - 0x10_0000_0000_0000u128) as u64;
    let e = (t3 + 1) as u64;
    let bits = sign + e * 0x10_0000_0000_0000u64 + frac;
    proof {
        lemma_decode(sg, e as int, frac as int);
        let v = q3 as int * pow2(t3 as nat);
        assert(v == q as int * pow2(t as nat)) by (nonlinear_arith)
            requires carry ==> (q3 == 0x10_0000_0000_0000 && q == 0x20_0000_0000_0000 && pow2(t3 as nat) == 2 * pow2(t as nat)),
                !carry ==> (q3 == q && t3 == t), v == q3 as int * pow2(t3 as nat);
        lemma_in_range(neg, q3 as int, t3 as nat);
        assert(abs(signed(neg, v) - signed(neg, m)) == abs(v - m));
    }
    bits
}

} // verus!
