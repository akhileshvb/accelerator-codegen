// A raw DOUBLE bit pattern as an exact integer, and an exact integer back to the bits of the double it is.
use vstd::prelude::*;
use vstd::arithmetic::div_mod::*;
use verus_common::rel::*;
use verus_prooflib::wide::Wide;
use verus_prooflib::float::signed;
use crate::f64bits::*;
use crate::numeric::*;

verus! {

/// v is the value of some bit pattern.
pub open spec fn dbl_raw(v: int) -> bool {
    exists|b: u64| f64_val(b) == v
}

/// The value of a bit pattern, NaN and infinity as 2^2098 like the spec.
pub fn wide_of_row_bits(bits: u64, t: &Vec<Wide>) -> (w: Wide)
    requires pows_ok(t@),
    ensures w@ == f64_val(bits),
{
    proof { lemma_pow2_consts(); }
    let e = (bits / 0x10_0000_0000_0000u64) % 2048u64;
    proof { assert(e as int == f64_exp(bits)); }
    if e == 2047 {
        t[2098].dup()
    } else {
        wide_of_bits(bits, t)
    }
}

proof fn lemma_decode_fields(sign: int, e: int, frac: int)
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

/// What a finite double's value is made of.
proof fn lemma_fields(b: u64)
    requires f64_finite(b),
    ensures
        0 <= f64_exp(b) <= 2046,
        0 <= f64_frac(b) < 0x10_0000_0000_0000,
        f64_val(b) == signed(b as int / pow2(63) == 1, f64_mag(b)),
        f64_mag(b) >= 0,
        f64_exp(b) == 0 ==> f64_mag(b) == f64_frac(b),
        f64_exp(b) != 0 ==> f64_mag(b) == (f64_frac(b) + 0x10_0000_0000_0000) * pow2((f64_exp(b) - 1) as nat),
{
    lemma_pow2_consts();
    lemma_mod_bound(b as int / pow2(52), 2048);
    lemma_mod_bound(b as int, pow2(52));
    if f64_exp(b) != 0 {
        lemma_pow2_pos((f64_exp(b) - 1) as nat);
        let (f, p) = (f64_frac(b), pow2((f64_exp(b) - 1) as nat));
        assert((f + 0x10_0000_0000_0000) * p >= 0) by (nonlinear_arith) requires f >= 0, p > 0;
    }
}

/// The bits of the double an exact integer is.
pub fn exact_bits(x: &Wide, c: &Consts) -> (r: u64)
    requires
        consts_ok(*c),
        f64_in_range(x@),
        dbl_raw(x@),
    ensures
        f64_val(r) == x@,
{
    proof { lemma_pow2_consts(); }
    let ghost b0 = choose|b: u64| f64_val(b) == x@;
    proof {
        lemma_f64_in_range(b0);
        lemma_fields(b0);
    }
    let ghost (e0, frac0) = (f64_exp(b0), f64_frac(b0));
    let ghost mag0 = f64_mag(b0);
    let neg = x.is_neg();
    let a = if neg { x.neg() } else { x.dup() };
    let sbit: u64 = if neg { 0x8000_0000_0000_0000 } else { 0 };
    proof {
        assert(a@ == mag0) by {
            if b0 as int / pow2(63) == 1 { assert(x@ == -mag0); } else { assert(x@ == mag0); }
        }
        assert(neg == (x@ < 0));
        assert(signed(neg, a@) == x@);
    }
    if a.lt(&c.pows[52]) {
        let f = match a.to_i128() { Some(v) => v, None => 0 };
        proof {
            assert(pow2(52) == 4503599627370496int) by { lemma_pow2_consts(); }
            assert(a@ < pow2(52));
            assert(f == a@);
            assert(e0 == 0) by {
                if e0 != 0 {
                    lemma_pow2_pos((e0 - 1) as nat);
                    let p = pow2((e0 - 1) as nat);
                    assert((frac0 + 0x10_0000_0000_0000) * p >= 0x10_0000_0000_0000) by (nonlinear_arith) requires frac0 >= 0, p >= 1;
                }
            }
        }
        let fu = f as u64;
        let r = sbit + fu;
        proof {
            let s: int = if neg { 1 } else { 0 };
            lemma_decode_fields(s, 0, fu as int);
            assert(r as int == s * 0x8000_0000_0000_0000 + 0 * 0x10_0000_0000_0000 + fu as int);
            assert(f64_val(r) == signed(neg, fu as int));
        }
        return r;
    }
    proof {
        assert(a@ >= pow2(52));
        if e0 == 0 { assert(a@ == frac0); assert(false); }
        assert(e0 >= 1);
        lemma_pow2_add(53, (e0 - 1) as nat);
        lemma_pow2_add(52, (e0 - 1) as nat);
        lemma_pow2_pos((e0 - 1) as nat);
        let p = pow2((e0 - 1) as nat);
        assert(mag0 < pow2((e0 + 52) as nat)) by (nonlinear_arith)
            requires mag0 == (frac0 + 0x10_0000_0000_0000) * p, frac0 < 0x10_0000_0000_0000, p > 0,
                pow2((e0 + 52) as nat) == pow2(53) * p, pow2(53) == 9007199254740992;
        assert(mag0 >= pow2((e0 + 51) as nat)) by (nonlinear_arith)
            requires mag0 == (frac0 + 0x10_0000_0000_0000) * p, frac0 >= 0, p > 0,
                pow2((e0 + 51) as nat) == pow2(52) * p, pow2(52) == 4503599627370496;
    }
    let mut e: usize = 1;
    while !a.lt(&c.pows[e + 52])
        invariant
            consts_ok(*c),
            1 <= e <= e0 <= 2046,
            a@ == mag0,
            mag0 >= pow2((e + 51) as nat),
            mag0 < pow2((e0 + 52) as nat),
        decreases e0 - e,
    {
        proof {
            if e == e0 { assert(false); }
            lemma_pow2_mono((e + 52) as nat, (e + 52) as nat);
        }
        e += 1;
        proof { assert(a@ >= pow2((e + 51) as nat)); }
    }
    proof {
        assert(e == e0) by {
            if e < e0 {
                lemma_pow2_mono((e + 52) as nat, (e0 + 51) as nat);
                assert(mag0 >= pow2((e0 + 51) as nat)) by {
                    lemma_pow2_add(52, (e0 - 1) as nat);
                    lemma_pow2_pos((e0 - 1) as nat);
                    let p = pow2((e0 - 1) as nat);
                    assert(mag0 >= pow2((e0 + 51) as nat)) by (nonlinear_arith)
                        requires mag0 == (frac0 + 0x10_0000_0000_0000) * p, frac0 >= 0, p > 0,
                            pow2((e0 + 51) as nat) == pow2(52) * p, pow2(52) == 4503599627370496;
                }
                assert(false);
            }
        }
    }
    let ghost m0 = frac0 + 0x10_0000_0000_0000;
    let p = &c.pows[e - 1];
    proof {
        assert(p@ == pow2((e - 1) as nat));
        assert(a@ == m0 * p@);
        lemma_pow2_pos((e - 1) as nat);
    }
    let mut lo: u64 = 0x10_0000_0000_0000;
    let mut hi: u64 = 0x1F_FFFF_FFFF_FFFF;
    while lo < hi
        invariant
            0x10_0000_0000_0000 <= lo <= m0 <= hi <= 0x1F_FFFF_FFFF_FFFF,
            m0 == frac0 + 0x10_0000_0000_0000,
            p@ == pow2((e - 1) as nat),
            p@ > 0,
            a@ == m0 * p@,
        decreases hi - lo,
    {
        let mid = lo + (hi - lo) / 2;
        let t = Wide::from_i128(mid as i128).mul(p);
        proof {
            let pp = p@;
            if t@ < a@ {
                assert(mid < m0) by (nonlinear_arith) requires t@ == mid as int * pp, a@ == m0 * pp, t@ < a@, pp > 0;
            } else {
                assert(mid >= m0) by (nonlinear_arith) requires t@ == mid as int * pp, a@ == m0 * pp, !(t@ < a@), pp > 0;
            }
        }
        if t.lt(&a) { lo = mid + 1; } else { hi = mid; }
    }
    let r = sbit + (e as u64) * 0x10_0000_0000_0000u64 + (lo - 0x10_0000_0000_0000u64);
    proof {
        let s: int = if neg { 1 } else { 0 };
        let fr = lo as int - 0x10_0000_0000_0000;
        lemma_decode_fields(s, e as int, fr);
        assert(r as int == s * 0x8000_0000_0000_0000 + e as int * 0x10_0000_0000_0000 + fr);
        assert(lo == m0);
        assert(fr == frac0);
        assert(f64_val(r) == signed(neg, (fr + 0x10_0000_0000_0000) * pow2((e as int - 1) as nat)));
        assert((fr + 0x10_0000_0000_0000) * pow2((e as int - 1) as nat) == a@);
    }
    r
}

}
