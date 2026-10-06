use crate::packed::{self, Packed, PackedCursor};
use vstd::prelude::*;
use verus_common::sums::*;
use verus_prooflib::ordered::{Index, KeyView};
use verus_prooflib::{ordered_dot, ordered_weight_at, lemma_ordered_dot_transpose};
use crate::model::*;
use crate::construction::*;
use crate::maintenance::{partsupp_effect, supplier_effect, profile_cell};
use crate::inventory::moment_at;
use crate::values::Moment;
use crate::gen_spec::*;
use crate::semantics::db_bounded;

verus! {

pub open spec fn joined(profiles: Profiles, suppliers: Counts, nation: i64, part: i64, value: bool) -> int {
    sum(suppliers.entries(), |row: (KeyView, Index<i128>)| supplier_effect(row, profiles, nation, part, value))
}

pub proof fn lemma_transpose(profiles: Profiles, suppliers: Counts, nation: i64, part: i64, value: bool)
    requires profile_layout(profiles), counts_layout(suppliers, true, true),
    ensures joined(profiles, suppliers, nation, part, value)
        == sum(profiles.entries(), |row: (KeyView, Packed)| partsupp_effect(row, suppliers, nation, part, value)),
{
    let profile_weight = |row: (KeyView, Packed)| moment_measure(moment_at(row.1, part as int), value);
    let supplier_weight = |row: (KeyView, Index<i128>)| link_count(row.1, nation);
    lemma_ordered_dot_transpose(&profiles, &suppliers, profile_weight, supplier_weight);
    let left = |row: (KeyView, Packed)| partsupp_effect(row, suppliers, nation, part, value);
    let dot_left = |row: (KeyView, Packed)| profile_weight(row) * ordered_weight_at(suppliers, row.0, supplier_weight);
    lemma_sum_ext_all(profiles.entries(), left, dot_left);
    let right = |row: (KeyView, Index<i128>)| supplier_effect(row, profiles, nation, part, value);
    let dot_right = |row: (KeyView, Index<i128>)| supplier_weight(row) * ordered_weight_at(profiles, row.0, profile_weight);
    suppliers.model();
    assert forall|position: int| 0 <= position < suppliers.entries().len() implies right(#[trigger] suppliers.entries()[position]) == dot_right(suppliers.entries()[position]) by {
        let row = suppliers.entries()[position];
        assert(suppliers.at(row.0) == Some(row.1)); assert(numeric(row.0));
        assert(moment_measure(profile_at(profiles, row.0.0, part as int), value) == ordered_weight_at(profiles, row.0, profile_weight));
        assert(right(row) == dot_right(row)) by (nonlinear_arith)
            requires right(row) == ordered_weight_at(profiles, row.0, profile_weight) * supplier_weight(row),
                dot_right(row) == supplier_weight(row) * ordered_weight_at(profiles, row.0, profile_weight);
    }
    lemma_sum_ext(suppliers.entries(), right, dot_right);
}

pub proof fn lemma_partsupp_join(before: Profiles, after: Profiles, changes: Profiles, suppliers: Counts, nation: i64, part: i64, value: bool)
    requires counts_layout(suppliers, true, true),
        forall|supplier: i64, selected_part: i64, selected_value: bool| #[trigger] profile_cell(after, supplier, selected_part, selected_value)
            == profile_cell(before, supplier, selected_part, selected_value) + profile_cell(changes, supplier, selected_part, selected_value),
    ensures joined(after, suppliers, nation, part, value) == joined(before, suppliers, nation, part, value) + joined(changes, suppliers, nation, part, value),
{
    let old_weight = |row: (KeyView, Index<i128>)| supplier_effect(row, before, nation, part, value);
    let delta_weight = |row: (KeyView, Index<i128>)| supplier_effect(row, changes, nation, part, value);
    let new_weight = |row: (KeyView, Index<i128>)| supplier_effect(row, after, nation, part, value);
    suppliers.model();
    assert forall|position: int| 0 <= position < suppliers.entries().len() implies new_weight(#[trigger] suppliers.entries()[position]) == old_weight(suppliers.entries()[position]) + delta_weight(suppliers.entries()[position]) by {
        let row = suppliers.entries()[position];
        assert(suppliers.at(row.0) == Some(row.1)); assert(numeric(row.0));
        let supplier = row.0.0 as i64;
        assert(profile_cell(after, supplier, part, value) == profile_cell(before, supplier, part, value) + profile_cell(changes, supplier, part, value));
        assert(new_weight(row) == old_weight(row) + delta_weight(row)) by (nonlinear_arith)
            requires profile_cell(after, supplier, part, value) == profile_cell(before, supplier, part, value) + profile_cell(changes, supplier, part, value),
                new_weight(row) == profile_cell(after, supplier, part, value) * link_count(row.1, nation),
                old_weight(row) == profile_cell(before, supplier, part, value) * link_count(row.1, nation),
                delta_weight(row) == profile_cell(changes, supplier, part, value) * link_count(row.1, nation);
    }
    let combined = |row: (KeyView, Index<i128>)| old_weight(row) + delta_weight(row);
    lemma_sum_ext(suppliers.entries(), new_weight, combined);
    lemma_sum_add(suppliers.entries(), old_weight, delta_weight, combined);
}

pub proof fn lemma_supplier_join(profiles: Profiles, before: Counts, after: Counts, changes: Counts, nation: i64, part: i64, value: bool)
    requires profile_layout(profiles), counts_layout(before, true, true), counts_layout(after, true, true), counts_layout(changes, true, true),
        forall|outer: KeyView, inner: KeyView| #[trigger] count_at(after, outer, inner) == count_at(before, outer, inner) + count_at(changes, outer, inner),
    ensures joined(profiles, after, nation, part, value) == joined(profiles, before, nation, part, value) + joined(profiles, changes, nation, part, value),
{
    lemma_transpose(profiles, before, nation, part, value);
    lemma_transpose(profiles, after, nation, part, value);
    lemma_transpose(profiles, changes, nation, part, value);
    let old_weight = |row: (KeyView, Packed)| partsupp_effect(row, before, nation, part, value);
    let delta_weight = |row: (KeyView, Packed)| partsupp_effect(row, changes, nation, part, value);
    let new_weight = |row: (KeyView, Packed)| partsupp_effect(row, after, nation, part, value);
    assert forall|row: (KeyView, Packed)| #[trigger] new_weight(row) == old_weight(row) + delta_weight(row) by {
        assert(count_at(after, row.0, numeric_key(nation as int)) == count_at(before, row.0, numeric_key(nation as int)) + count_at(changes, row.0, numeric_key(nation as int)));
        assert(new_weight(row) == old_weight(row) + delta_weight(row)) by (nonlinear_arith)
            requires count_at(after, row.0, numeric_key(nation as int)) == count_at(before, row.0, numeric_key(nation as int)) + count_at(changes, row.0, numeric_key(nation as int)),
                new_weight(row) == moment_measure(moment_at(row.1, part as int), value) * count_at(after, row.0, numeric_key(nation as int)),
                old_weight(row) == moment_measure(moment_at(row.1, part as int), value) * count_at(before, row.0, numeric_key(nation as int)),
                delta_weight(row) == moment_measure(moment_at(row.1, part as int), value) * count_at(changes, row.0, numeric_key(nation as int));
    }
    lemma_sum_add(profiles.entries(), old_weight, delta_weight, new_weight);
}

pub proof fn lemma_database_join(data: Data, db: Db, nation: i64, part: i64, value: bool)
    requires data.profiles_ok(db), data.suppliers_ok(db), db_bounded(db),
    ensures joined(data.profiles, data.suppliers, nation, part, value) == if value { h_value(db, nation as int, part as int) } else { h_count(db, nation as int, part as int) },
{
    lemma_inventory_part(data, db, nation, part, value);
    let original = |row: (KeyView, Index<i128>)| supplier_measure(data.profiles, row, nation, Some(part), value);
    let target = |row: (KeyView, Index<i128>)| supplier_effect(row, data.profiles, nation, part, value);
    data.suppliers.model();
    assert forall|position: int| 0 <= position < data.suppliers.entries().len() implies original(#[trigger] data.suppliers.entries()[position]) == target(data.suppliers.entries()[position]) by {
        let row = data.suppliers.entries()[position];
        assert(data.suppliers.at(row.0) == Some(row.1)); assert(numeric(row.0));
        lemma_profile_part(data.profiles, row.0, part, value);
    }
    lemma_sum_ext(data.suppliers.entries(), original, target);
}

pub proof fn lemma_inventory_commit(before: Data, after: Data, db: Db, db2: Db, parts: Profiles, suppliers: Counts, changes: Profiles)
    requires before.valid(db), after.structures_valid(), after.profiles_ok(db2), after.suppliers_ok(db2), db_bounded(db2),
        profile_layout(parts), counts_layout(suppliers, true, true), profile_layout(changes),
        forall|supplier: i64, part: i64, value: bool| #[trigger] profile_cell(after.profiles, supplier, part, value)
            == profile_cell(before.profiles, supplier, part, value) + profile_cell(parts, supplier, part, value),
        forall|outer: KeyView, inner: KeyView| #[trigger] count_at(after.suppliers, outer, inner)
            == count_at(before.suppliers, outer, inner) + count_at(suppliers, outer, inner),
        forall|nation: i64, part: i64, value: bool| #[trigger] profile_cell(changes, nation, part, value)
            == sum(parts.entries(), |row: (KeyView, Packed)| partsupp_effect(row, before.suppliers, nation, part, value))
                + sum(suppliers.entries(), |row: (KeyView, Index<i128>)| supplier_effect(row, after.profiles, nation, part, value)),
        forall|nation: i64, part: Option<i64>, value: bool| #[trigger] inventory_measure(after.inventories, nation, part, value)
            == inventory_measure(before.inventories, nation, part, value) + profile_measure_at(changes, numeric_key(nation as int), part, value),
    ensures after.inventories_ok(db2),
{
    assert forall|nation: i64, part: i64| #[trigger] inventory_at(after.inventories, nation as int, part as int).count as int == h_count(db2, nation as int, part as int)
        && inventory_at(after.inventories, nation as int, part as int).value as int == h_value(db2, nation as int, part as int) by {
        lemma_inventory_commit_part(before, after, db, db2, parts, suppliers, changes, nation, part, false);
        lemma_inventory_commit_part(before, after, db, db2, parts, suppliers, changes, nation, part, true);
    }
    assert forall|key: KeyView| (#[trigger] after.inventories.at(key)).is_some() implies {
        let inventory = after.inventories.at(key).unwrap();
        &&& numeric(key) && inventory.valid() && inventory.total.count > 0
        &&& inventory.total.count as int == h_total_count(db2, key.0)
        &&& inventory.total.value as int == h_total_value(db2, key.0)
    } by {
        assert(numeric(key));
        lemma_inventory_total(after, db2, key.0 as i64, false);
        lemma_inventory_total(after, db2, key.0 as i64, true);
    }
}

pub proof fn lemma_inventory_commit_part(before: Data, after: Data, db: Db, db2: Db, parts: Profiles, suppliers: Counts, changes: Profiles, nation: i64, part: i64, value: bool)
    requires before.valid(db), after.profiles_ok(db2), after.suppliers_ok(db2), db_bounded(db2),
        profile_layout(parts), counts_layout(suppliers, true, true), profile_layout(changes),
        forall|supplier: i64, selected_part: i64, selected_value: bool| #[trigger] profile_cell(after.profiles, supplier, selected_part, selected_value)
            == profile_cell(before.profiles, supplier, selected_part, selected_value) + profile_cell(parts, supplier, selected_part, selected_value),
        forall|outer: KeyView, inner: KeyView| #[trigger] count_at(after.suppliers, outer, inner)
            == count_at(before.suppliers, outer, inner) + count_at(suppliers, outer, inner),
        profile_cell(changes, nation, part, value)
            == sum(parts.entries(), |row: (KeyView, Packed)| partsupp_effect(row, before.suppliers, nation, part, value))
                + sum(suppliers.entries(), |row: (KeyView, Index<i128>)| supplier_effect(row, after.profiles, nation, part, value)),
        inventory_measure(after.inventories, nation, Some(part), value)
            == inventory_measure(before.inventories, nation, Some(part), value) + profile_measure_at(changes, numeric_key(nation as int), Some(part), value),
    ensures inventory_measure(after.inventories, nation, Some(part), value) == if value { h_value(db2, nation as int, part as int) } else { h_count(db2, nation as int, part as int) },
{
    lemma_database_join(before, db, nation, part, value);
    lemma_database_join(after, db2, nation, part, value);
    lemma_transpose(parts, before.suppliers, nation, part, value);
    lemma_partsupp_join(before.profiles, after.profiles, parts, before.suppliers, nation, part, value);
    lemma_supplier_join(after.profiles, before.suppliers, after.suppliers, suppliers, nation, part, value);
    lemma_profile_part(changes, numeric_key(nation as int), part, value);
    assert(inventory_measure(before.inventories, nation, Some(part), value) == joined(before.profiles, before.suppliers, nation, part, value));
}

}
