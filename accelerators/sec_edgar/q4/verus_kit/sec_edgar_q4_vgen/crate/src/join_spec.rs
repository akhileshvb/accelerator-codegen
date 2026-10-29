// What a group's aggregates read of a tuple, and the where clause's conjuncts by the tables they bind.
use vstd::prelude::*;
use verus_common::rel::*;
use verus_common::sums::*;
use crate::gen_spec::*;

verus! {

/// What a group's aggregates read of a tuple.
pub struct JK {
    pub sic: Option<int>,
    pub tlabel: Option<Seq<u8>>,
    pub cik: Option<int>,
    pub v: int,
    pub vs: bool,
    pub stmt: Option<Seq<u8>>,
}

pub open spec fn c_n(p: Params, n: NumRow) -> bool {
    n.uom == Some(p.p4) && n.value is Some && n.adsh is Some && n.tag is Some && n.version is Some
}

pub open spec fn c_s(p: Params, s: SubRow) -> bool {
    s.sic is Some && p.p1 <= s.sic->0 && s.sic->0 <= p.p2
}

pub open spec fn c_t(t: TagRow) -> bool {
    t.abstract_ == Some(0int)
}

pub open spec fn m_s(n: NumRow, s: SubRow) -> bool {
    n.adsh == Some(s.adsh)
}

pub open spec fn m_t(n: NumRow, t: TagRow) -> bool {
    n.tag == Some(t.tag) && n.version == Some(t.version)
}

pub open spec fn c_p(p: Params, n: NumRow, r: PreRow) -> bool {
    r.stmt == Some(p.p3) && r.adsh == n.adsh && r.tag == n.tag && r.version == n.version
}

pub open spec fn val_of(n: NumRow) -> int {
    if n.value is Some { n.value->0 } else { 0 }
}

pub open spec fn pj(tp: Tuple) -> JK {
    JK { sic: tp.s_.sic, tlabel: tp.t_.tlabel, cik: tp.s_.cik, v: val_of(tp.n), vs: tp.n.value is Some, stmt: tp.p.stmt }
}

}
