// A table kept as the list of signed row events that built it: the initial scan (sign +1) and
// every later delta, one shared chunk per call.
use vstd::prelude::*;
use std::sync::Arc;
use verus_common::rel::*;

verus! {

pub struct Log<T> {
    pub chunks: Vec<Arc<Vec<T>>>,
}

impl<T> Log<T> {
    /// The chunks, oldest first.
    pub open spec fn parts(self) -> Seq<Seq<T>> {
        self.chunks@.map_values(|c: Arc<Vec<T>>| (*c)@)
    }

    /// All events, oldest first.
    pub open spec fn events(self) -> Seq<T> {
        flat(self.parts())
    }

    pub fn empty() -> (r: Log<T>)
        ensures r.events() == Seq::<T>::empty(),
    {
        Log { chunks: Vec::new() }
    }

    /// This log followed by one more chunk.
    pub fn extended(&self, rows: Vec<T>) -> (r: Log<T>)
        ensures r.events() == self.events() + rows@, r.parts() == self.parts().push(rows@),
    {
        let mut chunks: Vec<Arc<Vec<T>>> = Vec::new();
        let mut i = 0;
        while i < self.chunks.len()
            invariant i <= self.chunks.len(),
                chunks@.len() == i, forall|j: int| 0 <= j < i ==> chunks@[j] == self.chunks@[j],
            decreases self.chunks.len() - i,
        {
            chunks.push(Arc::clone(&self.chunks[i]));
            i += 1;
        }
        chunks.push(Arc::new(rows));
        let r = Log { chunks };
        proof {
            assert(r.parts() =~= self.parts().push(r.chunks@.last()@));
            assert(r.events() == flat(self.parts().push((*r.chunks@.last())@)));
            assert(self.parts().push(rows@).drop_last() =~= self.parts());
        }
        r
    }
}

}
