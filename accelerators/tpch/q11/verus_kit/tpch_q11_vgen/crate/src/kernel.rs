use crate::packed::{self, Packed, PackedCursor};
use vstd::prelude::*;
use verus_prooflib::wide::Wide;
use verus_common::sums::{sum, lemma_sum_take, lemma_sum_empty};
use verus_prooflib::ordered::{Key, Index, KeyView};
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::inventory::*;
use crate::maintenance::*;
use crate::model::*;
use crate::values::*;
use crate::semantics::*;
use verus_prooflib::lemma_i64_product;

verus! {

pub struct State {
    data: Data,
    database: Ghost<Db>,
}

impl State {
    pub closed spec fn db(self) -> Db { self.database@ }

    #[verifier::type_invariant]
    spec fn valid(self) -> bool { self.data.valid(self.db()) }
}

pub open spec fn history(s: State) -> nat { 0 }

pub fn build(
    partsupp_profiles: &[PartsuppInput],
    supplier_links: &[SupplierInput],
    nation_names: Vec<NationInput>,
    Ghost(db): Ghost<Db>,
) -> (result: Option<State>)
    requires
        scan_partsupp_profiles_ok(db, partsupp_profiles@),
        scan_supplier_links_ok(db, supplier_links@),
        scan_nation_names_ok(db, nation_names@),
    ensures result matches Some(state) ==> state.db() == db,
        result is Some,
{
    proof { lemma_build_ranges(db, partsupp_profiles@, supplier_links@, nation_names@); }
    let mut data = Data::new();
    let mut index: usize = 0;
    let mut processed = Ghost(0int);
    proof {
        assert forall|supplier: i64, part: i64| #[trigger] profile_at(data.profiles, supplier as int, part as int) == zero_moment() by {
            assert(data.profiles.at(numeric_key(supplier as int)).is_none());
        }
        assert(partsupp_profiles@.take(0) =~= Seq::<PartsuppInput>::empty());
    }
    while index < partsupp_profiles.len()
        invariant
            index <= partsupp_profiles.len(), processed@ == index as int,
            data.structures_valid(),
            profile_storage_ok(data.profiles),
            nonzero_counts(data.suppliers), nonzero_counts(data.suppliers_reverse),
            nonzero_counts(data.nations), nonzero_counts(data.nations_reverse),
            counts_layout(data.suppliers, true, true), counts_layout(data.suppliers_reverse, true, true),
            counts_layout(data.nations, true, false), counts_layout(data.nations_reverse, false, true),
            forall|key: verus_prooflib::ordered::KeyView| #[trigger] data.suppliers.at(key).is_none(),
            forall|key: verus_prooflib::ordered::KeyView| #[trigger] data.suppliers_reverse.at(key).is_none(),
            forall|key: verus_prooflib::ordered::KeyView| #[trigger] data.nations.at(key).is_none(),
            forall|key: verus_prooflib::ordered::KeyView| #[trigger] data.nations_reverse.at(key).is_none(),
            forall|supplier: i64, part: i64| #[trigger] profile_at(data.profiles, supplier as int, part as int).count as int
                == sum(partsupp_profiles@.take(processed@), |row: PartsuppInput|
                    if row.ps_suppkey == supplier && row.ps_partkey == part { 1int } else { 0 }),
            forall|supplier: i64, part: i64| #[trigger] profile_at(data.profiles, supplier as int, part as int).value as int
                == sum(partsupp_profiles@.take(processed@), |row: PartsuppInput|
                    if row.ps_suppkey == supplier && row.ps_partkey == part { row.ps_availqty as int * row.ps_supplycost as int } else { 0 }),
        decreases partsupp_profiles.len() - index,
    {
        let row = partsupp_profiles[index];
        let ghost before = data.profiles;
        let value = Wide::from_i128(row.ps_availqty as i128).mul(&Wide::from_i128(row.ps_supplycost as i128));
        add_profile(&mut data.profiles, row.ps_suppkey, row.ps_partkey, &Mom { count: Wide::from_i128(1), value }, 63);
        proof {
            assert forall|supplier: i64, part: i64| #[trigger] profile_at(data.profiles, supplier as int, part as int).count as int
                == sum(partsupp_profiles@.take(index as int + 1), |other: PartsuppInput|
                    if other.ps_suppkey == supplier && other.ps_partkey == part { 1int } else { 0 }) by {
                lemma_sum_take(partsupp_profiles@, index as int, |other: PartsuppInput|
                    if other.ps_suppkey == supplier && other.ps_partkey == part { 1int } else { 0 });
                assert(profile_at(before, supplier as int, part as int).count as int
                    == sum(partsupp_profiles@.take(index as int), |other: PartsuppInput|
                        if other.ps_suppkey == supplier && other.ps_partkey == part { 1int } else { 0 }));
            }
            assert forall|supplier: i64, part: i64| #[trigger] profile_at(data.profiles, supplier as int, part as int).value as int
                == sum(partsupp_profiles@.take(index as int + 1), |other: PartsuppInput|
                    if other.ps_suppkey == supplier && other.ps_partkey == part { other.ps_availqty as int * other.ps_supplycost as int } else { 0 }) by {
                lemma_sum_take(partsupp_profiles@, index as int, |other: PartsuppInput|
                    if other.ps_suppkey == supplier && other.ps_partkey == part { other.ps_availqty as int * other.ps_supplycost as int } else { 0 });
                assert(profile_at(before, supplier as int, part as int).value as int
                    == sum(partsupp_profiles@.take(index as int), |other: PartsuppInput|
                        if other.ps_suppkey == supplier && other.ps_partkey == part { other.ps_availqty as int * other.ps_supplycost as int } else { 0 }));
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    proof {
        assert(partsupp_profiles@.take(processed@) =~= partsupp_profiles@);
        lemma_profile_scan(data, db, partsupp_profiles@);
    }
    index = 0;
    processed = Ghost(0int);
    proof {
        assert(supplier_links@.take(0) =~= Seq::<SupplierInput>::empty());
        assert forall|supplier: i64, nation: i64| #[trigger] count_at(data.suppliers, numeric_key(supplier as int), numeric_key(nation as int)) == 0 by {
            assert(data.suppliers.at(numeric_key(supplier as int)).is_none());
        }
        assert forall|supplier: i64, nation: i64| #[trigger] count_at(data.suppliers_reverse, numeric_key(nation as int), numeric_key(supplier as int)) == 0 by {
            assert(data.suppliers_reverse.at(numeric_key(nation as int)).is_none());
        }
    }
    while index < supplier_links.len()
        invariant
            index <= supplier_links.len(), processed@ == index as int,
            data.structures_valid(),
            data.profiles_ok(db),
            nonzero_counts(data.suppliers), nonzero_counts(data.suppliers_reverse),
            nonzero_counts(data.nations), nonzero_counts(data.nations_reverse),
            counts_layout(data.suppliers, true, true), counts_layout(data.suppliers_reverse, true, true),
            counts_layout(data.nations, true, false), counts_layout(data.nations_reverse, false, true),
            forall|key: verus_prooflib::ordered::KeyView| #[trigger] data.nations.at(key).is_none(),
            forall|key: verus_prooflib::ordered::KeyView| #[trigger] data.nations_reverse.at(key).is_none(),
            forall|key: verus_prooflib::ordered::KeyView| #[trigger] data.nations.at(key).is_none(),
            forall|key: verus_prooflib::ordered::KeyView| #[trigger] data.nations_reverse.at(key).is_none(),
            forall|supplier: i64, nation: i64| #[trigger] count_at(data.suppliers, numeric_key(supplier as int), numeric_key(nation as int))
                == sum(supplier_links@.take(processed@), |row: SupplierInput|
                    if row.s_suppkey == supplier && row.s_nationkey == nation { 1int } else { 0 }),
            forall|supplier: i64, nation: i64| #[trigger] count_at(data.suppliers_reverse, numeric_key(nation as int), numeric_key(supplier as int))
                == count_at(data.suppliers, numeric_key(supplier as int), numeric_key(nation as int)),
        decreases supplier_links.len() - index,
    {
        let row = supplier_links[index];
        let supplier = Key::number(row.s_suppkey);
        let nation = Key::number(row.s_nationkey);
        add_count(&mut data.suppliers, &supplier, &nation, &Wide::from_i128(1));
        add_count(&mut data.suppliers_reverse, &nation, &supplier, &Wide::from_i128(1));
        proof {
            assert forall|supplier: i64, nation: i64| #[trigger] count_at(data.suppliers, numeric_key(supplier as int), numeric_key(nation as int))
                == sum(supplier_links@.take(index as int + 1), |other: SupplierInput|
                    if other.s_suppkey == supplier && other.s_nationkey == nation { 1int } else { 0 }) by {
                lemma_sum_take(supplier_links@, index as int, |other: SupplierInput|
                    if other.s_suppkey == supplier && other.s_nationkey == nation { 1int } else { 0 });
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    index = 0;
    processed = Ghost(0int);
    proof {
        assert(supplier_links@.take(supplier_links.len() as int) =~= supplier_links@);
        lemma_supplier_scan(data, db, supplier_links@);
        assert(nation_names@.take(0) =~= Seq::<NationInput>::empty());
        assert forall|nation: i64, name: Seq<u8>| #[trigger] count_at(data.nations, numeric_key(nation as int), text_key(name)) == 0 by {
            assert(data.nations.at(numeric_key(nation as int)).is_none());
        }
        assert forall|nation: i64, name: Seq<u8>| #[trigger] count_at(data.nations_reverse, text_key(name), numeric_key(nation as int)) == 0 by {
            assert(data.nations_reverse.at(text_key(name)).is_none());
        }
    }
    while index < nation_names.len()
        invariant
            index <= nation_names.len(), processed@ == index as int,
            data.structures_valid(),
            data.profiles_ok(db),
            data.suppliers_ok(db),
            nonzero_counts(data.nations), nonzero_counts(data.nations_reverse),
            counts_layout(data.nations, true, false), counts_layout(data.nations_reverse, false, true),
            forall|nation: i64, name: Seq<u8>| #[trigger] count_at(data.nations, numeric_key(nation as int), text_key(name))
                == sum(nation_names@.take(processed@), |row: NationInput|
                    if row.n_nationkey == nation && row.n_name@ == name { 1int } else { 0 }),
            forall|nation: i64, name: Seq<u8>| #[trigger] count_at(data.nations_reverse, text_key(name), numeric_key(nation as int))
                == count_at(data.nations, numeric_key(nation as int), text_key(name)),
        decreases nation_names.len() - index,
    {
        let row = &nation_names[index];
        let nation = Key::number(row.n_nationkey);
        let name = Key::text(copy_bytes(row.n_name.as_slice()));
        add_count(&mut data.nations, &nation, &name, &Wide::from_i128(1));
        add_count(&mut data.nations_reverse, &name, &nation, &Wide::from_i128(1));
        proof {
            assert forall|nation: i64, name: Seq<u8>| #[trigger] count_at(data.nations, numeric_key(nation as int), text_key(name))
                == sum(nation_names@.take(index as int + 1), |other: NationInput|
                    if other.n_nationkey == nation && other.n_name@ == name { 1int } else { 0 }) by {
                lemma_sum_take(nation_names@, index as int, |other: NationInput|
                    if other.n_nationkey == nation && other.n_name@ == name { 1int } else { 0 });
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    proof {
        assert(nation_names@.take(processed@) =~= nation_names@);
        lemma_nation_scan(data, db, nation_names@);
    }
    build_inventories(&mut data);
    proof {
        assert(data.profiles_ok(db));
        assert(data.suppliers_ok(db));
        assert(data.nations_ok(db));
        crate::construction::lemma_indexed_inventories(data, db);
        crate::names::lemma_indexed_names(data, db);
    }
    Some(State { data, database: Ghost(db) })
}

fn convert_rows(exact: Vec<(i64, Wide)>, Ghost(db): Ghost<Db>, Ghost(wanted): Ghost<Params>) -> (result: Option<Vec<(i64, i128)>>)
    requires query_result(db, wanted, exact_view(exact_rows(exact@))),
    ensures result matches Some(rows) ==> query_result(db, wanted, out_view(rows@)),
        result is None ==> !answer_fits(db, wanted),
{
    let mut rows: Vec<(i64, i128)> = Vec::new();
    let mut index: usize = 0;
    while index < exact.len()
        invariant index <= exact.len(), rows.len() == index, query_result(db, wanted, exact_view(exact_rows(exact@))),
            forall|position: int| 0 <= position < index ==> rows@[position].0 == exact@[position].0 && rows@[position].1 as int == exact@[position].1@,
        decreases exact.len() - index,
    {
        let converted = exact[index].1.to_i128();
        match converted {
            Some(value) => rows.push((exact[index].0, value)),
            None => {
                proof {
                    let model = exact_view(exact_rows(exact@));
                    assert(model.len() == exact@.len());
                    assert(model[index as int].value == exact@[index as int].1@);
                    if answer_fits(db, wanted) {
                        assert(query_result(db, wanted, model));
                        assert(forall|i: int| 0 <= i < model.len() ==> out_fits(#[trigger] model[i]));
                        assert(out_fits(model[index as int]));
                    }
                }
                return None;
            },
        }
        index += 1;
    }
    proof { assert(out_view(rows@) =~= exact_view(exact_rows(exact@))); }
    Some(rows)
}

pub fn execute(state: &State, p1: &[u8], p2: i64) -> (result: Option<Vec<(i64, i128)>>)
    ensures result matches Some(rows) ==> query_result(state.db(), params(p1@, p2 as int), out_view(rows@)),
        envelope(state.db()) && answer_fits(state.db(), params(p1@, p2 as int)) ==> result is Some,
{
    proof { use_type_invariant(state); }
    let name = Key::text(copy_bytes(p1));
    let view = match state.data.names.get(&name) {
        Some(view) => view,
        None => {
            proof { lemma_missing_name(state.data, state.db(), p1@); lemma_empty_answer(state.db(), p1@, p2 as int); }
            return Some(Vec::new());
        },
    };
    match &*view {
        NameView::Singleton { nation, multiplicity } => {
            proof {
                lemma_singleton_total(state.data, state.db(), p1@, *nation, *multiplicity);
            }
            let inventory = match state.data.inventories.get(&Key::number(*nation)) {
                Some(inventory) => inventory,
                None => {
                    proof {
                        lemma_absent_inventory(state.data, state.db(), *nation);
                        assert(scalar_1_group_count(state.db(), params0(p1@)) == 0);
                        lemma_empty_answer(state.db(), p1@, p2 as int);
                    }
                    return Some(Vec::new());
                },
            };
            proof {
                state.data.inventories.model();
                assert(state.data.inventories.at(numeric_key(*nation as int)) == Some(*inventory));
                lemma_stored_nonneg(state.data, state.db(), *nation);
                assert(state.data.names_ok(state.db()));
            }
            let exact = emit(&*inventory, &*multiplicity, p2);
            proof {
                assert forall|part: i64| #[trigger] group_count(state.db(), params0(p1@), part as int)
                    == multiplicity@ * moment_at(inventory.groups, part as int).count as int by {
                    lemma_singleton_group(state.data, state.db(), p1@, *nation, *multiplicity, part as int);
                    assert(inventory_at(state.data.inventories, *nation as int, part as int).count as int == h_count(state.db(), *nation as int, part as int));
                }
                assert forall|part: i64| #[trigger] query_group_value(state.db(), p1@, part as int)
                    == multiplicity@ * moment_at(inventory.groups, part as int).value as int by {
                    lemma_singleton_group(state.data, state.db(), p1@, *nation, *multiplicity, part as int);
                    assert(inventory_at(state.data.inventories, *nation as int, part as int).count as int == h_count(state.db(), *nation as int, part as int));
                }
                lemma_execute_model(state.db(), p1@, p2 as int, &*inventory, multiplicity@, exact_rows(exact@));
            }
            convert_rows(exact, Ghost(state.db()), Ghost(params(p1@, p2 as int)))
        },
        NameView::Merged { inventory } => {
            proof {
                state.data.names.model();
                assert(state.data.names.at(name.view()) == Some(*view));
                lemma_merged_nonneg(state.data, state.db(), p1@, *inventory);
            }
            let exact = emit(inventory, &Wide::from_i128(1), p2);
            proof {
                assert forall|part: i64| #[trigger] group_count(state.db(), params0(p1@), part as int)
                    == moment_at(inventory.groups, part as int).count as int by {}
                assert forall|part: i64| #[trigger] query_group_value(state.db(), p1@, part as int)
                    == moment_at(inventory.groups, part as int).value as int by {
                    assert(group_count(state.db(), params0(p1@), part as int) == moment_at(inventory.groups, part as int).count as int);
                }
                lemma_execute_model(state.db(), p1@, p2 as int, inventory, 1int, exact_rows(exact@));
            }
            convert_rows(exact, Ghost(state.db()), Ghost(params(p1@, p2 as int)))
        },
    }
}

pub fn apply(
    state: &State,
    nation_delta: &Vec<NationDelta>,
    partsupp_delta: &[PartsuppDelta],
    supplier_delta: &[SupplierDelta],
    Ghost(db2): Ghost<Db>,
) -> (result: Result<State, u32>)
    requires capture_ok(state.db(), db2, nation_delta_seq_view(nation_delta@),
        partsupp_delta_seq_view(partsupp_delta@), supplier_delta_seq_view(supplier_delta@)),
    ensures result matches Ok(next) ==> next.db() == db2,
        result is Ok,
{
    proof {
        use_type_invariant(state);
        lemma_apply_ranges(state.db(), db2, partsupp_delta@, supplier_delta@, nation_delta@);
    }
    let profiles = coalesce_partsupp(partsupp_delta);
    let suppliers = coalesce_supplier(supplier_delta);
    let nations = coalesce_nation(nation_delta);
    let mut data = state.data.share();
    let ghost initial = data;
    let mut inventory_changes = Index::new();
    stage_partsupp(&mut data, &profiles, &mut inventory_changes);
    proof {
        assert(initial.valid(state.db()));
        crate::names::lemma_database_names(initial, state.db());
        assert forall|supplier: i64, part: i64| #[trigger] profile_at(data.profiles, supplier as int, part as int).count as int == p_count(db2, supplier as int, part as int)
            && profile_at(data.profiles, supplier as int, part as int).value as int == p_value(db2, supplier as int, part as int) by {
            lemma_partsupp_capture(state.db(), db2, partsupp_delta@, supplier, part);
            assert(profile_cell(data.profiles, supplier, part, false) == profile_cell(initial.profiles, supplier, part, false) + profile_cell(profiles, supplier, part, false));
            assert(profile_cell(data.profiles, supplier, part, true) == profile_cell(initial.profiles, supplier, part, true) + profile_cell(profiles, supplier, part, true));
            assert(profile_at(initial.profiles, supplier as int, part as int).count as int == p_count(state.db(), supplier as int, part as int));
            assert(profile_at(profiles, supplier as int, part as int).count as int == p_delta_count(partsupp_delta@, supplier as int, part as int));
        }
        assert(data.profiles_ok(db2));
    }
    let ghost after_profiles = data;
    let ghost profile_inventory_changes = inventory_changes;
    stage_supplier(&mut data, &suppliers, &mut inventory_changes);
    proof {
        assert(data.profiles_ok(db2));
        lemma_supplier_commit(after_profiles, data, state.db(), db2, suppliers, supplier_delta@);
    }
    let ghost before_nations = data;
    proof {
        crate::name_updates::lemma_names_frame(initial, data);
        assert(crate::names::indexed_names_ok(data));
        assert forall|nation: i64, name: Seq<u8>| #[trigger] count_at(data.nations, numeric_key(nation as int), text_key(name))
            == count_at(data.nations_reverse, text_key(name), numeric_key(nation as int)) by {
            assert(count_at(initial.nations, numeric_key(nation as int), text_key(name)) == n_count(state.db(), nation as int, name));
        }
    }
    apply_inventory_changes(&mut data, &inventory_changes);
    proof {
        lemma_dimension_frames(before_nations, data, db2);
        assert(data.suppliers == before_nations.suppliers);
        assert(data.suppliers_reverse == before_nations.suppliers_reverse);
        assert(data.nations == before_nations.nations);
        assert(data.nations_reverse == before_nations.nations_reverse);
        assert(data.suppliers_ok(db2));
        assert forall|nation: i64, part: i64, value: bool| #[trigger] profile_cell(inventory_changes, nation, part, value)
            == sum(profiles.entries(), |row: (KeyView, Packed)| partsupp_effect(row, initial.suppliers, nation, part, value))
                + sum(suppliers.entries(), |row: (KeyView, Index<Wide>)| supplier_effect(row, data.profiles, nation, part, value)) by {
            assert(profile_cell(profile_inventory_changes, nation, part, value)
                == sum(profiles.entries(), |row: (KeyView, Packed)| partsupp_effect(row, initial.suppliers, nation, part, value)));
            assert(profile_cell(inventory_changes, nation, part, value) == profile_cell(profile_inventory_changes, nation, part, value)
                + sum(suppliers.entries(), |row: (KeyView, Index<Wide>)| supplier_effect(row, data.profiles, nation, part, value)));
        }
        crate::join_proofs::lemma_inventory_commit(initial, data, state.db(), db2, profiles, suppliers, inventory_changes);
    }
    let ghost before_nation_update = data;
    apply_nation(&mut data, &nations);
    proof {
        assert(data.profiles == before_nation_update.profiles);
        assert(data.profiles == after_profiles.profiles);
        assert(data.profiles_ok(db2));
        assert(data.suppliers_ok(db2));
        lemma_nation_commit(before_nation_update, data, state.db(), db2, nations, nation_delta@);
        assert(data.inventories_ok(db2));
        crate::names::lemma_indexed_names(data, db2);
    }
    Ok(State { data, database: Ghost(db2) })
}

}
