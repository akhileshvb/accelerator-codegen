use vstd::prelude::*;
use std::sync::Arc;
use verus_prooflib::ordered::KeyView;
use crate::model::*;
use crate::encoding::*;
use verus_common::sums::*;
use verus_prooflib::ordered::Index;

verus! {

pub open spec fn group_stream_key(group: &Group, count: int, brand: Option<Seq<u8>>) -> KeyView {
    stream_key(*group, count, brand)
}

pub proof fn lemma_ranks_positive(ranks: Ranks, counts: Counts)
    requires ranks_valid(ranks, counts),
    ensures positive_counts(counts),
{
}

pub proof fn lemma_empty_ranks(ranks: Ranks, counts: Counts)
    requires ranks.wf(), ranks.entries().len() == 0, counts.wf(), counts.entries().len() == 0,
    ensures ranks_valid(ranks, counts),
{
    ranks.model();
    counts.model();
    assert forall|key: KeyView| #[trigger] ranks.at(key).is_some() implies false by {}
    assert forall|key: KeyView| #[trigger] counts.at(key).is_some() implies false by {}
}

pub proof fn lemma_empty_stream(rows: Counts, counts: Counts, size: int, brand: Option<Seq<u8>>)
    requires rows.wf(), rows.entries().len() == 0,
        forall|key: KeyView| #[trigger] counts.at(key).is_some() ==> !stream_matches(*counts.at(key).unwrap().group, size, brand),
    ensures stream_valid(rows, counts, size, brand),
{
    rows.model();
    assert forall|key: KeyView| #[trigger] rows.at(key).is_some() implies false by {}
}

pub proof fn lemma_empty_brands(brands: Index<BrandRanks>, counts: Counts, size: int)
    requires brands.wf(), brands.entries().len() == 0,
        forall|key: KeyView| #[trigger] counts.at(key).is_some() ==> counts.at(key).unwrap().group.size != size,
    ensures brands_valid(brands, counts, size),
{
    brands.model();
    assert forall|key: KeyView| #[trigger] brands.at(key).is_some() implies false by {}
}

pub proof fn lemma_counts_step_positive(before: Counts, after: Counts, group: Arc<Group>, count: i64)
    requires positive_counts(before), after.wf(), group.valid(), count >= 0, count_step(before, after, group, count),
    ensures positive_counts(after),
{
    assert forall|key: KeyView| #[trigger] after.at(key).is_some() implies
        after.at(key).unwrap().count > 0 && after.at(key).unwrap().group.valid() && after.at(key).unwrap().group.key() == key && after.at(key).unwrap().consistent() by {
        if key == group.key() { assert(count > 0); }
        else { assert(before.at(key).is_some()); }
    }
}

pub open spec fn count_step(before: Counts, after: Counts, group: Arc<Group>, count: i64) -> bool {
    forall|key: KeyView| #[trigger] after.at(key) == if key == group.key() {
        if count > 0 { Some(Cell::of(group, count)) } else { None }
    } else { before.at(key) }
}

pub proof fn lemma_stream_equal(before: &Counts, after: &Counts, counts: Counts, size: int, brand: Option<Seq<u8>>)
    requires stream_valid(*before, counts, size, brand), after.wf(),
        forall|key: KeyView| #[trigger] after.at(key) == before.at(key),
    ensures stream_valid(*after, counts, size, brand),
{
    assert forall|key: KeyView| #[trigger] after.at(key).is_some() implies {
        let cell = after.at(key).unwrap();
        stream_matches(*cell.group, size, brand) && key == stream_key(*cell.group, cell.count as int, brand)
            && counts.at(cell.group.key()) == after.at(key)
    } by { assert(before.at(key).is_some()); }
    assert forall|key: KeyView| #[trigger] counts.at(key).is_some()
        && stream_matches(*counts.at(key).unwrap().group, size, brand) implies
        after.at(stream_key(*counts.at(key).unwrap().group, counts.at(key).unwrap().count as int, brand)) == counts.at(key) by {
        assert(before.at(stream_key(*counts.at(key).unwrap().group, counts.at(key).unwrap().count as int, brand)) == counts.at(key));
    }
}

pub proof fn lemma_brands_equal(before: &Index<BrandRanks>, after: &Index<BrandRanks>, counts: Counts, size: int)
    requires brands_valid(*before, counts, size), after.wf(),
        forall|key: KeyView| #[trigger] after.at(key) == before.at(key),
    ensures brands_valid(*after, counts, size),
{
    assert forall|brand: KeyView| #[trigger] after.at(brand).is_some() implies
        brand.0 == 0 && stream_valid(after.at(brand).unwrap().rows, counts, size, Some(brand.1))
        && after.at(brand).unwrap().groups == after.at(brand).unwrap().rows.entries().len()
        && after.at(brand).unwrap().groups > 0 by { assert(before.at(brand).is_some()); }
    assert forall|key: KeyView| #[trigger] counts.at(key).is_some() && counts.at(key).unwrap().group.size == size implies
        after.at((0int, (*counts.at(key).unwrap().group.brand)@)).is_some() by {
        assert(before.at((0int, (*counts.at(key).unwrap().group.brand)@)).is_some());
    }
}

pub proof fn lemma_stream_counts_equal(rows: Counts, before: Counts, after: Counts, size: int, brand: Option<Seq<u8>>)
    requires stream_valid(rows, before, size, brand),
        forall|key: KeyView| #[trigger] after.at(key) == before.at(key),
    ensures stream_valid(rows, after, size, brand),
{
    assert forall|key: KeyView| #[trigger] rows.at(key).is_some() implies {
        let cell = rows.at(key).unwrap();
        stream_matches(*cell.group, size, brand) && key == stream_key(*cell.group, cell.count as int, brand)
            && after.at(cell.group.key()) == rows.at(key)
    } by { assert(before.at(rows.at(key).unwrap().group.key()) == rows.at(key)); }
    assert forall|key: KeyView| #[trigger] after.at(key).is_some()
        && stream_matches(*after.at(key).unwrap().group, size, brand) implies
        rows.at(stream_key(*after.at(key).unwrap().group, after.at(key).unwrap().count as int, brand)) == after.at(key) by {
        assert(before.at(key).is_some());
    }
}

pub proof fn lemma_size_counts_equal(rows: SizeRanks, before: Counts, after: Counts, size: int)
    requires size_valid(rows, before, size),
        forall|key: KeyView| #[trigger] after.at(key) == before.at(key),
    ensures size_valid(rows, after, size),
{
    lemma_stream_counts_equal(rows.rows, before, after, size, None);
    assert forall|brand: KeyView| #[trigger] rows.brands.at(brand).is_some() implies
        brand.0 == 0 && stream_valid(rows.brands.at(brand).unwrap().rows, after, size, Some(brand.1))
        && rows.brands.at(brand).unwrap().groups == rows.brands.at(brand).unwrap().rows.entries().len()
        && rows.brands.at(brand).unwrap().groups > 0 by {
        lemma_stream_counts_equal(rows.brands.at(brand).unwrap().rows, before, after, size, Some(brand.1));
    }
    assert forall|key: KeyView| #[trigger] after.at(key).is_some() && after.at(key).unwrap().group.size == size implies
        rows.brands.at((0int, (*after.at(key).unwrap().group.brand)@)).is_some() by {
        assert(before.at(key).is_some());
    }
}

pub proof fn lemma_ranks_equal(before: &Ranks, after: &Ranks, old_counts: Counts, counts: Counts)
    requires ranks_valid(*before, old_counts), after.wf(), positive_counts(counts),
        forall|key: KeyView| #[trigger] after.at(key) == before.at(key),
        forall|key: KeyView| #[trigger] counts.at(key) == old_counts.at(key),
    ensures ranks_valid(*after, counts),
{
    assert forall|size: KeyView| #[trigger] after.at(size).is_some() implies
        size.1 == Seq::<u8>::empty() && size_valid(after.at(size).unwrap(), counts, size.0) by {
        assert(before.at(size).is_some());
        lemma_size_counts_equal(before.at(size).unwrap(), old_counts, counts, size.0);
    }
    assert forall|key: KeyView| #[trigger] counts.at(key).is_some() implies
        after.at((counts.at(key).unwrap().group.size as int, Seq::empty())).is_some() by {
        assert(old_counts.at(key).is_some());
        assert(before.at((counts.at(key).unwrap().group.size as int, Seq::empty())).is_some());
    }
}

pub open spec fn stream_step(before: Counts, after: Counts, group: Arc<Group>, previous: i64, next: i64, brand: Option<Seq<u8>>) -> bool {
    forall|key: KeyView| #[trigger] after.at(key) ==
        if next > 0 && key == stream_key(*group, next as int, brand) { Some(Cell::of(group, next)) }
        else if previous > 0 && key == stream_key(*group, previous as int, brand) { None }
        else { before.at(key) }
}

pub proof fn lemma_rank_remove(before: &Counts, after: &Counts, key: KeyView)
    requires before.wf(), after.wf(), after.view() == before.view().remove(key),
    ensures after.entries().len() as int == before.entries().len() as int - if before.at(key).is_some() { 1int } else { 0int },
{
    verus_prooflib::ordered::lemma_sum_remove(before, after, key, |row: (KeyView, Cell)| 1int);
    verus_prooflib::lemma_sum_ones(before.entries());
    verus_prooflib::lemma_sum_ones(after.entries());
}

pub proof fn lemma_rank_put(before: &Counts, after: &Counts, key: KeyView, cell: &Cell)
    requires before.wf(), after.wf(), after.view() == before.view().insert(key, *cell),
    ensures after.entries().len() as int == before.entries().len() as int + if before.at(key).is_none() { 1int } else { 0int },
{
    verus_prooflib::ordered::lemma_sum_put(before, after, key, *cell, |row: (KeyView, Cell)| 1int);
    verus_prooflib::lemma_sum_ones(before.entries());
    verus_prooflib::lemma_sum_ones(after.entries());
}

pub proof fn lemma_rank_present(rows: Counts, counts: Counts, group: Arc<Group>, size: int, brand: Option<Seq<u8>>, count: i64)
    requires stream_valid(rows, counts, size, brand), positive_counts(counts), group.valid(),
        stream_matches(*group, size, brand), count_at(counts, group.key()) == count as int, count > 0,
    ensures rows.at(stream_key(*group, count as int, brand)).is_some(),
{
    let ghost cell = counts.at(group.key()).unwrap();
    lemma_group_identity(&*cell.group, &*group);
    assert(cell.count == count);
    assert(cell.group.valid());
    assert(cell.group.order.view().1 == group.order.view().1);
    assert(rows.at(stream_key(*cell.group, cell.count as int, brand)) == Some(cell));
}

pub proof fn lemma_rank_fresh(rows: Counts, counts: Counts, group: Arc<Group>, size: int, brand: Option<Seq<u8>>, previous: i64, next: i64)
    requires stream_valid(rows, counts, size, brand), positive_counts(counts), group.valid(),
        stream_matches(*group, size, brand), count_at(counts, group.key()) == previous as int,
        previous != next,
    ensures rows.at(stream_key(*group, next as int, brand)).is_none(),
{
    let ghost rank = group_stream_key(&*group, next as int, brand);
    if rows.at(rank).is_some() {
        let cell = rows.at(rank).unwrap();
        assert(counts.at(cell.group.key()) == Some(cell));
        lemma_stream_collision(cell, group, size, brand, next as int);
        assert(cell.count as int == next as int);
        assert(false);
    }
}

pub proof fn lemma_stream_collision(cell: Cell, group: Arc<Group>, size: int, brand: Option<Seq<u8>>, count: int)
    requires cell.group.valid(), group.valid(), stream_matches(*cell.group, size, brand),
        stream_matches(*group, size, brand), stream_key(*cell.group, cell.count as int, brand) == stream_key(*group, count, brand),
    ensures cell.group.key() == group.key(),
{
    match brand {
        Some(bytes) => {},
        None => { lemma_group_order_identity(&*cell.group, &*group); },
    }
}

pub proof fn lemma_stream_update(before: Counts, after: Counts, old_counts: Counts, counts: Counts,
    group: Arc<Group>, previous: i64, next: i64, size: int, brand: Option<Seq<u8>>)
    requires stream_valid(before, old_counts, size, brand), positive_counts(old_counts), positive_counts(counts),
        group.valid(), stream_matches(*group, size, brand), previous >= 0, next >= 0,
        count_at(old_counts, group.key()) == previous as int, count_step(old_counts, counts, group, next),
        after.wf(), stream_step(before, after, group, previous, next, brand),
    ensures stream_valid(after, counts, size, brand),
{
    assert forall|key: KeyView| #[trigger] after.at(key).is_some() implies {
        let cell = after.at(key).unwrap();
        stream_matches(*cell.group, size, brand) && key == stream_key(*cell.group, cell.count as int, brand)
            && counts.at(cell.group.key()) == after.at(key)
    } by {
        let cell = after.at(key).unwrap();
        if next > 0 && key == stream_key(*group, next as int, brand) {
            assert(cell == Cell::of(group, next));
        } else {
            assert(before.at(key) == Some(cell));
            assert(old_counts.at(cell.group.key()) == Some(cell));
            assert(cell.group.valid());
            if cell.group.key() == group.key() {
                lemma_group_identity(&*cell.group, &*group);
                assert(cell.count == previous);
                assert(previous > 0);
                assert(key == stream_key(*group, previous as int, brand));
                assert(false);
            }
            assert(counts.at(cell.group.key()) == Some(cell));
        }
    }
    assert forall|key: KeyView| #[trigger] counts.at(key).is_some()
        && stream_matches(*counts.at(key).unwrap().group, size, brand) implies
        after.at(stream_key(*counts.at(key).unwrap().group, counts.at(key).unwrap().count as int, brand)) == counts.at(key) by {
        let cell = counts.at(key).unwrap();
        if key == group.key() {
            assert(next > 0);
            assert(cell == Cell::of(group, next));
        } else {
            assert(old_counts.at(key) == Some(cell));
            assert(cell.group.key() == key && cell.group.valid());
            let rank = stream_key(*cell.group, cell.count as int, brand);
            assert(before.at(rank) == Some(cell));
            if rank == stream_key(*group, previous as int, brand) {
                lemma_stream_collision(cell, group, size, brand, previous as int);
                assert(false);
            }
            if rank == stream_key(*group, next as int, brand) {
                lemma_stream_collision(cell, group, size, brand, next as int);
                assert(false);
            }
            assert(after.at(rank) == Some(cell));
        }
    }
}

pub proof fn lemma_stream_unchanged(rows: Counts, old_counts: Counts, counts: Counts,
    group: Arc<Group>, next: i64, size: int, brand: Option<Seq<u8>>)
    requires stream_valid(rows, old_counts, size, brand), positive_counts(old_counts), positive_counts(counts),
        group.valid(), !stream_matches(*group, size, brand), next >= 0, count_step(old_counts, counts, group, next),
    ensures stream_valid(rows, counts, size, brand),
{
    assert forall|key: KeyView| #[trigger] rows.at(key).is_some() implies {
        let cell = rows.at(key).unwrap();
        stream_matches(*cell.group, size, brand) && key == stream_key(*cell.group, cell.count as int, brand)
            && counts.at(cell.group.key()) == rows.at(key)
    } by {
        let cell = rows.at(key).unwrap();
        assert(old_counts.at(cell.group.key()) == Some(cell));
        if cell.group.key() == group.key() {
            lemma_group_identity(&*cell.group, &*group);
            assert(false);
        }
    }
    assert forall|key: KeyView| #[trigger] counts.at(key).is_some()
        && stream_matches(*counts.at(key).unwrap().group, size, brand) implies
        rows.at(stream_key(*counts.at(key).unwrap().group, counts.at(key).unwrap().count as int, brand)) == counts.at(key) by {
        if key == group.key() {
            assert(next > 0);
            assert(counts.at(key).unwrap() == Cell::of(group, next));
            assert(false);
        }
        assert(old_counts.at(key) == counts.at(key));
    }
}

pub open spec fn brand_step(before: Index<BrandRanks>, after: Index<BrandRanks>, group: Arc<Group>, stream: Counts, groups: i64) -> bool {
    forall|key: KeyView| #[trigger] after.at(key) == if key == (0int, (*group.brand)@) {
        if stream.entries().len() > 0 { Some(BrandRanks { rows: stream, groups }) } else { None }
    } else { before.at(key) }
}

pub proof fn lemma_brands_update(before: Index<BrandRanks>, after: Index<BrandRanks>, old_counts: Counts, counts: Counts,
    group: Arc<Group>, next: i64, stream: Counts, groups: i64)
    requires brands_valid(before, old_counts, group.size as int), positive_counts(old_counts), positive_counts(counts),
        group.valid(), next >= 0, count_step(old_counts, counts, group, next),
        stream_valid(stream, counts, group.size as int, Some((*group.brand)@)), groups == stream.entries().len(),
        after.wf(), brand_step(before, after, group, stream, groups),
    ensures brands_valid(after, counts, group.size as int),
{
    stream.model();
    assert forall|brand: KeyView| #[trigger] after.at(brand).is_some() implies
        brand.0 == 0 && stream_valid(after.at(brand).unwrap().rows, counts, group.size as int, Some(brand.1))
        && after.at(brand).unwrap().groups == after.at(brand).unwrap().rows.entries().len()
        && after.at(brand).unwrap().groups > 0 by {
        if brand == (0int, (*group.brand)@) {
            assert(stream.entries().len() > 0);
        } else {
            assert(before.at(brand).is_some());
            assert(brand.0 == 0);
            lemma_stream_unchanged(before.at(brand).unwrap().rows, old_counts, counts, group, next, group.size as int, Some(brand.1));
        }
    }
    assert forall|key: KeyView| #[trigger] counts.at(key).is_some() && counts.at(key).unwrap().group.size == group.size implies
        after.at((0int, (*counts.at(key).unwrap().group.brand)@)).is_some() by {
        let cell = counts.at(key).unwrap();
        let brand = (0int, (*cell.group.brand)@);
        if brand == (0int, (*group.brand)@) {
            let rank = stream_key(*cell.group, cell.count as int, Some((*group.brand)@));
            assert(stream.at(rank) == Some(cell));
            assert(stream.entries().len() > 0);
        } else {
            if key == group.key() {
                assert(cell == Cell::of(group, next));
                assert(false);
            }
            assert(old_counts.at(key) == Some(cell));
            assert(before.at(brand).is_some());
            assert(after.at(brand).is_some());
        }
    }
}

pub proof fn lemma_profile_nonempty(rows: SizeRanks, counts: Counts, size: int)
    requires size_profile_valid(rows, counts, size), positive_counts(counts),
    ensures (rows.brands.entries().len() == 0) == (rows.rows.entries().len() == 0),
{
    rows.brands.model();
    rows.rows.model();
    if rows.brands.entries().len() > 0 {
        let brand = rows.brands.entries()[0].0;
        let stream = rows.brands.entries()[0].1.rows;
        assert(rows.brands.at(brand).is_some());
        assert(stream_valid(stream, counts, size, Some(brand.1)));
        assert(stream.entries().len() > 0);
        stream.model();
        let rank = stream.entries()[0].0;
        let cell = stream.entries()[0].1;
        assert(stream.at(rank) == Some(cell));
        assert(counts.at(cell.group.key()) == Some(cell));
        assert(rows.rows.at(stream_key(*cell.group, cell.count as int, None)).is_some());
        assert(rows.rows.entries().len() > 0);
    }
    if rows.rows.entries().len() > 0 {
        let rank = rows.rows.entries()[0].0;
        let cell = rows.rows.entries()[0].1;
        assert(rows.rows.at(rank) == Some(cell));
        assert(counts.at(cell.group.key()) == Some(cell));
        assert(rows.brands.at((0int, (*cell.group.brand)@)).is_some());
        assert(rows.brands.entries().len() > 0);
    }
}

pub proof fn lemma_size_unchanged(rows: SizeRanks, old_counts: Counts, counts: Counts, group: Arc<Group>, next: i64, size: int)
    requires size_valid(rows, old_counts, size), positive_counts(old_counts), positive_counts(counts),
        group.valid(), next >= 0, group.size != size, count_step(old_counts, counts, group, next),
    ensures size_valid(rows, counts, size),
{
    lemma_stream_unchanged(rows.rows, old_counts, counts, group, next, size, None);
    assert forall|brand: KeyView| #[trigger] rows.brands.at(brand).is_some() implies
        brand.0 == 0 && stream_valid(rows.brands.at(brand).unwrap().rows, counts, size, Some(brand.1))
        && rows.brands.at(brand).unwrap().groups == rows.brands.at(brand).unwrap().rows.entries().len()
        && rows.brands.at(brand).unwrap().groups > 0 by {
        lemma_stream_unchanged(rows.brands.at(brand).unwrap().rows, old_counts, counts, group, next, size, Some(brand.1));
    }
    assert forall|key: KeyView| #[trigger] counts.at(key).is_some() && counts.at(key).unwrap().group.size == size implies
        rows.brands.at((0int, (*counts.at(key).unwrap().group.brand)@)).is_some() by {
        if key == group.key() {
            assert(counts.at(key).unwrap() == Cell::of(group, next));
            assert(false);
        }
        assert(old_counts.at(key) == counts.at(key));
    }
}

pub open spec fn rank_step(before: Ranks, after: Ranks, size: int, rows: SizeRanks) -> bool {
    forall|key: KeyView| #[trigger] after.at(key) == if key == (size, Seq::<u8>::empty()) {
        if rows.brands.entries().len() > 0 { Some(rows) } else { None }
    } else { before.at(key) }
}

pub proof fn lemma_ranks_update(before: Ranks, after: Ranks, old_counts: Counts, counts: Counts, group: Arc<Group>, next: i64, rows: SizeRanks)
    requires ranks_valid(before, old_counts), positive_counts(counts), group.valid(), next >= 0,
        count_step(old_counts, counts, group, next), size_profile_valid(rows, counts, group.size as int),
        after.wf(), rank_step(before, after, group.size as int, rows),
    ensures ranks_valid(after, counts),
{
    lemma_profile_nonempty(rows, counts, group.size as int);
    assert forall|size: KeyView| #[trigger] after.at(size).is_some() implies
        size.1 == Seq::<u8>::empty() && size_valid(after.at(size).unwrap(), counts, size.0) by {
        if size == (group.size as int, Seq::<u8>::empty()) {
            assert(rows.brands.entries().len() > 0);
            assert(rows.groups > 0);
        } else {
            assert(before.at(size).is_some());
            assert(size.1 == Seq::<u8>::empty());
            assert(size.0 != group.size as int);
            lemma_size_unchanged(before.at(size).unwrap(), old_counts, counts, group, next, size.0);
        }
    }
    assert forall|key: KeyView| #[trigger] counts.at(key).is_some() implies
        after.at((counts.at(key).unwrap().group.size as int, Seq::empty())).is_some() by {
        let cell = counts.at(key).unwrap();
        if cell.group.size == group.size {
            rows.rows.model();
            assert(rows.rows.at(stream_key(*cell.group, cell.count as int, None)).is_some());
            assert(rows.rows.entries().len() > 0);
            assert(rows.brands.entries().len() > 0);
        } else {
            if key == group.key() {
                assert(cell == Cell::of(group, next));
                assert(false);
            }
            assert(old_counts.at(key) == Some(cell));
            assert(before.at((cell.group.size as int, Seq::empty())).is_some());
        }
    }
}

pub proof fn lemma_rank_lookup(ranks: Ranks, counts: Counts, size: int, brand: Seq<u8>, count: int, kind: Seq<u8>)
    requires ranks_valid(ranks, counts),
    ensures rank_at(ranks, size, brand, count, kind) ==
        if count > 0 && count_at(counts, group_key(brand, kind, size)) == count { Some(count as i64) } else { None },
{
    let key = group_key(brand, kind, size);
    if count > 0 && count_at(counts, key) == count {
        let cell = counts.at(key).unwrap();
        lemma_group_text_identity((*cell.group.brand)@, (*cell.group.kind)@, brand, kind);
        assert(cell.group.size == size);
        let rows = ranks.at((size, Seq::empty())).unwrap();
        assert(size_valid(rows, counts, size));
        assert(rows.brands.at((0int, brand)).is_some());
        assert(stream_valid(rows.brands.at((0int, brand)).unwrap().rows, counts, size, Some(brand)));
        assert(rows.brands.at((0int, brand)).unwrap().rows.at((-count, kind)) == Some(cell));
    } else if ranks.at((size, Seq::empty())).is_some() {
        let rows = ranks.at((size, Seq::empty())).unwrap();
        if rows.brands.at((0int, brand)).is_some() {
            let stream = rows.brands.at((0int, brand)).unwrap().rows;
            if stream.at((-count, kind)).is_some() {
                let cell = stream.at((-count, kind)).unwrap();
                assert(counts.at(cell.group.key()) == Some(cell));
                assert(cell.group.key() == key);
                assert(cell.count as int == count);
                assert(false);
            }
        }
    }
}

}
