// SEC-EDGAR Q2: the answer rows of every filing, indexed by (fy, uom) and value, with each filing's cells listed.
#![allow(unused_imports)]
use vstd::prelude::*;
use std::sync::Arc;
use verus_common::rat::*;
use verus_common::sums::*;
use verus_common::rel::*;
use verus_prooflib::ordered::{Index, Key, KeyView};
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::model::*;
use crate::f64bits::*;
use crate::cells::*;
use crate::maint::*;
use crate::group::*;
use crate::filing::*;
use crate::agree::*;
use crate::rescan::*;
use crate::delta::*;
use crate::classes::*;
use crate::finish::*;
use crate::walk::*;
use crate::sortout::*;

verus! {

pub struct State {
    db: Ghost<Db>,
    cells: Cells,
    dir: Index<Vec<CK>>,
}

impl State {
    pub closed spec fn db(self) -> Db {
        self.db@
    }

    #[verifier::type_invariant]
    closed spec fn valid(self) -> bool {
        &&& cells_rep(self.cells, cw(self.db@))
        &&& cover(self.dir, cw(self.db@))
        &&& finite_num(self.db@)
    }
}

pub closed spec fn history(s: State) -> nat {
    0
}

/// The filings whose rows the host rescans: those of every changed row.
pub open spec fn rescan_num_by_filing_wanted(s: State, num_delta: Seq<NumDelta>, sub_delta: Seq<SubDelta>) -> Seq<Seq<u8>> {
    delta_keys(num_delta, sub_delta)
}

pub open spec fn rescan_sub_by_filing_wanted(s: State, num_delta: Seq<NumDelta>, sub_delta: Seq<SubDelta>) -> Seq<Seq<u8>> {
    delta_keys(num_delta, sub_delta)
}

pub fn rescan_num_by_filing_keys(
    s: &State,
    num_delta: &Vec<NumDelta>,
    sub_delta: &Vec<SubDelta>,
) -> (r: Vec<Vec<u8>>)
    ensures
        seq_view_bytes(r@) == crate::kernel::rescan_num_by_filing_wanted(*s, num_delta@, sub_delta@),
{
    delta_key_vec(num_delta, sub_delta)
}

pub fn rescan_sub_by_filing_keys(
    s: &State,
    num_delta: &Vec<NumDelta>,
    sub_delta: &Vec<SubDelta>,
) -> (r: Vec<Vec<u8>>)
    ensures
        seq_view_bytes(r@) == crate::kernel::rescan_sub_by_filing_wanted(*s, num_delta@, sub_delta@),
{
    delta_key_vec(num_delta, sub_delta)
}

pub open spec fn sum_eq(rows: Seq<NumInput>, num: Seq<NumRow>, f: spec_fn(NumRow) -> int, g: spec_fn(NumRow) -> int) -> bool {
    sum(num_input_seq_view(rows), f) == sum(num, g)
}

/// A non-finite value in a delivered row leaves the database outside the envelope.
proof fn lemma_nonfinite_row(db: Db, rows: Seq<NumInput>, i: int, pred: spec_fn(NumRow) -> bool)
    requires
        0 <= i < rows.len(),
        qual_in(rows[i]),
        !f64_finite(rows[i].value),
        forall|f: spec_fn(NumRow) -> int, g: spec_fn(NumRow) -> int|
            (forall|n: NumRow| #[trigger] g(n) == if pred(n) { f(n) } else { 0 }) ==> #[trigger] sum_eq(rows, db.num, f, g),
        pred(num_input_view(rows[i])),
    ensures
        !envelope(db),
{
    lemma_f64_in_range(rows[i].value);
    let bad = |n: NumRow| if n.value is Some && !f64_in_range(n.value->0) { 1int } else { 0 };
    let view = num_input_seq_view(rows);
    assert forall|k: int| 0 <= k < view.len() implies bad(#[trigger] view[k]) >= 0 by {}
    assert(view[i] == num_input_view(rows[i]));
    lemma_sum_pos(view, bad, i);
    let g = |n: NumRow| if pred(n) { bad(n) } else { 0 };
    assert forall|n: NumRow| #[trigger] g(n) == if pred(n) { bad(n) } else { 0 } by {}
    assert(sum_eq(rows, db.num, bad, g));
    if envelope(db) {
        assert forall|k: int| 0 <= k < db.num.len() implies g(#[trigger] db.num[k]) == 0 by {
            assert(num_in_type(db.num[k]));
        }
        lemma_sum_zero(db.num, g);
    }
}

/// Rows of a database that agrees with a finite one on their filing are finite too.
proof fn lemma_finite_via(dx: Db, dy: Db, n: NumRow)
    requires
        finite_num(dx),
        is_qual(n),
        dy.num.contains(n),
        agree_num(dx.num, dy.num, n.adsh->0),
    ensures
        exists|b: u64| f64_finite(b) && f64_val(b) == n.value->0,
{
    lemma_agree_contains(dx.num, dy.num, n.adsh->0, n);
}

/// A sub row of the database has its adsh among the scanned rows'.
proof fn lemma_sub_keys(db: Db, rows: Seq<SubInput>, kv: Seq<Seq<u8>>, a: Seq<u8>)
    requires
        scan_sub_build_ok(db, rows),
        kv == sub_input_seq_view(rows).map_values(|s: SubRow| s.adsh),
        !kv.contains(a),
    ensures
        forall|k: int| 0 <= k < db.sub.len() ==> (#[trigger] db.sub[k]).adsh != a,
{
    let f = |s: SubRow| if s.adsh == a { 1int } else { 0 };
    let view = sub_input_seq_view(rows);
    assert forall|k: int| 0 <= k < view.len() implies f(#[trigger] view[k]) == 0 by {
        assert(kv[k] == view[k].adsh);
    }
    lemma_sum_zero(view, f);
    assert(sum(db.sub, f) == 0);
    assert forall|k: int| 0 <= k < db.sub.len() implies f(#[trigger] db.sub[k]) >= 0 by {}
    assert forall|k: int| 0 <= k < db.sub.len() implies (#[trigger] db.sub[k]).adsh != a by {
        if db.sub[k].adsh == a { lemma_sum_pos(db.sub, f, k); }
    }
}

pub fn build(
    sub_build: Vec<SubInput>,
    num_build: Vec<NumInput>,
    Ghost(db): Ghost<Db>,
) -> (r: Option<State>)
    requires
        scan_sub_build_ok(db, sub_build@),
        scan_num_build_ok(db, num_build@),
    ensures
        r matches Some(s) ==> s.db() == db,
        r matches Some(s) ==> crate::kernel::history(s) == 0,
        envelope(db) ==> r is Some,
{
    let (mri, sri) = match compute(&num_build, &sub_build) {
        Some(x) => x,
        None => {
            proof {
                let i = choose|i: int| 0 <= i < num_build@.len() && qual_in(num_build@[i]) && !f64_finite(num_build@[i].value);
                assert forall|f: spec_fn(NumRow) -> int, g: spec_fn(NumRow) -> int|
                    (forall|n: NumRow| #[trigger] g(n) == if true { f(n) } else { 0 }) implies #[trigger] sum_eq(num_build@, db.num, f, g) by {
                    assert forall|n: NumRow| #[trigger] g(n) == f(n) by {}
                    lemma_sum_ext_all(db.num, g, f);
                }
                lemma_nonfinite_row(db, num_build@, i, |n: NumRow| true);
            }
            return None;
        }
    };
    let mut keys: Vec<Vec<u8>> = Vec::new();
    let mut i: usize = 0;
    while i < sub_build.len()
        invariant
            i <= sub_build@.len(),
            seq_view_bytes(keys@) == sub_input_seq_view(sub_build@.take(i as int)).map_values(|s: SubRow| s.adsh),
        decreases sub_build.len() - i,
    {
        keys.push(copy_vec(sub_build[i].adsh.as_slice()));
        proof {
            assert(sub_build@.take(i as int + 1) =~= sub_build@.take(i as int).push(sub_build@[i as int]));
            assert(sub_input_seq_view(sub_build@.take(i as int + 1)).map_values(|s: SubRow| s.adsh)
                =~= sub_input_seq_view(sub_build@.take(i as int)).map_values(|s: SubRow| s.adsh).push(sub_build@[i as int].adsh@));
            assert(seq_view_bytes(keys@) =~= seq_view_bytes(keys@.drop_last()).push(sub_build@[i as int].adsh@));
        }
        i += 1;
    }
    proof { assert(sub_build@.take(sub_build@.len() as int) =~= sub_build@); }
    let ghost kv = seq_view_bytes(keys@);
    let ghost zero: Fam = |id: CellId, g: spec_fn(OutRow) -> int| 0int;
    let ghost dr = delivered(num_build@, sub_build@);
    let mut cells: Cells = Index::new();
    let mut dir: Index<Vec<CK>> = Index::new();
    proof {
        assert forall|id: CellId| cell_ok(#[trigger] cell_at(cells, id), zero, id) by {}
        assert(cells_rep(cells, zero));
        assert forall|id: CellId| #[trigger] zero_fam(zero, id) by {}
        assert(cover(dir, zero));
    }
    refresh(&mut cells, &mut dir, &keys, &mri, &sri, Ghost(zero), Ghost(dr));
    proof {
        assert forall|id: CellId, g: spec_fn(OutRow) -> int| #[trigger] fam_w(cw(db), id, g) == fam_w(fam_keys(zero, kv, dr), id, g) by {
            lemma_scan_agree(db, num_build@, sub_build@, id.a);
            if kv.contains(id.a) {
                lemma_cell_w_agree(dr, db, id.a, id, g);
            } else {
                lemma_sub_keys(db, sub_build@, kv, id.a);
                lemma_no_sub_no_cells(db, id, g);
            }
        }
        lemma_rep_congr(cells, fam_keys(zero, kv, dr), cw(db));
        lemma_cover_congr(dir, fam_keys(zero, kv, dr), cw(db));
        assert forall|n: NumRow| db.num.contains(n) && is_qual(n) implies exists|b: u64| f64_finite(b) && f64_val(b) == n.value->0 by {
            lemma_scan_agree(db, num_build@, sub_build@, n.adsh->0);
            lemma_finite_via(dr, db, n);
        }
    }
    Some(State { db: Ghost(db), cells, dir })
}

pub fn apply(
    s: &State,
    num_delta: &Vec<NumDelta>,
    sub_delta: &Vec<SubDelta>,
    num_by_filing_keys: &Vec<Vec<u8>>,
    num_by_filing_rows: &Vec<NumInput>,
    sub_by_filing_keys: &Vec<Vec<u8>>,
    sub_by_filing_rows: &Vec<SubInput>,
    Ghost(db2): Ghost<Db>,
) -> (r: Result<State, u32>)
    requires
        capture_ok(s.db(), db2, num_delta_seq_view(num_delta@), sub_delta_seq_view(sub_delta@)),
        seq_view_bytes(num_by_filing_keys@) == crate::kernel::rescan_num_by_filing_wanted(*s, num_delta@, sub_delta@),
        rescan_num_by_filing_ok(db2, num_by_filing_keys@, num_by_filing_rows@),
        seq_view_bytes(sub_by_filing_keys@) == crate::kernel::rescan_sub_by_filing_wanted(*s, num_delta@, sub_delta@),
        rescan_sub_by_filing_ok(db2, sub_by_filing_keys@, sub_by_filing_rows@),
    ensures
        r matches Ok(s2) ==> s2.db() == db2,
        r matches Ok(s2) ==> crate::kernel::history(s2) <= crate::kernel::history(*s) + num_delta@.len() + sub_delta@.len(),
        envelope(s.db()) && envelope(db2) && crate::kernel::history(*s) + num_delta@.len() + sub_delta@.len() <= max_history() ==> r is Ok,
{
    proof { use_type_invariant(s); }
    let (mri, sri) = match compute(num_by_filing_rows, sub_by_filing_rows) {
        Some(x) => x,
        None => {
            proof {
                let i = choose|i: int| 0 <= i < num_by_filing_rows@.len() && qual_in(num_by_filing_rows@[i]) && !f64_finite(num_by_filing_rows@[i].value);
                let kv = seq_view_bytes(num_by_filing_keys@);
                assert forall|f: spec_fn(NumRow) -> int, g: spec_fn(NumRow) -> int|
                    (forall|n: NumRow| #[trigger] g(n) == if n.adsh is Some && kv.contains(n.adsh->0) { f(n) } else { 0 })
                    implies #[trigger] sum_eq(num_by_filing_rows@, db2.num, f, g) by {
                    let g1 = |num: NumRow| if num.adsh is Some && seq_view_bytes(num_by_filing_keys@).contains(num.adsh->0) { f(num) } else { 0 };
                    assert forall|n: NumRow| #[trigger] g1(n) == g(n) by {}
                    lemma_sum_ext_all(db2.num, g1, g);
                }
                lemma_rescan_rows_keyed(db2, num_by_filing_keys@, num_by_filing_rows@, i);
                lemma_nonfinite_row(db2, num_by_filing_rows@, i,
                    |n: NumRow| n.adsh is Some && kv.contains(n.adsh->0));
            }
            return Err(1);
        }
    };
    let ghost db = s.db@;
    let ghost kv = seq_view_bytes(num_by_filing_keys@);
    let ghost dr = delivered(num_by_filing_rows@, sub_by_filing_rows@);
    let mut cells = s.cells.share();
    let mut dir = s.dir.share();
    proof {
        assert forall|id: CellId| cell_ok(#[trigger] cell_at(cells, id), cw(db), id) by {
            assert(cell_at(cells, id) == cell_at(s.cells, id));
        }
        assert forall|k: KeyView| #[trigger] cells.at(k) is Some implies cells.at(k)->0.wf() by { assert(cells.at(k) == s.cells.at(k)); }
        assert(cells_rep(cells, cw(db)));
        assert forall|id: CellId| !(#[trigger] zero_fam(cw(db), id)) implies listed(dir_of(dir, id.a), id) by {
            assert(dir_of(dir, id.a) == dir_of(s.dir, id.a));
        }
        assert(cover(dir, cw(db)));
    }
    refresh(&mut cells, &mut dir, num_by_filing_keys, &mri, &sri, Ghost(cw(db)), Ghost(dr));
    proof {
        let nd = num_delta@;
        let sd = sub_delta@;
        assert forall|id: CellId, g: spec_fn(OutRow) -> int| #[trigger] fam_w(cw(db2), id, g) == fam_w(fam_keys(cw(db), kv, dr), id, g) by {
            if kv.contains(id.a) {
                lemma_rescan_agree_num(db2, num_by_filing_keys@, num_by_filing_rows@, id.a);
                assert(seq_view_bytes(sub_by_filing_keys@).contains(id.a));
                lemma_rescan_agree_sub(db2, sub_by_filing_keys@, sub_by_filing_rows@, id.a);
                assert(agree(dr, db2, id.a));
                lemma_cell_w_agree(dr, db2, id.a, id, g);
            } else {
                lemma_delta_agree(db, db2, nd, sd, id.a);
                lemma_cell_w_agree(db, db2, id.a, id, g);
            }
        }
        lemma_rep_congr(cells, fam_keys(cw(db), kv, dr), cw(db2));
        lemma_cover_congr(dir, fam_keys(cw(db), kv, dr), cw(db2));
        assert forall|n: NumRow| db2.num.contains(n) && is_qual(n) implies exists|b: u64| f64_finite(b) && f64_val(b) == n.value->0 by {
            let a = n.adsh->0;
            if kv.contains(a) {
                lemma_rescan_agree_num(db2, num_by_filing_keys@, num_by_filing_rows@, a);
                lemma_finite_via(dr, db2, n);
            } else {
                lemma_delta_agree(db, db2, nd, sd, a);
                lemma_finite_via(db, db2, n);
            }
        }
    }
    Ok(State { db: Ghost(db2), cells, dir })
}

pub fn execute(
    s: &State,
    p1: i64,
    p2: &[u8],
) -> (r: Option<Vec<(Option<Vec<u8>>, Option<Vec<u8>>, Option<u64>)>>)
    ensures
        r matches Some(out) ==> query_result(s.db(), params(p1 as int, p2@), out_view(out@)),
        envelope(s.db()) && answer_fits(s.db(), params(p1 as int, p2@)) ==> r is Some,
        r matches Some(out) ==> forall|i: int| 0 <= i < out.len() ==> out_fits(#[trigger] out_view(out@)[i]),
{
    proof { use_type_invariant(s); }
    let ghost db = s.db@;
    let uom = Arc::new(copy_vec(p2));
    let ghost es = class_entries(s.cells, p1 as int, p2@);
    let coll: Vec<Row> = match s.cells.get(&kkey(p1, &uom)) {
        None => {
            proof {
                assert(es =~= Seq::<(KeyView, Vec<Row>)>::empty());
                assert(es.take(0) =~= es);
                assert(views(Seq::<Row>::empty()) =~= Seq::<OutRow>::empty());
                assert(flat_rows(es.take(0)) =~= Seq::<OutRow>::empty());
                assert(coll_ok(Seq::<Row>::empty(), es));
            }
            Vec::new()
        }
        Some(inner) => {
            proof {
                assert(s.cells.at((p1 as int, p2@)) == Some(*inner));
                assert(inner.wf());
                assert(inner.entries() == es);
                assert forall|i: int| 0 <= i < inner.entries().len() implies rows_finite((#[trigger] inner.entries()[i]).1@) by {
                    lemma_entry_rows(db, s.cells, p1 as int, p2@, i);
                }
            }
            let c = walk(&*inner);
            proof { assert(coll_ok(c@, es)); }
            c
        }
    };
    let sorted = sort_rows(&coll);
    let out = emit(&coll, &sorted);
    proof {
        lemma_collected(db, s.cells, p1 as int, p2@, coll@, sorted@);
        let sv = sorted_views(coll@, sorted@);
        assert forall|i: int| 0 <= i < out.len() implies out_fits(#[trigger] out_view(out@)[i]) by {
            let lim = limit_rows(sv, 100);
            assert(out_view(out@) == lim);
            assert(out_view(out@).len() == out.len());
            if sv.len() <= 100 { assert(lim == sv); } else { assert(lim == sv.take(100)); assert(lim[i] == sv[i]); }
            assert(i < sv.len());
            assert(sv.len() == sorted@.len());
            let row = coll@[sorted@[i] as int];
            assert(sv[i] == row.view());
            lemma_f64_in_range(row.val);
        }
    }
    Some(out)
}

} // verus!
