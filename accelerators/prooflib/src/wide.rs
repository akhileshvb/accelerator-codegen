// Exact 256-bit signed integers (hi * 2^128 + lo).  Use them for sums and products that can exceed
// i128 (many rows times large per-row values); the view `int()` is the exact mathematical value.
use vstd::prelude::*;

verus! {

pub open spec fn wide_limit() -> int { (u128::MAX as int + 1) * (u128::MAX as int + 1) / 4 }

#[derive(Clone, Copy)]
pub struct Wide {
    pub hi: i128,
    pub lo: u128,
}

impl Wide {
    pub open spec fn int(self) -> int {
        self.hi as int * (u128::MAX as int + 1) + self.lo as int
    }

    /// Use as the precondition of `add`/`sub`/`neg`: the value is at most 2^254 in magnitude, far inside the range the operations are proved for.
    pub open spec fn tame(self) -> bool {
        -wide_limit() <= self.int() <= wide_limit()
    }
}

/// The Wide holding the integer `x` (for 0 <= x < 2^128 or small negatives).
pub open spec fn wide_of(x: int) -> Wide {
    Wide { hi: if x < 0 { -1i128 } else { 0i128 }, lo: (x % (u128::MAX as int + 1)) as u128 }
}

/// Use to lift a per-row i128 value into a Wide accumulator.
pub fn from_i128(x: i128) -> (result: Wide)
    ensures result.int() == x as int,
{
    if x < 0 { Wide { hi: -1, lo: (x - i128::MIN) as u128 + 0x8000_0000_0000_0000_0000_0000_0000_0000u128 } }
    else { Wide { hi: 0, lo: x as u128 } }
}

/// The empty sum.
pub fn zero() -> (result: Wide)
    ensures result.int() == 0,
{
    Wide { hi: 0, lo: 0 }
}

/// Exact sum; the caller proves the result stays within `wide_limit`.
pub fn add(a: Wide, b: Wide) -> (result: Wide)
    requires -wide_limit() <= a.int() + b.int() <= wide_limit(),
    ensures result.int() == a.int() + b.int(),
{
    if a.lo > u128::MAX - b.lo {
        Wide { hi: a.hi + b.hi + 1, lo: a.lo - (u128::MAX - b.lo) - 1 }
    } else {
        Wide { hi: a.hi + b.hi, lo: a.lo + b.lo }
    }
}

/// Exact comparison.
pub fn lt(a: Wide, b: Wide) -> (result: bool)
    ensures result == (a.int() < b.int()),
{
    a.hi < b.hi || (a.hi == b.hi && a.lo < b.lo)
}

/// Exact equality.
pub fn eq(a: Wide, b: Wide) -> (result: bool)
    ensures result == (a.int() == b.int()),
{
    a.hi == b.hi && a.lo == b.lo
}

/// Narrow to i128 when the exact value fits; None says it does not.
pub fn to_i128(a: Wide) -> (result: Option<i128>)
    ensures
        result matches Some(value) ==> value as int == a.int(),
        result is None ==> a.int() < i128::MIN as int || a.int() > i128::MAX as int,
{
    if a.hi == 0 {
        if a.lo <= i128::MAX as u128 { Some(a.lo as i128) } else { None }
    } else if a.hi == -1 {
        if a.lo >= 0x8000_0000_0000_0000_0000_0000_0000_0000u128 {
            Some((a.lo - 0x8000_0000_0000_0000_0000_0000_0000_0000u128) as i128 + i128::MIN)
        } else { None }
    } else { None }
}

proof fn lemma_mul_le(a: int, b: int, c: int, d: int)
    requires 0 <= a <= c, 0 <= b <= d,
    ensures a * b <= c * d,
{
    assert(a * b <= c * b) by (nonlinear_arith) requires 0 <= a <= c, 0 <= b;
    assert(c * b <= c * d) by (nonlinear_arith) requires 0 <= b <= d, 0 <= c;
}

/// Building block of `mul`: x * y as a 256-bit pair, for y below 2^100.
pub fn mul_wide(x: u128, y: u128) -> (result: (u128, u128))
    requires y < 0x10_0000_0000_0000_0000_0000_0000u128,
    ensures
        result.0 as int * (u128::MAX as int + 1) + result.1 as int == x as int * y as int,
        result.0 < y || y == 0,
{
    let b: u128 = 0x1_0000_0000_0000_0000;
    let x0 = x % b; let x1 = x / b;
    let y0 = y % b; let y1 = y / b;
    proof {
        lemma_mul_le(x0 as int, y0 as int, (u64::MAX as int + 1) - 1, (u64::MAX as int + 1) - 1);
        lemma_mul_le(x0 as int, y1 as int, (u64::MAX as int + 1) - 1, 68719476735);
        lemma_mul_le(x1 as int, y0 as int, (u64::MAX as int + 1) - 1, (u64::MAX as int + 1) - 1);
        lemma_mul_le(x1 as int, y1 as int, (u64::MAX as int + 1) - 1, 68719476735);
    }
    let ll = x0 * y0; let lh = x0 * y1; let hl = x1 * y0; let hh = x1 * y1;
    let cross = ll / b + lh % b + hl % b;
    let lo = (cross % b) * b + ll % b;
    let hi = hh + lh / b + hl / b + cross / b;
    proof {
        let (xi, yi, bi) = (x as int, y as int, (u64::MAX as int + 1));
        assert(xi == x1 as int * bi + x0 as int);
        assert(yi == y1 as int * bi + y0 as int);
        assert(xi * yi == hh as int * (bi * bi) + (lh as int + hl as int) * bi + ll as int) by (nonlinear_arith)
            requires xi == x1 as int * bi + x0 as int, yi == y1 as int * bi + y0 as int,
                hh as int == x1 as int * y1 as int, lh as int == x0 as int * y1 as int,
                hl as int == x1 as int * y0 as int, ll as int == x0 as int * y0 as int;
        assert(bi * bi == (u128::MAX as int + 1));
        let (llh, lll) = (ll as int / bi, ll as int % bi);
        let (lhh, lhl) = (lh as int / bi, lh as int % bi);
        let (hlh, hll) = (hl as int / bi, hl as int % bi);
        assert(ll as int == llh * bi + lll);
        assert(lh as int == lhh * bi + lhl);
        assert(hl as int == hlh * bi + hll);
        assert(cross as int == cross as int / bi * bi + cross as int % bi);
        assert(hi as int * (u128::MAX as int + 1) + lo as int == xi * yi) by (nonlinear_arith)
            requires
                xi * yi == hh as int * (bi * bi) + (lh as int + hl as int) * bi + ll as int,
                bi * bi == (u128::MAX as int + 1),
                ll as int == llh * bi + lll, lh as int == lhh * bi + lhl, hl as int == hlh * bi + hll,
                cross as int == llh + lhl + hll,
                cross as int == cross as int / bi * bi + cross as int % bi,
                hi as int == hh as int + lhh + hlh + cross as int / bi,
                lo as int == (cross as int % bi) * bi + lll;
        if y != 0 {
            assert(xi * yi < (u128::MAX as int + 1) * yi) by (nonlinear_arith)
                requires xi < u128::MAX as int + 1, yi > 0;
            assert(hi < y);
        }
    }
    (hi, lo)
}

/// Exact product with a nonnegative multiplier w below 2^100, for a with a high word within 2^21 (|a| < 2^149).
pub fn mul(a: Wide, w: u128) -> (result: Wide)
    requires
        -0x20_0000 <= a.hi <= 0x20_0000,
        w < 0x10_0000_0000_0000_0000_0000_0000u128,
    ensures result.int() == a.int() * w as int,
{
    let (carry, lo) = mul_wide(a.lo, w);
    proof {
        let (h, v) = (a.hi as int, w as int);
        assert(-0x2000_0000_0000_0000_0000_0000_0000_0000int < h * v < 0x2000_0000_0000_0000_0000_0000_0000_0000int) by (nonlinear_arith)
            requires -0x200000int <= h <= 0x200000int, 0 <= v < 0x10_0000_0000_0000_0000_0000_0000int;
        assert(a.int() * v == (h * v + carry as int) * (u128::MAX as int + 1) + lo as int) by (nonlinear_arith)
            requires a.int() == h * (u128::MAX as int + 1) + a.lo as int,
                carry as int * (u128::MAX as int + 1) + lo as int == a.lo as int * v;
    }
    Wide { hi: a.hi * w as i128 + carry as i128, lo }
}

/// Exact negation.
pub fn neg(a: Wide) -> (result: Wide)
    requires -wide_limit() <= a.int() <= wide_limit(),
    ensures result.int() == -a.int(),
{
    if a.lo == 0 { Wide { hi: -a.hi, lo: 0 } }
    else { Wide { hi: -a.hi - 1, lo: u128::MAX - a.lo + 1 } }
}

/// Exact difference a - b; the caller proves the result stays within `wide_limit`.
pub fn sub(a: Wide, b: Wide) -> (result: Wide)
    requires -wide_limit() <= a.int() - b.int() <= wide_limit(),
    ensures result.int() == a.int() - b.int(),
{
    if a.lo >= b.lo { Wide { hi: a.hi - b.hi, lo: a.lo - b.lo } }
    else { Wide { hi: a.hi - b.hi - 1, lo: a.lo + (u128::MAX - b.lo) + 1 } }
}

/// Exact product with a signed multiplier of magnitude below 2^100, for a with a high word within 2^21.
pub fn mul_signed(a: Wide, k: i128) -> (result: Wide)
    requires
        -0x20_0000 <= a.hi <= 0x20_0000,
        -0x10_0000_0000_0000_0000_0000_0000i128 < k < 0x10_0000_0000_0000_0000_0000_0000i128,
    ensures result.int() == a.int() * k as int,
{
    if k >= 0 { mul(a, k as u128) }
    else {
        let product = mul(a, (-k) as u128);
        proof {
            assert(((-k) as u128) as int == -(k as int));
            let (h, v) = (a.hi as int, (-k) as int);
            assert(-0x2000_0000_0000_0000_0000_0000_0000_0000int < h * v < 0x2000_0000_0000_0000_0000_0000_0000_0000int) by (nonlinear_arith)
                requires -0x200000int <= h <= 0x200000int, 0 <= v < 0x10_0000_0000_0000_0000_0000_0000int;
            assert(product.int() == a.int() * v);
            assert(-(a.int() * v) == a.int() * (k as int)) by (nonlinear_arith) requires v == -(k as int);
            assert(-wide_limit() <= product.int() <= wide_limit()) by (nonlinear_arith)
                requires product.int() == a.int() * v, a.int() == h * (u128::MAX as int + 1) + a.lo as int,
                    -0x200000int <= h <= 0x200000int, 0 <= v < 0x10_0000_0000_0000_0000_0000_0000int,
                    0 <= (a.lo as int) < u128::MAX as int + 1, wide_limit() == (u128::MAX as int + 1) * (u128::MAX as int + 1) / 4;
        }
        neg(product)
    }
}

} // verus!
