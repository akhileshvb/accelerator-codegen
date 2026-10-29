// The kernel: num and sub as bags, sub bucketed by its join key; a read joins, groups and orders them
// (execute.rs).  Writes apply the changed rows to the bags.
#![allow(unused_imports)]
use vstd::prelude::*;
use std::sync::Arc;
use verus_common::bag::*;
use verus_common::rat::*;
use verus_common::sums::*;
use verus_prooflib::ordered::Index;
use crate::bucket::*;
use crate::execute::*;
use crate::f64bits::*;
use crate::gen_obligations::*;
use crate::gen_spec::*;
use crate::numeric::*;
use crate::rows::*;

verus! {

pub struct State {
    nums: Vec<NumRt>,
    subs: Vec<SubRt>,
    sub_bag: Index<Vec<SubRt>>,
    consts: Arc<Consts>,
    db: Ghost<Db>,
}

pub open spec fn same_sums<A>(rows: Seq<A>, tab: Seq<A>) -> bool {
    forall|f: spec_fn(A) -> int| #[trigger] sum(rows, f) == sum(tab, f)
}

pub open spec fn finite_nums(nums: Seq<NumRt>) -> bool {
    forall|i: int| 0 <= i < nums.len() ==> (#[trigger] nums[i]).value__null != 0 || f64_finite(nums[i].value)
}

impl State {
    pub closed spec fn db(self) -> Db {
        self.db@
    }

    #[verifier::type_invariant]
    closed spec fn holds(self) -> bool {
        &&& same_sums(rows_v(self.nums@), self.db@.num)
        &&& same_sums(rows_v(self.subs@), self.db@.sub)
        &&& finite_nums(self.nums@)
        &&& bag_holds::<SubRt>(self.sub_bag, self.db@.sub)
        &&& consts_ok(*self.consts)
    }
}

pub open spec fn history(s: State) -> nat {
    0
}

pub open spec fn rescan_nums_by_adsh_wanted(s: State, num_delta: Seq<NumDelta>, sub_delta: Seq<SubDelta>) -> Seq<Vec<u8>> {
    Seq::empty()
}

/// A num row with a value that is no double is outside the envelope.
proof fn lemma_nonfinite_outside(db: Db, nums: Seq<NumRt>, i: int)
    requires
        same_sums(rows_v(nums), db.num),
        0 <= i < nums.len(),
        nums[i].value__null == 0,
        !f64_finite(nums[i].value),
    ensures
        !envelope(db),
{
    lemma_sums_eq_multiset(rows_v(nums), db.num);
    let row = nums[i].rv();
    assert(rows_v(nums)[i] == row);
    assert(rows_v(nums).contains(row));
    assert(rows_v(nums).to_multiset().count(row) > 0) by {
        broadcast use vstd::seq_lib::group_to_multiset_ensures;
    }
    assert(db.num.to_multiset().count(row) > 0);
    assert(db.num.contains(row)) by {
        broadcast use vstd::seq_lib::group_to_multiset_ensures;
    }
    let j = choose|j: int| 0 <= j < db.num.len() && db.num[j] == row;
    lemma_f64_in_range(nums[i].value);
    assert(!num_in_type(db.num[j]));
}

fn all_finite(nums: &Vec<NumRt>) -> (r: bool)
    ensures r == finite_nums(nums@),
{
    let mut i: usize = 0;
    while i < nums.len()
        invariant i <= nums.len(), forall|j: int| 0 <= j < i ==> (#[trigger] nums[j]).value__null != 0 || f64_finite(nums[j].value),
        decreases nums.len() - i,
    {
        if nums[i].value__null == 0 && (nums[i].value / 4503599627370496) % 2048 == 2047 {
            proof { lemma_pow2_consts(); assert(!f64_finite(nums[i as int].value)); }
            return false;
        }
        proof { lemma_pow2_consts(); }
        i += 1;
    }
    true
}

pub fn build(
    sub: Vec<SubRt>,
    nums: Vec<NumRt>,
    Ghost(db): Ghost<Db>,
) -> (r: Option<State>)
    requires
        scan_sub_ok(db, sub@),
        scan_nums_ok(db, nums@),
    ensures
        r matches Some(s) ==> s.db() == db,
        r matches Some(s) ==> crate::kernel::history(s) == 0,
        envelope(db) ==> r is Some,
{
    proof {
        assert(rows_v(nums@) =~= num_rt_seq_view(nums@));
        assert(rows_v(sub@) =~= sub_rt_seq_view(sub@));
    }
    if !all_finite(&nums) {
        proof {
            let i = choose|i: int| 0 <= i < nums.len() && !(nums[i].value__null != 0 || f64_finite(nums[i].value));
            lemma_nonfinite_outside(db, nums@, i);
        }
        return None;
    }
    let sub_bag = bag_build(&sub);
    proof { lemma_bag_holds_transfer::<SubRt>(sub_bag, rows_v(sub@), db.sub); }
    let consts = Arc::new(consts_new());
    Some(State { nums, subs: sub, sub_bag, consts, db: Ghost(db) })
}

pub fn execute(
    s: &State,
    p1: i64,
    p2: &[u8],
) -> (r: Option<Vec<(Option<Vec<u8>>, Option<i64>, Option<u64>)>>)
    ensures
        r matches Some(out) ==> query_result(s.db(), params(p1 as int, p2@), out_view(out@)),
        envelope(s.db()) && answer_fits(s.db(), params(p1 as int, p2@)) ==> r is Some,
        r matches Some(out) ==> forall|i: int| 0 <= i < out.len() ==> out_fits(#[trigger] out_view(out@)[i]),
{
    proof { use_type_invariant(s); }
    answer(&s.nums, &s.sub_bag, p1, p2, &*s.consts, Ghost(s.db@))
}

pub fn rescan_nums_by_adsh_keys(
    s: &State,
    num_delta: &Vec<NumDelta>,
    sub_delta: &Vec<SubDelta>,
) -> (r: Vec<Vec<u8>>)
    ensures r@ == crate::kernel::rescan_nums_by_adsh_wanted(*s, num_delta@, sub_delta@),
{
    Vec::new()
}

/// A table with its changed rows applied.
fn apply_table<R: BagRow>(old: &Vec<R>, d: &Vec<(R, i64)>, Ghost(before): Ghost<Seq<R::V>>, Ghost(after): Ghost<Seq<R::V>>) -> (r: Vec<R>)
    requires
        same_sums(rows_v(old@), before),
        bag_step(before, after, dview(d@)),
        unit_signs(dview(d@)),
    ensures
        same_sums(rows_v(r@), after),
{
    broadcast use vstd::multiset::group_multiset_axioms;
    proof {
        lemma_sums_eq_multiset(rows_v(old@), before);
        lemma_bag_step_multiset(before, after, dview(d@));
    }
    match apply_bag(old, d) {
        Some(v) => {
            proof {
                assert(rows_v(old@).to_multiset() == before.to_multiset());
                lemma_apply_bag_step(old@, v@, d@, before, after);
                assert forall|f: spec_fn(R::V) -> int| #[trigger] sum(rows_v(v@), f) == sum(after, f) by {
                    lemma_sum_perm(rows_v(v@), after, f);
                }
            }
            v
        },
        None => {
            proof {
                let x = choose|x: R::V| #[trigger] minus_rows(dview(d@)).to_multiset().count(x)
                    > rows_v(old@).to_multiset().add(plus_rows(dview(d@)).to_multiset()).count(x);
                assert(rows_v(old@).to_multiset() == before.to_multiset());
                assert(after.to_multiset().add(minus_rows(dview(d@)).to_multiset()).count(x)
                    == before.to_multiset().add(plus_rows(dview(d@)).to_multiset()).count(x));
                assert(false);
            }
            Vec::new()
        },
    }
}

fn pairs_num(d: &Vec<NumDelta>) -> (r: Vec<(NumRt, i64)>)
    ensures dview(r@) == num_delta_seq_view(d@),
{
    let mut r: Vec<(NumRt, i64)> = Vec::new();
    let mut i: usize = 0;
    while i < d.len()
        invariant i <= d.len(), dview(r@) == num_delta_seq_view(d@.take(i as int)),
        decreases d.len() - i,
    {
        let ghost old_r = r@;
        let row = d[i].row.dup();
        r.push((row, d[i].sign));
        proof {
            assert(d@.take(i as int + 1) =~= d@.take(i as int).push(d@[i as int]));
            assert(r@ =~= old_r.push((row, d[i as int].sign)));
            assert(dview(r@) =~= dview(old_r).push((row.rv(), d[i as int].sign as int)));
            assert(num_delta_seq_view(d@.take(i as int + 1)) =~= num_delta_seq_view(d@.take(i as int)).push((num_delta_seq_view(d@).index(i as int))));
            assert(dview(r@) =~= num_delta_seq_view(d@.take(i as int + 1)));
        }
        i += 1;
    }
    proof { assert(d@.take(d.len() as int) =~= d@); }
    r
}

fn pairs_sub(d: &Vec<SubDelta>) -> (r: Vec<(SubRt, i64)>)
    ensures dview(r@) == sub_delta_seq_view(d@),
{
    let mut r: Vec<(SubRt, i64)> = Vec::new();
    let mut i: usize = 0;
    while i < d.len()
        invariant i <= d.len(), dview(r@) == sub_delta_seq_view(d@.take(i as int)),
        decreases d.len() - i,
    {
        let ghost old_r = r@;
        let row = d[i].row.dup();
        r.push((row, d[i].sign));
        proof {
            assert(d@.take(i as int + 1) =~= d@.take(i as int).push(d@[i as int]));
            assert(r@ =~= old_r.push((row, d[i as int].sign)));
            assert(dview(r@) =~= dview(old_r).push((row.rv(), d[i as int].sign as int)));
            assert(sub_delta_seq_view(d@.take(i as int + 1)) =~= sub_delta_seq_view(d@.take(i as int)).push((sub_delta_seq_view(d@).index(i as int))));
            assert(dview(r@) =~= sub_delta_seq_view(d@.take(i as int + 1)));
        }
        i += 1;
    }
    proof { assert(d@.take(d.len() as int) =~= d@); }
    r
}

pub fn apply(
    s: &State,
    num_delta: &Vec<NumDelta>,
    sub_delta: &Vec<SubDelta>,
    nums_by_adsh_keys: &Vec<Vec<u8>>,
    nums_by_adsh_rows: &Vec<NumRt>,
    Ghost(db2): Ghost<Db>,
) -> (r: Result<State, u32>)
    requires
        capture_ok(s.db(), db2, num_delta_seq_view(num_delta@), sub_delta_seq_view(sub_delta@)),
        nums_by_adsh_keys@ == crate::kernel::rescan_nums_by_adsh_wanted(*s, num_delta@, sub_delta@),
        rescan_nums_by_adsh_ok(db2, nums_by_adsh_keys@, nums_by_adsh_rows@),
    ensures
        r matches Ok(s2) ==> s2.db() == db2,
        r matches Ok(s2) ==> crate::kernel::history(s2) <= crate::kernel::history(*s) + num_delta@.len() + sub_delta@.len(),
        envelope(s.db()) && envelope(db2) && crate::kernel::history(*s) + num_delta@.len() + sub_delta@.len() <= max_history() ==> r is Ok,
{
    proof { use_type_invariant(s); }
    let ghost db = s.db@;
    let nums = apply_table(&s.nums, &pairs_num(num_delta), Ghost(db.num), Ghost(db2.num));
    let subs = apply_table(&s.subs, &pairs_sub(sub_delta), Ghost(db.sub), Ghost(db2.sub));
    if !all_finite(&nums) {
        proof {
            let i = choose|i: int| 0 <= i < nums.len() && !(nums[i].value__null != 0 || f64_finite(nums[i].value));
            lemma_nonfinite_outside(db2, nums@, i);
        }
        return Err(1);
    }
    let sub_bag = bag_build(&subs);
    proof { lemma_bag_holds_transfer::<SubRt>(sub_bag, rows_v(subs@), db2.sub); }
    Ok(State { nums, subs, sub_bag, consts: s.consts.clone(), db: Ghost(db2) })
}

}
