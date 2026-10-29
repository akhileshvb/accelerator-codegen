// Two tables that hold the same rows of one filing give that filing the same cells.
use vstd::prelude::*;
use verus_common::sums::*;
use verus_common::rel::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::model::*;
use crate::cells::*;
use crate::group::lemma_eqv_pos;

verus! {

/// Every weight that vanishes outside filing a sums alike over the two num tables.
pub open spec fn agree_num(x: Seq<NumRow>, y: Seq<NumRow>, a: Seq<u8>) -> bool {
    forall|f: spec_fn(NumRow) -> int| (forall|n: NumRow| n.adsh != Some(a) ==> #[trigger] f(n) == 0) ==> #[trigger] sum(x, f) == sum(y, f)
}

pub open spec fn agree_sub(x: Seq<SubRow>, y: Seq<SubRow>, a: Seq<u8>) -> bool {
    forall|f: spec_fn(SubRow) -> int| (forall|s: SubRow| s.adsh != a ==> #[trigger] f(s) == 0) ==> #[trigger] sum(x, f) == sum(y, f)
}

pub open spec fn agree(x: Db, y: Db, a: Seq<u8>) -> bool {
    agree_num(x.num, y.num, a) && agree_sub(x.sub, y.sub, a)
}

pub proof fn lemma_agree_contains(x: Seq<NumRow>, y: Seq<NumRow>, a: Seq<u8>, r: NumRow)
    requires
        agree_num(x, y, a),
        r.adsh == Some(a),
    ensures
        x.contains(r) == y.contains(r),
{
    let f = eqv(r);
    assert forall|n: NumRow| n.adsh != Some(a) implies #[trigger] f(n) == 0 by {}
    lemma_eqv_pos(x, r);
    lemma_eqv_pos(y, r);
}

pub proof fn lemma_agree_is_max(x: Seq<NumRow>, y: Seq<NumRow>, a: Seq<u8>, n: NumRow)
    requires
        agree_num(x, y, a),
        n.adsh == Some(a),
    ensures
        is_max(x, n) == is_max(y, n),
{
    if is_max(x, n) {
        assert forall|r: NumRow| y.contains(r) && same_grp(n, r) && r.value is Some implies #[trigger] r.value->0 <= n.value->0 by {
            lemma_agree_contains(x, y, a, r);
        }
    }
    if is_max(y, n) {
        assert forall|r: NumRow| x.contains(r) && same_grp(n, r) && r.value is Some implies #[trigger] r.value->0 <= n.value->0 by {
            lemma_agree_contains(x, y, a, r);
        }
    }
}

pub proof fn lemma_agree_pair_w(x: Db, y: Db, a: Seq<u8>, n: NumRow, fy: int, g: spec_fn(OutRow) -> int)
    requires
        agree_sub(x.sub, y.sub, a),
        n.adsh == Some(a),
    ensures
        pair_w(x.sub, n, fy, g) == pair_w(y.sub, n, fy, g),
{
    let f = |s: SubRow| if sub_hits(n, s, fy) { g(ent_row(n, s)) } else { 0 };
    assert forall|s: SubRow| s.adsh != a implies #[trigger] f(s) == 0 by {}
}

/// The cells of filing a depend on that filing's rows only.
pub proof fn lemma_cell_w_agree(x: Db, y: Db, a: Seq<u8>, id: CellId, g: spec_fn(OutRow) -> int)
    requires
        agree(x, y, a),
        id.a == a,
    ensures
        cell_w(x, id, g) == cell_w(y, id, g),
{
    let tx = |n: NumRow| if picked(x.num, n, id.uom) && n.adsh == Some(id.a) && vkey(n.value->0, id.d) { pair_w(x.sub, n, id.fy, g) } else { 0 };
    let ty = |n: NumRow| if picked(y.num, n, id.uom) && n.adsh == Some(id.a) && vkey(n.value->0, id.d) { pair_w(y.sub, n, id.fy, g) } else { 0 };
    assert forall|n: NumRow| n.adsh != Some(a) implies #[trigger] tx(n) == 0 by {}
    assert forall|n: NumRow| #[trigger] tx(n) == ty(n) by {
        if n.adsh == Some(a) {
            lemma_agree_is_max(x.num, y.num, a, n);
            lemma_agree_pair_w(x, y, a, n, id.fy, g);
        }
    }
    assert(sum(x.num, tx) == sum(y.num, tx));
    lemma_sum_ext_all(y.num, tx, ty);
}


/// A filing with no sub row has no cells.
pub proof fn lemma_no_sub_no_cells(db: Db, id: CellId, g: spec_fn(OutRow) -> int)
    requires
        forall|k: int| 0 <= k < db.sub.len() ==> (#[trigger] db.sub[k]).adsh != id.a,
    ensures
        cell_w(db, id, g) == 0,
{
    let t = |n: NumRow| if picked(db.num, n, id.uom) && n.adsh == Some(id.a) && vkey(n.value->0, id.d) { pair_w(db.sub, n, id.fy, g) } else { 0 };
    assert forall|i: int| 0 <= i < db.num.len() implies t(#[trigger] db.num[i]) == 0 by {
        let n = db.num[i];
        if picked(db.num, n, id.uom) && n.adsh == Some(id.a) && vkey(n.value->0, id.d) {
            let f = |s: SubRow| if sub_hits(n, s, id.fy) { g(ent_row(n, s)) } else { 0 };
            assert forall|k: int| 0 <= k < db.sub.len() implies f(#[trigger] db.sub[k]) == 0 by {}
            lemma_sum_zero(db.sub, f);
        }
    }
    lemma_sum_zero(db.num, t);
}

} // verus!
