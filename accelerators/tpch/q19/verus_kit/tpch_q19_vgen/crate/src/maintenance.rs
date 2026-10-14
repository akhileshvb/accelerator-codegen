use vstd::prelude::*;
use std::collections::HashMap;
use crate::gen_obligations::*;
use crate::gen_spec::*;
use crate::kernel::State;
use crate::model::*;
use crate::storage::*;
use crate::values::*;
use verus_common::sums::*;
use verus_prooflib::*;

verus! {

broadcast use vstd::seq_lib::group_seq_properties;

pub fn rescan_keys(delta: &Vec<PartDelta>) -> Vec<i64> {
    let mut out = Vec::new();
    match coalesce_parts(delta) {
        Some(work) => {
            let mut index = 0usize;
            while index < work.keys.len()
                invariant index <= work.keys.len(),
                decreases work.keys.len() - index,
            {
                let key = work.keys[index];
                match work.map.get(&key) {
                    Some(cells) => { if changed(cells) { out.push(key as i64); } },
                    None => {},
                }
                index += 1;
            }
        },
        None => {
            let mut seen = HashMap::new();
            let mut index = 0usize;
            while index < delta.len()
                invariant index <= delta.len(),
                decreases delta.len() - index,
            {
                let key = delta[index].p_partkey;
                if !seen.contains_key(&key) { seen.insert(key, true); out.push(key); }
                index += 1;
            }
        },
    }
    out
}

fn publish_parts(old: &Store<PartCell>, changes: &Work<PartCell>) -> (result: Option<Store<PartCell>>)
    requires parts_valid(*old), part_patch_valid(*changes),
    ensures result matches Some(next) ==> parts_valid(next) && forall|key: int, class: u8, brand: Seq<u8>|
        #![trigger part_value(next, key, class, brand)]
        part_value(next, key, class, brand) == part_value(*old, key, class, brand) + part_work_value(*changes, key, class, brand),
{
    let mut parts = old.share();
    let mut index = 0usize;
    while index < changes.keys.len()
        invariant index <= changes.keys.len(), parts_valid(*old), part_patch_valid(*changes), parts_valid(parts),
            forall|key: u64, class: u8, brand: Seq<u8>|
                #![trigger part_cells_value(parts.lookup(key), class, brand)]
                part_cells_value(parts.lookup(key), class, brand) == part_cells_value(old.lookup(key), class, brand)
                    + if changes.keys@.take(index as int).contains(key) { part_cells_value(changes.lookup(key), class, brand) } else { 0 },
        decreases changes.keys.len() - index,
    {
        let key = changes.keys[index];
        let delta = match changes.map.get(&key) { Some(cells) => cells, None => return None };
        proof { assert(changes.lookup(key) == delta@); }
        proof { lemma_prefix_fresh(changes.keys@, index as int); }
        if changed(delta) {
            let empty = Vec::new();
            let old_cells = old.get(key);
            let base = match &old_cells { Some(cells) => &**cells, None => &empty };
            let final_cells = match merge_parts(base, delta) { Some(cells) => cells, None => return None };
            parts.replace(key, final_cells);
        } else {
            proof {
                assert forall|class: u8, brand: Seq<u8>| #[trigger] part_cells_value(delta@, class, brand) == 0 by {
                    lemma_sum_zero(delta@, |cell: PartCell| part_cell_weight(cell, class, brand));
                }
            }
        }
        proof { assert(changes.keys@.take(index as int + 1) =~= changes.keys@.take(index as int).push(key)); }
        index += 1;
    }
    proof {
        assert forall|key: int, class: u8, brand: Seq<u8>| #![trigger part_value(parts, key, class, brand)]
            part_value(parts, key, class, brand) == part_value(*old, key, class, brand) + part_work_value(*changes, key, class, brand) by {
            if i64::MIN <= key <= i64::MAX && !changes.keys@.contains((key as i64) as u64) {
                assert(!changes.map@.dom().contains((key as i64) as u64));
            }
        }
    }
    Some(parts)
}

fn publish_histogram(old: &Store<HistCell>, patch: &Work<HistCell>) -> (result: Option<Store<HistCell>>)
    requires histogram_valid(*old), hist_patch_valid(*patch),
    ensures result matches Some(next) ==> histogram_valid(next) && forall|class: u8, brand: Seq<u8>, quantity: int, revenue: bool|
        #![trigger hist_value(next, class, brand, quantity, revenue)]
        hist_value(next, class, brand, quantity, revenue) == hist_value(*old, class, brand, quantity, revenue)
            + hist_work_value(*patch, class, brand, quantity, revenue),
{
    let mut histogram = old.share();
    let mut index = 0usize;
    while index < patch.keys.len()
        invariant index <= patch.keys.len(), histogram_valid(*old), hist_patch_valid(*patch), histogram_valid(histogram),
            forall|key: u64, class: u8, brand: Seq<u8>, quantity: int, revenue: bool|
                #![trigger hist_cells_value(histogram.lookup(key), class, brand, quantity, revenue)]
                hist_cells_value(histogram.lookup(key), class, brand, quantity, revenue)
                    == hist_cells_value(old.lookup(key), class, brand, quantity, revenue)
                        + if patch.keys@.take(index as int).contains(key) { hist_cells_value(patch.lookup(key), class, brand, quantity, revenue) } else { 0 },
        decreases patch.keys.len() - index,
    {
        let key = patch.keys[index];
        let delta = match patch.map.get(&key) { Some(cells) => cells, None => return None };
        proof { assert(patch.lookup(key) == delta@); }
        proof { lemma_prefix_fresh(patch.keys@, index as int); }
        let empty = Vec::new();
        let old_cells = old.get(key);
        let base = match &old_cells { Some(cells) => &**cells, None => &empty };
        let final_cells = match merge_histogram(base, delta, key) { Some(cells) => cells, None => return None };
        histogram.replace(key, final_cells);
        proof { assert(patch.keys@.take(index as int + 1) =~= patch.keys@.take(index as int).push(key)); }
        index += 1;
    }
    proof {
        assert forall|class: u8, brand: Seq<u8>, quantity: int, revenue: bool|
            #![trigger hist_value(histogram, class, brand, quantity, revenue)]
            hist_value(histogram, class, brand, quantity, revenue) == hist_value(*old, class, brand, quantity, revenue)
                + hist_work_value(*patch, class, brand, quantity, revenue) by {
            if i64::MIN <= quantity <= i64::MAX && !patch.keys@.contains(hist_key(class, brand, quantity as i64)) {
                assert(!patch.map@.dom().contains(hist_key(class, brand, quantity as i64)));
            }
        }
    }
    Some(histogram)
}

pub fn update(state: &State, lineitem_delta: &[LineDelta], part_delta: &Vec<PartDelta>,
    keys: &[i64], rows: &[LineInput], Ghost(lineitem_full): Ghost<Seq<(LineitemRow, int)>>,
    Ghost(part_full): Ghost<Seq<(PartRow, int)>>, Ghost(db2): Ghost<Db>)
    -> (result: Option<(Store<PartCell>, Store<HistCell>)>)
    requires lineitem_delta_ok(lineitem_delta@, lineitem_full), part_delta_ok(part_delta@, part_full),
        capture_ok(state.db(), db2, lineitem_full, part_full), rescan_lines_for_parts_ok(db2, keys@, rows@),
    ensures result matches Some(next) ==> {
        &&& parts_valid(next.0)
        &&& histogram_valid(next.1)
        &&& forall|key: int, class: u8, brand: Seq<u8>| #![trigger part_value(next.0, key, class, brand)]
            part_value(next.0, key, class, brand) == part_sum(db2, key, class, brand)
        &&& forall|class: u8, brand: Seq<u8>, quantity: int, revenue: bool|
            #![trigger hist_value(next.1, class, brand, quantity, revenue)]
            hist_value(next.1, class, brand, quantity, revenue) == hist_sum(db2, class, brand, quantity, revenue)
    },
{
    proof { state.facts(); }
    let changes = match coalesce_parts(part_delta) { Some(work) => work, None => return None };
    let mut covered = HashMap::new();
    let mut index = 0usize;
    while index < keys.len()
        invariant index <= keys.len(), changes_for(changes, part_delta@),
            covered@.dom() == keys_u64(keys@.take(index as int)).to_set(),
        decreases keys.len() - index,
    {
        proof {
            assert(keys_u64(keys@.take(index as int + 1)) =~= keys_u64(keys@.take(index as int)).push(keys@[index as int] as u64));
            lemma_push_to_set(keys_u64(keys@.take(index as int)), keys@[index as int] as u64);
        }
        covered.insert(keys[index] as u64, true);
        index += 1;
    }
    let mut index = 0usize;
    while index < changes.keys.len()
        invariant index <= changes.keys.len(), changes_for(changes, part_delta@),
            covered@.dom() == keys_u64(keys@).to_set(),
            forall|key: u64, class: u8, brand: Seq<u8>| #![trigger part_cells_value(changes.lookup(key), class, brand)]
                changes.keys@.take(index as int).contains(key) && !covered@.dom().contains(key) ==>
                    part_cells_value(changes.lookup(key), class, brand) == 0,
        decreases changes.keys.len() - index,
    {
        let key = changes.keys[index];
        match changes.map.get(&key) {
            Some(cells) => {
                if changed(cells) {
                    if !covered.contains_key(&key) { return None; }
                } else {
                    proof {
                        assert(changes.lookup(key) == cells@);
                        assert forall|class: u8, brand: Seq<u8>| #[trigger] part_cells_value(cells@, class, brand) == 0 by {
                            lemma_sum_zero(cells@, |cell: PartCell| part_cell_weight(cell, class, brand));
                        }
                    }
                }
            },
            None => return None,
        }
        proof { assert(changes.keys@.take(index as int + 1) =~= changes.keys@.take(index as int).push(key)); }
        index += 1;
    }
    proof {
        assert forall|key: int, class: u8, brand: Seq<u8>| #![trigger part_work_value(changes, key, class, brand)]
            !seq_view_i64(keys@).contains(key) implies part_work_value(changes, key, class, brand) == 0 by {
            if i64::MIN <= key <= i64::MAX {
                lemma_signed_keys(keys@, key as i64);
                if !changes.keys@.contains((key as i64) as u64) { assert(changes.lookup((key as i64) as u64) =~= Seq::empty()); }
            }
        }
    }
    let mut patch = Work::new();
    let mut index = 0usize;
    while index < lineitem_delta.len()
        invariant index <= lineitem_delta.len(), hist_patch_valid(patch), parts_valid(state.parts),
            changes_for(changes, part_delta@), coverage(changes, keys@),
            forall|class: u8, brand: Seq<u8>, quantity: int, revenue: bool|
                #![trigger hist_work_value(patch, class, brand, quantity, revenue)]
                hist_work_value(patch, class, brand, quantity, revenue)
                    == line_delta_prefix(state.parts, lineitem_delta@, index as int, class, brand, quantity, revenue),
        decreases lineitem_delta.len() - index,
    {
        let ghost processed = index as int;
        let ghost previous = patch;
        let row = &lineitem_delta[index];
        if row.eligible != 0 && row.sign != 0 {
            match state.parts.get(row.l_partkey as u64) {
                Some(cells) => {
                    proof {
                        assert forall|class: u8, brand: Seq<u8>| #![trigger part_value(state.parts, row.l_partkey as int, class, brand)]
                            part_value(state.parts, row.l_partkey as int, class, brand) == part_cells_value((*cells)@, class, brand) by {}
                    }
                    let value = match amount(row.l_extendedprice, row.l_discount) { Some(value) => value, None => return None };
                    if !contribute(&mut patch, &*cells, row.l_quantity, row.sign as i128, value) { return None; }
                },
                None => {
                    proof {
                        assert(state.parts.lookup(row.l_partkey as u64) =~= Seq::empty());
                        assert forall|class: u8, brand: Seq<u8>| #[trigger] part_value(state.parts, row.l_partkey as int, class, brand) == 0 by {}
                    }
                },
            }
        } else {
            proof { assert(patch == previous); }
        }
        proof {
            assert forall|class: u8, brand: Seq<u8>, quantity: int, revenue: bool|
                #![trigger hist_work_value(patch, class, brand, quantity, revenue)]
                hist_work_value(patch, class, brand, quantity, revenue)
                    == line_delta_prefix(state.parts, lineitem_delta@, index as int + 1, class, brand, quantity, revenue) by {
                assert(hist_work_value(previous, class, brand, quantity, revenue) == line_delta_prefix(state.parts, lineitem_delta@, index as int, class, brand, quantity, revenue));
                lemma_sum_take(lineitem_delta@, index as int, |row: LineDelta| line_delta_weight(state.parts, row, class, brand, quantity, revenue));
            }
        }
        index += 1;
    }
    let mut index = 0usize;
    while index < rows.len()
        invariant index <= rows.len(), hist_patch_valid(patch), changes_for(changes, part_delta@), coverage(changes, keys@),
            forall|class: u8, brand: Seq<u8>, quantity: int, revenue: bool|
                #![trigger hist_work_value(patch, class, brand, quantity, revenue)]
                hist_work_value(patch, class, brand, quantity, revenue)
                    == line_delta_prefix(state.parts, lineitem_delta@, lineitem_delta.len() as int, class, brand, quantity, revenue)
                        + rescan_prefix(changes, rows@, index as int, class, brand, quantity, revenue),
        decreases rows.len() - index,
    {
        let ghost processed = index as int;
        let row = &rows[index];
        match changes.map.get(&(row.l_partkey as u64)) {
            Some(cells) => {
                proof { assert(changes.lookup(row.l_partkey as u64) == cells@); }
                let value = match amount(row.l_extendedprice, row.l_discount) { Some(value) => value, None => return None };
                if !contribute(&mut patch, cells, row.l_quantity, 1, value) { return None; }
            },
            None => {},
        }
        proof {
            assert forall|class: u8, brand: Seq<u8>, quantity: int, revenue: bool|
                #![trigger hist_work_value(patch, class, brand, quantity, revenue)]
                hist_work_value(patch, class, brand, quantity, revenue)
                    == line_delta_prefix(state.parts, lineitem_delta@, lineitem_delta.len() as int, class, brand, quantity, revenue)
                        + rescan_prefix(changes, rows@, index as int + 1, class, brand, quantity, revenue) by {
                lemma_sum_take(rows@, index as int, |row: LineInput| rescan_weight(changes, row, class, brand, quantity, revenue));
            }
        }
        index += 1;
    }
    proof { state.facts(); }
    let parts = match publish_parts(&state.parts, &changes) { Some(parts) => parts, None => return None };
    let histogram = match publish_histogram(&state.histogram, &patch) { Some(histogram) => histogram, None => return None };
    proof {
        assert forall|key: int, class: u8, brand: Seq<u8>| #![trigger part_value(parts, key, class, brand)]
            part_value(parts, key, class, brand) == part_sum(db2, key, class, brand) by {
            crate::changes::lemma_part_capture(state.db(), db2, part_delta@, part_full, key, class, brand);
        }
        assert forall|class: u8, brand: Seq<u8>, quantity: int, revenue: bool|
            #![trigger hist_value(histogram, class, brand, quantity, revenue)]
            hist_value(histogram, class, brand, quantity, revenue) == hist_sum(db2, class, brand, quantity, revenue) by {
            crate::changes::lemma_hist_capture(state.db(), db2, state.parts, changes, lineitem_delta@, part_delta@,
                rows@, keys@, lineitem_full, part_full, class, brand, quantity, revenue);
        }
    }
    Some((parts, histogram))
}

}
