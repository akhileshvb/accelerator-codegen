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

pub fn rescan_keys(delta: &Vec<PartDelta>) -> (out: Vec<i64>)
    ensures out@ == wanted_keys(delta@),
{
    let mut out = Vec::new();
    let mut seen: HashMap<i64, bool> = HashMap::new();
    let mut index = 0usize;
    while index < delta.len()
        invariant index <= delta.len(), out@ == wanted_keys(delta@.take(index as int)),
            seen@.dom() == out@.to_set(),
        decreases delta.len() - index,
    {
        let row = &delta[index];
        proof {
            assert(delta@.take(index as int + 1).drop_last() =~= delta@.take(index as int));
            assert(delta@.take(index as int + 1).last() == delta@[index as int]);
            out@.to_set_ensures();
        }
        if (row.small != 0 || row.medium != 0 || row.large != 0) && !seen.contains_key(&row.p_partkey) {
            proof { lemma_push_to_set(out@, row.p_partkey); }
            seen.insert(row.p_partkey, true);
            out.push(row.p_partkey);
        }
        index += 1;
    }
    proof { assert(delta@.take(delta.len() as int) =~= delta@); }
    out
}

fn publish_parts(old: &Store<PartCell>, changes: &Work<PartCell>) -> (next: Store<PartCell>)
    requires parts_valid(*old), part_patch_valid(*changes),
    ensures parts_valid(next) && forall|key: int, class: u8, brand: Seq<u8>|
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
        proof { changes.keys@.to_set_ensures(); }
        let delta = changes.map.get(&key).unwrap();
        proof { assert(changes.lookup(key) == delta@); }
        proof { lemma_prefix_fresh(changes.keys@, index as int); }
        if changed(delta) {
            let empty = Vec::new();
            let old_cells = old.get(key);
            let base = match &old_cells { Some(cells) => &**cells, None => &empty };
            let final_cells = merge_parts(base, delta);
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
    parts
}

fn publish_histogram(old: &Store<HistCell>, patch: &Work<HistCell>) -> (next: Store<HistCell>)
    requires histogram_valid(*old), hist_patch_valid(*patch),
    ensures histogram_valid(next) && forall|class: u8, brand: Seq<u8>, quantity: int, revenue: bool|
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
        proof { patch.keys@.to_set_ensures(); }
        let delta = patch.map.get(&key).unwrap();
        proof { assert(patch.lookup(key) == delta@); }
        proof { lemma_prefix_fresh(patch.keys@, index as int); }
        let empty = Vec::new();
        let old_cells = old.get(key);
        let base = match &old_cells { Some(cells) => &**cells, None => &empty };
        let final_cells = merge_histogram(base, delta, key);
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
    histogram
}

pub fn update(state: &State, lineitem_delta: &[LineDelta], part_delta: &Vec<PartDelta>,
    keys: &[i64], rows: &[LineInput], Ghost(lineitem_full): Ghost<Seq<(LineitemRow, int)>>,
    Ghost(part_full): Ghost<Seq<(PartRow, int)>>, Ghost(db2): Ghost<Db>)
    -> (result: (Store<PartCell>, Store<HistCell>))
    requires lineitem_delta_ok(lineitem_delta@, lineitem_full), part_delta_ok(part_delta@, part_full),
        capture_ok(state.db(), db2, lineitem_full, part_full), keys@ == wanted_keys(part_delta@),
        rescan_lines_for_parts_ok(db2, keys@, rows@),
    ensures
        parts_valid(result.0),
        histogram_valid(result.1),
        forall|key: int, class: u8, brand: Seq<u8>| #![trigger part_value(result.0, key, class, brand)]
            part_value(result.0, key, class, brand) == part_sum(db2, key, class, brand),
        forall|class: u8, brand: Seq<u8>, quantity: int, revenue: bool|
            #![trigger hist_value(result.1, class, brand, quantity, revenue)]
            hist_value(result.1, class, brand, quantity, revenue) == hist_sum(db2, class, brand, quantity, revenue),
{
    proof { state.facts(); }
    let changes = coalesce_parts(part_delta);
    proof {
        assert forall|key: int, class: u8, brand: Seq<u8>| #![trigger part_work_value(changes, key, class, brand)]
            !seq_view_i64(keys@).contains(key) implies part_work_value(changes, key, class, brand) == 0 by {
            lemma_wanted_covers(part_delta@, key, class, brand);
            lemma_sum_take_full(part_delta@, |row: PartDelta| part_delta_weight(row, key, class, brand));
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
                    let value = amount(row.l_extendedprice, row.l_discount);
                    contribute(&mut patch, &*cells, row.l_quantity, row.sign, &value);
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
                let value = amount(row.l_extendedprice, row.l_discount);
                contribute(&mut patch, cells, row.l_quantity, 1, &value);
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
    let parts = publish_parts(&state.parts, &changes);
    let histogram = publish_histogram(&state.histogram, &patch);
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
    (parts, histogram)
}

}
