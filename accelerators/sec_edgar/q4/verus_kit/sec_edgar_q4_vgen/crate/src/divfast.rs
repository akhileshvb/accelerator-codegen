// Division of a Wide by a small count in about twenty steps: each step divides the leading 53 bits of the
// remainder, so the remainder shrinks by a factor of 2^50 per step.
use vstd::prelude::*;
use vstd::arithmetic::div_mod::*;
use verus_common::rel::*;
use verus_prooflib::float::*;
use verus_prooflib::wide::Wide;
use crate::f64bits::*;
use crate::numeric::*;

verus! {

/// The quotient of the leading bits of a double by k, at the scale of the double's last bit: within 2^-54
/// of the double, at the multiple k.
fn quot_step(bits: u64, kv: u64, pows: &Vec<Wide>) -> (delta: Wide)
    requires
        pows_ok(pows@),
        f64_in_range(f64_val(bits)),
        pow2(119) <= abs(f64_val(bits)),
        1 <= kv < 4611686018427387904,
    ensures
        pow2(54) * abs(f64_val(bits) - delta@ * kv as int) <= abs(f64_val(bits)),
{
    proof {
        lemma_pow2_consts();
        lemma_f64_in_range(bits);
    }
    let frac = bits % 0x10_0000_0000_0000u64;
    let e = (bits / 0x10_0000_0000_0000u64) % 2048u64;
    let sign = bits / 0x8000_0000_0000_0000u64;
    proof {
        assert(frac as int == bits as int % pow2(52));
        assert(e as int == (bits as int / pow2(52)) % 2048);
        assert(sign as int == bits as int / pow2(63));
        assert(f64_exp(bits) == e as int);
        assert(e != 2047);
    }
    let mant: u64 = frac + 0x10_0000_0000_0000u64;
    proof {
        // a denormal is below 2^52, so the double is normal
        if e == 0 {
            assert(f64_val(bits) == if sign == 1 { -(frac as int) } else { frac as int });
            lemma_pow2_mono(52, 119);
            assert(false);
        }
    }
    let sh: usize = (e - 1) as usize;
    let ghost mag = mant as int * pow2(sh as nat);
    proof {
        lemma_pow2_pos(sh as nat);
        assert(mag >= 0) by (nonlinear_arith) requires mag == mant as int * pow2(sh as nat), pow2(sh as nat) >= 1, mant >= 0;
        assert(f64_val(bits) == if sign == 1 { -mag } else { mag });
        assert(abs(f64_val(bits)) == mag);
        // sh >= 64
        if sh < 64 {
            lemma_pow2_mono(sh as nat, 66);
            lemma_pow2_pos(sh as nat);
            assert(mag < pow2(53) * pow2(66)) by (nonlinear_arith)
                requires mag == mant as int * pow2(sh as nat), mant < 0x20_0000_0000_0000u64, pow2(sh as nat) <= pow2(66), pow2(sh as nat) >= 1, pow2(53) == 0x20_0000_0000_0000int;
            lemma_pow2_add(53, 66);
            lemma_pow2_mono(119, 119);
            assert(false);
        }
    }
    let prod: u128 = (mant as u128) * 18446744073709551616u128;
    let m: u128 = prod / (kv as u128);
    proof {
        lemma_fundamental_div_mod(prod as int, kv as int);
        lemma_mod_bound(prod as int, kv as int);
        assert(m as int == prod as int / kv as int);
    }
    let rem: u128 = prod % (kv as u128);
    let d = Wide::from_i128(m as i128).mul(&pows[sh - 64]);
    let ghost jd = pow2((sh - 64) as nat);
    proof {
        lemma_pow2_add(64, (sh - 64) as nat);
        assert(pow2(sh as nat) == 18446744073709551616int * jd);
        assert(prod as int == m as int * kv as int + rem as int);
        assert(d@ == m as int * jd);
        // |delta*k| = M*k*2^(sh-64) <= mag, and mag - M*k*2^(sh-64) = rem * 2^(sh-64)
        assert(mag - d@ * kv as int == rem as int * jd) by (nonlinear_arith)
            requires
                mag == mant as int * pow2(sh as nat),
                pow2(sh as nat) == 18446744073709551616int * jd,
                prod as int == mant as int * 18446744073709551616int,
                prod as int == m as int * kv as int + rem as int,
                d@ == m as int * jd;
        // 2^54 * rem * jd <= 2^54 * 2^62 * jd = 2^116 * jd <= 2^52 * 2^64 * jd <= mant * pow2(sh)
        lemma_pow2_add(54, 62);
        lemma_pow2_add(52, 64);
        assert(pow2(54) * (rem as int * jd) <= mag) by (nonlinear_arith)
            requires
                (rem as int) < (kv as int), kv < 4611686018427387904u64, jd >= 1,
                pow2(54) * 4611686018427387904int == pow2(116),
                pow2(116) == pow2(52) * 18446744073709551616int,
                mag == mant as int * pow2(sh as nat),
                pow2(sh as nat) == 18446744073709551616int * jd,
                mant >= 0x10_0000_0000_0000u64,
                pow2(52) == 0x10_0000_0000_0000int,
                rem >= 0;
    }
    let signed = if sign == 1 { d.neg() } else { d };
    proof {
        if sign == 1 {
            assert(f64_val(bits) - signed@ * kv as int == -(mag - d@ * kv as int)) by (nonlinear_arith)
                requires f64_val(bits) == -mag, signed@ == -d@;
        } else {
            assert(f64_val(bits) - signed@ * kv as int == mag - d@ * kv as int);
        }
        assert(abs(f64_val(bits) - signed@ * kv as int) == rem as int * jd);
    }
    signed
}


fn at_least(x: &Wide, b: &Wide) -> (r: bool)
    ensures r == (abs(x@) >= b@),
{
    !x.lt(b) || !x.neg().lt(b)
}

proof fn lemma_quot_abs(q: int, e: int, k: int)
    requires 2 * abs(q * k - e) <= k, k >= 1,
    ensures abs(q) <= abs(e),
{
    assert(abs(q) <= abs(e)) by (nonlinear_arith)
        requires 2 * abs(q * k - e) <= k, k >= 1;
}

/// e / k rounded to an integer, as div_round, in about twenty steps when k is small and e is in the range of a double.
pub fn div_round_fast(e: &Wide, k: &Wide, c: &Consts) -> (q: Wide)
    requires consts_ok(*c), k@ >= 1,
    ensures
        2 * abs(q@ * k@ - e@) <= k@,
        abs(q@) <= abs(e@),
{
    let kopt = k.to_i128();
    let small = match kopt {
        Some(kv) => 1 <= kv && kv < 4611686018427387904i128,
        None => false,
    };
    if !small || !in_range(e, c) {
        return div_round(e, k);
    }
    let kv: i128 = kopt.unwrap();
    let kk: u64 = kv as u64;
    let kw = Wide::from_i128(kv);
    let bound = c.pows[120].dup();
    proof {
        lemma_pow2_consts();
        assert(bound@ == pow2(120));
    }
    let mut q = Wide::zero();
    let mut res = e.dup();
    while at_least(&res, &bound)
        invariant
            consts_ok(*c),
            kw@ == kv as int, k@ == kv as int, 1 <= kv < 4611686018427387904i128, kk as int == kv as int,
            bound@ == pow2(120),
            f64_in_range(e@),
            e@ == q@ * k@ + res@,
            abs(res@) <= abs(e@),
        decreases abs(res@),
    {
        proof {
            assert(f64_in_range(res@));
        }
        let bits = res.f64_bits();
        proof {
            lemma_pow2_consts();
            assert(pow2(53) * abs(f64_val(bits) - res@) <= abs(res@));
            assert(pow2(119) <= abs(f64_val(bits))) by (nonlinear_arith)
                requires
                    pow2(53) * abs(f64_val(bits) - res@) <= abs(res@),
                    pow2(53) == 0x20_0000_0000_0000int,
                    pow2(120) == 2 * pow2(119),
                    abs(res@) >= pow2(120);
        }
        let delta = quot_step(bits, kk, &c.pows);
        let q2 = q.add(&delta);
        let r2 = e.sub(&q2.mul(&kw));
        proof {
            let (d, rs) = (f64_val(bits), res@);
            assert(r2@ == rs - delta@ * kv as int) by (nonlinear_arith)
                requires r2@ == e@ - (q@ + delta@) * kv as int, e@ == q@ * kv as int + rs;
            assert(pow2(54) == 2 * pow2(53)) by { lemma_pow2_add(1, 53); reveal_with_fuel(pow2, 2); }
            lemma_pow2_pos(120);
            // |r2| <= |res - d| + |d - delta*k| <= |res|/2^53 + |d|/2^54 and |d| <= 2|res|
            assert(abs(r2@) < abs(rs)) by (nonlinear_arith)
                requires
                    r2@ == rs - delta@ * kv as int,
                    pow2(53) * abs(d - rs) <= abs(rs),
                    pow2(54) * abs(d - delta@ * kv as int) <= abs(d),
                    pow2(54) == 2 * pow2(53),
                    pow2(53) == 0x20_0000_0000_0000int,
                    abs(rs) >= pow2(120),
                    pow2(120) >= 1;
        }
        q = q2;
        res = r2;
    }
    proof {
        assert(abs(res@) < pow2(120));
        lemma_pow2_consts();
        assert(pow2(120) == 1329227995784915872903807060280344576int) by (compute_only);
    }
    let r: i128 = res.to_i128().unwrap();
    let neg = r < 0;
    let m: i128 = if neg { -r } else { r };
    proof { assert(m as int == abs(r as int)); assert(m < 1329227995784915872903807060280344576i128); }
    let a: i128 = (2 * m + kv) / (2 * kv);
    let adj: i128 = if neg { -a } else { a };
    let out = q.add(&Wide::from_i128(adj));
    proof {
        let (n, d) = (2 * m as int + kv as int, 2 * kv as int);
        lemma_fundamental_div_mod(n, d);
        lemma_mod_bound(n, d);
        assert(a as int == n / d);
        assert(n == d * a as int + n % d);
        assert(0 <= n % d < d);
        assert(d * a as int <= n) by (nonlinear_arith) requires n == d * a as int + n % d, n % d >= 0;
        assert(n < d * (a as int + 1)) by (nonlinear_arith) requires n == d * a as int + n % d, n % d < d;
        assert(2 * abs(out@ * k@ - e@) <= k@) by (nonlinear_arith)
            requires
                out@ == q@ + adj as int, e@ == q@ * k@ + res@, res@ == r as int, k@ == kv as int,
                m as int == abs(r as int), (neg ==> r < 0), (!neg ==> r >= 0),
                adj as int == (if neg { -(a as int) } else { a as int }),
                2 * kv as int * a as int <= (2 * m as int + kv as int),
                (2 * m as int + kv as int) < 2 * kv as int * (a as int + 1);
        lemma_quot_abs(out@, e@, k@);
    }
    out
}

}
