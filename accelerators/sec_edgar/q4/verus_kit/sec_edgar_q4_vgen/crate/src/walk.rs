// A read: the groups of one (uom, stmt) with sic in a range, from an ordered walk of the index.
use vstd::prelude::*;
use std::sync::Arc;
use verus_common::bytes::*;
use verus_prooflib::ordered::{Index, Key, KeyView, key_lt, ordered_rows, lower_rows, lemma_lower_bindings, lemma_ordered_unique};
use crate::cells::*;

verus! {

pub open spec fn entries_of(rows: Seq<(Key, Arc<Cell>)>) -> Seq<(KeyView, Cell)> {
    rows.map_values(|row: (Key, Arc<Cell>)| (row.0.view(), *row.1))
}

pub proof fn lemma_entries_at(rows: Seq<(Key, Arc<Cell>)>, j: int)
    requires 0 <= j < rows.len(),
    ensures entries_of(rows)[j] == (rows[j].0.view(), *rows[j].1),
{
}

/// The groups whose sic is in [lo, hi], in key order.
pub fn range_rows(idx: &Index<Cell>, lo: i64, hi: i64) -> (rows: Vec<(Key, Arc<Cell>)>)
    ensures
        forall|j: int| 0 <= j < rows.len() ==> lo <= (#[trigger] rows[j]).0.view().0 <= hi,
        forall|j: int| 0 <= j < rows.len() ==> idx.entries().contains(#[trigger] entries_of(rows@)[j]),
        forall|e: (KeyView, Cell)| #[trigger] idx.entries().contains(e) && lo <= e.0.0 <= hi ==>
            exists|j: int| 0 <= j < rows.len() && entries_of(rows@)[j] == e,
        ordered_rows(entries_of(rows@)),
{
    let start = Key::number(lo);
    let mut cur = idx.cursor_from(&start);
    let ghost lower = lower_rows(idx.entries(), start.view());
    let mut rows: Vec<(Key, Arc<Cell>)> = Vec::new();
    let mut done = false;
    let ghost mut lost: Seq<(KeyView, Cell)> = Seq::empty();
    proof {
        lemma_lower_bindings(idx.entries(), start.view());
        assert(entries_of(rows@) + lost + cur.remaining() =~= lower);
        assert(ordered_rows(lower));
    }
    while !done
        invariant
            entries_of(rows@) + lost + cur.remaining() =~= lower,
            lower == lower_rows(idx.entries(), start.view()),
            ordered_rows(lower),
            forall|e: (KeyView, Cell)| #[trigger] lower.contains(e) ==> !key_lt(e.0, start.view()),
            ordered_rows(cur.remaining()),
            ordered_rows(entries_of(rows@)),
            start.view() == (lo as int, Seq::<u8>::empty()),
            forall|j: int| 0 <= j < rows.len() ==> lo <= (#[trigger] rows[j]).0.view().0 <= hi,
            cur.work() >= 0,
            lost.len() == 0 || done,
            done ==> forall|k: int| 0 <= k < lost.len() + cur.remaining().len() ==> ((lost + cur.remaining())[k]).0.0 > hi as int,
        decreases if done { 0int } else { cur.work() + 1 },
    {
        let ghost rem0 = cur.remaining();
        let ghost vis0 = entries_of(rows@);
        let next = cur.next();
        match next {
            None => {
                proof { assert(lost + cur.remaining() =~= Seq::<(KeyView, Cell)>::empty()); }
                done = true;
            },
            Some(row) => {
                if row.0.number > hi {
                    proof {
                        assert(rem0[0] == (row.0.view(), *row.1));
                        lost = seq![rem0[0]];
                        assert forall|k: int| 1 <= k < rem0.len() implies rem0[k].0.0 > hi as int by {
                            assert(key_lt(rem0[0].0, rem0[k].0));
                        }
                        assert(rem0 =~= seq![rem0[0]] + cur.remaining());
                        assert forall|k: int| 0 <= k < lost.len() + cur.remaining().len() implies ((lost + cur.remaining())[k]).0.0 > hi as int by {
                            if k == 0 { assert((lost + cur.remaining())[0] == rem0[0]); }
                            else { assert((lost + cur.remaining())[k] == rem0[k]); }
                        }
                    }
                    done = true;
                } else {
                    proof {
                        assert(rem0[0] == (row.0.view(), *row.1));
                        assert(lost.len() == 0);
                        assert(lower[vis0.len() as int] == rem0[0]);
                        assert(lower.contains(rem0[0]));
                        assert(rem0[0].0.0 >= lo as int);
                        assert(entries_of(rows@.push(row)) =~= vis0.push((row.0.view(), *row.1)));
                        assert(rem0 =~= seq![rem0[0]] + cur.remaining());
                        assert(vis0.push(rem0[0]) + cur.remaining() =~= vis0 + rem0);
                        assert forall|a: int, b: int| 0 <= a < b < vis0.len() + 1 implies
                            key_lt(entries_of(rows@.push(row))[a].0, entries_of(rows@.push(row))[b].0) by {
                            if b == vis0.len() as int {
                                assert(lower[a] == vis0[a]);
                            }
                        }
                    }
                    rows.push(row);
                }
            },
        }
    }
    proof {
        assert forall|j: int| 0 <= j < rows.len() implies idx.entries().contains(#[trigger] entries_of(rows@)[j]) by {
            assert(lower[j] == entries_of(rows@)[j]);
            assert(lower.contains(entries_of(rows@)[j]));
        }
        assert forall|e: (KeyView, Cell)| #[trigger] idx.entries().contains(e) && lo <= e.0.0 <= hi implies
            exists|j: int| 0 <= j < rows.len() && entries_of(rows@)[j] == e by {
            assert(!key_lt(e.0, start.view()));
            assert(lower.contains(e));
            let k = choose|k: int| 0 <= k < lower.len() && lower[k] == e;
            let v = entries_of(rows@).len();
            if k >= v {
                assert(lower[k] == (lost + cur.remaining())[k - v]);
            }
        }
    }
    rows
}

}
