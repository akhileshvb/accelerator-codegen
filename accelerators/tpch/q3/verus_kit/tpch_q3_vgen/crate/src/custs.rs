// The customer cells, in a compact persistent ordered map (a `Paged` set of (customer, cell)
// entries) with the map interface the proofs use: `at`, `wf`, `get`, `put`, `remove`, `share`.
use std::sync::Arc;
use vstd::prelude::*;
use verus_common::bytes::*;
use verus_prooflib::ordered::{Key, KeyView, key_lt};
use crate::model::CustomerCell;
use crate::paged::{Paged, Ent};

verus! {

#[derive(Clone)]
pub struct CustEnt {
    pub customer: i64,
    pub cell: Arc<CustomerCell>,
}

impl Ent for CustEnt {
    fn dup(&self) -> (result: Self) { CustEnt { customer: self.customer, cell: self.cell.clone() } }
    open spec fn kv(self) -> KeyView { (self.customer as int, Seq::<u8>::empty()) }
    open spec fn top() -> KeyView { (i64::MAX as int, seq![0u8]) }
    fn key(&self) -> (result: Key) { Key::number(self.customer) }
    fn top_key() -> (result: Key) {
        let mut bytes = Vec::new();
        bytes.push(0u8);
        Key::composite(i64::MAX, bytes)
    }
    fn before(&self, other: &Self) -> (result: bool) {
        proof { lemma_bytes_lt_irrefl(Seq::<u8>::empty()); }
        self.customer < other.customer
    }
    fn same_key(&self, other: &Self) -> (result: bool) { self.customer == other.customer }
    proof fn top_above(entry: Self) {
        assert(bytes_lt(Seq::<u8>::empty(), seq![0u8]));
    }
}

pub struct Custs {
    pub paged: Paged<CustEnt>,
    pub probe: Arc<CustomerCell>,
}

impl Custs {
    pub open spec fn wf(self) -> bool { self.paged.wf() }

    pub open spec fn at(self, key: KeyView) -> Option<CustomerCell> {
        match self.paged.at(key) { Some(entry) => Some(*entry.cell), None => None }
    }

    pub fn new() -> (result: Self)
        ensures result.wf(), forall|key: KeyView| #![trigger result.at(key)] result.at(key).is_none(),
    {
        let probe = Arc::new(CustomerCell { segments: Vec::new(), orders: Vec::new() });
        Custs { paged: Paged::new(), probe }
    }

    pub fn share(&self) -> (result: Self)
        requires self.wf(),
        ensures result.wf(), forall|key: KeyView| #![trigger result.at(key)] result.at(key) == self.at(key),
    {
        let result = Custs { paged: self.paged.share(), probe: self.probe.clone() };
        proof {
            assert forall|key: KeyView| #![trigger result.at(key)] result.at(key) == self.at(key) by {
                assert(result.paged.at(key) == self.paged.at(key));
            }
        }
        result
    }

    pub fn get(&self, customer: i64) -> (result: Option<Arc<CustomerCell>>)
        requires self.wf(),
        ensures match result {
            Some(cell) => self.at((customer as int, Seq::<u8>::empty())) == Some(*cell),
            None => self.at((customer as int, Seq::<u8>::empty())).is_none(),
        },
    {
        let probe = CustEnt { customer, cell: self.probe.clone() };
        match self.paged.get(&probe) {
            Some(entry) => Some(entry.cell),
            None => None,
        }
    }

    pub fn put(&mut self, customer: i64, cell: Arc<CustomerCell>)
        requires old(self).wf(),
        ensures final(self).wf(),
            forall|key: KeyView| #![trigger final(self).at(key)]
                final(self).at(key) == if key == (customer as int, Seq::<u8>::empty()) { Some(*cell) } else { old(self).at(key) },
    {
        let entry = CustEnt { customer, cell };
        self.paged.put(entry);
    }

    pub fn remove(&mut self, customer: i64)
        requires old(self).wf(),
        ensures final(self).wf(),
            forall|key: KeyView| #![trigger final(self).at(key)]
                final(self).at(key) == if key == (customer as int, Seq::<u8>::empty()) { None } else { old(self).at(key) },
    {
        let probe = CustEnt { customer, cell: self.probe.clone() };
        self.paged.remove(&probe);
    }
}

}
