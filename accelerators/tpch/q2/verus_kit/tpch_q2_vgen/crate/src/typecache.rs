use vstd::prelude::*;
use verus_prooflib::like_match_exec;
use verus_common::sqlfns::like_match;
use crate::bags::copy_bytes;

verus! {

/// A cheap hash of a byte string (low 10 bits).
pub open spec fn cheap_hash(bytes: Seq<u8>) -> int
    decreases bytes.len(),
{
    if bytes.len() == 0 { 0 }
    else { (cheap_hash(bytes.drop_last()) * 31 + bytes.last() as int) % 1024 }
}

pub fn cheap_hash_exec(bytes: &[u8]) -> (result: usize)
    ensures result as int == cheap_hash(bytes@), result < 1024,
{
    let mut value: usize = 0;
    let mut index = 0;
    while index < bytes.len()
        invariant index <= bytes.len(), value < 1024,
            value as int == cheap_hash(bytes@.take(index as int)),
        decreases bytes.len() - index,
    {
        value = (value * 31 + bytes[index] as usize) % 1024;
        proof {
            assert(bytes@.take(index as int + 1).drop_last() =~= bytes@.take(index as int));
            assert(bytes@.take(index as int + 1).last() == bytes@[index as int]);
        }
        index += 1;
    }
    proof { assert(bytes@.take(index as int) =~= bytes@); }
    value
}

pub fn same_bytes(left: &[u8], right: &[u8]) -> (result: bool)
    ensures result == (left@ == right@),
{
    if left.len() != right.len() { return false; }
    let mut index = 0;
    while index < left.len()
        invariant index <= left.len(), left.len() == right.len(),
            forall|probe: int| 0 <= probe < index ==> left@[probe] == right@[probe],
        decreases left.len() - index,
    {
        if left[index] != right[index] { return false; }
        index += 1;
    }
    proof { assert(left@ =~= right@); }
    true
}

/// The answers of one LIKE pattern for the distinct strings seen so far.
pub struct TypeCache {
    pub buckets: Vec<Vec<(Vec<u8>, bool)>>,
}

impl TypeCache {
    pub open spec fn valid(self, pat: Seq<u8>) -> bool {
        &&& self.buckets@.len() == 1024
        &&& forall|slot: int, entry: int| 0 <= slot < 1024 && 0 <= entry < (#[trigger] self.buckets@[slot]).len() ==> {
            let item = #[trigger] self.buckets@[slot]@[entry];
            cheap_hash(item.0@) == slot && item.1 == like_match(item.0@, pat)
        }
    }

    pub fn new(Ghost(pat): Ghost<Seq<u8>>) -> (result: Self)
        ensures result.valid(pat),
    {
        let mut buckets: Vec<Vec<(Vec<u8>, bool)>> = Vec::new();
        let mut index = 0;
        while index < 1024
            invariant index <= 1024, buckets@.len() == index,
                forall|slot: int| 0 <= slot < index ==> (#[trigger] buckets@[slot]).len() == 0,
            decreases 1024 - index,
        {
            buckets.push(Vec::new());
            index += 1;
        }
        TypeCache { buckets }
    }

    pub fn check(&mut self, text: &[u8], pat: &[u8]) -> (result: bool)
        requires old(self).valid(pat@),
        ensures final(self).valid(pat@), result == like_match(text@, pat@),
    {
        let slot = cheap_hash_exec(text);
        let mut entry = 0;
        while entry < self.buckets[slot].len()
            invariant self.valid(pat@), slot < 1024, slot == cheap_hash(text@), entry <= self.buckets@[slot as int].len(),
                *self == *old(self),
            decreases self.buckets@[slot as int].len() - entry,
        {
            if same_bytes(&self.buckets[slot][entry].0, text) {
                return self.buckets[slot][entry].1;
            }
            entry += 1;
        }
        let matched = like_match_exec(text, pat);
        let owned = copy_bytes(text);
        let ghost before = self.buckets@;
        self.buckets[slot].push((owned, matched));
        proof {
            assert forall|s: int, e: int| 0 <= s < 1024 && 0 <= e < (#[trigger] self.buckets@[s]).len() implies {
                let item = #[trigger] self.buckets@[s]@[e];
                cheap_hash(item.0@) == s && item.1 == like_match(item.0@, pat@)
            } by {
                if s == slot as int && e == before[s].len() as int {
                } else if s == slot as int {
                    assert(self.buckets@[s]@[e] == before[s]@[e]);
                }
            }
        }
        matched
    }
}

}
