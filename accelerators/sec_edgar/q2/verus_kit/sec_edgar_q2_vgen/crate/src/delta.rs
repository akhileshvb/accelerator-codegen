// The filings a transaction touches, and that the others are left as they were.
use vstd::prelude::*;
use verus_common::sums::*;
use verus_common::order::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::model::*;
use crate::agree::*;
use crate::group::copy_vec;

verus! {

pub open spec fn num_delta_keys(nd: Seq<NumDelta>) -> Seq<Seq<u8>> {
    map_where(nd, |d: NumDelta| d.row.adsh__null == 0, |d: NumDelta| d.row.adsh@)
}

pub open spec fn sub_delta_keys(sd: Seq<SubDelta>) -> Seq<Seq<u8>> {
    sd.map_values(|d: SubDelta| d.row.adsh@)
}

/// The adsh of every changed row of num and sub.
pub open spec fn delta_keys(nd: Seq<NumDelta>, sd: Seq<SubDelta>) -> Seq<Seq<u8>> {
    num_delta_keys(nd) + sub_delta_keys(sd)
}

pub proof fn lemma_map_where_push<A, T>(s: Seq<A>, x: A, keep: spec_fn(A) -> bool, f: spec_fn(A) -> T)
    ensures
        map_where(s.push(x), keep, f) == if keep(x) { map_where(s, keep, f).push(f(x)) } else { map_where(s, keep, f) },
{
    assert(s.push(x).drop_last() =~= s);
}

pub fn num_delta_key_vec(nd: &Vec<NumDelta>) -> (r: Vec<Vec<u8>>)
    ensures
        seq_view_bytes(r@) == num_delta_keys(nd@),
{
    let mut r: Vec<Vec<u8>> = Vec::new();
    let mut i: usize = 0;
    proof { assert(nd@.take(0) =~= Seq::<NumDelta>::empty()); }
    while i < nd.len()
        invariant
            i <= nd@.len(),
            seq_view_bytes(r@) == num_delta_keys(nd@.take(i as int)),
        decreases nd.len() - i,
    {
        proof {
            assert(nd@.take(i as int + 1) =~= nd@.take(i as int).push(nd@[i as int]));
            lemma_map_where_push(nd@.take(i as int), nd@[i as int], |d: NumDelta| d.row.adsh__null == 0, |d: NumDelta| d.row.adsh@);
        }
        if nd[i].row.adsh__null == 0 {
            let ghost before = r@;
            r.push(copy_vec(nd[i].row.adsh.as_slice()));
            proof { assert(seq_view_bytes(r@) =~= seq_view_bytes(before).push(nd@[i as int].row.adsh@)); }
        }
        i += 1;
    }
    proof { assert(nd@.take(nd@.len() as int) =~= nd@); }
    r
}

pub fn delta_key_vec(nd: &Vec<NumDelta>, sd: &Vec<SubDelta>) -> (r: Vec<Vec<u8>>)
    ensures
        seq_view_bytes(r@) == delta_keys(nd@, sd@),
{
    let mut r = num_delta_key_vec(nd);
    let ghost nk = seq_view_bytes(r@);
    let mut i: usize = 0;
    proof { assert(sd@.take(0) =~= Seq::<SubDelta>::empty()); }
    while i < sd.len()
        invariant
            i <= sd@.len(),
            nk == num_delta_keys(nd@),
            seq_view_bytes(r@) == nk + sub_delta_keys(sd@.take(i as int)),
        decreases sd.len() - i,
    {
        let ghost before = r@;
        r.push(copy_vec(sd[i].row.adsh.as_slice()));
        proof {
            assert(sd@.take(i as int + 1) =~= sd@.take(i as int).push(sd@[i as int]));
            assert(seq_view_bytes(r@) =~= seq_view_bytes(before).push(sd@[i as int].row.adsh@));
            assert(sub_delta_keys(sd@.take(i as int + 1)) =~= sub_delta_keys(sd@.take(i as int)).push(sd@[i as int].row.adsh@));
            assert(nk + sub_delta_keys(sd@.take(i as int + 1)) =~= (nk + sub_delta_keys(sd@.take(i as int))).push(sd@[i as int].row.adsh@));
        }
        i += 1;
    }
    proof {
        assert(sd@.take(sd@.len() as int) =~= sd@);
        assert(sub_delta_keys(sd@) =~= sub_delta_keys(sd@.take(sd@.len() as int)));
    }
    r
}

/// Filings whose rows no changed row belongs to are the same in both databases.
pub proof fn lemma_delta_agree(db: Db, db2: Db, nd: Seq<NumDelta>, sd: Seq<SubDelta>, a: Seq<u8>)
    requires
        capture_ok(db, db2, num_delta_seq_view(nd), sub_delta_seq_view(sd)),
        !delta_keys(nd, sd).contains(a),
    ensures
        agree(db, db2, a),
{
    let nv = num_delta_seq_view(nd);
    let sv = sub_delta_seq_view(sd);
    lemma_map_where(nd, |d: NumDelta| d.row.adsh__null == 0, |d: NumDelta| d.row.adsh@);
    assert forall|f: spec_fn(NumRow) -> int| (forall|n: NumRow| n.adsh != Some(a) ==> #[trigger] f(n) == 0) implies #[trigger] sum(db.num, f) == sum(db2.num, f) by {
        let w = |d: (NumRow, int)| d.1 * f(d.0);
        assert forall|i: int| 0 <= i < nv.len() implies w(#[trigger] nv[i]) == 0 by {
            let d = nd[i];
            assert(nv[i] == (num_input_view(d.row), d.sign as int));
            if d.row.adsh__null == 0 {
                assert(num_delta_keys(nd).contains(d.row.adsh@));
                assert(delta_keys(nd, sd).contains(d.row.adsh@)) by {
                    let k = choose|k: int| 0 <= k < num_delta_keys(nd).len() && num_delta_keys(nd)[k] == d.row.adsh@;
                    assert(delta_keys(nd, sd)[k] == num_delta_keys(nd)[k]);
                }
                assert(num_input_view(d.row).adsh == Some(d.row.adsh@));
                assert(d.row.adsh@ != a);
            }
            assert(f(num_input_view(d.row)) == 0);
        }
        lemma_sum_zero(nv, w);
        assert(sum(db2.num, f) == sum(db.num, f) + sum(nv, w));
    }
    assert forall|f: spec_fn(SubRow) -> int| (forall|s: SubRow| s.adsh != a ==> #[trigger] f(s) == 0) implies #[trigger] sum(db.sub, f) == sum(db2.sub, f) by {
        let w = |d: (SubRow, int)| d.1 * f(d.0);
        assert forall|i: int| 0 <= i < sv.len() implies w(#[trigger] sv[i]) == 0 by {
            let d = sd[i];
            assert(sv[i] == (sub_input_view(d.row), d.sign as int));
            assert(sub_delta_keys(sd)[i] == d.row.adsh@);
            assert(delta_keys(nd, sd)[num_delta_keys(nd).len() + i] == d.row.adsh@);
            assert(d.row.adsh@ != a);
            assert(f(sub_input_view(d.row)) == 0);
        }
        lemma_sum_zero(sv, w);
        assert(sum(db2.sub, f) == sum(db.sub, f) + sum(sv, w));
    }
}

} // verus!
