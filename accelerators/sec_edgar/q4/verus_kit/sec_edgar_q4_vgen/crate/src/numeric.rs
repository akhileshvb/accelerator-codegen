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

/// The power table and the largest double, built once.
pub struct Consts {
    pub pows: Vec<Wide>,
    pub top: Wide,
}

pub open spec fn consts_ok(c: Consts) -> bool {
    &&& pows_ok(c.pows@)
    &&& c.top@ == (pow2(53) - 1) * pow2(2045)
}

pub fn consts_new() -> (c: Consts)
    ensures consts_ok(c),
{
    let pows = pows_new();
    let top = Wide::from_i128(0x1F_FFFF_FFFF_FFFFi128).mul(&pows[2045]);
    proof {
        assert(pow2(53) == 0x20_0000_0000_0000int) by { lemma_pow2_consts(); }
    }
    Consts { pows, top }
}

/// |x| <= the largest double (at the spec's scale).
pub fn in_range(x: &Wide, c: &Consts) -> (r: bool)
    requires consts_ok(*c),
    ensures r == f64_in_range(x@),
{
    let neg = x.neg();
    !c.top.lt(x) && !c.top.lt(&neg)
}

/// A value as the engine would hold it: bits when it is a double, else the exact value itself.
pub fn round_value(x: &Wide, c: &Consts) -> (r: (Wide, Option<u64>))
    requires consts_ok(*c),
    ensures
        match r.1 {
            Some(bits) => f64_rounded(bits, x@) && f64_finite(bits) && r.0@ == f64_val(bits),
            None => !f64_in_range(x@) && r.0@ == x@,
        },
{
    if in_range(x, c) {
        let bits = x.f64_bits();
        proof { lemma_f64_in_range(bits); }
        (wide_of_bits(bits, &c.pows), Some(bits))
    } else {
        (x.dup(), None)
    }
}

/// e / k rounded to an integer (halves away from zero).
pub fn div_round(e: &Wide, k: &Wide) -> (q: Wide)
    requires k@ >= 1,
    ensures
        2 * abs(q@ * k@ - e@) <= k@,
        abs(q@) <= abs(e@),
{
    let neg = e.is_neg();
    let m = if neg { e.neg() } else { e.dup() };
    let kw = k.dup();
    let mut ds: Vec<Wide> = Vec::new();
    ds.push(kw.dup());
    proof { assert(pow2(0) == 1); }
    while !m.lt(&ds[ds.len() - 1])
        invariant
            m@ >= 0,
            kw@ == k@,
            k@ >= 1,
            ds.len() >= 1,
            forall|j: int| 0 <= j < ds.len() ==> (#[trigger] ds[j])@ == k@ * pow2(j as nat),
        decreases if m@ >= ds[ds.len() - 1]@ { m@ - ds[ds.len() - 1]@ + 1 } else { 0int },
    {
        let last = &ds[ds.len() - 1];
        let d = last.add(last);
        proof {
            let j = (ds.len() - 1) as nat;
            assert(pow2(j + 1) == 2 * pow2(j)) by { reveal_with_fuel(pow2, 2); }
            lemma_pow2_pos(j);
            assert(last@ >= 1) by (nonlinear_arith) requires last@ == k@ * pow2(j), k@ >= 1, pow2(j) >= 1;
            assert(d@ == k@ * pow2((j + 1) as nat)) by (nonlinear_arith)
                requires d@ == last@ + last@, last@ == k@ * pow2(j), pow2((j + 1) as nat) == 2 * pow2(j);
        }
        ds.push(d);
    }
    let mut q = Wide::zero();
    let mut r = m.dup();
    let one = Wide::from_i128(1);
    let mut i = ds.len();
    proof {
        let l = (ds.len() - 1) as nat;
        lemma_pow2_pos(l);
        assert(pow2(l + 1) == 2 * pow2(l)) by { reveal_with_fuel(pow2, 2); }
        assert(m@ < ds[l as int]@);
        assert(r@ < k@ * pow2(ds.len() as nat)) by (nonlinear_arith)
            requires r@ == m@, m@ < k@ * pow2(l), pow2((l + 1) as nat) == 2 * pow2(l), pow2(l) >= 1, k@ >= 1, ds.len() == l + 1;
    }
    while i > 0
        invariant
            0 <= i <= ds.len(),
            ds.len() >= 1,
            k@ >= 1,
            m@ >= 0,
            one@ == 1,
            forall|j: int| 0 <= j < ds.len() ==> (#[trigger] ds[j])@ == k@ * pow2(j as nat),
            m@ == q@ * (k@ * pow2(i as nat)) + r@,
            0 <= r@ < k@ * pow2(i as nat),
            q@ >= 0,
        decreases i,
    {
        i -= 1;
        proof {
            reveal_with_fuel(pow2, 2);
            assert(pow2((i + 1) as nat) == 2 * pow2(i as nat));
            lemma_pow2_pos(i as nat);
        }
        let ghost (q0, r0) = (q@, r@);
        let ghost kj = k@ * pow2(i as nat);
        let q2 = q.add(&q);
        if !r.lt(&ds[i]) {
            r = r.sub(&ds[i]);
            q = q2.add(&one);
            proof {
                assert(ds[i as int]@ == kj);
                assert(k@ * pow2((i + 1) as nat) == 2 * kj) by (nonlinear_arith) requires pow2((i + 1) as nat) == 2 * pow2(i as nat), kj == k@ * pow2(i as nat);
                assert(m@ == q@ * kj + r@) by (nonlinear_arith)
                    requires q@ == 2 * q0 + 1, r@ == r0 - kj, m@ == q0 * (2 * kj) + r0;
                assert(0 <= r@ < kj);
            }
        } else {
            q = q2;
            proof {
                assert(ds[i as int]@ == kj);
                assert(k@ * pow2((i + 1) as nat) == 2 * kj) by (nonlinear_arith) requires pow2((i + 1) as nat) == 2 * pow2(i as nat), kj == k@ * pow2(i as nat);
                assert(m@ == q@ * kj + r@) by (nonlinear_arith)
                    requires q@ == 2 * q0, r@ == r0, m@ == q0 * (2 * kj) + r0;
            }
        }
    }
    proof { assert(pow2(0) == 1); }
    let rem2 = r.add(&r);
    let up = !rem2.lt(&kw);
    let qq = if up { q.add(&one) } else { q.dup() };
    let out = if neg { qq.neg() } else { qq.dup() };
    proof {
        let (mm, ee, kk) = (m@, e@, k@);
        assert(mm == abs(ee));
        assert(mm == q@ * kk + r@) by { assert(pow2(0) == 1); }
        assert(0 <= r@ < kk);
        if up {
            assert(2 * abs(qq@ * kk - mm) <= kk) by (nonlinear_arith)
                requires qq@ == q@ + 1, mm == q@ * kk + r@, 0 <= r@ < kk, 2 * r@ >= kk;
        } else {
            assert(2 * abs(qq@ * kk - mm) <= kk) by (nonlinear_arith)
                requires qq@ == q@, mm == q@ * kk + r@, 0 <= r@ < kk, 2 * r@ < kk;
        }
        assert(qq@ >= 0);
        assert(qq@ <= mm) by (nonlinear_arith)
            requires qq@ >= 0, 2 * abs(qq@ * kk - mm) <= kk, kk >= 1, mm >= 0;
        if neg {
            assert(out@ == -qq@);
            assert(ee == -mm);
            assert(out@ * kk - ee == -(qq@ * kk - mm)) by (nonlinear_arith) requires out@ == -qq@, ee == -mm;
        } else {
            assert(out@ == qq@);
            assert(ee == mm);
        }
        assert(abs(out@) == qq@);
    }
    out
}

}
