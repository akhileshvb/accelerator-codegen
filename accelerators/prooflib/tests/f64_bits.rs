// Wide::f64_bits against Rust's `as f64` on random exact limb sums, with subnormal, tie and carry cases.
// The rounding is half up, so a tie may sit one ulp above Rust's half-even result.
//   verus accelerators/prooflib/tests/f64_bits.rs --no-verify --compile -C panic=abort -o f64_bits \
//     --import verus_common=$OUT/verus_common.vir --extern verus_common=$OUT/libverus_common.rlib \
//     --import verus_prooflib=$OUT/verus_prooflib.vir --extern verus_prooflib=$OUT/libverus_prooflib.rlib -L dependency=$OUT
use verus_prooflib::wide::Wide;

struct Rng(u64);

impl Rng {
    fn next(&mut self) -> u64 {
        self.0 ^= self.0 << 13;
        self.0 ^= self.0 >> 7;
        self.0 ^= self.0 << 17;
        self.0
    }
    fn below(&mut self, n: u64) -> u64 { self.next() % n }
}

/// sum of limbs[i] * 2^(64 i), exactly.
fn wide_of(limbs: &[u64]) -> Wide {
    let base = Wide::from_i128(1i128 << 64);
    let (mut sum, mut pow) = (Wide::zero(), Wide::from_i128(1));
    for &l in limbs {
        sum = sum.add(&Wide::from_i128(l as i128).mul(&pow));
        pow = pow.mul(&base);
    }
    sum
}

fn pow2(e: i32) -> f64 {
    if e >= -1022 { f64::from_bits(((e + 1023) as u64) << 52) } else { f64::from_bits(1u64 << (e + 1074)) }
}

/// Rust's rounding of the limbs' value times 2^-1074, and whether the value is an exact tie.
fn reference(limbs: &[u64]) -> (u64, bool) {
    let n = limbs.iter().rposition(|&l| l != 0).map_or(0, |i| i + 1);
    let lo = n.saturating_sub(2);
    let at = |i: usize| limbs.get(i).copied().unwrap_or(0) as u128;
    let w = at(lo) | at(lo + 1) << 64;
    let rest_zero = limbs[..lo].iter().all(|&l| l == 0);
    let bits = 128 - w.leading_zeros();
    let tie = rest_zero && bits > 53 && w & ((1u128 << (bits - 53)) - 1) == 1u128 << (bits - 54);
    let x = (w | !rest_zero as u128) as f64 * pow2(64 * lo as i32 - 1074);
    (x.to_bits(), tie)
}

fn check(limbs: &[u64], neg: bool) {
    let mag = wide_of(limbs);
    let got = if neg { mag.neg() } else { mag }.f64_bits();
    let (want, tie) = reference(limbs);
    let want = want | ((neg && want != 0) as u64) << 63;
    let magnitude = |b: u64| b & !(1 << 63);
    assert!(got == want || tie && magnitude(got) == magnitude(want) + 1 && got >> 63 == want >> 63,
        "limbs {limbs:x?} neg {neg}: got {got:#x}, want {want:#x} (tie {tie})");
}

/// A window of 53 mantissa bits, then a tail of the given shape, at limb offset lo.
fn rounding_edge(rng: &mut Rng) -> Vec<u64> {
    let mant = if rng.below(2) == 0 { (1u128 << 53) - 1 } else { (1u128 << 52) | rng.next() as u128 >> 12 };
    let sh = 1 + rng.below(75) as u32;
    let half = 1u128 << (sh - 1);
    let tail = [0, half, half - 1, half + 1, half - 1 + (rng.next() as u128 & (half - 1))][rng.below(5) as usize];
    let w = mant << sh | tail;
    let mut limbs = vec![0; rng.below(28) as usize];
    limbs.extend([w as u64, (w >> 64) as u64]);
    if rng.below(3) == 0 { for l in limbs.iter_mut() { if rng.below(2) == 0 { *l |= rng.next() >> 40; } } }
    limbs
}

fn random_limbs(rng: &mut Rng) -> Vec<u64> {
    let n = rng.below(31) as usize;
    (0..n).map(|_| match rng.below(4) { 0 => 0, 1 => u64::MAX, 2 => rng.next() >> rng.below(64), _ => rng.next() }).collect()
}

fn main() {
    let mut rng = Rng(0x9e37_79b9_7f4a_7c15);
    for v in 0..1u64 << 14 { check(&[v], false); check(&[v], true); }
    for v in (1u64 << 52) - 4..(1u64 << 52) + 4 { check(&[v], false); }
    for shift in 0..64 { for d in 0..3 { check(&[(1u64 << shift).wrapping_add(d).wrapping_sub(1)], false); } }
    for _ in 0..20000 { check(&random_limbs(&mut rng), rng.below(2) == 0); }
    for _ in 0..50000 { check(&rounding_edge(&mut rng), rng.below(2) == 0); }
    println!("ok");
}
