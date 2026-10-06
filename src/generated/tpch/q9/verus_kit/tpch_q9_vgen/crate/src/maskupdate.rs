// Maintenance of the name-class rows of the parts under part deltas.
use vstd::prelude::*;
use verus_common::sums::*;
use verus_common::sqlfns::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::meaning::*;
use crate::storage::*;
use crate::values::*;
use crate::rows::*;
use crate::events::*;
use crate::query_proofs::*;
use crate::buildparts::pat_views;

verus! {

broadcast use vstd::seq_lib::group_seq_properties;

pub open spec fn pchange_weight(p: Seq<u8>, key: i64) -> spec_fn(PartDelta) -> int {
    |ch: PartDelta| if ch.row.p_partkey == key && like_match(ch.row.p_name@, p) { ch.sign as int } else { 0int }
}

pub open spec fn mask_overlay(pages: Pages<Vec<MaskRow>>, work: Work<Vec<MaskRow>>, key: i64, c: int) -> int {
    if work.rows@.contains_key(key) { mask_color(work.rows@[key]@, c) } else { page_mask_color(pages, key, c) }
}

pub open spec fn masks_overlay_ready(pages: Pages<Vec<MaskRow>>, work: Work<Vec<MaskRow>>, db: Db, pats: Seq<Seq<u8>>) -> bool {
    work.valid()
        && (forall|key: i64, c: int| 0 <= c < pats.len() ==> #[trigger] mask_overlay(pages, work, key, c) == green(filter_db(db, pats[c]), key as int))
        && forall|key: i64| #[trigger] work.rows@.contains_key(key) ==> {
            let rows = work.rows@[key];
            masks_unique(rows@) && masks_positive(rows@)
        }
}

pub proof fn lemma_part_color_capture(db: Db, db2: Db, changes: Seq<PartDelta>, p: Seq<u8>, key: i64)
    requires bag_step(db.part, db2.part, part_delta_seq_view(changes)),
    ensures green(filter_db(db2, p), key as int) == green(filter_db(db, p), key as int) + sum(changes, pchange_weight(p, key)),
{
    let w = matched_weight(p, key as int);
    let signed = |entry: (PartRow, int)| entry.1 * w(entry.0);
    lemma_matched_green(db, p, key as int);
    lemma_matched_green(db2, p, key as int);
    assert(sum(db2.part, w) == sum(db.part, w) + sum(part_delta_seq_view(changes), signed));
    lemma_sum_map(changes, |ch: PartDelta| (part_in_view(ch.row), ch.sign as int), signed, pchange_weight(p, key));
}

pub fn prune_masks(rows: &mut Vec<MaskRow>) -> (result: Option<()>)
    ensures
        forall|c: int| #[trigger] mask_color(final(rows)@, c) == mask_color(old(rows)@, c),
        masks_unique(old(rows)@) ==> masks_unique(final(rows)@),
        result.is_some() ==> masks_positive(final(rows)@),
{
    let ghost initial = rows@;
    let mut index = 0;
    while index < rows.len()
        invariant index <= rows.len(),
            initial == old(rows)@,
            forall|c: int| #[trigger] mask_color(rows@, c) == mask_color(initial, c),
            masks_unique(initial) ==> masks_unique(rows@),
            forall|position: int| 0 <= position < index ==> 0 < (#[trigger] rows[position]).count,
        decreases rows.len() - index,
    {
        if rows[index].count < 0 { return None; }
        if rows[index].count == 0 {
            let ghost before = rows@;
            rows.remove(index);
            proof {
                assert forall|c: int| #[trigger] mask_color(rows@, c) == mask_color(before, c) by {
                    lemma_sum_remove(before, index as int, |row: MaskRow| if bit(row.mask, c) { row.count as int } else { 0int });
                }
                if masks_unique(before) {
                    assert(rows@.map_values(|row: MaskRow| row.mask) =~= before.map_values(|row: MaskRow| row.mask).remove(index as int));
                    verus_prooflib::lemma_unique_remove(before.map_values(|row: MaskRow| row.mask), index as int);
                }
            }
        } else { index += 1; }
    }
    Some(())
}

#[verifier::spinoff_prover]
pub fn update_masks(pages: &Pages<Vec<MaskRow>>, changes: &Vec<PartDelta>, pats: &Vec<Vec<u8>>, Ghost(db): Ghost<Db>, Ghost(db2): Ghost<Db>) -> (result: Option<Work<Vec<MaskRow>>>)
    requires masks_ready(*pages, db, pat_views_of(pats@)), pats.len() <= 127,
        bag_step(db.part, db2.part, part_delta_seq_view(changes@)),
    ensures result matches Some(work) ==> masks_overlay_ready(*pages, work, db2, pat_views_of(pats@)),
{
    let ghost pv = pat_views_of(pats@);
    let mut work: Work<Vec<MaskRow>> = Work::new();
    proof {
        assert forall|key: i64, c: int| 0 <= c < pv.len() implies #[trigger] mask_overlay(*pages, work, key, c)
            == page_mask_color(*pages, key, c) + sum(changes@.take(0), pchange_weight(pv[c], key)) by {
            lemma_sum_empty(changes@.take(0), pchange_weight(pv[c], key));
        }
    }
    let mut index = 0;
    while index < changes.len()
        invariant index <= changes.len(), pats.len() <= 127, pv == pat_views_of(pats@),
            masks_ready(*pages, db, pv), work.valid(),
            forall|key: i64, c: int| 0 <= c < pv.len() ==> #[trigger] mask_overlay(*pages, work, key, c)
                == page_mask_color(*pages, key, c) + sum(changes@.take(index as int), pchange_weight(pv[c], key)),
            forall|key: i64| #[trigger] work.rows@.contains_key(key) ==> masks_unique(work.rows@[key]@),
        decreases changes.len() - index,
    {
        let change = &changes[index];
        let key = change.row.p_partkey;
        let m = mask_of(&change.row.p_name, pats);
        let ghost before = work;
        let mut rows = match work.take(key) {
            Some(rows) => rows,
            None => match pages.get(key) { Some(rows) => copy_masks(&rows), None => Vec::new() },
        };
        proof {
            assert(masks_unique(rows@));
            assert forall|c: int| 0 <= c < pv.len() implies #[trigger] mask_color(rows@, c) == mask_overlay(*pages, before, key, c) by {
                if !before.rows@.contains_key(key) && pages.at(key).is_none() {
                    lemma_sum_empty(rows@, |row: MaskRow| if bit(row.mask, c) { row.count as int } else { 0int });
                }
            }
        }
        bump_mask(&mut rows, m, change.sign as i128)?;
        work.put(key, rows);
        proof {
            assert forall|target: i64, c: int| 0 <= c < pv.len() implies #[trigger] mask_overlay(*pages, work, target, c)
                == page_mask_color(*pages, target, c) + sum(changes@.take(index as int + 1), pchange_weight(pv[c], target)) by {
                lemma_sum_take(changes@, index as int, pchange_weight(pv[c], target));
                if target != key { assert(mask_overlay(*pages, work, target, c) == mask_overlay(*pages, before, target, c)); }
                else { assert(bit(m, c) <==> like_match(change.row.p_name@, pv[c])); }
            }
        }
        index += 1;
    }
    proof {
        assert(changes@.take(index as int) =~= changes@);
        assert forall|key: i64, c: int| 0 <= c < pv.len() implies #[trigger] mask_overlay(*pages, work, key, c) == green(filter_db(db2, pv[c]), key as int) by {
            lemma_part_color_capture(db, db2, changes@, pv[c], key);
            assert(page_mask_color(*pages, key, c) == green(filter_db(db, pv[c]), key as int));
        }
    }
    index = 0;
    while index < work.keys.len()
        invariant index <= work.keys.len(), work.valid(), pv == pat_views_of(pats@),
            forall|key: i64, c: int| 0 <= c < pv.len() ==> #[trigger] mask_overlay(*pages, work, key, c) == green(filter_db(db2, pv[c]), key as int),
            forall|key: i64| #[trigger] work.rows@.contains_key(key) ==> masks_unique(work.rows@[key]@),
            forall|key: i64| work.keys@.take(index as int).contains(key) && #[trigger] work.rows@.contains_key(key) ==> masks_positive(work.rows@[key]@),
        decreases work.keys.len() - index,
    {
        let key = work.keys[index];
        let ghost before = work;
        let mut rows = work.take(key)?;
        prune_masks(&mut rows)?;
        work.put(key, rows);
        proof {
            assert forall|target: i64, c: int| 0 <= c < pv.len() implies #[trigger] mask_overlay(*pages, work, target, c) == green(filter_db(db2, pv[c]), target as int) by {
                assert(mask_overlay(*pages, work, target, c) == mask_overlay(*pages, before, target, c));
            }
        }
        index += 1;
    }
    proof {
        assert(work.keys@.take(index as int) =~= work.keys@);
        assert forall|key: i64| #[trigger] work.rows@.contains_key(key) implies masks_positive(work.rows@[key]@) by {
            assert(work.keys@.contains(key));
        }
    }
    Some(work)
}

#[verifier::spinoff_prover]
pub fn publish_masks(pages: &Pages<Vec<MaskRow>>, work: &mut Work<Vec<MaskRow>>, Ghost(db): Ghost<Db>, Ghost(db2): Ghost<Db>, Ghost(pv): Ghost<Seq<Seq<u8>>>) -> (result: Option<Pages<Vec<MaskRow>>>)
    requires masks_ready(*pages, db, pv), masks_overlay_ready(*pages, *old(work), db2, pv),
    ensures final(work).valid(), result matches Some(next) ==> masks_ready(next, db2, pv),
{
    let ghost initial = *work;
    let mut next = pages.share();
    let mut index = 0;
    while index < work.keys.len()
        invariant index <= work.keys.len(), work.valid(), next.valid(), pages.valid(), masks_ready(*pages, db, pv),
            work.keys@ == initial.keys@, initial.valid(),
            masks_overlay_ready(*pages, initial, db2, pv),
            forall|key: i64| #[trigger] work.rows@.contains_key(key)
                <==> initial.rows@.contains_key(key) && !initial.keys@.take(index as int).contains(key),
            forall|key: i64| #[trigger] work.rows@.contains_key(key) ==> work.rows@[key] == initial.rows@[key],
            forall|key: i64| #[trigger] next.at(key) == if initial.keys@.take(index as int).contains(key) && initial.rows@.contains_key(key) {
                let rows = initial.rows@[key];
                if rows.len() == 0 { None } else { Some(rows) }
            } else { pages.at(key) },
        decreases work.keys.len() - index,
    {
        let key = work.keys[index];
        proof { assert(!initial.keys@.take(index as int).contains(key)); }
        let rows = work.take(key)?;
        next.put(key, if rows.len() == 0 { None } else { Some(rows) });
        proof {
            assert forall|target: i64| #[trigger] next.at(target) == if initial.keys@.take(index as int + 1).contains(target) && initial.rows@.contains_key(target) {
                let rows = initial.rows@[target];
                if rows.len() == 0 { None } else { Some(rows) }
            } else { pages.at(target) } by {
                assert(initial.keys@.take(index as int + 1) =~= initial.keys@.take(index as int).push(key));
                verus_prooflib::lemma_push_membership(initial.keys@.take(index as int), key);
            }
            assert forall|target: i64| #[trigger] work.rows@.contains_key(target)
                <==> initial.rows@.contains_key(target) && !initial.keys@.take(index as int + 1).contains(target) by {
                assert(initial.keys@.take(index as int + 1) =~= initial.keys@.take(index as int).push(key));
                verus_prooflib::lemma_push_membership(initial.keys@.take(index as int), key);
            }
        }
        index += 1;
    }
    proof {
        assert(initial.keys@.take(index as int) =~= initial.keys@);
        assert forall|key: i64, c: int| 0 <= c < pv.len() implies #[trigger] page_mask_color(next, key, c) == green(filter_db(db2, pv[c]), key as int) by {
            if initial.rows@.contains_key(key) {
                assert(initial.keys@.contains(key));
                let rows = initial.rows@[key];
                if rows.len() == 0 {
                    lemma_sum_empty(rows@, |row: MaskRow| if bit(row.mask, c) { row.count as int } else { 0int });
                }
            }
            assert(page_mask_color(next, key, c) == mask_overlay(*pages, initial, key, c));
        }
        assert forall|key: i64| #[trigger] next.at(key).is_some() implies {
            let rows = next.at(key).unwrap();
            rows.len() > 0 && masks_unique(rows@) && masks_positive(rows@)
        } by {
            if initial.rows@.contains_key(key) { assert(initial.keys@.contains(key)); }
        }
    }
    Some(next)
}

}
