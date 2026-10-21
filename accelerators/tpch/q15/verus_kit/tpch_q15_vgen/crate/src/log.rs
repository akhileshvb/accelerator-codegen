#![verifier::loop_isolation(false)]
// Signed logs and chunked persistent storage shared by the general path.
use vstd::prelude::*;
use std::sync::Arc;
use verus_common::sums::*;
use verus_common::rel::*;

verus! {

broadcast use vstd::std_specs::hash::group_hash_axioms, vstd::seq_lib::group_seq_properties;

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

pub struct Log<T> { pub chunks: Arc<Vec<Arc<Vec<T>>>> }

pub fn share_chunks<T>(source: &Vec<Arc<Vec<T>>>) -> (out: Vec<Arc<Vec<T>>>)
    ensures out@ == source@,
{
    let mut out = Vec::new();
    let mut index = 0usize;
    while index < source.len()
        invariant index <= source.len(), out@ == source@.take(index as int),
        decreases source.len() - index,
    {
        out.push(Arc::clone(&source[index]));
        index += 1;
    }
    out
}

impl<T> Log<T> {
    pub open spec fn views(self) -> Seq<Seq<T>> {
        (*self.chunks)@.map_values(|chunk: Arc<Vec<T>>| (*chunk)@)
    }

    pub open spec fn entries(self) -> Seq<T> { flat(self.views()) }

    pub fn empty() -> (out: Self)
        ensures out.entries() == Seq::<T>::empty(),
    {
        let out = Log { chunks: Arc::new(Vec::new()) };
        proof { assert(out.views() =~= Seq::<Seq<T>>::empty()); }
        out
    }

    pub fn with_chunk(&self, chunk: Vec<T>) -> (out: Self)
        ensures out.views() == self.views().push(chunk@), out.entries() == self.entries() + chunk@,
    {
        let mut chunks = share_chunks(&self.chunks);
        let ghost before = self.views();
        chunks.push(Arc::new(chunk));
        let out = Log { chunks: Arc::new(chunks) };
        proof {
            assert(out.views() =~= before.push(chunk@));
            assert(before.push(chunk@).drop_last() =~= before);
            assert(flat(before.push(chunk@)) == flat(before) + chunk@);
        }
        out
    }
}

pub proof fn lemma_flat_prefix_len<A>(chunks: Seq<Seq<A>>, count: int)
    requires 0 <= count <= chunks.len(),
    ensures flat(chunks.take(count)).len() <= flat(chunks).len(),
    decreases chunks.len() - count,
{
    if count < chunks.len() {
        lemma_flat_prefix_len(chunks, count + 1);
        assert(chunks.take(count + 1).drop_last() =~= chunks.take(count));
        assert(flat(chunks.take(count + 1)) == flat(chunks.take(count)) + chunks[count]);
    }
}

pub proof fn lemma_flat_step<A>(chunks: Seq<Seq<A>>, count: int)
    requires 0 <= count < chunks.len(),
    ensures flat(chunks.take(count + 1)) == flat(chunks.take(count)) + chunks[count],
{
    assert(chunks.take(count + 1).drop_last() =~= chunks.take(count));
}

}
