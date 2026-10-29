// A signed record: one (num, sub, tag, pre) combination of the join with the group it falls in and
// what the group's aggregates read of it.  Applying a record changes a group by w times its row.
use vstd::prelude::*;
use verus_common::sums::*;
use verus_prooflib::wide::Wide;
use crate::gen_spec::*;
use crate::join_spec::*;

verus! {

pub struct Rec {
    pub uom: Vec<u8>,
    pub stmt: Vec<u8>,
    pub sic: i64,
    pub tlabel: Option<Vec<u8>>,
    pub cik: Option<i64>,
    pub v: Wide,
    pub w: i128,
}

pub open spec fn opt_int(x: Option<i64>) -> Option<int> {
    match x { Some(a) => Some(a as int), None => None }
}

pub open spec fn opt_bytes(x: Option<Vec<u8>>) -> Option<Seq<u8>> {
    match x { Some(a) => Some(a@), None => None }
}

/// The record belongs to the parameter set's tuples.
pub open spec fn rec_in(p: Params, r: Rec) -> bool {
    r.uom@ == p.p4 && r.stmt@ == p.p3 && p.p1 <= r.sic as int && r.sic as int <= p.p2
}

pub open spec fn rec_jk(p: Params, r: Rec) -> JK {
    JK { sic: Some(r.sic as int), tlabel: opt_bytes(r.tlabel), cik: opt_int(r.cik), v: r.v@, vs: true, stmt: Some(p.p3) }
}

pub open spec fn rterm(p: Params, h: spec_fn(JK) -> int, r: Rec) -> int {
    if rec_in(p, r) { r.w as int * h(rec_jk(p, r)) } else { 0 }
}

/// What the records add to a sum over the parameter set's tuples.
pub open spec fn rsum(l: Seq<Rec>, p: Params, h: spec_fn(JK) -> int) -> int {
    sum(l, |r: Rec| rterm(p, h, r))
}

pub proof fn lemma_rsum_push(l: Seq<Rec>, r: Rec, p: Params, h: spec_fn(JK) -> int)
    ensures rsum(l.push(r), p, h) == rsum(l, p, h) + rterm(p, h, r),
{
    lemma_sum_push(l, r, |x: Rec| rterm(p, h, x));
}

pub proof fn lemma_rsum_concat(a: Seq<Rec>, b: Seq<Rec>, p: Params, h: spec_fn(JK) -> int)
    ensures rsum(a + b, p, h) == rsum(a, p, h) + rsum(b, p, h),
{
    lemma_sum_concat(a, b, |x: Rec| rterm(p, h, x));
}

}
