// Transaction deltas as signed row pairs.
use vstd::prelude::*;
use verus_common::bag::*;
use crate::gen_obligations::*;
use crate::gen_spec::*;
use crate::rows::*;

verus! {

pub fn pairs_num(d: &Vec<NumDelta>) -> (r: Vec<(NumRt, i64)>)
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

pub fn pairs_pre(d: &Vec<PreDelta>) -> (r: Vec<(PreRt, i64)>)
    ensures dview(r@) == pre_delta_seq_view(d@),
{
    let mut r: Vec<(PreRt, i64)> = Vec::new();
    let mut i: usize = 0;
    while i < d.len()
        invariant i <= d.len(), dview(r@) == pre_delta_seq_view(d@.take(i as int)),
        decreases d.len() - i,
    {
        let ghost old_r = r@;
        let row = d[i].row.dup();
        r.push((row, d[i].sign));
        proof {
            assert(d@.take(i as int + 1) =~= d@.take(i as int).push(d@[i as int]));
            assert(r@ =~= old_r.push((row, d[i as int].sign)));
            assert(dview(r@) =~= dview(old_r).push((row.rv(), d[i as int].sign as int)));
            assert(pre_delta_seq_view(d@.take(i as int + 1)) =~= pre_delta_seq_view(d@.take(i as int)).push((pre_delta_seq_view(d@).index(i as int))));
            assert(dview(r@) =~= pre_delta_seq_view(d@.take(i as int + 1)));
        }
        i += 1;
    }
    proof { assert(d@.take(d.len() as int) =~= d@); }
    r
}

pub fn pairs_sub(d: &Vec<SubDelta>) -> (r: Vec<(SubRt, i64)>)
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

pub fn pairs_tag(d: &Vec<TagDelta>) -> (r: Vec<(TagRt, i64)>)
    ensures dview(r@) == tag_delta_seq_view(d@),
{
    let mut r: Vec<(TagRt, i64)> = Vec::new();
    let mut i: usize = 0;
    while i < d.len()
        invariant i <= d.len(), dview(r@) == tag_delta_seq_view(d@.take(i as int)),
        decreases d.len() - i,
    {
        let ghost old_r = r@;
        let row = d[i].row.dup();
        r.push((row, d[i].sign));
        proof {
            assert(d@.take(i as int + 1) =~= d@.take(i as int).push(d@[i as int]));
            assert(r@ =~= old_r.push((row, d[i as int].sign)));
            assert(dview(r@) =~= dview(old_r).push((row.rv(), d[i as int].sign as int)));
            assert(tag_delta_seq_view(d@.take(i as int + 1)) =~= tag_delta_seq_view(d@.take(i as int)).push((tag_delta_seq_view(d@).index(i as int))));
            assert(dview(r@) =~= tag_delta_seq_view(d@.take(i as int + 1)));
        }
        i += 1;
    }
    proof { assert(d@.take(d.len() as int) =~= d@); }
    r
}

}
