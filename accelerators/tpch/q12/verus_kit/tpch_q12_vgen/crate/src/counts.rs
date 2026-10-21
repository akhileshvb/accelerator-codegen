use vstd::prelude::*;
use verus_prooflib::wide::Wide;

verus! {

/// The high and low line counts of a cell, as exact integers.
pub struct Counts {
    pub high: int,
    pub low: int,
}

impl Counts {
    pub open spec fn nonnegative(self) -> bool {
        self.high >= 0 && self.low >= 0
    }

    pub open spec fn positive(self) -> bool {
        self.nonnegative() && (self.high > 0 || self.low > 0)
    }
}

/// Runtime counts: exact, never overflowing.
pub struct Tally {
    pub high: Wide,
    pub low: Wide,
}

impl View for Tally {
    type V = Counts;

    open spec fn view(&self) -> Counts {
        Counts { high: self.high@, low: self.low@ }
    }
}

impl Tally {
    pub fn zero() -> (out: Self)
        ensures out@.high == 0, out@.low == 0,
    {
        Tally { high: Wide::zero(), low: Wide::zero() }
    }

    pub fn of(high: i128, low: i128) -> (out: Self)
        ensures out@.high == high as int, out@.low == low as int,
    {
        Tally { high: Wide::from_i128(high), low: Wide::from_i128(low) }
    }

    pub fn dup(&self) -> (out: Self)
        ensures out@ == self@,
    {
        Tally { high: self.high.dup(), low: self.low.dup() }
    }

    pub fn is_zero(&self) -> (out: bool)
        ensures out == (self@.high == 0 && self@.low == 0),
    {
        self.high.is_zero() && self.low.is_zero()
    }

    pub fn add(&self, other: &Self) -> (out: Self)
        ensures out@.high == self@.high + other@.high && out@.low == self@.low + other@.low,
    {
        Tally { high: self.high.add(&other.high), low: self.low.add(&other.low) }
    }

    pub fn sub(&self, other: &Self) -> (out: Self)
        ensures out@.high == self@.high - other@.high && out@.low == self@.low - other@.low,
    {
        Tally { high: self.high.sub(&other.high), low: self.low.sub(&other.low) }
    }

    pub fn scale(&self, factor: &Wide) -> (out: Self)
        ensures out@.high == self@.high * factor@ && out@.low == self@.low * factor@,
    {
        Tally { high: self.high.mul(factor), low: self.low.mul(factor) }
    }
}

}
