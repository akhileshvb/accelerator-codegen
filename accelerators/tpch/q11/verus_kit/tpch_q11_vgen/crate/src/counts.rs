use vstd::prelude::*;
use verus_prooflib::wide::Wide;
use verus_common::sums::{sum, lemma_sum_take};
use verus_prooflib::ordered::{Index, Key, KeyView};
use crate::model::*;

verus! {

pub open spec fn inner_at(groups: Index<Wide>, key: KeyView) -> int {
    match groups.at(key) { Some(value) => value@, None => 0 }
}

#[verifier::inline]
pub open spec fn group_sum(rows: Seq<(KeyView, Wide)>, query: KeyView, inner: KeyView, outer: KeyView) -> int {
    sum(rows, |row: (KeyView, Wide)| if query == outer && row.0 == inner { row.1@ } else { 0 })
}

#[verifier::inline]
pub open spec fn pairs_sum(rows: Seq<(KeyView, Index<Wide>)>, outer: KeyView, inner: KeyView) -> int {
    sum(rows, |row: (KeyView, Index<Wide>)| if row.0 == outer { inner_at(row.1, inner) } else { 0 })
}

pub fn apply_group(forward: &mut Counts, reverse: &mut Counts, outer: &Key, changes: &Index<Wide>, number: bool)
    requires nonzero_counts(*old(forward)), nonzero_counts(*old(reverse)),
        counts_layout(*old(forward), number, true), counts_layout(*old(reverse), true, number),
        key_class(outer.view(), number), changes.wf(),
        forall|key: KeyView| (#[trigger] changes.at(key)).is_some() ==> numeric(key),
    ensures nonzero_counts(*final(forward)), nonzero_counts(*final(reverse)),
        forall|key: KeyView| key != outer.view() ==> (#[trigger] final(forward).at(key)) == old(forward).at(key),
        counts_layout(*final(forward), number, true), counts_layout(*final(reverse), true, number),
        forall|query: KeyView, inner: KeyView| #[trigger] count_at(*final(forward), query, inner)
            == count_at(*old(forward), query, inner) + if query == outer.view() { inner_at(*changes, inner) } else { 0 },
        forall|query: KeyView, inner: KeyView| #[trigger] count_at(*final(reverse), inner, query)
            == count_at(*old(reverse), inner, query) + if query == outer.view() { inner_at(*changes, inner) } else { 0 },
{
    let ghost initial_forward = *forward;
    let ghost initial_reverse = *reverse;
    let mut cursor = changes.cursor();
    let mut processed = Ghost(0int);
    loop
        invariant initial_forward == *old(forward), initial_reverse == *old(reverse),
            forall|key: KeyView| key != outer.view() ==> (#[trigger] forward.at(key)) == initial_forward.at(key),
            nonzero_counts(*forward), nonzero_counts(*reverse),
            counts_layout(*forward, number, true), counts_layout(*reverse, true, number),
            key_class(outer.view(), number), changes.wf(),
            forall|key: KeyView| (#[trigger] changes.at(key)).is_some() ==> numeric(key),
            0 <= processed@ <= changes.entries().len(), cursor.remaining() == changes.entries().skip(processed@),
            forall|query: KeyView, inner: KeyView| #[trigger] count_at(*forward, query, inner)
                == count_at(initial_forward, query, inner) + group_sum(changes.entries().take(processed@), query, inner, outer.view()),
            forall|query: KeyView, inner: KeyView| #[trigger] count_at(*reverse, inner, query)
                == count_at(initial_reverse, inner, query) + group_sum(changes.entries().take(processed@), query, inner, outer.view()),
        decreases cursor.remaining().len(),
    {
        let (inner, change) = match cursor.next() {
            Some(row) => row,
            None => {
                proof {
                    assert(processed@ == changes.entries().len()); assert(changes.entries().take(processed@) =~= changes.entries());
                    assert forall|query: KeyView, inner: KeyView| #[trigger] count_at(*forward, query, inner)
                        == count_at(initial_forward, query, inner) + if query == outer.view() { inner_at(*changes, inner) } else { 0 } by {
                        assert(count_at(*forward, query, inner) == count_at(initial_forward, query, inner) + sum(changes.entries().take(processed@), |row: (KeyView, Wide)| if query == outer.view() && row.0 == inner { row.1@ } else { 0 }));
                        let weight = |row: (KeyView, Wide)| if query == outer.view() && row.0 == inner { row.1@ } else { 0 };
                        lemma_finish(*changes, query, inner, outer.view(), weight);
                        assert(count_at(*forward, query, inner) == count_at(initial_forward, query, inner) + sum(changes.entries(), weight));
                    }
                    assert forall|query: KeyView, inner: KeyView| #[trigger] count_at(*reverse, inner, query)
                        == count_at(initial_reverse, inner, query) + if query == outer.view() { inner_at(*changes, inner) } else { 0 } by {
                        assert(count_at(*reverse, inner, query) == count_at(initial_reverse, inner, query) + sum(changes.entries().take(processed@), |row: (KeyView, Wide)| if query == outer.view() && row.0 == inner { row.1@ } else { 0 }));
                        let weight = |row: (KeyView, Wide)| if query == outer.view() && row.0 == inner { row.1@ } else { 0 };
                        lemma_finish(*changes, query, inner, outer.view(), weight);
                        assert(count_at(*reverse, inner, query) == count_at(initial_reverse, inner, query) + sum(changes.entries(), weight));
                    }
                }
                return;
            },
        };
        proof { changes.model(); assert(changes.at(inner.view()) == Some(*change)); assert(numeric(inner.view())); }
        let ghost previous_forward = *forward;
        let ghost previous_reverse = *reverse;
        add_count(forward, outer, &inner, &*change);
        add_count(reverse, &inner, outer, &*change);
        proof {
            assert forall|query: KeyView, key: KeyView| #[trigger] count_at(*forward, query, key)
                == count_at(initial_forward, query, key) + sum(changes.entries().take(processed@ + 1), |row: (KeyView, Wide)|
                    if query == outer.view() && row.0 == key { row.1@ } else { 0 }) by {
                lemma_sum_take(changes.entries(), processed@, |row: (KeyView, Wide)| if query == outer.view() && row.0 == key { row.1@ } else { 0 });
                assert(count_at(previous_forward, query, key) == count_at(initial_forward, query, key) + sum(changes.entries().take(processed@), |row: (KeyView, Wide)| if query == outer.view() && row.0 == key { row.1@ } else { 0 }));
            }
            assert forall|query: KeyView, key: KeyView| #[trigger] count_at(*reverse, key, query)
                == count_at(initial_reverse, key, query) + sum(changes.entries().take(processed@ + 1), |row: (KeyView, Wide)|
                    if query == outer.view() && row.0 == key { row.1@ } else { 0 }) by {
                lemma_sum_take(changes.entries(), processed@, |row: (KeyView, Wide)| if query == outer.view() && row.0 == key { row.1@ } else { 0 });
                assert(count_at(previous_reverse, key, query) == count_at(initial_reverse, key, query) + sum(changes.entries().take(processed@), |row: (KeyView, Wide)| if query == outer.view() && row.0 == key { row.1@ } else { 0 }));
            }
        }
        processed = Ghost(processed@ + 1);
    }
}

pub proof fn lemma_finish(changes: Index<Wide>, query: KeyView, inner: KeyView, outer: KeyView, selected: spec_fn((KeyView, Wide)) -> int)
    requires changes.wf(), forall|row: (KeyView, Wide)| #[trigger] selected(row) == if query == outer && row.0 == inner { row.1@ } else { 0 },
    ensures sum(changes.entries(), selected)
        == if query == outer { inner_at(changes, inner) } else { 0 },
{
    let weight = |row: (KeyView, Wide)| if query == outer { row.1@ } else { 0 };
    verus_prooflib::ordered::lemma_key_contribution(&changes, inner, weight);
    assert((|row: (KeyView, Wide)| if query == outer && row.0 == inner { row.1@ } else { 0 })
        =~= (|row: (KeyView, Wide)| if row.0 == inner { weight(row) } else { 0 }));
    reveal(verus_prooflib::ordered::contribution);
    if changes.at(inner).is_none() { assert(inner_at(changes, inner) == 0); }
    reveal(group_sum);
    verus_common::sums::lemma_sum_ext_all(changes.entries(),
        |row: (KeyView, Wide)| if query == outer && row.0 == inner { row.1@ } else { 0 },
        |row: (KeyView, Wide)| if row.0 == inner { weight(row) } else { 0 });
    assert(group_sum(changes.entries(), query, inner, outer) == sum(changes.entries(), |row: (KeyView, Wide)| if query == outer && row.0 == inner { row.1@ } else { 0 }));
    verus_common::sums::lemma_sum_ext_all(changes.entries(), selected, |row: (KeyView, Wide)| if row.0 == inner { weight(row) } else { 0 });
}

pub fn apply_pairs(forward: &mut Counts, reverse: &mut Counts, changes: &Counts, number: bool)
    requires nonzero_counts(*old(forward)), nonzero_counts(*old(reverse)),
        counts_layout(*old(forward), number, true), counts_layout(*old(reverse), true, number), counts_layout(*changes, number, true),
    ensures nonzero_counts(*final(forward)), nonzero_counts(*final(reverse)),
        forall|key: KeyView| (#[trigger] changes.at(key)).is_none() ==> final(forward).at(key) == old(forward).at(key),
        counts_layout(*final(forward), number, true), counts_layout(*final(reverse), true, number),
        forall|outer: KeyView, inner: KeyView| #[trigger] count_at(*final(forward), outer, inner)
            == count_at(*old(forward), outer, inner) + count_at(*changes, outer, inner),
        forall|outer: KeyView, inner: KeyView| #[trigger] count_at(*final(reverse), inner, outer)
            == count_at(*old(reverse), inner, outer) + count_at(*changes, outer, inner),
{
    let ghost initial_forward = *forward;
    let ghost initial_reverse = *reverse;
    let mut cursor = changes.cursor();
    let mut processed = Ghost(0int);
    loop
        invariant initial_forward == *old(forward), initial_reverse == *old(reverse),
            forall|key: KeyView| (#[trigger] changes.at(key)).is_none() ==> forward.at(key) == initial_forward.at(key),
            nonzero_counts(*forward), nonzero_counts(*reverse),
            counts_layout(*forward, number, true), counts_layout(*reverse, true, number), counts_layout(*changes, number, true),
            0 <= processed@ <= changes.entries().len(), cursor.remaining() == changes.entries().skip(processed@),
            forall|outer: KeyView, inner: KeyView| #[trigger] count_at(*forward, outer, inner)
                == count_at(initial_forward, outer, inner) + pairs_sum(changes.entries().take(processed@), outer, inner),
            forall|outer: KeyView, inner: KeyView| #[trigger] count_at(*reverse, inner, outer)
                == count_at(initial_reverse, inner, outer) + pairs_sum(changes.entries().take(processed@), outer, inner),
        decreases cursor.remaining().len(),
    {
        let (outer, groups) = match cursor.next() {
            Some(row) => row,
            None => {
                proof {
                    assert(processed@ == changes.entries().len()); assert(changes.entries().take(processed@) =~= changes.entries());
                    assert forall|outer: KeyView, inner: KeyView| #[trigger] count_at(*forward, outer, inner)
                        == count_at(initial_forward, outer, inner) + count_at(*changes, outer, inner) by {
                        assert(count_at(*forward, outer, inner) == count_at(initial_forward, outer, inner) + sum(changes.entries().take(processed@), |row: (KeyView, Index<Wide>)| if row.0 == outer { inner_at(row.1, inner) } else { 0 }));
                        let weight = |row: (KeyView, Index<Wide>)| if row.0 == outer { inner_at(row.1, inner) } else { 0 };
                        lemma_pairs_finish(*changes, outer, inner, weight);
                        assert(count_at(*forward, outer, inner) == count_at(initial_forward, outer, inner) + sum(changes.entries(), weight));
                    }
                    assert forall|outer: KeyView, inner: KeyView| #[trigger] count_at(*reverse, inner, outer)
                        == count_at(initial_reverse, inner, outer) + count_at(*changes, outer, inner) by {
                        assert(count_at(*reverse, inner, outer) == count_at(initial_reverse, inner, outer) + sum(changes.entries().take(processed@), |row: (KeyView, Index<Wide>)| if row.0 == outer { inner_at(row.1, inner) } else { 0 }));
                        let weight = |row: (KeyView, Index<Wide>)| if row.0 == outer { inner_at(row.1, inner) } else { 0 };
                        lemma_pairs_finish(*changes, outer, inner, weight);
                        assert(count_at(*reverse, inner, outer) == count_at(initial_reverse, inner, outer) + sum(changes.entries(), weight));
                    }
                }
                return;
            },
        };
        proof { changes.model(); assert(changes.at(outer.view()) == Some(*groups)); }
        let ghost previous_forward = *forward;
        let ghost previous_reverse = *reverse;
        apply_group(forward, reverse, &outer, &*groups, number);
        proof {
            assert forall|query: KeyView, inner: KeyView| #[trigger] count_at(*forward, query, inner)
                == count_at(initial_forward, query, inner) + sum(changes.entries().take(processed@ + 1), |row: (KeyView, Index<Wide>)| if row.0 == query { inner_at(row.1, inner) } else { 0 }) by {
                lemma_sum_take(changes.entries(), processed@, |row: (KeyView, Index<Wide>)| if row.0 == query { inner_at(row.1, inner) } else { 0 });
                assert(count_at(previous_forward, query, inner) == count_at(initial_forward, query, inner) + sum(changes.entries().take(processed@), |row: (KeyView, Index<Wide>)| if row.0 == query { inner_at(row.1, inner) } else { 0 }));
            }
            assert forall|query: KeyView, inner: KeyView| #[trigger] count_at(*reverse, inner, query)
                == count_at(initial_reverse, inner, query) + sum(changes.entries().take(processed@ + 1), |row: (KeyView, Index<Wide>)| if row.0 == query { inner_at(row.1, inner) } else { 0 }) by {
                lemma_sum_take(changes.entries(), processed@, |row: (KeyView, Index<Wide>)| if row.0 == query { inner_at(row.1, inner) } else { 0 });
                assert(count_at(previous_reverse, inner, query) == count_at(initial_reverse, inner, query) + sum(changes.entries().take(processed@), |row: (KeyView, Index<Wide>)| if row.0 == query { inner_at(row.1, inner) } else { 0 }));
            }
        }
        processed = Ghost(processed@ + 1);
    }
}

pub proof fn lemma_pairs_finish(changes: Counts, outer: KeyView, inner: KeyView, selected: spec_fn((KeyView, Index<Wide>)) -> int)
    requires changes.wf(), forall|row: (KeyView, Index<Wide>)| #[trigger] selected(row) == if row.0 == outer { inner_at(row.1, inner) } else { 0 },
    ensures sum(changes.entries(), selected) == count_at(changes, outer, inner),
{
    let weight = |row: (KeyView, Index<Wide>)| inner_at(row.1, inner);
    verus_prooflib::ordered::lemma_key_contribution(&changes, outer, weight);
    assert((|row: (KeyView, Index<Wide>)| if row.0 == outer { inner_at(row.1, inner) } else { 0 })
        =~= (|row: (KeyView, Index<Wide>)| if row.0 == outer { weight(row) } else { 0 }));
    reveal(verus_prooflib::ordered::contribution);
    if changes.at(outer).is_none() { assert(count_at(changes, outer, inner) == 0); }
    else { assert(inner_at(changes.at(outer).unwrap(), inner) == count_at(changes, outer, inner)); }
    reveal(pairs_sum);
    verus_common::sums::lemma_sum_ext_all(changes.entries(),
        |row: (KeyView, Index<Wide>)| if row.0 == outer { inner_at(row.1, inner) } else { 0 },
        |row: (KeyView, Index<Wide>)| if row.0 == outer { weight(row) } else { 0 });
    assert(pairs_sum(changes.entries(), outer, inner) == sum(changes.entries(), |row: (KeyView, Index<Wide>)| if row.0 == outer { inner_at(row.1, inner) } else { 0 }));
    verus_common::sums::lemma_sum_ext_all(changes.entries(), selected, |row: (KeyView, Index<Wide>)| if row.0 == outer { weight(row) } else { 0 });
}

}
