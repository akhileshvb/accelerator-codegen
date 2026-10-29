// A read: the join of the parameter set, grouped, rounded, ordered and cut.
use vstd::prelude::*;
use std::sync::Arc;
use verus_common::bag::*;
use verus_common::sums::*;
use verus_prooflib::ordered::{Index, KeyView};
use verus_prooflib::wide::Wide;
use crate::bridge::*;
use crate::bucket::*;
use crate::f64bits::*;
use crate::gen_obligations::*;
use crate::gen_spec::*;
use crate::groups::*;
use crate::join::*;
use crate::join_spec::*;
use crate::numeric::*;
use crate::output::*;
use crate::rows::*;

verus! {

/// The join records of every num row.
fn join_all(
    nums: &Vec<NumRt>,
    sub_bag: &Index<Vec<SubRt>>,
    tag_bag: &Index<Vec<TagRt>>,
    pre_bag: &Index<Vec<PreRt>>,
    p1: i64, p2: i64, p3: &[u8], p4: &[u8],
    c: &Consts,
    Ghost(db): Ghost<Db>,
) -> (out: Vec<Rec>)
    requires
        bag_holds::<SubRt>(*sub_bag, db.sub),
        bag_holds::<TagRt>(*tag_bag, db.tag),
        bag_holds::<PreRt>(*pre_bag, db.pre),
        consts_ok(*c),
        forall|f: spec_fn(NumRow) -> int| #[trigger] sum(rows_v(nums@), f) == sum(db.num, f),
        forall|i: int| 0 <= i < nums.len() ==> (#[trigger] nums[i]).value__null != 0 || f64_finite(nums[i].value),
    ensures
        records_ok(db, params(p1 as int, p2 as int, p3@, p4@), out@),
{
    let ghost p = params(p1 as int, p2 as int, p3@, p4@);
    let mut out: Vec<Rec> = Vec::new();
    let mut i: usize = 0;
    proof {
        assert forall|h: spec_fn(JK) -> int| #[trigger] recsum(out@, p, h)
            == sum(rows_v(nums@.take(0)), |n: NumRow| num_term(db, p, h, n)) by {
            assert(rows_v(nums@.take(0)) =~= Seq::<NumRow>::empty());
        }
    }
    while i < nums.len()
        invariant
            i <= nums.len(),
            bag_holds::<SubRt>(*sub_bag, db.sub),
            bag_holds::<TagRt>(*tag_bag, db.tag),
            bag_holds::<PreRt>(*pre_bag, db.pre),
            consts_ok(*c),
            p == params(p1 as int, p2 as int, p3@, p4@),
            forall|i: int| 0 <= i < nums.len() ==> (#[trigger] nums[i]).value__null != 0 || f64_finite(nums[i].value),
            all_nonneg(out@),
            forall|h: spec_fn(JK) -> int| #[trigger] recsum(out@, p, h)
                == sum(rows_v(nums@.take(i as int)), |n: NumRow| num_term(db, p, h, n)),
        decreases nums.len() - i,
    {
        let ghost before = out@;
        let ghost nv = nums@[i as int].rv();
        emit_num(&mut out, sub_bag, tag_bag, pre_bag, &nums[i], p1, p2, p3, p4, &c.pows, Ghost(db));
        proof {
            let prefix = rows_v(nums@.take(i as int));
            assert(nums@.take(i as int + 1) =~= nums@.take(i as int).push(nums@[i as int]));
            assert(rows_v(nums@.take(i as int + 1)) =~= prefix.push(nv));
            assert forall|h: spec_fn(JK) -> int| #[trigger] recsum(out@, p, h)
                == sum(rows_v(nums@.take(i as int + 1)), |n: NumRow| num_term(db, p, h, n)) by {
                lemma_sum_push(prefix, nv, |n: NumRow| num_term(db, p, h, n));
                assert(recsum(before, p, h) == sum(prefix, |n: NumRow| num_term(db, p, h, n)));
            }
        }
        i += 1;
    }
    proof {
        assert(nums@.take(nums.len() as int) =~= nums@);
        assert forall|h: spec_fn(JK) -> int| #[trigger] recsum(out@, p, h) == sum(tuples(db, p), |tp: Tuple| h(pj(tp))) by {
            let f = |n: NumRow| num_term(db, p, h, n);
            assert(sum(rows_v(nums@), f) == sum(db.num, f));
            lemma_tuple_sum(db, p, h);
        }
    }
    out
}

/// The cells of the join records.
fn group_all(recs: &Vec<Rec>, Ghost(p): Ghost<Params>) -> (cells: Index<Cell>)
    requires all_nonneg(recs@),
    ensures groups_ok(cells, recs@, p),
{
    let mut cells: Index<Cell> = Index::new();
    proof {
        assert forall|sic: Option<int>, tl: Option<Seq<u8>>| (#[trigger] cells.at(gk(sic, tl)) is None)
            implies gsum(recs@.take(0), p, sic, tl, one()) == 0 by {
            assert(recs@.take(0) =~= Seq::<Rec>::empty());
        }
    }
    let mut i: usize = 0;
    while i < recs.len()
        invariant
            i <= recs.len(),
            all_nonneg(recs@),
            groups_ok(cells, recs@.take(i as int), p),
        decreases recs.len() - i,
    {
        proof {
            assert forall|j: int| 0 <= j < recs@.take(i as int).len() implies (#[trigger] recs@.take(i as int)[j]).w >= 0 by {
                assert(recs@.take(i as int)[j] == recs@[j]);
            }
            assert(recs@.take(i as int + 1) =~= recs@.take(i as int).push(recs@[i as int]));
        }
        add_rec(&mut cells, &recs[i], Ghost(recs@.take(i as int)), Ghost(p));
        i += 1;
    }
    proof { assert(recs@.take(recs.len() as int) =~= recs@); }
    cells
}

/// The answer to the query for a parameter set, from the tables.
pub fn answer(
    nums: &Vec<NumRt>,
    sub_bag: &Index<Vec<SubRt>>,
    tag_bag: &Index<Vec<TagRt>>,
    pre_bag: &Index<Vec<PreRt>>,
    p1: i64, p2: i64, p3: &[u8], p4: &[u8],
    c: &Consts,
    Ghost(db): Ghost<Db>,
) -> (r: Option<Vec<Out>>)
    requires
        bag_holds::<SubRt>(*sub_bag, db.sub),
        bag_holds::<TagRt>(*tag_bag, db.tag),
        bag_holds::<PreRt>(*pre_bag, db.pre),
        consts_ok(*c),
        forall|f: spec_fn(NumRow) -> int| #[trigger] sum(rows_v(nums@), f) == sum(db.num, f),
        forall|i: int| 0 <= i < nums.len() ==> (#[trigger] nums[i]).value__null != 0 || f64_finite(nums[i].value),
    ensures
        r matches Some(out) ==> query_result(db, params(p1 as int, p2 as int, p3@, p4@), out_view(out@)),
        r matches Some(out) ==> forall|i: int| 0 <= i < out.len() ==> out_fits(#[trigger] out_view(out@)[i]),
        r is None ==> !answer_fits(db, params(p1 as int, p2 as int, p3@, p4@)),
{
    let ghost p = params(p1 as int, p2 as int, p3@, p4@);
    let recs = join_all(nums, sub_bag, tag_bag, pre_bag, p1, p2, p3, p4, c, Ghost(db));
    let cells = group_all(&recs, Ghost(p));
    let cands = make_cands(&cells, c, Ghost(db), Ghost(p), Ghost(recs@));
    finish(&cands, p3, Ghost(db), Ghost(p), Ghost(recs@))
}

}
