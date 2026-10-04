use std::sync::Arc;
use vstd::prelude::*;
use verus_common::sums::*;
use verus_prooflib::ordered::{Index, Key, KeyView};
use crate::model::*;
use crate::values::*;
use crate::inventory::*;
use crate::construction::*;
use crate::names::*;

verus! {

pub open spec fn cell_change(change: Moment, part: i64, selected: Option<i64>, value: bool) -> int {
    if selected.is_none() || selected == Some(part) { moment_measure(change, value) } else { 0 }
}

pub open spec fn merged_at(names: Index<NameView>, key: KeyView) -> bool {
    match names.at(key) { Some(NameView::Merged { .. }) => true, _ => false }
}

pub open spec fn name_measure(names: Index<NameView>, key: KeyView, part: Option<i64>, value: bool) -> int {
    match names.at(key) { Some(NameView::Merged { inventory }) => measure(inventory, part, value), _ => 0 }
}

pub open spec fn same_shape(before: Index<NameView>, after: Index<NameView>) -> bool {
    &&& forall|key: KeyView| #[trigger] before.at(key).is_none() ==> after.at(key).is_none()
    &&& forall|key: KeyView| (#[trigger] before.at(key)).is_some() ==> match before.at(key).unwrap() {
            NameView::Singleton { .. } => after.at(key) == before.at(key),
            NameView::Merged { .. } => merged_at(after, key),
        }
}

pub open spec fn symmetric(data: Data) -> bool {
    forall|nation: i64, name: Seq<u8>| #[trigger] count_at(data.nations, numeric_key(nation as int), text_key(name))
        == count_at(data.nations_reverse, text_key(name), numeric_key(nation as int))
}

pub proof fn lemma_names_frame(before: Data, after: Data)
    requires before.inventories == after.inventories, before.nations_reverse == after.nations_reverse, before.names == after.names,
    ensures indexed_names_ok(before) ==> indexed_names_ok(after),
{
    if indexed_names_ok(before) {
        assert forall|key: KeyView| #[trigger] binding_ok(after, key) by { assert(binding_ok(before, key)); }
    }
}

pub proof fn lemma_shape_transitive(before: Index<NameView>, middle: Index<NameView>, after: Index<NameView>)
    requires same_shape(before, middle), same_shape(middle, after),
    ensures same_shape(before, after),
{
    assert forall|key: KeyView| (#[trigger] before.at(key)).is_some() implies match before.at(key).unwrap() {
        NameView::Singleton { .. } => after.at(key) == before.at(key),
        NameView::Merged { .. } => merged_at(after, key),
    } by { assert(middle.at(key).is_some()); }
}

pub fn change_name(data: &mut Data, name: &Key, part: i64, change: Moment, multiplicity: i128) -> (result: bool)
    requires old(data).structures_valid(),
    ensures final(data).structures_valid(), same_dimensions(*old(data), *final(data)), final(data).inventories == old(data).inventories,
        same_shape(old(data).names, final(data).names),
        forall|key: KeyView| key != name.view() ==> (#[trigger] final(data).names.at(key)) == old(data).names.at(key),
        result ==> forall|key: KeyView, selected: Option<i64>, value: bool| #[trigger] name_measure(final(data).names, key, selected, value)
            == name_measure(old(data).names, key, selected, value)
                + if key == name.view() && merged_at(old(data).names, key) { cell_change(change, part, selected, value) * multiplicity as int } else { 0 },
{
    let view = match data.names.get(name) { Some(view) => view, None => return true };
    if let NameView::Merged { inventory } = &*view {
        let mut updated = inventory.share();
        let weighted = match scale(change, multiplicity) { Some(weighted) => weighted, None => return false };
        if !updated.update(part, weighted) { return false; }
        data.names.put(name.share(), Arc::new(NameView::Merged { inventory: updated }));
        proof {
            assert forall|selected: Option<i64>, value: bool| #[trigger] measure(updated, selected, value)
                == measure(*inventory, selected, value) + cell_change(change, part, selected, value) * multiplicity as int by {
                if let Some(selected) = selected {
                    if selected == part {
                        assert(moment_at(updated.groups, selected as int).count as int == moment_at(inventory.groups, selected as int).count as int + weighted.count as int);
                        assert(moment_at(updated.groups, selected as int).value as int == moment_at(inventory.groups, selected as int).value as int + weighted.value as int);
                    } else { assert(moment_at(updated.groups, selected as int) == moment_at(inventory.groups, selected as int)); }
                }
            }
        }
    }
    true
}

pub proof fn lemma_support_cell(before: Index<Inventory>, after: Index<Inventory>, support: Index<i128>, nation: i64, part: i64, change: Moment, selected: Option<i64>, value: bool)
    requires support.wf(),
        forall|key: KeyView| (#[trigger] support.at(key)).is_some() ==> source_measure(after, key, selected, value)
            == source_measure(before, key, selected, value) + if key == numeric_key(nation as int) { cell_change(change, part, selected, value) } else { 0 },
    ensures support_measure(after, support, selected, value) == support_measure(before, support, selected, value)
        + link_count(support, nation) * cell_change(change, part, selected, value),
{
    let old_weight = |row: (KeyView, i128)| source_measure(before, row.0, selected, value) * row.1 as int;
    let new_weight = |row: (KeyView, i128)| source_measure(after, row.0, selected, value) * row.1 as int;
    let delta_weight = |row: (KeyView, i128)| if row.0 == numeric_key(nation as int) { row.1 as int * cell_change(change, part, selected, value) } else { 0 };
    support.model();
    assert forall|position: int| 0 <= position < support.entries().len() implies new_weight(#[trigger] support.entries()[position]) == old_weight(support.entries()[position]) + delta_weight(support.entries()[position]) by {
        let row = support.entries()[position]; assert(support.at(row.0).is_some());
        assert(new_weight(row) == old_weight(row) + delta_weight(row)) by (nonlinear_arith)
            requires source_measure(after, row.0, selected, value) == source_measure(before, row.0, selected, value)
                + if row.0 == numeric_key(nation as int) { cell_change(change, part, selected, value) } else { 0 },
                new_weight(row) == source_measure(after, row.0, selected, value) * row.1 as int,
                old_weight(row) == source_measure(before, row.0, selected, value) * row.1 as int,
                delta_weight(row) == if row.0 == numeric_key(nation as int) { row.1 as int * cell_change(change, part, selected, value) } else { 0 };
    }
    let combined = |row: (KeyView, i128)| old_weight(row) + delta_weight(row);
    lemma_sum_ext(support.entries(), new_weight, combined);
    lemma_sum_add(support.entries(), old_weight, delta_weight, combined);
    crate::staging::lemma_link_finish(support, nation, cell_change(change, part, selected, value), delta_weight);
}

pub proof fn lemma_binding_cell(before: Data, after: Data, nation: i64, part: i64, change: Moment)
    requires indexed_names_ok(before), symmetric(before), after.structures_valid(), same_dimensions(before, after), same_shape(before.names, after.names),
        counts_layout(before.nations_reverse, false, true),
        forall|key: KeyView, selected: Option<i64>, value: bool| before.inventories.at(key).is_some() || after.inventories.at(key).is_some() || numeric(key) ==>
            #[trigger] source_measure(after.inventories, key, selected, value) == source_measure(before.inventories, key, selected, value)
                + if key == numeric_key(nation as int) { cell_change(change, part, selected, value) } else { 0 },
        forall|key: KeyView, selected: Option<i64>, value: bool| #[trigger] name_measure(after.names, key, selected, value)
            == name_measure(before.names, key, selected, value)
                + if merged_at(before.names, key) { count_at(before.nations, numeric_key(nation as int), key) * cell_change(change, part, selected, value) } else { 0 },
    ensures indexed_names_ok(after),
{
    assert forall|key: KeyView| #[trigger] binding_ok(after, key) by {
        assert(binding_ok(before, key));
        if let Some(support) = before.nations_reverse.at(key) {
            assert(key.0 == 0);
            if let NameView::Merged { inventory } = after.names.at(key).unwrap() {
                assert(merged_at(before.names, key));
                assert forall|selected: Option<i64>, value: bool| #[trigger] measure(inventory, selected, value)
                    == support_measure(after.inventories, support, selected, value) by {
                    assert(name_measure(before.names, key, selected, value) == support_measure(before.inventories, support, selected, value));
                    assert forall|inner: KeyView| (#[trigger] support.at(inner)).is_some() implies source_measure(after.inventories, inner, selected, value)
                        == source_measure(before.inventories, inner, selected, value) + if inner == numeric_key(nation as int) { cell_change(change, part, selected, value) } else { 0 } by {
                        assert(numeric(inner));
                    }
                    lemma_support_cell(before.inventories, after.inventories, support, nation, part, change, selected, value);
                    assert(count_at(before.nations, numeric_key(nation as int), text_key(key.1)) == count_at(before.nations_reverse, text_key(key.1), numeric_key(nation as int)));
                    assert(count_at(before.nations, numeric_key(nation as int), key) == link_count(support, nation));
                    assert(name_measure(after.names, key, selected, value) == name_measure(before.names, key, selected, value)
                        + count_at(before.nations, numeric_key(nation as int), key) * cell_change(change, part, selected, value));
                    assert(support_measure(after.inventories, support, selected, value) == support_measure(before.inventories, support, selected, value)
                        + link_count(support, nation) * cell_change(change, part, selected, value));
                }
            }
        }
    }
}

pub fn update_nation_names(data: &mut Data, names: &Index<i128>, part: i64, change: Moment) -> (result: bool)
    requires old(data).structures_valid(), names.wf(),
    ensures final(data).structures_valid(), same_dimensions(*old(data), *final(data)), final(data).inventories == old(data).inventories,
        same_shape(old(data).names, final(data).names),
        result ==> forall|key: KeyView, selected: Option<i64>, value: bool| #[trigger] name_measure(final(data).names, key, selected, value)
            == name_measure(old(data).names, key, selected, value) + if merged_at(old(data).names, key) {
                verus_prooflib::ordered_weight_at(*names, key, |row: (KeyView, i128)| row.1 as int) * cell_change(change, part, selected, value)
            } else { 0 },
{
    let ghost initial = *data;
    let mut processed = Ghost(0int);
    let mut cursor = names.cursor();
    loop
        invariant data.structures_valid(), same_dimensions(initial, *data), data.inventories == initial.inventories, initial == *old(data),
            same_shape(initial.names, data.names), names.wf(),
            0 <= processed@ <= names.entries().len(), cursor.remaining() == names.entries().skip(processed@),
            forall|key: KeyView, selected: Option<i64>, value: bool| #[trigger] name_measure(data.names, key, selected, value)
                == name_measure(initial.names, key, selected, value) + sum(names.entries().take(processed@), |row: (KeyView, i128)|
                    if row.0 == key && merged_at(initial.names, key) { row.1 as int * cell_change(change, part, selected, value) } else { 0 }),
        decreases cursor.remaining().len(),
    {
        let (name, multiplicity) = match cursor.next() {
            Some(row) => row,
            None => {
                proof {
                    assert(processed@ == names.entries().len()); assert(names.entries().take(processed@) =~= names.entries());
                    assert forall|key: KeyView, selected: Option<i64>, value: bool| #[trigger] name_measure(data.names, key, selected, value)
                        == name_measure(initial.names, key, selected, value) + if merged_at(initial.names, key) {
                            verus_prooflib::ordered_weight_at(*names, key, |row: (KeyView, i128)| row.1 as int) * cell_change(change, part, selected, value)
                        } else { 0 } by {
                        let weight = |row: (KeyView, i128)| if row.0 == key && merged_at(initial.names, key) { row.1 as int * cell_change(change, part, selected, value) } else { 0 };
                        if merged_at(initial.names, key) {
                            verus_prooflib::lemma_ordered_selected_weight(names, key, |row: (KeyView, i128)| row.1 as int, cell_change(change, part, selected, value), weight);
                        } else { lemma_sum_zero(names.entries(), weight); }
                        assert(name_measure(data.names, key, selected, value) == name_measure(initial.names, key, selected, value) + sum(names.entries(), weight));
                    }
                }
                return true;
            },
        };
        let ghost previous = *data;
        if !change_name(data, &name, part, change, *multiplicity) { return false; }
        proof {
            lemma_shape_transitive(initial.names, previous.names, data.names);
            assert forall|key: KeyView, selected: Option<i64>, value: bool| #[trigger] name_measure(data.names, key, selected, value)
                == name_measure(initial.names, key, selected, value) + sum(names.entries().take(processed@ + 1), |row: (KeyView, i128)|
                    if row.0 == key && merged_at(initial.names, key) { row.1 as int * cell_change(change, part, selected, value) } else { 0 }) by {
                let weight = |row: (KeyView, i128)| if row.0 == key && merged_at(initial.names, key) { row.1 as int * cell_change(change, part, selected, value) } else { 0 };
                lemma_sum_take(names.entries(), processed@, weight);
                assert(names.entries()[processed@] == (name.view(), *multiplicity));
                assert(merged_at(previous.names, key) == merged_at(initial.names, key));
                assert(name_measure(previous.names, key, selected, value) == name_measure(initial.names, key, selected, value) + sum(names.entries().take(processed@), weight));
                assert(cell_change(change, part, selected, value) * *multiplicity as int == *multiplicity as int * cell_change(change, part, selected, value)) by (nonlinear_arith);
            }
        }
        processed = Ghost(processed@ + 1);
    }
}

pub proof fn lemma_inventory_sources(before: Data, after: Data, nation: i64, part: i64, change: Moment)
    requires before.structures_valid(), after.structures_valid(),
        inventory_at(after.inventories, nation as int, part as int).count as int == inventory_at(before.inventories, nation as int, part as int).count as int + change.count as int,
        inventory_at(after.inventories, nation as int, part as int).value as int == inventory_at(before.inventories, nation as int, part as int).value as int + change.value as int,
        inventory_total(after.inventories, nation as int).count as int == inventory_total(before.inventories, nation as int).count as int + change.count as int,
        inventory_total(after.inventories, nation as int).value as int == inventory_total(before.inventories, nation as int).value as int + change.value as int,
        forall|other_nation: int, other_part: int| other_nation != nation || other_part != part ==> #[trigger] inventory_at(after.inventories, other_nation, other_part) == inventory_at(before.inventories, other_nation, other_part),
        forall|other_nation: int| other_nation != nation ==> #[trigger] inventory_total(after.inventories, other_nation) == inventory_total(before.inventories, other_nation),
    ensures forall|key: KeyView, selected: Option<i64>, value: bool| #[trigger] source_measure(after.inventories, key, selected, value)
        == source_measure(before.inventories, key, selected, value) + if key == numeric_key(nation as int) { cell_change(change, part, selected, value) } else { 0 },
{
    assert forall|key: KeyView, selected: Option<i64>, value: bool| #[trigger] source_measure(after.inventories, key, selected, value)
        == source_measure(before.inventories, key, selected, value) + if key == numeric_key(nation as int) { cell_change(change, part, selected, value) } else { 0 } by {
        if !numeric(key) {
            assert(before.inventories.at(key).is_none()); assert(after.inventories.at(key).is_none());
        } else {
            assert(source_measure(after.inventories, key, selected, value) == inventory_measure(after.inventories, key.0 as i64, selected, value));
            assert(source_measure(before.inventories, key, selected, value) == inventory_measure(before.inventories, key.0 as i64, selected, value));
            if let Some(selected) = selected {
                if key.0 != nation || selected != part { assert(inventory_at(after.inventories, key.0, selected as int) == inventory_at(before.inventories, key.0, selected as int)); }
            } else if key.0 != nation { assert(inventory_total(after.inventories, key.0) == inventory_total(before.inventories, key.0)); }
        }
    }
}

pub open spec fn outer_measure(inventories: Index<Inventory>, counts: Counts, name: KeyView, part: Option<i64>, value: bool) -> int {
    match counts.at(name) { Some(support) => support_measure(inventories, support, part, value), None => 0 }
}

pub proof fn lemma_support_transpose(inventories: Index<Inventory>, support: Index<i128>, part: Option<i64>, value: bool)
    requires inventories.wf(), support.wf(),
    ensures support_measure(inventories, support, part, value)
        == sum(inventories.entries(), |row: (KeyView, Inventory)| measure(row.1, part, value)
            * verus_prooflib::ordered_weight_at(support, row.0, |entry: (KeyView, i128)| entry.1 as int)),
{
    let inventory_weight = |row: (KeyView, Inventory)| measure(row.1, part, value);
    let count_weight = |row: (KeyView, i128)| row.1 as int;
    verus_prooflib::lemma_ordered_dot_transpose(&inventories, &support, inventory_weight, count_weight);
    let original = |row: (KeyView, i128)| source_measure(inventories, row.0, part, value) * row.1 as int;
    let dot_weight = |row: (KeyView, i128)| count_weight(row) * verus_prooflib::ordered_weight_at(inventories, row.0, inventory_weight);
    assert forall|row: (KeyView, i128)| #[trigger] original(row) == dot_weight(row) by {
        assert(source_measure(inventories, row.0, part, value) == verus_prooflib::ordered_weight_at(inventories, row.0, inventory_weight));
        assert(original(row) == dot_weight(row)) by (nonlinear_arith)
            requires original(row) == source_measure(inventories, row.0, part, value) * row.1 as int,
                dot_weight(row) == row.1 as int * source_measure(inventories, row.0, part, value);
    }
    lemma_sum_ext_all(support.entries(), original, dot_weight);
    let dot_inventory = |row: (KeyView, Inventory)| inventory_weight(row) * verus_prooflib::ordered_weight_at(support, row.0, count_weight);
    let target = |row: (KeyView, Inventory)| measure(row.1, part, value) * verus_prooflib::ordered_weight_at(support, row.0, |entry: (KeyView, i128)| entry.1 as int);
    lemma_sum_ext_all(inventories.entries(), dot_inventory, target);
}

pub proof fn lemma_outer_transpose(inventories: Index<Inventory>, counts: Counts, name: KeyView, part: Option<i64>, value: bool)
    requires inventories.wf(), counts_layout(counts, false, true),
    ensures outer_measure(inventories, counts, name, part, value)
        == sum(inventories.entries(), |row: (KeyView, Inventory)| measure(row.1, part, value) * count_at(counts, name, row.0)),
{
    if let Some(support) = counts.at(name) {
        lemma_support_transpose(inventories, support, part, value);
        let original = |row: (KeyView, Inventory)| measure(row.1, part, value)
            * verus_prooflib::ordered_weight_at(support, row.0, |entry: (KeyView, i128)| entry.1 as int);
        let target = |row: (KeyView, Inventory)| measure(row.1, part, value) * count_at(counts, name, row.0);
        lemma_sum_ext_all(inventories.entries(), original, target);
    } else { lemma_sum_zero(inventories.entries(), |row: (KeyView, Inventory)| measure(row.1, part, value) * count_at(counts, name, row.0)); }
}

pub proof fn lemma_support_count_step(inventories: Index<Inventory>, before: Counts, after: Counts, name: KeyView, changes: Index<i128>, part: Option<i64>, value: bool)
    requires inventories.wf(), counts_layout(before, false, true), counts_layout(after, false, true), changes.wf(),
        forall|key: KeyView| #[trigger] count_at(after, name, key) == count_at(before, name, key)
            + verus_prooflib::ordered_weight_at(changes, key, |row: (KeyView, i128)| row.1 as int),
    ensures outer_measure(inventories, after, name, part, value) == outer_measure(inventories, before, name, part, value)
        + support_measure(inventories, changes, part, value),
{
    lemma_outer_transpose(inventories, before, name, part, value);
    lemma_outer_transpose(inventories, after, name, part, value);
    lemma_support_transpose(inventories, changes, part, value);
    let old_weight = |row: (KeyView, Inventory)| measure(row.1, part, value) * count_at(before, name, row.0);
    let new_weight = |row: (KeyView, Inventory)| measure(row.1, part, value) * count_at(after, name, row.0);
    let delta_weight = |row: (KeyView, Inventory)| measure(row.1, part, value) * verus_prooflib::ordered_weight_at(changes, row.0, |entry: (KeyView, i128)| entry.1 as int);
    assert forall|row: (KeyView, Inventory)| #[trigger] new_weight(row) == old_weight(row) + delta_weight(row) by {
        let count = verus_prooflib::ordered_weight_at(changes, row.0, |entry: (KeyView, i128)| entry.1 as int);
        assert(count_at(after, name, row.0) == count_at(before, name, row.0) + count);
        assert(new_weight(row) == old_weight(row) + delta_weight(row)) by (nonlinear_arith)
            requires count_at(after, name, row.0) == count_at(before, name, row.0) + count,
                new_weight(row) == measure(row.1, part, value) * count_at(after, name, row.0),
                old_weight(row) == measure(row.1, part, value) * count_at(before, name, row.0),
                delta_weight(row) == measure(row.1, part, value) * count;
    }
    lemma_sum_add(inventories.entries(), old_weight, delta_weight, new_weight);
}

pub fn merge_changes(target: &mut Inventory, data: &Data, changes: &Index<i128>) -> (result: bool)
    requires old(target).valid(), data.structures_valid(), changes.wf(),
    ensures final(target).valid(),
        result ==> forall|part: Option<i64>, value: bool| #[trigger] measure(*final(target), part, value)
            == measure(*old(target), part, value) + support_measure(data.inventories, *changes, part, value),
{
    let ghost initial = *target;
    let mut processed = Ghost(0int);
    let mut cursor = changes.cursor();
    loop
        invariant target.valid(), data.structures_valid(), changes.wf(), initial == *old(target),
            0 <= processed@ <= changes.entries().len(), cursor.remaining() == changes.entries().skip(processed@),
            forall|part: Option<i64>, value: bool| #[trigger] measure(*target, part, value)
                == measure(initial, part, value) + sum(changes.entries().take(processed@), |row: (KeyView, i128)| source_measure(data.inventories, row.0, part, value) * row.1 as int),
        decreases cursor.remaining().len(),
    {
        let (nation, weight) = match cursor.next() {
            Some(row) => row,
            None => {
                proof { assert(processed@ == changes.entries().len()); assert(changes.entries().take(processed@) =~= changes.entries()); }
                return true;
            },
        };
        let ghost previous = *target;
        if let Some(source) = data.inventories.get(&nation) {
            if !crate::maintenance::merge_inventory(target, &*source, *weight) { return false; }
        }
        proof {
            assert forall|part: Option<i64>, value: bool| #[trigger] measure(*target, part, value)
                == measure(initial, part, value) + sum(changes.entries().take(processed@ + 1), |row: (KeyView, i128)| source_measure(data.inventories, row.0, part, value) * row.1 as int) by {
                let selected = |row: (KeyView, i128)| source_measure(data.inventories, row.0, part, value) * row.1 as int;
                lemma_sum_take(changes.entries(), processed@, selected);
                assert(measure(previous, part, value) == measure(initial, part, value) + sum(changes.entries().take(processed@), selected));
                if let Some(part) = part {
                    if let Some(source) = data.inventories.at(nation.view()) {
                        assert(moment_at(target.groups, part as int).count as int == moment_at(previous.groups, part as int).count as int + moment_at(source.groups, part as int).count as int * *weight as int);
                        assert(moment_at(target.groups, part as int).value as int == moment_at(previous.groups, part as int).value as int + moment_at(source.groups, part as int).value as int * *weight as int);
                    }
                }
            }
        }
        processed = Ghost(processed@ + 1);
    }
}

}
