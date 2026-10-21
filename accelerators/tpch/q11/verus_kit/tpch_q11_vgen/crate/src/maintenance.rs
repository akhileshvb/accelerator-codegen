use crate::packed::{self, Packed, PackedCursor};
use std::sync::Arc;
use vstd::prelude::*;
use verus_prooflib::wide::Wide;
use verus_common::sums::{sum, lemma_sum_take};
use verus_prooflib::lemma_i64_product;
use verus_prooflib::ordered::{Index, Key, KeyView};
use crate::gen_obligations::*;
use crate::gen_spec::*;
use crate::inventory::*;
use crate::model::*;
use crate::values::*;
use crate::construction::{inventory_measure, moment_measure, profile_measure, profile_row_measure};

verus! {

pub fn merge_inventory(target: &mut Inventory, source: &Inventory, weight: &Wide)
    requires old(target).valid(), source.valid(),
    ensures final(target).valid(),
        final(target).total.count@ == old(target).total.count@ + source.total.count@ * weight@,
        final(target).total.value@ == old(target).total.value@ + source.total.value@ * weight@,
        forall|part: i64| #[trigger] moment_at(final(target).groups, part as int).count as int
            == moment_at(old(target).groups, part as int).count as int + moment_at(source.groups, part as int).count as int * weight@,
        forall|part: i64| #[trigger] moment_at(final(target).groups, part as int).value as int
            == moment_at(old(target).groups, part as int).value as int + moment_at(source.groups, part as int).value as int * weight@,
{
    let ghost initial = target.groups;
    let ghost initial_total = target.total;
    let mut processed = Ghost(0int);
    let mut cursor = source.groups.cursor();
    loop
        invariant
            target.valid(), source.valid(),
            initial == old(target).groups, initial_total == old(target).total,
            0 <= processed@ <= source.groups.entries().len(),
            cursor.remaining() == source.groups.entries().skip(processed@),
            target.total.count@ == initial_total.count@
                + sum(source.groups.entries().take(processed@), |row: (KeyView, Moment)| row.1.count as int * weight@),
            target.total.value@ == initial_total.value@
                + sum(source.groups.entries().take(processed@), |row: (KeyView, Moment)| row.1.value as int * weight@),
            forall|part: i64| #[trigger] moment_at(target.groups, part as int).count as int
                == moment_at(initial, part as int).count as int
                    + sum(source.groups.entries().take(processed@), |row: (KeyView, Moment)|
                        if row.0 == numeric_key(part as int) { row.1.count as int * weight@ } else { 0 }),
            forall|part: i64| #[trigger] moment_at(target.groups, part as int).value as int
                == moment_at(initial, part as int).value as int
                    + sum(source.groups.entries().take(processed@), |row: (KeyView, Moment)|
                        if row.0 == numeric_key(part as int) { row.1.value as int * weight@ } else { 0 }),
        decreases cursor.remaining().len(),
    {
        let (key, moment) = match cursor.next() {
            Some(row) => row,
            None => {
                proof {
                    assert(processed@ == source.groups.entries().len());
                    assert(source.groups.entries().take(processed@) =~= source.groups.entries());
                    assert forall|row: (KeyView, Moment)| #[trigger] (row.1.count as int * weight@) == weight@ * row.1.count as int by {
                        assert(row.1.count as int * weight@ == weight@ * row.1.count as int) by (nonlinear_arith);
                    }
                    assert forall|row: (KeyView, Moment)| #[trigger] (row.1.value as int * weight@) == weight@ * row.1.value as int by {
                        assert(row.1.value as int * weight@ == weight@ * row.1.value as int) by (nonlinear_arith);
                    }
                    verus_common::sums::lemma_sum_scale(source.groups.entries(),
                        |row: (KeyView, Moment)| row.1.count as int,
                        |row: (KeyView, Moment)| row.1.count as int * weight@, weight@);
                    verus_common::sums::lemma_sum_scale(source.groups.entries(),
                        |row: (KeyView, Moment)| row.1.value as int,
                        |row: (KeyView, Moment)| row.1.value as int * weight@, weight@);
                    assert(target.total.count@ == initial_total.count@ + source.total.count@ * weight@) by (nonlinear_arith)
                        requires target.total.count@ == initial_total.count@ + weight@ * source.total.count@;
                    assert(target.total.value@ == initial_total.value@ + source.total.value@ * weight@) by (nonlinear_arith)
                        requires target.total.value@ == initial_total.value@ + weight@ * source.total.value@;
                    assert forall|part: i64| #[trigger] moment_at(target.groups, part as int).count as int
                        == moment_at(initial, part as int).count as int + moment_at(source.groups, part as int).count as int * weight@ by {
                        let weighted = |row: (KeyView, Moment)| row.1.count as int * weight@;
                        packed::lemma_key_contribution(&source.groups, numeric_key(part as int),
                            weighted);
                        assert((|row: (KeyView, Moment)| if row.0 == numeric_key(part as int) { row.1.count as int * weight@ } else { 0 })
                            =~= (|row: (KeyView, Moment)| if row.0 == numeric_key(part as int) { weighted(row) } else { 0 }));
                        assert(moment_at(target.groups, part as int).count as int == moment_at(initial, part as int).count as int
                            + sum(source.groups.entries().take(processed@), |row: (KeyView, Moment)|
                                if row.0 == numeric_key(part as int) { row.1.count as int * weight@ } else { 0 }));
                        assert(packed::contribution(source.groups, numeric_key(part as int),
                            weighted)
                            == moment_at(source.groups, part as int).count as int * weight@) by {
                            reveal(verus_prooflib::ordered::contribution);
                            if source.groups.at(numeric_key(part as int)).is_some() {
                                assert(moment_at(source.groups, part as int) == source.groups.at(numeric_key(part as int)).unwrap());
                            } else { assert(moment_at(source.groups, part as int).count == 0); }
                        }
                    }
                    assert forall|part: i64| #[trigger] moment_at(target.groups, part as int).value as int
                        == moment_at(initial, part as int).value as int + moment_at(source.groups, part as int).value as int * weight@ by {
                        let weighted = |row: (KeyView, Moment)| row.1.value as int * weight@;
                        packed::lemma_key_contribution(&source.groups, numeric_key(part as int),
                            weighted);
                        assert((|row: (KeyView, Moment)| if row.0 == numeric_key(part as int) { row.1.value as int * weight@ } else { 0 })
                            =~= (|row: (KeyView, Moment)| if row.0 == numeric_key(part as int) { weighted(row) } else { 0 }));
                        assert(moment_at(target.groups, part as int).value as int == moment_at(initial, part as int).value as int
                            + sum(source.groups.entries().take(processed@), |row: (KeyView, Moment)|
                                if row.0 == numeric_key(part as int) { row.1.value as int * weight@ } else { 0 }));
                        assert(packed::contribution(source.groups, numeric_key(part as int),
                            weighted)
                            == moment_at(source.groups, part as int).value as int * weight@) by {
                            reveal(verus_prooflib::ordered::contribution);
                            if source.groups.at(numeric_key(part as int)).is_some() {
                                assert(moment_at(source.groups, part as int) == source.groups.at(numeric_key(part as int)).unwrap());
                            } else { assert(moment_at(source.groups, part as int).value == 0); }
                        }
                    }
                }
                return;
            },
        };
        proof {
            source.groups.model();
            assert(source.groups.at(key.view()) == Some((*moment)@));
            assert(key.view().1 == Seq::<u8>::empty());
        }
        let change = scale(&*moment, weight);
        target.update(key.number, &change);
        proof {
            lemma_sum_take(source.groups.entries(), processed@, |row: (KeyView, Moment)| row.1.count as int * weight@);
            lemma_sum_take(source.groups.entries(), processed@, |row: (KeyView, Moment)| row.1.value as int * weight@);
            assert forall|part: i64| #[trigger] moment_at(target.groups, part as int).count as int
                == moment_at(initial, part as int).count as int
                    + sum(source.groups.entries().take(processed@ + 1), |row: (KeyView, Moment)|
                        if row.0 == numeric_key(part as int) { row.1.count as int * weight@ } else { 0 }) by {
                lemma_sum_take(source.groups.entries(), processed@, |row: (KeyView, Moment)|
                    if row.0 == numeric_key(part as int) { row.1.count as int * weight@ } else { 0 });
            }
            assert forall|part: i64| #[trigger] moment_at(target.groups, part as int).value as int
                == moment_at(initial, part as int).value as int
                    + sum(source.groups.entries().take(processed@ + 1), |row: (KeyView, Moment)|
                        if row.0 == numeric_key(part as int) { row.1.value as int * weight@ } else { 0 }) by {
                lemma_sum_take(source.groups.entries(), processed@, |row: (KeyView, Moment)|
                    if row.0 == numeric_key(part as int) { row.1.value as int * weight@ } else { 0 });
            }
        }
        processed = Ghost(processed@ + 1);
    }
}

#[verifier::rlimit(20)]
pub fn change_inventory(data: &mut Data, nation: i64, part: i64, change: &Mom)
    requires old(data).structures_valid(),
    ensures final(data).structures_valid(),
        crate::names::indexed_names_ok(*old(data)) && crate::name_updates::symmetric(*old(data))
            && counts_layout(old(data).nations_reverse, false, true) ==> crate::names::indexed_names_ok(*final(data)),
        same_dimensions(*old(data), *final(data)),
        forall|key: KeyView| (#[trigger] old(data).names.at(key)).is_none() ==> final(data).names.at(key).is_none(),
        inventory_at(final(data).inventories, nation as int, part as int).count as int
            == inventory_at(old(data).inventories, nation as int, part as int).count as int + change.count@,
        inventory_at(final(data).inventories, nation as int, part as int).value as int
            == inventory_at(old(data).inventories, nation as int, part as int).value as int + change.value@,
        inventory_total(final(data).inventories, nation as int).count as int
            == inventory_total(old(data).inventories, nation as int).count as int + change.count@,
        inventory_total(final(data).inventories, nation as int).value as int
            == inventory_total(old(data).inventories, nation as int).value as int + change.value@,
        forall|other_nation: int, other_part: int| other_nation != nation || other_part != part ==>
            #[trigger] inventory_at(final(data).inventories, other_nation, other_part) == inventory_at(old(data).inventories, other_nation, other_part),
        forall|other_nation: int| other_nation != nation ==>
            #[trigger] inventory_total(final(data).inventories, other_nation) == inventory_total(old(data).inventories, other_nation),
{
    let ghost original = *data;
    let key = Key::number(nation);
    let mut inventory = match data.inventories.get(&key) {
        Some(inventory) => inventory.share(), None => Inventory::new(),
    };
    proof {
        assert forall|other: int| #[trigger] moment_at(inventory.groups, other) == inventory_at(original.inventories, nation as int, other) by {}
        assert(inventory.total@ == inventory_total(original.inventories, nation as int));
    }
    inventory.update(part, change);
    if inventory.groups.is_empty() {
        proof { lemma_empty_inventory(&inventory); }
        data.inventories.remove(&key);
    }
    else { data.inventories.put(key.share(), Arc::new(inventory)); }
    proof {
        assert forall|other: int| #[trigger] inventory_at(data.inventories, nation as int, other) == moment_at(inventory.groups, other) by {}
        assert(inventory_total(data.inventories, nation as int) == inventory.total@);
    }
    let names = data.nations.get(&key);
    if let Some(names) = &names {
        let shared_names = names.share();
        crate::name_updates::update_nation_names(data, &**names, part, change);
        proof {
            assert forall|name: KeyView, selected: Option<i64>, value: bool| #[trigger] crate::name_updates::name_measure(data.names, name, selected, value)
                == crate::name_updates::name_measure(original.names, name, selected, value)
                    + if crate::name_updates::merged_at(original.names, name) { count_at(original.nations, numeric_key(nation as int), name) * crate::name_updates::cell_change(change@, part, selected, value) } else { 0 } by {
                assert(original.nations.at(numeric_key(nation as int)) == Some(**names));
            }
        }
    } else {
        proof {
            assert(data.names == original.names);
            assert(original.nations.at(numeric_key(nation as int)).is_none());
        }
    }
    proof {
        if crate::names::indexed_names_ok(original) && crate::name_updates::symmetric(original) && counts_layout(original.nations_reverse, false, true) {
            crate::name_updates::lemma_inventory_sources(original, *data, nation, part, change@);
            crate::name_updates::lemma_binding_cell(original, *data, nation, part, change@);
        }
    }
}

pub fn refresh_name(data: &mut Data, name: &Key, changes: Option<&Index<Wide>>, Ghost(basis): Ghost<Data>)
    requires old(data).structures_valid(), nonzero_counts(old(data).nations_reverse), counts_layout(old(data).nations_reverse, false, true),
        changes matches Some(links) ==> links.wf() && crate::names::binding_ok(basis, name.view())
            && basis.names.at(name.view()) == old(data).names.at(name.view()) && basis.inventories == old(data).inventories
            && counts_layout(basis.nations_reverse, false, true)
            && forall|key: KeyView| #[trigger] count_at(old(data).nations_reverse, name.view(), key)
                == count_at(basis.nations_reverse, name.view(), key) + crate::counts::inner_at(*links, key),
    ensures final(data).structures_valid(),
        same_dimensions(*old(data), *final(data)), final(data).inventories == old(data).inventories,
        crate::names::binding_ok(*final(data), name.view()),
        forall|other: KeyView| other != name.view() ==> (#[trigger] final(data).names.at(other)) == old(data).names.at(other),
{
    let support = match data.nations_reverse.get(name) {
        Some(support) => support,
        None => { data.names.remove(name); return; },
    };
    let mut cursor = support.cursor();
    let (first, multiplicity) = match cursor.next() {
        Some(row) => row,
        None => { data.names.remove(name); return; },
    };
    if cursor.next().is_none() {
        proof {
            support.model();
            assert(support.entries().len() == 1);
            assert(support.at(first.view()).is_some() && support.at(first.view()).unwrap()@ == (*multiplicity)@);
            assert(numeric(first.view()));
        }
        data.names.put(name.share(), Arc::new(NameView::Singleton { nation: first.number, multiplicity: multiplicity.dup() }));
        return;
    }
    let previous = data.names.get(name);
    if let Some(view) = previous {
        if let NameView::Merged { inventory } = &*view {
            if let Some(changes) = changes {
                let mut updated = inventory.share();
                crate::name_updates::merge_changes(&mut updated, data, changes);
                proof {
                    assert(support.entries().len() >= 2);
                    assert forall|part: Option<i64>, value: bool| #[trigger] crate::names::measure(updated, part, value)
                        == crate::names::support_measure(data.inventories, *support, part, value) by {
                        assert(crate::names::measure(*inventory, part, value) == crate::name_updates::outer_measure(basis.inventories, basis.nations_reverse, name.view(), part, value));
                        assert forall|key: KeyView| #[trigger] count_at(data.nations_reverse, name.view(), key)
                            == count_at(basis.nations_reverse, name.view(), key) + verus_prooflib::ordered_weight_at(*changes, key, |row: (KeyView, Wide)| row.1@) by {}
                        crate::name_updates::lemma_support_count_step(data.inventories, basis.nations_reverse, data.nations_reverse, name.view(), *changes, part, value);
                        assert(crate::names::measure(updated, part, value) == crate::names::measure(*inventory, part, value) + crate::names::support_measure(data.inventories, *changes, part, value));
                    }
                }
                data.names.put(name.share(), Arc::new(NameView::Merged { inventory: updated }));
                return;
            }
        }
    }
    let shared_support = support.share();
    let merged = crate::names::merge_support(data, &shared_support);
    data.names.put(name.share(), Arc::new(NameView::Merged { inventory: merged }));
}

pub fn build_inventories(data: &mut Data)
    requires old(data).structures_valid(), profile_storage_ok(old(data).profiles), counts_layout(old(data).suppliers, true, true),
        nonzero_counts(old(data).nations_reverse), counts_layout(old(data).nations_reverse, false, true),
        forall|key: KeyView| (#[trigger] old(data).inventories.at(key)).is_none(),
        forall|key: KeyView| (#[trigger] old(data).names.at(key)).is_none(),
    ensures final(data).structures_valid(),
        same_dimensions(*old(data), *final(data)),
        crate::construction::indexed_inventory_ok(*final(data)),
        crate::names::indexed_names_ok(*final(data)),
{
    crate::construction::build_inventory_rows(data);
    let ghost built = *data;
    let mut names = data.nations_reverse.cursor();
    let mut processed = Ghost(0int);
    loop
        invariant data.structures_valid(), same_dimensions(*old(data), *data),
            data.inventories == built.inventories, same_dimensions(built, *data),
            crate::construction::indexed_inventory_ok(built),
            nonzero_counts(data.nations_reverse), counts_layout(data.nations_reverse, false, true),
            0 <= processed@ <= data.nations_reverse.entries().len(),
            names.remaining() == data.nations_reverse.entries().skip(processed@),
            forall|position: int| 0 <= position < processed@ ==> #[trigger] crate::names::binding_ok(*data, data.nations_reverse.entries()[position].0),
            forall|key: KeyView| (#[trigger] data.nations_reverse.at(key)).is_none() ==> data.names.at(key).is_none(),
        decreases names.remaining().len(),
    {
        let (name, _) = match names.next() {
            Some(row) => row,
            None => {
                proof {
                    assert(processed@ == data.nations_reverse.entries().len());
                    data.nations_reverse.model();
                    assert forall|key: KeyView| #[trigger] crate::names::binding_ok(*data, key) by {
                        if data.nations_reverse.at(key).is_some() {
                            let position = choose|position: int| 0 <= position < data.nations_reverse.entries().len()
                                && (#[trigger] data.nations_reverse.entries()[position]).0 == key;
                            assert(crate::names::binding_ok(*data, data.nations_reverse.entries()[position].0));
                        }
                    }
                }
                return;
            },
        };
        let ghost previous = *data;
        refresh_name(data, &name, None, Ghost(built));
        proof {
            assert forall|position: int| 0 <= position < processed@ + 1 implies #[trigger] crate::names::binding_ok(*data, data.nations_reverse.entries()[position].0) by {
                if data.nations_reverse.entries()[position].0 != name.view() {
                    assert(position < processed@);
                    assert(crate::names::binding_ok(previous, previous.nations_reverse.entries()[position].0));
                }
            }
        }
        processed = Ghost(processed@ + 1);
    }
}

pub open spec fn p_delta_count(rows: Seq<PartsuppDelta>, supplier: int, part: int) -> int {
    sum(rows, |change: PartsuppDelta| if change.row.ps_suppkey == supplier && change.row.ps_partkey == part {
        change.sign as int
    } else { 0 })
}

pub open spec fn p_delta_value(rows: Seq<PartsuppDelta>, supplier: int, part: int) -> int {
    sum(rows, |change: PartsuppDelta| if change.row.ps_suppkey == supplier && change.row.ps_partkey == part {
        change.sign as int * change.row.ps_availqty as int * change.row.ps_supplycost as int
    } else { 0 })
}

pub open spec fn partsupp_changes_ok(profiles: Profiles, rows: Seq<PartsuppDelta>) -> bool {
    forall|supplier: i64, part: i64|
        #[trigger] profile_at(profiles, supplier as int, part as int).count as int == p_delta_count(rows, supplier as int, part as int)
            && profile_at(profiles, supplier as int, part as int).value as int == p_delta_value(rows, supplier as int, part as int)
}

pub fn coalesce_partsupp(rows: &[PartsuppDelta]) -> (profiles: Profiles)
    ensures partsupp_changes_ok(profiles, rows@) && profile_layout(profiles),
{
    let mut profiles = Index::new();
    let mut index: usize = 0;
    let mut processed = Ghost(0int);
    while index < rows.len()
        invariant
            index <= rows.len(), processed@ == index as int,
            profile_layout(profiles),
            forall|supplier: i64, part: i64| #[trigger] profile_at(profiles, supplier as int, part as int).count as int
                == p_delta_count(rows@.take(processed@), supplier as int, part as int)
                && profile_at(profiles, supplier as int, part as int).value as int
                    == p_delta_value(rows@.take(processed@), supplier as int, part as int),
        decreases rows.len() - index,
    {
        let row = rows[index];
        let value = Wide::from_i128(row.row.ps_availqty as i128).mul(&Wide::from_i128(row.row.ps_supplycost as i128));
        let change = scale(&Mom { count: Wide::from_i128(1), value }, &Wide::from_i128(row.sign as i128));
        add_profile(&mut profiles, row.row.ps_suppkey, row.row.ps_partkey, &change, 63);
        proof {
            assert forall|supplier: i64, part: i64| #[trigger] profile_at(profiles, supplier as int, part as int).count as int
                == p_delta_count(rows@.take(index as int + 1), supplier as int, part as int)
                && profile_at(profiles, supplier as int, part as int).value as int
                    == p_delta_value(rows@.take(index as int + 1), supplier as int, part as int) by {
                lemma_sum_take(rows@, index as int, |other: PartsuppDelta|
                    if other.row.ps_suppkey == supplier && other.row.ps_partkey == part { other.sign as int } else { 0 });
                lemma_sum_take(rows@, index as int, |other: PartsuppDelta|
                    if other.row.ps_suppkey == supplier && other.row.ps_partkey == part {
                        other.sign as int * other.row.ps_availqty as int * other.row.ps_supplycost as int
                    } else { 0 });
                assert(change.value@ == row.sign as int * row.row.ps_availqty as int * row.row.ps_supplycost as int) by (nonlinear_arith)
                    requires change.value@ == value@ * row.sign as int,
                        value@ == row.row.ps_availqty as int * row.row.ps_supplycost as int;
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    proof {
        assert(rows@.take(processed@) =~= rows@);
        assert forall|supplier: i64, part: i64| #[trigger] profile_at(profiles, supplier as int, part as int).count as int
            == p_delta_count(rows@, supplier as int, part as int)
            && profile_at(profiles, supplier as int, part as int).value as int == p_delta_value(rows@, supplier as int, part as int) by {
            assert(profile_at(profiles, supplier as int, part as int).count as int == p_delta_count(rows@.take(processed@), supplier as int, part as int));
            assert(profile_at(profiles, supplier as int, part as int).value as int == p_delta_value(rows@.take(processed@), supplier as int, part as int));
        }
    }
    profiles
}

pub fn coalesce_supplier(rows: &[SupplierDelta]) -> (counts: Counts)
    ensures counts_layout(counts, true, true),
        forall|supplier: i64, nation: i64|
        #[trigger] count_at(counts, numeric_key(supplier as int), numeric_key(nation as int))
            == sum(rows@, |change: SupplierDelta| if change.row.s_suppkey == supplier && change.row.s_nationkey == nation {
                change.sign as int
            } else { 0 }),
{
    let mut counts = Index::new();
    let mut index: usize = 0;
    let mut processed = Ghost(0int);
    while index < rows.len()
        invariant
            index <= rows.len(), processed@ == index as int,
            counts_layout(counts, true, true),
            forall|supplier: i64, nation: i64| #[trigger] count_at(counts, numeric_key(supplier as int), numeric_key(nation as int))
                == sum(rows@.take(processed@), |change: SupplierDelta|
                    if change.row.s_suppkey == supplier && change.row.s_nationkey == nation { change.sign as int } else { 0 }),
        decreases rows.len() - index,
    {
        let row = rows[index];
        add_count(&mut counts, &Key::number(row.row.s_suppkey), &Key::number(row.row.s_nationkey), &Wide::from_i128(row.sign as i128));
        proof {
            assert forall|supplier: i64, nation: i64| #[trigger] count_at(counts, numeric_key(supplier as int), numeric_key(nation as int))
                == sum(rows@.take(index as int + 1), |other: SupplierDelta|
                    if other.row.s_suppkey == supplier && other.row.s_nationkey == nation { other.sign as int } else { 0 }) by {
                lemma_sum_take(rows@, index as int, |other: SupplierDelta|
                    if other.row.s_suppkey == supplier && other.row.s_nationkey == nation { other.sign as int } else { 0 });
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    proof { assert(rows@.take(processed@) =~= rows@); }
    counts
}

pub fn coalesce_nation(rows: &Vec<NationDelta>) -> (counts: Counts)
    ensures counts_layout(counts, false, true),
        forall|nation: i64, name: Seq<u8>|
        #[trigger] count_at(counts, text_key(name), numeric_key(nation as int))
            == nation_delta_count(rows@, nation, name),
{
    let mut counts = Index::new();
    let mut index: usize = 0;
    let mut processed = Ghost(0int);
    while index < rows.len()
        invariant
            index <= rows.len(), processed@ == index as int,
            counts_layout(counts, false, true),
            forall|nation: i64, name: Seq<u8>| #[trigger] count_at(counts, text_key(name), numeric_key(nation as int))
                == sum(rows@.take(processed@), |change: NationDelta|
                    if change.row.n_nationkey == nation && change.row.n_name@ == name { change.sign as int } else { 0 }),
        decreases rows.len() - index,
    {
        let row = &rows[index];
        let name = Key::text(copy_bytes(row.row.n_name.as_slice()));
        add_count(&mut counts, &name, &Key::number(row.row.n_nationkey), &Wide::from_i128(row.sign as i128));
        proof {
            assert forall|nation: i64, name: Seq<u8>| #[trigger] count_at(counts, text_key(name), numeric_key(nation as int))
                == sum(rows@.take(index as int + 1), |other: NationDelta|
                    if other.row.n_nationkey == nation && other.row.n_name@ == name { other.sign as int } else { 0 }) by {
                lemma_sum_take(rows@, index as int, |other: NationDelta|
                    if other.row.n_nationkey == nation && other.row.n_name@ == name { other.sign as int } else { 0 });
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    proof { assert(rows@.take(processed@) =~= rows@); }
    counts
}

pub open spec fn nation_delta_weight(nation: i64, name: Seq<u8>) -> spec_fn(NationDelta) -> int {
    |change: NationDelta| if change.row.n_nationkey == nation && change.row.n_name@ == name { change.sign as int } else { 0 }
}

pub open spec fn nation_delta_count(rows: Seq<NationDelta>, nation: i64, name: Seq<u8>) -> int {
    sum(rows, nation_delta_weight(nation, name))
}

pub proof fn lemma_inventory_group_finish(groups: Packed, nation: i64, query: i64, part: Option<i64>, value: bool, selected: spec_fn((KeyView, Moment)) -> int)
    requires forall|row: (KeyView, Moment)| #[trigger] selected(row) == if query == nation { profile_row_measure(row, part, value) } else { 0 },
    ensures sum(groups.entries(), selected) == if query == nation { profile_measure(groups, part, value) } else { 0 },
{
    if query == nation {
        verus_common::sums::lemma_sum_ext_all(groups.entries(), selected, |row: (KeyView, Moment)| profile_row_measure(row, part, value));
    } else { verus_common::sums::lemma_sum_zero(groups.entries(), selected); }
}

pub proof fn lemma_inventory_changes_finish(changes: Profiles, nation: i64, part: Option<i64>, value: bool, selected: spec_fn((KeyView, Packed)) -> int)
    requires changes.wf(), forall|row: (KeyView, Packed)| #[trigger] selected(row) == if row.0 == numeric_key(nation as int) { profile_measure(row.1, part, value) } else { 0 },
    ensures sum(changes.entries(), selected) == crate::construction::profile_measure_at(changes, numeric_key(nation as int), part, value),
{
    let weight = |row: (KeyView, Packed)| profile_measure(row.1, part, value);
    let filtered = |row: (KeyView, Packed)| if row.0 == numeric_key(nation as int) { weight(row) } else { 0 };
    verus_prooflib::ordered::lemma_key_contribution(&changes, numeric_key(nation as int), weight);
    verus_common::sums::lemma_sum_ext_all(changes.entries(), selected, filtered);
    reveal(verus_prooflib::ordered::contribution);
}

pub fn apply_inventory_group(data: &mut Data, nation: i64, groups: &Packed)
    requires old(data).structures_valid(), crate::construction::profile_keys_ok(*groups),
        crate::names::indexed_names_ok(*old(data)), crate::name_updates::symmetric(*old(data)), counts_layout(old(data).nations_reverse, false, true),
    ensures final(data).structures_valid(), same_dimensions(*old(data), *final(data)),
        crate::names::indexed_names_ok(*final(data)),
        forall|query: i64, part: Option<i64>, value: bool| #[trigger] inventory_measure(final(data).inventories, query, part, value)
            == inventory_measure(old(data).inventories, query, part, value)
                + if query == nation { profile_measure(*groups, part, value) } else { 0 },
{
    let ghost initial = *data;
    let mut processed = Ghost(0int);
    let mut parts = groups.cursor();
    loop
        invariant data.structures_valid(), same_dimensions(initial, *data), initial == *old(data),
            crate::names::indexed_names_ok(*data), crate::name_updates::symmetric(*data), counts_layout(data.nations_reverse, false, true),
            crate::construction::profile_keys_ok(*groups),
            0 <= processed@ <= groups.entries().len(), parts.remaining() == groups.entries().skip(processed@),
            forall|query: i64, part: Option<i64>, value: bool| #[trigger] inventory_measure(data.inventories, query, part, value)
                == inventory_measure(initial.inventories, query, part, value)
                    + sum(groups.entries().take(processed@), |row: (KeyView, Moment)|
                        if query == nation { profile_row_measure(row, part, value) } else { 0 }),
        decreases parts.remaining().len(),
    {
        let (part, change) = match parts.next() {
            Some(row) => row,
            None => {
                proof {
                    assert(processed@ == groups.entries().len());
                    assert(groups.entries().take(processed@) =~= groups.entries());
                    assert forall|query: i64, part: Option<i64>, value: bool| #[trigger] inventory_measure(data.inventories, query, part, value)
                        == inventory_measure(initial.inventories, query, part, value)
                            + if query == nation { profile_measure(*groups, part, value) } else { 0 } by {
                        let selected = |row: (KeyView, Moment)| if query == nation { profile_row_measure(row, part, value) } else { 0 };
                        lemma_inventory_group_finish(*groups, nation, query, part, value, selected);
                        assert(inventory_measure(data.inventories, query, part, value) == inventory_measure(initial.inventories, query, part, value) + sum(groups.entries(), selected));
                    }
                }
                return;
            },
        };
        proof { groups.model(); assert(numeric(part.view())); }
        let ghost previous = *data;
        change_inventory(data, nation, part.number, &*change);
        proof {
            assert forall|query: i64, selected: Option<i64>, value: bool| #[trigger] inventory_measure(data.inventories, query, selected, value)
                == inventory_measure(initial.inventories, query, selected, value)
                    + sum(groups.entries().take(processed@ + 1), |row: (KeyView, Moment)|
                        if query == nation { profile_row_measure(row, selected, value) } else { 0 }) by {
                let weight = |row: (KeyView, Moment)| if query == nation { profile_row_measure(row, selected, value) } else { 0 };
                lemma_sum_take(groups.entries(), processed@, weight);
                assert(groups.entries()[processed@] == (part.view(), (*change)@));
                assert(inventory_measure(previous.inventories, query, selected, value)
                    == inventory_measure(initial.inventories, query, selected, value) + sum(groups.entries().take(processed@), weight));
                if let Some(selected) = selected {
                    if query == nation && selected == part.number {
                        assert(inventory_at(data.inventories, query as int, selected as int).count as int == inventory_at(previous.inventories, query as int, selected as int).count as int + change.count@);
                        assert(inventory_at(data.inventories, query as int, selected as int).value as int == inventory_at(previous.inventories, query as int, selected as int).value as int + change.value@);
                    } else { assert(inventory_at(data.inventories, query as int, selected as int) == inventory_at(previous.inventories, query as int, selected as int)); }
                } else if query == nation {
                    assert(inventory_total(data.inventories, query as int).count as int == inventory_total(previous.inventories, query as int).count as int + change.count@);
                    assert(inventory_total(data.inventories, query as int).value as int == inventory_total(previous.inventories, query as int).value as int + change.value@);
                } else { assert(inventory_total(data.inventories, query as int) == inventory_total(previous.inventories, query as int)); }
            }
        }
        processed = Ghost(processed@ + 1);
    }
}

pub fn apply_inventory_changes(data: &mut Data, changes: &Profiles)
    requires old(data).structures_valid(), profile_layout(*changes),
        crate::names::indexed_names_ok(*old(data)), crate::name_updates::symmetric(*old(data)), counts_layout(old(data).nations_reverse, false, true),
    ensures final(data).structures_valid(), same_dimensions(*old(data), *final(data)),
        crate::names::indexed_names_ok(*final(data)),
        forall|nation: i64, part: Option<i64>, value: bool| #[trigger] inventory_measure(final(data).inventories, nation, part, value)
            == inventory_measure(old(data).inventories, nation, part, value)
                + crate::construction::profile_measure_at(*changes, numeric_key(nation as int), part, value),
{
    let ghost initial = *data;
    let mut processed = Ghost(0int);
    let mut nations = changes.cursor();
    loop
        invariant data.structures_valid(), same_dimensions(*old(data), *data), initial == *old(data),
            crate::names::indexed_names_ok(*data), crate::name_updates::symmetric(*data), counts_layout(data.nations_reverse, false, true),
            profile_layout(*changes), 0 <= processed@ <= changes.entries().len(), nations.remaining() == changes.entries().skip(processed@),
            forall|nation: i64, part: Option<i64>, value: bool| #[trigger] inventory_measure(data.inventories, nation, part, value)
                == inventory_measure(initial.inventories, nation, part, value)
                    + sum(changes.entries().take(processed@), |row: (KeyView, Packed)|
                        if row.0 == numeric_key(nation as int) { profile_measure(row.1, part, value) } else { 0 }),
        decreases nations.remaining().len(),
    {
        let (nation, groups) = match nations.next() {
            Some(row) => row,
            None => {
                proof {
                    assert(processed@ == changes.entries().len()); assert(changes.entries().take(processed@) =~= changes.entries());
                    assert forall|nation: i64, part: Option<i64>, value: bool| #[trigger] inventory_measure(data.inventories, nation, part, value)
                        == inventory_measure(initial.inventories, nation, part, value)
                            + crate::construction::profile_measure_at(*changes, numeric_key(nation as int), part, value) by {
                        let selected = |row: (KeyView, Packed)| if row.0 == numeric_key(nation as int) { profile_measure(row.1, part, value) } else { 0 };
                        lemma_inventory_changes_finish(*changes, nation, part, value, selected);
                        assert(inventory_measure(data.inventories, nation, part, value) == inventory_measure(initial.inventories, nation, part, value) + sum(changes.entries(), selected));
                    }
                }
                return;
            },
        };
        proof { changes.model(); assert(numeric(nation.view())); assert(changes.at(nation.view()) == Some(*groups)); }
        let ghost previous = *data;
        apply_inventory_group(data, nation.number, &*groups);
        proof {
            assert forall|query: i64, part: Option<i64>, value: bool| #[trigger] inventory_measure(data.inventories, query, part, value)
                == inventory_measure(initial.inventories, query, part, value)
                    + sum(changes.entries().take(processed@ + 1), |row: (KeyView, Packed)|
                        if row.0 == numeric_key(query as int) { profile_measure(row.1, part, value) } else { 0 }) by {
                let weight = |row: (KeyView, Packed)| if row.0 == numeric_key(query as int) { profile_measure(row.1, part, value) } else { 0 };
                lemma_sum_take(changes.entries(), processed@, weight);
                assert(changes.entries()[processed@] == (nation.view(), *groups));
                assert(inventory_measure(previous.inventories, query, part, value)
                    == inventory_measure(initial.inventories, query, part, value) + sum(changes.entries().take(processed@), weight));
            }
        }
        processed = Ghost(processed@ + 1);
    }
}

pub open spec fn profile_cell(profiles: Profiles, supplier: i64, part: i64, value: bool) -> int {
    crate::construction::moment_measure(profile_at(profiles, supplier as int, part as int), value)
}

pub open spec fn profile_frame(before: Data, after: Data) -> bool {
    before.suppliers == after.suppliers && before.suppliers_reverse == after.suppliers_reverse
        && before.nations == after.nations && before.nations_reverse == after.nations_reverse
        && before.inventories == after.inventories && before.names == after.names
}

pub fn stage_partsupp_cell(data: &mut Data, supplier: &Key, part: &Key, change: &Mom, links: Option<&Index<Wide>>, inventory_changes: &mut Profiles)
    requires old(data).structures_valid(), profile_storage_ok(old(data).profiles), numeric(supplier.view()), numeric(part.view()),
        profile_layout(*old(inventory_changes)), counts_layout(old(data).suppliers, true, true),
        match links { Some(links) => old(data).suppliers.at(supplier.view()) == Some(*links), None => old(data).suppliers.at(supplier.view()).is_none() },
    ensures final(data).structures_valid(), profile_storage_ok(final(data).profiles), profile_frame(*old(data), *final(data)),
        profile_layout(*final(inventory_changes)),
        forall|nation: i64, selected_part: i64, value: bool| #[trigger] profile_cell(*final(inventory_changes), nation, selected_part, value)
            == profile_cell(*old(inventory_changes), nation, selected_part, value)
                + if selected_part == part.number { count_at(old(data).suppliers, supplier.view(), numeric_key(nation as int)) * moment_measure(change@, value) } else { 0 },
        forall|other_supplier: i64, other_part: i64, value: bool|
            #[trigger] profile_cell(final(data).profiles, other_supplier, other_part, value)
                == profile_cell(old(data).profiles, other_supplier, other_part, value)
                    + if other_supplier == supplier.number && other_part == part.number { crate::construction::moment_measure(change@, value) } else { 0 },
{
    let ghost initial = *data;
    add_profile(&mut data.profiles, supplier.number, part.number, change, 63);
    if let Some(links) = links {
        crate::staging::linked_part(inventory_changes, part.number, change, links);
    }
}

pub fn stage_partsupp_groups(data: &mut Data, supplier: &Key, groups: &Packed, inventory_changes: &mut Profiles)
    requires old(data).structures_valid(), profile_storage_ok(old(data).profiles), numeric(supplier.view()), crate::construction::profile_keys_ok(*groups),
        profile_layout(*old(inventory_changes)), counts_layout(old(data).suppliers, true, true),
    ensures final(data).structures_valid(), profile_storage_ok(final(data).profiles), profile_frame(*old(data), *final(data)),
        profile_layout(*final(inventory_changes)),
        forall|nation: i64, part: i64, value: bool| #[trigger] profile_cell(*final(inventory_changes), nation, part, value)
            == profile_cell(*old(inventory_changes), nation, part, value)
                + moment_measure(moment_at(*groups, part as int), value) * count_at(old(data).suppliers, supplier.view(), numeric_key(nation as int)),
        forall|other_supplier: i64, part: i64, value: bool|
            #[trigger] profile_cell(final(data).profiles, other_supplier, part, value)
                == profile_cell(old(data).profiles, other_supplier, part, value)
                    + if other_supplier == supplier.number { crate::construction::moment_measure(moment_at(*groups, part as int), value) } else { 0 },
{
    let ghost initial = *data;
    let ghost initial_changes = *inventory_changes;
    let links = data.suppliers.get(supplier);
    let mut cursor = groups.cursor();
    let mut processed = Ghost(0int);
    loop
        invariant initial == *old(data), data.structures_valid(), profile_storage_ok(data.profiles), profile_frame(initial, *data),
            profile_layout(*inventory_changes),
            initial_changes == *old(inventory_changes), counts_layout(initial.suppliers, true, true),
            forall|nation: i64, part: i64, value: bool| #[trigger] profile_cell(*inventory_changes, nation, part, value)
                == profile_cell(initial_changes, nation, part, value) + sum(groups.entries().take(processed@), |row: (KeyView, Moment)|
                    if row.0 == numeric_key(part as int) { moment_measure(row.1, value) * count_at(initial.suppliers, supplier.view(), numeric_key(nation as int)) } else { 0 }),
            match links { Some(links) => data.suppliers.at(supplier.view()) == Some(*links), None => data.suppliers.at(supplier.view()).is_none() },
            numeric(supplier.view()), crate::construction::profile_keys_ok(*groups),
            0 <= processed@ <= groups.entries().len(), cursor.remaining() == groups.entries().skip(processed@),
            forall|other_supplier: i64, part: i64, value: bool| #[trigger] profile_cell(data.profiles, other_supplier, part, value)
                == profile_cell(initial.profiles, other_supplier, part, value)
                    + sum(groups.entries().take(processed@), |row: (KeyView, Moment)|
                        if other_supplier == supplier.number && row.0 == numeric_key(part as int) { crate::construction::moment_measure(row.1, value) } else { 0 }),
        decreases cursor.remaining().len(),
    {
        let (part, change) = match cursor.next() {
            Some(row) => row,
            None => {
                proof {
                    assert(processed@ == groups.entries().len()); assert(groups.entries().take(processed@) =~= groups.entries());
                    assert forall|nation: i64, part: i64, value: bool| #[trigger] profile_cell(*inventory_changes, nation, part, value)
                        == profile_cell(initial_changes, nation, part, value)
                            + moment_measure(moment_at(*groups, part as int), value) * count_at(initial.suppliers, supplier.view(), numeric_key(nation as int)) by {
                        let selected = |row: (KeyView, Moment)| if row.0 == numeric_key(part as int) { moment_measure(row.1, value) * count_at(initial.suppliers, supplier.view(), numeric_key(nation as int)) } else { 0 };
                        crate::staging::lemma_part_finish(*groups, nation, nation, part, value, count_at(initial.suppliers, supplier.view(), numeric_key(nation as int)), selected);
                        assert(profile_cell(*inventory_changes, nation, part, value) == profile_cell(initial_changes, nation, part, value) + sum(groups.entries(), selected));
                    }
                    assert forall|other_supplier: i64, part: i64, value: bool| #[trigger] profile_cell(data.profiles, other_supplier, part, value)
                        == profile_cell(initial.profiles, other_supplier, part, value)
                            + if other_supplier == supplier.number { crate::construction::moment_measure(moment_at(*groups, part as int), value) } else { 0 } by {
                        let weight = |row: (KeyView, Moment)| if other_supplier == supplier.number { crate::construction::moment_measure(row.1, value) } else { 0 };
                        packed::lemma_key_contribution(groups, numeric_key(part as int), weight);
                        assert((|row: (KeyView, Moment)| if other_supplier == supplier.number && row.0 == numeric_key(part as int) { crate::construction::moment_measure(row.1, value) } else { 0 })
                            =~= (|row: (KeyView, Moment)| if row.0 == numeric_key(part as int) { weight(row) } else { 0 }));
                        reveal(verus_prooflib::ordered::contribution);
                        if groups.at(numeric_key(part as int)).is_none() { assert(crate::construction::moment_measure(moment_at(*groups, part as int), value) == 0); }
                    }
                }
                return;
            },
        };
        proof { groups.model(); assert(groups.at(part.view()) == Some((*change)@)); assert(numeric(part.view())); }
        let ghost previous = *data;
        let ghost previous_changes = *inventory_changes;
        let borrowed = match &links { Some(links) => Some(&**links), None => None };
        stage_partsupp_cell(data, supplier, &part, &*change, borrowed, inventory_changes);
        proof {
            assert forall|nation: i64, selected_part: i64, value: bool| #[trigger] profile_cell(*inventory_changes, nation, selected_part, value)
                == profile_cell(initial_changes, nation, selected_part, value) + sum(groups.entries().take(processed@ + 1), |row: (KeyView, Moment)|
                    if row.0 == numeric_key(selected_part as int) { moment_measure(row.1, value) * count_at(initial.suppliers, supplier.view(), numeric_key(nation as int)) } else { 0 }) by {
                let weight = |row: (KeyView, Moment)| if row.0 == numeric_key(selected_part as int) { moment_measure(row.1, value) * count_at(initial.suppliers, supplier.view(), numeric_key(nation as int)) } else { 0 };
                lemma_sum_take(groups.entries(), processed@, weight);
                assert(profile_cell(previous_changes, nation, selected_part, value) == profile_cell(initial_changes, nation, selected_part, value) + sum(groups.entries().take(processed@), weight));
                assert(count_at(initial.suppliers, supplier.view(), numeric_key(nation as int)) * moment_measure((*change)@, value)
                    == moment_measure((*change)@, value) * count_at(initial.suppliers, supplier.view(), numeric_key(nation as int))) by (nonlinear_arith);
            }
            assert forall|other_supplier: i64, other_part: i64, value: bool| #[trigger] profile_cell(data.profiles, other_supplier, other_part, value)
                == profile_cell(initial.profiles, other_supplier, other_part, value)
                    + sum(groups.entries().take(processed@ + 1), |row: (KeyView, Moment)|
                        if other_supplier == supplier.number && row.0 == numeric_key(other_part as int) { crate::construction::moment_measure(row.1, value) } else { 0 }) by {
                lemma_sum_take(groups.entries(), processed@, |row: (KeyView, Moment)|
                    if other_supplier == supplier.number && row.0 == numeric_key(other_part as int) { crate::construction::moment_measure(row.1, value) } else { 0 });
                assert(profile_cell(previous.profiles, other_supplier, other_part, value)
                    == profile_cell(initial.profiles, other_supplier, other_part, value)
                        + sum(groups.entries().take(processed@), |row: (KeyView, Moment)|
                            if other_supplier == supplier.number && row.0 == numeric_key(other_part as int) { crate::construction::moment_measure(row.1, value) } else { 0 }));
            }
        }
        processed = Ghost(processed@ + 1);
    }
}

pub fn stage_partsupp(data: &mut Data, changes: &Profiles, inventory_changes: &mut Profiles)
    requires old(data).structures_valid(), profile_storage_ok(old(data).profiles), profile_layout(*changes),
        profile_layout(*old(inventory_changes)), counts_layout(old(data).suppliers, true, true),
    ensures final(data).structures_valid(), profile_storage_ok(final(data).profiles), profile_frame(*old(data), *final(data)),
        profile_layout(*final(inventory_changes)),
        forall|nation: i64, part: i64, value: bool| #[trigger] profile_cell(*final(inventory_changes), nation, part, value)
            == profile_cell(*old(inventory_changes), nation, part, value)
                + sum(changes.entries(), |row: (KeyView, Packed)| partsupp_effect(row, old(data).suppliers, nation, part, value)),
        forall|supplier: i64, part: i64, value: bool| #[trigger] profile_cell(final(data).profiles, supplier, part, value)
            == profile_cell(old(data).profiles, supplier, part, value) + profile_cell(*changes, supplier, part, value),
{
    let ghost initial = *data;
    let ghost initial_changes = *inventory_changes;
    let mut suppliers = changes.cursor();
    let mut processed = Ghost(0int);
    loop
        invariant data.structures_valid(), profile_storage_ok(data.profiles), profile_frame(initial, *data), initial == *old(data),
            profile_layout(*inventory_changes),
            initial_changes == *old(inventory_changes), counts_layout(initial.suppliers, true, true),
            forall|nation: i64, part: i64, value: bool| #[trigger] profile_cell(*inventory_changes, nation, part, value)
                == profile_cell(initial_changes, nation, part, value)
                    + sum(changes.entries().take(processed@), |row: (KeyView, Packed)| partsupp_effect(row, initial.suppliers, nation, part, value)),
            profile_layout(*changes), 0 <= processed@ <= changes.entries().len(), suppliers.remaining() == changes.entries().skip(processed@),
            forall|supplier: i64, part: i64, value: bool| #[trigger] profile_cell(data.profiles, supplier, part, value)
                == profile_cell(initial.profiles, supplier, part, value)
                    + sum(changes.entries().take(processed@), |row: (KeyView, Packed)|
                        if row.0 == numeric_key(supplier as int) { crate::construction::moment_measure(moment_at(row.1, part as int), value) } else { 0 }),
        decreases suppliers.remaining().len(),
    {
        let (supplier, groups) = match suppliers.next() {
            Some(row) => row,
            None => {
                proof {
                    assert(processed@ == changes.entries().len()); assert(changes.entries().take(processed@) =~= changes.entries());
                    assert forall|supplier: i64, part: i64, value: bool| #[trigger] profile_cell(data.profiles, supplier, part, value)
                        == profile_cell(initial.profiles, supplier, part, value) + profile_cell(*changes, supplier, part, value) by {
                        let weight = |row: (KeyView, Packed)| crate::construction::moment_measure(moment_at(row.1, part as int), value);
                        verus_prooflib::ordered::lemma_key_contribution(changes, numeric_key(supplier as int), weight);
                        assert((|row: (KeyView, Packed)| if row.0 == numeric_key(supplier as int) { crate::construction::moment_measure(moment_at(row.1, part as int), value) } else { 0 })
                            =~= (|row: (KeyView, Packed)| if row.0 == numeric_key(supplier as int) { weight(row) } else { 0 }));
                        reveal(verus_prooflib::ordered::contribution);
                        if changes.at(numeric_key(supplier as int)).is_none() { assert(profile_cell(*changes, supplier, part, value) == 0); }
                    }
                }
                return;
            },
        };
        proof { changes.model(); assert(changes.at(supplier.view()) == Some(*groups)); assert(numeric(supplier.view())); }
        let ghost previous = *data;
        let ghost previous_changes = *inventory_changes;
        stage_partsupp_groups(data, &supplier, &*groups, inventory_changes);
        proof {
            assert forall|nation: i64, part: i64, value: bool| #[trigger] profile_cell(*inventory_changes, nation, part, value)
                == profile_cell(initial_changes, nation, part, value)
                    + sum(changes.entries().take(processed@ + 1), |row: (KeyView, Packed)| partsupp_effect(row, initial.suppliers, nation, part, value)) by {
                let weight = |row: (KeyView, Packed)| partsupp_effect(row, initial.suppliers, nation, part, value);
                lemma_sum_take(changes.entries(), processed@, weight);
                assert(profile_cell(previous_changes, nation, part, value) == profile_cell(initial_changes, nation, part, value) + sum(changes.entries().take(processed@), weight));
            }
            assert forall|other: i64, part: i64, value: bool| #[trigger] profile_cell(data.profiles, other, part, value)
                == profile_cell(initial.profiles, other, part, value)
                    + sum(changes.entries().take(processed@ + 1), |row: (KeyView, Packed)|
                        if row.0 == numeric_key(other as int) { crate::construction::moment_measure(moment_at(row.1, part as int), value) } else { 0 }) by {
                lemma_sum_take(changes.entries(), processed@, |row: (KeyView, Packed)|
                    if row.0 == numeric_key(other as int) { crate::construction::moment_measure(moment_at(row.1, part as int), value) } else { 0 });
                assert(profile_cell(previous.profiles, other, part, value)
                    == profile_cell(initial.profiles, other, part, value)
                        + sum(changes.entries().take(processed@), |row: (KeyView, Packed)|
                            if row.0 == numeric_key(other as int) { crate::construction::moment_measure(moment_at(row.1, part as int), value) } else { 0 }));
            }
        }
        processed = Ghost(processed@ + 1);
    }
}

pub open spec fn partsupp_effect(row: (KeyView, Packed), suppliers: Counts, nation: i64, part: i64, value: bool) -> int {
    moment_measure(moment_at(row.1, part as int), value) * count_at(suppliers, row.0, numeric_key(nation as int))
}

pub fn stage_supplier(data: &mut Data, changes: &Counts, inventory_changes: &mut Profiles)
    requires old(data).structures_valid(), nonzero_counts(old(data).suppliers), nonzero_counts(old(data).suppliers_reverse),
        profile_layout(*old(inventory_changes)), profile_storage_ok(old(data).profiles),
    counts_layout(old(data).suppliers, true, true), counts_layout(old(data).suppliers_reverse, true, true), counts_layout(*changes, true, true),
    ensures final(data).structures_valid(), final(data).profiles == old(data).profiles,
        profile_layout(*final(inventory_changes)),
        forall|nation: i64, part: i64, value: bool| #[trigger] profile_cell(*final(inventory_changes), nation, part, value)
            == profile_cell(*old(inventory_changes), nation, part, value)
                + sum(changes.entries(), |row: (KeyView, Index<Wide>)| supplier_effect(row, old(data).profiles, nation, part, value)),
        final(data).nations == old(data).nations, final(data).nations_reverse == old(data).nations_reverse,
        final(data).inventories == old(data).inventories, final(data).names == old(data).names,
        nonzero_counts(final(data).suppliers), nonzero_counts(final(data).suppliers_reverse),
        counts_layout(final(data).suppliers, true, true), counts_layout(final(data).suppliers_reverse, true, true),
        forall|outer: KeyView, inner: KeyView| #[trigger] count_at(final(data).suppliers, outer, inner)
            == count_at(old(data).suppliers, outer, inner) + count_at(*changes, outer, inner),
        forall|outer: KeyView, inner: KeyView| #[trigger] count_at(final(data).suppliers_reverse, inner, outer)
            == count_at(old(data).suppliers_reverse, inner, outer) + count_at(*changes, outer, inner),
{
    crate::counts::apply_pairs(&mut data.suppliers, &mut data.suppliers_reverse, changes, true);
    stage_supplier_inventory(&data.profiles, changes, inventory_changes)
}

pub fn stage_supplier_inventory(profiles: &Profiles, changes: &Counts, inventory_changes: &mut Profiles)
    requires profile_layout(*old(inventory_changes)), profile_storage_ok(*profiles), counts_layout(*changes, true, true),
    ensures profile_layout(*final(inventory_changes)),
        forall|nation: i64, part: i64, value: bool| #[trigger] profile_cell(*final(inventory_changes), nation, part, value)
            == profile_cell(*old(inventory_changes), nation, part, value)
                + sum(changes.entries(), |row: (KeyView, Index<Wide>)| supplier_effect(row, *profiles, nation, part, value)),
{
    let ghost initial = *inventory_changes;
    let mut processed = Ghost(0int);
    let mut suppliers = changes.cursor();
    loop
        invariant profile_layout(*inventory_changes), initial == *old(inventory_changes), profile_storage_ok(*profiles), counts_layout(*changes, true, true),
            0 <= processed@ <= changes.entries().len(), suppliers.remaining() == changes.entries().skip(processed@),
            forall|nation: i64, part: i64, value: bool| #[trigger] profile_cell(*inventory_changes, nation, part, value)
                == profile_cell(initial, nation, part, value)
                    + sum(changes.entries().take(processed@), |row: (KeyView, Index<Wide>)| supplier_effect(row, *profiles, nation, part, value)),
        decreases suppliers.remaining().len(),
    {
        let (supplier, links) = match suppliers.next() {
            Some(row) => row,
            None => {
                proof { assert(processed@ == changes.entries().len()); assert(changes.entries().take(processed@) =~= changes.entries()); }
                return;
            },
        };
        proof { changes.model(); assert(changes.at(supplier.view()) == Some(*links)); assert(numeric(supplier.view())); }
        let ghost previous = *inventory_changes;
        if let Some(groups) = profiles.get(&supplier) {
            crate::staging::supplier_group(inventory_changes, &*groups, &*links);
            proof {
                assert forall|nation: i64, part: i64, value: bool| #[trigger] profile_cell(*inventory_changes, nation, part, value)
                    == profile_cell(previous, nation, part, value) + supplier_effect((supplier.view(), *links), *profiles, nation, part, value) by {
                    assert(profiles.at(supplier.view()) == Some(*groups));
                    assert(profile_at(*profiles, supplier.number as int, part as int) == moment_at(*groups, part as int));
                }
            }
        } else {
            proof {
                assert(profiles.at(supplier.view()).is_none());
                assert forall|nation: i64, part: i64, value: bool| #[trigger] profile_cell(*inventory_changes, nation, part, value)
                    == profile_cell(previous, nation, part, value) + supplier_effect((supplier.view(), *links), *profiles, nation, part, value) by {
                    assert(moment_measure(profile_at(*profiles, supplier.number as int, part as int), value) == 0);
                }
            }
        }
        proof {
            assert forall|nation: i64, part: i64, value: bool| #[trigger] profile_cell(*inventory_changes, nation, part, value)
                == profile_cell(initial, nation, part, value)
                    + sum(changes.entries().take(processed@ + 1), |row: (KeyView, Index<Wide>)| supplier_effect(row, *profiles, nation, part, value)) by {
                let weight = |row: (KeyView, Index<Wide>)| supplier_effect(row, *profiles, nation, part, value);
                lemma_sum_take(changes.entries(), processed@, weight);
                assert(changes.entries()[processed@] == (supplier.view(), *links));
                assert(profile_cell(previous, nation, part, value) == profile_cell(initial, nation, part, value) + sum(changes.entries().take(processed@), weight));
                assert(profile_cell(*inventory_changes, nation, part, value) == profile_cell(previous, nation, part, value)
                    + moment_measure(profile_at(*profiles, supplier.number as int, part as int), value) * crate::construction::link_count(*links, nation));
            }
        }
        processed = Ghost(processed@ + 1);
    }
}

pub open spec fn supplier_effect(row: (KeyView, Index<Wide>), profiles: Profiles, nation: i64, part: i64, value: bool) -> int {
    moment_measure(profile_at(profiles, row.0.0, part as int), value) * crate::construction::link_count(row.1, nation)
}

pub fn apply_nation(data: &mut Data, changes: &Counts)
    requires old(data).structures_valid(), nonzero_counts(old(data).nations), nonzero_counts(old(data).nations_reverse),
        crate::names::indexed_names_ok(*old(data)),
        counts_layout(old(data).nations, true, false), counts_layout(old(data).nations_reverse, false, true), counts_layout(*changes, false, true),
    ensures final(data).structures_valid(), final(data).profiles == old(data).profiles,
        crate::names::indexed_names_ok(*final(data)),
        final(data).suppliers == old(data).suppliers, final(data).suppliers_reverse == old(data).suppliers_reverse,
        final(data).inventories == old(data).inventories,
        nonzero_counts(final(data).nations), nonzero_counts(final(data).nations_reverse),
        counts_layout(final(data).nations, true, false), counts_layout(final(data).nations_reverse, false, true),
        forall|name: KeyView, nation: KeyView| #[trigger] count_at(final(data).nations_reverse, name, nation)
            == count_at(old(data).nations_reverse, name, nation) + count_at(*changes, name, nation),
        forall|name: KeyView, nation: KeyView| #[trigger] count_at(final(data).nations, nation, name)
            == count_at(old(data).nations, nation, name) + count_at(*changes, name, nation),
{
    let ghost basis = *data;
    crate::counts::apply_pairs(&mut data.nations_reverse, &mut data.nations, changes, false);
    let ghost committed = *data;
    let mut processed = Ghost(0int);
    let mut names = changes.cursor();
    proof {
        assert forall|key: KeyView| (#[trigger] changes.at(key)).is_none() implies crate::names::binding_ok(*data, key) by {
            assert(data.nations_reverse.at(key) == basis.nations_reverse.at(key));
            assert(crate::names::binding_ok(basis, key));
        }
    }
    loop
        invariant data.structures_valid(), same_dimensions(committed, *data), data.inventories == committed.inventories,
            basis == *old(data), crate::names::indexed_names_ok(basis), counts_layout(basis.nations_reverse, false, true),
            committed.inventories == basis.inventories, counts_layout(*changes, false, true),
            0 <= processed@ <= changes.entries().len(), names.remaining() == changes.entries().skip(processed@),
            forall|position: int| 0 <= position < processed@ ==> #[trigger] crate::names::binding_ok(*data, changes.entries()[position].0),
            forall|position: int| processed@ <= position < changes.entries().len() ==> (#[trigger] data.names.at(changes.entries()[position].0)) == basis.names.at(changes.entries()[position].0),
            forall|key: KeyView| (#[trigger] changes.at(key)).is_none() ==> crate::names::binding_ok(*data, key),
            committed.profiles == old(data).profiles, committed.suppliers == old(data).suppliers,
            committed.suppliers_reverse == old(data).suppliers_reverse, committed.inventories == old(data).inventories,
            nonzero_counts(committed.nations), nonzero_counts(committed.nations_reverse),
            counts_layout(committed.nations, true, false), counts_layout(committed.nations_reverse, false, true),
            forall|name: KeyView, nation: KeyView| #[trigger] count_at(committed.nations_reverse, name, nation)
                == count_at(old(data).nations_reverse, name, nation) + count_at(*changes, name, nation),
            forall|name: KeyView, nation: KeyView| #[trigger] count_at(committed.nations, nation, name)
                == count_at(old(data).nations, nation, name) + count_at(*changes, name, nation),
        decreases names.remaining().len(),
    {
        let (name, links) = match names.next() {
            Some(row) => row,
            None => {
                proof {
                    assert(processed@ == changes.entries().len()); changes.model();
                    assert forall|key: KeyView| #[trigger] crate::names::binding_ok(*data, key) by {
                        if changes.at(key).is_some() {
                            let position = choose|position: int| 0 <= position < changes.entries().len() && (#[trigger] changes.entries()[position]).0 == key;
                            assert(crate::names::binding_ok(*data, changes.entries()[position].0));
                        }
                    }
                }
                return;
            },
        };
        proof {
            changes.model(); assert(changes.entries()[processed@] == (name.view(), *links));
            verus_prooflib::ordered::lemma_ordered_unique(changes.entries());
            assert(changes.at(name.view()) == Some(*links));
            assert(crate::names::binding_ok(basis, name.view()));
            assert(data.names.at(name.view()) == basis.names.at(name.view()));
            assert forall|key: KeyView| #[trigger] count_at(data.nations_reverse, name.view(), key)
                == count_at(basis.nations_reverse, name.view(), key) + crate::counts::inner_at(*links, key) by {}
        }
        let ghost previous = *data;
        refresh_name(data, &name, Some(&*links), Ghost(basis));
        proof {
            assert forall|position: int| 0 <= position < processed@ + 1 implies #[trigger] crate::names::binding_ok(*data, changes.entries()[position].0) by {
                if position < processed@ {
                    assert(changes.entries()[position].0 != name.view());
                    assert(crate::names::binding_ok(previous, changes.entries()[position].0));
                }
            }
            assert forall|position: int| processed@ + 1 <= position < changes.entries().len() implies (#[trigger] data.names.at(changes.entries()[position].0)) == basis.names.at(changes.entries()[position].0) by {
                assert(changes.entries()[position].0 != name.view());
            }
            assert forall|key: KeyView| (#[trigger] changes.at(key)).is_none() implies crate::names::binding_ok(*data, key) by {
                assert(key != name.view()); assert(crate::names::binding_ok(previous, key));
            }
        }
        processed = Ghost(processed@ + 1);
    }
}

}
