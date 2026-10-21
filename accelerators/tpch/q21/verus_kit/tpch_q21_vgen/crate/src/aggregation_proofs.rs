use vstd::prelude::*;
use std::sync::Arc;
use verus_common::sums::*;
use verus_prooflib::wide::Wide;
use verus_common::order::*;
use crate::index::*;
use crate::aggregates::*;
use crate::gen_spec::*;

verus! {

pub open spec fn key_order() -> spec_fn(KeyView, KeyView) -> bool { |left: KeyView, right: KeyView| key_lt(left, right) }
pub open spec fn key_list(source: Seq<KeyView>) -> Seq<KeyView> { sort_by(source, key_order()) }

pub proof fn lemma_key_list(source: Seq<KeyView>)
    ensures distinct(key_list(source)), forall|key: KeyView| #[trigger] key_list(source).contains(key) == source.contains(key),
{
    assert(strict_order(key_order())) by {
        assert forall|key: KeyView| !#[trigger] key_order()(key, key) by { lemma_key_order(key, key); }
        assert forall|first: KeyView, second: KeyView, third: KeyView| #[trigger] key_order()(first, second) && #[trigger] key_order()(second, third) implies key_order()(first, third) by {
            lemma_key_trans(first, second, third);
        }
    }
    assert(total_on(key_order(), source)) by {
        assert forall|first: KeyView, second: KeyView| source.contains(first) && source.contains(second) && first != second implies #[trigger] key_order()(first, second) || key_order()(second, first) by {
            lemma_key_order(first, second);
        }
    }
    lemma_sort_by(source, key_order());
    let keys = key_list(source);
    assert(distinct(keys)) by {
        assert forall|first: int, second: int| 0 <= first < keys.len() && 0 <= second < keys.len() && first != second implies keys[first] != keys[second] by {
            lemma_key_order(keys[first], keys[first]);
            if first < second { assert(key_order()(keys[first], keys[second])); }
            else { assert(key_order()(keys[second], keys[first])); }
        }
    }
}

pub open spec fn weighted_entry<Value>(weight: spec_fn(KeyView) -> int, value: spec_fn(Value) -> int) -> spec_fn((KeyView, Value)) -> int {
    |row: (KeyView, Value)| weight(row.0) * value(row.1)
}

pub open spec fn weighted_index<Value>(index: Index<Value>, weight: spec_fn(KeyView) -> int, value: spec_fn(Value) -> int) -> int {
    sum(index.entries(), weighted_entry(weight, value))
}

pub open spec fn lookup_weight<Value>(index: Index<Value>, weight: spec_fn(KeyView) -> int, value: spec_fn(Value) -> int) -> spec_fn(KeyView) -> int {
    |key: KeyView| match index.at(key) { Some(entry) => weight(key) * value(entry), None => 0 }
}

#[verifier::rlimit(20)]
pub proof fn lemma_weighted_index<Value>(index: &Index<Value>, keys: Seq<KeyView>, weight: spec_fn(KeyView) -> int, value: spec_fn(Value) -> int)
    requires index.wf(), distinct(keys),
        forall|position: int| 0 <= position < index.entries().len() ==> keys.contains((#[trigger] index.entries()[position]).0),
    ensures weighted_index(*index, weight, value) == sum(keys, lookup_weight(*index, weight, value)),
{
    hide(Tree::at);
    hide(Tree::entries);
    hide(Tree::wf);
    index.model();
    let pair = |row: (KeyView, Value), key: KeyView| if row.0 == key { weight(row.0) * value(row.1) } else { 0 };
    let rows = |row: (KeyView, Value)| sum(keys, |key: KeyView| pair(row, key));
    let by_key = |key: KeyView| sum(index.entries(), |row: (KeyView, Value)| pair(row, key));
    lemma_sum_swap(index.entries(), keys, pair, rows, by_key);
    let actual = |row: (KeyView, Value)| weight(row.0) * value(row.1);
    assert forall|position: int| #![trigger index.entries()[position]] 0 <= position < index.entries().len() implies rows(index.entries()[position]) == actual(index.entries()[position]) by {
        let row = index.entries()[position];
        lemma_sum_indicator_distinct(keys, row.0, actual(row), |key: KeyView| pair(row, key));
    }
    lemma_sum_ext(index.entries(), rows, actual);
    lemma_sum_ext_all(index.entries(), actual, weighted_entry(weight, value));
    assert forall|key: KeyView| #[trigger] by_key(key) == lookup_weight(*index, weight, value)(key) by {
        let indicator = |row: (KeyView, Value)| pair(row, key);
        if index.at(key).is_some() {
            let position = choose|position: int| #![auto] 0 <= position < index.entries().len() && index.entries()[position].0 == key;
            assert forall|other: int| #![trigger index.entries()[other]] 0 <= other < index.entries().len() && other != position implies indicator(index.entries()[other]) == 0 by {
                lemma_key_order(key, key);
                if other < position { assert(key_lt(index.entries()[other].0, index.entries()[position].0)); }
                else { assert(key_lt(index.entries()[position].0, index.entries()[other].0)); }
            }
            lemma_sum_single_match(index.entries(), indicator, position);
        } else { lemma_sum_zero(index.entries(), indicator); }
    }
    lemma_sum_ext_all(keys, by_key, lookup_weight(*index, weight, value));
}

pub open spec fn supplier_keys(index: Slices) -> Seq<KeyView> { index.entries().map_values(|row: (KeyView, Counts)| row.0) }
pub open spec fn count_keys(index: Counts) -> Seq<KeyView> { index.entries().map_values(|row: (KeyView, Wide)| row.0) }
pub open spec fn bucket_factor(waiting_index: Counts, suppliers: Slices, nation: int, name: Seq<u8>) -> int {
    weighted_index(suppliers, |key: KeyView| count(waiting_index, key), |slice: Counts| count(slice, (nation, name)))
}

#[verifier::rlimit(20)]
pub proof fn lemma_bucket_factor(db: Db, waiting_index: &Counts, suppliers: &Slices, nation: int, name: Seq<u8>)
    requires waiting_for(*waiting_index, db), suppliers_for(*suppliers, db),
    ensures bucket(db, nation, name) == bucket_factor(*waiting_index, *suppliers, nation, name),
{
    hide(Tree::at);
    hide(Tree::entries);
    hide(Tree::wf);
    let data_keys = db.supplier.map_values(|row: SupplierRow| (row.s_suppkey, Seq::<u8>::empty()));
    let source = data_keys + supplier_keys(*suppliers);
    let keys = key_list(source);
    lemma_key_list(source);
    assert forall|position: int| 0 <= position < suppliers.entries().len() implies keys.contains((#[trigger] suppliers.entries()[position]).0) by {
        assert(supplier_keys(*suppliers)[position] == suppliers.entries()[position].0);
        assert(supplier_keys(*suppliers).contains(suppliers.entries()[position].0));
        assert(source[data_keys.len() + position] == suppliers.entries()[position].0);
        assert(source.contains(suppliers.entries()[position].0));
    }
    let weight = |key: KeyView| count(*waiting_index, key);
    let value = |slice: Counts| count(slice, (nation, name));
    lemma_weighted_index(suppliers, keys, weight, value);
    let key_of = |row: SupplierRow| (row.s_suppkey, Seq::<u8>::empty());
    let term = |row: SupplierRow| if row.s_nationkey == nation && row.s_name == name { waiting(db, row.s_suppkey) } else { 0 };
    let per_key = |key: KeyView| sum(db.supplier, supplier_group_weight(db, nation, name, key));
    assert forall|key: KeyView| #[trigger] per_key(key) == sum(db.supplier, |row: SupplierRow| if key_of(row) == key { term(row) } else { 0 }) by {
        lemma_sum_ext(db.supplier, supplier_group_weight(db, nation, name, key), |row: SupplierRow| if key_of(row) == key { term(row) } else { 0 });
    }
    assert forall|position: int| 0 <= position < db.supplier.len() && term(db.supplier[position]) != 0 implies keys.contains(key_of(#[trigger] db.supplier[position])) by {
        assert(data_keys[position] == key_of(db.supplier[position]));
        assert(data_keys.contains(key_of(db.supplier[position])));
        assert(source[position] == key_of(db.supplier[position]));
        assert(source.contains(key_of(db.supplier[position])));
    }
    lemma_sum_group(db.supplier, keys, key_of, term, per_key);
    assert forall|key: KeyView| #[trigger] per_key(key) == lookup_weight(*suppliers, weight, value)(key) by {
        if key.1 == Seq::<u8>::empty() {
            let indicator = |row: SupplierRow| if row.s_suppkey == key.0 && row.s_nationkey == nation && row.s_name == name { 1int } else { 0int };
            lemma_sum_scale(db.supplier, indicator, supplier_group_weight(db, nation, name, key), waiting(db, key.0));
            assert(count(*waiting_index, key) == waiting(db, key.0));
            assert(nested_count(*suppliers, key, (nation, name)) == supplier_multiplicity(db, key.0, nation, name));
        } else { lemma_sum_zero(db.supplier, supplier_group_weight(db, nation, name, key)); }
    }
    lemma_sum_ext_all(keys, per_key, lookup_weight(*suppliers, weight, value));
}

pub open spec fn supplier_group_weight(db: Db, nation: int, name: Seq<u8>, key: KeyView) -> spec_fn(SupplierRow) -> int {
    |row: SupplierRow| if key == (row.s_suppkey, Seq::<u8>::empty()) && row.s_nationkey == nation && row.s_name == name { waiting(db, row.s_suppkey) } else { 0 }
}

pub open spec fn waiting_bucket_weight(suppliers: Slices, outer: KeyView, inner: KeyView) -> spec_fn((Key, Arc<Wide>)) -> int {
    |row: (Key, Arc<Wide>)| if outer.1 == Seq::<u8>::empty() && inner.0 == 0 {
        (*row.1)@ * nested_count(suppliers, row.0.view(), (outer.0, inner.1))
    } else { 0 }
}

pub open spec fn supplier_bucket_weight(waiting_index: Counts, outer: KeyView, inner: KeyView) -> spec_fn((Key, Arc<Counts>)) -> int {
    |row: (Key, Arc<Counts>)| if outer.1 == Seq::<u8>::empty() && inner.0 == 0 {
        count(waiting_index, row.0.view()) * count(*row.1, (outer.0, inner.1))
    } else { 0 }
}

pub open spec fn waiting_change_for(changes: Counts, before: Db, after: Db) -> bool {
    forall|key: KeyView| #![trigger count(changes, key)] count(changes, key) ==
        if key.1 == Seq::<u8>::empty() { waiting(after, key.0) - waiting(before, key.0) } else { 0 }
}

pub open spec fn waiting_bucket_factor(changes: Counts, suppliers: Slices, nation: int, name: Seq<u8>) -> int {
    weighted_index(changes, |key: KeyView| nested_count(suppliers, key, (nation, name)), |value: Wide| value@)
}

pub proof fn lemma_bucket_delta_rows(waiting_delta: &Counts, supplier_delta: &Slices, old_suppliers: Slices, new_waiting: Counts,
    waiting_rows: Seq<(Key, Arc<Wide>)>, supplier_rows: Seq<(Key, Arc<Counts>)>, outer: KeyView, inner: KeyView)
    requires
        waiting_rows.map_values(|row: (Key, Arc<Wide>)| (row.0.view(), *row.1)) == waiting_delta.entries(),
        supplier_rows.map_values(|row: (Key, Arc<Counts>)| (row.0.view(), *row.1)) == supplier_delta.entries(),
    ensures
        sum(waiting_rows, waiting_bucket_weight(old_suppliers, outer, inner)) + sum(supplier_rows, supplier_bucket_weight(new_waiting, outer, inner)) ==
        if outer.1 == Seq::<u8>::empty() && inner.0 == 0 {
            waiting_bucket_factor(*waiting_delta, old_suppliers, outer.0, inner.1) + bucket_factor(new_waiting, *supplier_delta, outer.0, inner.1)
        } else { 0 },
{
    let w = |row: (KeyView, Wide)| if outer.1 == Seq::<u8>::empty() && inner.0 == 0 {
        row.1@ * nested_count(old_suppliers, row.0, (outer.0, inner.1))
    } else { 0 };
    let s = |row: (KeyView, Counts)| if outer.1 == Seq::<u8>::empty() && inner.0 == 0 {
        count(new_waiting, row.0) * count(row.1, (outer.0, inner.1))
    } else { 0 };
    lemma_sum_map(waiting_rows, |row: (Key, Arc<Wide>)| (row.0.view(), *row.1), w, waiting_bucket_weight(old_suppliers, outer, inner));
    lemma_sum_map(supplier_rows, |row: (Key, Arc<Counts>)| (row.0.view(), *row.1), s, supplier_bucket_weight(new_waiting, outer, inner));
    if outer.1 == Seq::<u8>::empty() && inner.0 == 0 {
        let factor = |row: (KeyView, Wide)| nested_count(old_suppliers, row.0, (outer.0, inner.1)) * row.1@;
        assert forall|row: (KeyView, Wide)| #[trigger] w(row) == factor(row) by {
            let amount = row.1@;
            let variants = nested_count(old_suppliers, row.0, (outer.0, inner.1));
            assert(amount * variants == variants * amount) by (nonlinear_arith);
        }
        lemma_sum_ext_all(waiting_delta.entries(), w, factor);
        lemma_sum_ext_all(waiting_delta.entries(), factor, weighted_entry(
            |key: KeyView| nested_count(old_suppliers, key, (outer.0, inner.1)), |value: Wide| value@));
        assert(sum(waiting_rows, waiting_bucket_weight(old_suppliers, outer, inner)) == waiting_bucket_factor(*waiting_delta, old_suppliers, outer.0, inner.1));
        let supplier_factor = |row: (KeyView, Counts)| count(new_waiting, row.0) * count(row.1, (outer.0, inner.1));
        lemma_sum_ext_all(supplier_delta.entries(), s, supplier_factor);
        lemma_sum_ext_all(supplier_delta.entries(), supplier_factor, weighted_entry(
            |key: KeyView| count(new_waiting, key), |slice: Counts| count(slice, (outer.0, inner.1))));
        assert(sum(supplier_rows, supplier_bucket_weight(new_waiting, outer, inner)) == bucket_factor(new_waiting, *supplier_delta, outer.0, inner.1));
    } else {
        lemma_sum_zero(waiting_delta.entries(), w);
        lemma_sum_zero(supplier_delta.entries(), s);
    }
}

#[verifier::rlimit(30)]
pub proof fn lemma_bucket_update(old_waiting: &Counts, old_suppliers: &Slices, new_waiting: &Counts, new_suppliers: &Slices,
    waiting_delta: &Counts, supplier_delta: &Slices, nation: int, name: Seq<u8>)
    requires
        old_suppliers.wf(), new_suppliers.wf(), waiting_delta.wf(), supplier_delta.wf(),
        forall|key: KeyView| #![trigger count(*new_waiting, key)] count(*new_waiting, key) == count(*old_waiting, key) + count(*waiting_delta, key),
        forall|key: KeyView, inner: KeyView| #![trigger nested_count(*new_suppliers, key, inner)] nested_count(*new_suppliers, key, inner)
            == nested_count(*old_suppliers, key, inner) + nested_count(*supplier_delta, key, inner),
    ensures
        bucket_factor(*old_waiting, *old_suppliers, nation, name)
            + waiting_bucket_factor(*waiting_delta, *old_suppliers, nation, name)
            + bucket_factor(*new_waiting, *supplier_delta, nation, name)
        == bucket_factor(*new_waiting, *new_suppliers, nation, name),
{
    hide(Tree::at);
    hide(Tree::entries);
    hide(Tree::wf);
    let old_keys = supplier_keys(*old_suppliers);
    let new_keys = supplier_keys(*new_suppliers);
    let w_keys = count_keys(*waiting_delta);
    let s_keys = supplier_keys(*supplier_delta);
    let source = old_keys + new_keys + w_keys + s_keys;
    let keys = key_list(source);
    lemma_key_list(source);
    assert forall|position: int| 0 <= position < old_suppliers.entries().len() implies keys.contains((#[trigger] old_suppliers.entries()[position]).0) by {
        assert(source[position] == old_suppliers.entries()[position].0);
        assert(source.contains(old_suppliers.entries()[position].0));
    }
    assert forall|position: int| 0 <= position < new_suppliers.entries().len() implies keys.contains((#[trigger] new_suppliers.entries()[position]).0) by {
        assert(source[old_keys.len() + position] == new_suppliers.entries()[position].0);
        assert(source.contains(new_suppliers.entries()[position].0));
    }
    assert forall|position: int| 0 <= position < waiting_delta.entries().len() implies keys.contains((#[trigger] waiting_delta.entries()[position]).0) by {
        assert(source[old_keys.len() + new_keys.len() + position] == waiting_delta.entries()[position].0);
        assert(source.contains(waiting_delta.entries()[position].0));
    }
    assert forall|position: int| 0 <= position < supplier_delta.entries().len() implies keys.contains((#[trigger] supplier_delta.entries()[position]).0) by {
        assert(source[old_keys.len() + new_keys.len() + w_keys.len() + position] == supplier_delta.entries()[position].0);
        assert(source.contains(supplier_delta.entries()[position].0));
    }
    let old_weight = |key: KeyView| count(*old_waiting, key);
    let new_weight = |key: KeyView| count(*new_waiting, key);
    let value = |slice: Counts| count(slice, (nation, name));
    let variant_weight = |key: KeyView| nested_count(*old_suppliers, key, (nation, name));
    let count_value = |value: Wide| value@;
    lemma_weighted_index(old_suppliers, keys, old_weight, value);
    lemma_weighted_index(new_suppliers, keys, new_weight, value);
    lemma_weighted_index(supplier_delta, keys, new_weight, value);
    lemma_weighted_index(waiting_delta, keys, variant_weight, count_value);
    let old = lookup_weight(*old_suppliers, old_weight, value);
    let dw = lookup_weight(*waiting_delta, variant_weight, count_value);
    let ds = lookup_weight(*supplier_delta, new_weight, value);
    let new = lookup_weight(*new_suppliers, new_weight, value);
    let partial = |key: KeyView| old(key) + dw(key);
    let combined = |key: KeyView| partial(key) + ds(key);
    assert forall|key: KeyView| #[trigger] combined(key) == new(key) by {
        assert(old(key) == count(*old_waiting, key) * nested_count(*old_suppliers, key, (nation, name)));
        let change = count(*waiting_delta, key);
        let variants = nested_count(*old_suppliers, key, (nation, name));
        assert(dw(key) == variants * change);
        assert(variants * change == change * variants) by (nonlinear_arith);
        assert(ds(key) == count(*new_waiting, key) * nested_count(*supplier_delta, key, (nation, name)));
        assert(new(key) == count(*new_waiting, key) * nested_count(*new_suppliers, key, (nation, name)));
        assert(count(*new_waiting, key) == count(*old_waiting, key) + count(*waiting_delta, key));
        assert(nested_count(*new_suppliers, key, (nation, name)) == nested_count(*old_suppliers, key, (nation, name)) + nested_count(*supplier_delta, key, (nation, name)));
        let old_amount = count(*old_waiting, key);
        let new_amount = count(*new_waiting, key);
        let new_variants = nested_count(*new_suppliers, key, (nation, name));
        let variant_change = nested_count(*supplier_delta, key, (nation, name));
        assert(old_amount * variants + change * variants + new_amount * variant_change == new_amount * new_variants) by (nonlinear_arith)
            requires new_amount == old_amount + change, new_variants == variants + variant_change;
    }
    lemma_sum_add(keys, old, dw, partial);
    lemma_sum_add(keys, partial, ds, combined);
    lemma_sum_ext_all(keys, combined, new);
}

}
