use vstd::prelude::*;
use std::sync::Arc;
use crate::bags::copy_bytes;
use crate::typecache::{cheap_hash, cheap_hash_exec, same_bytes};

verus! {

/// Interned strings (manufacturers and part types): id -> bytes, with a hash index for lookup.
pub struct Dict {
    pub strings: Vec<Arc<Vec<u8>>>,
    pub index: Vec<Vec<usize>>,
}

pub open spec fn dict_seq(strings: Seq<Arc<Vec<u8>>>) -> Seq<Seq<u8>> {
    strings.map_values(|text: Arc<Vec<u8>>| (*text)@)
}

pub proof fn lemma_dict_ids_eq(d: Dict, first: int, second: int)
    requires d.valid(), 0 <= first < d.len(), 0 <= second < d.len(), d.text(first) == d.text(second),
    ensures first == second,
{
    if first < second { assert((*d.strings@[first])@ != (*d.strings@[second])@); }
    if second < first { assert((*d.strings@[second])@ != (*d.strings@[first])@); }
}

impl Dict {
    pub open spec fn view_strings(self) -> Seq<Seq<u8>> { dict_seq(self.strings@) }

    pub open spec fn len(self) -> int { self.strings@.len() as int }

    pub open spec fn text(self, id: int) -> Seq<u8> { (*self.strings@[id])@ }

    pub open spec fn valid(self) -> bool {
        &&& self.index@.len() == 1024
        &&& forall|first: int, second: int| 0 <= first < second < self.strings@.len() ==>
            (*(#[trigger] self.strings@[first]))@ != (*(#[trigger] self.strings@[second]))@
        &&& forall|id: usize| id < self.strings@.len() ==>
            (#[trigger] self.index@[cheap_hash((*self.strings@[id as int])@)])@.contains(id)
        &&& forall|slot: int, position: int| 0 <= slot < 1024 && 0 <= position < self.index@[slot]@.len() ==>
            (#[trigger] self.index@[slot]@[position]) < self.strings@.len()
            && cheap_hash((*self.strings@[self.index@[slot]@[position] as int])@) == slot
    }

    pub fn new() -> (result: Self)
        ensures result.valid(), result.len() == 0,
    {
        let mut index: Vec<Vec<usize>> = Vec::new();
        let mut slot = 0;
        while slot < 1024
            invariant slot <= 1024, index@.len() == slot,
                forall|s: int| 0 <= s < slot ==> (#[trigger] index@[s])@.len() == 0,
            decreases 1024 - slot,
        {
            index.push(Vec::new());
            slot += 1;
        }
        Dict { strings: Vec::new(), index }
    }

    pub fn find(&self, text: &[u8]) -> (result: Option<usize>)
        requires self.valid(),
        ensures
            match result {
                Some(id) => id < self.strings@.len() && (*self.strings@[id as int])@ == text@,
                None => forall|id: usize| id < self.strings@.len() ==> (*self.strings@[id as int])@ != text@,
            },
    {
        let slot = cheap_hash_exec(text);
        let mut position = 0;
        while position < self.index[slot].len()
            invariant self.valid(), slot < 1024, slot == cheap_hash(text@), position <= self.index@[slot as int]@.len(),
                forall|p: int| 0 <= p < position ==> (*self.strings@[self.index@[slot as int]@[p] as int])@ != text@,
            decreases self.index@[slot as int]@.len() - position,
        {
            let id = self.index[slot][position];
            if same_bytes(&self.strings[id], text) { return Some(id); }
            position += 1;
        }
        proof {
            assert(position == self.index@[slot as int]@.len());
            assert forall|id: usize| id < self.strings@.len() implies (*self.strings@[id as int])@ != text@ by {
                if (*self.strings@[id as int])@ == text@ {
                    assert(cheap_hash((*self.strings@[id as int])@) == slot as int);
                    assert(self.index@[slot as int]@.contains(id));
                    let p = choose|p: int| 0 <= p < self.index@[slot as int]@.len() && self.index@[slot as int]@[p] == id;
                    assert((*self.strings@[self.index@[slot as int]@[p] as int])@ != text@);
                }
            }
        }
        None
    }

    /// The id of `text`, adding it when it is new.  Existing ids keep their strings.
    pub fn intern(&mut self, text: &[u8]) -> (result: usize)
        requires old(self).valid(), old(self).strings@.len() < 1000000,
        ensures final(self).valid(), result < final(self).strings@.len(), (*final(self).strings@[result as int])@ == text@,
            final(self).strings@.len() >= old(self).strings@.len(), final(self).strings@.len() <= old(self).strings@.len() + 1,
            forall|id: int| 0 <= id < old(self).strings@.len() ==> final(self).strings@[id] == old(self).strings@[id],
    {
        if let Some(id) = self.find(text) { return id; }
        let ghost before = *self;
        let id = self.strings.len();
        let slot = cheap_hash_exec(text);
        let owned = Arc::new(copy_bytes(text));
        self.strings.push(owned);
        self.index[slot].push(id);
        proof {
            broadcast use vstd::seq_lib::group_seq_properties;
            assert(self.strings@ == before.strings@.push(self.strings@[id as int]));
            assert forall|first: int, second: int| 0 <= first < second < self.strings@.len() implies
                (*(#[trigger] self.strings@[first]))@ != (*(#[trigger] self.strings@[second]))@ by {
                if second == id as int {
                    assert(before.strings@.len() == id as int);
                    assert((*before.strings@[first])@ != text@);
                }
            }
            assert forall|i: usize| i < self.strings@.len() implies
                (#[trigger] self.index@[cheap_hash((*self.strings@[i as int])@)])@.contains(i) by {
                let h = cheap_hash((*self.strings@[i as int])@);
                if i < id {
                    assert(self.strings@[i as int] == before.strings@[i as int]);
                    assert(before.index@[h]@.contains(i));
                    if h == slot as int {
                        let p = choose|p: int| 0 <= p < before.index@[h]@.len() && before.index@[h]@[p] == i;
                        assert(self.index@[h]@[p] == i);
                    } else { assert(self.index@[h] == before.index@[h]); }
                } else {
                    assert(h == slot as int);
                    assert(self.index@[h]@.last() == id);
                }
            }
            assert forall|s: int, p: int| 0 <= s < 1024 && 0 <= p < self.index@[s]@.len() implies
                (#[trigger] self.index@[s]@[p]) < self.strings@.len()
                && cheap_hash((*self.strings@[self.index@[s]@[p] as int])@) == s by {
                if s == slot as int && p == before.index@[s]@.len() as int {
                } else if s == slot as int {
                    assert(self.index@[s]@[p] == before.index@[s]@[p]);
                    let k = before.index@[s]@[p];
                    assert(self.strings@[k as int] == before.strings@[k as int]);
                } else {
                    assert(self.index@[s] == before.index@[s]);
                    let k = before.index@[s]@[p];
                    assert(self.strings@[k as int] == before.strings@[k as int]);
                }
            }
        }
        id
    }

    pub fn share(&self) -> (result: Self)
        requires self.valid(),
        ensures result.strings@ == self.strings@, result.index@.len() == self.index@.len(),
            forall|s: int| 0 <= s < self.index@.len() ==> (#[trigger] result.index@[s])@ == self.index@[s]@,
            result.valid(),
    {
        let mut strings: Vec<Arc<Vec<u8>>> = Vec::with_capacity(self.strings.len());
        let mut i = 0;
        while i < self.strings.len()
            invariant i <= self.strings.len(), strings@ == self.strings@.take(i as int),
            decreases self.strings.len() - i,
        {
            strings.push(Arc::clone(&self.strings[i]));
            proof { assert(self.strings@.take(i as int + 1) =~= self.strings@.take(i as int).push(self.strings@[i as int])); }
            i += 1;
        }
        proof { assert(self.strings@.take(i as int) =~= self.strings@); }
        let mut index: Vec<Vec<usize>> = Vec::with_capacity(1024);
        let mut slot = 0;
        while slot < self.index.len()
            invariant slot <= self.index.len(), index@.len() == slot, self.index@.len() == 1024,
                forall|s: int| 0 <= s < slot ==> (#[trigger] index@[s])@ == self.index@[s]@,
            decreases self.index.len() - slot,
        {
            let mut row: Vec<usize> = Vec::new();
            let mut j = 0;
            while j < self.index[slot].len()
                invariant slot < self.index.len(), j <= self.index@[slot as int]@.len(), row@ == self.index@[slot as int]@.take(j as int),
                decreases self.index@[slot as int]@.len() - j,
            {
                row.push(self.index[slot][j]);
                proof { assert(self.index@[slot as int]@.take(j as int + 1) =~= self.index@[slot as int]@.take(j as int).push(self.index@[slot as int]@[j as int])); }
                j += 1;
            }
            proof { assert(self.index@[slot as int]@.take(j as int) =~= self.index@[slot as int]@); }
            index.push(row);
            slot += 1;
        }
        let result = Dict { strings, index };
        proof {
            assert(result.valid()) by {
                assert forall|id: usize| id < result.strings@.len() implies
                    (#[trigger] result.index@[cheap_hash((*result.strings@[id as int])@)])@.contains(id) by {
                    assert(self.index@[cheap_hash((*self.strings@[id as int])@)]@.contains(id));
                }
                assert forall|sl: int, po: int| 0 <= sl < 1024 && 0 <= po < result.index@[sl]@.len() implies
                    (#[trigger] result.index@[sl]@[po]) < result.strings@.len()
                    && cheap_hash((*result.strings@[result.index@[sl]@[po] as int])@) == sl by {
                    assert(self.index@[sl]@[po] == result.index@[sl]@[po]);
                }
            }
        }
        result
    }
}

}
