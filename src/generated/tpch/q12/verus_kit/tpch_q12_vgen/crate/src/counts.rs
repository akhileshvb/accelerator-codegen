use vstd::prelude::*;

verus! {

#[derive(Clone, Copy)]
pub struct Counts {
    pub high: i128,
    pub low: i128,
}

impl Counts {
    pub open spec fn nonnegative(self) -> bool {
        self.high >= 0 && self.low >= 0
    }

    pub open spec fn positive(self) -> bool {
        self.nonnegative() && (self.high > 0 || self.low > 0)
    }

    pub fn zero() -> (out: Self)
        ensures out.high == 0, out.low == 0,
    {
        Counts { high: 0, low: 0 }
    }

    pub fn is_zero(&self) -> (out: bool)
        ensures out == (self.high == 0 && self.low == 0),
    {
        self.high == 0 && self.low == 0
    }

    pub fn add(self, other: Self) -> (out: Option<Self>)
        ensures out matches Some(value) ==> value.high == self.high + other.high
            && value.low == self.low + other.low,
    {
        let high = match self.high.checked_add(other.high) {
            Some(value) => value, None => return None,
        };
        let low = match self.low.checked_add(other.low) {
            Some(value) => value, None => return None,
        };
        Some(Counts { high, low })
    }

    pub fn sub(self, other: Self) -> (out: Option<Self>)
        ensures out matches Some(value) ==> value.high == self.high - other.high
            && value.low == self.low - other.low,
    {
        let high = match self.high.checked_sub(other.high) {
            Some(value) => value, None => return None,
        };
        let low = match self.low.checked_sub(other.low) {
            Some(value) => value, None => return None,
        };
        Some(Counts { high, low })
    }

    pub fn scale(self, factor: i128) -> (out: Option<Self>)
        ensures out matches Some(value) ==> value.high == self.high * factor
            && value.low == self.low * factor,
    {
        let high = match self.high.checked_mul(factor) {
            Some(value) => value, None => return None,
        };
        let low = match self.low.checked_mul(factor) {
            Some(value) => value, None => return None,
        };
        Some(Counts { high, low })
    }
}

}
