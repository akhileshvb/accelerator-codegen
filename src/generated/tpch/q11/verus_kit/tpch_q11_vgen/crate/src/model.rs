use crate::semantics::params0;
use crate::packed::{self, Packed, PackedCursor};
use std::sync::Arc;
use vstd::prelude::*;
use verus_common::sums::sum;
use verus_prooflib::ordered::{Index, Key, KeyView};
use crate::gen_spec::*;
use crate::gen_obligations::params;
use crate::inventory::*;
use crate::values::*;
use verus_prooflib::lemma_ordered_equal;

verus! {

pub type Profiles = Index<Packed>;
pub type Counts = Index<Index<i128>>;

pub enum NameView {
    Singleton { nation: i64, multiplicity: i128 },
    Merged { inventory: Inventory },
}

pub struct Data {
    pub profiles: Profiles,
    pub suppliers: Counts,
    pub suppliers_reverse: Counts,
    pub nations: Counts,
    pub nations_reverse: Counts,
    pub inventories: Index<Inventory>,
    pub names: Index<NameView>,
}

pub open spec fn numeric_key(number: int) -> KeyView { (number, Seq::<u8>::empty()) }
pub open spec fn text_key(text: Seq<u8>) -> KeyView { (0int, text) }

pub open spec fn profile_at(profiles: Profiles, supplier: int, part: int) -> Moment {
    match profiles.at(numeric_key(supplier)) {
        Some(groups) => moment_at(groups, part),
        None => Moment { count: 0, value: 0 },
    }
}

pub open spec fn count_at(counts: Counts, outer: KeyView, inner: KeyView) -> int {
    match counts.at(outer) {
        Some(groups) => match groups.at(inner) { Some(count) => count as int, None => 0int },
        None => 0,
    }
}

pub open spec fn p_count(db: Db, supplier: int, part: int) -> int {
    sum(db.partsupp, |row: PartsuppRow| if row.ps_suppkey == supplier && row.ps_partkey == part { 1int } else { 0 })
}

pub open spec fn p_value(db: Db, supplier: int, part: int) -> int {
    sum(db.partsupp, |row: PartsuppRow| if row.ps_suppkey == supplier && row.ps_partkey == part {
        row.ps_availqty * row.ps_supplycost
    } else { 0 })
}

pub open spec fn s_count(db: Db, supplier: int, nation: int) -> int {
    sum(db.supplier, |row: SupplierRow| if row.s_suppkey == supplier && row.s_nationkey == nation { 1int } else { 0 })
}

pub open spec fn n_count(db: Db, nation: int, name: Seq<u8>) -> int {
    sum(db.nation, |row: NationRow| if row.n_nationkey == nation && row.n_name == name { 1int } else { 0 })
}

pub open spec fn h_count(db: Db, nation: int, part: int) -> int {
    sum(db.partsupp, |row: PartsuppRow| if row.ps_partkey == part { s_count(db, row.ps_suppkey, nation) } else { 0 })
}

pub open spec fn h_value(db: Db, nation: int, part: int) -> int {
    sum(db.partsupp, |row: PartsuppRow| if row.ps_partkey == part {
        row.ps_availqty * row.ps_supplycost * s_count(db, row.ps_suppkey, nation)
    } else { 0 })
}

pub open spec fn h_total_count(db: Db, nation: int) -> int {
    sum(db.partsupp, |row: PartsuppRow| s_count(db, row.ps_suppkey, nation))
}

pub open spec fn h_total_value(db: Db, nation: int) -> int {
    sum(db.partsupp, |row: PartsuppRow| row.ps_availqty * row.ps_supplycost * s_count(db, row.ps_suppkey, nation))
}

pub open spec fn inventory_at(inventories: Index<Inventory>, nation: int, part: int) -> Moment {
    match inventories.at(numeric_key(nation)) {
        Some(inventory) => moment_at(inventory.groups, part),
        None => Moment { count: 0, value: 0 },
    }
}

pub open spec fn inventory_total(inventories: Index<Inventory>, nation: int) -> Moment {
    match inventories.at(numeric_key(nation)) { Some(inventory) => inventory.total, None => zero_moment() }
}

pub open spec fn same_dimensions(before: Data, after: Data) -> bool {
    &&& before.profiles == after.profiles
    &&& before.suppliers == after.suppliers
    &&& before.suppliers_reverse == after.suppliers_reverse
    &&& before.nations == after.nations
    &&& before.nations_reverse == after.nations_reverse
}

pub open spec fn positive_counts(counts: Counts) -> bool {
    counts.wf() && forall|outer: KeyView| #[trigger] counts.at(outer).is_some() ==> {
        let groups = counts.at(outer).unwrap();
        groups.wf() && groups.entries().len() > 0 && forall|inner: KeyView| #[trigger] groups.at(inner).is_some() ==> groups.at(inner).unwrap() > 0
    }
}

pub open spec fn numeric( key: KeyView) -> bool {
    key.1 == Seq::<u8>::empty() && i64::MIN <= key.0 <= i64::MAX
}

pub open spec fn profile_storage_ok(profiles: Profiles) -> bool {
    &&& profiles.wf()
    &&& forall|supplier: KeyView| (#[trigger] profiles.at(supplier)).is_some() ==> {
        let groups = profiles.at(supplier).unwrap();
        &&& numeric(supplier)
        &&& groups.wf()
        &&& groups.entries().len() > 0
        &&& forall|part: KeyView| (#[trigger] groups.at(part)).is_some() ==> numeric(part) && groups.at(part).unwrap().count > 0
    }
}

pub open spec fn profile_layout(profiles: Profiles) -> bool {
    profiles.wf() && forall|supplier: KeyView| (#[trigger] profiles.at(supplier)).is_some() ==> {
        let groups = profiles.at(supplier).unwrap();
        numeric(supplier) && groups.wf() && forall|part: KeyView| (#[trigger] groups.at(part)).is_some() ==> numeric(part)
    }
}

pub open spec fn key_class(key: KeyView, number: bool) -> bool {
    if number { numeric(key) } else { key.0 == 0 }
}

pub open spec fn counts_layout(counts: Counts, outer_number: bool, inner_number: bool) -> bool {
    &&& counts.wf()
    &&& forall|outer: KeyView| (#[trigger] counts.at(outer)).is_some() ==> {
        let groups = counts.at(outer).unwrap();
        &&& key_class(outer, outer_number)
        &&& groups.wf()
        &&& forall|inner: KeyView| (#[trigger] groups.at(inner)).is_some() ==> key_class(inner, inner_number)
    }
}

pub proof fn lemma_profile_storage_equal(before: Profiles, after: Profiles)
    requires profile_storage_ok(before), after.wf(),
        forall|key: KeyView| #[trigger] before.at(key) == #[trigger] after.at(key),
    ensures profile_storage_ok(after),
{
    assert forall|key: KeyView| (#[trigger] after.at(key)).is_some() implies {
        let groups = after.at(key).unwrap();
        numeric(key) && groups.wf() && groups.entries().len() > 0
            && forall|part: KeyView| (#[trigger] groups.at(part)).is_some() ==> numeric(part) && groups.at(part).unwrap().count > 0
    } by {
        assert(before.at(key) == after.at(key));
        assert(before.at(key).is_some());
    }
}

pub proof fn lemma_counts_storage_equal(before: Counts, after: Counts)
    requires before.wf(), after.wf(), forall|key: KeyView| #[trigger] before.at(key) == #[trigger] after.at(key),
    ensures positive_counts(before) ==> positive_counts(after),
        forall|outer_number: bool, inner_number: bool| #[trigger] counts_layout(before, outer_number, inner_number) ==> counts_layout(after, outer_number, inner_number),
{
    if positive_counts(before) {
        assert forall|key: KeyView| (#[trigger] after.at(key)).is_some() implies {
            let groups = after.at(key).unwrap();
            groups.wf() && groups.entries().len() > 0 && forall|inner: KeyView| (#[trigger] groups.at(inner)).is_some() ==> groups.at(inner).unwrap() > 0
        } by { assert(before.at(key) == after.at(key)); assert(before.at(key).is_some()); }
    }
    assert forall|outer_number: bool, inner_number: bool| #[trigger] counts_layout(before, outer_number, inner_number) implies counts_layout(after, outer_number, inner_number) by {
        assert forall|key: KeyView| (#[trigger] after.at(key)).is_some() implies {
            let groups = after.at(key).unwrap();
            key_class(key, outer_number) && groups.wf() && forall|inner: KeyView| (#[trigger] groups.at(inner)).is_some() ==> key_class(inner, inner_number)
        } by { assert(before.at(key) == after.at(key)); assert(before.at(key).is_some()); }
    }
}

pub proof fn lemma_dimension_frames(before: Data, after: Data, db: Db)
    requires before.profiles == after.profiles,
        before.suppliers == after.suppliers, before.suppliers_reverse == after.suppliers_reverse,
        before.nations == after.nations, before.nations_reverse == after.nations_reverse,
    ensures before.profiles_ok(db) ==> after.profiles_ok(db),
        before.suppliers_ok(db) ==> after.suppliers_ok(db),
        before.nations_ok(db) ==> after.nations_ok(db),
{}

impl Data {
    pub open spec fn structures_valid(self) -> bool {
        &&& self.inventories.wf()
        &&& self.names.wf()
        &&& forall|key: KeyView| (#[trigger] self.inventories.at(key)).is_some() ==> {
            &&& numeric(key)
            &&& self.inventories.at(key).unwrap().valid()
            &&& self.inventories.at(key).unwrap().total.count > 0
        }
        &&& forall|key: KeyView| (#[trigger] self.names.at(key)).is_some() ==> match self.names.at(key).unwrap() {
            NameView::Singleton { .. } => true,
            NameView::Merged { inventory } => inventory.valid(),
        }
    }

    pub open spec fn profiles_ok(self, db: Db) -> bool {
        &&& forall|supplier: i64, part: i64| #[trigger] profile_at(self.profiles, supplier as int, part as int).count as int
            == p_count(db, supplier as int, part as int)
            && profile_at(self.profiles, supplier as int, part as int).value as int == p_value(db, supplier as int, part as int)
        &&& profile_storage_ok(self.profiles)
    }

    pub open spec fn suppliers_ok(self, db: Db) -> bool {
        &&& positive_counts(self.suppliers) && positive_counts(self.suppliers_reverse)
        &&& forall|supplier: i64, nation: i64| #[trigger] count_at(self.suppliers, numeric_key(supplier as int), numeric_key(nation as int))
            == s_count(db, supplier as int, nation as int)
            && count_at(self.suppliers_reverse, numeric_key(nation as int), numeric_key(supplier as int))
                == s_count(db, supplier as int, nation as int)
        &&& counts_layout(self.suppliers, true, true)
        &&& counts_layout(self.suppliers_reverse, true, true)
    }

    pub open spec fn nations_ok(self, db: Db) -> bool {
        &&& positive_counts(self.nations) && positive_counts(self.nations_reverse)
        &&& forall|nation: i64, name: Seq<u8>| #[trigger] count_at(self.nations, numeric_key(nation as int), text_key(name))
            == n_count(db, nation as int, name)
            && count_at(self.nations_reverse, text_key(name), numeric_key(nation as int)) == n_count(db, nation as int, name)
        &&& counts_layout(self.nations, true, false)
        &&& counts_layout(self.nations_reverse, false, true)
    }

    pub open spec fn inventories_ok(self, db: Db) -> bool {
        &&& forall|nation: i64, part: i64| #[trigger] inventory_at(self.inventories, nation as int, part as int).count as int
            == h_count(db, nation as int, part as int)
            && inventory_at(self.inventories, nation as int, part as int).value as int == h_value(db, nation as int, part as int)
        &&& forall|key: KeyView| #[trigger] self.inventories.at(key).is_some() ==> {
            let inventory = self.inventories.at(key).unwrap();
            &&& numeric(key)
            &&& inventory.valid()
            &&& inventory.total.count > 0
            &&& inventory.total.count as int == h_total_count(db, key.0)
            &&& inventory.total.value as int == h_total_value(db, key.0)
        }
    }

    pub open spec fn names_ok(self, db: Db) -> bool {
        &&& forall|key: KeyView| (#[trigger] self.names.at(key)).is_some() <==> self.nations_reverse.at(key).is_some()
        &&& forall|key: KeyView| (#[trigger] self.names.at(key)).is_some() ==> {
            let support = self.nations_reverse.at(key).unwrap();
            &&& key.0 == 0
            &&& match self.names.at(key).unwrap() {
                NameView::Singleton { nation, multiplicity } => {
                    &&& support.entries().len() == 1
                    &&& support.at(numeric_key(nation as int)) == Some(multiplicity)
                    &&& multiplicity > 0
                },
                NameView::Merged { inventory } => {
                    &&& support.entries().len() >= 2
                    &&& inventory.valid()
                    &&& inventory.total.count as int == scalar_1_group_count(db, params0(key.1))
                    &&& inventory.total.value as int == scalar_1_group_sum(db, params0(key.1), |tuple: Scalar1Tuple|
                        tuple.partsupp_2.ps_supplycost * tuple.partsupp_2.ps_availqty)
                    &&& forall|part: i64| #[trigger] moment_at(inventory.groups, part as int).count as int
                        == group_count(db, params0(key.1), part as int)
                        && moment_at(inventory.groups, part as int).value as int == group_sum(db, params0(key.1), part as int,
                            |tuple: Tuple| tuple.partsupp.ps_supplycost * tuple.partsupp.ps_availqty)
                },
            }
        }
    }

    pub open spec fn valid(self, db: Db) -> bool {
        crate::semantics::db_bounded(db) && self.profiles_ok(db) && self.suppliers_ok(db) && self.nations_ok(db)
            && self.inventories_ok(db) && self.names_ok(db) && self.structures_valid()
    }

    pub fn new() -> (result: Self)
        ensures
            result.structures_valid(),
            profile_storage_ok(result.profiles),
            positive_counts(result.suppliers), positive_counts(result.suppliers_reverse),
            positive_counts(result.nations), positive_counts(result.nations_reverse),
            counts_layout(result.suppliers, true, true), counts_layout(result.suppliers_reverse, true, true),
            counts_layout(result.nations, true, false), counts_layout(result.nations_reverse, false, true),
            forall|key: KeyView| #[trigger] result.profiles.at(key).is_none(),
            forall|key: KeyView| #[trigger] result.suppliers.at(key).is_none(),
            forall|key: KeyView| #[trigger] result.suppliers_reverse.at(key).is_none(),
            forall|key: KeyView| #[trigger] result.nations.at(key).is_none(),
            forall|key: KeyView| #[trigger] result.nations_reverse.at(key).is_none(),
            forall|key: KeyView| #[trigger] result.inventories.at(key).is_none(),
            forall|key: KeyView| #[trigger] result.names.at(key).is_none(),
    {
        Data { profiles: Index::new(), suppliers: Index::new(), suppliers_reverse: Index::new(),
            nations: Index::new(), nations_reverse: Index::new(), inventories: Index::new(), names: Index::new() }
    }

    pub fn share(&self) -> (result: Self)
        requires self.structures_valid(),
        ensures result.profiles.view() == self.profiles.view(), result.suppliers.view() == self.suppliers.view(),
            result.suppliers_reverse.view() == self.suppliers_reverse.view(), result.nations.view() == self.nations.view(),
            result.nations_reverse.view() == self.nations_reverse.view(), result.inventories.view() == self.inventories.view(),
            result.names.view() == self.names.view(),
            result.structures_valid(),
            forall|db: Db| #[trigger] self.valid(db) ==> result.valid(db),
    {
        let result = Data { profiles: self.profiles.share(), suppliers: self.suppliers.share(), suppliers_reverse: self.suppliers_reverse.share(),
            nations: self.nations.share(), nations_reverse: self.nations_reverse.share(), inventories: self.inventories.share(), names: self.names.share() };
        proof {
            lemma_ordered_equal(&self.inventories, &result.inventories);
            lemma_ordered_equal(&self.names, &result.names);
            if self.profiles.wf() { lemma_ordered_equal(&self.profiles, &result.profiles); }
            if self.suppliers.wf() { lemma_ordered_equal(&self.suppliers, &result.suppliers); }
            if self.suppliers_reverse.wf() { lemma_ordered_equal(&self.suppliers_reverse, &result.suppliers_reverse); }
            if self.nations.wf() { lemma_ordered_equal(&self.nations, &result.nations); }
            if self.nations_reverse.wf() { lemma_ordered_equal(&self.nations_reverse, &result.nations_reverse); }
            assert forall|key: KeyView| (#[trigger] result.inventories.at(key)).is_some() implies
                numeric(key) && result.inventories.at(key).unwrap().valid() && result.inventories.at(key).unwrap().total.count > 0
                by { assert(self.inventories.at(key).is_some()); }
            assert forall|key: KeyView| (#[trigger] result.names.at(key)).is_some() implies
                (match result.names.at(key).unwrap() { NameView::Singleton { .. } => true, NameView::Merged { inventory } => inventory.valid() }) by {
                assert(self.names.at(key).is_some());
            }
            assert forall|db: Db| #[trigger] self.valid(db) implies result.valid(db) by {
                lemma_ordered_equal(&self.profiles, &result.profiles);
                lemma_ordered_equal(&self.suppliers, &result.suppliers);
                lemma_ordered_equal(&self.suppliers_reverse, &result.suppliers_reverse);
                lemma_ordered_equal(&self.nations, &result.nations);
                lemma_ordered_equal(&self.nations_reverse, &result.nations_reverse);
                lemma_profile_storage_equal(self.profiles, result.profiles);
                lemma_counts_storage_equal(self.suppliers, result.suppliers);
                lemma_counts_storage_equal(self.suppliers_reverse, result.suppliers_reverse);
                lemma_counts_storage_equal(self.nations, result.nations);
                lemma_counts_storage_equal(self.nations_reverse, result.nations_reverse);
                assert forall|supplier: i64, part: i64| #[trigger] profile_at(result.profiles, supplier as int, part as int)
                    == profile_at(self.profiles, supplier as int, part as int) by {}
                assert(profile_storage_ok(result.profiles));
                assert forall|supplier: i64, part: i64| #[trigger] profile_at(result.profiles, supplier as int, part as int).count as int == p_count(db, supplier as int, part as int)
                    && profile_at(result.profiles, supplier as int, part as int).value as int == p_value(db, supplier as int, part as int) by {
                    assert(profile_at(self.profiles, supplier as int, part as int).count as int == p_count(db, supplier as int, part as int));
                }
                assert(result.profiles_ok(db));
                assert(positive_counts(result.suppliers));
                assert(positive_counts(result.suppliers_reverse));
                assert(counts_layout(result.suppliers, true, true));
                assert(counts_layout(result.suppliers_reverse, true, true));
                assert forall|supplier: i64, nation: i64| #[trigger] count_at(result.suppliers, numeric_key(supplier as int), numeric_key(nation as int)) == s_count(db, supplier as int, nation as int)
                    && count_at(result.suppliers_reverse, numeric_key(nation as int), numeric_key(supplier as int)) == s_count(db, supplier as int, nation as int) by {
                    assert(count_at(self.suppliers, numeric_key(supplier as int), numeric_key(nation as int)) == s_count(db, supplier as int, nation as int));
                }
                assert(result.suppliers_ok(db));
                assert forall|nation: i64, name: Seq<u8>| #[trigger] count_at(result.nations, numeric_key(nation as int), text_key(name)) == n_count(db, nation as int, name)
                    && count_at(result.nations_reverse, text_key(name), numeric_key(nation as int)) == n_count(db, nation as int, name) by {
                    assert(count_at(self.nations, numeric_key(nation as int), text_key(name)) == n_count(db, nation as int, name));
                }
                assert(result.nations_ok(db));
                assert forall|nation: i64, part: i64| #[trigger] inventory_at(result.inventories, nation as int, part as int).count as int == h_count(db, nation as int, part as int)
                    && inventory_at(result.inventories, nation as int, part as int).value as int == h_value(db, nation as int, part as int) by {
                    assert(inventory_at(self.inventories, nation as int, part as int).count as int == h_count(db, nation as int, part as int));
                }
                assert forall|key: KeyView| (#[trigger] result.inventories.at(key)).is_some() implies {
                    let inventory = result.inventories.at(key).unwrap();
                    numeric(key) && inventory.valid() && inventory.total.count > 0
                        && inventory.total.count as int == h_total_count(db, key.0) && inventory.total.value as int == h_total_value(db, key.0)
                } by { assert(self.inventories.at(key).is_some()); }
                assert(result.inventories_ok(db));
                assert forall|key: KeyView| (#[trigger] result.names.at(key)).is_some() <==> result.nations_reverse.at(key).is_some() by {
                    assert(self.names.at(key) == result.names.at(key));
                    assert(self.nations_reverse.at(key) == result.nations_reverse.at(key));
                }
                assert forall|key: KeyView| (#[trigger] result.names.at(key)).is_some() implies {
                    let support = result.nations_reverse.at(key).unwrap();
                    key.0 == 0 && match result.names.at(key).unwrap() {
                        NameView::Singleton { nation, multiplicity } => support.entries().len() == 1
                            && support.at(numeric_key(nation as int)) == Some(multiplicity) && multiplicity > 0,
                        NameView::Merged { inventory } => support.entries().len() >= 2 && inventory.valid()
                            && inventory.total.count as int == scalar_1_group_count(db, params0(key.1))
                            && inventory.total.value as int == scalar_1_group_sum(db, params0(key.1), |tuple: Scalar1Tuple| tuple.partsupp_2.ps_supplycost * tuple.partsupp_2.ps_availqty)
                            && forall|part: i64| #[trigger] moment_at(inventory.groups, part as int).count as int == group_count(db, params0(key.1), part as int)
                                && moment_at(inventory.groups, part as int).value as int == group_sum(db, params0(key.1), part as int, |tuple: Tuple| tuple.partsupp.ps_supplycost * tuple.partsupp.ps_availqty),
                    }
                } by {
                    assert(self.names.at(key) == result.names.at(key));
                    assert(self.names.at(key).is_some());
                    assert(self.nations_reverse.at(key) == result.nations_reverse.at(key));
                }
                assert(result.names_ok(db));
            }
        }
        result
    }
}

pub fn add_profile(profiles: &mut Profiles, supplier: i64, part: i64, change: Moment, signed: bool, shift: u32) -> (result: bool)
    requires shift < 64,
    ensures !signed && profile_storage_ok(*old(profiles)) ==> profile_storage_ok(*final(profiles)),
        profile_layout(*old(profiles)) ==> profile_layout(*final(profiles)),
        result ==> profile_at(*final(profiles), supplier as int, part as int).count as int
            == profile_at(*old(profiles), supplier as int, part as int).count as int + change.count as int,
        result ==> profile_at(*final(profiles), supplier as int, part as int).value as int
            == profile_at(*old(profiles), supplier as int, part as int).value as int + change.value as int,
        result ==> forall|other_supplier: int, other_part: int|
            other_supplier != supplier || other_part != part ==>
                #[trigger] profile_at(*final(profiles), other_supplier, other_part)
                    == profile_at(*old(profiles), other_supplier, other_part),
{
    let ghost original = *profiles;
    let outer = Key::number(supplier);
    let inner = Key::number(part);
    let mut groups = match profiles.get(&outer) {
        Some(source) => {
            let copy = source.share();
            copy
        },
        None => Packed::new(shift),
    };
    proof {
        assert forall|other_part: int| #[trigger] moment_at(groups, other_part)
            == profile_at(original, supplier as int, other_part) by {}
    }
    let ghost initial_groups = groups;
    let previous = match groups.get(part) { Some(moment) => moment, None => zero() };
    let next = match plus(previous, change) { Some(moment) => moment, None => return false };
    if !packed::fits_exec(&next) { return false; }
    if !signed && (next.count < 0 || (next.count == 0 && next.value != 0)) { return false; }
    if previous.count == next.count && previous.value == next.value {
        proof {
            assert(moment_at(groups, part as int) == next);
            assert forall|other_part: int| other_part != part implies
                #[trigger] moment_at(groups, other_part) == moment_at(initial_groups, other_part) by {}
        }
        return true;
    }
    if next.count == 0 && next.value == 0 { groups.remove(part); }
    else { groups.put(part, next); }
    proof {
        assert(moment_at(groups, part as int) == next);
        assert forall|other_part: int| other_part != part implies
            #[trigger] moment_at(groups, other_part) == moment_at(initial_groups, other_part) by {}
    }
    if groups.is_empty() { profiles.remove(&outer); }
    else { profiles.put(outer, Arc::new(groups)); }
    proof {
        if groups.entries().len() == 0 {
            groups.model();
            assert forall|other_part: int| #[trigger] moment_at(groups, other_part) == zero_moment() by {}
        }
        assert forall|other_part: int| #[trigger] profile_at(*profiles, supplier as int, other_part)
            == moment_at(groups, other_part) by {
            if groups.entries().len() == 0 { assert(moment_at(groups, other_part) == zero_moment()); }
        }
        assert forall|other_supplier: int, other_part: int|
            other_supplier != supplier || other_part != part implies
                #[trigger] profile_at(*profiles, other_supplier, other_part)
                    == profile_at(original, other_supplier, other_part) by {
            if other_supplier != supplier { assert(profiles.at(numeric_key(other_supplier)) == original.at(numeric_key(other_supplier))); }
            else { assert(moment_at(groups, other_part) == moment_at(initial_groups, other_part)); }
        }
        if !signed && profile_storage_ok(original) {
            assert forall|supplier_key: KeyView| (#[trigger] profiles.at(supplier_key)).is_some() implies {
                let stored = profiles.at(supplier_key).unwrap();
                &&& numeric(supplier_key)
                &&& stored.wf()
                &&& stored.entries().len() > 0
                &&& forall|part_key: KeyView| (#[trigger] stored.at(part_key)).is_some() ==> numeric(part_key) && stored.at(part_key).unwrap().count > 0
            } by {
                if supplier_key == numeric_key(supplier as int) {
                    assert(profiles.at(supplier_key) == Some(groups));
                    assert forall|part_key: KeyView| (#[trigger] groups.at(part_key)).is_some() implies numeric(part_key) && groups.at(part_key).unwrap().count > 0 by {
                        if part_key != numeric_key(part as int) {
                            assert(groups.at(part_key) == initial_groups.at(part_key));
                            assert(original.at(supplier_key).is_some());
                            assert(initial_groups.at(part_key) == original.at(supplier_key).unwrap().at(part_key));
                        }
                    }
                } else { assert(profiles.at(supplier_key) == original.at(supplier_key)); }
            }
        }
        if profile_layout(original) {
            assert forall|supplier_key: KeyView| (#[trigger] profiles.at(supplier_key)).is_some() implies {
                let stored = profiles.at(supplier_key).unwrap();
                numeric(supplier_key) && stored.wf() && forall|part_key: KeyView| (#[trigger] stored.at(part_key)).is_some() ==> numeric(part_key)
            } by {
                if supplier_key == numeric_key(supplier as int) {
                    assert forall|part_key: KeyView| (#[trigger] groups.at(part_key)).is_some() implies numeric(part_key) by {
                        if part_key != numeric_key(part as int) {
                            assert(initial_groups.at(part_key).is_some());
                            assert(original.at(supplier_key).is_some());
                            assert(initial_groups.at(part_key) == original.at(supplier_key).unwrap().at(part_key));
                            assert(numeric(part_key));
                        }
                    }
                } else { assert(profiles.at(supplier_key) == original.at(supplier_key)); }
            }
        }
    }
    true
}

pub fn add_count(counts: &mut Counts, outer: &Key, inner: &Key, change: i128, signed: bool) -> (result: bool)
    ensures !signed && positive_counts(*old(counts)) ==> positive_counts(*final(counts)),
        forall|key: KeyView| key != outer.view() ==> (#[trigger] final(counts).at(key)) == old(counts).at(key),
        forall|outer_number: bool, inner_number: bool| #[trigger] counts_layout(*old(counts), outer_number, inner_number)
            && key_class(outer.view(), outer_number) && key_class(inner.view(), inner_number)
            ==> counts_layout(*final(counts), outer_number, inner_number),
        result ==> count_at(*final(counts), outer.view(), inner.view())
        == count_at(*old(counts), outer.view(), inner.view()) + change as int,
        result ==> forall|other_outer: KeyView, other_inner: KeyView|
            other_outer != outer.view() || other_inner != inner.view() ==>
                #[trigger] count_at(*final(counts), other_outer, other_inner)
                    == count_at(*old(counts), other_outer, other_inner),
{
    let ghost original = *counts;
    let mut groups = match counts.get(outer) {
        Some(source) => {
            let copy = source.share();
            proof { lemma_ordered_equal(&*source, &copy); }
            copy
        },
        None => Index::new(),
    };
    let ghost initial_groups = groups;
    proof {
        assert forall|other_inner: KeyView| #[trigger] count_at(original, outer.view(), other_inner)
            == match groups.at(other_inner) { Some(count) => count as int, None => 0int } by {}
    }
    let previous = match groups.get(inner) { Some(count) => *count, None => 0 };
    let next = match crate::values::add(previous, change) { Some(count) => count, None => return false };
    if !signed && next < 0 { return false; }
    if next == 0 { groups.remove(inner); }
    else { groups.put(inner.share(), Arc::new(next)); }
    if groups.is_empty() { counts.remove(outer); }
    else { counts.put(outer.share(), Arc::new(groups)); }
    proof {
        if groups.entries().len() == 0 {
            groups.model();
            assert forall|key: KeyView| #[trigger] groups.at(key).is_none() by {}
        }
        assert forall|other_inner: KeyView| #[trigger] count_at(*counts, outer.view(), other_inner)
            == match groups.at(other_inner) { Some(count) => count as int, None => 0int } by {}
        assert(count_at(*counts, outer.view(), inner.view()) == next as int);
        assert forall|other_outer: KeyView, other_inner: KeyView|
            other_outer != outer.view() || other_inner != inner.view() implies
                #[trigger] count_at(*counts, other_outer, other_inner)
                    == count_at(original, other_outer, other_inner) by {
            if other_outer != outer.view() { assert(counts.at(other_outer) == original.at(other_outer)); }
            else { assert(groups.at(other_inner) == initial_groups.at(other_inner)); }
        }
        if !signed && positive_counts(original) {
            assert forall|stored_outer: KeyView| (#[trigger] counts.at(stored_outer)).is_some() implies {
                let stored = counts.at(stored_outer).unwrap();
                &&& stored.wf()
                &&& stored.entries().len() > 0
                &&& forall|stored_inner: KeyView| (#[trigger] stored.at(stored_inner)).is_some() ==> stored.at(stored_inner).unwrap() > 0
            } by {
                if stored_outer == outer.view() {
                    assert(counts.at(stored_outer) == Some(groups));
                    assert forall|stored_inner: KeyView| (#[trigger] groups.at(stored_inner)).is_some() implies groups.at(stored_inner).unwrap() > 0 by {
                        if stored_inner != inner.view() {
                            assert(groups.at(stored_inner) == initial_groups.at(stored_inner));
                            assert(original.at(outer.view()).is_some());
                            assert(initial_groups.at(stored_inner) == original.at(outer.view()).unwrap().at(stored_inner));
                        }
                    }
                } else { assert(counts.at(stored_outer) == original.at(stored_outer)); }
            }
        }
        assert forall|outer_number: bool, inner_number: bool| #[trigger] counts_layout(original, outer_number, inner_number)
            && key_class(outer.view(), outer_number) && key_class(inner.view(), inner_number) implies counts_layout(*counts, outer_number, inner_number) by {
            assert forall|stored_outer: KeyView| (#[trigger] counts.at(stored_outer)).is_some() implies {
                let stored = counts.at(stored_outer).unwrap();
                &&& key_class(stored_outer, outer_number)
                &&& stored.wf()
                &&& forall|stored_inner: KeyView| (#[trigger] stored.at(stored_inner)).is_some() ==> key_class(stored_inner, inner_number)
            } by {
                if stored_outer == outer.view() {
                    assert(counts.at(stored_outer) == Some(groups));
                    assert forall|stored_inner: KeyView| (#[trigger] groups.at(stored_inner)).is_some() implies key_class(stored_inner, inner_number) by {
                        if stored_inner != inner.view() {
                            assert(groups.at(stored_inner) == initial_groups.at(stored_inner));
                            assert(original.at(outer.view()).is_some());
                            assert(initial_groups.at(stored_inner) == original.at(outer.view()).unwrap().at(stored_inner));
                        }
                    }
                } else { assert(counts.at(stored_outer) == original.at(stored_outer)); }
            }
        }
    }
    true
}

}
