use std::sync::Arc;
use vstd::prelude::*;
use verus_common::rel::flat;
use verus_prooflib::ordered::{Index, Key, KeyView, key_lt};
use crate::values::{rank_key, rank_view, sentinel_key, sentinel_view, lemma_rank_order, lemma_rank_sentinel};
use verus_prooflib::lemma_flat_empty_at;
use verus_prooflib::wide::Wide;

verus! {

pub const PAGE_LIMIT: usize = 32;

#[derive(Clone, Copy)]
pub struct Ranked {
    pub part: i64,
    pub value: i128,
}

pub struct Page {
    pub rows: Vec<Ranked>,
}

pub type Pages = Index<Page>;

pub open spec fn before(left: Ranked, right: Ranked) -> bool {
    left.value > right.value || (left.value == right.value && left.part < right.part)
}

pub open spec fn rows(pages: Pages) -> Seq<Ranked> {
    flat(pages.entries().map_values(|entry: (KeyView, Page)| entry.1.rows@))
}

pub open spec fn valid(pages: Pages) -> bool {
    &&& pages.wf()
    &&& pages.at(sentinel_view()).is_some()
    &&& forall|fence: KeyView| #[trigger] pages.at(fence).is_some() ==> {
        let page = pages.at(fence).unwrap();
        &&& page.rows.len() <= PAGE_LIMIT
        &&& (fence == sentinel_view() || (i64::MIN <= fence.0 <= i64::MAX && fence.1.len() == 16 && page.rows.len() >= PAGE_LIMIT / 2))
        &&& forall|left: int, right: int| 0 <= left < right < page.rows.len() ==>
            before(#[trigger] page.rows@[left], #[trigger] page.rows@[right])
    }
    &&& forall|page: int, position: int| 0 <= page < pages.entries().len()
        && 0 <= position < pages.entries()[page].1.rows.len() ==> {
        let ranked = #[trigger] pages.entries()[page].1.rows@[position];
        let fence = pages.entries()[page].0;
        &&& !key_lt(fence, rank_view(ranked.value, ranked.part))
        &&& (page == 0 || key_lt(pages.entries()[page - 1].0, rank_view(ranked.value, ranked.part)))
    }
}

pub proof fn lemma_valid_equal(before_pages: &Pages, after_pages: &Pages)
    requires valid(*before_pages), after_pages.wf(), after_pages.entries() == before_pages.entries(),
        forall|key: KeyView| #[trigger] after_pages.at(key) == before_pages.at(key),
    ensures valid(*after_pages), rows(*after_pages) == rows(*before_pages),
{
    assert forall|fence: KeyView| #[trigger] after_pages.at(fence).is_some() implies {
        let page = after_pages.at(fence).unwrap();
        &&& page.rows.len() <= PAGE_LIMIT
        &&& (fence == sentinel_view() || (i64::MIN <= fence.0 <= i64::MAX && fence.1.len() == 16 && page.rows.len() >= PAGE_LIMIT / 2))
        &&& forall|left: int, right: int| 0 <= left < right < page.rows.len() ==>
            before(#[trigger] page.rows@[left], #[trigger] page.rows@[right])
    } by { assert(after_pages.at(fence) == before_pages.at(fence)); }
    assert forall|page: int, position: int| 0 <= page < after_pages.entries().len()
        && 0 <= position < after_pages.entries()[page].1.rows.len() implies {
        let ranked = #[trigger] after_pages.entries()[page].1.rows@[position];
        let fence = after_pages.entries()[page].0;
        &&& !key_lt(fence, rank_view(ranked.value, ranked.part))
        &&& (page == 0 || key_lt(after_pages.entries()[page - 1].0, rank_view(ranked.value, ranked.part)))
    } by { assert(after_pages.entries()[page] == before_pages.entries()[page]); }
}

pub proof fn lemma_rows_sorted(pages: &Pages)
    requires valid(*pages),
    ensures verus_common::order::sorted_by(rows(*pages), |left: Ranked, right: Ranked| before(left, right)),
{
    pages.model();
    let mapped = pages.entries().map_values(|entry: (KeyView, Page)| entry.1.rows@);
    assert forall|page: int, left: int, right: int| 0 <= page < mapped.len() && 0 <= left < right < mapped[page].len()
        implies before(#[trigger] mapped[page][left], #[trigger] mapped[page][right]) by {
        assert(pages.at(pages.entries()[page].0) == Some(pages.entries()[page].1));
    }
    assert forall|earlier: int, later: int, left: int, right: int| 0 <= earlier < later < mapped.len()
        && 0 <= left < mapped[earlier].len() && 0 <= right < mapped[later].len()
        implies before(#[trigger] mapped[earlier][left], #[trigger] mapped[later][right]) by {
        let first = mapped[earlier][left];
        let second = mapped[later][right];
        let first_key = rank_view(first.value, first.part);
        let second_key = rank_view(second.value, second.part);
        let fence = pages.entries()[earlier].0;
        let lower = pages.entries()[later - 1].0;
        assert(first == pages.entries()[earlier].1.rows@[left]);
        assert(second == pages.entries()[later].1.rows@[right]);
        assert(!key_lt(fence, first_key));
        assert(key_lt(lower, second_key));
        if earlier < later - 1 {
            assert(key_lt(fence, lower));
            verus_prooflib::ordered::lemma_key_trans(fence, lower, second_key);
        }
        verus_prooflib::ordered::lemma_key_order(first_key, fence);
        if first_key != fence {
            assert(key_lt(first_key, fence));
            verus_prooflib::ordered::lemma_key_trans(first_key, fence, second_key);
        }
        lemma_rank_order(first.value, second.value, first.part, second.part);
    }
    verus_prooflib::lemma_flat_sorted(mapped, |left: Ranked, right: Ranked| before(left, right));
}

pub proof fn lemma_page_rows(pages: &Pages, page_index: int)
    requires valid(*pages), 0 <= page_index < pages.entries().len(),
    ensures forall|position: int| 0 <= position < pages.entries()[page_index].1.rows.len() ==>
        #[trigger] rows(*pages).contains(pages.entries()[page_index].1.rows@[position]),
{
    assert forall|position: int| 0 <= position < pages.entries()[page_index].1.rows.len() implies
        #[trigger] rows(*pages).contains(pages.entries()[page_index].1.rows@[position]) by {
        let ranked = pages.entries()[page_index].1.rows@[position];
        let mapper = |entry: (KeyView, Page)| entry.1.rows@;
        assert(mapper(pages.entries()[page_index]).contains(ranked));
        assert(exists|offset: int| 0 <= offset < pages.entries().len() && #[trigger] mapper(pages.entries()[offset]).contains(ranked));
        verus_prooflib::lemma_flat_map_contains(pages.entries(), mapper, ranked);
    }
}

pub proof fn lemma_route(pages: &Pages, ranked: Ranked)
    requires valid(*pages),
    ensures {
        let start = verus_prooflib::ordered::lower_position(pages.entries(), rank_view(ranked.value, ranked.part));
        &&& 0 <= start < pages.entries().len()
        &&& rows(*pages).contains(ranked) == pages.entries()[start].1.rows@.contains(ranked)
        &&& fits(*pages, pages.entries()[start].0, seq![ranked])
    },
{
    pages.model();
    let target = rank_view(ranked.value, ranked.part);
    lemma_rank_sentinel(ranked.value, ranked.part);
    verus_prooflib::ordered::lemma_lower_suffix(pages.entries(), target);
    let start = verus_prooflib::ordered::lower_position(pages.entries(), target);
    let sentinel = choose|position: int| 0 <= position < pages.entries().len()
        && (#[trigger] pages.entries()[position]).0 == sentinel_view();
    if sentinel < start {
        assert(key_lt(sentinel_view(), target));
        verus_prooflib::ordered::lemma_key_order(target, sentinel_view());
        assert(false);
    }
    assert(start <= sentinel);
    assert forall|lower: KeyView| (#[trigger] pages.at(lower)).is_some() && key_lt(lower, pages.entries()[start].0) implies key_lt(lower, target) by {
        let position = choose|position: int| 0 <= position < pages.entries().len() && (#[trigger] pages.entries()[position]).0 == lower;
        verus_prooflib::ordered::lemma_key_order(lower, pages.entries()[start].0);
        assert(position < start);
        assert(key_lt(pages.entries()[position].0, target));
    }
    if rows(*pages).contains(ranked) {
        let mapper = |entry: (KeyView, Page)| entry.1.rows@;
        verus_prooflib::lemma_flat_map_contains(pages.entries(), mapper, ranked);
        let page_index = choose|position: int| 0 <= position < pages.entries().len()
            && #[trigger] mapper(pages.entries()[position]).contains(ranked);
        let row_index = choose|position: int| 0 <= position < pages.entries()[page_index].1.rows.len()
            && (#[trigger] pages.entries()[page_index].1.rows@[position]) == ranked;
        assert(!key_lt(pages.entries()[page_index].0, target));
        assert(page_index >= start);
        if page_index > start {
            assert(key_lt(pages.entries()[page_index - 1].0, target));
            assert(!key_lt(pages.entries()[page_index - 1].0, target));
            assert(false);
        }
    }
    if pages.entries()[start].1.rows@.contains(ranked) {
        let position = choose|position: int| 0 <= position < pages.entries()[start].1.rows.len()
            && (#[trigger] pages.entries()[start].1.rows@[position]) == ranked;
        lemma_page_rows(pages, start);
    }
}

pub proof fn lemma_bound_membership(page: Seq<Ranked>, target: Ranked, bound: int)
    requires 0 <= bound <= page.len(),
        forall|left: int, right: int| 0 <= left < right < page.len() ==> before(#[trigger] page[left], #[trigger] page[right]),
        forall|position: int| 0 <= position < bound ==> before(#[trigger] page[position], target),
        forall|position: int| bound <= position < page.len() ==> !before(#[trigger] page[position], target),
    ensures page.contains(target) == (bound < page.len() && page[bound] == target),
{
    if page.contains(target) {
        let position = choose|position: int| 0 <= position < page.len() && #[trigger] page[position] == target;
        assert(position >= bound);
        if position > bound { assert(before(page[bound], target)); }
    }
}

pub open spec fn page_valid(fence: KeyView, items: Seq<Ranked>) -> bool {
    &&& items.len() <= PAGE_LIMIT
    &&& (fence == sentinel_view() || (i64::MIN <= fence.0 <= i64::MAX && fence.1.len() == 16 && items.len() >= PAGE_LIMIT / 2))
    &&& forall|left: int, right: int| 0 <= left < right < items.len() ==> before(#[trigger] items[left], #[trigger] items[right])
}

pub open spec fn fits(pages: Pages, fence: KeyView, items: Seq<Ranked>) -> bool {
    forall|position: int| 0 <= position < items.len() ==> {
        let ranked = #[trigger] items[position];
        &&& !key_lt(fence, rank_view(ranked.value, ranked.part))
        &&& forall|lower: KeyView| (#[trigger] pages.at(lower)).is_some() && key_lt(lower, fence) ==>
            key_lt(lower, rank_view(ranked.value, ranked.part))
    }
}

pub open spec fn lookup_valid(pages: Pages) -> bool {
    &&& pages.wf()
    &&& pages.at(sentinel_view()).is_some()
    &&& forall|fence: KeyView| (#[trigger] pages.at(fence)).is_some() ==>
        page_valid(fence, pages.at(fence).unwrap().rows@) && fits(pages, fence, pages.at(fence).unwrap().rows@)
}

pub proof fn lemma_fit_union(pages: &Pages, fence: KeyView, items: Seq<Ranked>, first: Seq<Ranked>, second: Seq<Ranked>)
    requires fits(*pages, fence, first), fits(*pages, fence, second),
        forall|row: Ranked| #[trigger] items.contains(row) ==> first.contains(row) || second.contains(row),
    ensures fits(*pages, fence, items),
{
    assert forall|position: int| 0 <= position < items.len() implies {
        let ranked = #[trigger] items[position];
        &&& !key_lt(fence, rank_view(ranked.value, ranked.part))
        &&& forall|lower: KeyView| (#[trigger] pages.at(lower)).is_some() && key_lt(lower, fence) ==>
            key_lt(lower, rank_view(ranked.value, ranked.part))
    } by {
        let row = items[position];
        assert(items.contains(row));
        if first.contains(row) {
            let source = choose|source: int| 0 <= source < first.len() && #[trigger] first[source] == row;
        } else {
            let source = choose|source: int| 0 <= source < second.len() && #[trigger] second[source] == row;
        }
    }
}

pub proof fn lemma_adjacent_fit(before_pages: &Pages, after_pages: &Pages, left: KeyView, right: KeyView, items: Seq<Ranked>)
    requires lookup_valid(*before_pages), after_pages.wf(), before_pages.at(left).is_some(), before_pages.at(right).is_some(), key_lt(left, right),
        forall|key: KeyView| (#[trigger] before_pages.at(key)).is_some() && key != left && key_lt(key, right) ==> key_lt(key, left),
        forall|key: KeyView| #[trigger] after_pages.at(key) == if key == left { None } else { before_pages.at(key) },
        forall|row: Ranked| #[trigger] items.contains(row) ==> before_pages.at(left).unwrap().rows@.contains(row) || before_pages.at(right).unwrap().rows@.contains(row),
    ensures fits(*after_pages, right, items),
{
    assert forall|position: int| 0 <= position < items.len() implies {
        let ranked = #[trigger] items[position];
        &&& !key_lt(right, rank_view(ranked.value, ranked.part))
        &&& forall|lower: KeyView| (#[trigger] after_pages.at(lower)).is_some() && key_lt(lower, right) ==>
            key_lt(lower, rank_view(ranked.value, ranked.part))
    } by {
        let ranked = items[position];
        let target = rank_view(ranked.value, ranked.part);
        assert(items.contains(ranked));
        if before_pages.at(right).unwrap().rows@.contains(ranked) {
            let source = choose|source: int| 0 <= source < before_pages.at(right).unwrap().rows.len()
                && (#[trigger] before_pages.at(right).unwrap().rows@[source]) == ranked;
            assert forall|lower: KeyView| (#[trigger] after_pages.at(lower)).is_some() && key_lt(lower, right) implies key_lt(lower, target) by {
                assert(before_pages.at(lower).is_some());
            }
        } else {
            let source = choose|source: int| 0 <= source < before_pages.at(left).unwrap().rows.len()
                && (#[trigger] before_pages.at(left).unwrap().rows@[source]) == ranked;
            assert(!key_lt(left, target));
            verus_prooflib::ordered::lemma_key_order(target, left);
            if target != left { verus_prooflib::ordered::lemma_key_trans(target, left, right); }
            verus_prooflib::ordered::lemma_key_order(target, right);
            assert forall|lower: KeyView| (#[trigger] after_pages.at(lower)).is_some() && key_lt(lower, right) implies key_lt(lower, target) by {
                assert(before_pages.at(lower).is_some());
                assert(lower != left);
                assert(key_lt(lower, left));
            }
        }
    }
}

pub proof fn lemma_lookup_valid(pages: &Pages)
    requires valid(*pages),
    ensures lookup_valid(*pages),
{
    pages.model();
    assert forall|fence: KeyView| (#[trigger] pages.at(fence)).is_some() implies
        page_valid(fence, pages.at(fence).unwrap().rows@) && fits(*pages, fence, pages.at(fence).unwrap().rows@) by {
        let page_index = choose|position: int| 0 <= position < pages.entries().len() && (#[trigger] pages.entries()[position]).0 == fence;
        let items = pages.at(fence).unwrap().rows@;
        assert forall|position: int| 0 <= position < items.len() implies {
            let ranked = #[trigger] items[position];
            &&& !key_lt(fence, rank_view(ranked.value, ranked.part))
            &&& forall|lower: KeyView| (#[trigger] pages.at(lower)).is_some() && key_lt(lower, fence) ==>
                key_lt(lower, rank_view(ranked.value, ranked.part))
        } by {
            assert(items[position] == pages.entries()[page_index].1.rows@[position]);
            let target = rank_view(items[position].value, items[position].part);
            assert forall|lower: KeyView| (#[trigger] pages.at(lower)).is_some() && key_lt(lower, fence) implies key_lt(lower, target) by {
                let earlier = choose|offset: int| 0 <= offset < pages.entries().len() && (#[trigger] pages.entries()[offset]).0 == lower;
                verus_prooflib::ordered::lemma_key_order(lower, fence);
                assert(earlier < page_index);
                assert(key_lt(pages.entries()[page_index - 1].0, target));
                if earlier < page_index - 1 {
                    assert(key_lt(lower, pages.entries()[page_index - 1].0));
                    verus_prooflib::ordered::lemma_key_trans(lower, pages.entries()[page_index - 1].0, target);
                }
            }
        }
    }
}

pub proof fn lemma_indexed_valid(pages: &Pages)
    requires lookup_valid(*pages),
    ensures valid(*pages),
{
    pages.model();
    assert forall|fence: KeyView| #[trigger] pages.at(fence).is_some() implies {
        let page = pages.at(fence).unwrap();
        &&& page.rows.len() <= PAGE_LIMIT
        &&& (fence == sentinel_view() || (i64::MIN <= fence.0 <= i64::MAX && fence.1.len() == 16 && page.rows.len() >= PAGE_LIMIT / 2))
        &&& forall|left: int, right: int| 0 <= left < right < page.rows.len() ==> before(#[trigger] page.rows@[left], #[trigger] page.rows@[right])
    } by { assert(page_valid(fence, pages.at(fence).unwrap().rows@)); }
    assert forall|page: int, position: int| 0 <= page < pages.entries().len() && 0 <= position < pages.entries()[page].1.rows.len() implies {
        let ranked = #[trigger] pages.entries()[page].1.rows@[position];
        let fence = pages.entries()[page].0;
        &&& !key_lt(fence, rank_view(ranked.value, ranked.part))
        &&& (page == 0 || key_lt(pages.entries()[page - 1].0, rank_view(ranked.value, ranked.part)))
    } by {
        let fence = pages.entries()[page].0;
        assert(pages.at(fence) == Some(pages.entries()[page].1));
        assert(fits(*pages, fence, pages.entries()[page].1.rows@));
        if page > 0 { assert(pages.at(pages.entries()[page - 1].0).is_some()); }
    }
}

pub open spec fn outside(pages: Pages, excluded: KeyView, row: Ranked) -> bool {
    exists|fence: KeyView| fence != excluded && (#[trigger] pages.at(fence)).is_some()
        && #[trigger] pages.at(fence).unwrap().rows@.contains(row)
}

pub proof fn lemma_rows_lookup(pages: &Pages, row: Ranked)
    requires pages.wf(),
    ensures rows(*pages).contains(row) <==> exists|fence: KeyView| (#[trigger] pages.at(fence)).is_some()
        && #[trigger] pages.at(fence).unwrap().rows@.contains(row),
{
    pages.model();
    let mapper = |entry: (KeyView, Page)| entry.1.rows@;
    verus_prooflib::lemma_flat_map_contains(pages.entries(), mapper, row);
    if rows(*pages).contains(row) {
        let position = choose|position: int| 0 <= position < pages.entries().len() && #[trigger] mapper(pages.entries()[position]).contains(row);
        let fence = pages.entries()[position].0;
        assert(pages.at(fence) == Some(pages.entries()[position].1));
        assert(mapper(pages.entries()[position]) == pages.entries()[position].1.rows@);
        assert(pages.at(fence).unwrap().rows@.contains(row));
        assert(exists|fence: KeyView| (#[trigger] pages.at(fence)).is_some() && #[trigger] pages.at(fence).unwrap().rows@.contains(row));
    }
    if exists|fence: KeyView| (#[trigger] pages.at(fence)).is_some() && #[trigger] pages.at(fence).unwrap().rows@.contains(row) {
        let fence = choose|fence: KeyView| (#[trigger] pages.at(fence)).is_some() && #[trigger] pages.at(fence).unwrap().rows@.contains(row);
        let position = choose|position: int| 0 <= position < pages.entries().len() && (#[trigger] pages.entries()[position]).0 == fence;
        assert(mapper(pages.entries()[position]).contains(row));
        assert(exists|position: int| 0 <= position < pages.entries().len() && #[trigger] mapper(pages.entries()[position]).contains(row));
        verus_prooflib::lemma_flat_map_contains(pages.entries(), mapper, row);
    }
}

pub proof fn lemma_partition(pages: &Pages, fence: KeyView, row: Ranked)
    requires pages.wf(), pages.at(fence).is_some(),
    ensures rows(*pages).contains(row) == (pages.at(fence).unwrap().rows@.contains(row) || outside(*pages, fence, row)),
{
    lemma_rows_lookup(pages, row);
    if rows(*pages).contains(row) {
        let key = choose|key: KeyView| (#[trigger] pages.at(key)).is_some() && #[trigger] pages.at(key).unwrap().rows@.contains(row);
        if key != fence { assert(outside(*pages, fence, row)); }
    }
    if pages.at(fence).unwrap().rows@.contains(row) {
        assert(exists|key: KeyView| (#[trigger] pages.at(key)).is_some() && #[trigger] pages.at(key).unwrap().rows@.contains(row));
    }
    if outside(*pages, fence, row) {
        let key = choose|key: KeyView| key != fence && (#[trigger] pages.at(key)).is_some() && #[trigger] pages.at(key).unwrap().rows@.contains(row);
        assert(exists|key: KeyView| (#[trigger] pages.at(key)).is_some() && #[trigger] pages.at(key).unwrap().rows@.contains(row));
    }
}

pub proof fn lemma_final_sentinel(pages: &Pages)
    requires valid(*pages),
    ensures pages.entries().len() > 0, pages.entries().last().0 == sentinel_view(),
        forall|fence: KeyView| (#[trigger] pages.at(fence)).is_some() && fence != sentinel_view() ==> key_lt(fence, sentinel_view()),
{
    pages.model();
    assert forall|fence: KeyView| (#[trigger] pages.at(fence)).is_some() && fence != sentinel_view() implies key_lt(fence, sentinel_view()) by {
        assert(fence.0 <= i64::MAX);
        assert(fence.1.len() == 16);
        verus_prooflib::lemma_bytes_max_sentinel(fence.1, 17);
    }
    let position = choose|position: int| 0 <= position < pages.entries().len() && (#[trigger] pages.entries()[position]).0 == sentinel_view();
    if position < pages.entries().len() - 1 {
        let last = pages.entries().last().0;
        assert(pages.at(last).is_some());
        assert(key_lt(sentinel_view(), last));
        verus_prooflib::ordered::lemma_key_order(last, sentinel_view());
        assert(last != sentinel_view());
        assert(key_lt(last, sentinel_view()));
        verus_prooflib::ordered::lemma_key_order(last, sentinel_view());
    }
}

pub proof fn lemma_route_exclusive(pages: &Pages, ranked: Ranked)
    requires valid(*pages),
    ensures !outside(*pages, pages.entries()[verus_prooflib::ordered::lower_position(pages.entries(), rank_view(ranked.value, ranked.part))].0, ranked),
{
    lemma_route(pages, ranked);
    lemma_lookup_valid(pages);
    pages.model();
    let target = rank_view(ranked.value, ranked.part);
    let start = verus_prooflib::ordered::lower_position(pages.entries(), target);
    let fence = pages.entries()[start].0;
    if outside(*pages, fence, ranked) {
        let other = choose|other: KeyView| other != fence && (#[trigger] pages.at(other)).is_some() && #[trigger] pages.at(other).unwrap().rows@.contains(ranked);
        let position = choose|position: int| 0 <= position < pages.entries().len() && (#[trigger] pages.entries()[position]).0 == other;
        let offset = choose|offset: int| 0 <= offset < pages.at(other).unwrap().rows.len() && (#[trigger] pages.at(other).unwrap().rows@[offset]) == ranked;
        verus_prooflib::ordered::lemma_lower_suffix(pages.entries(), target);
        assert(!key_lt(other, target));
        assert(position >= start);
        if position > start {
            assert(key_lt(pages.entries()[position - 1].0, target));
            assert(!key_lt(pages.entries()[position - 1].0, target));
        }
        assert(other == fence);
    }
}

pub proof fn lemma_replace_lookup(before_pages: &Pages, after_pages: &Pages, fence: KeyView, replacement: Page)
    requires lookup_valid(*before_pages), after_pages.wf(), before_pages.at(fence).is_some(),
        page_valid(fence, replacement.rows@), fits(*before_pages, fence, replacement.rows@),
        forall|query: KeyView| #[trigger] after_pages.at(query) == if query == fence { Some(replacement) } else { before_pages.at(query) },
    ensures lookup_valid(*after_pages),
{
    assert forall|query: KeyView| (#[trigger] after_pages.at(query)).is_some() implies
        page_valid(query, after_pages.at(query).unwrap().rows@) && fits(*after_pages, query, after_pages.at(query).unwrap().rows@) by {
        let items = after_pages.at(query).unwrap().rows@;
        assert(fits(*before_pages, query, items));
        assert forall|position: int| 0 <= position < items.len() implies {
            let ranked = #[trigger] items[position];
            &&& !key_lt(query, rank_view(ranked.value, ranked.part))
            &&& forall|lower: KeyView| (#[trigger] after_pages.at(lower)).is_some() && key_lt(lower, query) ==>
                key_lt(lower, rank_view(ranked.value, ranked.part))
        } by {
            assert forall|lower: KeyView| (#[trigger] after_pages.at(lower)).is_some() && key_lt(lower, query) implies
                key_lt(lower, rank_view(items[position].value, items[position].part)) by {
                assert(before_pages.at(lower).is_some());
            }
        }
    }
}

pub proof fn lemma_store_updates(before_pages: &Pages, after_pages: &Pages, fence: KeyView, items: Seq<Ranked>, lower: Page, upper: Page)
    requires lookup_valid(*before_pages), after_pages.wf(), before_pages.at(fence).is_some(),
        items.len() <= 2 * PAGE_LIMIT, fits(*before_pages, fence, items),
        fence == sentinel_view() || items.len() >= PAGE_LIMIT / 2,
        forall|first: int, last: int| 0 <= first < last < items.len() ==> before(#[trigger] items[first], #[trigger] items[last]),
        if items.len() <= PAGE_LIMIT {
            &&& upper.rows@ == items
            &&& forall|key: KeyView| #[trigger] after_pages.at(key) == if key == fence { Some(upper) } else { before_pages.at(key) }
        } else {
            let middle = (items.len() / 2) as int;
            let last = items[middle - 1];
            let split = rank_view(last.value, last.part);
            &&& lower.rows@ == items.take(middle)
            &&& upper.rows@ == items.skip(middle)
            &&& forall|key: KeyView| #[trigger] after_pages.at(key) == if key == fence { Some(upper) }
                else if key == split { Some(lower) } else { before_pages.at(key) }
        },
    ensures valid(*after_pages),
        forall|row: Ranked| #[trigger] rows(*after_pages).contains(row) <==> items.contains(row) || outside(*before_pages, fence, row),
{
    if items.len() <= PAGE_LIMIT {
        assert(page_valid(fence, items));
        lemma_replace_lookup(before_pages, after_pages, fence, upper);
    } else {
        let middle = (items.len() / 2) as int;
        let last = items[middle - 1];
        let split = rank_view(last.value, last.part);
        let first_upper = items[middle];
        let next_rank = rank_view(first_upper.value, first_upper.part);
        lemma_rank_order(last.value, first_upper.value, last.part, first_upper.part);
        assert(key_lt(split, next_rank));
        assert(!key_lt(fence, next_rank));
        verus_prooflib::ordered::lemma_key_order(next_rank, fence);
        if next_rank != fence { verus_prooflib::ordered::lemma_key_trans(split, next_rank, fence); }
        assert(key_lt(split, fence));
        lemma_rank_sentinel(last.value, last.part);
        if before_pages.at(split).is_some() {
            assert(key_lt(split, rank_view(last.value, last.part)));
            verus_prooflib::ordered::lemma_key_order(split, split);
            assert(false);
        }
        assert(page_valid(split, lower.rows@));
        assert(page_valid(fence, upper.rows@));
        assert forall|query: KeyView| (#[trigger] after_pages.at(query)).is_some() implies
            page_valid(query, after_pages.at(query).unwrap().rows@) && fits(*after_pages, query, after_pages.at(query).unwrap().rows@) by {
            let contents = after_pages.at(query).unwrap().rows@;
            assert(page_valid(query, contents));
            assert forall|position: int| 0 <= position < contents.len() implies {
                let ranked = #[trigger] contents[position];
                &&& !key_lt(query, rank_view(ranked.value, ranked.part))
                &&& forall|lower_fence: KeyView| (#[trigger] after_pages.at(lower_fence)).is_some() && key_lt(lower_fence, query) ==>
                    key_lt(lower_fence, rank_view(ranked.value, ranked.part))
            } by {
                let ranked = contents[position];
                let target = rank_view(ranked.value, ranked.part);
                if query == split {
                    assert(ranked == items[position]);
                    if position < middle - 1 { lemma_rank_order(ranked.value, last.value, ranked.part, last.part); }
                    verus_prooflib::ordered::lemma_key_order(split, target);
                } else if query == fence {
                    assert(ranked == items[middle + position]);
                } else { assert(fits(*before_pages, query, contents)); }
                assert forall|lower_fence: KeyView| (#[trigger] after_pages.at(lower_fence)).is_some() && key_lt(lower_fence, query) implies
                    key_lt(lower_fence, target) by {
                    if lower_fence == split {
                        if query == fence {
                            assert(before(last, ranked));
                            lemma_rank_order(last.value, ranked.value, last.part, ranked.part);
                        } else {
                            assert(before_pages.at(query).is_some());
                            verus_prooflib::ordered::lemma_key_order(query, fence);
                            if key_lt(query, fence) {
                                assert(key_lt(query, split));
                                verus_prooflib::ordered::lemma_key_order(split, query);
                                assert(false);
                            }
                            assert(key_lt(fence, query));
                            assert(key_lt(fence, target));
                            verus_prooflib::ordered::lemma_key_trans(split, fence, target);
                        }
                    } else {
                        assert(before_pages.at(lower_fence).is_some());
                        if query == split {
                            verus_prooflib::ordered::lemma_key_trans(lower_fence, split, fence);
                            assert(key_lt(lower_fence, fence));
                            assert(key_lt(lower_fence, target));
                        } else if query == fence { assert(key_lt(lower_fence, target)); }
                        else { assert(key_lt(lower_fence, target)); }
                    }
                }
            }
        }
        assert(after_pages.at(sentinel_view()).is_some());
    }
    lemma_indexed_valid(after_pages);
    assert forall|row: Ranked| #[trigger] rows(*after_pages).contains(row) <==> items.contains(row) || outside(*before_pages, fence, row) by {
        lemma_rows_lookup(before_pages, row);
        lemma_rows_lookup(after_pages, row);
        if rows(*after_pages).contains(row) {
            let key = choose|key: KeyView| (#[trigger] after_pages.at(key)).is_some() && #[trigger] after_pages.at(key).unwrap().rows@.contains(row);
            if key != fence {
                if items.len() > PAGE_LIMIT && key == rank_view(items[(items.len() / 2) as int - 1].value, items[(items.len() / 2) as int - 1].part) {
                    assert(lower.rows@.contains(row));
                } else {
                    assert(before_pages.at(key).unwrap().rows@.contains(row));
                    assert(outside(*before_pages, fence, row));
                }
            }
        }
        if outside(*before_pages, fence, row) {
            let key = choose|key: KeyView| key != fence && (#[trigger] before_pages.at(key)).is_some() && #[trigger] before_pages.at(key).unwrap().rows@.contains(row);
            if items.len() > PAGE_LIMIT {
                let last = items[(items.len() / 2) as int - 1];
                let split = rank_view(last.value, last.part);
                assert(before_pages.at(split).is_none());
                assert(key != split);
            }
            assert(after_pages.at(key).unwrap().rows@.contains(row));
            assert(rows(*after_pages).contains(row));
        }
        if items.contains(row) {
            let position = choose|position: int| 0 <= position < items.len() && #[trigger] items[position] == row;
            let key = if items.len() > PAGE_LIMIT && position < items.len() / 2 {
                let last = items[(items.len() / 2) as int - 1]; rank_view(last.value, last.part)
            } else { fence };
            assert(after_pages.at(key).is_some());
            if items.len() <= PAGE_LIMIT {
                assert(after_pages.at(key).unwrap().rows@[position] == row);
            } else if position < items.len() / 2 {
                assert(after_pages.at(key).unwrap().rows@[position] == row);
            } else {
                assert(after_pages.at(key).unwrap().rows@[position - (items.len() / 2) as int] == row);
            }
            assert(after_pages.at(key).unwrap().rows@.contains(row));
            assert(rows(*after_pages).contains(row));
        }
    }
}

pub fn new() -> (result: Pages)
    ensures valid(result), rows(result).len() == 0,
{
    let mut result = Index::new();
    let page = Page { rows: Vec::new() };
    let ghost empty = page;
    result.put(sentinel_key(), Arc::new(page));
    proof {
        result.model();
        assert forall|fence: KeyView| #[trigger] result.at(fence).is_some() implies
            fence == sentinel_view() && result.at(fence).unwrap().rows@.len() == 0 by {}
        assert forall|position: int| 0 <= position < result.entries().len() implies
            (#[trigger] result.entries()[position]).1.rows@ == Seq::<Ranked>::empty() by {
            assert(result.at(result.entries()[position].0) == Some(result.entries()[position].1));
        }
        let mapped = result.entries().map_values(|entry: (KeyView, Page)| entry.1.rows@);
        assert forall|position: int| 0 <= position < mapped.len() implies
            (#[trigger] mapped[position]).len() == 0 by {}
        lemma_flat_empty_at(mapped);
    }
    result
}

pub fn precedes(left: Ranked, right: Ranked) -> (result: bool)
    ensures result == before(left, right),
{
    left.value > right.value || (left.value == right.value && left.part < right.part)
}

pub fn lower_bound(page: &Vec<Ranked>, target: Ranked) -> (result: usize)
    requires forall|left: int, right: int| 0 <= left < right < page.len() ==>
        before(#[trigger] page@[left], #[trigger] page@[right]),
    ensures result <= page.len(),
        forall|position: int| 0 <= position < result ==> before(#[trigger] page@[position], target),
        forall|position: int| result <= position < page.len() ==> !before(#[trigger] page@[position], target),
{
    let mut low: usize = 0;
    let mut high = page.len();
    while low < high
        invariant
            low <= high <= page.len(),
            forall|left: int, right: int| 0 <= left < right < page.len() ==>
                before(#[trigger] page@[left], #[trigger] page@[right]),
            forall|position: int| 0 <= position < low ==> before(#[trigger] page@[position], target),
            forall|position: int| high <= position < page.len() ==> !before(#[trigger] page@[position], target),
        decreases high - low,
    {
        let middle = low + (high - low) / 2;
        if precedes(page[middle], target) { low = middle + 1; }
        else { high = middle; }
    }
    low
}

pub fn copy_range(source: &Vec<Ranked>, start: usize, end: usize) -> (result: Vec<Ranked>)
    requires start <= end <= source.len(),
    ensures result@ == source@.subrange(start as int, end as int),
{
    let mut result = Vec::new();
    let mut position = start;
    while position < end
        invariant start <= position <= end <= source.len(), result@ == source@.subrange(start as int, position as int),
        decreases end - position,
    {
        result.push(source[position]);
        position += 1;
    }
    result
}

pub fn store(pages: &mut Pages, fence: Key, items: Vec<Ranked>)
    requires lookup_valid(*old(pages)), old(pages).at(fence.view()).is_some(),
        items.len() <= 2 * PAGE_LIMIT, fits(*old(pages), fence.view(), items@),
        fence.view() == sentinel_view() || items.len() >= PAGE_LIMIT / 2,
        forall|first: int, last: int| 0 <= first < last < items.len() ==> before(#[trigger] items@[first], #[trigger] items@[last]),
    ensures valid(*final(pages)),
        forall|row: Ranked| #[trigger] rows(*final(pages)).contains(row) <==> items@.contains(row) || outside(*old(pages), fence.view(), row),
{
    let ghost initial = *pages;
    let ghost original_items = items@;
    let ghost original_fence = fence.view();
    if items.len() <= PAGE_LIMIT {
        let page = Page { rows: items };
        let ghost replacement = page;
        pages.put(fence, Arc::new(page));
        proof { lemma_store_updates(&initial, &*pages, original_fence, original_items, replacement, replacement); }
    } else {
        let middle = items.len() / 2;
        let lower = copy_range(&items, 0, middle);
        let upper = copy_range(&items, middle, items.len());
        let last = lower[lower.len() - 1];
        let lower_page = Page { rows: lower };
        let upper_page = Page { rows: upper };
        let ghost low = lower_page;
        let ghost high = upper_page;
        pages.put(rank_key(last.value, last.part), Arc::new(lower_page));
        pages.put(fence, Arc::new(upper_page));
        proof { lemma_store_updates(&initial, &*pages, original_fence, original_items, low, high); }
    }
}

pub fn insert(pages: &mut Pages, ranked: Ranked) -> (result: bool)
    requires valid(*old(pages)),
    ensures valid(*final(pages)),
        result == !rows(*old(pages)).contains(ranked),
        result ==> forall|row: Ranked| #[trigger] rows(*final(pages)).contains(row)
            <==> rows(*old(pages)).contains(row) || row == ranked,
{
    let ghost original = *pages;
    let target = rank_key(ranked.value, ranked.part);
    proof { lemma_route(&*pages, ranked); lemma_lookup_valid(&*pages); }
    let mut cursor = pages.cursor_from(&target);
    let (fence, page) = match cursor.next() { Some(entry) => entry, None => return false };
    proof {
        pages.model();
        let start = verus_prooflib::ordered::lower_position(pages.entries(), target.view());
        assert((fence.view(), *page) == pages.entries()[start]);
        assert(pages.at(fence.view()) == Some(*page));
    }
    let position = lower_bound(&page.rows, ranked);
    proof { lemma_bound_membership(page.rows@, ranked, position as int); }
    if position < page.rows.len() && page.rows[position].part == ranked.part && page.rows[position].value == ranked.value {
        return false;
    }
    let mut items = copy_range(&page.rows, 0, page.rows.len());
    proof {
        assert(items@ =~= page.rows@);
        verus_prooflib::lemma_sequence_insert_members(page.rows@, position as int, ranked);
    }
    items.insert(position, ranked);
    proof {
        assert(items@ == page.rows@.insert(position as int, ranked));
        let single = seq![ranked];
        assert(single[0] == ranked);
        assert(single.contains(single[0]));
        assert(single.contains(ranked));
        lemma_fit_union(&*pages, fence.view(), items@, page.rows@, seq![ranked]);
    }
    store(pages, fence, items);
    proof {
        assert forall|row: Ranked| #[trigger] rows(*pages).contains(row) <==> rows(original).contains(row) || row == ranked by {
            lemma_partition(&original, fence.view(), row);
            lemma_rows_lookup(&original, row);
            if rows(original).contains(row) && !page.rows@.contains(row) {
                let key = choose|key: KeyView| (#[trigger] original.at(key)).is_some() && #[trigger] original.at(key).unwrap().rows@.contains(row);
                assert(key != fence.view());
                assert(outside(original, fence.view(), row));
            }
            if outside(original, fence.view(), row) {
                let key = choose|key: KeyView| key != fence.view() && (#[trigger] original.at(key)).is_some() && #[trigger] original.at(key).unwrap().rows@.contains(row);
                assert(rows(original).contains(row));
            }
        }
    }
    true
}

pub fn remove(pages: &mut Pages, ranked: Ranked) -> (result: bool)
    requires valid(*old(pages)),
    ensures valid(*final(pages)),
        result == rows(*old(pages)).contains(ranked),
        result ==> forall|row: Ranked| #[trigger] rows(*final(pages)).contains(row)
            <==> rows(*old(pages)).contains(row) && row != ranked,
{
    let ghost original = *pages;
    let target = rank_key(ranked.value, ranked.part);
    proof { lemma_route(&*pages, ranked); lemma_lookup_valid(&*pages); }
    let mut cursor = pages.cursor_from(&target);
    let (fence, page) = match cursor.next() { Some(entry) => entry, None => return false };
    proof {
        pages.model();
        let start = verus_prooflib::ordered::lower_position(pages.entries(), target.view());
        assert((fence.view(), *page) == pages.entries()[start]);
        assert(pages.at(fence.view()) == Some(*page));
    }
    let position = lower_bound(&page.rows, ranked);
    proof { lemma_bound_membership(page.rows@, ranked, position as int); }
    if position >= page.rows.len() || page.rows[position].part != ranked.part || page.rows[position].value != ranked.value {
        return false;
    }
    let mut items = copy_range(&page.rows, 0, page.rows.len());
    proof {
        assert(items@ =~= page.rows@);
        assert(page.rows@.no_duplicates()) by {
            assert forall|left: int, right: int| 0 <= left < page.rows.len() && 0 <= right < page.rows.len() && left != right implies
                (#[trigger] page.rows@[left]) != (#[trigger] page.rows@[right]) by {
                if left < right { assert(before(page.rows@[left], page.rows@[right])); }
                else { assert(before(page.rows@[right], page.rows@[left])); }
            }
        }
        verus_prooflib::lemma_sequence_remove_members(page.rows@, position as int);
        lemma_route_exclusive(&*pages, ranked);
    }
    items.remove(position);
    proof {
        assert(items@ == page.rows@.remove(position as int));
        lemma_fit_union(&*pages, fence.view(), items@, page.rows@, Seq::<Ranked>::empty());
    }
    if items.len() >= PAGE_LIMIT / 2 || fence.text.len() == 17 {
        store(pages, fence, items);
        proof {
            assert forall|row: Ranked| #[trigger] rows(*pages).contains(row) <==> rows(original).contains(row) && row != ranked by {
                lemma_partition(&original, fence.view(), row);
                if row == ranked { assert(!outside(original, fence.view(), row)); }
            }
        }
        return true;
    }
    let ghost fence_index = verus_prooflib::ordered::lower_position(pages.entries(), fence.view());
    proof {
        lemma_final_sentinel(&*pages);
        verus_prooflib::ordered::lemma_lower_suffix(pages.entries(), fence.view());
        assert(fence.view() != sentinel_view());
        assert(pages.entries()[fence_index].0 == fence.view());
        assert(fence_index < pages.entries().len() - 1);
    }
    let mut successor = pages.cursor_from(&fence);
    successor.next();
    let (next_fence, next_page) = match successor.next() {
        Some(entry) => entry,
        None => {
            proof {
                assert(successor.remaining() == pages.entries().skip(fence_index + 1));
                assert(false);
            }
            store(pages, fence, items);
            return true;
        },
    };
    proof {
        assert((next_fence.view(), *next_page) == pages.entries()[fence_index + 1]);
        assert forall|key: KeyView| (#[trigger] pages.at(key)).is_some() && key != fence.view() && key_lt(key, next_fence.view()) implies key_lt(key, fence.view()) by {
            let offset = choose|offset: int| 0 <= offset < pages.entries().len() && (#[trigger] pages.entries()[offset]).0 == key;
            verus_prooflib::ordered::lemma_key_order(key, next_fence.view());
            assert(offset < fence_index + 1);
            assert(offset != fence_index);
        }
    }
    let ghost initial = items@;
    let ghost removed_offset = position as int;
    let mut position: usize = 0;
    while position < next_page.rows.len()
        invariant position <= next_page.rows.len(), items@ == initial + next_page.rows@.take(position as int),
        decreases next_page.rows.len() - position,
    {
        items.push(next_page.rows[position]);
        position += 1;
    }
    pages.remove(&fence);
    let ghost intermediate = *pages;
    let ghost combined = items@;
    proof {
        assert(next_page.rows@.take(next_page.rows.len() as int) =~= next_page.rows@);
        assert(items@ == initial + next_page.rows@);
        assert forall|row: Ranked| #[trigger] items@.contains(row) implies page.rows@.contains(row) || next_page.rows@.contains(row) by {
            let offset = choose|offset: int| 0 <= offset < items.len() && #[trigger] items@[offset] == row;
            if offset < initial.len() {
                assert(initial[offset] == row);
                assert(initial.contains(row));
            } else { assert(next_page.rows@[offset - initial.len()] == row); }
        }
        lemma_adjacent_fit(&original, &*pages, fence.view(), next_fence.view(), items@);
        assert forall|first: int, last: int| 0 <= first < last < items.len() implies before(#[trigger] items@[first], #[trigger] items@[last]) by {
            let left_row = items@[first];
            let right_row = items@[last];
            if last < initial.len() {
                let left_source = if first < removed_offset { first } else { first + 1 };
                let right_source = if last < removed_offset { last } else { last + 1 };
                assert(left_row == page.rows@[left_source]);
                assert(right_row == page.rows@[right_source]);
                assert(before(left_row, right_row));
            } else if first >= initial.len() {
                assert(left_row == next_page.rows@[first - initial.len()]);
                assert(right_row == next_page.rows@[last - initial.len()]);
                assert(before(left_row, right_row));
            } else {
                let left_source = if first < removed_offset { first } else { first + 1 };
                assert(left_row == page.rows@[left_source]);
                assert(right_row == next_page.rows@[last - initial.len()]);
                let left_rank = rank_view(left_row.value, left_row.part);
                let right_rank = rank_view(right_row.value, right_row.part);
                assert(!key_lt(fence.view(), left_rank));
                assert(key_lt(fence.view(), right_rank));
                verus_prooflib::ordered::lemma_key_order(left_rank, fence.view());
                if left_rank != fence.view() { verus_prooflib::ordered::lemma_key_trans(left_rank, fence.view(), right_rank); }
                lemma_rank_order(left_row.value, right_row.value, left_row.part, right_row.part);
            }
        }
    }
    store(pages, next_fence, items);
    proof {
        lemma_route_exclusive(&original, ranked);
        assert(!next_page.rows@.contains(ranked)) by {
            if next_page.rows@.contains(ranked) { assert(outside(original, fence.view(), ranked)); }
        }
        assert forall|row: Ranked| #[trigger] combined.contains(row) <==> initial.contains(row) || next_page.rows@.contains(row) by {
            if combined.contains(row) {
                let offset = choose|offset: int| 0 <= offset < combined.len() && #[trigger] combined[offset] == row;
                if offset < initial.len() { assert(initial[offset] == row); }
                else { assert(next_page.rows@[offset - initial.len()] == row); }
            }
            if initial.contains(row) {
                let offset = choose|offset: int| 0 <= offset < initial.len() && #[trigger] initial[offset] == row;
                assert(combined[offset] == row);
            }
            if next_page.rows@.contains(row) {
                let offset = choose|offset: int| 0 <= offset < next_page.rows.len() && (#[trigger] next_page.rows@[offset]) == row;
                assert(combined[initial.len() as int + offset] == row);
            }
        }
        assert forall|row: Ranked| #[trigger] rows(*pages).contains(row) <==> rows(original).contains(row) && row != ranked by {
            lemma_rows_lookup(&original, row);
            if rows(*pages).contains(row) {
                if combined.contains(row) {
                    if initial.contains(row) { assert(page.rows@.contains(row)); }
                    else { assert(next_page.rows@.contains(row)); }
                    assert(rows(original).contains(row));
                    assert(row != ranked);
                } else {
                    let key = choose|key: KeyView| key != next_fence.view() && (#[trigger] intermediate.at(key)).is_some()
                        && #[trigger] intermediate.at(key).unwrap().rows@.contains(row);
                    assert(key != fence.view());
                    assert(original.at(key).unwrap().rows@.contains(row));
                    assert(rows(original).contains(row));
                    if row == ranked { assert(outside(original, fence.view(), row)); }
                }
            }
            if rows(original).contains(row) && row != ranked {
                let key = choose|key: KeyView| (#[trigger] original.at(key)).is_some() && #[trigger] original.at(key).unwrap().rows@.contains(row);
                if key == fence.view() { assert(initial.contains(row)); assert(combined.contains(row)); }
                else if key == next_fence.view() { assert(next_page.rows@.contains(row)); assert(combined.contains(row)); }
                else {
                    assert(intermediate.at(key).unwrap().rows@.contains(row));
                    assert(outside(intermediate, next_fence.view(), row));
                }
            }
        }
    }
    true
}

pub open spec fn passes(value: int, limit: int) -> bool {
    1000000000000 * value > limit
}

pub fn passes_exec(value: i128, limit: &Wide) -> (result: bool)
    ensures result == passes(value as int, limit@),
{
    let scaled = Wide::from_i128(value).mul(&Wide::from_i128(1000000000000i128));
    limit.lt(&scaled)
}

pub fn qualifying_prefix(page: &Vec<Ranked>, limit: &Wide) -> (result: usize)
    requires forall|left: int, right: int| 0 <= left < right < page.len() ==>
        before(#[trigger] page@[left], #[trigger] page@[right]),
    ensures result <= page.len(),
        forall|position: int| 0 <= position < result ==> passes((#[trigger] page@[position]).value as int, limit@),
        forall|position: int| result <= position < page.len() ==> !passes((#[trigger] page@[position]).value as int, limit@),
{
    let mut low: usize = 0;
    let mut high = page.len();
    while low < high
        invariant
            low <= high <= page.len(),
            forall|left: int, right: int| 0 <= left < right < page.len() ==>
                before(#[trigger] page@[left], #[trigger] page@[right]),
            forall|position: int| 0 <= position < low ==> passes((#[trigger] page@[position]).value as int, limit@),
            forall|position: int| high <= position < page.len() ==> !passes((#[trigger] page@[position]).value as int, limit@),
        decreases high - low,
    {
        let middle = low + (high - low) / 2;
        if passes_exec(page[middle].value, limit) {
            proof {
                assert forall|position: int| 0 <= position < middle + 1 implies passes((#[trigger] page@[position]).value as int, limit@) by {
                    if position < middle { assert(before(page@[position], page@[middle as int])); }
                }
            }
            low = middle + 1;
        } else {
            proof {
                assert forall|position: int| middle <= position < page.len() implies !passes((#[trigger] page@[position]).value as int, limit@) by {
                    if position > middle { assert(before(page@[middle as int], page@[position])); }
                }
            }
            high = middle;
        }
    }
    low
}

}
