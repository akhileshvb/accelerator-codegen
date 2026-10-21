#![verifier::loop_isolation(false)]
// The signed changes of a build (every scanned row an insert) and of a transaction's deltas.
use vstd::prelude::*;
use std::sync::Arc;
use verus_common::sums::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::bag_proofs::*;
use crate::batch::*;
use crate::core::*;

verus! {

pub fn copy_bytes(source: &Vec<u8>) -> (out: Vec<u8>)
    ensures out@ == source@,
{
    let mut out = Vec::with_capacity(source.len());
    let mut index = 0usize;
    while index < source.len()
        invariant index <= source.len(), out@ == source@.take(index as int),
        decreases source.len() - index,
    {
        out.push(source[index]);
        index += 1;
    }
    proof { assert(source@.take(source.len() as int) =~= source@); }
    out
}

pub open spec fn scan_term(f: spec_fn((int, Seq<u8>)) -> int) -> spec_fn(OChange) -> int {
    |change: OChange| change.sign as int * f((change.custkey as int, (*change.comment)@))
}

pub fn scan_customers(customers: &[CustomerScan]) -> (out: Vec<CChange>)
    ensures forall|f: spec_fn(CustomerRow) -> int| #[trigger] csum(out@, f) == sum(customer_scan_seq_view(customers@), f),
{
    let mut out: Vec<CChange> = Vec::new();
    let mut index = 0usize;
    while index < customers.len()
        invariant index <= customers.len(), out@.len() == index,
            forall|k: int| 0 <= k < index ==> out@[k].custkey == customers@[k].c_custkey && out@[k].sign == 1,
        decreases customers.len() - index,
    {
        out.push(CChange { custkey: customers[index].c_custkey, sign: 1 });
        index += 1;
    }
    proof {
        let view = customer_scan_seq_view(customers@);
        assert forall|f: spec_fn(CustomerRow) -> int| #[trigger] csum(out@, f) == sum(view, f) by {
            let term = |change: CChange| change.sign as int * f(CustomerRow { c_custkey: change.custkey as int });
            assert forall|k: int| 0 <= k < out@.len() implies term(#[trigger] out@[k]) == f(view[k]) by {
                assert(view[k] == customer_scan_view(customers@[k]));
            }
            lemma_sum_aligned(out@, view, term, f);
        }
    }
    out
}

pub fn scan_orders(orders: Vec<OrderScan>) -> (out: Vec<OChange>)
    ensures forall|f: spec_fn((int, Seq<u8>)) -> int| #[trigger] obsum(out@, f) == sum(order_scan_seq_view(orders@), f),
{
    let ghost original = order_scan_seq_view(orders@);
    let mut remaining = orders;
    let mut out: Vec<OChange> = Vec::new();
    proof {
        assert forall|f: spec_fn((int, Seq<u8>)) -> int| #[trigger] obsum(out@, f) + sum(order_scan_seq_view(remaining@), f) == sum(original, f) by {
            lemma_sum_empty(out@, scan_term(f));
        }
    }
    while remaining.len() > 0
        invariant
            forall|k: int| 0 <= k < out@.len() ==> out@[k].sign == 1,
            forall|f: spec_fn((int, Seq<u8>)) -> int| #[trigger] obsum(out@, f) + sum(order_scan_seq_view(remaining@), f) == sum(original, f),
        decreases remaining.len(),
    {
        let ghost before_out = out@;
        let ghost before_remaining = remaining@;
        let row = remaining.pop().unwrap();
        let ghost viewed = (row.o_custkey as int, row.o_comment@);
        out.push(OChange { custkey: row.o_custkey, comment: Arc::new(row.o_comment), sign: 1 });
        proof {
            assert(order_scan_seq_view(before_remaining) =~= order_scan_seq_view(remaining@).push(viewed));
            assert forall|f: spec_fn((int, Seq<u8>)) -> int| #[trigger] obsum(out@, f) + sum(order_scan_seq_view(remaining@), f) == sum(original, f) by {
                assert(obsum(before_out, f) + sum(order_scan_seq_view(before_remaining), f) == sum(original, f));
                assert(out@ =~= before_out.push(out@.last()));
                lemma_sum_push(before_out, out@.last(), scan_term(f));
                lemma_sum_push(order_scan_seq_view(remaining@), viewed, f);
                assert(scan_term(f)(out@.last()) == f(viewed));
            }
        }
    }
    proof {
        assert(order_scan_seq_view(remaining@) =~= Seq::<(int, Seq<u8>)>::empty());
        assert forall|f: spec_fn((int, Seq<u8>)) -> int| #[trigger] obsum(out@, f) == sum(original, f) by {
            lemma_sum_empty(order_scan_seq_view(remaining@), f);
        }
    }
    out
}

pub fn delta_customers(delta: &[CustomerDelta]) -> (out: Vec<CChange>)
    ensures out@.len() == delta@.len(),
        forall|k: int| 0 <= k < delta@.len() ==> out@[k].custkey == delta@[k].row.c_custkey && out@[k].sign == delta@[k].sign,
{
    let mut out: Vec<CChange> = Vec::new();
    let mut index = 0usize;
    while index < delta.len()
        invariant index <= delta.len(), out@.len() == index,
            forall|k: int| 0 <= k < index ==> out@[k].custkey == delta@[k].row.c_custkey && out@[k].sign == delta@[k].sign,
        decreases delta.len() - index,
    {
        out.push(CChange { custkey: delta[index].row.c_custkey, sign: delta[index].sign });
        index += 1;
    }
    out
}

pub fn delta_orders(delta: &Vec<OrderDelta>) -> (out: Vec<OChange>)
    ensures out@.len() == delta@.len(),
        forall|k: int| 0 <= k < delta@.len() ==> out@[k].custkey == delta@[k].o_custkey && out@[k].sign == delta@[k].sign
            && (*out@[k].comment)@ == delta@[k].o_comment@,
{
    let mut out: Vec<OChange> = Vec::new();
    let mut index = 0usize;
    while index < delta.len()
        invariant index <= delta.len(), out@.len() == index,
            forall|k: int| 0 <= k < index ==> out@[k].custkey == delta@[k].o_custkey && out@[k].sign == delta@[k].sign
                && (*out@[k].comment)@ == delta@[k].o_comment@,
        decreases delta.len() - index,
    {
        out.push(OChange { custkey: delta[index].o_custkey, comment: Arc::new(copy_bytes(&delta[index].o_comment)), sign: delta[index].sign });
        index += 1;
    }
    out
}

}
