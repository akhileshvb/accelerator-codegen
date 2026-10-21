// Exact keyed accumulators: a map (k1, k2) -> (a, b) of wide integers, held as a hash table of
// small vectors, with the sum of every update kept in the view.
use vstd::prelude::*;
use verus_common::sums::*;
use crate::storage::*;
use crate::wide::*;

verus! {

broadcast use vstd::seq_lib::group_seq_properties;

pub struct Ent { pub k2: i64, pub a: Wide, pub b: Wide }

pub open spec fn ent_at(rows: Seq<Ent>, k2: int, f: int) -> int {
    sum(rows, ent_weight(k2, f))
}

pub open spec fn ent_weight(k2: int, f: int) -> spec_fn(Ent) -> int {
    |e: Ent| if e.k2 == k2 { if f == 0 { e.a@ } else { e.b@ } } else { 0int }
}

pub open spec fn ent_keys(rows: Seq<Ent>) -> Seq<i64> {
    rows.map_values(|e: Ent| e.k2)
}

pub open spec fn delta_of(da: int, db: int, f: int) -> int {
    if f == 0 { da } else { db }
}

/// rows += (k2: da, db).
pub fn bump_ent(rows: &mut Vec<Ent>, k2: i64, da: &Wide, db: &Wide)
    ensures
        forall|k: int, f: int| 0 <= f <= 1 ==> #[trigger] ent_at(final(rows)@, k, f)
            == ent_at(old(rows)@, k, f) + if k == k2 as int { delta_of(da@, db@, f) } else { 0 },
        ent_keys(old(rows)@).no_duplicates() ==> ent_keys(final(rows)@).no_duplicates(),
{
    let ghost before = rows@;
    let mut index = 0;
    while index < rows.len()
        invariant index <= rows.len(), rows@ == before, before == old(rows)@,
            forall|position: int| 0 <= position < index ==> #[trigger] rows@[position].k2 != k2,
        decreases rows.len() - index,
    {
        if rows[index].k2 == k2 {
            let a2 = rows[index].a.add(da);
            let b2 = rows[index].b.add(db);
            let entry = Ent { k2, a: a2, b: b2 };
            let ghost replacement = entry;
            rows.set(index, entry);
            proof {
                assert forall|k: int, f: int| 0 <= f <= 1 implies #[trigger] ent_at(rows@, k, f)
                    == ent_at(before, k, f) + if k == k2 as int { delta_of(da@, db@, f) } else { 0 } by {
                    lemma_sum_update(before, index as int, replacement, ent_weight(k, f));
                }
                assert(ent_keys(rows@) =~= ent_keys(before));
            }
            return;
        }
        index += 1;
    }
    let entry = Ent { k2, a: da.dup(), b: db.dup() };
    let ghost appended = entry;
    rows.push(entry);
    proof {
        assert forall|k: int, f: int| 0 <= f <= 1 implies #[trigger] ent_at(rows@, k, f)
            == ent_at(before, k, f) + if k == k2 as int { delta_of(da@, db@, f) } else { 0 } by {
            lemma_sum_push(before, appended, ent_weight(k, f));
        }
        if ent_keys(before).no_duplicates() {
            assert(!ent_keys(before).contains(k2));
            verus_prooflib::lemma_unique_push(ent_keys(before), k2);
            assert(ent_keys(rows@) =~= ent_keys(before).push(k2));
        }
    }
}

pub type WMap = Work<Vec<Ent>>;

pub open spec fn wm_at(w: WMap, k1: i64, k2: int, f: int) -> int {
    if w.rows@.contains_key(k1) { ent_at(w.rows@[k1]@, k2, f) } else { 0 }
}

/// map[(k1, k2)] += (da, db).
pub fn wm_add(w: &mut WMap, k1: i64, k2: i64, da: &Wide, db: &Wide)
    requires old(w).valid(),
    ensures final(w).valid(),
        forall|j1: i64, j2: int, f: int| 0 <= f <= 1 ==> #[trigger] wm_at(*final(w), j1, j2, f)
            == wm_at(*old(w), j1, j2, f) + if j1 == k1 && j2 == k2 as int { delta_of(da@, db@, f) } else { 0 },
{
    let ghost before = *w;
    let mut rows = match w.take(k1) { Some(rows) => rows, None => Vec::new() };
    proof {
        assert forall|k: int, f: int| 0 <= f <= 1 implies #[trigger] ent_at(rows@, k, f) == wm_at(before, k1, k, f) by {
            if !before.rows@.contains_key(k1) { lemma_sum_empty(rows@, ent_weight(k, f)); }
        }
    }
    bump_ent(&mut rows, k2, da, db);
    w.put(k1, rows);
}


pub open spec fn kf(k0: int, k1: int, f: int) -> int {
    if f == 0 { k0 } else { k1 }
}

/// The bucket of key k1 (an empty one when absent).
pub fn bucket<'a>(w: &'a WMap, k1: i64, empty: &'a Vec<Ent>) -> (r: &'a Vec<Ent>)
    requires empty@.len() == 0,
    ensures forall|k2: int, f: int| #[trigger] ent_at(r@, k2, f) == wm_at(*w, k1, k2, f),
{
    match w.rows.get(&k1) {
        Some(rows) => rows,
        None => {
            proof {
                assert forall|k2: int, f: int| #[trigger] ent_at(empty@, k2, f) == wm_at(*w, k1, k2, f) by {
                    lemma_sum_empty(empty@, ent_weight(k2, f));
                }
            }
            empty
        }
    }
}


}
