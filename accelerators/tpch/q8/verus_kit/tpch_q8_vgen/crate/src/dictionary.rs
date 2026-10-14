use vstd::prelude::*;
use std::sync::Arc;
use crate::table::*;

verus! {

pub proof fn lemma_resolved_text(dictionary: Dictionary, bytes: Seq<u8>, id: i64)
    requires dictionary.resolve_spec(bytes) == Some(id),
    ensures 0 <= id < dictionary.length, dictionary.text(id) == bytes,
{
}

pub struct Dictionary {
    pub words: Arc<Vec<Arc<Vec<Arc<Vec<u8>>>>>>,
    pub index: Table,
    pub length: i64,
}

pub fn equal_bytes(left: &[u8], right: &[u8]) -> (result: bool)
    ensures result == (left@ == right@),
{
    if left.len() != right.len() { return false; }
    let mut index = 0;
    while index < left.len()
        invariant index <= left.len(), left.len() == right.len(),
            forall|offset: int| 0 <= offset < index ==> #[trigger] left@[offset] == right@[offset],
        decreases left.len() - index,
    {
        if left[index] != right[index] { return false; }
        index += 1;
    }
    proof { assert(left@ =~= right@); }
    true
}

pub fn byte_hash(bytes: &[u8]) -> (result: i64)
    ensures result as int == hash_bytes(bytes@),
{
    let mut hash: i128 = 0;
    let mut index = 0;
    while index < bytes.len()
        invariant 0 <= hash < 9223372036854775783i128,
            index <= bytes.len(), hash as int == hash_bytes(bytes@.take(index as int)),
        decreases bytes.len() - index,
    {
        let ghost prefix = bytes@.take(index as int);
        hash = (hash * 257 + bytes[index] as i128 + 1) % 9223372036854775783i128;
        proof {
            assert(bytes@.take(index as int + 1) =~= prefix.push(bytes@[index as int]));
            assert(prefix.push(bytes@[index as int]).drop_last() =~= prefix);
        }
        index += 1;
    }
    proof { assert(bytes@.take(bytes@.len() as int) =~= bytes@); }
    hash as i64
}

pub open spec fn hash_bytes(bytes: Seq<u8>) -> int
    decreases bytes.len(),
{
    if bytes.len() == 0 { 0 }
    else { (hash_bytes(bytes.drop_last()) * 257 + bytes.last() as int + 1) % 9223372036854775783 }
}

impl Dictionary {
    pub open spec fn text_code(self, bytes: Seq<u8>) -> i64 {
        match self.resolve_spec(bytes) { Some(id) => id, None => (-1int) as i64 }
    }

    pub open spec fn resolve_spec(self, bytes: Seq<u8>) -> Option<i64> {
        if exists|id: i64| 0 <= id < self.length && #[trigger] self.text(id) == bytes {
            Some(choose|id: i64| 0 <= id < self.length && #[trigger] self.text(id) == bytes)
        } else { None }
    }

    pub open spec fn text(self, id: i64) -> Seq<u8> {
        (*(*(*self.words)@[id as int / 64])@[id as int % 64])@
    }

    #[verifier::opaque]
    pub open spec fn valid(self) -> bool {
        &&& self.layout()
        &&& forall|id: i64| 0 <= id < self.length ==> #[trigger] self.text(id).len() >= 0
        &&& forall|left: i64, right: i64| 0 <= left < right < self.length ==>
            #[trigger] self.text(left) != #[trigger] self.text(right)
        &&& self.index.valid()
        &&& forall|id: i64| 0 <= id < self.length ==>
            #[trigger] self.index.at((hash_bytes(self.text(id)) as i64, 0, 0), (id, 0, 0), false) == 1
        &&& forall|key: Major, offset: int| 0 <= offset < self.index.rows(key).len() ==>
            0 <= (#[trigger] self.index.rows(key)[offset]).first < self.length
            && key == (hash_bytes(self.text(self.index.rows(key)[offset].first)) as i64, 0, 0)
            && self.index.rows(key)[offset].second == 0 && self.index.rows(key)[offset].third == 0
            && self.index.rows(key)[offset].count == 1 && self.index.rows(key)[offset].volume == 0
    }

    pub open spec fn layout(self) -> bool {
        &&& 0 <= self.length as int <= usize::MAX as int
        &&& (*self.words)@.len() == (self.length as int + 63) / 64
        &&& forall|page: int| 0 <= page < (*self.words)@.len() ==>
            (*#[trigger] (*self.words)@[page])@.len() ==
                if page + 1 < (*self.words)@.len() { 64 } else { self.length as int - page * 64 }
    }

    pub fn new(size: usize) -> (result: Self)
        ensures result.valid(),
    {
        proof { reveal(Dictionary::valid); }
        Dictionary { words: Arc::new(Vec::new()), index: Table::empty(size), length: 0 }
    }

    pub fn share(&self) -> (result: Self)
        ensures result == *self,
    {
        Dictionary { words: self.words.clone(), index: self.index.share(), length: self.length }
    }

    pub fn resolve(&self, bytes: &[u8]) -> (result: Option<i64>)
        requires self.valid(),
        ensures result == self.resolve_spec(bytes@),
            result matches Some(id) ==> 0 <= id < self.length && self.text(id) == bytes@,
            result is None ==> forall|id: i64| 0 <= id < self.length ==> #[trigger] self.text(id) != bytes@,
    {
        proof { reveal(Dictionary::valid); }
        let hash = byte_hash(bytes);
        let candidates = self.index.get((hash, 0, 0));
        let mut index = 0;
        while index < candidates.len()
            invariant self.valid(), index <= candidates.len(),
                (*candidates)@ == self.index.rows((hash, 0, 0)), hash as int == hash_bytes(bytes@),
                forall|offset: int| 0 <= offset < index ==>
                    #[trigger] self.text((*candidates)@[offset].first) != bytes@,
            decreases candidates.len() - index,
        {
            let id = candidates[index].first;
            proof {
                reveal(Dictionary::valid);
                assert(0 <= id < self.length);
                lemma_word_bounds(*self, id);
            }
            let word = &self.words[id as usize / 64][id as usize % 64];
            if equal_bytes(bytes, word.as_slice()) {
                proof { lemma_resolve_id(*self, bytes@, id); }
                return Some(id);
            }
            index += 1;
        }
        proof {
            assert forall|id: i64| 0 <= id < self.length implies #[trigger] self.text(id) != bytes@ by {
                if self.text(id) == bytes@ {
                    lemma_candidate_cover(*self, id, hash);
                    let offset = choose|offset: int| 0 <= offset < (*candidates)@.len() && (#[trigger] (*candidates)@[offset]).first == id;
                    assert(self.text((*candidates)@[offset].first) != bytes@);
                }
            }
        }
        None
    }

    pub fn intern(&mut self, bytes: &[u8]) -> (result: Option<i64>)
        requires old(self).valid(),
        ensures result matches Some(id) ==> final(self).valid() && 0 <= id < final(self).length && final(self).text(id) == bytes@
            && old(self).length <= final(self).length <= old(self).length + 1
            && forall|previous: i64| 0 <= previous < old(self).length ==>
                #[trigger] final(self).text(previous) == old(self).text(previous),
            result is Some ==> extension(*old(self), *final(self)),
    {
        hide(hash_bytes);
        proof { reveal(Dictionary::valid); }
        if let Some(id) = self.resolve(bytes) { return Some(id); }
        let ghost before = *self;
        let id = self.length;
        let length = id.checked_add(1)?;
        if length as u64 > usize::MAX as u64 { return None; }
        let mut word = Vec::new();
        let mut offset = 0;
        while offset < bytes.len()
            invariant offset <= bytes.len(), word@ == bytes@.take(offset as int),
            decreases bytes.len() - offset,
        { word.push(bytes[offset]); offset += 1; }
        let mut pages = Vec::new();
        offset = 0;
        while offset < self.words.len()
            invariant offset <= self.words.len(), pages@ == (*self.words)@.take(offset as int),
            decreases self.words.len() - offset,
        { pages.push(self.words[offset].clone()); offset += 1; }
        let mut page = Vec::new();
        proof { lemma_append_bounds(*self); }
        if id % 64 != 0 {
            let previous = &self.words[id as usize / 64];
            offset = 0;
            while offset < previous.len()
                invariant offset <= previous.len(), page@ == (*previous)@.take(offset as int),
                decreases previous.len() - offset,
            { page.push(previous[offset].clone()); offset += 1; }
        }
        let ghost old_page = page@;
        page.push(Arc::new(word));
        if id % 64 == 0 { pages.push(Arc::new(page)); }
        else { pages.set(id as usize / 64, Arc::new(page)); }
        let hash = byte_hash(bytes);
        let candidates = self.index.get((hash, 0, 0));
        let mut rows = copy_cells(&candidates);
        let cell = Cell { first: id, second: 0, third: 0, count: 1, volume: 0 };
        rows.push(cell);
        proof {
            assert forall|left: int, right: int| 0 <= left < right < rows@.len() implies
                minor_of(#[trigger] rows@[left]) != minor_of(#[trigger] rows@[right]) by {
                if right == (*candidates)@.len() {
                    assert(rows@[left] == (*candidates)@[left]);
                    assert(rows@[left].first < id);
                }
            }
            assert(rows_valid(rows@));
        }
        self.index = self.index.replace((hash, 0, 0), Arc::new(rows));
        self.words = Arc::new(pages);
        self.length = length;
        proof {
            assert(self.layout());
            assert(self.text(id) == bytes@);
            assert forall|previous: i64| 0 <= previous < before.length implies
                #[trigger] self.text(previous) == before.text(previous) by {
                lemma_word_bounds(before, previous);
                vstd::arithmetic::div_mod::lemma_fundamental_div_mod(previous as int, 64);
                vstd::arithmetic::div_mod::lemma_fundamental_div_mod(id as int, 64);
                if previous as int / 64 == id as int / 64 {
                    assert(previous as int % 64 < id as int % 64);
                    assert((*(*self.words)@[previous as int / 64])@[previous as int % 64]
                        == old_page[previous as int % 64]);
                }
            }
            assert forall|left: i64, right: i64| 0 <= left < right < self.length implies
                #[trigger] self.text(left) != #[trigger] self.text(right) by {
                if right == id { assert(before.text(left) != bytes@); }
            }
            assert forall|previous: i64| 0 <= previous < self.length implies
                #[trigger] self.index.at((hash_bytes(self.text(previous)) as i64, 0, 0), (previous, 0, 0), false) == 1 by {
                reveal(Table::at); reveal(cell_sum);
                let key = (hash_bytes(self.text(previous)) as i64, 0, 0);
                if previous < id {
                    assert(before.index.at((hash_bytes(before.text(previous)) as i64, 0, 0), (previous, 0, 0), false) == 1);
                    assert(before.text(previous) == self.text(previous));
                } else {
                    assert(self.text(previous) == bytes@);
                    assert(key == (hash, 0, 0));
                }
                if key == (hash, 0, 0) {
                    assert(self.index.rows(key) == before.index.rows(key).push(cell));
                    verus_common::sums::lemma_sum_push(before.index.rows(key), cell,
                        |entry: Cell| cell_weight(entry, (previous, 0, 0), false));
                    if previous == id {
                        assert forall|offset: int| 0 <= offset < before.index.rows(key).len() implies
                            cell_weight(#[trigger] before.index.rows(key)[offset], (previous, 0, 0), false) == 0 by {
                            assert(before.index.rows(key)[offset].first < id);
                        }
                        verus_common::sums::lemma_sum_zero(before.index.rows(key),
                            |entry: Cell| cell_weight(entry, (previous, 0, 0), false));
                    }
                }
            }
            assert forall|key: Major, offset: int| 0 <= offset < self.index.rows(key).len() implies
                0 <= (#[trigger] self.index.rows(key)[offset]).first < self.length
                && key == (hash_bytes(self.text(self.index.rows(key)[offset].first)) as i64, 0, 0)
                && self.index.rows(key)[offset].second == 0 && self.index.rows(key)[offset].third == 0
                && self.index.rows(key)[offset].count == 1 && self.index.rows(key)[offset].volume == 0 by {
                if key == (hash, 0, 0) && offset == before.index.rows(key).len() {
                    assert(self.index.rows(key)[offset] == cell);
                } else {
                    assert(self.index.rows(key)[offset] == before.index.rows(key)[offset]);
                }
            }
            assert(self.valid());
        }
        Some(id)
    }
}

pub open spec fn extension(before: Dictionary, after: Dictionary) -> bool {
    before.valid() && after.valid() && before.length <= after.length
        && forall|id: i64| 0 <= id < before.length ==> #[trigger] after.text(id) == before.text(id)
}

pub proof fn lemma_extension_transitive(before: Dictionary, middle: Dictionary, after: Dictionary)
    requires extension(before, middle), extension(middle, after),
    ensures extension(before, after),
{
    assert forall|id: i64| 0 <= id < before.length implies #[trigger] after.text(id) == before.text(id) by {
        assert(after.text(id) == middle.text(id));
    }
}

proof fn lemma_append_bounds(dictionary: Dictionary)
    requires dictionary.layout(), dictionary.length < i64::MAX,
    ensures
        dictionary.length as int / 64 <= (*dictionary.words)@.len(),
        dictionary.length as int % 64 == 0 ==> dictionary.length as int / 64 == (*dictionary.words)@.len(),
        dictionary.length as int % 64 != 0 ==> dictionary.length as int / 64 + 1 == (*dictionary.words)@.len()
            && (*(*dictionary.words)@[dictionary.length as int / 64])@.len() == dictionary.length as int % 64,
        (dictionary.length as int + 64) / 64 == (*dictionary.words)@.len() as int
            + if dictionary.length as int % 64 == 0 { 1int } else { 0int },
{
    let length = dictionary.length as int;
    vstd::arithmetic::div_mod::lemma_fundamental_div_mod(length, 64);
    vstd::arithmetic::div_mod::lemma_mod_bound(length, 64);
    vstd::arithmetic::div_mod::lemma_fundamental_div_mod(length + 63, 64);
    vstd::arithmetic::div_mod::lemma_mod_bound(length + 63, 64);
    vstd::arithmetic::div_mod::lemma_fundamental_div_mod(length + 64, 64);
    vstd::arithmetic::div_mod::lemma_mod_bound(length + 64, 64);
}

pub proof fn lemma_known_text_extension(before: Dictionary, after: Dictionary, text: Seq<u8>)
    requires before.valid(), after.valid(), before.length <= after.length,
        forall|id: i64| 0 <= id < before.length ==> #[trigger] after.text(id) == before.text(id),
        exists|id: i64| 0 <= id < before.length && #[trigger] before.text(id) == text,
    ensures forall|id: i64| #[trigger] crate::model::text_matches(before, id, text)
        == crate::model::text_matches(after, id, text),
        exists|id: i64| 0 <= id < after.length && #[trigger] after.text(id) == text,
{
    hide(hash_bytes);
    reveal(Dictionary::valid);
    let known = choose|id: i64| 0 <= id < before.length && #[trigger] before.text(id) == text;
    assert(0 <= known < after.length && after.text(known) == text);
    assert forall|id: i64| #[trigger] crate::model::text_matches(before, id, text)
        == crate::model::text_matches(after, id, text) by {
        if before.length <= id < after.length { assert(after.text(known) != after.text(id)); }
    }
}

pub proof fn lemma_text_identifier(dictionary: Dictionary, id: i64, text: Seq<u8>)
    requires dictionary.valid(), 0 <= id < dictionary.length, dictionary.text(id) == text,
    ensures forall|other: i64| #[trigger] crate::model::text_matches(dictionary, other, text) == (other == id),
{
    reveal(Dictionary::valid);
    assert forall|other: i64| #[trigger] crate::model::text_matches(dictionary, other, text) == (other == id) by {
        if 0 <= other < id { assert(dictionary.text(other) != dictionary.text(id)); }
        if id < other < dictionary.length { assert(dictionary.text(id) != dictionary.text(other)); }
    }
}

pub proof fn lemma_text_code(dictionary: Dictionary, text: Seq<u8>)
    requires dictionary.valid(), crate::model::text_present(dictionary, text),
    ensures 0 <= dictionary.text_code(text) < dictionary.length,
        dictionary.text(dictionary.text_code(text)) == text,
        forall|id: i64| #[trigger] crate::model::text_matches(dictionary, id, text) == (id == dictionary.text_code(text)),
{
    let known = choose|id: i64| 0 <= id < dictionary.length && #[trigger] dictionary.text(id) == text;
    lemma_resolve_id(dictionary, text, known);
    lemma_text_identifier(dictionary, known, text);
}

proof fn lemma_word_bounds(dictionary: Dictionary, id: i64)
    requires dictionary.layout(), 0 <= id < dictionary.length,
    ensures id as int / 64 < (*dictionary.words)@.len(),
        id as int % 64 < (*(*dictionary.words)@[id as int / 64])@.len(),
{
    let page = id as int / 64;
    vstd::arithmetic::div_mod::lemma_fundamental_div_mod(id as int, 64);
    vstd::arithmetic::div_mod::lemma_fundamental_div_mod(dictionary.length as int + 63, 64);
    vstd::arithmetic::div_mod::lemma_mod_bound(id as int, 64);
    vstd::arithmetic::div_mod::lemma_mod_bound(dictionary.length as int + 63, 64);
    assert(page < (dictionary.length as int + 63) / 64);
    assert((*(*dictionary.words)@[page])@.len() ==
        if page + 1 < (*dictionary.words)@.len() { 64 } else { dictionary.length as int - page * 64 });
}

proof fn lemma_resolve_id(dictionary: Dictionary, bytes: Seq<u8>, id: i64)
    requires dictionary.valid(), 0 <= id < dictionary.length, dictionary.text(id) == bytes,
    ensures dictionary.resolve_spec(bytes) == Some(id),
{
    hide(hash_bytes);
    reveal(Dictionary::valid);
    let chosen = choose|chosen: i64| 0 <= chosen < dictionary.length && #[trigger] dictionary.text(chosen) == bytes;
    if chosen < id { assert(dictionary.text(chosen) != dictionary.text(id)); }
    if id < chosen { assert(dictionary.text(id) != dictionary.text(chosen)); }
}

proof fn lemma_candidate_cover(dictionary: Dictionary, id: i64, hash: i64)
    requires dictionary.valid(), 0 <= id < dictionary.length, hash as int == hash_bytes(dictionary.text(id)),
    ensures exists|offset: int| 0 <= offset < dictionary.index.rows((hash, 0, 0)).len()
        && (#[trigger] dictionary.index.rows((hash, 0, 0))[offset]).first == id,
{
    reveal(Dictionary::valid);
    let rows = dictionary.index.rows((hash, 0, 0));
    assert(hash_bytes(dictionary.text(id)) as i64 == hash);
    assert(dictionary.index.at((hash, 0, 0), (id, 0, 0), false) == 1);
    if !(exists|offset: int| 0 <= offset < rows.len() && (#[trigger] rows[offset]).first == id) {
        reveal(Table::at);
        reveal(cell_sum);
        verus_common::sums::lemma_sum_zero(rows, |cell: Cell| cell_weight(cell, (id, 0, 0), false));
    }
}

}
