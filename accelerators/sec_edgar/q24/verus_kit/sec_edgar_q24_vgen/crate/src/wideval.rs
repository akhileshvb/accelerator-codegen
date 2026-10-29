// Exact values of IEEE doubles as `Wide` integers (the spec's f64_val), via a table of powers of two.
#![allow(unused_imports)]
use vstd::prelude::*;
use verus_common::rel::*;
use verus_prooflib::wide::*;
use crate::f64bits::*;

verus! {

/// pow2(i) for every i a finite double's exponent can shift by.
pub struct Pow2Tab {
    pub t: Vec<Wide>,
}

impl Pow2Tab {
    pub open spec fn ok(self) -> bool {
        self.t.len() == 2099 && forall|i: int| 0 <= i < 2099 ==> (#[trigger] self.t@[i])@ == pow2(i as nat)
    }
}

pub fn pow2_tab() -> (r: Pow2Tab)
    ensures r.ok(),
{
    let mut t: Vec<Wide> = Vec::new();
    t.push(Wide::from_i128(1));
    proof { reveal(pow2); }
    let mut i = 1usize;
    while i < 2099
        invariant
            1 <= i <= 2099,
            t.len() == i,
            forall|j: int| 0 <= j < i ==> (#[trigger] t@[j])@ == pow2(j as nat),
        decreases 2099 - i,
    {
        let next = t[i - 1].add(&t[i - 1]);
        proof {
            assert(pow2(i as nat) == 2 * pow2((i - 1) as nat)) by { reveal(pow2); }
        }
        t.push(next);
        i += 1;
    }
    Pow2Tab { t }
}

/// The exact value of a finite double's bits.
pub fn wide_of_bits(tab: &Pow2Tab, bits: u64) -> (r: Wide)
    requires
        tab.ok(),
    ensures
        r@ == f64_val(bits),
{
    proof { lemma_pow2_consts(); }
    let e = (bits / 4503599627370496) % 2048;
    let frac = bits % 4503599627370496;
    let neg = bits / 9223372036854775808 == 1;
    proof {
        assert(e as int == f64_exp(bits));
        assert(frac as int == f64_frac(bits));
        assert(bits as int / pow2(63) == (bits / 9223372036854775808) as int);
    }
    if e == 2047 {
        return tab.t[2098].dup();
    }
    let mag: Wide = if e == 0 {
        Wide::from_i128(frac as i128)
    } else {
        let m = Wide::from_i128((frac + 4503599627370496) as i128);
        m.mul(&tab.t[(e - 1) as usize])
    };
    if neg { mag.neg() } else { mag }
}

/// Setting the sign bit negates a non-negative finite double.
proof fn lemma_set_sign(lo: u64)
    requires f64_finite(lo), lo < 9223372036854775808,
    ensures
        f64_finite((lo + 9223372036854775808u64) as u64),
        f64_val((lo + 9223372036854775808u64) as u64) == -f64_val(lo),
{
    lemma_pow2_consts();
    lemma_val_pos(lo);
    let r = (lo + 9223372036854775808u64) as u64;
    let p52: int = 4503599627370496;
    let q = lo as int / p52;
    vstd::arithmetic::div_mod::lemma_fundamental_div_mod(lo as int, p52);
    vstd::arithmetic::div_mod::lemma_mod_bound(lo as int, p52);
    assert(r as int == p52 * (q + 2048) + lo as int % p52);
    vstd::arithmetic::div_mod::lemma_fundamental_div_mod_converse(r as int, p52, q + 2048, lo as int % p52);
    assert(r as int / p52 == q + 2048 && r as int % p52 == lo as int % p52);
    assert((q + 2048) % 2048 == q % 2048) by (nonlinear_arith);
    assert(f64_exp(r) == f64_exp(lo) && f64_frac(r) == f64_frac(lo));
    assert(r as int / pow2(63) == 1) by (nonlinear_arith)
        requires (r as int) >= pow2(63), (r as int) < 2 * pow2(63), pow2(63) > 0;
}

/// Bits whose value is e, found by bisection (e is the value of some finite double).
pub fn bits_exact(tab: &Pow2Tab, e: &Wide) -> (r: u64)
    requires
        tab.ok(),
        exists|x: u64| f64_finite(x) && f64_val(x) == e@,
    ensures
        f64_finite(r),
        f64_val(r) == e@,
{
    let neg = e.is_neg();
    let a = if neg { e.neg() } else { e.dup() };
    let ghost t0: u64 = {
        let x = choose|x: u64| f64_finite(x) && f64_val(x) == e@;
        lemma_clear_sign(x);
        (x % 9223372036854775808) as u64
    };
    proof {
        let x = choose|x: u64| f64_finite(x) && f64_val(x) == e@;
        lemma_clear_sign(x);
        lemma_val_pos(t0);
        assert(f64_val(t0) == f64_mag(t0) && f64_mag(t0) == f64_mag(x));
        assert(e@ == f64_val(x));
        lemma_mag_bound(x);
        if neg { assert(a@ == -e@); } else { assert(a@ == e@); }
        assert(a@ == f64_val(t0));
        lemma_pow2_consts();
        lemma_decompose(t0);
        // t0 <= 0x7FEF_FFFF_FFFF_FFFF: exponent field at most 2046
        assert(t0 as int == f64_exp(t0) * pow2(52) + f64_frac(t0));
        assert(f64_exp(t0) <= 2046);
        assert(t0 <= 0x7FEF_FFFF_FFFF_FFFFu64) by (nonlinear_arith)
            requires t0 as int == f64_exp(t0) * 4503599627370496 + f64_frac(t0), f64_exp(t0) <= 2046, 0 <= f64_frac(t0) < 4503599627370496;
    }
    let mut lo: u64 = 0;
    let mut hi: u64 = 0x7FEF_FFFF_FFFF_FFFF;
    while lo < hi
        invariant
            lo <= t0 <= hi, hi <= 0x7FEF_FFFF_FFFF_FFFFu64,
            tab.ok(), a@ == f64_val(t0), f64_finite(t0), t0 < 9223372036854775808,
        decreases hi - lo,
    {
        let mid = lo + (hi - lo) / 2;
        proof {
            lemma_pow2_consts();
            // every pattern up to hi is finite: exponent field at most 2046
            assert(f64_finite(mid) && f64_finite(lo) && f64_finite(hi)) by {
                assert forall|b: u64| b <= 0x7FEF_FFFF_FFFF_FFFFu64 implies f64_finite(b) by {
                    assert((b / 4503599627370496) % 2048 != 2047) by (nonlinear_arith)
                        requires b <= 0x7FEF_FFFF_FFFF_FFFFu64;
                    assert(f64_exp(b) == (b / 4503599627370496) % 2048);
                }
            }
        }
        let w = wide_of_bits(tab, mid);
        proof { lemma_f64_finite_below(mid); }
        if w.lt(&a) {
            proof {
                // value(mid) < value(t0) forces mid < t0
                if t0 <= mid {
                    if t0 < mid { lemma_val_mono(t0, mid); }
                }
            }
            lo = mid + 1;
        } else {
            proof {
                if t0 > mid { lemma_f64_finite_below(t0); lemma_val_mono(mid, t0); }
            }
            hi = mid;
        }
    }
    proof { lemma_f64_finite_below(lo); }
    if neg {
        proof { lemma_set_sign(lo); lemma_val_pos(lo); }
        (lo + 9223372036854775808u64) as u64
    } else {
        proof { lemma_val_pos(lo); }
        lo
    }
}

proof fn lemma_f64_finite_below(b: u64)
    requires b <= 0x7FEF_FFFF_FFFF_FFFFu64,
    ensures f64_finite(b),
{
    lemma_pow2_consts();
    assert((b / 4503599627370496) % 2048 != 2047) by (nonlinear_arith)
        requires b <= 0x7FEF_FFFF_FFFF_FFFFu64;
}


} // verus!
