// Compact persistent ordered map: a prooflib `Index` directory of small sorted pages.
//
// Entries are `Copy` records ordered by `kv()` (a prooflib KeyView).  The directory maps a
// page's upper fence to the page; every entry of a page lies strictly above the previous fence
// and at or below its own fence, and the last fence is always `E::top()`, above every entry.
// A version is shared in O(1); an update copies one page and one directory path.
use vstd::prelude::*;
use std::sync::Arc;
use verus_prooflib::ordered::{Key, Index, Cursor, KeyView, key_lt, lemma_key_order, lemma_key_trans};

verus! {

pub trait Ent: Sized {
    fn dup(&self) -> (result: Self) ensures result == *self;
    spec fn kv(self) -> KeyView;
    spec fn top() -> KeyView;
    fn key(&self) -> (result: Key) ensures result.view() == self.kv();
    fn top_key() -> (result: Key) ensures result.view() == Self::top();
    fn before(&self, other: &Self) -> (result: bool) ensures result == key_lt(self.kv(), other.kv());
    fn same_key(&self, other: &Self) -> (result: bool) ensures result == (self.kv() == other.kv());
    proof fn top_above(entry: Self) ensures key_lt(entry.kv(), Self::top());
}

pub open spec fn page_sorted<E: Ent>(page: Seq<E>) -> bool {
    forall|first: int, last: int| #![trigger page[first], page[last]]
        0 <= first < last < page.len() ==> key_lt(page[first].kv(), page[last].kv())
}

pub open spec fn cap() -> int { 128 }

pub struct Paged<E: Ent> {
    pub dir: Index<Vec<E>>,
}

impl<E: Ent> Paged<E> {
    pub open spec fn page_at(self, fence: KeyView) -> Seq<E> {
        match self.dir.at(fence) { Some(page) => page@, None => Seq::empty() }
    }

    /// Entry `entry` is stored on the page with fence `fence`.
    pub open spec fn stored(self, fence: KeyView, entry: E) -> bool {
        self.page_at(fence).contains(entry)
    }

    pub open spec fn holds(self, entry: E) -> bool {
        exists|fence: KeyView| self.stored(fence, entry)
    }

    pub open spec fn sorted_pages(self) -> bool {
        forall|fence: KeyView| #![trigger self.page_at(fence)] page_sorted(self.page_at(fence))
    }

    pub open spec fn small_pages(self) -> bool {
        forall|fence: KeyView| #![trigger self.page_at(fence)] self.page_at(fence).len() <= cap()
    }

    /// Every entry is at or below its page fence.
    pub open spec fn bounded_pages(self) -> bool {
        forall|fence: KeyView, index: int| #![trigger self.page_at(fence)[index]]
            0 <= index < self.page_at(fence).len() ==> !key_lt(fence, self.page_at(fence)[index].kv())
    }

    /// Every entry is above every smaller fence.
    pub open spec fn separated_pages(self) -> bool {
        forall|fence: KeyView, other: KeyView, index: int| #![trigger self.page_at(fence)[index], self.dir.at(other)]
            self.dir.at(other).is_some() && key_lt(other, fence) && 0 <= index < self.page_at(fence).len()
            ==> key_lt(other, self.page_at(fence)[index].kv())
    }

    pub open spec fn top_fence(self) -> bool {
        &&& forall|fence: KeyView| #![trigger self.dir.at(fence)] self.dir.at(fence).is_some() ==> !key_lt(E::top(), fence)
        &&& (self.dir.entries().len() > 0 ==> self.dir.at(E::top()).is_some())
    }

    /// Only the last page may be empty.
    pub open spec fn full_pages(self) -> bool {
        forall|fence: KeyView| #![trigger self.dir.at(fence)]
            self.dir.at(fence).is_some() && fence != E::top() ==> self.page_at(fence).len() > 0
    }

    pub open spec fn wf(self) -> bool {
        &&& self.dir.wf()
        &&& self.sorted_pages() && self.small_pages() && self.bounded_pages() && self.separated_pages()
        &&& self.top_fence() && self.full_pages()
    }

    pub open spec fn has_key(self, key: KeyView) -> bool {
        exists|entry: E| self.holds(entry) && entry.kv() == key
    }

    pub open spec fn pick(self, key: KeyView) -> E {
        choose|entry: E| self.holds(entry) && entry.kv() == key
    }

    pub open spec fn at(self, key: KeyView) -> Option<E> {
        if self.has_key(key) { Some(self.pick(key)) } else { None }
    }

    pub open spec fn nonempty(self) -> bool {
        exists|key: KeyView| #![trigger self.at(key)] self.at(key).is_some()
    }
}

pub proof fn lemma_same_key_same_page<E: Ent>(paged: Paged<E>, first_fence: KeyView, second_fence: KeyView, first: E, second: E)
    requires paged.wf(), paged.stored(first_fence, first), paged.stored(second_fence, second),
        first.kv() == second.kv(),
    ensures first_fence == second_fence, first == second,
{
    let first_page = paged.dir.at(first_fence).unwrap()@;
    let second_page = paged.dir.at(second_fence).unwrap()@;
    let i = choose|i: int| 0 <= i < first_page.len() && first_page[i] == first;
    let j = choose|j: int| 0 <= j < second_page.len() && second_page[j] == second;
    lemma_key_order(first_fence, second_fence);
    if first_fence != second_fence {
        if key_lt(first_fence, second_fence) {
            assert(key_lt(first_fence, second_page[j].kv()));
            assert(!key_lt(first_fence, first_page[i].kv()));
        } else {
            assert(key_lt(second_fence, second_fence) || key_lt(second_fence, first_fence));
            assert(key_lt(second_fence, first_page[i].kv()));
            assert(!key_lt(second_fence, second_page[j].kv()));
        }
    }
    if first_fence == second_fence && i != j {
        if i < j { assert(key_lt(first_page[i].kv(), first_page[j].kv())); }
        else { assert(key_lt(first_page[j].kv(), first_page[i].kv())); }
        lemma_key_order(first_page[i].kv(), first_page[j].kv());
    }
}

pub proof fn lemma_unique<E: Ent>(paged: Paged<E>, first: E, second: E)
    requires paged.wf(), paged.holds(first), paged.holds(second), first.kv() == second.kv(),
    ensures first == second,
{
    let a = choose|fence: KeyView| paged.stored(fence, first);
    let b = choose|fence: KeyView| paged.stored(fence, second);
    lemma_same_key_same_page(paged, a, b, first, second);
}

pub proof fn lemma_at<E: Ent>(paged: Paged<E>, key: KeyView, entry: E)
    requires paged.wf(),
    ensures paged.at(key) == Some(entry) <==> (paged.holds(entry) && entry.kv() == key),
        paged.at(key).is_some() ==> paged.holds(paged.at(key).unwrap()) && paged.at(key).unwrap().kv() == key,
{
    if paged.holds(entry) && entry.kv() == key {
        assert(paged.has_key(key));
        lemma_unique(paged, paged.pick(key), entry);
    }
}

/// Index of the first entry of a sorted page that is not below the probe.
pub fn search<E: Ent>(page: &Vec<E>, probe: &E) -> (position: usize)
    requires page_sorted(page@),
    ensures position <= page.len(),
        forall|index: int| #![trigger page@[index]] 0 <= index < position ==> key_lt(page@[index].kv(), probe.kv()),
        forall|index: int| #![trigger page@[index]] position <= index < page.len() ==> !key_lt(page@[index].kv(), probe.kv()),
{
    let mut low = 0usize;
    let mut high = page.len();
    while low < high
        invariant 0 <= low <= high <= page.len(), page_sorted(page@),
            forall|index: int| #![trigger page@[index]] 0 <= index < low ==> key_lt(page@[index].kv(), probe.kv()),
            forall|index: int| #![trigger page@[index]] high <= index < page.len() ==> !key_lt(page@[index].kv(), probe.kv()),
        decreases high - low,
    {
        let middle = low + (high - low) / 2;
        if page[middle].before(probe) {
            proof {
                assert forall|index: int| #![trigger page@[index]] 0 <= index < middle + 1 implies key_lt(page@[index].kv(), probe.kv()) by {
                    if index >= low as int && index < middle as int {
                        lemma_key_trans(page@[index].kv(), page@[middle as int].kv(), probe.kv());
                    }
                }
            }
            low = middle + 1;
        } else {
            proof {
                assert forall|index: int| #![trigger page@[index]] middle <= index < page.len() implies !key_lt(page@[index].kv(), probe.kv()) by {
                    if index > middle as int && key_lt(page@[index].kv(), probe.kv()) {
                        lemma_key_trans(page@[middle as int].kv(), page@[index].kv(), probe.kv());
                    }
                }
            }
            high = middle;
        }
    }
    low
}

pub proof fn lemma_least_fence<E: Ent>(paged: Paged<E>, probe: KeyView, position: int)
    requires paged.dir.wf(), 0 <= position < paged.dir.entries().len(),
        position == verus_prooflib::ordered::lower_position(paged.dir.entries(), probe),
    ensures
        !key_lt(paged.dir.entries()[position].0, probe),
        forall|other: KeyView| #![trigger paged.dir.at(other)]
            paged.dir.at(other).is_some() && key_lt(other, paged.dir.entries()[position].0) ==> key_lt(other, probe),
{
    paged.dir.model();
    let rows = paged.dir.entries();
    verus_prooflib::ordered::lemma_lower_suffix(rows, probe);
    assert forall|other: KeyView| #![trigger paged.dir.at(other)]
        paged.dir.at(other).is_some() && key_lt(other, rows[position].0) implies key_lt(other, probe) by {
        let at = choose|at: int| #![auto] 0 <= at < rows.len() && rows[at].0 == other;
        if at >= position {
            if at == position { lemma_key_order(other, other); }
            else {
                assert(key_lt(rows[position].0, rows[at].0));
                lemma_key_order(other, rows[position].0);
            }
        }
    }
}

impl<E: Ent> Paged<E> {
    /// The page whose range holds the probe, if any entry equal to it exists.
    pub fn locate(&self, probe: &E) -> (result: (Key, Arc<Vec<E>>))
        requires self.wf(), self.dir.entries().len() > 0,
        ensures self.dir.at(result.0.view()) == Some(*result.1),
            page_sorted(result.1@), result.1@.len() <= cap(),
            !key_lt(result.0.view(), probe.kv()),
            forall|other: KeyView| #![trigger self.dir.at(other)]
                self.dir.at(other).is_some() && key_lt(other, result.0.view()) ==> key_lt(other, probe.kv()),
    {
        proof { self.dir.model(); }
        let key = probe.key();
        let cursor_start = self.dir.cursor_from(&key);
        let ghost rows = self.dir.entries();
        let ghost position = verus_prooflib::ordered::lower_position(rows, key.view());
        proof {
            verus_prooflib::ordered::lemma_lower_suffix(rows, key.view());
            E::top_above(*probe);
            lemma_key_order(E::top(), key.view());
            let top_at = choose|at: int| #![auto] 0 <= at < rows.len() && rows[at].0 == E::top();
            if position == rows.len() { assert(key_lt(rows[top_at].0, key.view())); }
        }
        let mut cursor = cursor_start;
        match cursor.next() {
            Some((fence, page)) => {
                proof {
                    lemma_least_fence(*self, key.view(), position);
                    assert(self.dir.at(fence.view()) == Some(*page));
                    assert(self.page_at(fence.view()) == page@);
                }
                (fence, page)
            },
            None => {
                proof { assert(false); }
                (key, Arc::new(Vec::new()))
            },
        }
    }

    pub fn get(&self, probe: &E) -> (result: Option<E>)
        requires self.wf(),
        ensures result == self.at(probe.kv()),
    {
        if self.dir.is_empty() {
            proof { lemma_empty_dir(*self); }
            return None;
        }
        let (fence, page) = self.locate(probe);
        let position = search(&*page, probe);
        proof { lemma_key_order(probe.kv(), probe.kv()); }
        if position < page.len() && !probe.before(&page[position]) {
            proof {
                lemma_key_order(page@[position as int].kv(), probe.kv());
                assert(self.stored(fence.view(), page@[position as int]));
                lemma_at(*self, probe.kv(), page@[position as int]);
            }
            Some(page[position].dup())
        } else {
            proof { lemma_not_found(*self, fence.view(), page@, position as int, probe.kv()); }
            None
        }
    }
}

pub proof fn lemma_empty_dir<E: Ent>(paged: Paged<E>)
    requires paged.dir.wf(), paged.dir.entries().len() == 0,
    ensures forall|entry: E| !paged.holds(entry), forall|key: KeyView| paged.at(key).is_none(),
{
    paged.dir.model();
    assert forall|entry: E| !paged.holds(entry) by {
        if paged.holds(entry) {
            let fence = choose|fence: KeyView| paged.stored(fence, entry);
            assert(paged.dir.at(fence).is_some());
        }
    }
    assert forall|key: KeyView| paged.at(key).is_none() by { assert(!paged.has_key(key)); }
}

/// No entry has the probe's key when the probe's page has none.
pub proof fn lemma_not_found<E: Ent>(paged: Paged<E>, fence: KeyView, page: Seq<E>, position: int, probe: KeyView)
    requires paged.wf(), paged.dir.at(fence).is_some(), paged.page_at(fence) == page, page_sorted(page),
        !key_lt(fence, probe),
        forall|other: KeyView| #![trigger paged.dir.at(other)]
            paged.dir.at(other).is_some() && key_lt(other, fence) ==> key_lt(other, probe),
        0 <= position <= page.len(),
        forall|index: int| #![trigger page[index]] 0 <= index < position ==> key_lt(page[index].kv(), probe),
        forall|index: int| #![trigger page[index]] position <= index < page.len() ==> !key_lt(page[index].kv(), probe),
        !(position < page.len() && !key_lt(probe, page[position].kv())),
    ensures !paged.has_key(probe),
{
    if paged.has_key(probe) {
        let found = paged.pick(probe);
        let other = choose|other: KeyView| paged.stored(other, found);
        if other != fence {
            lemma_other_page(paged, fence, probe, other, found);
        } else {
            let index = choose|index: int| 0 <= index < page.len() && page[index] == found;
            if index < position { assert(key_lt(page[index].kv(), probe)); lemma_key_order(page[index].kv(), probe); }
            else {
                assert(!key_lt(page[index].kv(), probe));
                lemma_key_order(page[index].kv(), probe);
                if index == position { assert(!key_lt(probe, page[position].kv())); }
                else {
                    assert(key_lt(page[position].kv(), page[index].kv()));
                    lemma_key_order(page[position].kv(), probe);
                    if key_lt(page[position].kv(), probe) {}
                    assert(!key_lt(page[position].kv(), probe));
                    assert(key_lt(probe, page[position].kv()) || page[position].kv() == probe);
                    if page[position].kv() == probe { assert(!key_lt(probe, page[position].kv())); }
                    lemma_key_trans(page[position].kv(), page[index].kv(), probe);
                }
            }
        }
        assert(false);
    }
}

// ---- page construction -------------------------------------------------------------------

/// `page` with the entry at `position` (when `skip` is 1) or none (when 0) replaced by `entry` at `position`.
pub fn page_with<E: Ent>(page: &Vec<E>, position: usize, skip: usize, entry: E) -> (result: Vec<E>)
    requires position + skip <= page.len(), skip <= 1, page.len() < 0x1000_0000,
    ensures result@ == page@.take(position as int) + seq![entry] + page@.skip(position as int + skip as int),
{
    let mut result = Vec::with_capacity(page.len() + 1 - skip);
    let mut index = 0usize;
    while index < position
        invariant index <= position, position + skip <= page.len(), result@ == page@.take(index as int),
        decreases position - index,
    {
        result.push(page[index].dup());
        proof { assert(page@.take(index as int + 1) =~= page@.take(index as int).push(page@[index as int])); }
        index += 1;
    }
    result.push(entry);
    let ghost head = result@;
    index = position + skip;
    while index < page.len()
        invariant position + skip <= index <= page.len(), skip <= 1,
            result@ == page@.take(position as int) + seq![entry] + page@.subrange(position as int + skip as int, index as int),
        decreases page.len() - index,
    {
        result.push(page[index].dup());
        proof {
            assert(page@.subrange(position as int + skip as int, index as int + 1)
                =~= page@.subrange(position as int + skip as int, index as int).push(page@[index as int]));
            assert(result@ =~= page@.take(position as int) + seq![entry]
                + page@.subrange(position as int + skip as int, index as int + 1));
        }
        index += 1;
    }
    proof { assert(page@.subrange(position as int + skip as int, page.len() as int) =~= page@.skip(position as int + skip as int)); }
    result
}

pub fn page_without<E: Ent>(page: &Vec<E>, position: usize) -> (result: Vec<E>)
    requires position < page.len(),
    ensures result@ == page@.take(position as int) + page@.skip(position as int + 1),
{
    let mut result = Vec::with_capacity(page.len() - 1);
    let mut index = 0usize;
    while index < position
        invariant index <= position, position < page.len(), result@ == page@.take(index as int),
        decreases position - index,
    {
        result.push(page[index].dup());
        proof { assert(page@.take(index as int + 1) =~= page@.take(index as int).push(page@[index as int])); }
        index += 1;
    }
    index = position + 1;
    while index < page.len()
        invariant position + 1 <= index <= page.len(),
            result@ == page@.take(position as int) + page@.subrange(position as int + 1, index as int),
        decreases page.len() - index,
    {
        result.push(page[index].dup());
        proof {
            assert(page@.subrange(position as int + 1, index as int + 1)
                =~= page@.subrange(position as int + 1, index as int).push(page@[index as int]));
            assert(result@ =~= page@.take(position as int) + page@.subrange(position as int + 1, index as int + 1));
        }
        index += 1;
    }
    proof { assert(page@.subrange(position as int + 1, page.len() as int) =~= page@.skip(position as int + 1)); }
    result
}

pub fn page_part<E: Ent>(page: &Vec<E>, start: usize, end: usize) -> (result: Vec<E>)
    requires start <= end <= page.len(),
    ensures result@ == page@.subrange(start as int, end as int),
{
    let mut result = Vec::with_capacity(end - start);
    let mut index = start;
    while index < end
        invariant start <= index <= end <= page.len(), result@ == page@.subrange(start as int, index as int),
        decreases end - index,
    {
        result.push(page[index].dup());
        proof { assert(page@.subrange(start as int, index as int + 1) =~= page@.subrange(start as int, index as int).push(page@[index as int])); }
        index += 1;
    }
    result
}

// ---- page lemmas -------------------------------------------------------------------------

pub proof fn lemma_splice<E: Ent>(page: Seq<E>, position: int, skip: int, entry: E)
    requires page_sorted(page), 0 <= position <= page.len(), skip == 0 || skip == 1, position + skip <= page.len(),
        forall|index: int| #![trigger page[index]] 0 <= index < position ==> key_lt(page[index].kv(), entry.kv()),
        forall|index: int| #![trigger page[index]] position <= index < page.len() ==> !key_lt(page[index].kv(), entry.kv()),
        skip == 1 ==> page[position].kv() == entry.kv(),
        skip == 0 ==> (position == page.len() || page[position].kv() != entry.kv()),
    ensures
        page_sorted(page.take(position) + seq![entry] + page.skip(position + skip)),
        forall|other: E| #![trigger (page.take(position) + seq![entry] + page.skip(position + skip)).contains(other)]
            (page.take(position) + seq![entry] + page.skip(position + skip)).contains(other)
            <==> (other == entry || (page.contains(other) && other.kv() != entry.kv())),
{
    let head = page.take(position);
    let tail = page.skip(position + skip);
    let result = head + seq![entry] + tail;
    assert forall|index: int| #![trigger tail[index]] 0 <= index < tail.len() implies key_lt(entry.kv(), tail[index].kv()) by {
        assert(tail[index] == page[position + skip + index]);
        if skip == 1 {
            assert(key_lt(page[position].kv(), page[position + 1 + index].kv()));
        } else {
            lemma_key_order(page[position + index].kv(), entry.kv());
            if index > 0 { assert(key_lt(page[position].kv(), page[position + index].kv())); }
            assert(page[position].kv() != entry.kv());
        }
    }
    assert forall|first: int, last: int| #![trigger result[first], result[last]]
        0 <= first < last < result.len() implies key_lt(result[first].kv(), result[last].kv()) by {
        let h = head.len() as int;
        if last < h { assert(result[first] == page[first]); assert(result[last] == page[last]); }
        else if last == h {
            assert(result[first] == page[first]);
            assert(result[last] == entry);
        } else if first < h {
            assert(result[first] == page[first]);
            assert(result[last] == tail[last - h - 1]);
            lemma_key_trans(page[first].kv(), entry.kv(), tail[last - h - 1].kv());
        } else if first == h {
            assert(result[first] == entry);
            assert(result[last] == tail[last - h - 1]);
        } else {
            assert(result[first] == tail[first - h - 1]);
            assert(result[last] == tail[last - h - 1]);
            assert(key_lt(page[position + skip + (first - h - 1)].kv(), page[position + skip + (last - h - 1)].kv()));
        }
    }
    assert forall|other: E| #![trigger result.contains(other)]
        result.contains(other) <==> (other == entry || (page.contains(other) && other.kv() != entry.kv())) by {
        if result.contains(other) {
            let at = choose|at: int| 0 <= at < result.len() && result[at] == other;
            if at < head.len() {
                assert(page[at] == other);
                assert(page.contains(other));
                assert(key_lt(page[at].kv(), entry.kv()));
                lemma_key_order(other.kv(), entry.kv());
            } else if at > head.len() {
                let t = at - head.len() - 1;
                assert(page[position + skip + t] == other);
                assert(page.contains(other));
                assert(key_lt(entry.kv(), other.kv()));
                lemma_key_order(other.kv(), entry.kv());
            }
        }
        if other == entry { assert(result[position] == entry); }
        if page.contains(other) && other.kv() != entry.kv() {
            let at = choose|at: int| 0 <= at < page.len() && page[at] == other;
            if at < position { assert(result[at] == other); }
            else {
                assert(!key_lt(page[at].kv(), entry.kv()));
                lemma_key_order(page[at].kv(), entry.kv());
                assert(at >= position + skip) by {
                    if skip == 1 && at == position { assert(page[at].kv() == entry.kv()); }
                    if skip == 1 && at < position { }
                };
                assert(result[at + 1 - skip] == other);
            }
        }
    }
}

// ---- updates -----------------------------------------------------------------------------

/// Entry `x` on a page other than the probe's page differs from the probe in key.
pub proof fn lemma_other_page<E: Ent>(paged: Paged<E>, fence: KeyView, probe: KeyView, other: KeyView, x: E)
    requires paged.wf(), paged.dir.at(fence).is_some(),
        !key_lt(fence, probe),
        forall|smaller: KeyView| #![trigger paged.dir.at(smaller)]
            paged.dir.at(smaller).is_some() && key_lt(smaller, fence) ==> key_lt(smaller, probe),
        paged.stored(other, x), other != fence,
    ensures x.kv() != probe,
{
    let index = choose|index: int| 0 <= index < paged.page_at(other).len() && paged.page_at(other)[index] == x;
    assert(paged.dir.at(other).is_some()) by {
        assert(paged.page_at(other).len() > 0);
    }
    assert(!key_lt(other, x.kv()));
    lemma_key_order(other, fence);
    if key_lt(other, fence) {
        assert(key_lt(other, probe));
        if x.kv() == probe { assert(key_lt(other, x.kv())); }
    } else {
        assert(key_lt(fence, other));
        assert(key_lt(fence, x.kv()));
        if x.kv() == probe { assert(key_lt(fence, probe)); }
    }
}

pub proof fn lemma_at_update<E: Ent>(old: Paged<E>, new: Paged<E>, key: KeyView, inserted: Option<E>)
    requires old.wf(), new.wf(),
        forall|entry: E| #![trigger new.holds(entry)] new.holds(entry) <==> (inserted == Some(entry) || (old.holds(entry) && entry.kv() != key)),
        inserted matches Some(entry) ==> entry.kv() == key,
    ensures forall|query: KeyView| #![trigger new.at(query)] new.at(query) == if query == key { inserted } else { old.at(query) },
{
    assert forall|query: KeyView| #![trigger new.at(query)] new.at(query) == if query == key { inserted } else { old.at(query) } by {
        if query == key {
            match inserted {
                Some(entry) => { assert(new.holds(entry)); lemma_at(new, query, entry); },
                None => {
                    if new.has_key(query) {
                        let found = new.pick(query);
                        assert(new.holds(found) && found.kv() == key);
                    }
                },
            }
        } else {
            match old.at(query) {
                Some(entry) => {
                    lemma_at(old, query, entry);
                    assert(new.holds(entry));
                    lemma_at(new, query, entry);
                },
                None => {
                    if new.has_key(query) {
                        let found = new.pick(query);
                        assert(new.holds(found) && found.kv() == query);
                        assert(old.holds(found) || inserted == Some(found));
                        assert(old.has_key(query));
                    }
                },
            }
        }
    }
}

/// Replace the page of an existing fence.
pub proof fn lemma_replace_page<E: Ent>(old: Paged<E>, new: Paged<E>, fence: KeyView, page: Vec<E>)
    requires old.wf(), old.dir.at(fence).is_some(), new.dir.wf(),
        forall|query: KeyView| #![trigger new.dir.at(query)] new.dir.at(query) == if query == fence { Some(page) } else { old.dir.at(query) },
        page_sorted(page@), page@.len() <= cap(),
        forall|index: int| #![trigger page@[index]] 0 <= index < page@.len() ==> !key_lt(fence, page@[index].kv()),
        forall|index: int, other: KeyView| #![trigger page@[index], old.dir.at(other)]
            0 <= index < page@.len() && old.dir.at(other).is_some() && key_lt(other, fence) ==> key_lt(other, page@[index].kv()),
        fence != E::top() ==> page@.len() > 0,
    ensures new.wf(),
{
    assert forall|query: KeyView| new.dir.at(query).is_some() <==> old.dir.at(query).is_some() by {}
    assert(new.dir.entries().len() == 0 <==> old.dir.entries().len() == 0) by {
        old.dir.model(); new.dir.model();
        if old.dir.entries().len() > 0 {
            assert(old.dir.at(old.dir.entries()[0].0).is_some());
            assert(new.dir.at(old.dir.entries()[0].0).is_some());
        }
        if new.dir.entries().len() > 0 {
            assert(new.dir.at(new.dir.entries()[0].0).is_some());
            assert(old.dir.at(new.dir.entries()[0].0).is_some());
        }
    }
    assert forall|query: KeyView| #![trigger new.page_at(query)] new.page_at(query) == if query == fence { page@ } else { old.page_at(query) } by {}
    assert(new.sorted_pages());
    assert(new.small_pages());
    assert(new.bounded_pages()) by {
        assert forall|query: KeyView, index: int| #![trigger new.page_at(query)[index]]
            0 <= index < new.page_at(query).len() implies !key_lt(query, new.page_at(query)[index].kv()) by {
            if query != fence { assert(old.page_at(query)[index] == new.page_at(query)[index]); }
        }
    }
    assert(new.separated_pages()) by {
        assert forall|query: KeyView, other: KeyView, index: int| #![trigger new.page_at(query)[index], new.dir.at(other)]
            new.dir.at(other).is_some() && key_lt(other, query) && 0 <= index < new.page_at(query).len()
            implies key_lt(other, new.page_at(query)[index].kv()) by {
            assert(old.dir.at(other).is_some());
            if query != fence { assert(old.page_at(query)[index] == new.page_at(query)[index]); }
        }
    }
    assert(new.top_fence());
    assert(new.full_pages()) by {
        assert forall|query: KeyView| #![trigger new.dir.at(query)]
            new.dir.at(query).is_some() && query != E::top() implies new.page_at(query).len() > 0 by {
            if query != fence { assert(old.dir.at(query).is_some()); }
        }
    }
}

impl<E: Ent> Paged<E> {
    pub fn new() -> (result: Self)
        ensures result.wf(), forall|key: KeyView| #![trigger result.at(key)] result.at(key).is_none(),
            result.entries() == Seq::<E>::empty(),
    {
        let result = Paged { dir: Index::new() };
        proof {
            assert forall|key: KeyView| result.at(key).is_none() by {
                assert(!result.has_key(key)) by {
                    assert forall|entry: E| !(result.holds(entry)) by {}
                }
            }
        }
        result
    }

    pub fn share(&self) -> (result: Self)
        requires self.wf(),
        ensures result.wf(), forall|key: KeyView| #![trigger result.at(key)] result.at(key) == self.at(key),
            forall|fence: KeyView| #![trigger result.dir.at(fence)] result.dir.at(fence) == self.dir.at(fence),
            result.dir.entries() == self.dir.entries(), result.entries() == self.entries(),
    {
        let result = Paged { dir: self.dir.share() };
        proof {
            assert forall|fence: KeyView| result.dir.at(fence) == self.dir.at(fence) by {}
            assert forall|fence: KeyView| #![trigger result.page_at(fence)] result.page_at(fence) == self.page_at(fence) by {}
            assert(result.dir.entries() == self.dir.entries());
            assert(result.wf());
            assert forall|entry: E| result.holds(entry) <==> self.holds(entry) by {
                if self.holds(entry) { let fence = choose|fence: KeyView| self.stored(fence, entry); assert(result.stored(fence, entry)); }
                if result.holds(entry) { let fence = choose|fence: KeyView| result.stored(fence, entry); assert(self.stored(fence, entry)); }
            }
            assert forall|key: KeyView| result.at(key) == self.at(key) by {
                assert(result.has_key(key) <==> self.has_key(key));
                if self.has_key(key) { assert(result.pick(key) == self.pick(key)) by {
                    lemma_unique(*self, result.pick(key), self.pick(key)); } }
            }
        }
        result
    }
}

/// Replace the page of `fence` by two pages, the first under a new fence at its last entry.
pub proof fn lemma_split_pages<E: Ent>(old: Paged<E>, new: Paged<E>, fence: KeyView, left_fence: KeyView, left: Vec<E>, right: Vec<E>)
    requires old.wf(), old.dir.at(fence).is_some(), new.dir.wf(),
        left@.len() > 0, right@.len() > 0, left_fence == left@.last().kv(),
        forall|query: KeyView| #![trigger new.dir.at(query)] new.dir.at(query)
            == if query == fence { Some(right) } else if query == left_fence { Some(left) } else { old.dir.at(query) },
        page_sorted(left@ + right@), left@.len() <= cap(), right@.len() <= cap(),
        forall|index: int| #![trigger (left@ + right@)[index]] 0 <= index < left@.len() + right@.len()
            ==> !key_lt(fence, (left@ + right@)[index].kv()),
        forall|index: int, other: KeyView| #![trigger (left@ + right@)[index], old.dir.at(other)]
            0 <= index < left@.len() + right@.len() && old.dir.at(other).is_some() && key_lt(other, fence)
            ==> key_lt(other, (left@ + right@)[index].kv()),
    ensures new.wf(), old.dir.at(left_fence).is_none(), left_fence != fence,
{
    let combined = left@ + right@;
    let last = left@.len() - 1;
    assert(combined[last] == left@.last());
    assert(!key_lt(fence, left_fence));
    assert(combined[last + 1] == right@[0]);
    assert(key_lt(left_fence, right@[0].kv()));
    assert(!key_lt(fence, right@[0].kv()));
    lemma_key_order(left_fence, fence);
    assert(key_lt(left_fence, fence)) by {
        if left_fence == fence { assert(key_lt(left_fence, right@[0].kv())); }
        else if key_lt(fence, left_fence) {}
    }
    assert(old.dir.at(left_fence).is_none()) by {
        if old.dir.at(left_fence).is_some() {
            assert(key_lt(left_fence, left@.last().kv()));
        }
    }
    assert(left_fence != fence);
    assert forall|query: KeyView| new.dir.at(query).is_some() <==> (old.dir.at(query).is_some() || query == left_fence) by {}
    assert(new.dir.entries().len() > 0);
    new.dir.model();
    assert forall|query: KeyView| #![trigger new.page_at(query)] new.page_at(query)
        == if query == fence { right@ } else if query == left_fence { left@ } else { old.page_at(query) } by {}
    assert(new.sorted_pages()) by {
        assert forall|query: KeyView| #![trigger new.page_at(query)] page_sorted(new.page_at(query)) by {
            if query == fence {
                assert forall|a: int, b: int| #![trigger right@[a], right@[b]] 0 <= a < b < right@.len() implies key_lt(right@[a].kv(), right@[b].kv()) by {
                    assert(combined[left@.len() + a] == right@[a]);
                    assert(combined[left@.len() + b] == right@[b]);
                }
            } else if query == left_fence {
                assert forall|a: int, b: int| #![trigger left@[a], left@[b]] 0 <= a < b < left@.len() implies key_lt(left@[a].kv(), left@[b].kv()) by {
                    assert(combined[a] == left@[a]);
                    assert(combined[b] == left@[b]);
                }
            }
        }
    }
    assert(new.small_pages());
    assert(new.bounded_pages()) by {
        assert forall|query: KeyView, index: int| #![trigger new.page_at(query)[index]]
            0 <= index < new.page_at(query).len() implies !key_lt(query, new.page_at(query)[index].kv()) by {
            if query == fence {
                assert(combined[left@.len() + index] == right@[index]);
            } else if query == left_fence {
                assert(combined[index] == left@[index]);
                if index < last {
                    assert(key_lt(combined[index].kv(), combined[last].kv()));
                    lemma_key_order(left_fence, left@[index].kv());
                } else { lemma_key_order(left_fence, left_fence); }
            } else {
                assert(old.page_at(query)[index] == new.page_at(query)[index]);
            }
        }
    }
    assert(new.separated_pages()) by {
        assert forall|query: KeyView, other: KeyView, index: int| #![trigger new.page_at(query)[index], new.dir.at(other)]
            new.dir.at(other).is_some() && key_lt(other, query) && 0 <= index < new.page_at(query).len()
            implies key_lt(other, new.page_at(query)[index].kv()) by {
            if query == fence {
                let entry = right@[index];
                assert(combined[left@.len() + index] == entry);
                if other == left_fence {
                    assert(key_lt(left_fence, entry.kv()));
                } else {
                    assert(old.dir.at(other).is_some());
                }
            } else if query == left_fence {
                let entry = left@[index];
                assert(combined[index] == entry);
                assert(other != left_fence);
                assert(other != fence) by { lemma_key_order(other, left_fence); if other == fence { assert(key_lt(fence, left_fence)); } }
                assert(old.dir.at(other).is_some());
                lemma_key_trans(other, left_fence, fence);
            } else {
                let entry = old.page_at(query)[index];
                assert(new.page_at(query)[index] == entry);
                if other == left_fence {
                    // query is an old fence above the new one
                    lemma_key_order(query, fence);
                    if key_lt(query, fence) {
                        // an old fence below `fence` lies below every entry of the page, including the last of the left page
                        assert(key_lt(query, left_fence)) by { assert(old.dir.at(query).is_some()); }
                        lemma_key_order(query, left_fence);
                    } else {
                        assert(query != fence);
                        assert(key_lt(fence, query));
                        assert(key_lt(fence, entry.kv()));
                        lemma_key_trans(left_fence, fence, entry.kv());
                    }
                } else {
                    assert(old.dir.at(other).is_some());
                }
            }
        }
    }
    assert(new.top_fence()) by {
        assert forall|query: KeyView| #![trigger new.dir.at(query)] new.dir.at(query).is_some() implies !key_lt(E::top(), query) by {
            if query == left_fence {
                assert(old.dir.at(fence).is_some());
                assert(!key_lt(E::top(), fence));
                if key_lt(E::top(), left_fence) { lemma_key_trans(E::top(), left_fence, fence); }
            }
        }
        assert(old.dir.entries().len() > 0) by { old.dir.model(); }
    }
    assert(new.full_pages()) by {
        assert forall|query: KeyView| #![trigger new.dir.at(query)]
            new.dir.at(query).is_some() && query != E::top() implies new.page_at(query).len() > 0 by {
            if query != fence && query != left_fence { assert(old.dir.at(query).is_some()); }
        }
    }
}

/// Entries held after putting `entry` on the page of `fence` (no split).
pub proof fn lemma_put_holds<E: Ent>(old: Paged<E>, new: Paged<E>, fence: KeyView, entry: E, view: Seq<E>)
    requires old.wf(), old.dir.at(fence).is_some(),
        !key_lt(fence, entry.kv()),
        forall|other: KeyView| #![trigger old.dir.at(other)]
            old.dir.at(other).is_some() && key_lt(other, fence) ==> key_lt(other, entry.kv()),
        forall|other: E| #![trigger view.contains(other)]
            view.contains(other) <==> (other == entry || (old.page_at(fence).contains(other) && other.kv() != entry.kv())),
        forall|query: KeyView| #![trigger new.page_at(query)]
            new.page_at(query) == if query == fence { view } else { old.page_at(query) },
    ensures forall|other: E| #![trigger new.holds(other)]
        new.holds(other) <==> (Some(entry) == Some(other) || (old.holds(other) && other.kv() != entry.kv())),
{
    assert forall|other: E| #![trigger new.holds(other)]
        new.holds(other) <==> (Some(entry) == Some(other) || (old.holds(other) && other.kv() != entry.kv())) by {
        if new.holds(other) {
            let at = choose|at: KeyView| new.stored(at, other);
            if at == fence {
                assert(view.contains(other));
                assert(old.stored(fence, other) || other == entry);
            } else {
                assert(old.stored(at, other));
                lemma_other_page(old, fence, entry.kv(), at, other);
            }
        }
        if other == entry { assert(view.contains(entry)); assert(new.stored(fence, entry)); }
        if old.holds(other) && other.kv() != entry.kv() {
            let at = choose|at: KeyView| old.stored(at, other);
            if at == fence { assert(view.contains(other)); assert(new.stored(fence, other)); }
            else { assert(new.stored(at, other)); }
        }
    }
}

/// Entries held after putting `entry` and splitting its page of `fence` into `left` (under `left_fence`) and `right`.
pub proof fn lemma_put_holds_split<E: Ent>(old: Paged<E>, new: Paged<E>, fence: KeyView, left_fence: KeyView, entry: E,
    view: Seq<E>, left: Seq<E>, right: Seq<E>)
    requires old.wf(), old.dir.at(fence).is_some(), old.dir.at(left_fence).is_none(), left_fence != fence,
        !key_lt(fence, entry.kv()),
        forall|other: KeyView| #![trigger old.dir.at(other)]
            old.dir.at(other).is_some() && key_lt(other, fence) ==> key_lt(other, entry.kv()),
        forall|other: E| #![trigger view.contains(other)]
            view.contains(other) <==> (other == entry || (old.page_at(fence).contains(other) && other.kv() != entry.kv())),
        view == left + right,
        forall|query: KeyView| #![trigger new.page_at(query)]
            new.page_at(query) == if query == fence { right } else if query == left_fence { left } else { old.page_at(query) },
    ensures forall|other: E| #![trigger new.holds(other)]
        new.holds(other) <==> (Some(entry) == Some(other) || (old.holds(other) && other.kv() != entry.kv())),
{
    assert forall|other: E| #![trigger new.holds(other)]
        new.holds(other) <==> (Some(entry) == Some(other) || (old.holds(other) && other.kv() != entry.kv())) by {
        assert forall|x: E| view.contains(x) <==> (left.contains(x) || right.contains(x)) by {
            if view.contains(x) {
                let at = choose|at: int| 0 <= at < view.len() && view[at] == x;
                if at < left.len() { assert(left[at] == x); } else { assert(right[at - left.len()] == x); }
            }
            if left.contains(x) { let at = choose|at: int| 0 <= at < left.len() && left[at] == x; assert(view[at] == x); }
            if right.contains(x) { let at = choose|at: int| 0 <= at < right.len() && right[at] == x; assert(view[left.len() + at] == x); }
        }
        if new.holds(other) {
            let at = choose|at: KeyView| new.stored(at, other);
            if at == fence || at == left_fence {
                assert(view.contains(other));
                assert(old.stored(fence, other) || other == entry);
            } else {
                assert(old.stored(at, other));
                lemma_other_page(old, fence, entry.kv(), at, other);
            }
        }
        if other == entry {
            assert(view.contains(entry));
            if left.contains(entry) { assert(new.stored(left_fence, entry)); } else { assert(new.stored(fence, entry)); }
        }
        if old.holds(other) && other.kv() != entry.kv() {
            let at = choose|at: KeyView| old.stored(at, other);
            if at == fence {
                assert(view.contains(other));
                if left.contains(other) { assert(new.stored(left_fence, other)); } else { assert(new.stored(fence, other)); }
            } else {
                assert(at != left_fence);
                assert(new.stored(at, other));
            }
        }
    }
}

impl<E: Ent> Paged<E> {
    pub fn put(&mut self, entry: E)
        requires old(self).wf(),
        ensures final(self).wf(),
            forall|key: KeyView| #![trigger final(self).at(key)]
                final(self).at(key) == if key == entry.kv() { Some(entry) } else { old(self).at(key) },
            forall|other: E| #![trigger final(self).holds(other)]
                final(self).holds(other) <==> (other == entry || (old(self).holds(other) && other.kv() != entry.kv())),
    {
        let ghost before = *self;
        proof { before.dir.model(); }
        if self.dir.is_empty() {
            let mut page = Vec::new();
            page.push(entry);
            let ghost view = page;
            self.dir.put(E::top_key(), Arc::new(page));
            proof {
                E::top_above(entry);
                lemma_empty_dir(before);
                assert forall|query: KeyView| #![trigger self.dir.at(query)] self.dir.at(query) == if query == E::top() { Some(view) } else { None::<Vec<E>> } by {
                    assert(before.dir.at(query).is_none());
                }
                assert forall|query: KeyView| #![trigger self.page_at(query)] self.page_at(query) == if query == E::top() { view@ } else { Seq::<E>::empty() } by {}
                self.dir.model();
                assert(self.wf()) by {
                    assert(self.sorted_pages());
                    assert(self.small_pages());
                    assert(self.bounded_pages()) by {
                        assert forall|fence: KeyView, index: int| #![trigger self.page_at(fence)[index]]
                            0 <= index < self.page_at(fence).len() implies !key_lt(fence, self.page_at(fence)[index].kv()) by {
                            assert(fence == E::top());
                            lemma_key_order(E::top(), entry.kv());
                        }
                    }
                    assert(self.separated_pages()) by {
                        assert forall|fence: KeyView, other: KeyView, index: int| #![trigger self.page_at(fence)[index], self.dir.at(other)]
                            self.dir.at(other).is_some() && key_lt(other, fence) && 0 <= index < self.page_at(fence).len()
                            implies key_lt(other, self.page_at(fence)[index].kv()) by {
                            lemma_key_order(other, fence);
                        }
                    }
                    assert(self.top_fence()) by {
                        assert forall|fence: KeyView| #![trigger self.dir.at(fence)] self.dir.at(fence).is_some() implies !key_lt(E::top(), fence) by {
                            assert(fence == E::top());
                            lemma_key_order(E::top(), E::top());
                        }
                    }
                    assert(self.full_pages());
                }
                assert forall|other: E| #![trigger self.holds(other)] self.holds(other) <==> (Some(entry) == Some(other) || (before.holds(other) && other.kv() != entry.kv())) by {
                    if self.holds(other) {
                        let fence = choose|fence: KeyView| self.stored(fence, other);
                        assert(fence == E::top());
                        assert(view@.contains(other));
                        assert(other == entry);
                    }
                    if other == entry {
                        assert(view@.len() == 1 && view@[0] == entry);
                        assert(self.page_at(E::top()) == view@);
                        assert(self.stored(E::top(), entry));
                    }
                }
                lemma_at_update(before, *self, entry.kv(), Some(entry));
            }
            return;
        }
        let (fence, page) = self.locate(&entry);
        let ghost fence_view = fence.view();
        let position = search(&*page, &entry);
        let replace = position < page.len() && !entry.before(&page[position]);
        let skip = if replace { 1usize } else { 0usize };
        proof {
            lemma_key_order(entry.kv(), entry.kv());
            if position < page.len() {
                lemma_key_order(page@[position as int].kv(), entry.kv());
            }
            assert(before.page_at(fence_view) == page@);
            lemma_splice(page@, position as int, skip as int, entry);
        }
        let newpage = page_with(&*page, position, skip, entry);
        let ghost view = newpage;
        proof {
            // every entry of the new page is within the fence range
            assert forall|index: int| #![trigger view@[index]] 0 <= index < view@.len() implies {
                &&& !key_lt(fence_view, view@[index].kv())
                &&& forall|other: KeyView| #![trigger before.dir.at(other)]
                    before.dir.at(other).is_some() && key_lt(other, fence_view) ==> key_lt(other, view@[index].kv())
            } by {
                let item = view@[index];
                assert(view@.contains(item));
                if item != entry {
                    assert(page@.contains(item));
                    let at = choose|at: int| 0 <= at < page@.len() && page@[at] == item;
                    assert(before.page_at(fence_view)[at] == item);
                }
            }
        }
        if newpage.len() <= 128 {
            self.dir.put(fence, Arc::new(newpage));
            proof {
                assert forall|index: int, other: KeyView| #![trigger view@[index], before.dir.at(other)]
                    0 <= index < view@.len() && before.dir.at(other).is_some() && key_lt(other, fence_view)
                    implies key_lt(other, view@[index].kv()) by {}
                lemma_replace_page(before, *self, fence_view, view);
                self.dir.model();
                assert forall|query: KeyView| #![trigger self.page_at(query)]
                    self.page_at(query) == if query == fence_view { view@ } else { before.page_at(query) } by {}
                lemma_put_holds(before, *self, fence_view, entry, view@);
                lemma_at_update(before, *self, entry.kv(), Some(entry));
            }
        } else {
            let half = newpage.len() / 2;
            let left = page_part(&newpage, 0, half);
            let right = page_part(&newpage, half, newpage.len());
            let left_fence = left[half - 1].key();
            let ghost left_view = left;
            let ghost right_view = right;
            proof {
                assert(view@ =~= left_view@ + right_view@);
                assert(left_view@.last() == left_view@[half as int - 1]);
            }
            self.dir.put(left_fence, Arc::new(left));
            self.dir.put(fence, Arc::new(right));
            let ghost left_key = left_view@.last().kv();
            proof {
                assert forall|query: KeyView| #![trigger self.dir.at(query)] self.dir.at(query)
                    == if query == fence_view { Some(right_view) } else if query == left_key { Some(left_view) } else { before.dir.at(query) } by {}
                assert forall|index: int| #![trigger (left_view@ + right_view@)[index]] 0 <= index < left_view@.len() + right_view@.len()
                    implies !key_lt(fence_view, (left_view@ + right_view@)[index].kv()) by {}
                lemma_split_pages(before, *self, fence_view, left_key, left_view, right_view);
                self.dir.model();
                assert forall|query: KeyView| #![trigger self.page_at(query)]
                    self.page_at(query) == if query == fence_view { right_view@ } else if query == left_key { left_view@ } else { before.page_at(query) } by {}
                lemma_put_holds_split(before, *self, fence_view, left_key, entry, view@, left_view@, right_view@);
                lemma_at_update(before, *self, entry.kv(), Some(entry));
            }
        }
    }
}

pub proof fn lemma_remove_page<E: Ent>(page: Seq<E>, position: int, probe: KeyView)
    requires page_sorted(page), 0 <= position < page.len(), page[position].kv() == probe,
    ensures page_sorted(page.take(position) + page.skip(position + 1)),
        forall|other: E| #![trigger (page.take(position) + page.skip(position + 1)).contains(other)]
            (page.take(position) + page.skip(position + 1)).contains(other) <==> (page.contains(other) && other.kv() != probe),
{
    let head = page.take(position);
    let tail = page.skip(position + 1);
    let result = head + tail;
    assert forall|first: int, last: int| #![trigger result[first], result[last]]
        0 <= first < last < result.len() implies key_lt(result[first].kv(), result[last].kv()) by {
        let h = head.len() as int;
        if last < h { assert(result[first] == page[first]); assert(result[last] == page[last]); }
        else if first < h {
            assert(result[first] == page[first]);
            assert(result[last] == page[position + 1 + (last - h)]);
        } else {
            assert(result[first] == page[position + 1 + (first - h)]);
            assert(result[last] == page[position + 1 + (last - h)]);
        }
    }
    assert forall|other: E| #![trigger result.contains(other)]
        result.contains(other) <==> (page.contains(other) && other.kv() != probe) by {
        if result.contains(other) {
            let at = choose|at: int| 0 <= at < result.len() && result[at] == other;
            if at < head.len() {
                assert(page[at] == other);
                assert(key_lt(page[at].kv(), page[position].kv()));
                lemma_key_order(other.kv(), probe);
            } else {
                let t = at - head.len();
                assert(page[position + 1 + t] == other);
                assert(key_lt(page[position].kv(), page[position + 1 + t].kv()));
                lemma_key_order(other.kv(), probe);
            }
        }
        if page.contains(other) && other.kv() != probe {
            let at = choose|at: int| 0 <= at < page.len() && page[at] == other;
            if at < position { assert(result[at] == other); }
            else {
                assert(at != position);
                assert(result[at - 1] == other);
            }
        }
    }
}

/// Drop a fence other than the last.
pub proof fn lemma_remove_fence<E: Ent>(old: Paged<E>, new: Paged<E>, fence: KeyView)
    requires old.wf(), new.dir.wf(), old.dir.at(fence).is_some(), fence != E::top(),
        forall|query: KeyView| #![trigger new.dir.at(query)] new.dir.at(query) == if query == fence { None } else { old.dir.at(query) },
    ensures new.wf(),
{
    new.dir.model();
    old.dir.model();
    assert forall|query: KeyView| #![trigger new.page_at(query)] new.page_at(query) == if query == fence { Seq::<E>::empty() } else { old.page_at(query) } by {}
    assert(new.sorted_pages()) by {
        assert forall|query: KeyView| #![trigger new.page_at(query)] page_sorted(new.page_at(query)) by {
            if query == fence { assert(new.page_at(query).len() == 0); }
        }
    }
    assert(new.bounded_pages()) by {
        assert forall|query: KeyView, index: int| #![trigger new.page_at(query)[index]]
            0 <= index < new.page_at(query).len() implies !key_lt(query, new.page_at(query)[index].kv()) by {
            assert(query != fence);
            assert(old.page_at(query)[index] == new.page_at(query)[index]);
        }
    }
    assert(new.separated_pages()) by {
        assert forall|query: KeyView, other: KeyView, index: int| #![trigger new.page_at(query)[index], new.dir.at(other)]
            new.dir.at(other).is_some() && key_lt(other, query) && 0 <= index < new.page_at(query).len()
            implies key_lt(other, new.page_at(query)[index].kv()) by {
            assert(query != fence);
            assert(old.dir.at(other).is_some());
            assert(old.page_at(query)[index] == new.page_at(query)[index]);
        }
    }
    assert(new.top_fence()) by {
        if new.dir.entries().len() > 0 {
            assert(new.dir.at(new.dir.entries()[0].0).is_some());
            assert(old.dir.at(new.dir.entries()[0].0).is_some());
            assert(old.dir.entries().len() > 0) by {
                assert(exists|position: int| #![auto] 0 <= position < old.dir.entries().len() && old.dir.entries()[position].0 == new.dir.entries()[0].0);
            }
            assert(old.dir.at(E::top()).is_some());
        }
    }
    assert(new.full_pages()) by {
        assert forall|query: KeyView| #![trigger new.dir.at(query)]
            new.dir.at(query).is_some() && query != E::top() implies new.page_at(query).len() > 0 by {
            assert(query != fence);
            assert(old.dir.at(query).is_some());
        }
    }
}

/// Entries held after removing the probe's entry from the page of `fence`.
pub proof fn lemma_remove_holds<E: Ent>(old: Paged<E>, new: Paged<E>, fence: KeyView, probe: KeyView, view: Seq<E>, kept: bool)
    requires old.wf(), old.dir.at(fence).is_some(),
        !key_lt(fence, probe),
        forall|other: KeyView| #![trigger old.dir.at(other)]
            old.dir.at(other).is_some() && key_lt(other, fence) ==> key_lt(other, probe),
        forall|other: E| #![trigger view.contains(other)]
            view.contains(other) <==> (old.page_at(fence).contains(other) && other.kv() != probe),
        forall|query: KeyView| #![trigger new.page_at(query)]
            new.page_at(query) == if query == fence { if kept { view } else { Seq::<E>::empty() } } else { old.page_at(query) },
        !kept ==> view.len() == 0,
    ensures forall|other: E| #![trigger new.holds(other)] new.holds(other) <==> (old.holds(other) && other.kv() != probe),
{
    assert forall|other: E| #![trigger new.holds(other)] new.holds(other) <==> (old.holds(other) && other.kv() != probe) by {
        if new.holds(other) {
            let at = choose|at: KeyView| new.stored(at, other);
            if at == fence {
                assert(kept);
                assert(view.contains(other));
                assert(old.stored(fence, other));
            } else {
                assert(old.stored(at, other));
                lemma_other_page(old, fence, probe, at, other);
            }
        }
        if old.holds(other) && other.kv() != probe {
            let at = choose|at: KeyView| old.stored(at, other);
            if at == fence {
                assert(view.contains(other));
                assert(view.len() > 0) by { assert(view.contains(other)); }
                assert(kept);
                assert(new.stored(fence, other));
            } else {
                assert(new.stored(at, other));
            }
        }
    }
}

impl<E: Ent> Paged<E> {
    pub fn remove(&mut self, probe: &E)
        requires old(self).wf(),
        ensures final(self).wf(),
            forall|key: KeyView| #![trigger final(self).at(key)]
                final(self).at(key) == if key == probe.kv() { None } else { old(self).at(key) },
            forall|other: E| #![trigger final(self).holds(other)]
                final(self).holds(other) <==> (old(self).holds(other) && other.kv() != probe.kv()),
    {
        let ghost before = *self;
        proof { before.dir.model(); }
        if self.dir.is_empty() {
            proof { lemma_empty_dir(before); }
            return;
        }
        let (fence, page) = self.locate(probe);
        let ghost fence_view = fence.view();
        let position = search(&*page, probe);
        proof {
            lemma_key_order(probe.kv(), probe.kv());
            assert(before.page_at(fence_view) == page@);
        }
        if position < page.len() && !probe.before(&page[position]) {
            proof {
                lemma_key_order(page@[position as int].kv(), probe.kv());
                lemma_remove_page(page@, position as int, probe.kv());
            }
            let newpage = page_without(&*page, position);
            let ghost view = newpage;
            let is_top = verus_prooflib::ordered::compare(&fence, &E::top_key()) == 0;
            if newpage.len() > 0 || is_top {
                self.dir.put(fence, Arc::new(newpage));
                proof {
                    assert forall|index: int| #![trigger view@[index]] 0 <= index < view@.len() implies {
                        &&& !key_lt(fence_view, view@[index].kv())
                        &&& forall|other: KeyView| #![trigger before.dir.at(other)]
                            before.dir.at(other).is_some() && key_lt(other, fence_view) ==> key_lt(other, view@[index].kv())
                    } by {
                        let item = view@[index];
                        assert(view@.contains(item));
                        assert(page@.contains(item));
                        let at = choose|at: int| 0 <= at < page@.len() && page@[at] == item;
                        assert(before.page_at(fence_view)[at] == item);
                    }
                    assert forall|index: int, other: KeyView| #![trigger view@[index], before.dir.at(other)]
                        0 <= index < view@.len() && before.dir.at(other).is_some() && key_lt(other, fence_view)
                        implies key_lt(other, view@[index].kv()) by {}
                    lemma_replace_page(before, *self, fence_view, view);
                    self.dir.model();
                    assert forall|query: KeyView| #![trigger self.page_at(query)]
                        self.page_at(query) == if query == fence_view { view@ } else { before.page_at(query) } by {}
                    lemma_remove_holds(before, *self, fence_view, probe.kv(), view@, true);
                }
            } else {
                self.dir.remove(&fence);
                proof {
                    lemma_remove_fence(before, *self, fence_view);
                    self.dir.model();
                    assert forall|query: KeyView| #![trigger self.page_at(query)]
                        self.page_at(query) == if query == fence_view { Seq::<E>::empty() } else { before.page_at(query) } by {}
                    lemma_remove_holds(before, *self, fence_view, probe.kv(), view@, false);
                }
            }
            proof {
                assert forall|other: E| #![trigger self.holds(other)] self.holds(other) <==> (None::<E> == Some(other) || (before.holds(other) && other.kv() != probe.kv())) by {}
                lemma_at_update(before, *self, probe.kv(), None);
            }
        } else {
            proof {
                lemma_not_found(before, fence_view, page@, position as int, probe.kv());
                assert forall|key: KeyView| #![trigger self.at(key)] self.at(key) == if key == probe.kv() { None } else { before.at(key) } by {
                    assert(self.at(key) == before.at(key));
                }
                assert forall|other: E| #![trigger self.holds(other)]
                    self.holds(other) <==> (before.holds(other) && other.kv() != probe.kv()) by {
                    if before.holds(other) && other.kv() == probe.kv() { assert(before.has_key(probe.kv())); }
                }
            }
        }
    }

    /// No entry is stored.
    pub fn is_empty(&self) -> (result: bool)
        requires self.wf(),
        ensures result == !self.nonempty(),
    {
        proof { lemma_nonempty(*self); }
        proof { self.dir.model(); }
        let mut cursor = self.dir.cursor();
        let ghost rows = self.dir.entries();
        match cursor.next() {
            None => {
                proof { lemma_empty_dir(*self); }
                true
            },
            Some((fence, page)) => {
                proof {
                    assert((fence.view(), *page) == rows[0]);
                    assert(self.dir.at(fence.view()) == Some(*page));
                }
                if page.len() > 0 {
                    proof {
                        assert(self.stored(fence.view(), page@[0]));
                        assert(self.holds(page@[0]));
                    }
                    false
                } else {
                    proof {
                        // an empty page is the last fence, so it is the only page
                        assert(fence.view() == E::top());
                        assert(cursor.remaining() == rows.skip(1));
                        assert forall|entry: E| !self.holds(entry) by {
                            if self.holds(entry) {
                                let at = choose|at: KeyView| self.stored(at, entry);
                                assert(self.dir.at(at).is_some());
                                let k = choose|k: int| #![auto] 0 <= k < rows.len() && rows[k].0 == at;
                                if k > 0 { assert(key_lt(rows[0].0, rows[k].0)); lemma_key_order(E::top(), at); }
                                assert(k == 0);
                                assert(self.page_at(at).len() == 0);
                            }
                        }
                    }
                    true
                }
            },
        }
    }
}

// ---- ordered view and cursor ----------------------------------------------------------------

pub open spec fn page_rows<E: Ent>(rows: Seq<(KeyView, Vec<E>)>) -> Seq<Seq<E>> {
    rows.map_values(|row: (KeyView, Vec<E>)| row.1@)
}

pub proof fn lemma_flat_first<A>(pages: Seq<Seq<A>>)
    requires pages.len() > 0,
    ensures verus_common::rel::flat(pages) == pages[0] + verus_common::rel::flat(pages.skip(1)),
    decreases pages.len(),
{
    if pages.len() == 1 {
        assert(pages.skip(1) =~= Seq::<Seq<A>>::empty());
        assert(pages.drop_last() =~= Seq::<Seq<A>>::empty());
        assert(verus_common::rel::flat(pages.drop_last()) =~= Seq::<A>::empty());
        assert(pages.last() == pages[0]);
        assert(verus_common::rel::flat(pages) =~= pages[0]);
    } else {
        let prefix = pages.drop_last();
        assert(prefix.len() > 0);
        lemma_flat_first(prefix);
        assert(pages.skip(1).drop_last() =~= prefix.skip(1));
        assert(pages.skip(1).len() > 0);
        assert(pages.skip(1).last() == pages.last());
        assert(verus_common::rel::flat(pages) =~= prefix[0] + verus_common::rel::flat(prefix.skip(1)) + pages.last());
        assert(verus_common::rel::flat(pages.skip(1)) =~= verus_common::rel::flat(prefix.skip(1)) + pages.last());
    }
}

impl<E: Ent> Paged<E> {
    pub open spec fn entries(self) -> Seq<E> {
        verus_common::rel::flat(page_rows(self.dir.entries()))
    }

    pub proof fn model(&self)
        requires self.wf(),
        ensures
            forall|first: int, last: int| #![trigger self.entries()[first], self.entries()[last]]
                0 <= first < last < self.entries().len() ==> key_lt(self.entries()[first].kv(), self.entries()[last].kv()),
            forall|entry: E| #![trigger self.entries().contains(entry)] self.entries().contains(entry) <==> self.holds(entry),
    {
        self.dir.model();
        let rows = self.dir.entries();
        let pages = page_rows(rows);
        assert forall|index: int| 0 <= index < rows.len() implies pages[index] == self.page_at(rows[index].0) by {
            assert(self.dir.at(rows[index].0) == Some(rows[index].1));
        }
        assert forall|entry: E| #![trigger self.entries().contains(entry)] self.entries().contains(entry) <==> self.holds(entry) by {
            verus_common::rel::lemma_flat_contains(pages, entry);
            if self.entries().contains(entry) {
                let index = choose|index: int| 0 <= index < pages.len() && pages[index].contains(entry);
                assert(self.stored(rows[index].0, entry));
            }
            if self.holds(entry) {
                let fence = choose|fence: KeyView| self.stored(fence, entry);
                assert(self.dir.at(fence).is_some());
                let index = choose|index: int| #![auto] 0 <= index < rows.len() && rows[index].0 == fence;
                assert(pages[index].contains(entry));
            }
        }
        assert forall|page: int, left: int, right: int| 0 <= page < pages.len() && 0 <= left < right < pages[page].len()
            implies key_lt(pages[page][left].kv(), pages[page][right].kv()) by {
            assert(pages[page] == self.page_at(rows[page].0));
        }
        assert forall|earlier: int, later: int, left: int, right: int| 0 <= earlier < later < pages.len()
            && 0 <= left < pages[earlier].len() && 0 <= right < pages[later].len()
            implies key_lt(pages[earlier][left].kv(), pages[later][right].kv()) by {
            let a = pages[earlier][left];
            let b = pages[later][right];
            assert(pages[earlier] == self.page_at(rows[earlier].0));
            assert(pages[later] == self.page_at(rows[later].0));
            assert(key_lt(rows[earlier].0, rows[later].0));
            assert(key_lt(rows[earlier].0, b.kv()));
            assert(!key_lt(rows[earlier].0, a.kv()));
            lemma_key_order(a.kv(), rows[earlier].0);
            if a.kv() != rows[earlier].0 { lemma_key_trans(a.kv(), rows[earlier].0, b.kv()); }
        }
        verus_prooflib::lemma_flat_sorted(pages, |left: E, right: E| key_lt(left.kv(), right.kv()));
    }
}

pub struct PageCursor<E: Ent> {
    dir: Cursor<Vec<E>>,
    page: Arc<Vec<E>>,
    position: usize,
}

impl<E: Ent> PageCursor<E> {
    pub closed spec fn remaining(self) -> Seq<E> {
        self.page@.skip(self.position as int) + verus_common::rel::flat(page_rows(self.dir.remaining()))
    }

    pub closed spec fn well_formed(self) -> bool {
        self.position <= self.page@.len() && self.dir.work() >= 0
    }

    /// The next entry in ascending order.
    pub fn next(&mut self) -> (result: Option<E>)
        requires old(self).well_formed(),
        ensures final(self).well_formed(),
            match result {
                Some(entry) => old(self).remaining().len() > 0 && entry == old(self).remaining()[0]
                    && final(self).remaining() == old(self).remaining().skip(1),
                None => old(self).remaining().len() == 0 && final(self).remaining().len() == 0,
            },
    {
        let ghost start = self.remaining();
        loop
            invariant self.well_formed(), self.remaining() == start, start == old(self).remaining(),
            decreases self.dir.work(),
        {
            if self.position < self.page.len() {
                let entry = self.page[self.position].dup();
                self.position += 1;
                proof {
                    assert(start.skip(1) =~= self.page@.skip(self.position as int)
                        + verus_common::rel::flat(page_rows(self.dir.remaining())));
                    assert(start[0] == entry);
                }
                return Some(entry);
            }
            let ghost before = self.dir.remaining();
            let ghost old_work = self.dir.work();
            match self.dir.next() {
                Some((_, page)) => {
                    proof {
                        assert(page_rows(before)[0] == page@);
                        lemma_flat_first(page_rows(before));
                        assert(page_rows(self.dir.remaining()) =~= page_rows(before).skip(1));
                        assert(self.page@.skip(self.position as int) =~= Seq::<E>::empty());
                    }
                    self.page = page;
                    self.position = 0;
                    proof {
                        assert(self.page@.skip(0) =~= self.page@);
                    }
                },
                None => {
                    proof {
                        assert(self.page@.skip(self.position as int) =~= Seq::<E>::empty());
                        assert(page_rows(before).len() == 0);
                        assert(verus_common::rel::flat(page_rows(before)) =~= Seq::<E>::empty());
                    }
                    return None;
                },
            }
        }
    }
}

impl<E: Ent> Paged<E> {
    pub fn cursor(&self) -> (result: PageCursor<E>)
        requires self.wf(),
        ensures result.remaining() == self.entries(), result.well_formed(),
    {
        let cursor = PageCursor { dir: self.dir.cursor(), page: Arc::new(Vec::new()), position: 0 };
        proof {
            assert(cursor.page@.skip(0) =~= Seq::<E>::empty());
            assert(cursor.remaining() =~= self.entries());
        }
        cursor
    }
}

pub proof fn lemma_nonempty<E: Ent>(paged: Paged<E>)
    ensures paged.nonempty() == exists|entry: E| #![trigger paged.holds(entry)] paged.holds(entry),
{
    if paged.nonempty() {
        let key = choose|key: KeyView| paged.at(key).is_some();
        assert(paged.has_key(key));
        let witness = paged.pick(key);
        assert(paged.holds(witness));
        assert(exists|entry: E| #![trigger paged.holds(entry)] paged.holds(entry));
    }
    if exists|entry: E| #![trigger paged.holds(entry)] paged.holds(entry) {
        let entry = choose|entry: E| #![trigger paged.holds(entry)] paged.holds(entry);
        assert(paged.holds(entry) && entry.kv() == entry.kv());
        assert(paged.has_key(entry.kv()));
        assert(paged.at(entry.kv()).is_some());
        assert(paged.nonempty());
    }
}

// ---- sums over the ordered entries ------------------------------------------------------------

/// Number of leading entries strictly below `key`.
pub open spec fn below<E: Ent>(entries: Seq<E>, key: KeyView) -> int
    decreases entries.len(),
{
    if entries.len() == 0 { 0 }
    else if key_lt(entries.last().kv(), key) { entries.len() as int }
    else { below(entries.drop_last(), key) }
}

pub open spec fn entry_sorted<E: Ent>(entries: Seq<E>) -> bool {
    forall|first: int, last: int| #![trigger entries[first], entries[last]]
        0 <= first < last < entries.len() ==> key_lt(entries[first].kv(), entries[last].kv())
}

pub proof fn lemma_below<E: Ent>(entries: Seq<E>, key: KeyView)
    requires entry_sorted(entries),
    ensures 0 <= below(entries, key) <= entries.len(),
        forall|index: int| #![trigger entries[index]] 0 <= index < below(entries, key) ==> key_lt(entries[index].kv(), key),
        forall|index: int| #![trigger entries[index]] below(entries, key) <= index < entries.len() ==> !key_lt(entries[index].kv(), key),
    decreases entries.len(),
{
    if entries.len() > 0 {
        let prefix = entries.drop_last();
        assert(entry_sorted(prefix)) by {
            assert forall|first: int, last: int| 0 <= first < last < prefix.len() implies key_lt(prefix[first].kv(), prefix[last].kv()) by {
                assert(prefix[first] == entries[first]);
                assert(prefix[last] == entries[last]);
            }
        }
        lemma_below(prefix, key);
        if key_lt(entries.last().kv(), key) {
            assert forall|index: int| 0 <= index < entries.len() implies key_lt(entries[index].kv(), key) by {
                if index < entries.len() - 1 { lemma_key_trans(entries[index].kv(), entries.last().kv(), key); }
            }
        } else {
            let at = below(prefix, key);
            assert forall|index: int| 0 <= index < at implies key_lt(entries[index].kv(), key) by { assert(entries[index] == prefix[index]); }
            assert forall|index: int| at <= index < entries.len() implies !key_lt(entries[index].kv(), key) by {
                if index < prefix.len() { assert(entries[index] == prefix[index]); }
            }
        }
    }
}

/// The entries below `key`, its own entry (when present), and the entries above.
pub proof fn lemma_split<E: Ent>(paged: Paged<E>, key: KeyView)
    requires paged.wf(),
    ensures
        ({
            let entries = paged.entries();
            let at = below(entries, key);
            let mid = if paged.at(key).is_some() { 1int } else { 0int };
            &&& 0 <= at <= entries.len() && at + mid <= entries.len()
            &&& forall|index: int| #![trigger entries[index]] 0 <= index < at ==> key_lt(entries[index].kv(), key)
            &&& forall|index: int| #![trigger entries[index]] at + mid <= index < entries.len() ==> key_lt(key, entries[index].kv())
            &&& paged.at(key).is_some() ==> entries[at] == paged.at(key).unwrap()
        }),
{
    paged.model();
    let entries = paged.entries();
    assert(entry_sorted(entries));
    lemma_below(entries, key);
    let at = below(entries, key);
    // an entry with the key, if any, is the first entry at or above `at`
    if paged.has_key(key) {
        let found = paged.pick(key);
        assert(paged.holds(found));
        assert(entries.contains(found));
        let index = choose|index: int| 0 <= index < entries.len() && entries[index] == found;
        lemma_key_order(found.kv(), key);
        assert(index >= at) by { if index < at { assert(key_lt(entries[index].kv(), key)); } }
        assert(index == at) by {
            if index > at {
                assert(key_lt(entries[at].kv(), entries[index].kv()));
                assert(!key_lt(entries[at].kv(), key));
            }
        }
        assert forall|j: int| at + 1 <= j < entries.len() implies key_lt(key, entries[j].kv()) by {
            assert(key_lt(entries[at].kv(), entries[j].kv()));
        }
        lemma_at(paged, key, found);
    } else {
        assert forall|j: int| at <= j < entries.len() implies key_lt(key, entries[j].kv()) by {
            assert(!key_lt(entries[j].kv(), key));
            lemma_key_order(entries[j].kv(), key);
            assert(entries[j].kv() != key) by {
                if entries[j].kv() == key { assert(entries.contains(entries[j])); assert(paged.holds(entries[j])); assert(paged.has_key(key)); }
            }
        }
    }
}

pub open spec fn strict_entry_order<E: Ent>() -> spec_fn(E, E) -> bool {
    |left: E, right: E| key_lt(left.kv(), right.kv())
}

pub proof fn lemma_entry_order<E: Ent>()
    ensures verus_common::order::strict_order(strict_entry_order::<E>()),
{
    assert forall|x: E| !#[trigger] strict_entry_order::<E>()(x, x) by { lemma_key_order(x.kv(), x.kv()); }
    assert forall|x: E, y: E, z: E| #[trigger] strict_entry_order::<E>()(x, y) && #[trigger] strict_entry_order::<E>()(y, z)
        implies strict_entry_order::<E>()(x, z) by { lemma_key_trans(x.kv(), y.kv(), z.kv()); }
}

/// After a put the entries are the old ones below the key, the new entry, and the old ones above.
#[verifier::rlimit(200)]
pub proof fn lemma_entries_put<E: Ent>(old: Paged<E>, new: Paged<E>, entry: E)
    requires old.wf(), new.wf(),
        forall|x: E| #![trigger new.holds(x)] new.holds(x) <==> (x == entry || (old.holds(x) && x.kv() != entry.kv())),
    ensures ({
        let entries = old.entries();
        let at = below(entries, entry.kv());
        let mid = if old.at(entry.kv()).is_some() { 1int } else { 0int };
        new.entries() == entries.take(at) + seq![entry] + entries.skip(at + mid)
    }),
{
    lemma_split(old, entry.kv());
    old.model();
    new.model();
    let entries = old.entries();
    let at = below(entries, entry.kv());
    let mid = if old.at(entry.kv()).is_some() { 1int } else { 0int };
    let pre = entries.take(at);
    let post = entries.skip(at + mid);
    let built = pre + seq![entry] + post;
    lemma_entry_order::<E>();
    assert(entry_sorted(built)) by {
        assert forall|first: int, last: int| #![trigger built[first], built[last]]
            0 <= first < last < built.len() implies key_lt(built[first].kv(), built[last].kv()) by {
            let h = pre.len() as int;
            if last < h { assert(built[first] == entries[first]); assert(built[last] == entries[last]); }
            else if last == h { assert(built[first] == entries[first]); assert(built[last] == entry); }
            else if first < h {
                assert(built[first] == entries[first]); assert(built[last] == entries[at + mid + (last - h - 1)]);
                lemma_key_trans(entries[first].kv(), entry.kv(), entries[at + mid + (last - h - 1)].kv());
            } else if first == h {
                assert(built[first] == entry); assert(built[last] == entries[at + mid + (last - h - 1)]);
            } else {
                assert(built[first] == entries[at + mid + (first - h - 1)]);
                assert(built[last] == entries[at + mid + (last - h - 1)]);
            }
        }
    }
    assert forall|x: E| new.entries().contains(x) <==> built.contains(x) by {
        if built.contains(x) {
            let index = choose|index: int| 0 <= index < built.len() && built[index] == x;
            if index < pre.len() {
                assert(entries[index] == x); assert(entries.contains(x)); assert(old.holds(x));
                assert(x.kv() != entry.kv()) by { assert(key_lt(entries[index].kv(), entry.kv())); lemma_key_order(x.kv(), entry.kv()); }
            } else if index > pre.len() {
                let t = at + mid + (index - pre.len() as int - 1);
                assert(entries[t] == x); assert(entries.contains(x)); assert(old.holds(x));
                assert(x.kv() != entry.kv()) by { assert(key_lt(entry.kv(), entries[t].kv())); lemma_key_order(x.kv(), entry.kv()); }
            } else { assert(x == entry); }
            assert(new.holds(x));
        }
        if new.entries().contains(x) {
            assert(new.holds(x));
            if x != entry {
                assert(old.holds(x) && x.kv() != entry.kv());
                assert(entries.contains(x));
                let index = choose|index: int| 0 <= index < entries.len() && entries[index] == x;
                if index < at { assert(built[index] == x); }
                else {
                    assert(index >= at + mid) by {
                        if mid == 1 && index == at { assert(x == old.at(entry.kv()).unwrap()); lemma_at(old, entry.kv(), x); }
                    }
                    assert(built[index + 1 - mid] == x);
                }
            } else { assert(built[at] == entry); }
        }
    }
    verus_common::order::lemma_sorted_unique(new.entries(), built, strict_entry_order::<E>());
}

pub proof fn lemma_entries_remove<E: Ent>(old: Paged<E>, new: Paged<E>, key: KeyView)
    requires old.wf(), new.wf(),
        forall|x: E| #![trigger new.holds(x)] new.holds(x) <==> (old.holds(x) && x.kv() != key),
    ensures ({
        let entries = old.entries();
        let at = below(entries, key);
        let mid = if old.at(key).is_some() { 1int } else { 0int };
        new.entries() == entries.take(at) + entries.skip(at + mid)
    }),
{
    lemma_split(old, key);
    old.model();
    new.model();
    let entries = old.entries();
    let at = below(entries, key);
    let mid = if old.at(key).is_some() { 1int } else { 0int };
    let built = entries.take(at) + entries.skip(at + mid);
    lemma_entry_order::<E>();
    assert(entry_sorted(built)) by {
        assert forall|first: int, last: int| #![trigger built[first], built[last]]
            0 <= first < last < built.len() implies key_lt(built[first].kv(), built[last].kv()) by {
            let h = at;
            if last < h { assert(built[first] == entries[first]); assert(built[last] == entries[last]); }
            else if first < h {
                assert(built[first] == entries[first]); assert(built[last] == entries[at + mid + (last - h)]);
                if mid == 1 { assert(key_lt(entries[first].kv(), entries[at].kv())); }
                lemma_key_trans(entries[first].kv(), key, entries[at + mid + (last - h)].kv());
            } else {
                assert(built[first] == entries[at + mid + (first - h)]);
                assert(built[last] == entries[at + mid + (last - h)]);
            }
        }
    }
    assert forall|x: E| new.entries().contains(x) <==> built.contains(x) by {
        if built.contains(x) {
            let index = choose|index: int| 0 <= index < built.len() && built[index] == x;
            if index < at {
                assert(entries[index] == x); assert(entries.contains(x)); assert(old.holds(x));
                assert(x.kv() != key) by { assert(key_lt(entries[index].kv(), key)); lemma_key_order(x.kv(), key); }
            } else {
                let t = at + mid + (index - at);
                assert(entries[t] == x); assert(entries.contains(x)); assert(old.holds(x));
                assert(x.kv() != key) by { assert(key_lt(key, entries[t].kv())); lemma_key_order(x.kv(), key); }
            }
            assert(new.holds(x));
        }
        if new.entries().contains(x) {
            assert(new.holds(x));
            assert(old.holds(x) && x.kv() != key);
            assert(entries.contains(x));
            let index = choose|index: int| 0 <= index < entries.len() && entries[index] == x;
            if index < at { assert(built[index] == x); }
            else {
                assert(index >= at + mid) by {
                    if mid == 1 && index == at { assert(x == old.at(key).unwrap()); lemma_at(old, key, x); }
                }
                assert(built[index - mid] == x);
            }
        }
    }
    verus_common::order::lemma_sorted_unique(new.entries(), built, strict_entry_order::<E>());
}

/// The weight total after a put.
pub proof fn lemma_sum_put<E: Ent>(old: Paged<E>, new: Paged<E>, entry: E, weight: spec_fn(E) -> int)
    requires old.wf(), new.wf(),
        forall|x: E| #![trigger new.holds(x)] new.holds(x) <==> (x == entry || (old.holds(x) && x.kv() != entry.kv())),
    ensures verus_common::sums::sum(new.entries(), weight) == verus_common::sums::sum(old.entries(), weight) + weight(entry)
        - match old.at(entry.kv()) { Some(previous) => weight(previous), None => 0int },
{
    lemma_entries_put(old, new, entry);
    lemma_split(old, entry.kv());
    let entries = old.entries();
    let at = below(entries, entry.kv());
    let mid = if old.at(entry.kv()).is_some() { 1int } else { 0int };
    verus_common::sums::lemma_sum_concat(entries.take(at) + seq![entry], entries.skip(at + mid), weight);
    verus_common::sums::lemma_sum_concat(entries.take(at), seq![entry], weight);
    verus_common::sums::lemma_sum_singleton(entry, weight);
    if mid == 1 {
        assert(entries =~= entries.take(at) + seq![entries[at]] + entries.skip(at + 1));
        verus_common::sums::lemma_sum_concat(entries.take(at) + seq![entries[at]], entries.skip(at + 1), weight);
        verus_common::sums::lemma_sum_concat(entries.take(at), seq![entries[at]], weight);
        verus_common::sums::lemma_sum_singleton(entries[at], weight);
    } else {
        assert(entries =~= entries.take(at) + entries.skip(at));
        verus_common::sums::lemma_sum_concat(entries.take(at), entries.skip(at), weight);
    }
}

pub proof fn lemma_sum_remove<E: Ent>(old: Paged<E>, new: Paged<E>, key: KeyView, weight: spec_fn(E) -> int)
    requires old.wf(), new.wf(),
        forall|x: E| #![trigger new.holds(x)] new.holds(x) <==> (old.holds(x) && x.kv() != key),
    ensures verus_common::sums::sum(new.entries(), weight) == verus_common::sums::sum(old.entries(), weight)
        - match old.at(key) { Some(previous) => weight(previous), None => 0int },
{
    lemma_entries_remove(old, new, key);
    lemma_split(old, key);
    let entries = old.entries();
    let at = below(entries, key);
    let mid = if old.at(key).is_some() { 1int } else { 0int };
    verus_common::sums::lemma_sum_concat(entries.take(at), entries.skip(at + mid), weight);
    if mid == 1 {
        assert(entries =~= entries.take(at) + seq![entries[at]] + entries.skip(at + 1));
        verus_common::sums::lemma_sum_concat(entries.take(at) + seq![entries[at]], entries.skip(at + 1), weight);
        verus_common::sums::lemma_sum_concat(entries.take(at), seq![entries[at]], weight);
        verus_common::sums::lemma_sum_singleton(entries[at], weight);
    } else {
        assert(entries =~= entries.take(at) + entries.skip(at));
        verus_common::sums::lemma_sum_concat(entries.take(at), entries.skip(at), weight);
    }
}

/// A weight that vanishes off one key sums to that key's entry alone.
pub proof fn lemma_sum_key<E: Ent>(paged: Paged<E>, key: KeyView, weight: spec_fn(E) -> int)
    requires paged.wf(),
        forall|x: E| #![trigger paged.holds(x)] paged.holds(x) && x.kv() != key ==> weight(x) == 0,
    ensures verus_common::sums::sum(paged.entries(), weight)
        == match paged.at(key) { Some(found) => weight(found), None => 0int },
{
    lemma_split(paged, key);
    paged.model();
    let entries = paged.entries();
    let at = below(entries, key);
    let mid = if paged.at(key).is_some() { 1int } else { 0int };
    assert forall|index: int| #![trigger entries.take(at)[index]] 0 <= index < at implies weight(entries.take(at)[index]) == 0 by {
        assert(entries.take(at)[index] == entries[index]);
        assert(entries.contains(entries[index]));
        assert(paged.holds(entries[index]));
        lemma_key_order(entries[index].kv(), key);
    }
    assert forall|index: int| #![trigger entries.skip(at + mid)[index]] 0 <= index < entries.skip(at + mid).len() implies weight(entries.skip(at + mid)[index]) == 0 by {
        assert(entries.skip(at + mid)[index] == entries[at + mid + index]);
        assert(entries.contains(entries[at + mid + index]));
        assert(paged.holds(entries[at + mid + index]));
        lemma_key_order(entries[at + mid + index].kv(), key);
    }
    verus_common::sums::lemma_sum_zero(entries.take(at), weight);
    verus_common::sums::lemma_sum_zero(entries.skip(at + mid), weight);
    if mid == 1 {
        assert(entries =~= entries.take(at) + seq![entries[at]] + entries.skip(at + 1));
        verus_common::sums::lemma_sum_concat(entries.take(at) + seq![entries[at]], entries.skip(at + 1), weight);
        verus_common::sums::lemma_sum_concat(entries.take(at), seq![entries[at]], weight);
        verus_common::sums::lemma_sum_singleton(entries[at], weight);
    } else {
        assert(entries =~= entries.take(at) + entries.skip(at));
        verus_common::sums::lemma_sum_concat(entries.take(at), entries.skip(at), weight);
    }
}

}
