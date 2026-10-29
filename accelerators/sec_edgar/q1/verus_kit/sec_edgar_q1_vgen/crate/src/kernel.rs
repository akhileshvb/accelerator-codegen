// SEC-EDGAR Q1: per-(stmt, rfile) aggregates and a persistent map of row counts per (group, filing).
#![allow(unused_imports)]
use vstd::prelude::*;
use std::sync::Arc;
use verus_common::rat::*;
use verus_common::sums::*;
use verus_prooflib::ordered::{Index, Key, KeyView};
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::model::*;
use crate::steps::*;
use crate::absorb::*;
use crate::bridge::*;
use crate::copy::*;
use crate::answer::lemma_sum_ones;

verus! {

pub struct State {
    db: Ghost<Db>,
    aggs: Vec<Agg>,
    cells: Index<i128>,
    built: Ghost<nat>,
    hist: u64,
}

impl State {
    pub closed spec fn db(self) -> Db {
        self.db@
    }

    #[verifier::type_invariant]
    closed spec fn valid(self) -> bool {
        &&& parts_ok(self.aggs@, self.cells, self.db@.pre, Seq::empty())
        &&& self.built@ <= 9223372036854775808
        &&& self.hist <= 4611686018427387904
        &&& self.aggs@.len() <= self.built@ + self.hist
        &&& forall|i: int| 0 <= i < self.aggs@.len() ==> (#[trigger] self.aggs@[i]).filings as int <= self.built@ + self.hist
    }
}

pub closed spec fn history(s: State) -> nat {
    s.hist as nat
}

pub fn build(
    pre_build: Vec<PreInput>,
    Ghost(db): Ghost<Db>,
) -> (r: Option<State>)
    requires
        scan_pre_build_ok(db, pre_build@),
    ensures
        r matches Some(s) ==> s.db() == db,
        r matches Some(s) ==> crate::kernel::history(s) == 0,
        envelope(db) ==> r is Some,
{
    proof {
        lemma_sum_ones(db.pre);
        lemma_sum_ones(pre_input_seq_view(pre_build@));
        let one = |r: PreRow| 1int;
        assert(sum(pre_input_seq_view(pre_build@), one) == sum(db.pre, one));
    }
    if pre_build.len() > 9223372036854775808usize {
        return None;
    }
    let ghost rows = pre_build@;
    let ghost items = scan_items(rows);
    proof { lemma_scan_items_unit(rows); }
    let mut aggs: Vec<Agg> = Vec::new();
    let mut cells: Index<i128> = Index::new();
    proof {
        if envelope(db) { lemma_scan_mass(rows, db); }
        assert(items.take(0) =~= Seq::<(PreRow, int)>::empty());
    }
    let mut i: usize = 0;
    while i < pre_build.len()
        invariant
            i <= pre_build.len(),
            pre_build@ == rows,
            items == scan_items(rows),
            unit_items(items),
            parts_ok(aggs@, cells, Seq::empty(), items.take(i as int)),
            aggs@.len() <= i,
            forall|j: int| 0 <= j < aggs@.len() ==> (#[trigger] aggs@[j]).filings as int <= i,
            envelope(db) ==> total_mass(Seq::empty(), items) < big(),
            pre_build.len() <= 9223372036854775808usize,
        decreases pre_build.len() - i,
    {
        proof {
            assert(unit_items(items.take(i as int))) by {
                assert forall|k: int| 0 <= k < i implies (#[trigger] items.take(i as int)[k]).1 == 1 || items.take(i as int)[k].1 == -1 by {}
            }
        }
        let ok = absorb(&mut aggs, &mut cells, &pre_build[i], 1, Ghost(Seq::empty()), Ghost(items.take(i as int)), Ghost(i as nat));
        proof {
            let r = pre_input_view(pre_build@[i as int]);
            assert(items.take(i as int + 1) =~= items.take(i as int).push((r, 1int)));
            if envelope(db) { lemma_mass_prefix(items, i as int + 1); }
            assert(items[i as int] == (r, 1int));
        }
        if !ok {
            return None;
        }
        i += 1;
    }
    proof {
        assert(items.take(items.len() as int) =~= items);
        lemma_parts_transfer_scan(aggs@, cells, rows, items, db);
    }
    Some(State { db: Ghost(db), aggs, cells, built: Ghost(rows.len()), hist: 0 })
}

pub fn apply(
    s: &State,
    pre_delta: &Vec<PreDelta>,
    Ghost(db2): Ghost<Db>,
) -> (r: Result<State, u32>)
    requires
        capture_ok(s.db(), db2, pre_delta_seq_view(pre_delta@)),
    ensures
        r matches Ok(s2) ==> s2.db() == db2,
        r matches Ok(s2) ==> crate::kernel::history(s2) <= crate::kernel::history(*s) + pre_delta@.len(),
        envelope(s.db()) && envelope(db2) && crate::kernel::history(*s) + pre_delta@.len() <= max_history() ==> r is Ok,
{
    proof { use_type_invariant(s); }
    let n = pre_delta.len() as u64;
    if n > 4611686018427387904u64 - s.hist {
        return Err(1);
    }
    let ghost db = s.db@;
    let ghost items = pre_delta_seq_view(pre_delta@);
    let ghost t0: nat = s.built@ + s.hist as nat;
    let mut aggs = copy_aggs(&s.aggs);
    let mut cells = s.cells.share();
    proof {
        assert(items.take(0) =~= Seq::<(PreRow, int)>::empty());
        lemma_copy_parts(s.aggs@, aggs@, s.cells, db.pre, Seq::empty());
        lemma_index_congr(aggs@, s.cells, cells, db.pre, Seq::empty());
        assert(unit_items(items)) by {
            assert forall|k: int| 0 <= k < items.len() implies (#[trigger] items[k]).1 == 1 || items[k].1 == -1 by {}
        }
        if envelope(db) && envelope(db2) && s.hist as nat + pre_delta@.len() <= max_history() {
            assert forall|k: int| 0 <= k < items.len() implies mass((#[trigger] items[k]).0) <= 9223372036854775809 by {}
            lemma_apply_mass(db, items, pre_delta@.len() as int);
        }
    }
    let mut i: usize = 0;
    while i < pre_delta.len()
        invariant
            i <= pre_delta.len(),
            items == pre_delta_seq_view(pre_delta@),
            unit_items(items),
            db == s.db@,
            t0 == s.built@ + s.hist as nat,
            s.built@ <= 9223372036854775808,
            s.hist <= 4611686018427387904,
            n as nat == pre_delta@.len(),
            n <= 4611686018427387904u64 - s.hist,
            cells.wf(),
            parts_ok(aggs@, cells, db.pre, items.take(i as int)),
            aggs@.len() <= t0 + i,
            forall|j: int| 0 <= j < aggs@.len() ==> (#[trigger] aggs@[j]).filings as int <= t0 + i,
            envelope(db) && envelope(db2) && s.hist as nat + pre_delta@.len() <= max_history() ==> total_mass(db.pre, items) < big(),
        decreases pre_delta.len() - i,
    {
        let d = &pre_delta[i];
        proof {
            assert(unit_items(items.take(i as int))) by {
                assert forall|k: int| 0 <= k < i implies (#[trigger] items.take(i as int)[k]).1 == 1 || items.take(i as int)[k].1 == -1 by {}
            }
            assert(items[i as int].1 == d.sign as int);
            assert(d.sign == 1 || d.sign == -1);
        }
        let ok = absorb(&mut aggs, &mut cells, &d.row, d.sign, Ghost(db.pre), Ghost(items.take(i as int)), Ghost((t0 + i) as nat));
        proof {
            assert(items.take(i as int + 1) =~= items.take(i as int).push((pre_input_view(d.row), d.sign as int)));
            lemma_mass_prefix(items, i as int + 1);
        }
        if !ok {
            return Err(2);
        }
        i += 1;
    }
    proof {
        assert(items.take(items.len() as int) =~= items);
        assert forall|f: spec_fn(PreRow) -> int| #[trigger] vsum(db.pre, items, f) == sum(db2.pre, f) by {
            assert(bag_step(db.pre, db2.pre, items));
        }
        lemma_parts_transfer(aggs@, cells, db.pre, items, db2.pre);
    }
    Ok(State { db: Ghost(db2), aggs, cells, built: s.built, hist: s.hist + n })
}

#[verifier::external_body]
pub fn execute(
    s: &State,
    p1: &[u8],
) -> (r: Option<Vec<(Option<Vec<u8>>, Option<Vec<u8>>, i64, i64, Option<(i128, i128)>)>>)
    ensures
        r matches Some(out) ==> query_result(s.db(), params(p1@), out_view(out@)),
        envelope(s.db()) && answer_fits(s.db(), params(p1@)) ==> r is Some,
{
    unimplemented!()
}

} // verus!
