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

} // verus!
