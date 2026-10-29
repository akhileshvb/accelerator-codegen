// Sorting the collected rows by the query's ORDER BY and writing the first hundred.
use vstd::prelude::*;
use std::sync::Arc;
use verus_common::bytes::*;
use verus_common::sums::*;
use verus_common::order::*;
use verus_common::sort::*;
use verus_common::rel::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::cells::*;
use crate::classes::*;
use crate::f64bits::*;
use crate::group::copy_vec;

verus! {

pub open spec fn rows_finite(rows: Seq<Row>) -> bool {
    forall|k: int| 0 <= k < rows.len() ==> f64_finite((#[trigger] rows[k]).val)
}

/// a sorts strictly before b under value descending, name ascending (NULL last), tag ascending.
pub fn row_lt_exec(a: &Row, b: &Row) -> (r: bool)
    requires
        f64_finite(a.val),
        f64_finite(b.val),
    ensures
        r == row_lt(a.view(), b.view()),
{
    proof { lemma_desc_key(a.val, b.val); lemma_desc_key(b.val, a.val); }
    let ka = desc_key(a.val);
    let kb = desc_key(b.val);
    if ka != kb {
        return ka < kb;
    }
    match (&a.name, &b.name) {
        (Some(x), Some(y)) => {
            let c = verus_prooflib::ordered::compare_bytes(x.as_slice(), y.as_slice());
            if c != 0 {
                return c < 0;
            }
        }
        (Some(_), None) => { return true; }
        (None, Some(_)) => { return false; }
        (None, None) => {}
    }
    let c = verus_prooflib::ordered::compare_bytes(a.tag.as_slice(), b.tag.as_slice());
    c < 0
}

pub open spec fn sorted_views(rows: Seq<Row>, s: Seq<usize>) -> Seq<OutRow> {
    s.map_values(|i: usize| rows[i as int].view())
}

pub proof fn lemma_ext_pre(x: OutRow, y: OutRow, z: OutRow)
    ensures
        ext_order()(x, y) || ext_order()(y, x) || x == y,
{
    if x != y { lemma_ext_total(x, y); }
}

/// The rows' positions, ordered by the query's ORDER BY.
pub fn sort_rows(rows: &Vec<Row>) -> (sorted: Vec<usize>)
    requires
        rows_finite(rows@),
    ensures
        forall|k: int| 0 <= k < sorted@.len() ==> (#[trigger] sorted@[k]) < rows@.len(),
        nondesc(sorted_views(rows@, sorted@), ext_order()),
        sorted_views(rows@, sorted@).to_multiset() =~= views(rows@).to_multiset(),
{
    let mut pos: Vec<usize> = Vec::new();
    let mut i: usize = 0;
    while i < rows.len()
        invariant
            i <= rows@.len(),
            pos@.len() == i,
            forall|k: int| 0 <= k < pos@.len() ==> (#[trigger] pos@[k]) as int == k,
        decreases rows.len() - i,
    {
        pos.push(i);
        i += 1;
    }
    let ghost rv = rows@;
    let ghost le = |x: usize, y: usize| !ext_order()(rv[y as int].view(), rv[x as int].view());
    let ghost valid = |x: usize| x < rv.len();
    let rows_ref = rows;
    let compare = |x: usize, y: usize| -> (r: bool)
        requires x < rows_ref.len(), y < rows_ref.len(), rows_finite(rows_ref@),
        ensures r == !ext_order()(rows_ref@[y as int].view(), rows_ref@[x as int].view()),
    {
        assert(rows_finite(rows_ref@));
        !row_lt_exec(&rows_ref[y], &rows_ref[x])
    };
    proof {
        lemma_ext_order();
        assert forall|x: usize, y: usize| valid(x) && valid(y) implies #[trigger] le(x, y) || le(y, x) by {
            let (a, b) = (rv[x as int].view(), rv[y as int].view());
            assert(ext_order()(a, b) ==> !ext_order()(b, a));
        }
        assert forall|x: usize, y: usize, z: usize| valid(x) && valid(y) && valid(z) && #[trigger] le(x, y) && #[trigger] le(y, z) implies le(x, z) by {
            let (a, b, c) = (rv[x as int].view(), rv[y as int].view(), rv[z as int].view());
            lemma_ext_pre(c, b, a);
            lemma_ext_pre(c, a, b);
            lemma_ext_pre(b, a, c);
        }
        assert(preorder_on(le, valid));
        assert(all_valid(pos@, valid));
        assert(cmp_ok(&compare, le, valid)) by {
            assert forall|x: usize, y: usize| valid(x) && valid(y) implies #[trigger] compare.requires((x, y)) by {}
            assert forall|x: usize, y: usize, r: bool| #[trigger] compare.ensures((x, y), r) implies r == le(x, y) by {}
        }
    }
    let sorted = sort_idx(&pos, &compare, Ghost(le), Ghost(valid));
    proof {
        assert forall|k: int| 0 <= k < sorted@.len() implies (#[trigger] sorted@[k]) < rows@.len() by { assert(valid(sorted@[k])); }
        let sv = sorted_views(rv, sorted@);
        assert forall|i: int, j: int| 0 <= i < j < sv.len() implies !ext_order()(#[trigger] sv[j], #[trigger] sv[i]) by {
            assert(le(sorted@[i], sorted@[j]));
        }
        assert forall|g: spec_fn(OutRow) -> int| #[trigger] sum(sv, g) == sum(views(rv), g) by {
            let gi = |x: usize| g(rv[x as int].view());
            assert(sum(sorted@, gi) == sum(pos@, gi));
            lemma_sum_map(sorted@, |x: usize| rv[x as int].view(), g, gi);
            assert(views(rv) =~= pos@.map_values(|x: usize| rv[x as int].view())) by {
                assert forall|k: int| 0 <= k < pos@.len() implies views(rv)[k] == pos@.map_values(|x: usize| rv[x as int].view())[k] by {}
            }
            lemma_sum_map(pos@, |x: usize| rv[x as int].view(), g, gi);
        }
        lemma_sums_eq_multiset(sv, views(rv));
    }
    sorted
}

pub type OutT = (Option<Vec<u8>>, Option<Vec<u8>>, Option<u64>);

pub fn out_of(r: &Row) -> (o: OutT)
    ensures
        out_view(seq![o]) =~= seq![r.view()],
{
    let name = match &r.name {
        Some(n) => Some(copy_vec(n.as_slice())),
        None => None,
    };
    (name, Some(copy_vec(r.tag.as_slice())), Some(r.val))
}

/// The first hundred of the sorted rows, as result tuples.
pub fn emit(rows: &Vec<Row>, sorted: &Vec<usize>) -> (out: Vec<OutT>)
    requires
        forall|k: int| 0 <= k < sorted@.len() ==> (#[trigger] sorted@[k]) < rows@.len(),
    ensures
        out_view(out@) == limit_rows(sorted_views(rows@, sorted@), 100),
{
    let n = if sorted.len() < 100 { sorted.len() } else { 100 };
    let mut out: Vec<OutT> = Vec::new();
    let mut i: usize = 0;
    let ghost sv = sorted_views(rows@, sorted@);
    proof { assert(sv.take(0) =~= Seq::<OutRow>::empty()); assert(out_view(out@) =~= Seq::<OutRow>::empty()); }
    while i < n
        invariant
            i <= n,
            n <= sorted@.len(),
            n <= 100,
            forall|k: int| 0 <= k < sorted@.len() ==> (#[trigger] sorted@[k]) < rows@.len(),
            sv == sorted_views(rows@, sorted@),
            out_view(out@) == sv.take(i as int),
        decreases n - i,
    {
        let o = out_of(&rows[sorted[i]]);
        let ghost before = out@;
        out.push(o);
        proof {
            assert(out_view(seq![o]) =~= seq![rows@[sorted@[i as int] as int].view()]);
            assert(out@ == before.push(o));
            assert(out_view(seq![o])[0] == sv[i as int]);
            assert(out_view(seq![o])[0] == out_view(out@)[before.len() as int]) by {
                assert(out@[before.len() as int] == o);
                assert(seq![o][0] == o);
            }
            assert(out_view(out@) =~= out_view(before).push(sv[i as int]));
            assert(sv.take(i as int + 1) =~= sv.take(i as int).push(sv[i as int]));
        }
        i += 1;
    }
    proof {
        if sorted@.len() <= 100 {
            assert(n == sorted@.len());
            assert(sv.take(n as int) =~= sv);
        } else {
            assert(n == 100);
        }
    }
    out
}

} // verus!
