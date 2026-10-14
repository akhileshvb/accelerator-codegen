// Exact rationals, for SQL AVG: the spec states AVG as the pair (SUM, COUNT),
// whose value is num / den, and leaves rounding to the engine's result type to
// the host.
use vstd::prelude::*;

verus! {

/// The rational num / den.
pub struct Rat {
    pub num: int,
    pub den: int,
}

} // verus!
