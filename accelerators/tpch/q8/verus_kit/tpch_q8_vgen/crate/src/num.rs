// Exact integers for cell counts and volumes: an i128 while it fits, a shared `Big` after.
// Unlike prooflib's `Wide` the view is open, so a spec can name `Num::Small(1)`.
use vstd::prelude::*;
use std::sync::Arc;
use verus_prooflib::wide::Big;

verus! {

pub enum Num {
    Small(i128),
    Large(Arc<Big>),
}

impl View for Num {
    type V = int;

    open spec fn view(&self) -> int {
        match self { Num::Small(x) => *x as int, Num::Large(b) => (**b)@ }
    }
}

impl Num {
    pub fn zero() -> (r: Num)
        ensures r@ == 0, r == Num::Small(0i128),
    { Num::Small(0) }

    /// A copy, structurally equal to the original.
    pub fn dup(&self) -> (r: Num)
        ensures r == *self,
    { match self { Num::Small(x) => Num::Small(*x), Num::Large(b) => Num::Large(b.clone()) } }

    fn big(&self) -> (r: Big)
        ensures r@ == (*self)@,
    { match self { Num::Small(x) => Big::from_i128(*x), Num::Large(b) => (**b).dup() } }

    fn norm(b: Big) -> (r: Num)
        ensures r@ == b@,
    { match b.to_i128() { Some(x) => Num::Small(x), None => Num::Large(Arc::new(b)) } }

    pub fn add(&self, o: &Num) -> (r: Num)
        ensures r@ == (*self)@ + (*o)@,
    {
        if let (Num::Small(a), Num::Small(b)) = (self, o) {
            if let Some(c) = a.checked_add(*b) { return Num::Small(c); }
        }
        Num::norm(self.big().add(&o.big()))
    }

    pub fn mul(&self, o: &Num) -> (r: Num)
        ensures r@ == (*self)@ * (*o)@,
    {
        if let (Num::Small(a), Num::Small(b)) = (self, o) {
            if let Some(c) = a.checked_mul(*b) { return Num::Small(c); }
        }
        Num::norm(self.big().mul(&o.big()))
    }

    pub fn is_zero(&self) -> (r: bool)
        ensures r == ((*self)@ == 0),
    { match self { Num::Small(x) => *x == 0, Num::Large(b) => (**b).is_zero() } }

    pub fn is_pos(&self) -> (r: bool)
        ensures r == ((*self)@ > 0),
    { match self { Num::Small(x) => *x > 0, Num::Large(b) => (**b).is_pos() } }

    /// Is it structurally Small(x)?  (Packed cells depend on the representation.)
    pub fn is_small(&self, v: i128) -> (r: bool)
        ensures r == (*self == Num::Small(v)),
    { match self { Num::Small(x) => *x == v, Num::Large(_) => false } }

    pub fn to_i128(&self) -> (r: Option<i128>)
        ensures
            r matches Some(v) ==> v as int == (*self)@,
            i128::MIN <= (*self)@ <= i128::MAX ==> r is Some,
    { match self { Num::Small(x) => Some(*x), Num::Large(b) => (**b).to_i128() } }
}

}
