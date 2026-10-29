// What the kernel's state means: the answer rows of a filing, as weighted sums over the tables.
use vstd::prelude::*;
use verus_common::sums::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::f64bits::*;

verus! {

/// n can head an answer row: all of its join and group columns are present.
pub open spec fn is_qual(n: NumRow) -> bool {
    n.adsh is Some && n.tag is Some && n.uom is Some && n.value is Some
}

pub open spec fn same_grp(n: NumRow, m: NumRow) -> bool {
    m.adsh == n.adsh && m.tag == n.tag && m.uom == n.uom
}

/// No row of n's (adsh, tag, uom) group has a larger value.
pub open spec fn is_max(num: Seq<NumRow>, n: NumRow) -> bool {
    forall|r: NumRow| num.contains(r) && same_grp(n, r) && r.value is Some ==> #[trigger] r.value->0 <= n.value->0
}

/// n is a row of its group holding the group maximum, in uom.
pub open spec fn picked(num: Seq<NumRow>, n: NumRow, uom: Seq<u8>) -> bool {
    is_qual(n) && n.uom == Some(uom) && is_max(num, n)
}

pub open spec fn sub_hits(n: NumRow, s: SubRow, fy: int) -> bool {
    s.fy == Some(fy) && n.adsh == Some(s.adsh)
}

pub open spec fn ent_row(n: NumRow, s: SubRow) -> OutRow {
    OutRow { name: s.name, tag: n.tag, value: n.value }
}

/// Σ g over the answer rows n yields in fiscal year fy.
pub open spec fn pair_w(sub: Seq<SubRow>, n: NumRow, fy: int, g: spec_fn(OutRow) -> int) -> int {
    sum(sub, |s: SubRow| if sub_hits(n, s, fy) { g(ent_row(n, s)) } else { 0 })
}

/// Σ g over the answer rows of class (fy, uom).
pub open spec fn class_w(db: Db, fy: int, uom: Seq<u8>, g: spec_fn(OutRow) -> int) -> int {
    sum(db.num, |n: NumRow| if picked(db.num, n, uom) { pair_w(db.sub, n, fy, g) } else { 0 })
}

} // verus!
