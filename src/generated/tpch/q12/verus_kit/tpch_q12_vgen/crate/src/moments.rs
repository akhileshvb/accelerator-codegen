use vstd::prelude::*;
use std::collections::HashMap;
use std::sync::Arc;
use crate::bytes;
use crate::counts::Counts;
use crate::histogram::Sparse;
use crate::gen_obligations::*;
use verus_common::sums::sum;

verus! {

broadcast use vstd::seq_lib::group_seq_properties;

pub struct Orders {
    pub values: HashMap<i64, Counts>,
    pub keys: Vec<i64>,
}

impl Orders {
    pub open spec fn at(self, key: i64) -> Counts {
        if self.values@.contains_key(key) { self.values@[key] }
        else { Counts { high: 0, low: 0 } }
    }

    pub open spec fn wf(self) -> bool {
        self.keys@.no_duplicates() && self.keys@.to_set() == self.values@.dom()
    }

    pub fn new() -> (out: Self)
        ensures out.wf(), out.keys@.len() == 0,
    {
        let out = Orders { values: HashMap::new(), keys: Vec::new() };
        proof {
            out.keys@.to_set_ensures();
            assert(out.keys@.no_duplicates());
            assert(out.keys@.to_set() =~= out.values@.dom());
        }
        out
    }

    pub fn get(&self, key: i64) -> (out: Counts)
        ensures out == self.at(key),
    {
        match self.values.get(&key) { Some(value) => *value, None => Counts::zero() }
    }

    pub fn add(&mut self, key: i64, change: Counts) -> (out: bool)
        requires old(self).wf(),
        ensures final(self).wf(),
            out ==> final(self).at(key).high == old(self).at(key).high + change.high
                && final(self).at(key).low == old(self).at(key).low + change.low,
            forall|other: i64| #![trigger final(self).at(other)]
                other != key ==> final(self).at(other) == old(self).at(other),
    {
        let ghost before = *self;
        let value = match self.get(key).add(change) { Some(value) => value, None => return false };
        proof { self.keys@.to_set_ensures(); }
        if !self.values.contains_key(&key) {
            proof { verus_prooflib::lemma_unique_push(self.keys@, key); }
            self.keys.push(key);
        }
        self.values.insert(key, value);
        proof {
            self.keys@.to_set_ensures();
            assert(self.values@ == before.values@.insert(key, value));
            assert(self.keys@ == if before.values@.contains_key(key) { before.keys@ } else { before.keys@.push(key) });
            assert(self.keys@.to_set() =~= self.values@.dom());
        }
        true
    }
}

pub struct CellMode {
    pub mode: Arc<Vec<u8>>,
    pub days: Sparse,
}

pub struct Cells {
    pub buckets: HashMap<u64, Vec<CellMode>>,
    pub hashes: Vec<u64>,
}

impl Cells {
    pub open spec fn modes(self, hash_count: nat) -> Set<Seq<u8>>
        recommends hash_count <= self.hashes@.len(),
        decreases hash_count,
    {
        if hash_count == 0 { Set::empty() }
        else { self.modes((hash_count - 1) as nat).union(
            self.buckets@[self.hashes@[hash_count as int - 1]]@.map_values(|entry: CellMode| entry.mode@).to_set()) }
    }

    pub open spec fn mode_prefix(self, hash_count: nat, mode_count: int) -> Set<Seq<u8>> {
        self.modes(hash_count).union(if hash_count < self.hashes@.len() {
            self.buckets@[self.hashes@[hash_count as int]]@.take(mode_count).map_values(|entry: CellMode| entry.mode@).to_set()
        } else { Set::empty() })
    }

    pub proof fn lemma_mode_step(self, hash_count: nat, mode_count: int)
        requires self.wf(), hash_count < self.hashes@.len(),
            0 <= mode_count < self.buckets@[self.hashes@[hash_count as int]]@.len(),
        ensures self.mode_prefix(hash_count, mode_count + 1) == self.mode_prefix(hash_count, mode_count).insert(
            self.buckets@[self.hashes@[hash_count as int]][mode_count].mode@),
    {
        let bucket = self.buckets@[self.hashes@[hash_count as int]];
        assert(bucket@.take(mode_count + 1) =~= bucket@.take(mode_count).push(bucket[mode_count]));
        assert(bucket@.take(mode_count + 1).map_values(|entry: CellMode| entry.mode@) =~=
            bucket@.take(mode_count).map_values(|entry: CellMode| entry.mode@).push(bucket[mode_count].mode@));
        bucket@.take(mode_count).map_values(|entry: CellMode| entry.mode@).to_set_ensures();
        bucket@.take(mode_count + 1).map_values(|entry: CellMode| entry.mode@).to_set_ensures();
        assert(self.mode_prefix(hash_count, mode_count + 1) =~= self.mode_prefix(hash_count, mode_count).insert(bucket[mode_count].mode@));
    }

    pub proof fn lemma_hash_step(self, hash_count: nat)
        requires self.wf(), hash_count < self.hashes@.len(),
        ensures self.mode_prefix(hash_count, self.buckets@[self.hashes@[hash_count as int]]@.len() as int)
            == self.mode_prefix(hash_count + 1, 0),
    {
        let bucket = self.buckets@[self.hashes@[hash_count as int]];
        assert(bucket@.take(bucket@.len() as int) =~= bucket@);
        if hash_count + 1 < self.hashes@.len() {
            self.buckets@[self.hashes@[hash_count as int + 1]]@.take(0).map_values(|entry: CellMode| entry.mode@).to_set_ensures();
        }
        assert(self.mode_prefix(hash_count, bucket@.len() as int) =~= self.mode_prefix(hash_count + 1, 0));
    }

    pub proof fn lemma_mode_member(self, hash_count: nat, hash_index: int, mode_index: int)
        requires self.wf(), hash_count <= self.hashes@.len(), 0 <= hash_index < hash_count,
            0 <= mode_index < self.buckets@[self.hashes@[hash_index]]@.len(),
        ensures self.modes(hash_count).contains(self.buckets@[self.hashes@[hash_index]][mode_index].mode@),
        decreases hash_count,
    {
        if hash_index < hash_count - 1 { self.lemma_mode_member((hash_count - 1) as nat, hash_index, mode_index); }
        else {
            let bucket = self.buckets@[self.hashes@[hash_index]];
            bucket@.map_values(|entry: CellMode| entry.mode@).to_set_ensures();
            assert(bucket@.map_values(|entry: CellMode| entry.mode@)[mode_index] == bucket[mode_index].mode@);
            assert(bucket@.map_values(|entry: CellMode| entry.mode@).contains(bucket[mode_index].mode@));
        }
    }

    pub proof fn lemma_mode_complete(self, mode: Seq<u8>, day: i32)
        requires self.wf(), !self.modes(self.hashes@.len()).contains(mode),
        ensures self.at(mode, day).high == 0, self.at(mode, day).low == 0,
    {
        reveal(Cells::at);
        let code = bytes::hash_bytes(mode);
        if self.buckets@.contains_key(code) {
            self.hashes@.to_set_ensures();
            let position = choose|position: int| #![trigger self.hashes@[position]]
                0 <= position < self.hashes@.len() && self.hashes@[position] == code;
            let bucket = self.buckets@[code];
            if exists|index: int| #![trigger bucket[index]] 0 <= index < bucket@.len() && bucket[index].mode@ == mode {
                let index = choose|index: int| #![trigger bucket[index]] 0 <= index < bucket@.len() && bucket[index].mode@ == mode;
                self.lemma_mode_member(self.hashes@.len(), position, index);
            }
        }
    }

    pub proof fn lemma_modes_absent(self, count: nat, mode: Seq<u8>)
        requires self.wf(), count <= self.hashes@.len(),
            forall|index: int| #![trigger self.hashes@[index]] 0 <= index < count ==> self.hashes@[index] != bytes::hash_bytes(mode),
        ensures !self.modes(count).contains(mode),
        decreases count,
    {
        if count > 0 {
            self.lemma_modes_absent((count - 1) as nat, mode);
            let code = self.hashes@[count as int - 1];
            let bucket = self.buckets@[code];
            bucket@.map_values(|entry: CellMode| entry.mode@).to_set_ensures();
            assert forall|index: int| #![trigger bucket[index]] 0 <= index < bucket@.len() implies bucket[index].mode@ != mode by {}
        }
    }

    pub proof fn lemma_mode_fresh(self, hash_count: nat, mode_count: int)
        requires self.wf(), hash_count < self.hashes@.len(),
            0 <= mode_count < self.buckets@[self.hashes@[hash_count as int]]@.len(),
        ensures !self.mode_prefix(hash_count, mode_count).contains(self.buckets@[self.hashes@[hash_count as int]][mode_count].mode@),
    {
        let code = self.hashes@[hash_count as int];
        let bucket = self.buckets@[code];
        let mode = bucket[mode_count].mode@;
        assert forall|index: int| #![trigger self.hashes@[index]] 0 <= index < hash_count implies self.hashes@[index] != bytes::hash_bytes(mode) by {}
        self.lemma_modes_absent(hash_count, mode);
        bucket@.take(mode_count).map_values(|entry: CellMode| entry.mode@).to_set_ensures();
        assert(!bucket@.take(mode_count).map_values(|entry: CellMode| entry.mode@).contains(mode));
    }

    pub proof fn lemma_inactive(self, mode: Seq<u8>, day: i32)
        requires !self.active_mode(mode),
        ensures self.at(mode, day).high == 0, self.at(mode, day).low == 0,
    {
    }
    #[verifier::opaque]
    pub open spec fn at(self, mode: Seq<u8>, day: i32) -> Counts {
        let code = bytes::hash_bytes(mode);
        if self.buckets@.contains_key(code) {
            let bucket = self.buckets@[code];
            if exists|index: int| #![trigger bucket[index]]
                0 <= index < bucket.len() && bucket[index].mode@ == mode {
                let index = choose|index: int| #![trigger bucket[index]]
                    0 <= index < bucket.len() && bucket[index].mode@ == mode;
                bucket[index].days.at(day)
            } else { Counts { high: 0, low: 0 } }
        } else { Counts { high: 0, low: 0 } }
    }

    pub proof fn lemma_index(self, code: u64, index: int, day: i32)
        requires self.wf(), self.buckets@.contains_key(code),
            0 <= index < self.buckets@[code].len(),
        ensures self.at(self.buckets@[code][index].mode@, day) == self.buckets@[code][index].days.at(day),
    {
        reveal(Cells::at);
        let bucket = self.buckets@[code];
        let mode = bucket[index].mode@;
        assert(bytes::hash_bytes(mode) == code);
        assert(exists|offset: int| #![trigger bucket[offset]]
            0 <= offset < bucket.len() && bucket[offset].mode@ == mode);
        let found = choose|offset: int| #![trigger bucket[offset]]
            0 <= offset < bucket.len() && bucket[offset].mode@ == mode;
        if found < index { assert(bucket[found].mode@ != bucket[index].mode@); }
        if index < found { assert(bucket[index].mode@ != bucket[found].mode@); }
        assert(found == index);
    }

    pub open spec fn wf(self) -> bool {
        &&& self.hashes@.no_duplicates()
        &&& self.hashes@.to_set() == self.buckets@.dom()
        &&& forall|code: u64| #![trigger self.buckets@[code]]
            self.buckets@.contains_key(code) ==> {
                let bucket = self.buckets@[code];
                &&& forall|index: int| #![trigger bucket[index]]
                    0 <= index < bucket.len() ==> bucket[index].days.wf()
                        && bytes::hash_bytes(bucket[index].mode@) == code
                &&& forall|left: int, right: int| #![trigger bucket[left], bucket[right]]
                    0 <= left < right < bucket.len() ==> bucket[left].mode@ != bucket[right].mode@
            }
    }

    pub fn new() -> (out: Self)
        ensures out.wf(), out.hashes@.len() == 0,
            forall|mode: Seq<u8>, day: i32| #![trigger out.at(mode, day)]
                out.at(mode, day).high == 0 && out.at(mode, day).low == 0,
    {
        let out = Cells { buckets: HashMap::new(), hashes: Vec::new() };
        proof {
            reveal(Cells::at);
            out.hashes@.to_set_ensures();
            assert(out.hashes@.no_duplicates());
            assert(out.hashes@.to_set() =~= out.buckets@.dom());
        }
        out
    }

    pub fn find(&self, mode: &[u8]) -> (out: Option<&Sparse>)
        requires self.wf(),
        ensures out matches Some(days) ==> days.wf()
            && forall|day: i32| #![trigger days.at(day)] days.at(day) == self.at(mode@, day),
            out.is_none() ==> forall|day: i32| #![trigger self.at(mode@, day)]
                self.at(mode@, day).high == 0 && self.at(mode@, day).low == 0,
    {
        let code = bytes::hash(mode);
        let bucket = match self.buckets.get(&code) {
            Some(bucket) => bucket,
            None => { proof { reveal(Cells::at); } return None; },
        };
        let mut index = 0;
        while index < bucket.len()
            invariant index <= bucket@.len(), self.wf(), self.buckets@.contains_key(code),
                bucket@ == self.buckets@[code]@, code == bytes::hash_bytes(mode@),
                forall|offset: int| #![trigger bucket@[offset]]
                    0 <= offset < index ==> bucket@[offset].mode@ != mode@,
            decreases bucket@.len() - index,
        {
            if bytes::equal(&bucket[index].mode, mode) {
                proof {
                    assert forall|day: i32| #![trigger self.at(mode@, day)]
                        self.at(mode@, day) == bucket@[index as int].days.at(day) by {
                        self.lemma_index(code, index as int, day);
                    }
                }
                return Some(&bucket[index].days);
            }
            index += 1;
        }
        proof { reveal(Cells::at); }
        None
    }

    pub fn get(&self, mode: &[u8], day: i32) -> (out: Counts)
        requires self.wf(),
        ensures out == self.at(mode@, day),
    {
        match self.find(mode) { Some(days) => days.get(day), None => Counts::zero() }
    }

    pub fn add(&mut self, mode: &[u8], day: i32, change: Counts) -> (out: bool)
        requires old(self).wf(),
        ensures final(self).wf(),
            out ==> final(self).at(mode@, day).high == old(self).at(mode@, day).high + change.high
                && final(self).at(mode@, day).low == old(self).at(mode@, day).low + change.low,
            !out ==> final(self).at(mode@, day) == old(self).at(mode@, day),
            forall|other: Seq<u8>, date: i32| #![trigger final(self).at(other, date)]
                (other != mode@ || date != day) ==> final(self).at(other, date) == old(self).at(other, date),
    {
        let ghost before = *self;
        let code = bytes::hash(mode);
        proof { self.hashes@.to_set_ensures(); }
        let mut bucket = match self.buckets.remove(&code) {
            Some(bucket) => bucket,
            None => {
                proof { verus_prooflib::lemma_unique_push(self.hashes@, code); }
                self.hashes.push(code);
                Vec::new()
            },
        };
        let ghost original = bucket@;
        let mut index = 0;
        let mut found = false;
        while index < bucket.len() && !found
            invariant index <= bucket@.len(), bucket@ == original, before.wf(),
                before == *old(self), self.buckets@ == before.buckets@.remove(code),
                self.hashes@ == if before.buckets@.contains_key(code) { before.hashes@ } else { before.hashes@.push(code) },
                original == if before.buckets@.contains_key(code) { before.buckets@[code]@ } else { Seq::empty() },
                code == bytes::hash_bytes(mode@),
                forall|offset: int| #![trigger bucket@[offset]]
                    0 <= offset < index ==> bucket@[offset].mode@ != mode@,
                found ==> index < bucket@.len() && bucket@[index as int].mode@ == mode@,
            decreases bucket@.len() - index, if found { 0int } else { 1int },
        {
            found = bytes::equal(&bucket[index].mode, mode);
            if !found { index += 1; }
        }
        let mut entry = if index < bucket.len() { bucket.remove(index) }
            else { CellMode { mode: Arc::new(bytes::copy(mode)), days: Sparse::new() } };
        let ghost previous = entry.days;
        proof {
            assert(entry.mode@ == mode@);
            assert(entry.days.wf());
            if index < original.len() {
                assert forall|date: i32| #![trigger before.at(mode@, date)]
                    before.at(mode@, date) == previous.at(date) by { before.lemma_index(code, index as int, date); }
            } else {
                reveal(Cells::at);
                assert forall|date: i32| #![trigger before.at(mode@, date)]
                    before.at(mode@, date) == previous.at(date) by {}
            }
        }
        let ok = entry.days.add(day, change);
        let ghost updated = entry.days;
        proof {
            assert forall|date: i32| #![trigger updated.at(date)] date != day implies updated.at(date) == previous.at(date) by {}
        }
        bucket.insert(index, entry);
        proof {
            assert forall|offset: int| #![trigger bucket@[offset]] 0 <= offset < bucket@.len() implies {
                &&& bucket@[offset].days.wf()
                &&& bytes::hash_bytes(bucket@[offset].mode@) == code
                &&& (offset != index ==> bucket@[offset] == original[offset])
            } by {}
            assert forall|left: int, right: int| #![trigger bucket@[left], bucket@[right]]
                0 <= left < right < bucket@.len() implies bucket@[left].mode@ != bucket@[right].mode@ by {
                if left == index {
                    assert(index < original.len());
                    assert(original[index as int].mode@ == mode@);
                    assert(original[right].mode@ != mode@);
                } else if right == index {
                    if index < original.len() { assert(original[index as int].mode@ == mode@); }
                    assert(original[left].mode@ != mode@);
                }
            }
        }
        self.buckets.insert(code, bucket);
        proof {
            self.hashes@.to_set_ensures();
            assert(self.hashes@.to_set() =~= self.buckets@.dom());
            assert(self.wf());
            assert forall|text: Seq<u8>, date: i32| #![trigger self.at(text, date)]
                (text != mode@ || date != day) implies self.at(text, date) == before.at(text, date) by {
                if text == mode@ {
                    self.lemma_index(code, index as int, date);
                    assert(self.at(text, date) == updated.at(date));
                    assert(updated.at(date) == previous.at(date));
                    assert(previous.at(date) == before.at(text, date));
                } else {
                    reveal(Cells::at);
                    if bytes::hash_bytes(text) != code {
                        assert(self.buckets@.contains_key(bytes::hash_bytes(text)) == before.buckets@.contains_key(bytes::hash_bytes(text)));
                        if self.buckets@.contains_key(bytes::hash_bytes(text)) {
                            assert(self.buckets@[bytes::hash_bytes(text)] == before.buckets@[bytes::hash_bytes(text)]);
                        }
                    }
                    if bytes::hash_bytes(text) == code {
                        if exists|offset: int| #![trigger original[offset]]
                            0 <= offset < original.len() && original[offset].mode@ == text {
                            let found = choose|offset: int| #![trigger original[offset]]
                                0 <= offset < original.len() && original[offset].mode@ == text;
                            assert(found != index);
                            assert(self.buckets@[code][found].days == before.buckets@[code][found].days);
                            before.lemma_index(code, found, date);
                            self.lemma_index(code, found, date);
                        } else {
                            assert forall|offset: int| #![trigger self.buckets@[code][offset]]
                                0 <= offset < self.buckets@[code].len() implies self.buckets@[code][offset].mode@ != text by {
                                if offset != index { assert(original[offset].mode@ != text); }
                            }
                        }
                    }
                }
            }
            self.lemma_index(code, index as int, day);
        }
        ok
    }

    pub open spec fn active_mode(self, mode: Seq<u8>) -> bool {
        exists|day: i32| #![trigger self.at(mode, day)] self.at(mode, day).high != 0 || self.at(mode, day).low != 0
    }

    pub fn has_nonzero(&self) -> (out: bool)
        requires self.wf(),
        ensures out == (exists|mode: Seq<u8>| #![trigger self.active_mode(mode)] self.active_mode(mode)),
    {
        let mut hash_index = 0;
        while hash_index < self.hashes.len()
            invariant hash_index <= self.hashes@.len(), self.wf(),
                forall|position: int, entry: int, date: i32| #![trigger self.buckets@[self.hashes@[position]][entry].days.at(date)]
                    0 <= position < hash_index && 0 <= entry < self.buckets@[self.hashes@[position]].len() ==>
                        self.buckets@[self.hashes@[position]][entry].days.at(date).high == 0
                            && self.buckets@[self.hashes@[position]][entry].days.at(date).low == 0,
            decreases self.hashes@.len() - hash_index,
        {
            let code = self.hashes[hash_index];
            match self.buckets.get(&code) {
                Some(bucket) => {
                    let mut mode_index = 0;
                    while mode_index < bucket.len()
                        invariant mode_index <= bucket@.len(), self.wf(),
                            self.buckets@.contains_key(code), bucket@ == self.buckets@[code]@,
                            forall|entry: int, date: i32| #![trigger bucket@[entry].days.at(date)]
                                0 <= entry < mode_index ==> bucket@[entry].days.at(date).high == 0
                                    && bucket@[entry].days.at(date).low == 0,
                        decreases bucket@.len() - mode_index,
                    {
                        let days = &bucket[mode_index].days;
                        let mut day_index = 0;
                        while day_index < days.keys.len()
                            invariant day_index <= days.keys@.len(), days.wf(), self.wf(),
                                self.buckets@.contains_key(code), bucket@ == self.buckets@[code]@,
                                0 <= mode_index < bucket@.len(), *days == bucket@[mode_index as int].days,
                                forall|offset: int| #![trigger days.keys@[offset]]
                                    0 <= offset < day_index ==> days.at(days.keys@[offset]).high == 0
                                        && days.at(days.keys@[offset]).low == 0,
                            decreases days.keys@.len() - day_index,
                        {
                            if !days.get(days.keys[day_index]).is_zero() {
                                proof {
                                    self.lemma_index(code, mode_index as int, days.keys@[day_index as int]);
                                    assert(self.active_mode(bucket@[mode_index as int].mode@));
                                    assert(exists|mode: Seq<u8>| #![trigger self.active_mode(mode)] self.active_mode(mode));
                                }
                                return true;
                            }
                            day_index += 1;
                        }
                        proof {
                            days.keys@.to_set_ensures();
                            assert forall|date: i32| #![trigger days.at(date)]
                                days.at(date).high == 0 && days.at(date).low == 0 by {
                                if days.values@.contains_key(date) {
                                    let offset = choose|offset: int| #![trigger days.keys@[offset]]
                                        0 <= offset < days.keys@.len() && days.keys@[offset] == date;
                                }
                            }
                        }
                        mode_index += 1;
                    }
                },
                None => {},
            }
            hash_index += 1;
        }
        proof {
            reveal(Cells::at);
            self.hashes@.to_set_ensures();
            assert forall|mode: Seq<u8>, day: i32| #![trigger self.at(mode, day)]
                self.at(mode, day).high == 0 && self.at(mode, day).low == 0 by {
                let code = bytes::hash_bytes(mode);
                if self.buckets@.contains_key(code) {
                    let position = choose|position: int| #![trigger self.hashes@[position]]
                        0 <= position < self.hashes@.len() && self.hashes@[position] == code;
                }
            }
        }
        false
    }

    pub fn add_support(&mut self, other: &Cells) -> (out: bool)
        requires old(self).wf(), other.wf(),
        ensures final(self).wf(),
            forall|mode: Seq<u8>, day: i32| #![trigger final(self).at(mode, day)]
                final(self).at(mode, day) == old(self).at(mode, day),
    {
        let ghost before = *self;
        let mut hash_index = 0;
        while hash_index < other.hashes.len()
            invariant hash_index <= other.hashes@.len(), self.wf(), other.wf(), before == *old(self),
                forall|mode: Seq<u8>, day: i32| #![trigger self.at(mode, day)] self.at(mode, day) == before.at(mode, day),
            decreases other.hashes@.len() - hash_index,
        {
            let code = other.hashes[hash_index];
            match other.buckets.get(&code) {
                Some(bucket) => {
                    let mut mode_index = 0;
                    while mode_index < bucket.len()
                        invariant mode_index <= bucket@.len(), self.wf(), other.wf(), before == *old(self),
                            forall|mode: Seq<u8>, day: i32| #![trigger self.at(mode, day)] self.at(mode, day) == before.at(mode, day),
                        decreases bucket@.len() - mode_index,
                    {
                        let entry = &bucket[mode_index];
                        let mut day_index = 0;
                        while day_index < entry.days.keys.len()
                            invariant day_index <= entry.days.keys@.len(), self.wf(), other.wf(), before == *old(self),
                                forall|mode: Seq<u8>, day: i32| #![trigger self.at(mode, day)] self.at(mode, day) == before.at(mode, day),
                            decreases entry.days.keys@.len() - day_index,
                        {
                            let ghost previous = *self;
                            let ok = self.add(&entry.mode, entry.days.keys[day_index], Counts::zero());
                            proof {
                                assert forall|mode: Seq<u8>, day: i32| #![trigger self.at(mode, day)]
                                    self.at(mode, day) == previous.at(mode, day) by {}
                            }
                            if !ok { return false; }
                            day_index += 1;
                        }
                        mode_index += 1;
                    }
                },
                None => {},
            }
            hash_index += 1;
        }
        true
    }
}

pub struct Lines {
    pub profiles: HashMap<i64, Cells>,
    pub keys: Vec<i64>,
}

impl Lines {
    pub open spec fn at(self, key: i64, mode: Seq<u8>, day: i32) -> int {
        if self.profiles@.contains_key(key) { self.profiles@[key].at(mode, day).high as int } else { 0 }
    }

    pub open spec fn wf(self) -> bool {
        &&& self.keys@.no_duplicates()
        &&& self.keys@.to_set() == self.profiles@.dom()
        &&& forall|key: i64| #![trigger self.profiles@[key]]
            self.profiles@.contains_key(key) ==> self.profiles@[key].wf()
        &&& forall|key: i64, mode: Seq<u8>, day: i32| #![trigger self.profiles@[key].at(mode, day)]
            self.profiles@.contains_key(key) ==> self.profiles@[key].at(mode, day).low == 0
    }

    pub fn new() -> (out: Self)
        ensures out.wf(), out.keys@.len() == 0,
    {
        let out = Lines { profiles: HashMap::new(), keys: Vec::new() };
        proof {
            out.keys@.to_set_ensures();
            assert(out.keys@.no_duplicates());
            assert(out.keys@.to_set() =~= out.profiles@.dom());
        }
        out
    }

    pub fn add(&mut self, key: i64, mode: &[u8], day: i32, change: i128) -> (out: bool)
        requires old(self).wf(),
        ensures final(self).wf(),
            out ==> final(self).at(key, mode@, day) == old(self).at(key, mode@, day) + change,
            forall|other: i64, text: Seq<u8>, date: i32| #![trigger final(self).at(other, text, date)]
                (other != key || text != mode@ || date != day) ==>
                    final(self).at(other, text, date) == old(self).at(other, text, date),
    {
        let ghost before = *self;
        proof { self.keys@.to_set_ensures(); }
        let mut cells = match self.profiles.remove(&key) {
            Some(cells) => cells,
            None => {
                proof { verus_prooflib::lemma_unique_push(self.keys@, key); }
                self.keys.push(key);
                Cells::new()
            },
        };
        let ok = cells.add(mode, day, Counts { high: change, low: 0 });
        self.profiles.insert(key, cells);
        proof {
            self.keys@.to_set_ensures();
            assert(self.keys@.to_set() =~= self.profiles@.dom());
            assert forall|other: i64, text: Seq<u8>, date: i32| #![trigger self.profiles@[other].at(text, date)]
                self.profiles@.contains_key(other) implies self.profiles@[other].at(text, date).low == 0 by {
                if other != key { assert(self.profiles@[other] == before.profiles@[other]); }
            }
        }
        ok
    }
}

pub open spec fn order_change(rows: Seq<OrderDelta>, key: i64, high: bool) -> int {
    sum(rows, |row: OrderDelta| if row.o_orderkey == key && (row.is_high != 0) == high {
        row.sign as int
    } else { 0 })
}

pub open spec fn line_change(rows: Seq<LineDelta>, key: i64, mode: Seq<u8>, day: i32) -> int {
    sum(rows, line_change_weight(key, mode, day))
}

pub open spec fn line_change_weight(key: i64, mode: Seq<u8>, day: i32) -> spec_fn(LineDelta) -> int {
    |row: LineDelta| if row.row.l_orderkey == key && row.row.l_shipmode@ == mode
        && row.row.l_receiptdate == day && row.row.l_shipdate < row.row.l_commitdate
        && row.row.l_commitdate < row.row.l_receiptdate { row.sign as int } else { 0 }
}

pub fn order_deltas(rows: &[OrderDelta]) -> (out: Option<Orders>)
    ensures out matches Some(value) ==> value.wf()
        && forall|key: i64| #![trigger value.at(key)]
            value.at(key).high == order_change(rows@, key, true)
                && value.at(key).low == order_change(rows@, key, false),
{
    let mut out = Orders::new();
    let mut index = 0;
    let mut processed = Ghost(0int);
    while index < rows.len()
        invariant index <= rows.len(), processed@ == index as int, out.wf(),
            forall|key: i64| #![trigger out.at(key)]
                out.at(key).high == order_change(rows@.take(processed@), key, true)
                    && out.at(key).low == order_change(rows@.take(processed@), key, false),
        decreases rows.len() - index,
    {
        let row = &rows[index];
        let change = if row.is_high != 0 { Counts { high: row.sign as i128, low: 0 } }
            else { Counts { high: 0, low: row.sign as i128 } };
        if !out.add(row.o_orderkey, change) { return None; }
        proof {
            assert forall|key: i64| #![trigger out.at(key)]
                out.at(key).high == order_change(rows@.take(index as int + 1), key, true)
                    && out.at(key).low == order_change(rows@.take(index as int + 1), key, false) by {
                verus_common::sums::lemma_sum_take(rows@, index as int, |row: OrderDelta|
                    if row.o_orderkey == key && (row.is_high != 0) == true { row.sign as int } else { 0 });
                verus_common::sums::lemma_sum_take(rows@, index as int, |row: OrderDelta|
                    if row.o_orderkey == key && (row.is_high != 0) == false { row.sign as int } else { 0 });
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    proof { assert(rows@.take(processed@) =~= rows@); }
    Some(out)
}

pub fn line_deltas(rows: &Vec<LineDelta>) -> (out: Option<Lines>)
    ensures out matches Some(value) ==> value.wf()
        && forall|key: i64, mode: Seq<u8>, day: i32| #![trigger value.at(key, mode, day)]
            value.at(key, mode, day) == line_change(rows@, key, mode, day),
{
    let mut out = Lines::new();
    let mut index = 0;
    let mut processed = Ghost(0int);
    while index < rows.len()
        invariant index <= rows@.len(), processed@ == index as int, out.wf(),
            forall|key: i64, mode: Seq<u8>, day: i32| #![trigger out.at(key, mode, day)]
                out.at(key, mode, day) == line_change(rows@.take(processed@), key, mode, day),
        decreases rows@.len() - index,
    {
        let delta = &rows[index];
        let row = &delta.row;
        if row.l_shipdate < row.l_commitdate && row.l_commitdate < row.l_receiptdate {
            if !out.add(row.l_orderkey, &row.l_shipmode, row.l_receiptdate, delta.sign as i128) { return None; }
        }
        proof {
            assert forall|key: i64, mode: Seq<u8>, day: i32| #![trigger out.at(key, mode, day)]
                out.at(key, mode, day) == line_change(rows@.take(index as int + 1), key, mode, day) by {
                verus_common::sums::lemma_sum_take(rows@, index as int, line_change_weight(key, mode, day));
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    proof { assert(rows@.take(processed@) =~= rows@); }
    Some(out)
}

pub fn affected(orders: &Orders, lines: &Lines) -> (out: Vec<i64>)
    requires orders.wf(), lines.wf(),
    ensures out@.no_duplicates(),
        forall|key: i64| #![trigger out@.contains(key)]
            out@.contains(key) <==> (orders.at(key).high != 0 || orders.at(key).low != 0
                || exists|mode: Seq<u8>, day: i32| #![trigger lines.at(key, mode, day)]
                    lines.at(key, mode, day) != 0),
{
    let mut out = Vec::new();
    let mut seen: HashMap<i64, bool> = HashMap::new();
    let mut index = 0;
    while index < orders.keys.len()
        invariant index <= orders.keys@.len(), orders.wf(), lines.wf(), out@.no_duplicates(),
            seen@.dom() == out@.to_set(),
            forall|key: i64| #![trigger out@.contains(key)]
                out@.contains(key) <==> orders.keys@.take(index as int).contains(key)
                    && (orders.at(key).high != 0 || orders.at(key).low != 0),
        decreases orders.keys@.len() - index,
    {
        let key = orders.keys[index];
        proof {
            verus_prooflib::lemma_prefix_fresh(orders.keys@, index as int);
            out@.to_set_ensures();
        }
        if !orders.get(key).is_zero() {
            proof { verus_prooflib::lemma_unique_push(out@, key); }
            out.push(key); seen.insert(key, true);
            proof { out@.to_set_ensures(); assert(seen@.dom() =~= out@.to_set()); }
        }
        index += 1;
    }
    proof {
        orders.keys@.to_set_ensures();
        assert(orders.keys@.take(index as int) =~= orders.keys@);
    }
    index = 0;
    while index < lines.keys.len()
        invariant index <= lines.keys@.len(), orders.wf(), lines.wf(), out@.no_duplicates(),
            seen@.dom() == out@.to_set(),
            forall|key: i64| #![trigger out@.contains(key)]
                out@.contains(key) <==> (orders.at(key).high != 0 || orders.at(key).low != 0
                    || lines.keys@.take(index as int).contains(key)
                        && (exists|mode: Seq<u8>, day: i32| #![trigger lines.at(key, mode, day)] lines.at(key, mode, day) != 0)),
        decreases lines.keys@.len() - index,
    {
        let key = lines.keys[index];
        if !seen.contains_key(&key) {
            proof { out@.to_set_ensures(); }
            match lines.profiles.get(&key) {
                Some(cells) => {
                    let active = cells.has_nonzero();
                    proof {
                        assert(*cells == lines.profiles@[key]);
                        assert forall|mode: Seq<u8>, day: i32| #![trigger cells.at(mode, day)]
                            cells.at(mode, day).low == 0 by {}
                        assert forall|mode: Seq<u8>, day: i32| #![trigger lines.at(key, mode, day)]
                            lines.at(key, mode, day) == cells.at(mode, day).high by {}
                        if active {
                            let text = choose|text: Seq<u8>| #![trigger cells.active_mode(text)] cells.active_mode(text);
                            let date = choose|date: i32| #![trigger cells.at(text, date)]
                                cells.at(text, date).high != 0 || cells.at(text, date).low != 0;
                            assert(lines.at(key, text, date) != 0);
                            assert(exists|mode: Seq<u8>, day: i32| #![trigger lines.at(key, mode, day)] lines.at(key, mode, day) != 0);
                        } else {
                            assert forall|mode: Seq<u8>, day: i32| #![trigger lines.at(key, mode, day)]
                                lines.at(key, mode, day) == 0 by { assert(!cells.active_mode(mode)); }
                        }
                        assert(active <==> exists|mode: Seq<u8>, day: i32| #![trigger lines.at(key, mode, day)]
                            lines.at(key, mode, day) != 0);
                    }
                    if active {
                        proof { verus_prooflib::lemma_unique_push(out@, key); }
                        out.push(key); seen.insert(key, true);
                        proof { out@.to_set_ensures(); assert(seen@.dom() =~= out@.to_set()); }
                    }
                },
                None => {},
            }
        }
        index += 1;
    }
    proof { lines.keys@.to_set_ensures(); assert(lines.keys@.take(index as int) =~= lines.keys@); }
    out
}

pub fn key_set(keys: &[i64]) -> (out: HashMap<i64, bool>)
    ensures out@.dom() == keys@.to_set(),
{
    let mut out = HashMap::new();
    let mut index = 0;
    while index < keys.len()
        invariant index <= keys.len(), out@.dom() == keys@.take(index as int).to_set(),
        decreases keys.len() - index,
    {
        out.insert(keys[index], true);
        proof {
            keys@.take(index as int + 1).to_set_ensures();
            keys@.take(index as int).to_set_ensures();
            assert(out@.dom() =~= keys@.take(index as int + 1).to_set());
        }
        index += 1;
    }
    proof { assert(keys@.take(index as int) =~= keys@); }
    out
}

}
