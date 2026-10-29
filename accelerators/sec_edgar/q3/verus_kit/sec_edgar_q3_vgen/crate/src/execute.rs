// A read: the join of the parameter set, grouped, rounded, ordered and cut.
use vstd::prelude::*;
use std::sync::Arc;
use verus_common::bag::*;
use verus_common::sums::*;
use verus_prooflib::ordered::{Index, KeyView};
use verus_prooflib::wide::Wide;
use crate::answer::*;
use crate::bridge::*;
use crate::bucket::*;
use crate::chosen::*;
use crate::entries::*;
use crate::f64bits::*;
use crate::gen_obligations::*;
use crate::gen_spec::*;
use crate::groups::*;
use crate::join::*;
use crate::join_spec::*;
use crate::numeric::*;
use crate::rows::*;

verus! {

/// The join records of every num row.
fn join_all(
    nums: &Vec<NumRt>,
    sub_bag: &Index<Vec<SubRt>>,
    p1: i64, p2: &[u8],
    c: &Consts,
    Ghost(db): Ghost<Db>,
) -> (out: Vec<Rec>)
    requires
        bag_holds::<SubRt>(*sub_bag, db.sub),
        consts_ok(*c),
        forall|f: spec_fn(NumRow) -> int| #[trigger] sum(rows_v(nums@), f) == sum(db.num, f),
        forall|i: int| 0 <= i < nums.len() ==> (#[trigger] nums[i]).value__null != 0 || f64_finite(nums[i].value),
    ensures
        records_ok(db, params(p1 as int, p2@), out@),
{
    let ghost p = params(p1 as int, p2@);
    let mut out: Vec<Rec> = Vec::new();
    let mut i: usize = 0;
    proof {
        assert forall|h: spec_fn(JK) -> int| #[trigger] recsum(out@, h)
            == sum(rows_v(nums@.take(0)), |n: NumRow| num_term(db, p, h, n)) by {
            assert(rows_v(nums@.take(0)) =~= Seq::<NumRow>::empty());
        }
    }
    while i < nums.len()
        invariant
            i <= nums.len(),
            bag_holds::<SubRt>(*sub_bag, db.sub),
            consts_ok(*c),
            p == params(p1 as int, p2@),
            forall|i: int| 0 <= i < nums.len() ==> (#[trigger] nums[i]).value__null != 0 || f64_finite(nums[i].value),
            all_rec_ok(out@),
            forall|h: spec_fn(JK) -> int| #[trigger] recsum(out@, h)
                == sum(rows_v(nums@.take(i as int)), |n: NumRow| num_term(db, p, h, n)),
        decreases nums.len() - i,
    {
        let ghost before = out@;
        let ghost nv = nums@[i as int].rv();
        emit_num(&mut out, sub_bag, &nums[i], p1, p2, &c.pows, Ghost(db));
        proof {
            let prefix = rows_v(nums@.take(i as int));
            assert(nums@.take(i as int + 1) =~= nums@.take(i as int).push(nums@[i as int]));
            assert(rows_v(nums@.take(i as int + 1)) =~= prefix.push(nv));
            assert forall|h: spec_fn(JK) -> int| #[trigger] recsum(out@, h)
                == sum(rows_v(nums@.take(i as int + 1)), |n: NumRow| num_term(db, p, h, n)) by {
                lemma_sum_push(prefix, nv, |n: NumRow| num_term(db, p, h, n));
                assert(recsum(before, h) == sum(prefix, |n: NumRow| num_term(db, p, h, n)));
            }
        }
        i += 1;
    }
    proof {
        assert(nums@.take(nums.len() as int) =~= nums@);
        assert forall|h: spec_fn(JK) -> int| #[trigger] recsum(out@, h) == sum(tuples(db, p), |tp: Tuple| h(pj(tp))) by {
            let f = |n: NumRow| num_term(db, p, h, n);
            assert(sum(rows_v(nums@), f) == sum(db.num, f));
            lemma_tuple_sum(db, p, h);
        }
        assert forall|h: spec_fn(JK) -> int| #[trigger] recsum(out@, h) == sum(avg_sub_tuples(db, p), |tp: AvgSubTuple| h(pj2(tp))) by {
            let f = |n: NumRow| num_term(db, p, h, n);
            assert(sum(rows_v(nums@), f) == sum(db.num, f));
            lemma_avg_tuple_sum(db, p, h);
        }
    }
    out
}

/// The cells of the join records.
fn group_all(recs: &Vec<Rec>) -> (cells: Index<Cell>)
    requires all_rec_ok(recs@),
    ensures groups_ok(cells, recs@),
{
    let mut cells: Index<Cell> = Index::new();
    proof {
        assert forall|name: Option<Seq<u8>>, cik: Option<int>| (#[trigger] cells.at(gk(name, cik)) is None)
            implies gsum(recs@.take(0), name, cik, one()) == 0 by {
            assert(recs@.take(0) =~= Seq::<Rec>::empty());
        }
    }
    let mut i: usize = 0;
    while i < recs.len()
        invariant
            i <= recs.len(),
            all_rec_ok(recs@),
            groups_ok(cells, recs@.take(i as int)),
        decreases recs.len() - i,
    {
        proof { assert(recs@.take(i as int + 1) =~= recs@.take(i as int).push(recs@[i as int])); }
        add_rec(&mut cells, &recs[i], Ghost(recs@.take(i as int)));
        i += 1;
    }
    proof { assert(recs@.take(recs.len() as int) =~= recs@); }
    cells
}

/// The answer to the query for a parameter set, from the tables.
pub fn answer(
    nums: &Vec<NumRt>,
    sub_bag: &Index<Vec<SubRt>>,
    p1: i64, p2: &[u8],
    c: &Consts,
    Ghost(db): Ghost<Db>,
) -> (r: Option<Vec<Out>>)
    requires
        bag_holds::<SubRt>(*sub_bag, db.sub),
        consts_ok(*c),
        forall|f: spec_fn(NumRow) -> int| #[trigger] sum(rows_v(nums@), f) == sum(db.num, f),
        forall|i: int| 0 <= i < nums.len() ==> (#[trigger] nums[i]).value__null != 0 || f64_finite(nums[i].value),
    ensures
        r matches Some(out) ==> query_result(db, params(p1 as int, p2@), out_view(out@)),
        r matches Some(out) ==> forall|i: int| 0 <= i < out.len() ==> out_fits(#[trigger] out_view(out@)[i]),
        r is None ==> !answer_fits(db, params(p1 as int, p2@)),
{
    let ghost p = params(p1 as int, p2@);
    let recs = join_all(nums, sub_bag, p1, p2, c, Ghost(db));
    let outer = group_all(&recs);
    let os = entries_of(&outer, c, Ghost(recs@));
    let erased_recs = erase_all(&recs);
    proof {
        assert forall|i: int| 0 <= i < erased_recs@.len() implies rec_ok(#[trigger] erased_recs@[i]) by {
            assert(erased_recs@[i] == erase_rec(recs@[i]));
        }
    }
    let inner = group_all(&erased_recs);
    let gin = entries_of(&inner, c, Ghost(erased_recs@));
    finish3(&os, &gin, c, Ghost(db), Ghost(p), Ghost(recs@))
}

}
