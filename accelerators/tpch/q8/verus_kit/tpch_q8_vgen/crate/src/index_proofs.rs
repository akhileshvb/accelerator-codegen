use vstd::prelude::*;
use verus_prooflib::*;
use verus_common::sums::*;
use crate::maintenance::*;
use crate::model::*;
use crate::table::*;

verus! {

pub proof fn lemma_row_count_add(before: Table, changes: Table, after: Table, key: Major)
    requires forall|minor: Major| #[trigger] after.at(key, minor, false) == before.at(key, minor, false) + changes.at(key, minor, false),
    ensures sum(after.rows(key), |cell: Cell| cell.count as int) ==
        sum(before.rows(key), |cell: Cell| cell.count as int) + sum(changes.rows(key), |cell: Cell| cell.count as int),
{
    hide(sum);
    reveal(Table::at);
    reveal(cell_sum);
    let old_rows = before.rows(key);
    let delta_rows = changes.rows(key);
    let joined = old_rows + delta_rows;
    let new_rows = after.rows(key);
    let project = |cell: Cell| minor_of(cell);
    let count = |cell: Cell| cell.count as int;
    assert forall|minor: Major| #[trigger] class_total(new_rows, project, count, minor) ==
        class_total(joined, project, count, minor) by {
        let weight = |cell: Cell| cell_weight(cell, minor, false);
        lemma_sum_concat(old_rows, delta_rows, weight);
        lemma_sum_ext_all(new_rows, |cell: Cell| if project(cell) == minor { count(cell) } else { 0int }, weight);
        lemma_sum_ext_all(joined, |cell: Cell| if project(cell) == minor { count(cell) } else { 0int }, weight);
        assert(after.at(key, minor, false) == before.at(key, minor, false) + changes.at(key, minor, false));
    }
    lemma_equal_classes(new_rows, joined, project, project, count, count, |minor: Major| 1int);
    lemma_sum_ext_all(new_rows, |cell: Cell| count(cell) * (|minor: Major| 1int)(project(cell)), count);
    lemma_sum_ext_all(joined, |cell: Cell| count(cell) * (|minor: Major| 1int)(project(cell)), count);
    lemma_sum_concat(old_rows, delta_rows, count);
}

pub proof fn lemma_apply_index_meaning(before: Data, data: Data, changes: Table)
    requires index_meaning(before), changes.valid(), base_key_shape(changes),
        forall|key: Major, minor: Major| #[trigger] data.base.at(key, minor, false) == before.base.at(key, minor, false) + changes.at(key, minor, false),
        forall|key: Major, minor: Major, measure: bool| #[trigger] data.base_by_customer.at(key, minor, measure)
            == before.base_by_customer.at(key, minor, measure) + pair_index_effect(changes.keys_view(), changes, key, minor, false, measure),
        forall|key: Major, minor: Major, measure: bool| #[trigger] data.base_by_supplier.at(key, minor, measure)
            == before.base_by_supplier.at(key, minor, measure) + pair_index_effect(changes.keys_view(), changes, key, minor, true, measure),
    ensures index_meaning(data),
{
    hide(sum);
    reveal(index_meaning);
    assert forall|customer: i64, supplier: i64| #[trigger] data.base_by_customer.at((customer, 0, 0), (supplier, 0, 0), false) ==
        sum(data.base.rows((customer, supplier, 0)), |cell: Cell| cell.count as int) by {
        lemma_pair_index_effect(changes, (customer, 0, 0), (supplier, 0, 0), false, false);
        lemma_row_count_add(before.base, changes, data.base, (customer, supplier, 0));
    }
    assert forall|supplier: i64, customer: i64| #[trigger] data.base_by_supplier.at((supplier, 0, 0), (customer, 0, 0), false) ==
        sum(data.base.rows((customer, supplier, 0)), |cell: Cell| cell.count as int) by {
        lemma_pair_index_effect(changes, (supplier, 0, 0), (customer, 0, 0), true, false);
        lemma_row_count_add(before.base, changes, data.base, (customer, supplier, 0));
    }
}

pub proof fn lemma_apply_indexes(before: Data, data: Data, changes: Table)
    requires runtime_valid(before), index_meaning(before), changes.valid(), base_key_shape(changes),
        data.base.valid(), positive(data.base), base_key_shape(data.base), data.base_by_customer.valid(), data.base_by_supplier.valid(),
        forall|key: Major, minor: Major| #[trigger] data.base.at(key, minor, false) == before.base.at(key, minor, false) + changes.at(key, minor, false),
        forall|key: Major, minor: Major, measure: bool| #[trigger] data.base_by_customer.at(key, minor, measure)
            == before.base_by_customer.at(key, minor, measure) + pair_index_effect(changes.keys_view(), changes, key, minor, false, measure),
        forall|key: Major, minor: Major, measure: bool| #[trigger] data.base_by_supplier.at(key, minor, measure)
            == before.base_by_supplier.at(key, minor, measure) + pair_index_effect(changes.keys_view(), changes, key, minor, true, measure),
    ensures index_meaning(data), positive(data.base_by_customer), positive(data.base_by_supplier), scalar(data.base_by_customer), scalar(data.base_by_supplier),
{
    hide(sum);
    lemma_runtime_access(before);
    lemma_apply_index_meaning(before, data, changes);
    reveal(index_meaning);
    crate::dimensions::lemma_support_from_cells(before.base_by_customer, before.dictionary, 1);
    crate::dimensions::lemma_support_from_cells(before.base_by_supplier, before.dictionary, 1);
    assert forall|key: Major, minor: Major, measure: bool| #[trigger] data.base_by_customer.at(key, minor, measure) ==
        pair_index_effect(data.base.keys_view(), data.base, key, minor, false, measure) by {
        if measure || !crate::dimensions::dimension_support(key, minor, before.dictionary, 1) {
            lemma_pair_index_support(changes, key, minor, false, measure);
            lemma_pair_index_support(data.base, key, minor, false, measure);
        } else {
            lemma_pair_index_effect(data.base, key, minor, false, measure);
        }
    }
    assert forall|key: Major, minor: Major, measure: bool| #[trigger] data.base_by_supplier.at(key, minor, measure) ==
        pair_index_effect(data.base.keys_view(), data.base, key, minor, true, measure) by {
        if measure || !crate::dimensions::dimension_support(key, minor, before.dictionary, 1) {
            lemma_pair_index_support(changes, key, minor, true, measure);
            lemma_pair_index_support(data.base, key, minor, true, measure);
        } else {
            lemma_pair_index_effect(data.base, key, minor, true, measure);
        }
    }
    lemma_build_indexes(data);
}

pub open spec fn base_key_shape(base: Table) -> bool {
    forall|key: Major, index: int| 0 <= index < base.rows(key).len() ==>
        (#[trigger] base.rows(key)[index]).third == 0 && key.2 == 0
}

pub proof fn lemma_pair_index_effect(changes: Table, key: Major, minor: Major, supplier: bool, measure: bool)
    requires changes.valid(), base_key_shape(changes), key.1 == 0, key.2 == 0, minor.1 == 0, minor.2 == 0, !measure,
    ensures pair_index_effect(changes.keys_view(), changes, key, minor, supplier, measure)
        == sum(changes.rows(if supplier { (minor.0, key.0, 0) } else { (key.0, minor.0, 0) }), |cell: Cell| cell.count as int),
{
    hide(sum);
    lemma_keys_complete(changes);
    lemma_keys_unique(changes);
    reveal(pair_index_effect);
    let keys = changes.keys_view();
    let target = if supplier { (minor.0, key.0, 0i64) } else { (key.0, minor.0, 0i64) };
    let value = sum(changes.rows(target), |cell: Cell| cell.count as int);
    let raw = |entry: Major| index_weight(entry, sum(changes.rows(entry), |cell: Cell| cell.count as int), key, minor, supplier, measure);
    let selected = |entry: Major| if entry == target { value } else { 0int };
    assert forall|entry: Major| #[trigger] raw(entry) == selected(entry) by {
        if entry.2 != 0 {
            if changes.rows(entry).len() > 0 { assert((changes.rows(entry)[0]).third == 0); }
            assert(changes.rows(entry).len() == 0);
            lemma_sum_empty(changes.rows(entry), |cell: Cell| cell.count as int);
        }
    }
    lemma_sum_ext_all(keys, raw, selected);
    if !keys.contains(target) {
        assert(changes.rows(target).len() == 0);
        lemma_sum_empty(changes.rows(target), |cell: Cell| cell.count as int);
    }
    lemma_sum_indicator_distinct(keys, target, value, selected);
}

pub proof fn lemma_pair_index_support(changes: Table, key: Major, minor: Major, supplier: bool, measure: bool)
    requires measure || key.1 != 0 || key.2 != 0 || minor.1 != 0 || minor.2 != 0,
    ensures pair_index_effect(changes.keys_view(), changes, key, minor, supplier, measure) == 0,
{
    hide(sum);
    reveal(pair_index_effect);
    lemma_sum_zero(changes.keys_view(), |entry: Major| index_weight(entry, sum(changes.rows(entry), |cell: Cell| cell.count as int), key, minor, supplier, measure));
}

pub proof fn lemma_build_indexes(data: Data)
    requires data.base.valid(), positive(data.base), base_key_shape(data.base), data.base_by_customer.valid(), data.base_by_supplier.valid(),
        forall|key: Major, minor: Major, measure: bool| #[trigger] data.base_by_customer.at(key, minor, measure)
            == pair_index_effect(data.base.keys_view(), data.base, key, minor, false, measure),
        forall|key: Major, minor: Major, measure: bool| #[trigger] data.base_by_supplier.at(key, minor, measure)
            == pair_index_effect(data.base.keys_view(), data.base, key, minor, true, measure),
    ensures index_meaning(data), positive(data.base_by_customer), positive(data.base_by_supplier),
        scalar(data.base_by_customer), scalar(data.base_by_supplier),
{
    hide(sum);
    reveal(index_meaning);
    assert forall|customer: i64, supplier: i64| #[trigger] data.base_by_customer.at((customer, 0, 0), (supplier, 0, 0), false)
        == sum(data.base.rows((customer, supplier, 0)), |cell: Cell| cell.count as int) by {
        lemma_pair_index_effect(data.base, (customer, 0, 0), (supplier, 0, 0), false, false);
    }
    assert forall|supplier: i64, customer: i64| #[trigger] data.base_by_supplier.at((supplier, 0, 0), (customer, 0, 0), false)
        == sum(data.base.rows((customer, supplier, 0)), |cell: Cell| cell.count as int) by {
        lemma_pair_index_effect(data.base, (supplier, 0, 0), (customer, 0, 0), true, false);
    }
    assert forall|key: Major, minor: Major, measure: bool| measure || !crate::dimensions::dimension_support(key, minor, data.dictionary, 1)
        implies #[trigger] data.base_by_customer.at(key, minor, measure) == 0 by {
        lemma_pair_index_support(data.base, key, minor, false, measure);
    }
    assert forall|key: Major, minor: Major, measure: bool| measure || !crate::dimensions::dimension_support(key, minor, data.dictionary, 1)
        implies #[trigger] data.base_by_supplier.at(key, minor, measure) == 0 by {
        lemma_pair_index_support(data.base, key, minor, true, measure);
    }
    assert forall|key: Major, minor: Major| #[trigger] data.base_by_customer.at(key, minor, false) >= 0 by {
        if crate::dimensions::dimension_support(key, minor, data.dictionary, 1) {
            let rows = data.base.rows((key.0, minor.0, 0));
            assert forall|index: int| 0 <= index < rows.len() implies (#[trigger] rows[index]).count as int >= 0 by {}
            lemma_sum_nonneg(rows, |cell: Cell| cell.count as int);
        }
    }
    assert forall|key: Major, minor: Major| #[trigger] data.base_by_supplier.at(key, minor, false) >= 0 by {
        if crate::dimensions::dimension_support(key, minor, data.dictionary, 1) {
            let rows = data.base.rows((minor.0, key.0, 0));
            assert forall|index: int| 0 <= index < rows.len() implies (#[trigger] rows[index]).count as int >= 0 by {}
            lemma_sum_nonneg(rows, |cell: Cell| cell.count as int);
        }
    }
    crate::dimensions::lemma_dimension_cells(data.base_by_customer, data.dictionary, 1);
    crate::dimensions::lemma_dimension_cells(data.base_by_supplier, data.dictionary, 1);
}

}
