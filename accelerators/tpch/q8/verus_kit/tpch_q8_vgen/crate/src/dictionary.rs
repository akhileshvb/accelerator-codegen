use vstd::prelude::*;
use std::sync::Arc;
use crate::table::*;
use crate::num::Num;

verus! {

pub proof fn lemma_resolved_words(dictionary: Words, bytes: Seq<u8>, id: i64)
    requires dictionary.resolve_spec(bytes) == Some(id),
    ensures dictionary.known(id), dictionary.text(id) == bytes,
{
}

/// One column's texts: ids start at i64::MIN, so a column holds up to 2^64 - 1 of them.
pub struct Words {
    pub words: Arc<Vec<Arc<Vec<Arc<Vec<u8>>>>>>,
    pub index: Table,
    pub length: u64,
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

/// Where an id sits in the column: ids run from i64::MIN up.
pub open spec fn pos(id: i64) -> int {
    id as int + 9223372036854775808
}

impl Words {
    pub open spec fn known(self, id: i64) -> bool {
        pos(id) < self.length as int
    }

    pub open spec fn text_code(self, bytes: Seq<u8>) -> i64 {
        match self.resolve_spec(bytes) { Some(id) => id, None => (-1int) as i64 }
    }

    pub open spec fn resolve_spec(self, bytes: Seq<u8>) -> Option<i64> {
        if exists|id: i64| self.known(id) && #[trigger] self.text(id) == bytes {
            Some(choose|id: i64| self.known(id) && #[trigger] self.text(id) == bytes)
        } else { None }
    }

    pub open spec fn text(self, id: i64) -> Seq<u8> {
        (*(*(*self.words)@[pos(id) / 64])@[pos(id) % 64])@
    }

    #[verifier::opaque]
    pub open spec fn valid(self) -> bool {
        &&& self.layout()
        &&& forall|id: i64| self.known(id) ==> #[trigger] self.text(id).len() >= 0
        &&& forall|left: i64, right: i64| left < right && self.known(right) ==>
            #[trigger] self.text(left) != #[trigger] self.text(right)
        &&& self.index.valid()
        &&& forall|id: i64| self.known(id) ==>
            #[trigger] self.index.at((hash_bytes(self.text(id)) as i64, 0, 0), (id, 0, 0), false) == 1
        &&& forall|key: Major, offset: int| 0 <= offset < self.index.rows(key).len() ==>
            self.known((#[trigger] self.index.rows(key)[offset]).first)
            && key == (hash_bytes(self.text(self.index.rows(key)[offset].first)) as i64, 0, 0)
            && self.index.rows(key)[offset].second == 0 && self.index.rows(key)[offset].third == 0
            && self.index.rows(key)[offset].count@ == 1 && self.index.rows(key)[offset].volume@ == 0
    }

    pub open spec fn layout(self) -> bool {
        &&& (*self.words)@.len() <= usize::MAX as int
        &&& (*self.words)@.len() == (self.length as int + 63) / 64
        &&& forall|page: int| 0 <= page < (*self.words)@.len() ==>
            (*#[trigger] (*self.words)@[page])@.len() ==
                if page + 1 < (*self.words)@.len() { 64 } else { self.length as int - page * 64 }
    }

    pub fn new(size: usize) -> (result: Self)
        ensures result.valid(), result.length == 0,
    {
        proof { reveal(Words::valid); }
        Words { words: Arc::new(Vec::new()), index: Table::empty(size), length: 0 }
    }

    pub fn share(&self) -> (result: Self)
        ensures result == *self,
    {
        Words { words: self.words.clone(), index: self.index.share(), length: self.length }
    }

    pub fn resolve(&self, bytes: &[u8]) -> (result: Option<i64>)
        requires self.valid(),
        ensures result == self.resolve_spec(bytes@),
            result matches Some(id) ==> self.known(id) && self.text(id) == bytes@,
            result is None ==> forall|id: i64| self.known(id) ==> #[trigger] self.text(id) != bytes@,
    {
        proof { reveal(Words::valid); }
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
                reveal(Words::valid);
                assert(self.known(id));
                lemma_word_bounds(*self, id);
            }
            let slot = (id as i128 + 9223372036854775808i128) as u64;
            let page = (slot / 64) as usize;
            let offset = (slot % 64) as usize;
            proof { lemma_slot_split(*self, id, slot, page, offset); }
            let word = &self.words[page][offset];
            if equal_bytes(bytes, word.as_slice()) {
                proof { lemma_resolve_id(*self, bytes@, id); }
                return Some(id);
            }
            index += 1;
        }
        proof {
            assert forall|id: i64| self.known(id) implies #[trigger] self.text(id) != bytes@ by {
                if self.text(id) == bytes@ {
                    lemma_candidate_cover(*self, id, hash);
                    let offset = choose|offset: int| 0 <= offset < (*candidates)@.len() && (#[trigger] (*candidates)@[offset]).first == id;
                    assert(self.text((*candidates)@[offset].first) != bytes@);
                }
            }
        }
        None
    }

    pub fn intern(&mut self, bytes: &[u8]) -> (result: i64)
        requires old(self).valid(), old(self).length < u64::MAX,
        ensures final(self).valid() && final(self).known(result) && final(self).text(result) == bytes@
            && old(self).length <= final(self).length <= old(self).length + 1
            && forall|previous: i64| old(self).known(previous) ==>
                #[trigger] final(self).text(previous) == old(self).text(previous),
            words_extension(*old(self), *final(self)),
    {
        hide(hash_bytes);
        proof { reveal(Words::valid); }
        if let Some(id) = self.resolve(bytes) { return id; }
        let ghost before = *self;
        let slot = self.length;
        let id: i64 = (slot as i128 - 9223372036854775808i128) as i64;
        let length = slot + 1;
        proof { assert(pos(id) == slot as int); }
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
        if slot % 64 != 0 {
            let previous = &self.words[(slot / 64) as usize];
            offset = 0;
            while offset < previous.len()
                invariant offset <= previous.len(), page@ == (*previous)@.take(offset as int),
                decreases previous.len() - offset,
            { page.push(previous[offset].clone()); offset += 1; }
        }
        let ghost old_page = page@;
        page.push(Arc::new(word));
        if slot % 64 == 0 { pages.push(Arc::new(page)); }
        else { pages.set((slot / 64) as usize, Arc::new(page)); }
        let hash = byte_hash(bytes);
        let candidates = self.index.get((hash, 0, 0));
        let mut rows = copy_cells(&candidates);
        let cell = Cell { first: id, second: 0, third: 0, count: Num::Small(1), volume: Num::zero() };
        rows.push(cell);
        proof {
            reveal(rows_valid);
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
        let page_count = pages.len();
        proof { assert(page_count as int == pages@.len()); }
        self.words = Arc::new(pages);
        self.length = length;
        proof {
            assert(self.layout());
            assert(self.text(id) == bytes@);
            assert forall|previous: i64| before.known(previous) implies
                #[trigger] self.text(previous) == before.text(previous) by {
                lemma_word_bounds(before, previous);
                vstd::arithmetic::div_mod::lemma_fundamental_div_mod(previous as int, 64);
                vstd::arithmetic::div_mod::lemma_fundamental_div_mod(id as int, 64);
                if pos(previous) / 64 == pos(id) / 64 {
                    assert(pos(previous) % 64 < pos(id) % 64);
                    assert((*(*self.words)@[pos(previous) / 64])@[pos(previous) % 64]
                        == old_page[pos(previous) % 64]);
                }
            }
            assert forall|left: i64, right: i64| left < right && self.known(right) implies
                #[trigger] self.text(left) != #[trigger] self.text(right) by {
                if right == id { assert(before.text(left) != bytes@); }
            }
            assert forall|previous: i64| self.known(previous) implies
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
                self.known((#[trigger] self.index.rows(key)[offset]).first)
                && key == (hash_bytes(self.text(self.index.rows(key)[offset].first)) as i64, 0, 0)
                && self.index.rows(key)[offset].second == 0 && self.index.rows(key)[offset].third == 0
                && self.index.rows(key)[offset].count@ == 1 && self.index.rows(key)[offset].volume@ == 0 by {
                if key == (hash, 0, 0) && offset == before.index.rows(key).len() {
                    assert(self.index.rows(key)[offset] == cell);
                } else {
                    assert(self.index.rows(key)[offset] == before.index.rows(key)[offset]);
                }
            }
            assert(self.valid());
        }
        id
    }
}

pub open spec fn words_extension(before: Words, after: Words) -> bool {
    before.valid() && after.valid() && before.length <= after.length
        && forall|id: i64| before.known(id) ==> #[trigger] after.text(id) == before.text(id)
}

pub proof fn lemma_words_extension_transitive(before: Words, middle: Words, after: Words)
    requires words_extension(before, middle), words_extension(middle, after),
    ensures words_extension(before, after),
{
    assert forall|id: i64| before.known(id) implies #[trigger] after.text(id) == before.text(id) by {
        assert(after.text(id) == middle.text(id));
    }
}

proof fn lemma_append_bounds(dictionary: Words)
    requires dictionary.layout(), dictionary.length < u64::MAX,
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

pub proof fn lemma_known_words_extension(before: Words, after: Words, text: Seq<u8>)
    requires before.valid(), after.valid(), before.length <= after.length,
        forall|id: i64| before.known(id) ==> #[trigger] after.text(id) == before.text(id),
        exists|id: i64| before.known(id) && #[trigger] before.text(id) == text,
    ensures forall|id: i64| #[trigger] crate::model::words_match(before, id, text)
        == crate::model::words_match(after, id, text),
        exists|id: i64| after.known(id) && #[trigger] after.text(id) == text,
{
    hide(hash_bytes);
    reveal(Words::valid);
    let known = choose|id: i64| before.known(id) && #[trigger] before.text(id) == text;
    assert(after.known(known) && after.text(known) == text);
    assert forall|id: i64| #[trigger] crate::model::words_match(before, id, text)
        == crate::model::words_match(after, id, text) by {
        if !before.known(id) && after.known(id) { assert(after.text(known) != after.text(id)); }
    }
}

pub proof fn lemma_words_identifier(dictionary: Words, id: i64, text: Seq<u8>)
    requires dictionary.valid(), dictionary.known(id), dictionary.text(id) == text,
    ensures forall|other: i64| #[trigger] crate::model::words_match(dictionary, other, text) == (other == id),
{
    reveal(Words::valid);
    assert forall|other: i64| #[trigger] crate::model::words_match(dictionary, other, text) == (other == id) by {
        if other < id { assert(dictionary.text(other) != dictionary.text(id)); }
        if id < other && dictionary.known(other) { assert(dictionary.text(id) != dictionary.text(other)); }
    }
}

pub proof fn lemma_words_code(dictionary: Words, text: Seq<u8>)
    requires dictionary.valid(), crate::model::words_present(dictionary, text),
    ensures dictionary.known(dictionary.text_code(text)),
        dictionary.text(dictionary.text_code(text)) == text,
        forall|id: i64| #[trigger] crate::model::words_match(dictionary, id, text) == (id == dictionary.text_code(text)),
{
    let known = choose|id: i64| dictionary.known(id) && #[trigger] dictionary.text(id) == text;
    lemma_resolve_id(dictionary, text, known);
    lemma_words_identifier(dictionary, known, text);
}

proof fn lemma_word_bounds(dictionary: Words, id: i64)
    requires dictionary.layout(), dictionary.known(id),
    ensures pos(id) / 64 < (*dictionary.words)@.len(),
        pos(id) % 64 < (*(*dictionary.words)@[pos(id) / 64])@.len(),
{
    let page = pos(id) / 64;
    vstd::arithmetic::div_mod::lemma_fundamental_div_mod(id as int, 64);
    vstd::arithmetic::div_mod::lemma_fundamental_div_mod(dictionary.length as int + 63, 64);
    vstd::arithmetic::div_mod::lemma_mod_bound(id as int, 64);
    vstd::arithmetic::div_mod::lemma_mod_bound(dictionary.length as int + 63, 64);
    assert(page < (dictionary.length as int + 63) / 64);
    assert((*(*dictionary.words)@[page])@.len() ==
        if page + 1 < (*dictionary.words)@.len() { 64 } else { dictionary.length as int - page * 64 });
}

/// The page and offset of an id's word, as machine integers.
proof fn lemma_slot_split(dictionary: Words, id: i64, slot: u64, page: usize, offset: usize)
    requires dictionary.layout(), dictionary.known(id), slot as int == pos(id),
        page == (slot / 64) as usize, offset == (slot % 64) as usize,
    ensures page as int == pos(id) / 64, offset as int == pos(id) % 64,
        page < (*dictionary.words)@.len(), offset < (*(*dictionary.words)@[page as int])@.len(),
{
    lemma_word_bounds(dictionary, id);
    assert(pos(id) / 64 <= usize::MAX as int);
    assert(page as int == pos(id) / 64);
    assert(offset as int == pos(id) % 64);
}

proof fn lemma_resolve_id(dictionary: Words, bytes: Seq<u8>, id: i64)
    requires dictionary.valid(), dictionary.known(id), dictionary.text(id) == bytes,
    ensures dictionary.resolve_spec(bytes) == Some(id),
{
    hide(hash_bytes);
    reveal(Words::valid);
    let chosen = choose|chosen: i64| dictionary.known(chosen) && #[trigger] dictionary.text(chosen) == bytes;
    if chosen < id { assert(dictionary.text(chosen) != dictionary.text(id)); }
    if id < chosen { assert(dictionary.text(id) != dictionary.text(chosen)); }
}

proof fn lemma_candidate_cover(dictionary: Words, id: i64, hash: i64)
    requires dictionary.valid(), dictionary.known(id), hash as int == hash_bytes(dictionary.text(id)),
    ensures exists|offset: int| 0 <= offset < dictionary.index.rows((hash, 0, 0)).len()
        && (#[trigger] dictionary.index.rows((hash, 0, 0))[offset]).first == id,
{
    reveal(Words::valid);
    let rows = dictionary.index.rows((hash, 0, 0));
    assert(hash_bytes(dictionary.text(id)) as i64 == hash);
    assert(dictionary.index.at((hash, 0, 0), (id, 0, 0), false) == 1);
    if !(exists|offset: int| 0 <= offset < rows.len() && (#[trigger] rows[offset]).first == id) {
        reveal(Table::at);
        reveal(cell_sum);
        verus_common::sums::lemma_sum_zero(rows, |cell: Cell| cell_weight(cell, (id, 0, 0), false));
    }
}


/// The texts of the three columns, each with its own id space: part types, nation names, region names.
pub struct Dictionary {
    pub types: Words,
    pub names: Words,
    pub regions: Words,
}

impl Dictionary {
    pub open spec fn col(self, c: int) -> Words {
        if c == 0 { self.types } else if c == 1 { self.names } else { self.regions }
    }

    pub open spec fn valid(self) -> bool {
        self.types.valid() && self.names.valid() && self.regions.valid()
    }

    pub fn new(size: usize) -> (result: Self)
        ensures result.valid(), result.types.length == 0, result.names.length == 0, result.regions.length == 0,
    {
        Dictionary { types: Words::new(size), names: Words::new(size), regions: Words::new(size) }
    }

    pub fn share(&self) -> (result: Self)
        ensures result == *self,
    {
        Dictionary { types: self.types.share(), names: self.names.share(), regions: self.regions.share() }
    }
}

pub open spec fn extension(before: Dictionary, after: Dictionary) -> bool {
    words_extension(before.types, after.types) && words_extension(before.names, after.names) && words_extension(before.regions, after.regions)
}

pub proof fn lemma_extension_transitive(before: Dictionary, middle: Dictionary, after: Dictionary)
    requires extension(before, middle), extension(middle, after),
    ensures extension(before, after),
{
    lemma_words_extension_transitive(before.types, middle.types, after.types);
    lemma_words_extension_transitive(before.names, middle.names, after.names);
    lemma_words_extension_transitive(before.regions, middle.regions, after.regions);
}


pub proof fn lemma_known_text_extension(before: Dictionary, after: Dictionary, c: int, text: Seq<u8>)
    requires extension(before, after), crate::model::text_present(before, c, text),
    ensures forall|id: i64| #[trigger] crate::model::text_matches(before, c, id, text) == crate::model::text_matches(after, c, id, text),
        crate::model::text_present(after, c, text),
{
    lemma_known_words_extension(before.col(c), after.col(c), text);
}

pub proof fn lemma_text_identifier(dictionary: Dictionary, c: int, id: i64, text: Seq<u8>)
    requires dictionary.valid(), dictionary.col(c).known(id), dictionary.col(c).text(id) == text,
    ensures forall|other: i64| #[trigger] crate::model::text_matches(dictionary, c, other, text) == (other == id),
{
    lemma_words_identifier(dictionary.col(c), id, text);
}

pub proof fn lemma_text_code(dictionary: Dictionary, c: int, text: Seq<u8>)
    requires dictionary.valid(), crate::model::text_present(dictionary, c, text),
    ensures dictionary.col(c).known(dictionary.col(c).text_code(text)),
        dictionary.col(c).text(dictionary.col(c).text_code(text)) == text,
        forall|id: i64| #[trigger] crate::model::text_matches(dictionary, c, id, text) == (id == dictionary.col(c).text_code(text)),
{
    lemma_words_code(dictionary.col(c), text);
}

}
