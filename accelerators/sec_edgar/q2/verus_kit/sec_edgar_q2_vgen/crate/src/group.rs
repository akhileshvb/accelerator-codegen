// The rescanned num rows of some filings, as the rows holding their group's maximum, per filing.
use vstd::prelude::*;
use std::sync::Arc;
use verus_common::bytes::*;
use verus_common::sums::*;
use verus_common::rel::*;
use verus_common::sort::*;
use verus_prooflib::ordered::{Index, Key, KeyView, compare_bytes};
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::model::*;
use crate::keys::*;
use crate::f64bits::*;
use crate::cells::kkey;

verus! {

pub open spec fn gk(r: NumInput) -> GK {
    GK { adsh: r.adsh@, tag: r.tag@, uom: r.uom@, dk: f64_desc_key(r.value) }
}

pub open spec fn qual_in(r: NumInput) -> bool {
    is_qual(num_input_view(r))
}

pub open spec fn pick_rows(rows: Seq<NumInput>, s: Seq<usize>) -> Seq<NumInput> {
    s.map_values(|i: usize| rows[i as int])
}

pub open spec fn keys_of(si: Seq<NumInput>) -> Seq<GK> {
    si.map_values(|r: NumInput| gk(r))
}

pub fn qual_exec(r: &NumInput) -> (b: bool)
    ensures b == qual_in(*r),
{
    r.adsh__null == 0 && r.tag__null == 0 && r.uom__null == 0 && r.value__null == 0
}

pub fn lt4_exec(a: &NumInput, b: &NumInput) -> (r: bool)
    ensures r == lt4(gk(*a), gk(*b)),
{
    let c = compare_bytes(a.adsh.as_slice(), b.adsh.as_slice());
    if c != 0 { return c < 0; }
    let c = compare_bytes(a.tag.as_slice(), b.tag.as_slice());
    if c != 0 { return c < 0; }
    let c = compare_bytes(a.uom.as_slice(), b.uom.as_slice());
    if c != 0 { return c < 0; }
    desc_key(a.value) < desc_key(b.value)
}

/// The qualifying rows' positions, sorted by (adsh, tag, uom, value descending); None when one is not finite.
pub fn sorted_qual(rows: &Vec<NumInput>) -> (r: Option<Vec<usize>>)
    ensures
        r matches Some(s) ==> {
            &&& forall|k: int| 0 <= k < s@.len() ==> (#[trigger] s@[k]) < rows@.len() && qual_in(rows@[s@[k] as int]) && f64_finite(rows@[s@[k] as int].value)
            &&& sorted4(keys_of(pick_rows(rows@, s@)))
            &&& forall|f: spec_fn(NumRow) -> int| #[trigger] sum(num_input_seq_view(pick_rows(rows@, s@)), f)
                == sum(num_input_seq_view(rows@), |n: NumRow| if is_qual(n) { f(n) } else { 0 })
            &&& forall|i: int| 0 <= i < rows@.len() && qual_in(rows@[i]) ==> f64_finite(#[trigger] rows@[i].value)
        },
        r is None ==> exists|i: int| 0 <= i < rows@.len() && qual_in(rows@[i]) && !f64_finite(rows@[i].value),
{
    let mut qs: Vec<usize> = Vec::new();
    let mut i: usize = 0;
    while i < rows.len()
        invariant
            i <= rows@.len(),
            forall|j: int| 0 <= j < i && qual_in(rows@[j]) ==> f64_finite(#[trigger] rows@[j].value),
            forall|k: int| 0 <= k < qs@.len() ==> (#[trigger] qs@[k]) < i && qual_in(rows@[qs@[k] as int]) && f64_finite(rows@[qs@[k] as int].value),
            forall|f: spec_fn(NumRow) -> int| #[trigger] sum(num_input_seq_view(pick_rows(rows@, qs@)), f)
                == sum(num_input_seq_view(rows@.take(i as int)), |n: NumRow| if is_qual(n) { f(n) } else { 0 }),
        decreases rows.len() - i,
    {
        let r = &rows[i];
        let q = qual_exec(r);
        let ghost qs0 = qs@;
        if q {
            if !is_finite(r.value) {
                return None;
            }
            qs.push(i);
        }
        proof {
            let take1 = rows@.take(i as int + 1);
            assert(take1 =~= rows@.take(i as int).push(rows@[i as int]));
            assert forall|f: spec_fn(NumRow) -> int| #[trigger] sum(num_input_seq_view(pick_rows(rows@, qs@)), f)
                == sum(num_input_seq_view(take1), |n: NumRow| if is_qual(n) { f(n) } else { 0 }) by {
                let g = |n: NumRow| if is_qual(n) { f(n) } else { 0 };
                assert(num_input_seq_view(take1) =~= num_input_seq_view(rows@.take(i as int)).push(num_input_view(rows@[i as int])));
                lemma_sum_push(num_input_seq_view(rows@.take(i as int)), num_input_view(rows@[i as int]), g);
                if q {
                    assert(pick_rows(rows@, qs@) =~= pick_rows(rows@, qs0).push(rows@[i as int]));
                    assert(num_input_seq_view(pick_rows(rows@, qs@)) =~= num_input_seq_view(pick_rows(rows@, qs0)).push(num_input_view(rows@[i as int])));
                    lemma_sum_push(num_input_seq_view(pick_rows(rows@, qs0)), num_input_view(rows@[i as int]), f);
                }
            }
            assert(forall|k: int| 0 <= k < qs@.len() ==> (#[trigger] qs@[k]) < i + 1) by {
                assert forall|k: int| 0 <= k < qs@.len() implies (#[trigger] qs@[k]) < i + 1 by {
                    if k < qs0.len() { assert(qs@[k] == qs0[k]); }
                }
            }
            assert forall|k: int| 0 <= k < qs@.len() implies qual_in(rows@[qs@[k] as int]) && f64_finite(rows@[qs@[k] as int].value) by {
                if k < qs0.len() { assert(qs@[k] == qs0[k]); }
            }
        }
        proof {
            assert forall|j: int| 0 <= j < i + 1 && qual_in(rows@[j]) implies f64_finite(#[trigger] rows@[j].value) by {
                if j == i { assert(q); }
            }
        }
        i += 1;
    }
    proof { assert(rows@.take(rows@.len() as int) =~= rows@); }
    let ghost rv = rows@;
    let ghost le = |x: usize, y: usize| le4(gk(rv[x as int]), gk(rv[y as int]));
    let ghost validg = |x: usize| x < rv.len();
    let rows_ref = rows;
    let compare = |x: usize, y: usize| -> (r: bool)
        requires x < rows_ref.len(), y < rows_ref.len(),
        ensures r == le4(gk(rows_ref@[x as int]), gk(rows_ref@[y as int])),
    { !lt4_exec(&rows_ref[y], &rows_ref[x]) };
    proof {
        assert forall|x: usize, y: usize| validg(x) && validg(y) implies #[trigger] le(x, y) || le(y, x) by {
            lemma_le4_total(gk(rv[x as int]), gk(rv[y as int]));
        }
        assert forall|x: usize, y: usize, z: usize| validg(x) && validg(y) && validg(z) && #[trigger] le(x, y) && #[trigger] le(y, z) implies le(x, z) by {
            lemma_le4_trans(gk(rv[x as int]), gk(rv[y as int]), gk(rv[z as int]));
        }
        assert(preorder_on(le, validg));
        assert(all_valid(qs@, validg));
    }
    let sorted = sort_idx(&qs, &compare, Ghost(le), Ghost(validg));
    proof {
        let sv = pick_rows(rv, sorted@);
        assert(sorted_le(sorted@, le));
        assert forall|p: int, q: int| 0 <= p < q < keys_of(sv).len() implies le4(#[trigger] keys_of(sv)[p], #[trigger] keys_of(sv)[q]) by {
            assert(le(sorted@[p], sorted@[q]));
        }
        assert forall|f: spec_fn(NumRow) -> int| #[trigger] sum(num_input_seq_view(sv), f)
            == sum(num_input_seq_view(rv), |n: NumRow| if is_qual(n) { f(n) } else { 0 }) by {
            let fi = |x: usize| f(num_input_view(rv[x as int]));
            assert(sum(sorted@, fi) == sum(qs@, fi));
            lemma_sum_map(sorted@, |i: usize| num_input_view(rv[i as int]), f, fi);
            lemma_sum_map(qs@, |i: usize| num_input_view(rv[i as int]), f, fi);
            assert(num_input_seq_view(sv) =~= sorted@.map_values(|i: usize| num_input_view(rv[i as int])));
            assert(num_input_seq_view(pick_rows(rv, qs@)) =~= qs@.map_values(|i: usize| num_input_view(rv[i as int])));
        }
        assert forall|k: int| 0 <= k < sorted@.len() implies (#[trigger] sorted@[k]) < rows@.len() && qual_in(rows@[sorted@[k] as int]) && f64_finite(rows@[sorted@[k] as int].value) by {
            let x = sorted@[k];
            let fq = |z: usize| if qual_in(rv[z as int]) && f64_finite(rv[z as int].value) { 0int } else { 1int };
            assert forall|j: int| 0 <= j < qs@.len() implies fq(#[trigger] qs@[j]) == 0 by {}
            lemma_sum_zero(qs@, fq);
            assert(sum(sorted@, fq) == 0);
            assert forall|j: int| 0 <= j < sorted@.len() implies fq(#[trigger] sorted@[j]) >= 0 by {}
            if fq(x) != 0 { lemma_sum_pos(sorted@, fq, k); }
        }
    }
    Some(sorted)
}


/// A row holding its group's maximum value; its filing is the key it is stored under.
pub struct MR {
    pub tag: Arc<Vec<u8>>,
    pub uom: Arc<Vec<u8>>,
    pub val: u64,
}

pub open spec fn mr_view(a: Seq<u8>, t: MR) -> NumRow {
    NumRow { adsh: Some(a), tag: Some((*t.tag)@), uom: Some((*t.uom)@), value: Some(f64_val(t.val)) }
}

pub open spec fn mr_at(idx: Index<Vec<MR>>, a: Seq<u8>) -> Seq<MR> {
    match idx.at((0int, a)) { Some(v) => v@, None => Seq::empty() }
}

pub open spec fn mr_views(a: Seq<u8>, ts: Seq<MR>) -> Seq<NumRow> {
    ts.map_values(|t: MR| mr_view(a, t))
}

pub open spec fn mr_w(idx: Index<Vec<MR>>, a: Seq<u8>, h: spec_fn(NumRow) -> int) -> int {
    sum(mr_views(a, mr_at(idx, a)), h)
}

pub open spec fn top_w(si: Seq<NumInput>, ks: Seq<GK>, j: int, a: Seq<u8>, h: spec_fn(NumRow) -> int) -> int {
    sum(Seq::new(j as nat, |p: int| (num_input_view(si[p]), grp_top(ks, p))),
        |x: (NumRow, bool)| if x.1 && x.0.adsh == Some(a) { h(x.0) } else { 0 })
}

pub fn copy_vec(src: &[u8]) -> (r: Vec<u8>)
    ensures r@ == src@,
{
    vstd::slice::slice_to_vec(src)
}

pub fn copy_mrs(src: &Vec<MR>) -> (out: Vec<MR>)
    ensures out@ == src@,
{
    let mut out: Vec<MR> = Vec::new();
    let mut i: usize = 0;
    while i < src.len()
        invariant
            i <= src@.len(),
            out@ == src@.take(i as int),
        decreases src.len() - i,
    {
        out.push(MR { tag: src[i].tag.clone(), uom: src[i].uom.clone(), val: src[i].val });
        proof { assert(src@.take(i as int + 1) =~= src@.take(i as int).push(src@[i as int])); }
        i += 1;
    }
    proof { assert(src@.take(src@.len() as int) =~= src@); }
    out
}

pub proof fn lemma_top_w_push(si: Seq<NumInput>, ks: Seq<GK>, j: int, a: Seq<u8>, h: spec_fn(NumRow) -> int)
    requires
        0 <= j < si.len(),
    ensures
        top_w(si, ks, j + 1, a, h) == top_w(si, ks, j, a, h)
            + (if grp_top(ks, j) && num_input_view(si[j]).adsh == Some(a) { h(num_input_view(si[j])) } else { 0 }),
{
    let f = |p: int| (num_input_view(si[p]), grp_top(ks, p));
    let w = |x: (NumRow, bool)| if x.1 && x.0.adsh == Some(a) { h(x.0) } else { 0 };
    assert(Seq::new((j + 1) as nat, f) =~= Seq::new(j as nat, f).push(f(j)));
    lemma_sum_push(Seq::new(j as nat, f), f(j), w);
}

pub proof fn lemma_mr_views_push(a: Seq<u8>, ts: Seq<MR>, t: MR)
    ensures
        mr_views(a, ts.push(t)) == mr_views(a, ts).push(mr_view(a, t)),
{
    assert(mr_views(a, ts.push(t)) =~= mr_views(a, ts).push(mr_view(a, t)));
}

/// The rows of the sorted positions that top their group, stored per filing.
pub fn tops_by_filing(rows: &Vec<NumInput>, s: &Vec<usize>) -> (idx: Index<Vec<MR>>)
    requires
        forall|k: int| 0 <= k < s@.len() ==> (#[trigger] s@[k]) < rows@.len() && qual_in(rows@[s@[k] as int]) && f64_finite(rows@[s@[k] as int].value),
        sorted4(keys_of(pick_rows(rows@, s@))),
    ensures
        idx.wf(),
        forall|a: Seq<u8>, h: spec_fn(NumRow) -> int| #[trigger] mr_w(idx, a, h)
            == top_w(pick_rows(rows@, s@), keys_of(pick_rows(rows@, s@)), s@.len() as int, a, h),
        forall|a: Seq<u8>, k: int| 0 <= k < mr_at(idx, a).len() ==> f64_finite(#[trigger] mr_at(idx, a)[k].val),
{
    let ghost si = pick_rows(rows@, s@);
    let ghost ks = keys_of(si);
    let mut idx: Index<Vec<MR>> = Index::new();
    let mut cur: Vec<MR> = Vec::new();
    let mut prev_top = false;
    let mut j: usize = 0;
    proof {
        assert forall|a: Seq<u8>, h: spec_fn(NumRow) -> int| #[trigger] mr_w(idx, a, h) + (if false { 0int } else { 0 }) == top_w(si, ks, 0, a, h) by {
            assert(mr_views(a, mr_at(idx, a)) =~= Seq::<NumRow>::empty());
            lemma_sum_empty(Seq::<NumRow>::empty(), h);
            lemma_sum_empty(Seq::new(0 as nat, |p: int| (num_input_view(si[p]), grp_top(ks, p))), |x: (NumRow, bool)| if x.1 && x.0.adsh == Some(a) { h(x.0) } else { 0 });
        }
    }
    while j < s.len()
        invariant
            j <= s@.len(),
            si == pick_rows(rows@, s@),
            ks == keys_of(si),
            sorted4(ks),
            forall|k: int| 0 <= k < s@.len() ==> (#[trigger] s@[k]) < rows@.len() && qual_in(rows@[s@[k] as int]) && f64_finite(rows@[s@[k] as int].value),
            idx.wf(),
            prev_top == (j > 0 && grp_top(ks, j as int - 1)),
            j == 0 ==> cur@.len() == 0,
            forall|a: Seq<u8>, h: spec_fn(NumRow) -> int| #[trigger] mr_w(idx, a, h)
                + (if j > 0 && a == ks[j as int - 1].adsh { sum(mr_views(a, cur@), h) } else { 0 })
                == top_w(si, ks, j as int, a, h),
            j > 0 ==> mr_at(idx, ks[j as int - 1].adsh).len() == 0,
            forall|a: Seq<u8>| #[trigger] idx.at((0int, a)) is Some ==> j > 0 && bytes_lt(a, ks[j as int - 1].adsh),
            forall|a: Seq<u8>, k: int| 0 <= k < mr_at(idx, a).len() ==> f64_finite(#[trigger] mr_at(idx, a)[k].val),
            forall|k: int| 0 <= k < cur@.len() ==> f64_finite((#[trigger] cur@[k]).val),
        decreases s.len() - j,
    {
        let r = &rows[s[j]];
        proof {
            lemma_grp_top_rule(ks, j as int);
            assert(ks[j as int] == gk(rows@[s@[j as int] as int]));
            assert(si[j as int] == rows@[s@[j as int] as int]);
            if j > 0 {
                assert(si[j as int - 1] == rows@[s@[j as int - 1] as int]);
                assert(ks[j as int - 1] == gk(rows@[s@[j as int - 1] as int]));
                assert(le4(ks[j as int - 1], ks[j as int]));
            }
        }
        let ghost idx0 = idx;
        let ghost cur0 = cur@;
        let ghost aj = ks[j as int].adsh;
        let ghost prev_adsh = if j > 0 { ks[j as int - 1].adsh } else { Seq::empty() };
        let changed = if j == 0 { true } else { !bytes_eq_exec(rows[s[j - 1]].adsh.as_slice(), r.adsh.as_slice()) };
        proof {
            if j > 0 {
                assert(changed == (prev_adsh != aj));
                if changed {
                    lemma_bytes_lt_total(prev_adsh, aj);
                    assert(!lt3(ks[j as int], ks[j as int - 1]));
                    assert(bytes_lt(prev_adsh, aj));
                }
            }
        }
        if j > 0 && changed {
            let akey = Arc::new(copy_vec(rows[s[j - 1]].adsh.as_slice()));
            let flushed = cur;
            cur = Vec::new();
            proof { assert((*akey)@ == prev_adsh); }
            idx.put(kkey(0, &akey), Arc::new(flushed));
        }
        let ghost idx1 = idx;
        let ghost cur1 = cur@;
        let same_grp_prev = !changed && bytes_eq_exec(rows[s[j - 1]].tag.as_slice(), r.tag.as_slice())
            && bytes_eq_exec(rows[s[j - 1]].uom.as_slice(), r.uom.as_slice());
        let top = if !same_grp_prev { true } else { prev_top && desc_key(rows[s[j - 1]].value) == desc_key(r.value) };
        proof {
            assert(top == grp_top(ks, j as int));
            assert(same_grp_prev == (j > 0 && same3(ks[j as int - 1], ks[j as int])));
        }
        if top {
            cur.push(MR { tag: Arc::new(copy_vec(r.tag.as_slice())), uom: Arc::new(copy_vec(r.uom.as_slice())), val: r.value });
        }
        proof {
            let rv = num_input_view(*r);
            assert(rv == num_input_view(si[j as int]));
            assert(qual_in(*r));
            assert(rv.adsh == Some(aj) && rv.tag is Some && rv.uom is Some && rv.value is Some);
            if top {
                let t = cur@[cur@.len() - 1];
                assert(cur@ == cur1.push(t));
                assert(mr_view(aj, t) == rv);
                assert(f64_finite(t.val));
            } else {
                assert(cur@ == cur1);
            }
            assert forall|a: Seq<u8>, h: spec_fn(NumRow) -> int| #[trigger] mr_w(idx, a, h)
                + (if a == aj { sum(mr_views(a, cur@), h) } else { 0 }) == top_w(si, ks, j as int + 1, a, h) by {
                lemma_top_w_push(si, ks, j as int, a, h);
                if top {
                    let t = cur@[cur@.len() - 1];
                    lemma_mr_views_push(a, cur1, t);
                    lemma_sum_push(mr_views(a, cur1), mr_view(a, t), h);
                    if a == aj { assert(mr_view(a, t) == rv); }
                }
                assert(mr_w(idx0, a, h) + (if j > 0 && a == ks[j as int - 1].adsh { sum(mr_views(a, cur0), h) } else { 0 }) == top_w(si, ks, j as int, a, h));
                if j > 0 && changed {
                    assert(cur1.len() == 0);
                    assert(idx0.at((0int, aj)) is None) by {
                        if idx0.at((0int, aj)) is Some { assert(bytes_lt(aj, prev_adsh)); lemma_bytes_lt_asym(aj, prev_adsh); }
                    }
                    assert(mr_at(idx1, prev_adsh) == cur0);
                    if a == prev_adsh {
                        lemma_sum_empty(Seq::<NumRow>::empty(), h);
                        assert(mr_at(idx0, prev_adsh).len() == 0);
                        assert(mr_views(a, mr_at(idx0, a)) =~= Seq::<NumRow>::empty());
                        assert(mr_w(idx0, a, h) == 0);
                        assert(mr_w(idx1, a, h) == sum(mr_views(a, cur0), h));
                        assert(top_w(si, ks, j as int + 1, a, h) == top_w(si, ks, j as int, a, h));
                    } else {
                        assert(mr_at(idx1, a) == mr_at(idx0, a));
                        assert(mr_w(idx1, a, h) == mr_w(idx0, a, h));
                    }
                } else if j > 0 {
                    assert(aj == prev_adsh);
                    assert(idx1 == idx0);
                } else {
                    assert(idx1 == idx0);
                    lemma_sum_empty(Seq::<NumRow>::empty(), h);
                }
            }
            assert(forall|a: Seq<u8>| #[trigger] idx.at((0int, a)) is Some ==> bytes_lt(a, aj)) by {
                assert forall|a: Seq<u8>| #[trigger] idx.at((0int, a)) is Some implies bytes_lt(a, aj) by {
                    if j > 0 && changed {
                        if a == prev_adsh { } else {
                            assert(idx0.at((0int, a)) is Some);
                            lemma_bytes_lt_trans(a, prev_adsh, aj);
                        }
                    } else if j > 0 {
                        assert(idx.at((0int, a)) == idx0.at((0int, a)));
                    }
                }
            }
            assert(mr_at(idx, aj).len() == 0) by {
                if j > 0 && changed { assert(idx1.at((0int, aj)) == idx0.at((0int, aj))); }
            }
            assert forall|a: Seq<u8>, k: int| 0 <= k < mr_at(idx, a).len() implies f64_finite(#[trigger] mr_at(idx, a)[k].val) by {
                if j > 0 && changed && a == prev_adsh { assert(mr_at(idx1, a) == cur0); }
                else if j > 0 && changed { assert(mr_at(idx1, a) == mr_at(idx0, a)); }
            }
        }
        prev_top = top;
        j += 1;
    }
    let ghost idx0 = idx;
    let ghost cur0 = cur@;
    if s.len() > 0 {
        proof { assert(si[s@.len() as int - 1] == rows@[s@[s@.len() as int - 1] as int]); assert(ks[s@.len() as int - 1] == gk(rows@[s@[s@.len() as int - 1] as int])); }
        let akey = Arc::new(copy_vec(rows[s[s.len() - 1]].adsh.as_slice()));
        let flushed = cur;
        idx.put(kkey(0, &akey), Arc::new(flushed));
        proof {
            let m = s@.len() as int;
            let last = ks[m - 1].adsh;
            assert((*akey)@ == last);
            assert forall|a: Seq<u8>, h: spec_fn(NumRow) -> int| #[trigger] mr_w(idx, a, h) == top_w(si, ks, m, a, h) by {
                assert(mr_w(idx0, a, h) + (if a == last { sum(mr_views(a, cur0), h) } else { 0 }) == top_w(si, ks, m, a, h));
                if a == last {
                    lemma_sum_empty(Seq::<NumRow>::empty(), h);
                    assert(mr_views(a, mr_at(idx0, a)) =~= Seq::<NumRow>::empty());
                    assert(mr_at(idx, a) == cur0);
                } else {
                    assert(mr_at(idx, a) == mr_at(idx0, a));
                }
            }
            assert forall|a: Seq<u8>, k: int| 0 <= k < mr_at(idx, a).len() implies f64_finite(#[trigger] mr_at(idx, a)[k].val) by {
                if a == last { assert(mr_at(idx, a) == cur0); } else { assert(mr_at(idx, a) == mr_at(idx0, a)); }
            }
        }
    } else {
        proof {
            assert forall|a: Seq<u8>, h: spec_fn(NumRow) -> int| #[trigger] mr_w(idx, a, h) == top_w(si, ks, s@.len() as int, a, h) by {}
        }
    }
    idx
}


pub proof fn lemma_eqv_pos<A>(s: Seq<A>, x: A)
    ensures
        (sum(s, eqv(x)) > 0) == s.contains(x),
{
    assert forall|i: int| 0 <= i < s.len() implies eqv(x)(#[trigger] s[i]) >= 0 by {}
    if s.contains(x) {
        let k = choose|k: int| 0 <= k < s.len() && s[k] == x;
        lemma_sum_pos(s, eqv(x), k);
    }
    if sum(s, eqv(x)) > 0 {
        let k = lemma_sum_nonzero_witness(s, eqv(x));
        assert(s[k] == x);
    }
}

/// What the sorted view holds: the qualifying rows of the scan.
pub open spec fn sorted_view_ok(rows: Seq<NumInput>, s: Seq<usize>) -> bool {
    forall|f: spec_fn(NumRow) -> int| #[trigger] sum(num_input_seq_view(pick_rows(rows, s)), f)
        == sum(num_input_seq_view(rows), |n: NumRow| if is_qual(n) { f(n) } else { 0 })
}

pub proof fn lemma_sorted_mem(rows: Seq<NumInput>, s: Seq<usize>, x: NumRow)
    requires
        sorted_view_ok(rows, s),
    ensures
        num_input_seq_view(pick_rows(rows, s)).contains(x) == (num_input_seq_view(rows).contains(x) && is_qual(x)),
{
    let vs = num_input_seq_view(pick_rows(rows, s));
    let rn = num_input_seq_view(rows);
    let f = eqv(x);
    assert(sum(vs, f) == sum(rn, |n: NumRow| if is_qual(n) { f(n) } else { 0 }));
    lemma_eqv_pos(vs, x);
    if is_qual(x) {
        let q1 = |n: NumRow| if is_qual(n) { f(n) } else { 0 };
        assert forall|n: NumRow| #[trigger] q1(n) == f(n) by {
            if n == x {} else {}
        }
        lemma_sum_ext_all(rn, q1, f);
        lemma_eqv_pos(rn, x);
    } else {
        let q1 = |n: NumRow| if is_qual(n) { f(n) } else { 0 };
        let z = |n: NumRow| 0int;
        assert forall|n: NumRow| #[trigger] q1(n) == z(n) by {
            if n == x { assert(!is_qual(n)); }
        }
        lemma_sum_ext_all(rn, q1, z);
        lemma_sum_zero(rn, z);
    }
}

/// A sorted position tops its group exactly when its row holds the group maximum among all scanned rows.
pub proof fn lemma_top_is_max(rows: Seq<NumInput>, s: Seq<usize>, p: int)
    requires
        sorted_view_ok(rows, s),
        sorted4(keys_of(pick_rows(rows, s))),
        forall|k: int| 0 <= k < s.len() ==> (#[trigger] s[k]) < rows.len() && qual_in(rows[s[k] as int]) && f64_finite(rows[s[k] as int].value),
        0 <= p < s.len(),
    ensures
        grp_top(keys_of(pick_rows(rows, s)), p) == is_max(num_input_seq_view(rows), num_input_view(pick_rows(rows, s)[p])),
{
    let si = pick_rows(rows, s);
    let ks = keys_of(si);
    let rn = num_input_seq_view(rows);
    let np = num_input_view(si[p]);
    assert(si[p] == rows[s[p] as int]);
    // every scanned row of the group is some sorted position, and conversely
    assert forall|r: NumRow| rn.contains(r) && same_grp(np, r) && r.value is Some implies
        exists|q: int| 0 <= q < si.len() && num_input_view(si[q]) == r by {
        assert(is_qual(r));
        lemma_sorted_mem(rows, s, r);
        assert(num_input_seq_view(si).contains(r));
        let q = choose|q: int| 0 <= q < num_input_seq_view(si).len() && num_input_seq_view(si)[q] == r;
        assert(num_input_seq_view(si)[q] == num_input_view(si[q]));
    }
    if grp_top(ks, p) {
        assert forall|r: NumRow| rn.contains(r) && same_grp(np, r) && r.value is Some implies #[trigger] r.value->0 <= np.value->0 by {
            let q = choose|q: int| 0 <= q < si.len() && num_input_view(si[q]) == r;
            assert(si[q] == rows[s[q] as int]);
            assert(same3(ks[q], ks[p]));
            assert(ks[q].dk >= ks[p].dk);
            lemma_desc_key(si[p].value, si[q].value);
        }
    }
    if is_max(rn, np) {
        assert forall|q: int| 0 <= q < ks.len() && same3(ks[q], ks[p]) implies (#[trigger] ks[q]).dk >= ks[p].dk by {
            let r = num_input_view(si[q]);
            assert(si[q] == rows[s[q] as int]);
            assert(num_input_seq_view(si).contains(r)) by { assert(num_input_seq_view(si)[q] == r); }
            lemma_sorted_mem(rows, s, r);
            assert(same_grp(np, r));
            assert(r.value->0 <= np.value->0);
            lemma_desc_key(si[q].value, si[p].value);
        }
    }
}

pub proof fn lemma_tops_sem(rows: Seq<NumInput>, s: Seq<usize>, a: Seq<u8>, h: spec_fn(NumRow) -> int)
    requires
        sorted_view_ok(rows, s),
        sorted4(keys_of(pick_rows(rows, s))),
        forall|k: int| 0 <= k < s.len() ==> (#[trigger] s[k]) < rows.len() && qual_in(rows[s[k] as int]) && f64_finite(rows[s[k] as int].value),
    ensures
        top_w(pick_rows(rows, s), keys_of(pick_rows(rows, s)), s.len() as int, a, h)
            == sum(num_input_seq_view(rows), |n: NumRow| if is_qual(n) && n.adsh == Some(a) && is_max(num_input_seq_view(rows), n) { h(n) } else { 0 }),
{
    let si = pick_rows(rows, s);
    let ks = keys_of(si);
    let rn = num_input_seq_view(rows);
    let m = s.len() as int;
    let fl = Seq::new(m as nat, |p: int| (num_input_view(si[p]), grp_top(ks, p)));
    let phi = |x: (NumRow, bool)| if x.1 && x.0.adsh == Some(a) { h(x.0) } else { 0 };
    let g = |n: NumRow| if is_max(rn, n) && n.adsh == Some(a) { h(n) } else { 0 };
    let vs = num_input_seq_view(si);
    // flagged rows are the sorted view with its maximum flag
    let tag = |n: NumRow| (n, is_max(rn, n));
    assert(fl =~= vs.map_values(tag)) by {
        assert forall|p: int| 0 <= p < m implies fl[p] == vs.map_values(tag)[p] by {
            lemma_top_is_max(rows, s, p);
            assert(vs[p] == num_input_view(si[p]));
        }
    }
    lemma_sum_map(vs, tag, phi, g);
    let f = |n: NumRow| if is_max(rn, n) && n.adsh == Some(a) { h(n) } else { 0 };
    assert(sum(vs, f) == sum(rn, |n: NumRow| if is_qual(n) { f(n) } else { 0 }));
    let tgt = |n: NumRow| if is_qual(n) && n.adsh == Some(a) && is_max(rn, n) { h(n) } else { 0 };
    let q2 = |n: NumRow| if is_qual(n) { f(n) } else { 0 };
    assert forall|n: NumRow| #[trigger] q2(n) == tgt(n) by {}
    lemma_sum_ext_all(rn, q2, tgt);
}

} // verus!
