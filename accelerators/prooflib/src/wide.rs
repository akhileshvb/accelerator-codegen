// Exact signed integers of any size (use for sums and products that can exceed i128, e.g. many rows
// times large per-row values, or bag-semantics group sizes).  `Wide` is an i128 while the value fits
// and a sign and little-endian base-2^64 magnitude (`Big`) after an overflow, so the common case
// does not allocate.  The view `@` is the integer; no operation can overflow or decline.
use vstd::prelude::*;

verus! {

pub open spec fn pw(k: nat) -> int
    decreases k,
{
    if k == 0 { 1 } else { 0x1_0000_0000_0000_0000int * pw((k - 1) as nat) }
}

pub open spec fn get(s: Seq<u64>, i: nat) -> int {
    if i < s.len() { s[i as int] as int } else { 0 }
}

/// The value of the first n limbs (limbs past the end are 0).
pub open spec fn lm(s: Seq<u64>, n: nat) -> int
    decreases n,
{
    if n == 0 { 0 } else { lm(s, (n - 1) as nat) + get(s, (n - 1) as nat) * pw((n - 1) as nat) }
}

proof fn lemma_pw_pos(k: nat)
    ensures pw(k) >= 1,
    decreases k,
{
    if k > 0 { lemma_pw_pos((k - 1) as nat); }
}

proof fn lemma_pw_mono(a: nat, b: nat)
    requires a <= b,
    ensures pw(a) <= pw(b),
    decreases b - a,
{
    if a < b {
        lemma_pw_mono(a, (b - 1) as nat);
        lemma_pw_pos((b - 1) as nat);
    }
}

proof fn lemma_lm_bound(s: Seq<u64>, n: nat)
    ensures 0 <= lm(s, n) < pw(n),
    decreases n,
{
    if n > 0 {
        lemma_lm_bound(s, (n - 1) as nat);
        let p = pw((n - 1) as nat);
        let g = get(s, (n - 1) as nat);
        lemma_pw_pos((n - 1) as nat);
        assert(g * p <= 0xffff_ffff_ffff_ffffint * p) by (nonlinear_arith) requires 0 <= g <= 0xffff_ffff_ffff_ffffint, p >= 1;
        assert(0 <= g * p) by (nonlinear_arith) requires 0 <= g, p >= 1;
    }
}

proof fn lemma_lm_ext(s: Seq<u64>, n: nat, m: nat)
    requires s.len() <= n <= m,
    ensures lm(s, n) == lm(s, m),
    decreases m,
{
    if m > n { lemma_lm_ext(s, n, (m - 1) as nat); }
}

proof fn lemma_lm_prefix(s: Seq<u64>, t: Seq<u64>, n: nat)
    requires forall|i: int| 0 <= i < n ==> #[trigger] get(s, i as nat) == get(t, i as nat),
    ensures lm(s, n) == lm(t, n),
    decreases n,
{
    if n > 0 { lemma_lm_prefix(s, t, (n - 1) as nat); }
}

/// A top limb that is not zero makes the value at least pw(len - 1).
proof fn lemma_lm_top(s: Seq<u64>)
    requires s.len() > 0, s.last() != 0,
    ensures lm(s, s.len()) >= pw((s.len() - 1) as nat),
{
    let n = (s.len() - 1) as nat;
    lemma_lm_bound(s, n);
    lemma_pw_pos(n);
    let g = get(s, n);
    assert(g >= 1);
    assert(g * pw(n) >= pw(n)) by (nonlinear_arith) requires g >= 1, pw(n) >= 1;
}

proof fn lemma_lm_step(s: Seq<u64>, i: nat)
    ensures lm(s, (i + 1) as nat) == lm(s, i) + get(s, i) * pw(i), pw((i + 1) as nat) == 0x1_0000_0000_0000_0000int * pw(i),
{
}

proof fn lemma_lm_two(s: Seq<u64>)
    ensures lm(s, 2) == get(s, 0) + get(s, 1) * 0x1_0000_0000_0000_0000int, pw(2) == 0x1_0000_0000_0000_0000int * 0x1_0000_0000_0000_0000int,
{
    lemma_lm_step(s, 0);
    lemma_lm_step(s, 1);
}

fn limb(v: &Vec<u64>, i: usize) -> (r: u64)
    ensures r as int == get(v@, i as nat),
{
    if i < v.len() { v[i] } else { 0 }
}

/// a + b.
fn add_mag(a: &Vec<u64>, b: &Vec<u64>) -> (r: Vec<u64>)
    ensures lm(r@, r.len() as nat) == lm(a@, a.len() as nat) + lm(b@, b.len() as nat),
{
    let n = if a.len() > b.len() { a.len() } else { b.len() };
    proof {
        lemma_lm_ext(a@, a.len() as nat, n as nat);
        lemma_lm_ext(b@, b.len() as nat, n as nat);
    }
    let mut r: Vec<u64> = Vec::new();
    let mut carry: u64 = 0;
    let mut i: usize = 0;
    while i < n
        invariant i <= n, n == if a.len() > b.len() { a.len() } else { b.len() }, r.len() == i, carry <= 1,
            lm(r@, i as nat) + carry as int * pw(i as nat) == lm(a@, i as nat) + lm(b@, i as nat),
        decreases n - i,
    {
        let x: u128 = limb(a, i) as u128 + limb(b, i) as u128 + carry as u128;
        let lo = x as u64;
        let hi = (x >> 64) as u64;
        proof {
            assert(x == lo as u128 + hi as u128 * 0x1_0000_0000_0000_0000u128) by (bit_vector)
                requires lo == x as u64, hi == (x >> 64) as u64, x < 0x2_0000_0000_0000_0000u128;
            assert(hi <= 1) by (bit_vector) requires hi == (x >> 64) as u64, x < 0x2_0000_0000_0000_0000u128;
        }
        let ghost old_r = r@;
        r.push(lo);
        proof {
            assert(r@.len() == i + 1);
            assert(get(r@, i as nat) == lo as int);
            assert forall|j: int| 0 <= j < i implies #[trigger] get(r@, j as nat) == get(old_r, j as nat) by {}
            lemma_lm_prefix(r@, old_r, i as nat);
            let p = pw(i as nat);
            lemma_lm_step(a@, i as nat);
            lemma_lm_step(b@, i as nat);
            lemma_lm_step(r@, i as nat);
            let (l, h, ai, bi, c) = (lo as int, hi as int, get(a@, i as nat), get(b@, i as nat), carry as int);
            assert(ai + bi + c == l + h * 0x1_0000_0000_0000_0000int);
            assert((ai + bi + c) * p == l * p + h * (0x1_0000_0000_0000_0000int * p)) by (nonlinear_arith)
                requires ai + bi + c == l + h * 0x1_0000_0000_0000_0000int;
            assert((ai + bi + c) * p == ai * p + bi * p + c * p) by (nonlinear_arith);
        }
        carry = hi;
        i += 1;
    }
    if carry == 1 {
        let ghost old_r = r@;
        r.push(1);
        proof {
            assert forall|j: int| 0 <= j < n implies #[trigger] get(r@, j as nat) == get(old_r, j as nat) by {}
            lemma_lm_prefix(r@, old_r, n as nat);
        }
    }
    r
}

/// a - b with a borrow out: r + borrow * pw(n) = a - b, 0 <= r < pw(n), for n = the longer length.
fn sub_mag(a: &Vec<u64>, b: &Vec<u64>) -> (r: (Vec<u64>, bool))
    ensures
        r.0.len() == if a.len() > b.len() { a.len() } else { b.len() },
        lm(r.0@, r.0.len() as nat) == lm(a@, a.len() as nat) - lm(b@, b.len() as nat) + (if r.1 { pw(r.0.len() as nat) } else { 0 }),
        r.1 == (lm(a@, a.len() as nat) < lm(b@, b.len() as nat)),
{
    let n = if a.len() > b.len() { a.len() } else { b.len() };
    proof {
        lemma_lm_ext(a@, a.len() as nat, n as nat);
        lemma_lm_ext(b@, b.len() as nat, n as nat);
    }
    let mut r: Vec<u64> = Vec::new();
    let mut borrow: bool = false;
    let mut i: usize = 0;
    while i < n
        invariant i <= n, n == if a.len() > b.len() { a.len() } else { b.len() }, r.len() == i,
            lm(r@, i as nat) == lm(a@, i as nat) - lm(b@, i as nat) + (if borrow { pw(i as nat) } else { 0 }),
        decreases n - i,
    {
        let x: i128 = limb(a, i) as i128 - limb(b, i) as i128 - (if borrow { 1i128 } else { 0i128 });
        let neg = x < 0;
        let d: u64 = if neg { (x + 0x1_0000_0000_0000_0000i128) as u64 } else { x as u64 };
        let ghost old_r = r@;
        r.push(d);
        proof {
            assert forall|j: int| 0 <= j < i implies #[trigger] get(r@, j as nat) == get(old_r, j as nat) by {}
            lemma_lm_prefix(r@, old_r, i as nat);
            let p = pw(i as nat);
            lemma_lm_step(a@, i as nat);
            lemma_lm_step(b@, i as nat);
            lemma_lm_step(r@, i as nat);
            let (dd, ai, bi) = (d as int, get(a@, i as nat), get(b@, i as nat));
            let bo: int = if borrow { 1 } else { 0 };
            let nb: int = if neg { 1 } else { 0 };
            assert(dd == ai - bi - bo + nb * 0x1_0000_0000_0000_0000int);
            let x = ai - bi - bo;
            assert((x + nb * 0x1_0000_0000_0000_0000int) * p == x * p + nb * (0x1_0000_0000_0000_0000int * p)) by (nonlinear_arith);
            assert(dd * p == x * p + nb * (0x1_0000_0000_0000_0000int * p));
            assert((ai - bi - bo) * p == dd * p - nb * (0x1_0000_0000_0000_0000int * p));
            assert((ai - bi - bo) * p == ai * p - bi * p - bo * p) by (nonlinear_arith);
            assert((if borrow { pw(i as nat) } else { 0 }) == bo * p);
            assert((if neg { pw((i + 1) as nat) } else { 0 }) == nb * (0x1_0000_0000_0000_0000int * p));
        }
        borrow = neg;
        i += 1;
    }
    proof {
        lemma_lm_bound(r@, n as nat);
        lemma_lm_bound(a@, n as nat);
        lemma_lm_bound(b@, n as nat);
    }
    (r, borrow)
}

/// k * a.
fn mul_limb(a: &Vec<u64>, k: u64) -> (r: Vec<u64>)
    ensures lm(r@, r.len() as nat) == lm(a@, a.len() as nat) * k as int,
{
    let mut r: Vec<u64> = Vec::new();
    let mut carry: u64 = 0;
    let mut i: usize = 0;
    while i < a.len()
        invariant i <= a.len(), r.len() == i,
            lm(r@, i as nat) + carry as int * pw(i as nat) == lm(a@, i as nat) * k as int,
        decreases a.len() - i,
    {
        proof {
            assert(a[i as int] as u128 * k as u128 + carry as u128 <= 0xffff_ffff_ffff_ffff_ffff_ffff_ffff_ffffu128) by (nonlinear_arith)
                requires a[i as int] <= 0xffff_ffff_ffff_ffffu64, k <= 0xffff_ffff_ffff_ffffu64, carry <= 0xffff_ffff_ffff_ffffu64;
        }
        let x: u128 = a[i] as u128 * k as u128 + carry as u128;
        let lo = x as u64;
        let hi = (x >> 64) as u64;
        proof {
            assert(x == lo as u128 + hi as u128 * 0x1_0000_0000_0000_0000u128) by (bit_vector)
                requires lo == x as u64, hi == (x >> 64) as u64;
        }
        let ghost old_r = r@;
        r.push(lo);
        proof {
            assert forall|j: int| 0 <= j < i implies #[trigger] get(r@, j as nat) == get(old_r, j as nat) by {}
            lemma_lm_prefix(r@, old_r, i as nat);
            let p = pw(i as nat);
            lemma_lm_step(a@, i as nat);
            lemma_lm_step(r@, i as nat);
            let (l, h, ai, kk, c) = (lo as int, hi as int, a[i as int] as int, k as int, carry as int);
            assert(ai * kk + c == l + h * 0x1_0000_0000_0000_0000int);
            assert((ai * kk + c) * p == l * p + h * (0x1_0000_0000_0000_0000int * p)) by (nonlinear_arith)
                requires ai * kk + c == l + h * 0x1_0000_0000_0000_0000int;
            assert(get(a@, i as nat) == ai);
            assert((ai * kk + c) * p == ai * kk * p + c * p) by (nonlinear_arith);
            assert(ai * p * kk == ai * kk * p) by (nonlinear_arith);
            assert((lm(a@, i as nat) + ai * p) * kk == lm(a@, i as nat) * kk + ai * p * kk) by (nonlinear_arith);
        }
        carry = hi;
        i += 1;
    }
    let ghost before = r@;
    r.push(carry);
    proof {
        let full = r@;
        lemma_lm_step(full, a.len() as nat);
        assert(get(full, a.len() as nat) == carry as int);
        assert forall|j: int| 0 <= j < a.len() implies #[trigger] get(full, j as nat) == get(before, j as nat) by {}
        lemma_lm_prefix(full, before, a.len() as nat);
    }
    r
}

/// a * pw(count): count zero limbs below a.
fn shift_up(a: &Vec<u64>, count: usize) -> (r: Vec<u64>)
    ensures lm(r@, r.len() as nat) == lm(a@, a.len() as nat) * pw(count as nat),
{
    let mut r: Vec<u64> = Vec::new();
    let mut i: usize = 0;
    while i < count
        invariant i <= count, r.len() == i, forall|j: int| 0 <= j < i ==> r@[j] == 0u64,
        decreases count - i,
    {
        r.push(0);
        i += 1;
    }
    let mut j: usize = 0;
    while j < a.len()
        invariant j <= a.len(), r.len() == count + j,
            forall|t: int| 0 <= t < count ==> r@[t] == 0u64,
            forall|t: int| 0 <= t < j ==> r@[count as int + t] == a@[t],
        decreases a.len() - j,
    {
        r.push(a[j]);
        j += 1;
    }
    proof { lemma_shift_value(r@, a@, count as nat, a.len() as nat); }
    r
}

proof fn lemma_shift_value(r: Seq<u64>, a: Seq<u64>, count: nat, n: nat)
    requires r.len() == count + n, a.len() == n,
        forall|t: int| 0 <= t < count ==> r[t] == 0u64,
        forall|t: int| 0 <= t < n ==> r[count as int + t] == a[t],
    ensures lm(r, (count + n) as nat) == lm(a, n) * pw(count),
    decreases n,
{
    if n == 0 {
        lemma_zero_prefix(r, count);
    } else {
        lemma_shift_value(r.drop_last(), a.drop_last(), count, (n - 1) as nat);
        let p = pw((count + n - 1) as nat);
        assert(pw((count + n - 1) as nat) == pw(count) * pw((n - 1) as nat)) by { lemma_pw_add(count, (n - 1) as nat); }
        let g = get(a, (n - 1) as nat);
        assert(lm(r, (count + n) as nat) == lm(r, (count + n - 1) as nat) + get(r, (count + n - 1) as nat) * p);
        assert(lm(r, (count + n - 1) as nat) == lm(r.drop_last(), (count + n - 1) as nat)) by {
            lemma_lm_prefix(r, r.drop_last(), (count + n - 1) as nat);
        }
        assert(lm(a, n) == lm(a, (n - 1) as nat) + g * pw((n - 1) as nat));
        assert(lm(a.drop_last(), (n - 1) as nat) == lm(a, (n - 1) as nat)) by { lemma_lm_prefix(a, a.drop_last(), (n - 1) as nat); }
        let (x, y, z) = (lm(a, (n - 1) as nat), pw(count), pw((n - 1) as nat));
        assert((x + g * z) * y == x * y + g * (y * z)) by (nonlinear_arith);
    }
}

proof fn lemma_zero_prefix(r: Seq<u64>, n: nat)
    requires r.len() >= n, forall|t: int| 0 <= t < n ==> r[t] == 0u64,
    ensures lm(r, n) == 0,
    decreases n,
{
    if n > 0 { lemma_zero_prefix(r, (n - 1) as nat); }
}

proof fn lemma_pw_add(a: nat, b: nat)
    ensures pw((a + b) as nat) == pw(a) * pw(b),
    decreases b,
{
    if b > 0 {
        lemma_pw_add(a, (b - 1) as nat);
        let (x, y) = (pw(a), pw((b - 1) as nat));
        assert(x * (0x1_0000_0000_0000_0000int * y) == 0x1_0000_0000_0000_0000int * (x * y)) by (nonlinear_arith);
    }
}

/// a * b.
fn mul_mag(a: &Vec<u64>, b: &Vec<u64>) -> (r: Vec<u64>)
    ensures lm(r@, r.len() as nat) == lm(a@, a.len() as nat) * lm(b@, b.len() as nat),
{
    let mut r: Vec<u64> = Vec::new();
    let mut i: usize = 0;
    while i < a.len()
        invariant i <= a.len(), lm(r@, r.len() as nat) == lm(a@, i as nat) * lm(b@, b.len() as nat),
        decreases a.len() - i,
    {
        let row = mul_limb(b, a[i]);
        let shifted = shift_up(&row, i);
        let ghost old_r = r@;
        r = add_mag(&r, &shifted);
        proof {
            let (x, y) = (get(a@, i as nat), lm(b@, b.len() as nat));
            let p = pw(i as nat);
            assert(a[i as int] as int == x);
            assert(x * y * p == lm(a@, (i + 1) as nat) * y - lm(a@, i as nat) * y) by (nonlinear_arith)
                requires lm(a@, (i + 1) as nat) == lm(a@, i as nat) + x * p;
            assert(lm(a@, (i + 1) as nat) * y == lm(a@, i as nat) * y + x * p * y) by (nonlinear_arith)
                requires lm(a@, (i + 1) as nat) == lm(a@, i as nat) + x * p;
            assert(lm(shifted@, shifted.len() as nat) == y * x * p) by (nonlinear_arith)
                requires lm(shifted@, shifted.len() as nat) == lm(row@, row.len() as nat) * p, lm(row@, row.len() as nat) == y * x;
            assert(y * x * p == x * p * y) by (nonlinear_arith);
        }
        i += 1;
    }
    proof { lemma_lm_ext(a@, a.len() as nat, a.len() as nat); }
    r
}

fn trim(v: &mut Vec<u64>)
    ensures lm(final(v)@, final(v).len() as nat) == lm(old(v)@, old(v).len() as nat),
        final(v).len() == 0 || final(v)@.last() != 0u64,
{
    while v.len() > 0 && v[v.len() - 1] == 0
        invariant lm(v@, v.len() as nat) == lm(old(v)@, old(v).len() as nat),
        decreases v.len(),
    {
        let ghost before = v@;
        v.pop();
        proof {
            assert(get(before, (before.len() - 1) as nat) == 0);
            assert forall|j: int| 0 <= j < v.len() implies #[trigger] get(v@, j as nat) == get(before, j as nat) by {}
            lemma_lm_prefix(v@, before, v.len() as nat);
        }
    }
}

/// The arbitrary-size representation behind `Wide`; prefer `Wide`.
pub struct Big {
    neg: bool,
    mag: Vec<u64>,
}

impl View for Big {
    type V = int;

    closed spec fn view(&self) -> int {
        if self.neg { -lm(self.mag@, self.mag.len() as nat) } else { lm(self.mag@, self.mag.len() as nat) }
    }
}

impl Big {
    #[verifier::type_invariant]
    spec fn wf(self) -> bool {
        self.mag.len() == 0 || self.mag@.last() != 0u64
    }

    pub closed spec fn mag_val(self) -> int {
        lm(self.mag@, self.mag.len() as nat)
    }

    fn make(neg: bool, mut mag: Vec<u64>) -> (r: Big)
        ensures r@ == if neg { -lm(mag@, mag.len() as nat) } else { lm(mag@, mag.len() as nat) },
    {
        trim(&mut mag);
        Big { neg, mag }
    }

    pub fn zero() -> (r: Big)
        ensures r@ == 0,
    {
        Big { neg: false, mag: Vec::new() }
    }

    pub fn from_i128(x: i128) -> (r: Big)
        ensures r@ == x as int,
    {
        let neg = x < 0;
        let m: u128 = if !neg { x as u128 } else if x == i128::MIN { 0x8000_0000_0000_0000_0000_0000_0000_0000u128 } else { (-x) as u128 };
        let lo = m as u64;
        let hi = (m >> 64) as u64;
        proof {
            assert(m == lo as u128 + hi as u128 * 0x1_0000_0000_0000_0000u128) by (bit_vector)
                requires lo == m as u64, hi == (m >> 64) as u64;
        }
        let mut mag = Vec::new();
        mag.push(lo);
        mag.push(hi);
        proof {
            lemma_lm_two(mag@);
            assert(m as int == if neg { -(x as int) } else { x as int });
        }
        Big::make(neg, mag)
    }

    pub fn dup(&self) -> (r: Big)
        ensures r@ == self@,
    {
        proof { use_type_invariant(self); }
        let mag = self.mag.clone();
        proof { assert(mag@ == self.mag@); }
        Big { neg: self.neg, mag }
    }

    pub fn neg(&self) -> (r: Big)
        ensures r@ == -self@,
    {
        proof { use_type_invariant(self); }
        let mag = self.mag.clone();
        proof { assert(mag@ == self.mag@); }
        Big { neg: !self.neg, mag }
    }

    pub fn add(&self, o: &Big) -> (r: Big)
        ensures r@ == self@ + o@,
    {
        proof { use_type_invariant(self); use_type_invariant(o); }
        if self.neg == o.neg {
            let m = add_mag(&self.mag, &o.mag);
            Big::make(self.neg, m)
        } else {
            let (d, borrow) = sub_mag(&self.mag, &o.mag);
            if !borrow {
                proof {
                    let n = d.len() as nat;
                    lemma_lm_ext(self.mag@, self.mag.len() as nat, n);
                }
                Big::make(self.neg, d)
            } else {
                let (d2, borrow2) = sub_mag(&o.mag, &self.mag);
                Big::make(o.neg, d2)
            }
        }
    }

    pub fn sub(&self, o: &Big) -> (r: Big)
        ensures r@ == self@ - o@,
    {
        let n = o.neg();
        self.add(&n)
    }

    pub fn mul(&self, o: &Big) -> (r: Big)
        ensures r@ == self@ * o@,
    {
        proof { use_type_invariant(self); use_type_invariant(o); }
        let m = mul_mag(&self.mag, &o.mag);
        let neg = self.neg != o.neg;
        proof {
            let (x, y) = (self.mag_val(), o.mag_val());
            assert((if neg { -(x * y) } else { x * y }) == self@ * o@) by (nonlinear_arith)
                requires neg == (self.neg != o.neg), self@ == if self.neg { -x } else { x }, o@ == if o.neg { -y } else { y };
        }
        Big::make(neg, m)
    }

    pub fn is_zero(&self) -> (r: bool)
        ensures r == (self@ == 0),
    {
        proof { use_type_invariant(self); if self.mag.len() > 0 { lemma_lm_top(self.mag@); lemma_pw_pos((self.mag.len() - 1) as nat); } }
        self.mag.len() == 0
    }

    pub fn is_pos(&self) -> (r: bool)
        ensures r == (self@ > 0),
    {
        proof { use_type_invariant(self); if self.mag.len() > 0 { lemma_lm_top(self.mag@); lemma_pw_pos((self.mag.len() - 1) as nat); } }
        !self.neg && self.mag.len() > 0
    }

    pub fn is_neg(&self) -> (r: bool)
        ensures r == (self@ < 0),
    {
        proof { use_type_invariant(self); if self.mag.len() > 0 { lemma_lm_top(self.mag@); lemma_pw_pos((self.mag.len() - 1) as nat); } }
        self.neg && self.mag.len() > 0
    }

    /// The value as an i128, when it fits.
    pub fn to_i128(&self) -> (r: Option<i128>)
        ensures
            r matches Some(v) ==> v as int == self@,
            i128::MIN <= self@ <= i128::MAX ==> r is Some,
    {
        proof { use_type_invariant(self); }
        let n = self.mag.len();
        if n > 2 {
            proof {
                lemma_lm_top(self.mag@);
                lemma_pw_mono(2, (n - 1) as nat);
                lemma_lm_two(self.mag@);
            }
            return None;
        }
        let lo = limb(&self.mag, 0);
        let hi = limb(&self.mag, 1);
        proof {
            lemma_lm_ext(self.mag@, n as nat, 2);
            lemma_lm_two(self.mag@);
            assert(hi as u128 * 0x1_0000_0000_0000_0000u128 <= 0xffff_ffff_ffff_ffffu128 * 0x1_0000_0000_0000_0000u128) by (nonlinear_arith)
                requires hi <= 0xffff_ffff_ffff_ffffu64;
        }
        let m: u128 = lo as u128 + (hi as u128) * 0x1_0000_0000_0000_0000u128;
        if self.neg {
            if m <= 0x8000_0000_0000_0000_0000_0000_0000_0000u128 {
                let v: i128 = if m == 0x8000_0000_0000_0000_0000_0000_0000_0000u128 { i128::MIN } else { -(m as i128) };
                Some(v)
            } else { None }
        } else {
            if m <= 0x7fff_ffff_ffff_ffff_ffff_ffff_ffff_ffffu128 { Some(m as i128) } else { None }
        }
    }
}


/// An exact signed integer: an i128 while it fits, `Big` otherwise.  Use for any total sum or
/// product whose size the envelope does not bound by 2^127.
pub enum Wide {
    Small(i128),
    Big(Big),
}

impl View for Wide {
    type V = int;

    closed spec fn view(&self) -> int {
        match self { Wide::Small(x) => *x as int, Wide::Big(b) => b@ }
    }
}

impl Wide {
    /// The value 0.
    pub fn zero() -> (r: Wide)
        ensures r@ == 0,
    { Wide::Small(0) }

    /// Lifts an i128 (use for a per-row value).
    pub fn from_i128(x: i128) -> (r: Wide)
        ensures r@ == x as int,
    { Wide::Small(x) }

    /// A copy.
    pub fn dup(&self) -> (r: Wide)
        ensures r@ == self@,
    { match self { Wide::Small(x) => Wide::Small(*x), Wide::Big(b) => Wide::Big(b.dup()) } }

    fn to_big(&self) -> (r: Big)
        ensures r@ == self@,
    { match self { Wide::Small(x) => Big::from_i128(*x), Wide::Big(b) => b.dup() } }

    /// Demotes a Big that fits an i128.
    fn norm(b: Big) -> (r: Wide)
        ensures r@ == b@,
    { match b.to_i128() { Some(x) => Wide::Small(x), None => Wide::Big(b) } }

    /// -self.
    pub fn neg(&self) -> (r: Wide)
        ensures r@ == -self@,
    {
        match self {
            Wide::Small(x) if *x != i128::MIN => Wide::Small(-*x),
            _ => Wide::norm(self.to_big().neg()),
        }
    }

    /// self + o.
    pub fn add(&self, o: &Wide) -> (r: Wide)
        ensures r@ == self@ + o@,
    {
        if let (Wide::Small(a), Wide::Small(b)) = (self, o) {
            if let Some(c) = a.checked_add(*b) { return Wide::Small(c); }
        }
        Wide::norm(self.to_big().add(&o.to_big()))
    }

    /// self - o.
    pub fn sub(&self, o: &Wide) -> (r: Wide)
        ensures r@ == self@ - o@,
    {
        if let (Wide::Small(a), Wide::Small(b)) = (self, o) {
            if let Some(c) = a.checked_sub(*b) { return Wide::Small(c); }
        }
        Wide::norm(self.to_big().sub(&o.to_big()))
    }

    /// self * o.
    pub fn mul(&self, o: &Wide) -> (r: Wide)
        ensures r@ == self@ * o@,
    {
        if let (Wide::Small(a), Wide::Small(b)) = (self, o) {
            if let Some(c) = a.checked_mul(*b) { return Wide::Small(c); }
        }
        Wide::norm(self.to_big().mul(&o.to_big()))
    }

    /// self == 0.
    pub fn is_zero(&self) -> (r: bool)
        ensures r == (self@ == 0),
    { match self { Wide::Small(x) => *x == 0, Wide::Big(b) => b.is_zero() } }

    /// self > 0.
    pub fn is_pos(&self) -> (r: bool)
        ensures r == (self@ > 0),
    { match self { Wide::Small(x) => *x > 0, Wide::Big(b) => b.is_pos() } }

    /// self < 0.
    pub fn is_neg(&self) -> (r: bool)
        ensures r == (self@ < 0),
    { match self { Wide::Small(x) => *x < 0, Wide::Big(b) => b.is_neg() } }

    /// self < o.
    pub fn lt(&self, o: &Wide) -> (r: bool)
        ensures r == (self@ < o@),
    {
        if let (Wide::Small(a), Wide::Small(b)) = (self, o) { return *a < *b; }
        self.sub(o).is_neg()
    }

    /// self == o.
    pub fn eq(&self, o: &Wide) -> (r: bool)
        ensures r == (self@ == o@),
    {
        if let (Wide::Small(a), Wide::Small(b)) = (self, o) { return *a == *b; }
        self.sub(o).is_zero()
    }

    /// The value as an i128 when it fits (None says it does not).
    pub fn to_i128(&self) -> (r: Option<i128>)
        ensures
            r matches Some(v) ==> v as int == self@,
            i128::MIN <= self@ <= i128::MAX ==> r is Some,
    { match self { Wide::Small(x) => Some(*x), Wide::Big(b) => b.to_i128() } }
}

}
