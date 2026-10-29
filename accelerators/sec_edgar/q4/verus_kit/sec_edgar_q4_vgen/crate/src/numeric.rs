// DOUBLE cells as exact integers (the spec's scale, value * 2^1074) and back.
use vstd::prelude::*;
use verus_common::rel::*;
use verus_prooflib::wide::Wide;
use verus_prooflib::float::*;
use crate::f64bits::*;

verus! {

/// pows[j] = 2^j for every exponent a double (or its limit) uses.
pub open spec fn pows_ok(t: Seq<Wide>) -> bool {
    t.len() >= 2100 && forall|j: int| 0 <= j < 2100 ==> (#[trigger] t[j])@ == pow2(j as nat)
}

pub fn pows_new() -> (t: Vec<Wide>)
    ensures pows_ok(t@),
{
    let mut t: Vec<Wide> = Vec::new();
    t.push(Wide::from_i128(1));
    proof { assert(pow2(0) == 1); }
    while t.len() < 2100
        invariant
            1 <= t.len() <= 2100,
            forall|j: int| 0 <= j < t.len() ==> (#[trigger] t[j])@ == pow2(j as nat),
        decreases 2100 - t.len(),
    {
        let last = &t[t.len() - 1];
        let next = last.add(last);
        proof {
            let j = (t.len() - 1) as nat;
            assert(pow2(j + 1) == 2 * pow2(j)) by { reveal_with_fuel(pow2, 2); }
        }
        t.push(next);
    }
    t
}

/// The exact value of finite bits.
pub fn wide_of_bits(bits: u64, t: &Vec<Wide>) -> (w: Wide)
    requires
        pows_ok(t@),
        f64_finite(bits),
    ensures
        w@ == f64_val(bits),
{
    proof { lemma_pow2_consts(); }
    let frac = bits % 0x10_0000_0000_0000u64;
    let e = (bits / 0x10_0000_0000_0000u64) % 2048u64;
    let sign = bits / 0x8000_0000_0000_0000u64;
    proof {
        assert(frac as int == bits as int % pow2(52));
        assert(e as int == (bits as int / pow2(52)) % 2048);
        assert(sign as int == bits as int / pow2(63));
        assert(f64_exp(bits) == e as int);
    }
    let mant: u64 = if e == 0 { frac } else { frac + 0x10_0000_0000_0000u64 };
    let shift: usize = if e == 0 { 0 } else { (e - 1) as usize };
    proof {
        assert(shift < 2046);
        assert(mant < 0x20_0000_0000_0000u64);
        assert(e != 0 ==> shift as int == e as int - 1);
    }
    let signed: i128 = if sign == 1 { -(mant as i128) } else { mant as i128 };
    let v = Wide::from_i128(signed).mul(&t[shift]);
    proof {
        let mag = if e == 0 { frac as int } else { (frac as int + pow2(52)) * pow2((e as int - 1) as nat) };
        assert(pow2(0) == 1) by { reveal_with_fuel(pow2, 1); }
        assert(mag == mant as int * pow2(shift as nat));
        assert(f64_val(bits) == if sign == 1 { -mag } else { mag });
        assert(v@ == signed as int * pow2(shift as nat));
        assert(v@ == if sign == 1 { -mag } else { mag }) by (nonlinear_arith)
            requires
                v@ == signed as int * pow2(shift as nat),
                signed as int == if sign == 1 { -(mant as int) } else { mant as int },
                mag == mant as int * pow2(shift as nat);
    }
    v
}

/// |x| <= the largest double (at the spec's scale).
pub fn in_range(x: &Wide, t: &Vec<Wide>) -> (r: bool)
    requires pows_ok(t@),
    ensures r == f64_in_range(x@),
{
    let top = Wide::from_i128(0x1F_FFFF_FFFF_FFFFi128).mul(&t[2045]);
    proof {
        assert(pow2(53) == 0x20_0000_0000_0000int) by { lemma_pow2_consts(); }
        assert(top@ == (pow2(53) - 1) * pow2(2045));
    }
    let neg = x.neg();
    let above = top.lt(x);
    let below = x.lt(&neg.neg());
    let _ = below;
    !top.lt(x) && !top.lt(&neg)
}

}
