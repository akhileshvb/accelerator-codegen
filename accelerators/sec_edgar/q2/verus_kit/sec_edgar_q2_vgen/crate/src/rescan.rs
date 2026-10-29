// From the rows the host delivers (a scan, or the rescanned filings) to the filings' max rows and sub rows.
use vstd::prelude::*;
use std::sync::Arc;
use verus_common::sums::*;
use verus_prooflib::ordered::{Index, Key, KeyView};
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::model::*;
use crate::keys::*;
use crate::f64bits::*;
use crate::group::*;
use crate::filing::*;
use crate::agree::*;
use verus_common::rel::*;
use crate::classes::finite_num;

verus! {

/// The rows the host delivered, as a database of their own.
pub open spec fn delivered(num: Seq<NumInput>, sub: Seq<SubInput>) -> Db {
    Db { num: num_input_seq_view(num), sub: sub_input_seq_view(sub) }
}

pub proof fn lemma_rescan_agree_num(db2: Db, keys: Seq<Vec<u8>>, rows: Seq<NumInput>, a: Seq<u8>)
    requires
        rescan_num_by_filing_ok(db2, keys, rows),
        seq_view_bytes(keys).contains(a),
    ensures
        agree_num(num_input_seq_view(rows), db2.num, a),
{
    assert forall|f: spec_fn(NumRow) -> int| (forall|n: NumRow| n.adsh != Some(a) ==> #[trigger] f(n) == 0) implies #[trigger] sum(num_input_seq_view(rows), f) == sum(db2.num, f) by {
        let g = |num: NumRow| if num.adsh is Some && seq_view_bytes(keys).contains(num.adsh->0) { f(num) } else { 0 };
        assert(sum(num_input_seq_view(rows), f) == sum(db2.num, g));
        assert forall|n: NumRow| #[trigger] g(n) == f(n) by {
            if n.adsh == Some(a) {} else { assert(f(n) == 0); }
        }
        lemma_sum_ext_all(db2.num, g, f);
    }
}

pub proof fn lemma_rescan_agree_sub(db2: Db, keys: Seq<Vec<u8>>, rows: Seq<SubInput>, a: Seq<u8>)
    requires
        rescan_sub_by_filing_ok(db2, keys, rows),
        seq_view_bytes(keys).contains(a),
    ensures
        agree_sub(sub_input_seq_view(rows), db2.sub, a),
{
    assert forall|f: spec_fn(SubRow) -> int| (forall|s: SubRow| s.adsh != a ==> #[trigger] f(s) == 0) implies #[trigger] sum(sub_input_seq_view(rows), f) == sum(db2.sub, f) by {
        let g = |sub: SubRow| if seq_view_bytes(keys).contains(sub.adsh) { f(sub) } else { 0 };
        assert(sum(sub_input_seq_view(rows), f) == sum(db2.sub, g));
        assert forall|s: SubRow| #[trigger] g(s) == f(s) by {
            if s.adsh == a {} else { assert(f(s) == 0); }
        }
        lemma_sum_ext_all(db2.sub, g, f);
    }
}

/// Every delivered row belongs to one of the asked filings.
pub proof fn lemma_rescan_rows_keyed(db2: Db, keys: Seq<Vec<u8>>, rows: Seq<NumInput>, i: int)
    requires
        rescan_num_by_filing_ok(db2, keys, rows),
        0 <= i < rows.len(),
        num_input_view(rows[i]).adsh is Some,
    ensures
        seq_view_bytes(keys).contains(num_input_view(rows[i]).adsh->0),
{
    let f = |n: NumRow| if n.adsh is Some && !seq_view_bytes(keys).contains(n.adsh->0) { 1int } else { 0 };
    let g = |num: NumRow| if num.adsh is Some && seq_view_bytes(keys).contains(num.adsh->0) { f(num) } else { 0 };
    assert forall|k: int| 0 <= k < db2.num.len() implies g(#[trigger] db2.num[k]) == 0 by {}
    lemma_sum_zero(db2.num, g);
    let view = num_input_seq_view(rows);
    assert(sum(view, f) == 0);
    assert forall|k: int| 0 <= k < view.len() implies f(#[trigger] view[k]) >= 0 by {}
    assert(view[i] == num_input_view(rows[i]));
    if !seq_view_bytes(keys).contains(num_input_view(rows[i]).adsh->0) {
        lemma_sum_pos(view, f, i);
    }
}

pub proof fn lemma_scan_agree(db: Db, num: Seq<NumInput>, sub: Seq<SubInput>, a: Seq<u8>)
    requires
        scan_num_build_ok(db, num),
        scan_sub_build_ok(db, sub),
    ensures
        agree(delivered(num, sub), db, a),
{
}

/// The delivered rows' filings: their max rows and sub rows, each matching what the rows say.
pub fn compute(rows_n: &Vec<NumInput>, rows_s: &Vec<SubInput>) -> (r: Option<(Index<Vec<MR>>, Index<Vec<SR>>)>)
    ensures
        r matches Some((m, s)) ==> {
            &&& m.wf()
            &&& s.wf()
            &&& forall|a: Seq<u8>| {
                &&& mr_sem(mr_at(m, a), a, delivered(rows_n@, rows_s@))
                &&& sr_sem(sr_at(s, a), a, delivered(rows_n@, rows_s@))
                &&& forall|k: int| 0 <= k < mr_at(m, a).len() ==> f64_finite((#[trigger] mr_at(m, a)[k]).val)
            }
            &&& finite_num(delivered(rows_n@, rows_s@))
        },
        r is None ==> exists|i: int| 0 <= i < rows_n@.len() && qual_in(rows_n@[i]) && !f64_finite(rows_n@[i].value),
{
    let sorted = match sorted_qual(rows_n) {
        Some(s) => s,
        None => { return None; }
    };
    let m = tops_by_filing(rows_n, &sorted);
    let s = subs_by_filing(rows_s);
    proof {
        let d = delivered(rows_n@, rows_s@);
        assert forall|a: Seq<u8>| mr_sem(mr_at(m, a), a, d) by {
            assert forall|h: spec_fn(NumRow) -> int| #[trigger] sum(mr_views(a, mr_at(m, a)), h)
                == sum(d.num, |n: NumRow| if is_qual(n) && n.adsh == Some(a) && is_max(d.num, n) { h(n) } else { 0 }) by {
                assert(mr_w(m, a, h) == sum(mr_views(a, mr_at(m, a)), h));
                lemma_tops_sem(rows_n@, sorted@, a, h);
            }
        }
        assert forall|a: Seq<u8>| sr_sem(sr_at(s, a), a, d) by {
            assert forall|k: spec_fn(SubRow) -> int| #[trigger] sum(sr_views(a, sr_at(s, a)), k)
                == sum(d.sub, |x: SubRow| if x.adsh == a && x.fy is Some { k(x) } else { 0 }) by {
                assert(sr_w(s, a, k) == sub_prefix_w(rows_s@, a, k));
            }
        }
    }
    proof {
        let d = delivered(rows_n@, rows_s@);
        assert forall|n: NumRow| d.num.contains(n) && is_qual(n) implies exists|b: u64| f64_finite(b) && f64_val(b) == n.value->0 by {
            let i = choose|i: int| 0 <= i < d.num.len() && d.num[i] == n;
            assert(d.num[i] == num_input_view(rows_n@[i]));
            assert(qual_in(rows_n@[i]));
            assert(f64_finite(rows_n@[i].value));
            assert(f64_val(rows_n@[i].value) == n.value->0);
        }
    }
    Some((m, s))
}

} // verus!
