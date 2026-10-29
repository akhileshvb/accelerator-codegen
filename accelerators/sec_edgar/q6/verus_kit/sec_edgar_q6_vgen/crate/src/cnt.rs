// How many entries an index holds: a list of distinct present keys is no longer than the index.
#![allow(unused_imports)]
use vstd::prelude::*;
use vstd::set_lib::*;
use verus_common::rel::*;
use verus_prooflib::ordered::{Index, KeyView, lemma_ordered_unique};

verus! {

pub open spec fn ix_keys<V>(ix: Index<V>) -> Seq<KeyView> {
    ix.entries().map_values(|e: (KeyView, V)| e.0)
}

proof fn lemma_keys_nodup<V>(ix: Index<V>)
    requires ix.wf(),
    ensures ix_keys(ix).no_duplicates(), forall|k: KeyView| ix_keys(ix).contains(k) <==> ix.at(k) is Some,
{
    ix.model();
    lemma_ordered_unique(ix.entries());
    let ks = ix_keys(ix);
    assert forall|a: int, b: int| 0 <= a < b < ks.len() implies ks[a] != ks[b] by {
        assert(ks[a] == ix.entries()[a].0 && ks[b] == ix.entries()[b].0);
    }
    assert forall|k: KeyView| ks.contains(k) <==> ix.at(k) is Some by {
        if ks.contains(k) {
            let q = choose|q: int| 0 <= q < ks.len() && ks[q] == k;
            assert(ix.at(ix.entries()[q].0) == Some(ix.entries()[q].1));
        }
        if ix.at(k) is Some {
            let pos = choose|pos: int| #![auto] 0 <= pos < ix.entries().len() && ix.entries()[pos].0 == k;
            assert(ks[pos] == k);
        }
    }
}

/// Distinct present keys are at most the entries.
pub proof fn lemma_len_ge<V>(a: Seq<KeyView>, ix: Index<V>)
    requires ix.wf(), a.no_duplicates(), forall|i: int| 0 <= i < a.len() ==> ix.at(#[trigger] a[i]) is Some,
    ensures a.len() <= ix.entries().len(),
{
    lemma_keys_nodup(ix);
    let ks = ix_keys(ix);
    ks.unique_seq_to_set();
    a.unique_seq_to_set();
    assert(a.to_set().subset_of(ks.to_set())) by {
        assert forall|k: KeyView| a.to_set().contains(k) implies ks.to_set().contains(k) by {
            assert(a.contains(k));
            let q = choose|q: int| 0 <= q < a.len() && a[q] == k;
            assert(ix.at(k) is Some);
            assert(ks.contains(k));
        }
    }
    lemma_len_subset(a.to_set(), ks.to_set());
}

/// An index with at most the keys of another is no longer.
pub proof fn lemma_len_le<V, W>(a: Index<V>, b: Index<W>)
    requires a.wf(), b.wf(), forall|q: KeyView| #![trigger a.at(q)] a.at(q) is Some ==> b.at(q) is Some,
    ensures a.entries().len() <= b.entries().len(),
{
    lemma_keys_nodup(a);
    let ka = ix_keys(a);
    assert forall|i: int| 0 <= i < ka.len() implies b.at(#[trigger] ka[i]) is Some by { assert(ka.contains(ka[i])); }
    lemma_len_ge(ka, b);
}

} // verus!
