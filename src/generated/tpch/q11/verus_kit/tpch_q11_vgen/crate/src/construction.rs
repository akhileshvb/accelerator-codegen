use vstd::prelude::*;
use verus_common::sums::{sum, lemma_sum_take};
use verus_prooflib::ordered::{Index, Key, KeyView};
use crate::inventory::*;
use crate::maintenance::change_inventory;
use crate::model::*;
use crate::values::*;
use crate::gen_spec::*;
use crate::semantics::db_bounded;

verus! {

pub open spec fn moment_measure(moment: Moment, value: bool) -> int {
    if value { moment.value as int } else { moment.count as int }
}

pub open spec fn inventory_measure(inventories: Index<Inventory>, nation: i64, part: Option<i64>, value: bool) -> int {
    match part {
        Some(part) => moment_measure(inventory_at(inventories, nation as int, part as int), value),
        None => moment_measure(inventory_total(inventories, nation as int), value),
    }
}

pub open spec fn profile_row_measure(row: (KeyView, Moment), part: Option<i64>, value: bool) -> int {
    if part.is_none() || row.0 == numeric_key(part.unwrap() as int) { moment_measure(row.1, value) } else { 0 }
}

pub open spec fn profile_measure(groups: Index<Moment>, part: Option<i64>, value: bool) -> int {
    sum(groups.entries(), |row: (KeyView, Moment)| profile_row_measure(row, part, value))
}

pub open spec fn profile_keys_ok(groups: Index<Moment>) -> bool {
    groups.wf() && forall|key: KeyView| (#[trigger] groups.at(key)).is_some() ==> numeric(key)
}

pub open spec fn profile_measure_at(profiles: Profiles, supplier: KeyView, part: Option<i64>, value: bool) -> int {
    match profiles.at(supplier) { Some(groups) => profile_measure(groups, part, value), None => 0 }
}

pub open spec fn link_count(links: Index<i128>, nation: i64) -> int {
    match links.at(numeric_key(nation as int)) { Some(count) => count as int, None => 0 }
}

pub open spec fn supplier_measure(profiles: Profiles, row: (KeyView, Index<i128>), nation: i64, part: Option<i64>, value: bool) -> int {
    profile_measure_at(profiles, row.0, part, value) * link_count(row.1, nation)
}

pub open spec fn indexed_inventory_ok(data: Data) -> bool {
    forall|nation: i64, part: Option<i64>, value: bool|
        #[trigger] inventory_measure(data.inventories, nation, part, value)
            == sum(data.suppliers.entries(), |row: (KeyView, Index<i128>)| supplier_measure(data.profiles, row, nation, part, value))
}

pub open spec fn part_weight(row: PartsuppRow, part: i64, value: bool) -> int {
    if row.ps_partkey == part { if value { row.ps_availqty * row.ps_supplycost } else { 1 } } else { 0 }
}

pub proof fn lemma_profile_part(profiles: Profiles, supplier: KeyView, part: i64, value: bool)
    requires profile_layout(profiles),
    ensures profile_measure_at(profiles, supplier, Some(part), value)
        == moment_measure(profile_at(profiles, supplier.0, part as int), value) || supplier.1 != Seq::<u8>::empty(),
{
    if supplier.1 == Seq::<u8>::empty() {
        if let Some(groups) = profiles.at(supplier) {
            let weight = |row: (KeyView, Moment)| moment_measure(row.1, value);
            verus_prooflib::ordered::lemma_key_contribution(&groups, numeric_key(part as int), weight);
            assert((|row: (KeyView, Moment)| profile_row_measure(row, Some(part), value))
                =~= (|row: (KeyView, Moment)| if row.0 == numeric_key(part as int) { weight(row) } else { 0 }));
            reveal(verus_prooflib::ordered::contribution);
            if groups.at(numeric_key(part as int)).is_none() { assert(moment_measure(moment_at(groups, part as int), value) == 0); }
        }
    }
}

pub proof fn lemma_inventory_part(data: Data, db: Db, nation: i64, part: i64, value: bool)
    requires data.profiles_ok(db), data.suppliers_ok(db), db_bounded(db),
    ensures sum(data.suppliers.entries(), |row: (KeyView, Index<i128>)| supplier_measure(data.profiles, row, nation, Some(part), value))
        == if value { h_value(db, nation as int, part as int) } else { h_count(db, nation as int, part as int) },
        indexed_inventory_ok(data) ==> inventory_measure(data.inventories, nation, Some(part), value)
        == if value { h_value(db, nation as int, part as int) } else { h_count(db, nation as int, part as int) },
{
    let entries = data.suppliers.entries();
    let left = |entry: (KeyView, Index<i128>)| supplier_measure(data.profiles, entry, nation, Some(part), value);
    let pair = |entry: (KeyView, Index<i128>), row: PartsuppRow|
        if entry.0 == numeric_key(row.ps_suppkey) { part_weight(row, part, value) * link_count(entry.1, nation) } else { 0 };
    let expanded = |entry: (KeyView, Index<i128>)| sum(db.partsupp, |row: PartsuppRow| pair(entry, row));
    data.suppliers.model();
    assert forall|position: int| 0 <= position < entries.len() implies left(#[trigger] entries[position]) == expanded(entries[position]) by {
        let entry = entries[position];
        assert(data.suppliers.at(entry.0) == Some(entry.1));
        assert(numeric(entry.0));
        let supplier = entry.0.0 as i64;
        assert(supplier as int == entry.0.0);
        assert(profile_at(data.profiles, supplier as int, part as int).count as int == p_count(db, supplier as int, part as int));
        assert(profile_at(data.profiles, supplier as int, part as int).value as int == p_value(db, supplier as int, part as int));
        lemma_profile_part(data.profiles, entry.0, part, value);
        assert(moment_measure(profile_at(data.profiles, entry.0.0, part as int), value)
            == if value { p_value(db, entry.0.0, part as int) } else { p_count(db, entry.0.0, part as int) });
        let raw = |row: PartsuppRow| if row.ps_suppkey == entry.0.0 { part_weight(row, part, value) } else { 0 };
        let scaled = |row: PartsuppRow| pair(entry, row);
        assert forall|row: PartsuppRow| #[trigger] scaled(row) == link_count(entry.1, nation) * raw(row) by {
            if row.ps_suppkey == entry.0.0 {
                assert(part_weight(row, part, value) * link_count(entry.1, nation)
                    == link_count(entry.1, nation) * part_weight(row, part, value)) by (nonlinear_arith);
            }
        }
        verus_common::sums::lemma_sum_scale(db.partsupp, raw, scaled, link_count(entry.1, nation));
        if value {
            assert(raw =~= (|row: PartsuppRow| if row.ps_suppkey == entry.0.0 && row.ps_partkey == part {
                row.ps_availqty * row.ps_supplycost
            } else { 0 }));
        } else {
            assert(raw =~= (|row: PartsuppRow| if row.ps_suppkey == entry.0.0 && row.ps_partkey == part { 1int } else { 0 }));
        }
        assert(left(entry) == expanded(entry)) by (nonlinear_arith)
            requires profile_measure_at(data.profiles, entry.0, Some(part), value) == sum(db.partsupp, raw),
                expanded(entry) == link_count(entry.1, nation) * sum(db.partsupp, raw),
                left(entry) == profile_measure_at(data.profiles, entry.0, Some(part), value) * link_count(entry.1, nation);
    }
    verus_common::sums::lemma_sum_ext(entries, left, expanded);
    let right = |row: PartsuppRow| sum(entries, |entry: (KeyView, Index<i128>)| pair(entry, row));
    verus_common::sums::lemma_sum_swap(entries, db.partsupp, pair, expanded, right);
    let expected = |row: PartsuppRow| part_weight(row, part, value) * s_count(db, row.ps_suppkey, nation as int);
    assert forall|position: int| 0 <= position < db.partsupp.len() implies right(#[trigger] db.partsupp[position]) == expected(db.partsupp[position]) by {
        let row = db.partsupp[position];
        let weight = |entry: (KeyView, Index<i128>)| part_weight(row, part, value) * link_count(entry.1, nation);
        verus_prooflib::ordered::lemma_key_contribution(&data.suppliers, numeric_key(row.ps_suppkey), weight);
        assert((|entry: (KeyView, Index<i128>)| pair(entry, row))
            =~= (|entry: (KeyView, Index<i128>)| if entry.0 == numeric_key(row.ps_suppkey) { weight(entry) } else { 0 }));
        reveal(verus_prooflib::ordered::contribution);
        assert(count_at(data.suppliers, numeric_key(row.ps_suppkey), numeric_key(nation as int)) == s_count(db, row.ps_suppkey, nation as int));
        if let Some(links) = data.suppliers.at(numeric_key(row.ps_suppkey)) {
            assert(link_count(links, nation) == s_count(db, row.ps_suppkey, nation as int));
        } else { assert(s_count(db, row.ps_suppkey, nation as int) == 0); }
    }
    verus_common::sums::lemma_sum_ext(db.partsupp, right, expected);
    if value {
        assert(expected =~= (|row: PartsuppRow| if row.ps_partkey == part {
            row.ps_availqty * row.ps_supplycost * s_count(db, row.ps_suppkey, nation as int)
        } else { 0 }));
    } else {
        assert(expected =~= (|row: PartsuppRow| if row.ps_partkey == part { s_count(db, row.ps_suppkey, nation as int) } else { 0 }));
    }
}

#[verifier::auto_ext_equal(assert, assert_by, ensures)]
pub proof fn lemma_inventory_total(data: Data, db: Db, nation: i64, value: bool)
    requires data.structures_valid(), db_bounded(db),
        forall|part: i64| #[trigger] inventory_at(data.inventories, nation as int, part as int).count as int == h_count(db, nation as int, part as int)
            && inventory_at(data.inventories, nation as int, part as int).value as int == h_value(db, nation as int, part as int),
        data.inventories.at(numeric_key(nation as int)).is_some(),
    ensures moment_measure(inventory_total(data.inventories, nation as int), value)
        == if value { h_total_value(db, nation as int) } else { h_total_count(db, nation as int) },
{
    let inventory = data.inventories.at(numeric_key(nation as int)).unwrap();
    assert(inventory.valid());
    let left_project = |row: (KeyView, Moment)| row.0.0;
    let right_project = |row: PartsuppRow| row.ps_partkey;
    let left_weight = |row: (KeyView, Moment)| moment_measure(row.1, value);
    let right_weight = |row: PartsuppRow| if value {
        row.ps_availqty * row.ps_supplycost * s_count(db, row.ps_suppkey, nation as int)
    } else { s_count(db, row.ps_suppkey, nation as int) };
    inventory.groups.model();
    assert forall|number: int| #[trigger] verus_prooflib::class_total(inventory.groups.entries(), left_project, left_weight, number)
        == verus_prooflib::class_total(db.partsupp, right_project, right_weight, number) by {
        let key = numeric_key(number);
        verus_prooflib::ordered::lemma_key_contribution(&inventory.groups, key, left_weight);
        reveal(verus_prooflib::class_total);
        reveal(verus_prooflib::ordered::contribution);
        let selected = |row: PartsuppRow| if right_project(row) == number { right_weight(row) } else { 0 };
        let grouped = |row: (KeyView, Moment)| if left_project(row) == number { left_weight(row) } else { 0 };
        let keyed = |row: (KeyView, Moment)| if row.0 == key { left_weight(row) } else { 0 };
        assert forall|position: int| 0 <= position < inventory.groups.entries().len() implies grouped(#[trigger] inventory.groups.entries()[position]) == keyed(inventory.groups.entries()[position]) by {
            assert(inventory.groups.at(inventory.groups.entries()[position].0).is_some());
        }
        verus_common::sums::lemma_sum_ext(inventory.groups.entries(), grouped, keyed);
        if numeric(key) {
            let part = key.0 as i64;
            assert(key == numeric_key(part as int));
            assert(inventory_at(data.inventories, nation as int, part as int) == moment_at(inventory.groups, part as int));
            assert(inventory_at(data.inventories, nation as int, part as int).count as int == h_count(db, nation as int, part as int));
            assert(inventory_at(data.inventories, nation as int, part as int).value as int == h_value(db, nation as int, part as int));
            if value {
                assert(selected =~= (|row: PartsuppRow| if row.ps_partkey == part {
                    row.ps_availqty * row.ps_supplycost * s_count(db, row.ps_suppkey, nation as int)
                } else { 0 }));
            } else {
                assert(selected =~= (|row: PartsuppRow| if row.ps_partkey == part { s_count(db, row.ps_suppkey, nation as int) } else { 0 }));
            }
            if inventory.groups.at(key).is_none() { assert(moment_at(inventory.groups, part as int) == zero_moment()); }
            assert(verus_prooflib::ordered::contribution(inventory.groups, key, left_weight)
                == moment_measure(moment_at(inventory.groups, part as int), value));
            assert(sum(db.partsupp, selected) == moment_measure(moment_at(inventory.groups, part as int), value));
        } else {
            assert(inventory.groups.at(key).is_none());
            assert forall|position: int| 0 <= position < db.partsupp.len() implies selected(#[trigger] db.partsupp[position]) == 0 by {
                assert(i64::MIN <= right_project(db.partsupp[position]) <= i64::MAX);
            }
            verus_common::sums::lemma_sum_zero(db.partsupp, selected);
            assert(verus_prooflib::ordered::contribution(inventory.groups, key, left_weight) == 0);
        }
    }
    let one = |key: int| 1int;
    verus_prooflib::lemma_equal_classes(inventory.groups.entries(), db.partsupp,
        left_project, right_project, left_weight, right_weight, one);
    assert((|row: (KeyView, Moment)| left_weight(row) * one(left_project(row))) =~= left_weight);
    assert((|row: PartsuppRow| right_weight(row) * one(right_project(row))) =~= right_weight);
    if value {
        assert(left_weight =~= (|row: (KeyView, Moment)| row.1.value as int));
        assert(right_weight =~= (|row: PartsuppRow| row.ps_availqty * row.ps_supplycost * s_count(db, row.ps_suppkey, nation as int)));
    } else {
        assert(left_weight =~= (|row: (KeyView, Moment)| row.1.count as int));
        assert(right_weight =~= (|row: PartsuppRow| s_count(db, row.ps_suppkey, nation as int)));
    }
}

pub proof fn lemma_indexed_inventories(data: Data, db: Db)
    requires data.structures_valid(), data.profiles_ok(db), data.suppliers_ok(db), db_bounded(db), indexed_inventory_ok(data),
    ensures data.inventories_ok(db),
{
    assert forall|nation: i64, part: i64| #[trigger] inventory_at(data.inventories, nation as int, part as int).count as int == h_count(db, nation as int, part as int)
        && inventory_at(data.inventories, nation as int, part as int).value as int == h_value(db, nation as int, part as int) by {
        lemma_inventory_part(data, db, nation, part, false);
        lemma_inventory_part(data, db, nation, part, true);
    }
    assert forall|key: KeyView| (#[trigger] data.inventories.at(key)).is_some() implies {
        let inventory = data.inventories.at(key).unwrap();
        &&& numeric(key)
        &&& inventory.valid()
        &&& inventory.total.count > 0
        &&& inventory.total.count as int == h_total_count(db, key.0)
        &&& inventory.total.value as int == h_total_value(db, key.0)
    } by {
        assert(numeric(key));
        lemma_inventory_total(data, db, key.0 as i64, false);
        lemma_inventory_total(data, db, key.0 as i64, true);
    }
}

pub fn add_profile_inventory(data: &mut Data, nation: i64, groups: &Index<Moment>, weight: i128) -> (result: bool)
    requires old(data).structures_valid(), profile_keys_ok(*groups),
    ensures final(data).structures_valid(), same_dimensions(*old(data), *final(data)),
        forall|key: KeyView| (#[trigger] old(data).names.at(key)).is_none() ==> final(data).names.at(key).is_none(),
        result ==> forall|other: i64, part: Option<i64>, value: bool|
            #[trigger] inventory_measure(final(data).inventories, other, part, value)
                == inventory_measure(old(data).inventories, other, part, value)
                    + if other == nation { profile_measure(*groups, part, value) * weight as int } else { 0 },
{
    let ghost initial = *data;
    let mut processed = Ghost(0int);
    let mut cursor = groups.cursor();
    loop
        invariant
            initial == *old(data), data.structures_valid(), same_dimensions(initial, *data),
            forall|key: KeyView| (#[trigger] initial.names.at(key)).is_none() ==> data.names.at(key).is_none(),
            profile_keys_ok(*groups), 0 <= processed@ <= groups.entries().len(),
            cursor.remaining() == groups.entries().skip(processed@),
            forall|other: i64, part: Option<i64>, value: bool|
                #[trigger] inventory_measure(data.inventories, other, part, value)
                    == inventory_measure(initial.inventories, other, part, value)
                        + sum(groups.entries().take(processed@), |row: (KeyView, Moment)|
                            if other == nation { profile_row_measure(row, part, value) * weight as int } else { 0 }),
        decreases cursor.remaining().len(),
    {
        let (key, moment) = match cursor.next() {
            Some(row) => row,
            None => {
                proof {
                    assert(processed@ == groups.entries().len());
                    assert(groups.entries().take(processed@) =~= groups.entries());
                    assert forall|other: i64, part: Option<i64>, value: bool|
                        #[trigger] inventory_measure(data.inventories, other, part, value)
                            == inventory_measure(initial.inventories, other, part, value)
                                + if other == nation { profile_measure(*groups, part, value) * weight as int } else { 0 } by {
                        assert(inventory_measure(data.inventories, other, part, value)
                            == inventory_measure(initial.inventories, other, part, value)
                                + sum(groups.entries().take(processed@), |row: (KeyView, Moment)|
                                    if other == nation { profile_row_measure(row, part, value) * weight as int } else { 0 }));
                        if other == nation {
                            assert((|row: (KeyView, Moment)| if other == nation { profile_row_measure(row, part, value) * weight as int } else { 0 })
                                =~= (|row: (KeyView, Moment)| profile_row_measure(row, part, value) * weight as int));
                            assert forall|row: (KeyView, Moment)| #[trigger] (profile_row_measure(row, part, value) * weight as int)
                                == weight as int * profile_row_measure(row, part, value) by {
                                assert(profile_row_measure(row, part, value) * weight as int == weight as int * profile_row_measure(row, part, value)) by (nonlinear_arith);
                            }
                            verus_common::sums::lemma_sum_scale(groups.entries(),
                                |row: (KeyView, Moment)| profile_row_measure(row, part, value),
                                |row: (KeyView, Moment)| profile_row_measure(row, part, value) * weight as int,
                                weight as int);
                            assert(sum(groups.entries(), |row: (KeyView, Moment)| profile_row_measure(row, part, value) * weight as int)
                                == profile_measure(*groups, part, value) * weight as int) by (nonlinear_arith)
                                requires sum(groups.entries(), |row: (KeyView, Moment)| profile_row_measure(row, part, value) * weight as int)
                                    == weight as int * profile_measure(*groups, part, value);
                        } else {
                            verus_common::sums::lemma_sum_zero(groups.entries(),
                                |row: (KeyView, Moment)| if other == nation { profile_row_measure(row, part, value) * weight as int } else { 0 });
                        }
                    }
                }
                return true;
            },
        };
        proof {
            groups.model();
            assert(groups.at(key.view()) == Some(*moment));
            assert(numeric(key.view()));
        }
        let change = match scale(*moment, weight) { Some(change) => change, None => return false };
        let ghost previous = *data;
        if !change_inventory(data, nation, key.number, change) { return false; }
        proof {
            assert forall|other: i64, part: Option<i64>, value: bool|
                #[trigger] inventory_measure(data.inventories, other, part, value)
                    == inventory_measure(initial.inventories, other, part, value)
                        + sum(groups.entries().take(processed@ + 1), |row: (KeyView, Moment)|
                            if other == nation { profile_row_measure(row, part, value) * weight as int } else { 0 }) by {
                lemma_sum_take(groups.entries(), processed@, |row: (KeyView, Moment)|
                    if other == nation { profile_row_measure(row, part, value) * weight as int } else { 0 });
                assert(inventory_measure(previous.inventories, other, part, value)
                    == inventory_measure(initial.inventories, other, part, value)
                        + sum(groups.entries().take(processed@), |row: (KeyView, Moment)|
                            if other == nation { profile_row_measure(row, part, value) * weight as int } else { 0 }));
                if other == nation {
                    if let Some(part) = part {
                        if part != key.number { assert(inventory_at(data.inventories, other as int, part as int) == inventory_at(previous.inventories, other as int, part as int)); }
                    }
                }
            }
        }
        processed = Ghost(processed@ + 1);
    }
}

pub fn add_supplier_inventory(data: &mut Data, supplier: &Key, links: &Index<i128>) -> (result: bool)
    requires old(data).structures_valid(), profile_storage_ok(old(data).profiles), links.wf(),
        forall|key: KeyView| (#[trigger] links.at(key)).is_some() ==> numeric(key),
    ensures final(data).structures_valid(), same_dimensions(*old(data), *final(data)),
        forall|key: KeyView| (#[trigger] old(data).names.at(key)).is_none() ==> final(data).names.at(key).is_none(),
        result ==> forall|nation: i64, part: Option<i64>, value: bool|
            #[trigger] inventory_measure(final(data).inventories, nation, part, value)
                == inventory_measure(old(data).inventories, nation, part, value)
                    + profile_measure_at(old(data).profiles, supplier.view(), part, value) * link_count(*links, nation),
{
    let ghost initial = *data;
    let groups = match data.profiles.get(supplier) { Some(groups) => groups, None => return true };
    let mut cursor = links.cursor();
    let mut processed = Ghost(0int);
    loop
        invariant
            initial == *old(data), data.structures_valid(), same_dimensions(initial, *data),
            forall|key: KeyView| (#[trigger] initial.names.at(key)).is_none() ==> data.names.at(key).is_none(),
            profile_storage_ok(data.profiles), profile_keys_ok(*groups),
            data.profiles.at(supplier.view()) == Some(*groups),
            links.wf(), forall|key: KeyView| (#[trigger] links.at(key)).is_some() ==> numeric(key),
            0 <= processed@ <= links.entries().len(), cursor.remaining() == links.entries().skip(processed@),
            forall|nation: i64, part: Option<i64>, value: bool|
                #[trigger] inventory_measure(data.inventories, nation, part, value)
                    == inventory_measure(initial.inventories, nation, part, value)
                        + sum(links.entries().take(processed@), |row: (KeyView, i128)|
                            if row.0 == numeric_key(nation as int) { profile_measure(*groups, part, value) * row.1 as int } else { 0 }),
        decreases cursor.remaining().len(),
    {
        let (nation, weight) = match cursor.next() {
            Some(row) => row,
            None => {
                proof {
                    assert(processed@ == links.entries().len());
                    assert(links.entries().take(processed@) =~= links.entries());
                    assert forall|nation: i64, part: Option<i64>, value: bool|
                        #[trigger] inventory_measure(data.inventories, nation, part, value)
                            == inventory_measure(initial.inventories, nation, part, value)
                                + profile_measure_at(initial.profiles, supplier.view(), part, value) * link_count(*links, nation) by {
                        let weight = |row: (KeyView, i128)| profile_measure(*groups, part, value) * row.1 as int;
                        verus_prooflib::ordered::lemma_key_contribution(links, numeric_key(nation as int), weight);
                        assert((|row: (KeyView, i128)| if row.0 == numeric_key(nation as int) { profile_measure(*groups, part, value) * row.1 as int } else { 0 })
                            =~= (|row: (KeyView, i128)| if row.0 == numeric_key(nation as int) { weight(row) } else { 0 }));
                        reveal(verus_prooflib::ordered::contribution);
                        if links.at(numeric_key(nation as int)).is_some() {} else { assert(link_count(*links, nation) == 0); }
                    }
                }
                return true;
            },
        };
        proof {
            links.model();
            assert(links.at(nation.view()) == Some(*weight));
            assert(numeric(nation.view()));
        }
        let ghost previous = *data;
        if !add_profile_inventory(data, nation.number, &*groups, *weight) { return false; }
        proof {
            assert forall|other: i64, part: Option<i64>, value: bool|
                #[trigger] inventory_measure(data.inventories, other, part, value)
                    == inventory_measure(initial.inventories, other, part, value)
                        + sum(links.entries().take(processed@ + 1), |row: (KeyView, i128)|
                            if row.0 == numeric_key(other as int) { profile_measure(*groups, part, value) * row.1 as int } else { 0 }) by {
                lemma_sum_take(links.entries(), processed@, |row: (KeyView, i128)|
                    if row.0 == numeric_key(other as int) { profile_measure(*groups, part, value) * row.1 as int } else { 0 });
                assert(inventory_measure(previous.inventories, other, part, value)
                    == inventory_measure(initial.inventories, other, part, value)
                        + sum(links.entries().take(processed@), |row: (KeyView, i128)|
                            if row.0 == numeric_key(other as int) { profile_measure(*groups, part, value) * row.1 as int } else { 0 }));
            }
        }
        processed = Ghost(processed@ + 1);
    }
}

pub fn build_inventory_rows(data: &mut Data) -> (result: bool)
    requires old(data).structures_valid(), profile_storage_ok(old(data).profiles), counts_layout(old(data).suppliers, true, true),
        forall|key: KeyView| (#[trigger] old(data).inventories.at(key)).is_none(),
        forall|key: KeyView| (#[trigger] old(data).names.at(key)).is_none(),
    ensures final(data).structures_valid(), same_dimensions(*old(data), *final(data)),
        forall|key: KeyView| (#[trigger] final(data).names.at(key)).is_none(),
        result ==> indexed_inventory_ok(*final(data)),
{
    let ghost initial = *data;
    let mut cursor = data.suppliers.cursor();
    let mut processed = Ghost(0int);
    loop
        invariant
            initial == *old(data), data.structures_valid(), same_dimensions(initial, *data),
            forall|key: KeyView| (#[trigger] data.names.at(key)).is_none(),
            profile_storage_ok(data.profiles), counts_layout(data.suppliers, true, true),
            0 <= processed@ <= data.suppliers.entries().len(), cursor.remaining() == data.suppliers.entries().skip(processed@),
            forall|nation: i64, part: Option<i64>, value: bool|
                #[trigger] inventory_measure(data.inventories, nation, part, value)
                    == sum(data.suppliers.entries().take(processed@), |row: (KeyView, Index<i128>)|
                        supplier_measure(data.profiles, row, nation, part, value)),
        decreases cursor.remaining().len(),
    {
        let (supplier, links) = match cursor.next() {
            Some(row) => row,
            None => {
                proof {
                    assert(processed@ == data.suppliers.entries().len());
                    assert(data.suppliers.entries().take(processed@) =~= data.suppliers.entries());
                }
                return true;
            },
        };
        proof {
            data.suppliers.model();
            assert(data.suppliers.at(supplier.view()) == Some(*links));
        }
        let ghost previous = *data;
        let applied = add_supplier_inventory(data, &supplier, &*links);
        proof {
            assert forall|key: KeyView| (#[trigger] data.names.at(key)).is_none() by {
                assert(previous.names.at(key).is_none());
            }
        }
        if !applied { return false; }
        proof {
            assert forall|nation: i64, part: Option<i64>, value: bool|
                #[trigger] inventory_measure(data.inventories, nation, part, value)
                    == sum(data.suppliers.entries().take(processed@ + 1), |row: (KeyView, Index<i128>)|
                        supplier_measure(data.profiles, row, nation, part, value)) by {
                lemma_sum_take(data.suppliers.entries(), processed@, |row: (KeyView, Index<i128>)|
                    supplier_measure(data.profiles, row, nation, part, value));
                assert(inventory_measure(previous.inventories, nation, part, value)
                    == sum(previous.suppliers.entries().take(processed@), |row: (KeyView, Index<i128>)|
                        supplier_measure(previous.profiles, row, nation, part, value)));
            }
        }
        processed = Ghost(processed@ + 1);
    }
}

}
