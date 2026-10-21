#![allow(unused_imports)]
use vstd::prelude::*;
use verus_common::sums::*;
use verus_common::rel::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::facts::*;
use verus_prooflib::ordered::Index;
use crate::store::*;
use crate::batch::*;
use crate::core::*;
use crate::changes::*;
use crate::merge::*;
use crate::read::*;
use crate::bag_proofs::*;

verus! {

pub open spec fn customer_count(db: Db, key: int) -> int {
    sum(db.customer, |row: CustomerRow| ind(row.c_custkey == key))
}

/// Orders of customer `key` whose comment does NOT match the LIKE pattern `p`.
pub open spec fn order_count(db: Db, p: Seq<u8>, key: int) -> int {
    sum(db.orders, |row: OrdersRow| ind(c_orders_on_orders(
        db, params(p), CustomerRow { c_custkey: key }, row)))
}

pub open spec fn histogram_count(db: Db, p: Seq<u8>, bucket: int) -> int {
    sum(customer_keys(db), |key: int| contribution(db, p, key, bucket))
}



/// The i-th first word (special, pending, unusual, express).
fn first_word(i: usize) -> Vec<u8> {
    if i == 0 { vec![b's', b'p', b'e', b'c', b'i', b'a', b'l'] }
    else if i == 1 { vec![b'p', b'e', b'n', b'd', b'i', b'n', b'g'] }
    else if i == 2 { vec![b'u', b'n', b'u', b's', b'u', b'a', b'l'] }
    else { vec![b'e', b'x', b'p', b'r', b'e', b's', b's'] }
}

/// The i-th second word (packages, requests, accounts, deposits).
fn second_word(i: usize) -> Vec<u8> {
    if i == 0 { vec![b'p', b'a', b'c', b'k', b'a', b'g', b'e', b's'] }
    else if i == 1 { vec![b'r', b'e', b'q', b'u', b'e', b's', b't', b's'] }
    else if i == 2 { vec![b'a', b'c', b'c', b'o', b'u', b'n', b't', b's'] }
    else { vec![b'd', b'e', b'p', b'o', b's', b'i', b't', b's'] }
}

/// The j-th tracked pattern: '%' || first_word(j / 4) || '%' || second_word(j % 4) || '%'.
/// Which patterns are tracked affects only which reads decline, not correctness.
fn tracked_pattern(j: usize) -> Vec<u8>
    requires j < 16,
{
    let mut v: Vec<u8> = Vec::new();
    v.push(b'%');
    let mut w = first_word(j / 4);
    v.append(&mut w);
    v.push(b'%');
    let mut w2 = second_word(j % 4);
    v.append(&mut w2);
    v.push(b'%');
    v
}
fn tracked_patterns() -> (r: Vec<Vec<u8>>)
    ensures r@.len() == 16,
{
    let mut v: Vec<Vec<u8>> = Vec::new();
    let mut j = 0usize;
    while j < 16
        invariant j <= 16, v@.len() == j as int,
        decreases 16 - j,
    {
        v.push(tracked_pattern(j));
        j += 1;
    }
    v
}


pub struct State { table: Index<Rec>, hists: Vec<Vec<HEntry>>, patterns: Vec<Vec<u8>>, db: Ghost<Db> }

impl State {
    pub closed spec fn db(self) -> Db { self.db@ }

    #[verifier::type_invariant]
    spec fn wf(self) -> bool {
        table_ok(self.table, self.db@) && hists_ok(self.hists@, self.patterns@, self.db@)
    }
}

/// The state keeps no history: applying a change updates the table in place.
pub closed spec fn history(s: State) -> nat { 0 }

pub fn copy_hist(source: &Vec<HEntry>) -> (out: Vec<HEntry>)
    requires hist_ok(source@),
    ensures hist_ok(out@), forall|bucket: int| #[trigger] hist_val(out@, bucket) == hist_val(source@, bucket),
{
    let mut out: Vec<HEntry> = Vec::new();
    let mut index = 0usize;
    while index < source.len()
        invariant index <= source.len(), out@.len() == index, hist_ok(source@),
            forall|k: int| 0 <= k < index ==> out@[k].bucket@ == source@[k].bucket@ && out@[k].freq@ == source@[k].freq@,
        decreases source.len() - index,
    {
        out.push(HEntry { bucket: source[index].bucket.dup(), freq: source[index].freq.dup() });
        index += 1;
    }
    proof {
        assert forall|bucket: int| hist_val(out@, bucket) == hist_val(source@, bucket) by {
            lemma_sum_aligned(out@, source@, hist_term(bucket), hist_term(bucket));
        }
    }
    out
}

pub fn copy_hists(source: &Vec<Vec<HEntry>>) -> (out: Vec<Vec<HEntry>>)
    requires forall|j: int| 0 <= j < source@.len() ==> hist_ok((#[trigger] source@[j])@),
    ensures out@.len() == source@.len(), forall|j: int| 0 <= j < out@.len() ==> hist_ok((#[trigger] out@[j])@),
        forall|j: int, bucket: int| 0 <= j < out@.len() ==> #[trigger] hist_val(out@[j]@, bucket) == hist_val(source@[j]@, bucket),
{
    let mut out: Vec<Vec<HEntry>> = Vec::new();
    let mut index = 0usize;
    while index < source.len()
        invariant index <= source.len(), out@.len() == index,
            forall|j: int| 0 <= j < source@.len() ==> hist_ok((#[trigger] source@[j])@),
            forall|j: int| 0 <= j < index ==> hist_ok((#[trigger] out@[j])@),
            forall|j: int, bucket: int| 0 <= j < index ==> #[trigger] hist_val(out@[j]@, bucket) == hist_val(source@[j]@, bucket),
        decreases source.len() - index,
    {
        out.push(copy_hist(&source[index]));
        index += 1;
    }
    out
}

fn empty_hists() -> (out: Vec<Vec<HEntry>>)
    ensures out@.len() == 16, forall|j: int| 0 <= j < 16 ==> (#[trigger] out@[j])@.len() == 0,
{
    let mut out: Vec<Vec<HEntry>> = Vec::new();
    let mut index = 0usize;
    while index < 16
        invariant index <= 16, out@.len() == index, forall|j: int| 0 <= j < index ==> (#[trigger] out@[j])@.len() == 0,
        decreases 16 - index,
    {
        out.push(Vec::new());
        index += 1;
    }
    out
}

pub proof fn lemma_empty_db(db: Db, patterns: Seq<Vec<u8>>, hists: Seq<Vec<HEntry>>)
    requires db.customer.len() == 0, db.orders.len() == 0, hists.len() == 16, patterns.len() == 16,
        forall|j: int| 0 <= j < 16 ==> (#[trigger] hists[j])@.len() == 0,
    ensures hists_ok(hists, patterns, db),
        forall|key: int| customer_count(db, key) == 0,
        forall|key: int, comment: Seq<u8>| oc_comment(db, key, comment) == 0,
{
    assert forall|j: int| 0 <= j < 16 implies hist_ok((#[trigger] hists[j])@) by {}
    assert forall|j: int, bucket: int| 0 <= j < 16 implies #[trigger] hist_val(hists[j]@, bucket) == histogram_count(db, patterns[j]@, bucket) by {
        lemma_sum_empty(hists[j]@, hist_term(bucket));
        assert(customer_keys(db) =~= Seq::<int>::empty());
        lemma_sum_empty(customer_keys(db), |key: int| contribution(db, patterns[j]@, key, bucket));
    }
    assert forall|key: int| customer_count(db, key) == 0 by {
        lemma_sum_empty(db.customer, |row: CustomerRow| ind(row.c_custkey == key));
    }
    assert forall|key: int, comment: Seq<u8>| oc_comment(db, key, comment) == 0 by {
        lemma_sum_empty(db.orders, |row: OrdersRow| ind(row.o_custkey == key && row.o_comment == comment));
    }
}

#[verifier::rlimit(100)]
pub fn build(customers: &[CustomerScan], orders: Vec<OrderScan>, Ghost(db): Ghost<Db>) -> (result: Option<State>)
    requires scan_customers_ok(db, customers@), scan_orders_ok(db, orders@),
    ensures result matches Some(state) ==> state.db() == db, result matches Some(state) ==> history(state) == 0, envelope(db) ==> result is Some,
{
    let ghost scanned = orders@;
    let ob = scan_orders(orders);
    let cb = scan_customers(customers);
    let patterns = tracked_patterns();
    let mut hists = empty_hists();
    let ghost empty = Db { customer: Seq::empty(), orders: Seq::empty() };
    let empty_table: Index<Rec> = Index::new();
    proof {
        lemma_empty_db(empty, patterns@, hists@);
        assert forall|key: verus_prooflib::ordered::KeyView| shape_ok(#[trigger] empty_table.at(key), key) by {}
        assert forall|key: int| #[trigger] rec_mult_of(tbl_rec(empty_table, key)) == customer_count(empty, key) by {}
        assert forall|key: int, comment: Seq<u8>| #[trigger] rec_cc_of(tbl_rec(empty_table, key), comment) == oc_comment(empty, key, comment) by {}
        assert forall|g: spec_fn((int, Seq<u8>)) -> int| #[trigger] osum(db.orders, g) == osum(empty.orders, g) + obsum(ob@, g) by {
            lemma_sum_empty(empty.orders, |row: OrdersRow| g((row.o_custkey, row.o_comment)));
            assert(sum(order_scan_seq_view(scanned), g) == sum(db.orders, |orders: OrdersRow| if true { g((orders.o_custkey, orders.o_comment)) } else { 0 }));
            lemma_sum_ext_all(db.orders, |orders: OrdersRow| if true { g((orders.o_custkey, orders.o_comment)) } else { 0 },
                |row: OrdersRow| g((row.o_custkey, row.o_comment)));
        }
        assert forall|f: spec_fn(CustomerRow) -> int| #[trigger] sum(db.customer, f) == sum(empty.customer, f) + csum(cb@, f) by {
            lemma_sum_empty(empty.customer, f);
        }
    }
    let table = merge_batch(&empty_table, &mut hists, &patterns, &ob, &cb, Ghost(empty), Ghost(db));
    Some(State { table, hists, patterns, db: Ghost(db) })
}

pub fn execute(state: &State, p1: &[u8]) -> (result: Option<Vec<(i64, i64)>>)
    ensures result matches Some(out) ==> query_result(state.db(), params(p1@), out_view(out@)),
        envelope(state.db()) && answer_fits(state.db(), params(p1@)) ==> result is Some,
{
    proof { use_type_invariant(state); }
    let mut j = 0usize;
    while j < state.patterns.len()
        invariant j <= state.patterns.len(), table_ok(state.table, state.db@), hists_ok(state.hists@, state.patterns@, state.db@),
            forall|k: int| 0 <= k < j ==> state.patterns@[k]@ != p1@,
        decreases state.patterns.len() - j,
    {
        if bytes_eq(state.patterns[j].as_slice(), p1) {
            return rows_from_hist(&state.hists[j], p1, Ghost(state.db@));
        }
        j += 1;
    }
    let hist = scan_table(&state.table, p1, Ghost(state.db@));
    rows_from_hist(&hist, p1, Ghost(state.db@))
}

#[verifier::rlimit(100)]
pub fn apply(state: &State, customer_delta: &[CustomerDelta], orders_delta: &Vec<OrderDelta>, Ghost(orders_full): Ghost<Seq<(OrdersRow, int)>>, Ghost(db2): Ghost<Db>) -> (result: Result<State, u32>)
    requires orders_delta_ok(orders_delta@, orders_full), capture_ok(state.db(), db2, customer_delta_seq_view(customer_delta@), orders_full),
    ensures result matches Ok(next) ==> next.db() == db2, result matches Ok(next) ==> history(next) <= history(*state) + customer_delta@.len() + orders_delta@.len(),
        envelope(state.db()) && envelope(db2) && history(*state) + customer_delta@.len() + orders_delta@.len() <= max_history() ==> result is Ok,
{
    proof { use_type_invariant(state); }
    let ob = delta_orders(orders_delta);
    let cb = delta_customers(customer_delta);
    let mut hists = copy_hists(&state.hists);
    let ghost db = state.db();
    proof {
        let cdelta = customer_delta_seq_view(customer_delta@);
        assert forall|g: spec_fn((int, Seq<u8>)) -> int| #[trigger] osum(db2.orders, g) == osum(db.orders, g) + obsum(ob@, g) by {
            let gg = |row: OrdersRow| g((row.o_custkey, row.o_comment));
            let term = |d: (OrdersRow, int)| d.1 * gg(d.0);
            assert(sum(db2.orders, gg) == sum(db.orders, gg) + sum(orders_full, term));
            assert forall|k: int| 0 <= k < ob@.len() implies scan_term(g)(#[trigger] ob@[k]) == term(orders_full[k]) by {
                assert(ob@[k].sign as int == orders_full[k].1);
            }
            lemma_sum_aligned(ob@, orders_full, scan_term(g), term);
        }
        assert forall|f: spec_fn(CustomerRow) -> int| #[trigger] sum(db2.customer, f) == sum(db.customer, f) + csum(cb@, f) by {
            let term = |d: (CustomerRow, int)| d.1 * f(d.0);
            assert(sum(db2.customer, f) == sum(db.customer, f) + sum(cdelta, term));
            let cterm = |change: CChange| change.sign as int * f(CustomerRow { c_custkey: change.custkey as int });
            assert forall|k: int| 0 <= k < cb@.len() implies cterm(#[trigger] cb@[k]) == term(cdelta[k]) by {
                assert(cb@[k].sign as int == cdelta[k].1);
            }
            lemma_sum_aligned(cb@, cdelta, cterm, term);
        }
    }
    let table = merge_batch(&state.table, &mut hists, &state.patterns, &ob, &cb, Ghost(db), Ghost(db2));
    let patterns = copy_patterns(&state.patterns);
    Ok(State { table, hists, patterns, db: Ghost(db2) })
}

fn copy_patterns(source: &Vec<Vec<u8>>) -> (out: Vec<Vec<u8>>)
    ensures out@.len() == source@.len(), forall|j: int| 0 <= j < out@.len() ==> out@[j]@ == source@[j]@,
{
    let mut out: Vec<Vec<u8>> = Vec::new();
    let mut index = 0usize;
    while index < source.len()
        invariant index <= source.len(), out@.len() == index, forall|j: int| 0 <= j < index ==> out@[j]@ == source@[j]@,
        decreases source.len() - index,
    {
        out.push(copy_bytes(&source[index]));
        index += 1;
    }
    out
}

}
