// The keys a transaction asks the host to rescan: the adsh of every changed filing row and the tag of
// every changed tag row, each once, and a set over them.
use vstd::prelude::*;
use std::sync::Arc;
use verus_common::rel::*;
use verus_prooflib::ordered::{Index, Key};
use crate::gen_obligations::*;
use crate::gen_spec::*;
use crate::keys::*;

verus! {

proof fn lemma_key1_inj(a: Seq<u8>, b: Seq<u8>)
    requires key1(a) == key1(b),
    ensures a == b,
{
    lemma_escape_split(a, Seq::empty(), b, Seq::empty());
    assert(escape(a) + Seq::<u8>::empty() =~= escape(a));
}

/// The set holds exactly these keys.
pub open spec fn set_holds(s: Index<()>, keys: Seq<Seq<u8>>) -> bool {
    &&& s.wf()
    &&& forall|x: Seq<u8>| #[trigger] s.at(key1(x)).is_some() == keys.contains(x)
}

proof fn lemma_contains_push<A>(s: Seq<A>, x: A, y: A)
    ensures s.push(x).contains(y) == (s.contains(y) || x == y),
{
    if s.push(x).contains(y) {
        let i = choose|i: int| 0 <= i < s.push(x).len() && s.push(x)[i] == y;
        if i < s.len() { assert(s[i] == y); }
    }
    if s.contains(y) {
        let i = choose|i: int| 0 <= i < s.len() && s[i] == y;
        assert(s.push(x)[i] == y);
    }
    if x == y {
        assert(s.push(x)[s.len() as int] == y);
    }
}

pub open spec fn views(keys: Seq<Vec<u8>>) -> Seq<Seq<u8>> {
    seq_view_bytes(keys)
}

/// The set of a list of keys.
pub fn key_set(keys: &Vec<Vec<u8>>) -> (s: Index<()>)
    ensures set_holds(s, seq_view_bytes(keys@)),
{
    let mut s: Index<()> = Index::new();
    let mut i: usize = 0;
    proof {
        assert forall|x: Seq<u8>| #[trigger] s.at(key1(x)).is_some() == seq_view_bytes(keys@.take(0)).contains(x) by {
            assert(seq_view_bytes(keys@.take(0)) =~= Seq::<Seq<u8>>::empty());
        }
    }
    while i < keys.len()
        invariant
            i <= keys.len(),
            set_holds(s, seq_view_bytes(keys@.take(i as int))),
        decreases keys.len() - i,
    {
        let ghost before = s;
        let k = make_key1(keys[i].as_slice());
        s.put(k.share(), Arc::new(()));
        proof {
            let vs = seq_view_bytes(keys@.take(i as int));
            assert(seq_view_bytes(keys@.take(i as int + 1)) =~= vs.push(keys@[i as int]@));
            assert forall|x: Seq<u8>| #[trigger] s.at(key1(x)).is_some() == vs.push(keys@[i as int]@).contains(x) by {
                lemma_contains_push(vs, keys@[i as int]@, x);
                if key1(x) == k.view() {
                    lemma_key1_inj(x, keys@[i as int]@);
                } else {
                    assert(s.at(key1(x)) == before.at(key1(x)));
                    if x == keys@[i as int]@ { assert(false); }
                }
            }
        }
        i += 1;
    }
    proof { assert(keys@.take(keys.len() as int) =~= keys@); }
    s
}

/// The key is in the set.
pub fn in_set(s: &Index<()>, x: &[u8], Ghost(keys): Ghost<Seq<Seq<u8>>>) -> (r: bool)
    requires set_holds(*s, keys),
    ensures r == keys.contains(x@),
{
    let k = make_key1(x);
    s.get(&k).is_some()
}

/// The distinct keys, first occurrences in order.
pub fn dedup_keys(keys: &Vec<Vec<u8>>) -> (out: Vec<Vec<u8>>)
    ensures seq_view_bytes(out@) == dedup(seq_view_bytes(keys@)),
{
    let mut out: Vec<Vec<u8>> = Vec::new();
    let mut seen: Index<()> = Index::new();
    let mut i: usize = 0;
    proof {
        assert(seq_view_bytes(keys@.take(0)) =~= Seq::<Seq<u8>>::empty());
        assert(dedup(seq_view_bytes(keys@.take(0))) =~= Seq::<Seq<u8>>::empty());
        assert forall|x: Seq<u8>| #[trigger] seen.at(key1(x)).is_some() == dedup(seq_view_bytes(keys@.take(0))).contains(x) by {}
    }
    while i < keys.len()
        invariant
            i <= keys.len(),
            seq_view_bytes(out@) == dedup(seq_view_bytes(keys@.take(i as int))),
            set_holds(seen, dedup(seq_view_bytes(keys@.take(i as int)))),
        decreases keys.len() - i,
    {
        let k = make_key1(keys[i].as_slice());
        let ghost vs = seq_view_bytes(keys@.take(i as int));
        let ghost d0 = dedup(vs);
        let ghost before = seen;
        proof {
            assert(keys@.take(i as int + 1) =~= keys@.take(i as int).push(keys@[i as int]));
            assert(seq_view_bytes(keys@.take(i as int + 1)) =~= vs.push(keys@[i as int]@));
            assert(vs.push(keys@[i as int]@).drop_last() =~= vs);
            assert(vs.push(keys@[i as int]@).last() == keys@[i as int]@);
        }
        if seen.get(&k).is_none() {
            let dup = crate::rows::copy_bytes(keys[i].as_slice());
            out.push(dup);
            seen.put(k.share(), Arc::new(()));
            proof {
                let x0 = keys@[i as int]@;
                assert(!d0.contains(x0)) by {
                    if d0.contains(x0) { assert(before.at(key1(x0)).is_some()); }
                }
                assert(dedup(vs.push(x0)) =~= d0.push(x0));
                assert(seq_view_bytes(out@) =~= d0.push(x0));
                assert forall|x: Seq<u8>| #[trigger] seen.at(key1(x)).is_some() == d0.push(x0).contains(x) by {
                    lemma_contains_push(d0, x0, x);
                    if key1(x) == k.view() {
                        lemma_key1_inj(x, x0);
                    } else {
                        assert(seen.at(key1(x)) == before.at(key1(x)));
                        if x == x0 { assert(false); }
                    }
                }
            }
        } else {
            proof {
                let x0 = keys@[i as int]@;
                assert(d0.contains(x0)) by {
                    let ki = k.view();
                    assert(before.at(ki).is_some());
                    assert(ki == key1(x0));
                }
                assert(dedup(vs.push(x0)) =~= d0);
            }
        }
        i += 1;
    }
    proof { assert(keys@.take(keys.len() as int) =~= keys@); }
    out
}

/// The adsh of every num, pre and sub change row (NULL adsh matches nothing).
pub open spec fn num_adshes(d: Seq<NumDelta>) -> Seq<Seq<u8>>
    decreases d.len(),
{
    if d.len() == 0 { Seq::empty() } else {
        let r = num_adshes(d.drop_last());
        if d.last().row.adsh__null != 0 { r } else { r.push(d.last().row.adsh@) }
    }
}

pub open spec fn pre_adshes(d: Seq<PreDelta>) -> Seq<Seq<u8>>
    decreases d.len(),
{
    if d.len() == 0 { Seq::empty() } else {
        let r = pre_adshes(d.drop_last());
        if d.last().row.adsh__null != 0 { r } else { r.push(d.last().row.adsh@) }
    }
}

pub open spec fn sub_adshes(d: Seq<SubDelta>) -> Seq<Seq<u8>>
    decreases d.len(),
{
    if d.len() == 0 { Seq::empty() } else { sub_adshes(d.drop_last()).push(d.last().row.adsh@) }
}

/// The tag of every tag change row.
pub open spec fn tag_names(d: Seq<TagDelta>) -> Seq<Seq<u8>>
    decreases d.len(),
{
    if d.len() == 0 { Seq::empty() } else { tag_names(d.drop_last()).push(d.last().row.tag@) }
}

pub open spec fn adsh_wanted(n: Seq<NumDelta>, p: Seq<PreDelta>, s: Seq<SubDelta>) -> Seq<Seq<u8>> {
    dedup(num_adshes(n) + pre_adshes(p) + sub_adshes(s))
}

pub open spec fn tag_wanted(t: Seq<TagDelta>) -> Seq<Seq<u8>> {
    dedup(tag_names(t))
}

proof fn lemma_num_adshes_has(d: Seq<NumDelta>, i: int)
    requires 0 <= i < d.len(), d[i].row.adsh__null == 0,
    ensures num_adshes(d).contains(d[i].row.adsh@),
    decreases d.len(),
{
    let r = num_adshes(d.drop_last());
    if i == d.len() - 1 {
        assert(num_adshes(d).last() == d.last().row.adsh@);
        assert(num_adshes(d)[num_adshes(d).len() - 1] == d[i].row.adsh@);
    } else {
        assert(d.drop_last()[i] == d[i]);
        lemma_num_adshes_has(d.drop_last(), i);
        let j = choose|j: int| 0 <= j < r.len() && r[j] == d[i].row.adsh@;
        if d.last().row.adsh__null != 0 { assert(num_adshes(d)[j] == r[j]); } else { assert(num_adshes(d).push(d.last().row.adsh@)[j] == r[j]); }
    }
}


proof fn lemma_pre_adshes_has(d: Seq<PreDelta>, i: int)
    requires 0 <= i < d.len(), d[i].row.adsh__null == 0,
    ensures pre_adshes(d).contains(d[i].row.adsh@),
    decreases d.len(),
{
    let r = pre_adshes(d.drop_last());
    if i == d.len() - 1 {
        assert(pre_adshes(d)[pre_adshes(d).len() - 1] == d[i].row.adsh@);
    } else {
        assert(d.drop_last()[i] == d[i]);
        lemma_pre_adshes_has(d.drop_last(), i);
        let j = choose|j: int| 0 <= j < r.len() && r[j] == d[i].row.adsh@;
        assert(pre_adshes(d)[j] == r[j]);
    }
}

proof fn lemma_sub_adshes_has(d: Seq<SubDelta>, i: int)
    requires 0 <= i < d.len(),
    ensures sub_adshes(d).contains(d[i].row.adsh@),
    decreases d.len(),
{
    let r = sub_adshes(d.drop_last());
    if i == d.len() - 1 {
        assert(sub_adshes(d)[sub_adshes(d).len() - 1] == d[i].row.adsh@);
    } else {
        assert(d.drop_last()[i] == d[i]);
        lemma_sub_adshes_has(d.drop_last(), i);
        let j = choose|j: int| 0 <= j < r.len() && r[j] == d[i].row.adsh@;
        assert(sub_adshes(d)[j] == r[j]);
    }
}

proof fn lemma_tag_names_has(d: Seq<TagDelta>, i: int)
    requires 0 <= i < d.len(),
    ensures tag_names(d).contains(d[i].row.tag@),
    decreases d.len(),
{
    let r = tag_names(d.drop_last());
    if i == d.len() - 1 {
        assert(tag_names(d)[tag_names(d).len() - 1] == d[i].row.tag@);
    } else {
        assert(d.drop_last()[i] == d[i]);
        lemma_tag_names_has(d.drop_last(), i);
        let j = choose|j: int| 0 <= j < r.len() && r[j] == d[i].row.tag@;
        assert(tag_names(d)[j] == r[j]);
    }
}

/// Every changed row's key is a wanted key.
pub proof fn lemma_wanted_covers(n: Seq<NumDelta>, p: Seq<PreDelta>, s: Seq<SubDelta>, t: Seq<TagDelta>)
    ensures
        forall|i: int| 0 <= i < n.len() && (#[trigger] n[i]).row.adsh__null == 0 ==> adsh_wanted(n, p, s).contains(n[i].row.adsh@),
        forall|i: int| 0 <= i < p.len() && (#[trigger] p[i]).row.adsh__null == 0 ==> adsh_wanted(n, p, s).contains(p[i].row.adsh@),
        forall|i: int| 0 <= i < s.len() ==> adsh_wanted(n, p, s).contains((#[trigger] s[i]).row.adsh@),
        forall|i: int| 0 <= i < t.len() ==> tag_wanted(t).contains((#[trigger] t[i]).row.tag@),
{
    lemma_dedup(num_adshes(n) + pre_adshes(p) + sub_adshes(s));
    lemma_dedup(tag_names(t));
    assert forall|i: int| 0 <= i < n.len() && (#[trigger] n[i]).row.adsh__null == 0 implies adsh_wanted(n, p, s).contains(n[i].row.adsh@) by {
        lemma_num_adshes_has(n, i);
        let j = choose|j: int| 0 <= j < num_adshes(n).len() && num_adshes(n)[j] == n[i].row.adsh@;
        assert((num_adshes(n) + pre_adshes(p) + sub_adshes(s))[j] == num_adshes(n)[j]);
    }
    assert forall|i: int| 0 <= i < p.len() && (#[trigger] p[i]).row.adsh__null == 0 implies adsh_wanted(n, p, s).contains(p[i].row.adsh@) by {
        lemma_pre_adshes_has(p, i);
        let j = choose|j: int| 0 <= j < pre_adshes(p).len() && pre_adshes(p)[j] == p[i].row.adsh@;
        assert((num_adshes(n) + pre_adshes(p) + sub_adshes(s))[num_adshes(n).len() + j] == pre_adshes(p)[j]);
    }
    assert forall|i: int| 0 <= i < s.len() implies adsh_wanted(n, p, s).contains((#[trigger] s[i]).row.adsh@) by {
        lemma_sub_adshes_has(s, i);
        let j = choose|j: int| 0 <= j < sub_adshes(s).len() && sub_adshes(s)[j] == s[i].row.adsh@;
        assert((num_adshes(n) + pre_adshes(p) + sub_adshes(s))[num_adshes(n).len() + pre_adshes(p).len() + j] == sub_adshes(s)[j]);
    }
    assert forall|i: int| 0 <= i < t.len() implies tag_wanted(t).contains((#[trigger] t[i]).row.tag@) by {
        lemma_tag_names_has(t, i);
    }
}

fn push_num_adshes(out: &mut Vec<Vec<u8>>, d: &Vec<NumDelta>)
    ensures seq_view_bytes(final(out)@) == seq_view_bytes(old(out)@) + num_adshes(d@),
{
    let ghost start = seq_view_bytes(out@);
    let mut i: usize = 0;
    proof { assert(num_adshes(d@.take(0)) =~= Seq::<Seq<u8>>::empty()); assert(start + Seq::<Seq<u8>>::empty() =~= start); }
    while i < d.len()
        invariant i <= d.len(), seq_view_bytes(out@) == start + num_adshes(d@.take(i as int)),
        decreases d.len() - i,
    {
        proof {
            assert(d@.take(i as int + 1).drop_last() =~= d@.take(i as int));
            assert(d@.take(i as int + 1).last() == d@[i as int]);
            assert(d@.take(i as int + 1).len() == i + 1);
            reveal_with_fuel(num_adshes, 2);
        }
        let ghost before = seq_view_bytes(out@);
        if d[i].row.adsh__null == 0 {
            out.push(crate::rows::copy_bytes(d[i].row.adsh.as_slice()));
            proof {
                assert(seq_view_bytes(out@) =~= before.push(d@[i as int].row.adsh@));
                assert(seq_view_bytes(out@) =~= start + num_adshes(d@.take(i as int + 1)));
            }
        } else {
            proof { assert(seq_view_bytes(out@) =~= start + num_adshes(d@.take(i as int + 1))); }
        }
        i += 1;
    }
    proof { assert(d@.take(d.len() as int) =~= d@); }
}

fn push_pre_adshes(out: &mut Vec<Vec<u8>>, d: &Vec<PreDelta>)
    ensures seq_view_bytes(final(out)@) == seq_view_bytes(old(out)@) + pre_adshes(d@),
{
    let ghost start = seq_view_bytes(out@);
    let mut i: usize = 0;
    proof { assert(pre_adshes(d@.take(0)) =~= Seq::<Seq<u8>>::empty()); assert(start + Seq::<Seq<u8>>::empty() =~= start); }
    while i < d.len()
        invariant i <= d.len(), seq_view_bytes(out@) == start + pre_adshes(d@.take(i as int)),
        decreases d.len() - i,
    {
        proof {
            assert(d@.take(i as int + 1).drop_last() =~= d@.take(i as int));
            assert(d@.take(i as int + 1).last() == d@[i as int]);
            assert(d@.take(i as int + 1).len() == i + 1);
            reveal_with_fuel(pre_adshes, 2);
        }
        let ghost before = seq_view_bytes(out@);
        if d[i].row.adsh__null == 0 {
            out.push(crate::rows::copy_bytes(d[i].row.adsh.as_slice()));
            proof {
                assert(seq_view_bytes(out@) =~= before.push(d@[i as int].row.adsh@));
                assert(seq_view_bytes(out@) =~= start + pre_adshes(d@.take(i as int + 1)));
            }
        } else {
            proof { assert(seq_view_bytes(out@) =~= start + pre_adshes(d@.take(i as int + 1))); }
        }
        i += 1;
    }
    proof { assert(d@.take(d.len() as int) =~= d@); }
}

fn push_sub_adshes(out: &mut Vec<Vec<u8>>, d: &Vec<SubDelta>)
    ensures seq_view_bytes(final(out)@) == seq_view_bytes(old(out)@) + sub_adshes(d@),
{
    let ghost start = seq_view_bytes(out@);
    let mut i: usize = 0;
    proof { assert(sub_adshes(d@.take(0)) =~= Seq::<Seq<u8>>::empty()); assert(start + Seq::<Seq<u8>>::empty() =~= start); }
    while i < d.len()
        invariant i <= d.len(), seq_view_bytes(out@) == start + sub_adshes(d@.take(i as int)),
        decreases d.len() - i,
    {
        proof {
            assert(d@.take(i as int + 1).drop_last() =~= d@.take(i as int));
            assert(d@.take(i as int + 1).last() == d@[i as int]);
            assert(d@.take(i as int + 1).len() == i + 1);
            reveal_with_fuel(sub_adshes, 2);
        }
        let ghost before = seq_view_bytes(out@);
        out.push(crate::rows::copy_bytes(d[i].row.adsh.as_slice()));
        proof {
            assert(seq_view_bytes(out@) =~= before.push(d@[i as int].row.adsh@));
            assert(seq_view_bytes(out@) =~= start + sub_adshes(d@.take(i as int + 1)));
        }
        i += 1;
    }
    proof { assert(d@.take(d.len() as int) =~= d@); }
}

/// The adsh keys a transaction asks to rescan.
pub fn adsh_keys(n: &Vec<NumDelta>, p: &Vec<PreDelta>, s: &Vec<SubDelta>) -> (r: Vec<Vec<u8>>)
    ensures seq_view_bytes(r@) == adsh_wanted(n@, p@, s@),
{
    let mut raw: Vec<Vec<u8>> = Vec::new();
    push_num_adshes(&mut raw, n);
    push_pre_adshes(&mut raw, p);
    push_sub_adshes(&mut raw, s);
    proof { assert(seq_view_bytes(raw@) =~= num_adshes(n@) + pre_adshes(p@) + sub_adshes(s@)); }
    dedup_keys(&raw)
}

/// The tag keys a transaction asks to rescan.
pub fn tag_keys(t: &Vec<TagDelta>) -> (r: Vec<Vec<u8>>)
    ensures seq_view_bytes(r@) == tag_wanted(t@),
{
    let mut raw: Vec<Vec<u8>> = Vec::new();
    let mut i: usize = 0;
    proof { assert(tag_names(t@.take(0)) =~= Seq::<Seq<u8>>::empty()); }
    while i < t.len()
        invariant i <= t.len(), seq_view_bytes(raw@) == tag_names(t@.take(i as int)),
        decreases t.len() - i,
    {
        proof { assert(t@.take(i as int + 1).drop_last() =~= t@.take(i as int)); assert(t@.take(i as int + 1).last() == t@[i as int]); }
        raw.push(crate::rows::copy_bytes(t[i].row.tag.as_slice()));
        proof { assert(seq_view_bytes(raw@) =~= tag_names(t@.take(i as int + 1))); }
        i += 1;
    }
    proof { assert(t@.take(t.len() as int) =~= t@); }
    dedup_keys(&raw)
}

}
