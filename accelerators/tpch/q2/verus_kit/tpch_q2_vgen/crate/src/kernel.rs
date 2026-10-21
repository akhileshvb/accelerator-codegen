use vstd::prelude::*;
use std::sync::Arc;
use verus_common::sums::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::bags::*;
use crate::model::*;
use crate::storage::*;
use crate::maintenance::*;
use crate::semantics::*;
use crate::parts::*;
use crate::parts_exec::*;
use crate::flat::*;
use crate::output::{Output, enumerate};
pub use crate::model::State;

verus! {

/// Dictionary ids use the whole usize range, so nothing in the state is capped by past changes.
pub open spec fn history(s: State) -> nat { 0 }

#[verifier::rlimit(20)]
pub fn build(
    brass_parts: Vec<BrassPartScan>, brass_partsupp: &[BrassPartsuppScan],
    suppliers: Vec<SupplierScan>, nations: Vec<NationScan>, regions: Vec<RegionScan>,
    Ghost(db): Ghost<Db>,
) -> (result: Option<State>)
    requires
        scan_brass_parts_ok(db, brass_parts@), scan_brass_partsupp_ok(db, brass_partsupp@),
        scan_suppliers_ok(db, suppliers@), scan_nations_ok(db, nations@), scan_regions_ok(db, regions@),
    ensures result matches Some(state) ==> state.db() == db, envelope(db) ==> result is Some,
{
    let parts = build_parts(brass_parts.as_slice());
    proof {
        lemma_scan_representable(db, brass_parts@, brass_partsupp@, suppliers@, nations@, regions@);
        lemma_build_parts_meaning(db, brass_parts@, parts);
        assert forall|index: int| 0 <= index < db.part.len() implies fits_i64((#[trigger] db.part[index]).p_size) by {
            assert(retained(db.part[index]));
        }
    }
    let mut quotes = Directory::new();
    let mut supplier_index = Directory::new();
    let mut nation_index = Directory::new();
    let mut region_index = Directory::new();
    let mut index = 0;
    let mut processed: Ghost<int> = Ghost(0);
    index = 0;
    processed = Ghost(0);
    while index < brass_partsupp.len()
        invariant
            index <= brass_partsupp.len(), processed@ == index as int, quotes.valid(),
            scan_brass_partsupp_ok(db, brass_partsupp@),
            parts.meaning(db), supported_db(db),
            forall|index: int| 0 <= index < db.part.len() ==> fits_i64((#[trigger] db.part[index]).p_size),
            index_positive(quotes),
            index_nonempty(quotes),
            index_routed(quotes, quote_routing(db)),
            forall|probe: i64| #[trigger] mass_at(quotes, probe) <= index as int,
            forall|key: i64, supplier: int, cost: int| #[trigger] quote_amount(quotes, key, supplier, cost) ==
                sum(brass_partsupp@.take(processed@), |row: BrassPartsuppScan| if row.ps_partkey == key && row.ps_suppkey as int == supplier && row.ps_supplycost as int == cost { 1 } else { 0 }),
        decreases brass_partsupp.len() - index,
    {
        let row = brass_partsupp[index];
        let cells = parts.tp.get((row.ps_partkey, 0, 0));
        proof {
            lemma_scan_row_eligible(db, brass_partsupp@, index as int);
            lemma_eligible_cells(db, parts, row.ps_partkey);
            assert(brass_partsupp@[index as int] == row);
        }
        if cells.len() == 0 { return None; }
        let ghost previous = quotes;
        let adjusted = adjust_bag(&mut quotes, row.ps_partkey, Arc::new(row), 1);
        proof { assert(adjusted) by { if !adjusted { assert(mass_at(previous, row.ps_partkey) + 1 > i128_max()); } } }
        if !adjusted { return None; }
        proof {
            assert forall|key: i64, supplier: int, cost: int| #[trigger] quote_amount(quotes, key, supplier, cost) ==
                sum(brass_partsupp@.take(index as int + 1), |row: BrassPartsuppScan| if row.ps_partkey == key && row.ps_suppkey as int == supplier && row.ps_supplycost as int == cost { 1int } else { 0int }) by {
                let weight = |value: PartsuppRow| if value.ps_suppkey == supplier && value.ps_supplycost == cost { 1int } else { 0int };
                assert(index_weight(quotes, key, weight) == index_weight(previous, key, weight) +
                    if key == row.ps_partkey { weight(brass_partsupp_scan_view(row)) } else { 0int });
                assert(quote_amount(previous, key, supplier, cost) == index_weight(previous, key, weight));
                assert(quote_amount(previous, key, supplier, cost) == sum(brass_partsupp@.take(index as int), |row: BrassPartsuppScan| if row.ps_partkey == key && row.ps_suppkey as int == supplier && row.ps_supplycost as int == cost { 1int } else { 0int }));
                lemma_sum_take(brass_partsupp@, index as int, |row: BrassPartsuppScan| if row.ps_partkey == key && row.ps_suppkey as int == supplier && row.ps_supplycost as int == cost { 1int } else { 0int });
                assert(quote_amount(quotes, key, supplier, cost) == index_weight(quotes, key, weight));
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    index = 0;
    processed = Ghost(0);
    while index < suppliers.len()
        invariant
            index <= suppliers.len(), processed@ == index as int, supplier_index.valid(),
            index_positive(supplier_index), index_routed(supplier_index, |key: i64, row: SupplierRow| row.s_suppkey == key as int),
            forall|probe: i64| #[trigger] mass_at(supplier_index, probe) <= index as int,
            forall|key: i64, row: SupplierRow| #[trigger] supplier_amount(supplier_index, key, row) ==
                sum(suppliers@.take(processed@), |value: SupplierScan| if value.s_suppkey == key && supplier_scan_view(value) == row { 1 } else { 0 }),
        decreases suppliers.len() - index,
    {
        let ghost previous = supplier_index;
        let adjusted = adjust_bag(&mut supplier_index, suppliers[index].s_suppkey, Arc::new(suppliers[index].dup()), 1);
        proof { assert(adjusted) by { if !adjusted { assert(mass_at(previous, suppliers@[index as int].s_suppkey) + 1 > i128_max()); } } }
        if !adjusted { return None; }
        proof {
            assert forall|key: i64, row: SupplierRow| #[trigger] supplier_amount(supplier_index, key, row) ==
                sum(suppliers@.take(index as int + 1), |value: SupplierScan| if value.s_suppkey == key && supplier_scan_view(value) == row { 1int } else { 0int }) by {
                lemma_sum_take(suppliers@, index as int, |value: SupplierScan| if value.s_suppkey == key && supplier_scan_view(value) == row { 1int } else { 0int });
                assert(index_amount(supplier_index, key, row) == index_amount(previous, key, row) +
                    if key == suppliers@[index as int].s_suppkey && supplier_scan_view(suppliers@[index as int]) == row { 1int } else { 0int });
                assert(supplier_amount(previous, key, row) == sum(suppliers@.take(index as int), |value: SupplierScan| if value.s_suppkey == key && supplier_scan_view(value) == row { 1int } else { 0int }));
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    index = 0;
    processed = Ghost(0);
    while index < nations.len()
        invariant
            index <= nations.len(), processed@ == index as int, nation_index.valid(),
            index_positive(nation_index), index_routed(nation_index, |key: i64, row: NationRow| row.n_nationkey == key as int),
            forall|probe: i64| #[trigger] mass_at(nation_index, probe) <= index as int,
            forall|key: i64, row: NationRow| #[trigger] nation_amount(nation_index, key, row) ==
                sum(nations@.take(processed@), |value: NationScan| if value.n_nationkey == key && nation_scan_view(value) == row { 1 } else { 0 }),
        decreases nations.len() - index,
    {
        let ghost previous = nation_index;
        let adjusted = adjust_bag(&mut nation_index, nations[index].n_nationkey, Arc::new(nations[index].dup()), 1);
        proof { assert(adjusted) by { if !adjusted { assert(mass_at(previous, nations@[index as int].n_nationkey) + 1 > i128_max()); } } }
        if !adjusted { return None; }
        proof {
            assert forall|key: i64, row: NationRow| #[trigger] nation_amount(nation_index, key, row) ==
                sum(nations@.take(index as int + 1), |value: NationScan| if value.n_nationkey == key && nation_scan_view(value) == row { 1int } else { 0int }) by {
                lemma_sum_take(nations@, index as int, |value: NationScan| if value.n_nationkey == key && nation_scan_view(value) == row { 1int } else { 0int });
                assert(index_amount(nation_index, key, row) == index_amount(previous, key, row) +
                    if key == nations@[index as int].n_nationkey && nation_scan_view(nations@[index as int]) == row { 1int } else { 0int });
                assert(nation_amount(previous, key, row) == sum(nations@.take(index as int), |value: NationScan| if value.n_nationkey == key && nation_scan_view(value) == row { 1int } else { 0int }));
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    index = 0;
    processed = Ghost(0);
    while index < regions.len()
        invariant
            index <= regions.len(), processed@ == index as int, regions_valid(region_index),
            regions_positive(region_index),
            forall|name: Seq<u8>, key: i64| #[trigger] region_amount(region_index, name, key) ==
                sum(regions@.take(processed@), |row: RegionScan| if row.r_name@ == name && row.r_regionkey == key { 1 } else { 0 }),
        decreases regions.len() - index,
    {
        let ghost previous_regions = region_index;
        proof {
            lemma_indicator_sum_le(regions@.take(processed@),
                |row: RegionScan| if row.r_name@ == regions@[index as int].r_name@ && row.r_regionkey == regions@[index as int].r_regionkey { 1int } else { 0int });
        }
        let adjusted = adjust_region(&mut region_index, &regions[index].r_name, regions[index].r_regionkey, 1);
        proof { assert(adjusted) by { if !adjusted { assert(magnitude(region_amount(previous_regions, regions@[index as int].r_name@, regions@[index as int].r_regionkey)) + 1 > i128_max()); } } }
        if !adjusted { return None; }
        proof {
            assert forall|name: Seq<u8>, key: i64| #[trigger] region_amount(region_index, name, key) ==
                sum(regions@.take(index as int + 1), |row: RegionScan| if row.r_name@ == name && row.r_regionkey == key { 1int } else { 0int }) by {
                lemma_sum_take(regions@, index as int, |row: RegionScan| if row.r_name@ == name && row.r_regionkey == key { 1int } else { 0int });
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    proof {
        assert forall|key: i64, supplier: int, cost: int| #[trigger] quote_amount(quotes, key, supplier, cost) ==
            if eligible(db, key) > 0 { sum(db.partsupp, |row: PartsuppRow| if row.ps_partkey == key as int && row.ps_suppkey == supplier && row.ps_supplycost == cost { 1int } else { 0int }) } else { 0int } by {
            assert(brass_partsupp@.take(brass_partsupp@.len() as int) =~= brass_partsupp@);
            lemma_quote_scan_count(db, brass_partsupp@, key, supplier, cost);
        }
        assert forall|key: i64, row: SupplierRow| #[trigger] supplier_amount(supplier_index, key, row) ==
            sum(db.supplier, |value: SupplierRow| if value == row && value.s_suppkey == key as int { 1int } else { 0int }) by {
            assert(suppliers@.take(suppliers@.len() as int) =~= suppliers@);
            let weight = |value: SupplierRow| if value == row && value.s_suppkey == key as int { 1int } else { 0int };
            lemma_sum_map(suppliers@, |value: SupplierScan| supplier_scan_view(value), weight,
                |value: SupplierScan| if value.s_suppkey == key && supplier_scan_view(value) == row { 1int } else { 0int });
            assert(sum(supplier_scan_seq_view(suppliers@), weight) == sum(db.supplier, weight));
        }
        assert forall|key: i64, row: NationRow| #[trigger] nation_amount(nation_index, key, row) ==
            sum(db.nation, |value: NationRow| if value == row && value.n_nationkey == key as int { 1int } else { 0int }) by {
            assert(nations@.take(nations@.len() as int) =~= nations@);
            let weight = |value: NationRow| if value == row && value.n_nationkey == key as int { 1int } else { 0int };
            lemma_sum_map(nations@, |value: NationScan| nation_scan_view(value), weight,
                |value: NationScan| if value.n_nationkey == key && nation_scan_view(value) == row { 1int } else { 0int });
            assert(sum(nation_scan_seq_view(nations@), weight) == sum(db.nation, weight));
        }
        assert forall|name: Seq<u8>, key: i64| #[trigger] region_amount(region_index, name, key) ==
            sum(db.region, |row: RegionRow| if row.r_name == name && row.r_regionkey == key as int { 1int } else { 0int }) by {
            assert(regions@.take(regions@.len() as int) =~= regions@);
            let weight = |value: RegionRow| if value.r_name == name && value.r_regionkey == key as int { 1int } else { 0int };
            lemma_sum_map(regions@, |value: RegionScan| region_scan_view(value), weight,
                |value: RegionScan| if value.r_name@ == name && value.r_regionkey == key { 1int } else { 0int });
            assert(sum(region_scan_seq_view(regions@), weight) == sum(db.region, weight));
        }
    }
    let sflat = Arc::new(build_flat(&supplier_index));
    proof {
        assert(index_positive(quotes));
        assert(index_positive(supplier_index));
        assert(index_positive(nation_index));
        assert(regions_valid(region_index));
        assert(regions_positive(region_index));
        assert forall|key: i64| match #[trigger] quotes.lookup(key) {
            None => true,
            Some(bag) => (*bag).valid() && eligible(db, key) > 0 &&
                forall|index: int| 0 <= index < (*bag).entries@.len() ==> (*((#[trigger] (*bag).entries@[index]).row)).ps_partkey == key,
        } by {
            if quotes.lookup(key) is Some {
                let bag = quotes.lookup(key)->0;
                assert((*bag).entries@.len() > 0);
                assert(index_routed(quotes, quote_routing(db)));
                assert(eligible(db, key) > 0) by { assert(quote_routing(db)(key, (*(*bag).entries@[0].row).rv())); }
                assert forall|index: int| 0 <= index < (*bag).entries@.len() implies
                    (*((#[trigger] (*bag).entries@[index]).row)).ps_partkey == key by {
                    assert(brass_partsupp_scan_view(*(*bag).entries@[index].row).ps_partkey == key as int);
                }
            }
        }
        assert forall|key: i64| match #[trigger] supplier_index.lookup(key) {
            None => true,
            Some(bag) => (*bag).valid() && forall|index: int| 0 <= index < (*bag).entries@.len() ==>
                (*((#[trigger] (*bag).entries@[index]).row)).s_suppkey == key,
        } by {
            if supplier_index.lookup(key) is Some {
                let bag = supplier_index.lookup(key)->0;
                assert forall|index: int| 0 <= index < (*bag).entries@.len() implies
                    (*((#[trigger] (*bag).entries@[index]).row)).s_suppkey == key by {
                    assert(supplier_scan_view(*(*bag).entries@[index].row).s_suppkey == key as int);
                }
            }
        }
        assert forall|key: i64| match #[trigger] nation_index.lookup(key) {
            None => true,
            Some(bag) => (*bag).valid() && forall|index: int| 0 <= index < (*bag).entries@.len() ==>
                (*((#[trigger] (*bag).entries@[index]).row)).n_nationkey == key,
        } by {
            if nation_index.lookup(key) is Some {
                let bag = nation_index.lookup(key)->0;
                assert forall|index: int| 0 <= index < (*bag).entries@.len() implies
                    (*((#[trigger] (*bag).entries@[index]).row)).n_nationkey == key by {
                    assert(nation_scan_view(*(*bag).entries@[index].row).n_nationkey == key as int);
                }
            }
        }
        lemma_dimension_database_counts(db, supplier_index, nation_index);
        lemma_state_indexes_meaning(db, parts, quotes, supplier_index, nation_index, region_index, sflat);
    }
    let state = State::new(parts, quotes, supplier_index, nation_index, region_index, sflat, Ghost(db));
    proof { state.database_witness(); }
    Some(state)
}

pub fn execute(s: &State, p1: i64, p2: &[u8], p3: &[u8]) -> (result: Option<Vec<Output>>)
    ensures result matches Some(out) ==> query_result(s.db(), params(p1 as int, p2@, p3@), out_view(out@)),
        envelope(s.db()) && answer_fits(s.db(), params(p1 as int, p2@, p3@)) ==> result is Some,
{ Some(enumerate(s, p1, p2, p3)) }

pub open spec fn rescan_brass_parts_after_wanted(s: State, nation_delta: Seq<NationDelta>, part_delta: Seq<PartKeyDelta>,
    partsupp_delta: Seq<PartsuppDelta>, region_delta: Seq<RegionDelta>, supplier_delta: Seq<SupplierDelta>) -> Seq<i64> {
    wanted_part_keys(part_delta)
}

pub open spec fn rescan_brass_partsupp_after_wanted(s: State, nation_delta: Seq<NationDelta>, part_delta: Seq<PartKeyDelta>,
    partsupp_delta: Seq<PartsuppDelta>, region_delta: Seq<RegionDelta>, supplier_delta: Seq<SupplierDelta>) -> Seq<i64> {
    wanted_part_keys(part_delta)
}

pub fn rescan_brass_parts_after_keys(
    s: &State, nation_delta: &Vec<NationDelta>, part_delta: &[PartKeyDelta],
    partsupp_delta: &[PartsuppDelta], region_delta: &Vec<RegionDelta>, supplier_delta: &Vec<SupplierDelta>,
) -> (result: Vec<i64>)
    ensures result@ == rescan_brass_parts_after_wanted(*s, nation_delta@, part_delta@, partsupp_delta@, region_delta@, supplier_delta@),
{ part_keys(part_delta) }

pub fn rescan_brass_partsupp_after_keys(
    s: &State, nation_delta: &Vec<NationDelta>, part_delta: &[PartKeyDelta],
    partsupp_delta: &[PartsuppDelta], region_delta: &Vec<RegionDelta>, supplier_delta: &Vec<SupplierDelta>,
) -> (result: Vec<i64>)
    ensures result@ == rescan_brass_partsupp_after_wanted(*s, nation_delta@, part_delta@, partsupp_delta@, region_delta@, supplier_delta@),
{ part_keys(part_delta) }

/// The region counts of the next database; None only when counts outgrow the i128 range.
fn apply_regions(s: &State, region_delta: &Vec<RegionDelta>, Ghost(db2): Ghost<Db>) -> (result: Option<Directory<RegionBucket>>)
    requires s.meaning(), s.db() == s.database@,
        bag_step(s.db().region, db2.region, region_delta_seq_view(region_delta@)),
        unit_weights(region_delta_seq_view(region_delta@)),
    ensures
        result matches Some(regions) ==> regions_valid(regions) && regions_nonzero(regions) && forall|name: Seq<u8>, key: i64|
            #[trigger] region_amount(regions, name, key) == sum(db2.region, |row: RegionRow| if row.r_name == name && row.r_regionkey == key as int { 1int } else { 0int }),
        result is None ==> !envelope(s.db()),
{
    proof {
        use_type_invariant(s);
        lemma_region_signs_unit(region_delta@);
    }
    let mut regions = s.regions.share();
    let mut index = 0;
    let mut processed: Ghost<int> = Ghost(0);
    while index < region_delta.len()
        invariant
            index <= region_delta.len(), processed@ == index as int, regions_valid(regions),
            regions_nonzero(regions), s.meaning(), s.db() == s.database@,
            forall|i: int| 0 <= i < region_delta@.len() ==> region_delta@[i].sign == 1 || region_delta@[i].sign == -1,
            forall|name: Seq<u8>, key: i64| #[trigger] region_amount(regions, name, key) == region_amount(s.regions, name, key) +
                region_prefix(region_delta@, processed@, name, key),
        decreases region_delta.len() - index,
    {
        let delta = &region_delta[index];
        let ghost previous_regions = regions;
        let region_count = region_delta.len();
        proof { assert(region_count as int == region_delta@.len()); }
        if !adjust_region(&mut regions, &delta.row.r_name, delta.row.r_regionkey, delta.sign as i128) {
            proof {
                assert(delta.sign == region_delta@[index as int].sign);
                lemma_region_step_fits(*s, previous_regions, region_delta@, processed@, delta.row.r_name@, delta.row.r_regionkey);
            }
            return None;
        }
        proof {
            assert forall|name: Seq<u8>, key: i64| #[trigger] region_amount(regions, name, key) == region_amount(s.regions, name, key) +
                region_prefix(region_delta@, index as int + 1, name, key) by {
                lemma_region_prefix_step(region_delta@, index as int, name, key);
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    proof {
        assert(region_delta@.take(processed@) =~= region_delta@);
        assert forall|name: Seq<u8>, key: i64| #[trigger] region_amount(regions, name, key) ==
            sum(db2.region, |row: RegionRow| if row.r_name == name && row.r_regionkey == key as int { 1int } else { 0int }) by {
            let weight = |row: RegionRow| if row.r_name == name && row.r_regionkey == key as int { 1int } else { 0int };
            let signed = |change: (RegionRow, int)| change.1 * weight(change.0);
            let project = |delta: RegionDelta| (region_scan_view(delta.row), delta.sign as int);
            lemma_sum_ext_all(region_delta@, |delta: RegionDelta| region_delta_weight(delta, name, key),
                |delta: RegionDelta| if delta.row.r_name@ == name && delta.row.r_regionkey == key { delta.sign as int } else { 0int });
            assert(region_prefix(region_delta@, processed@, name, key) == sum(region_delta@, |delta: RegionDelta| region_delta_weight(delta, name, key)));
            lemma_sum_map(region_delta@, project, signed,
                |delta: RegionDelta| if delta.row.r_name@ == name && delta.row.r_regionkey == key { delta.sign as int } else { 0int });
            assert(sum(db2.region, weight) == sum(s.db().region, weight) + sum(region_delta_seq_view(region_delta@), signed));
        }
    }
    index = 0;
    processed = Ghost(0);
    while index < region_delta.len()
        invariant index <= region_delta.len(), processed@ == index as int, regions_valid(regions),
            regions_nonzero(regions),
            forall|name: Seq<u8>, key: i64| #[trigger] region_amount(regions, name, key) ==
                sum(db2.region, |row: RegionRow| if row.r_name == name && row.r_regionkey == key as int { 1int } else { 0int }),
        decreases region_delta.len() - index,
    {
        let delta = &region_delta[index];
        if let Some(slice) = region_slice(&regions, &delta.row.r_name) {
            if let Some(count) = slice.get(delta.row.r_regionkey) {
                proof {
                    let weight = |row: RegionRow| if row.r_name == delta.row.r_name@ && row.r_regionkey == delta.row.r_regionkey as int { 1int } else { 0int };
                    lemma_sum_nonneg(db2.region, weight);
                    assert(count_at(slice, delta.row.r_regionkey) == region_amount(regions, delta.row.r_name@, delta.row.r_regionkey));
                    assert(count.value >= 0);
                }
                if count.value < 0 { return None; }
            }
        }
        compact_region(&mut regions, hash_bytes(&delta.row.r_name));
        index += 1;
        processed = Ghost(index as int);
    }
    Some(regions)
}

/// The supplier bags and their flat index after the deltas; None only when counts outgrow the i128 range.
fn apply_suppliers(s: &State, supplier_delta: &Vec<SupplierDelta>, Ghost(db2): Ghost<Db>) -> (result: Option<(Directory<Bag<SupplierScan>>, Arc<Vec<SInfo>>)>)
    requires s.meaning(), s.db() == s.database@,
        bag_step(s.db().supplier, db2.supplier, supplier_delta_seq_view(supplier_delta@)),
        unit_weights(supplier_delta_seq_view(supplier_delta@)),
    ensures
        result matches Some((suppliers, sflat)) ==> suppliers.valid() && index_positive(suppliers) && index_routed(suppliers, supplier_routing())
            && flat_ok((*sflat)@, suppliers)
            && forall|key: i64, row: SupplierRow| #[trigger] supplier_amount(suppliers, key, row) ==
                sum(db2.supplier, |value: SupplierRow| if value == row && value.s_suppkey == key as int { 1int } else { 0int }),
        result is None ==> !envelope(s.db()),
{
    proof {
        use_type_invariant(s);
        lemma_supplier_signs_unit(supplier_delta@);
    }
    let mut suppliers = s.suppliers.share();
    let mut supplier_changes = Directory::new();
    let mut index = 0;
    let mut processed: Ghost<int> = Ghost(0);
    while index < supplier_delta.len()
        invariant
            index <= supplier_delta.len(), processed@ == index as int, supplier_changes.valid(),
            index_unique(supplier_changes), index_nonempty(supplier_changes), index_nonzero(supplier_changes),
            forall|probe: i64| #[trigger] mass_at(supplier_changes, probe) <= index as int,
            forall|i: int| 0 <= i < supplier_delta@.len() ==> supplier_delta@[i].sign == 1 || supplier_delta@[i].sign == -1,
            index_routed(supplier_changes, supplier_routing()),
            forall|key: i64, row: SupplierRow| #[trigger] supplier_amount(supplier_changes, key, row) ==
                sum(supplier_delta@.take(processed@), |delta: SupplierDelta| if delta.row.s_suppkey == key && supplier_scan_view(delta.row) == row { delta.sign as int } else { 0 }),
        decreases supplier_delta.len() - index,
    {
        let delta = &supplier_delta[index];
        let ghost previous = supplier_changes;
        let adjusted = adjust_bag(&mut supplier_changes, delta.row.s_suppkey, Arc::new(delta.row.dup()), delta.sign as i128);
        proof {
            assert(adjusted) by {
                if !adjusted {
                    assert(delta.sign == supplier_delta@[index as int].sign);
                    assert(magnitude(delta.sign as int) == 1);
                    assert(mass_at(previous, delta.row.s_suppkey) + magnitude(delta.sign as int) > i128_max());
                }
            }
        }
        if !adjusted { return None; }
        proof {
            assert forall|key: i64, row: SupplierRow| #[trigger] supplier_amount(supplier_changes, key, row) ==
                sum(supplier_delta@.take(index as int + 1), |value: SupplierDelta| if value.row.s_suppkey == key && supplier_scan_view(value.row) == row { value.sign as int } else { 0int }) by {
                assert(index_amount(supplier_changes, key, row) == index_amount(previous, key, row) + if key == delta.row.s_suppkey && supplier_scan_view(delta.row) == row { delta.sign as int } else { 0int });
                assert(supplier_amount(previous, key, row) == sum(supplier_delta@.take(index as int), |value: SupplierDelta| if value.row.s_suppkey == key && supplier_scan_view(value.row) == row { value.sign as int } else { 0int }));
                lemma_sum_take(supplier_delta@, index as int, |value: SupplierDelta| if value.row.s_suppkey == key && supplier_scan_view(value.row) == row { value.sign as int } else { 0int });
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    proof { assert(supplier_delta@.take(processed@) =~= supplier_delta@); }
    proof { assert(index_routed(suppliers, supplier_routing())); }
    let supplier_count = supplier_delta.len();
    proof { assert(supplier_count as int == supplier_delta@.len()); }
    if !finish_bags(&mut suppliers, &supplier_changes) {
        proof {
            lemma_meaning_bags_positive(*s);
            lemma_positive_facts(s.suppliers);
            lemma_supplier_merge_fits(*s, supplier_changes, supplier_delta@);
        }
        return None;
    }
    let sflat = Arc::new(patch_flat(&*s.sflat, &s.suppliers, &suppliers, &supplier_changes));
    proof {
        assert(index_routed(suppliers, supplier_routing()));
        lemma_supplier_capture(*s, db2, supplier_delta@, supplier_changes, suppliers);
    }
    proof {
        lemma_meaning_bags_positive(*s);
        lemma_positive_facts(s.suppliers);
        assert forall|key: i64, row: SupplierRow| #[trigger] index_amount(suppliers, key, row) >= 0 by {
            assert(index_amount(suppliers, key, row) == supplier_amount(suppliers, key, row));
            lemma_sum_nonneg(db2.supplier, |value: SupplierRow| if value == row && value.s_suppkey == key as int { 1int } else { 0int });
        }
        lemma_positive_from_amounts(suppliers);
    }
    Some((suppliers, sflat))
}

/// The nation bags after the deltas; None only when counts outgrow the i128 range.
fn apply_nations(s: &State, nation_delta: &Vec<NationDelta>, Ghost(db2): Ghost<Db>) -> (result: Option<Directory<Bag<NationScan>>>)
    requires s.meaning(), s.db() == s.database@,
        bag_step(s.db().nation, db2.nation, nation_delta_seq_view(nation_delta@)),
        unit_weights(nation_delta_seq_view(nation_delta@)),
    ensures
        result matches Some(nations) ==> nations.valid() && index_positive(nations) && index_routed(nations, nation_routing())
            && forall|key: i64, row: NationRow| #[trigger] nation_amount(nations, key, row) ==
                sum(db2.nation, |value: NationRow| if value == row && value.n_nationkey == key as int { 1int } else { 0int }),
        result is None ==> !envelope(s.db()),
{
    proof {
        use_type_invariant(s);
        lemma_nation_signs_unit(nation_delta@);
    }
    let mut nations = s.nations.share();
    let mut nation_changes = Directory::new();
    let mut index = 0;
    let mut processed: Ghost<int> = Ghost(0);
    while index < nation_delta.len()
        invariant
            index <= nation_delta.len(), processed@ == index as int, nation_changes.valid(),
            index_unique(nation_changes), index_nonempty(nation_changes), index_nonzero(nation_changes),
            forall|probe: i64| #[trigger] mass_at(nation_changes, probe) <= index as int,
            forall|i: int| 0 <= i < nation_delta@.len() ==> nation_delta@[i].sign == 1 || nation_delta@[i].sign == -1,
            index_routed(nation_changes, nation_routing()),
            forall|key: i64, row: NationRow| #[trigger] nation_amount(nation_changes, key, row) ==
                sum(nation_delta@.take(processed@), |delta: NationDelta| if delta.row.n_nationkey == key && nation_scan_view(delta.row) == row { delta.sign as int } else { 0 }),
        decreases nation_delta.len() - index,
    {
        let delta = &nation_delta[index];
        let ghost previous = nation_changes;
        let adjusted = adjust_bag(&mut nation_changes, delta.row.n_nationkey, Arc::new(delta.row.dup()), delta.sign as i128);
        proof {
            assert(adjusted) by {
                if !adjusted {
                    assert(delta.sign == nation_delta@[index as int].sign);
                    assert(magnitude(delta.sign as int) == 1);
                    assert(mass_at(previous, delta.row.n_nationkey) + magnitude(delta.sign as int) > i128_max());
                }
            }
        }
        if !adjusted { return None; }
        proof {
            assert forall|key: i64, row: NationRow| #[trigger] nation_amount(nation_changes, key, row) ==
                sum(nation_delta@.take(index as int + 1), |value: NationDelta| if value.row.n_nationkey == key && nation_scan_view(value.row) == row { value.sign as int } else { 0int }) by {
                assert(index_amount(nation_changes, key, row) == index_amount(previous, key, row) + if key == delta.row.n_nationkey && nation_scan_view(delta.row) == row { delta.sign as int } else { 0int });
                assert(nation_amount(previous, key, row) == sum(nation_delta@.take(index as int), |value: NationDelta| if value.row.n_nationkey == key && nation_scan_view(value.row) == row { value.sign as int } else { 0int }));
                lemma_sum_take(nation_delta@, index as int, |value: NationDelta| if value.row.n_nationkey == key && nation_scan_view(value.row) == row { value.sign as int } else { 0int });
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    proof { assert(nation_delta@.take(processed@) =~= nation_delta@); }
    proof { assert(index_routed(nations, nation_routing())); }
    let nation_count = nation_delta.len();
    proof { assert(nation_count as int == nation_delta@.len()); }
    if !finish_bags(&mut nations, &nation_changes) {
        proof {
            lemma_meaning_bags_positive(*s);
            lemma_positive_facts(s.nations);
            lemma_nation_merge_fits(*s, nation_changes, nation_delta@);
        }
        return None;
    }
    proof {
        assert(index_routed(nations, nation_routing()));
        lemma_nation_capture(*s, db2, nation_delta@, nation_changes, nations);
    }
    proof {
        lemma_meaning_bags_positive(*s);
        lemma_positive_facts(s.nations);
        assert forall|key: i64, row: NationRow| #[trigger] index_amount(nations, key, row) >= 0 by {
            assert(index_amount(nations, key, row) == nation_amount(nations, key, row));
            lemma_sum_nonneg(db2.nation, |value: NationRow| if value == row && value.n_nationkey == key as int { 1int } else { 0int });
        }
        lemma_positive_from_amounts(nations);
    }
    Some(nations)
}

pub fn apply(
    s: &State, nation_delta: &Vec<NationDelta>, part_delta: &[PartKeyDelta],
    partsupp_delta: &[PartsuppDelta], region_delta: &Vec<RegionDelta>, supplier_delta: &Vec<SupplierDelta>,
    brass_parts_after_keys: &[i64], brass_parts_after_rows: &Vec<BrassPartScan>,
    brass_partsupp_after_keys: &[i64], brass_partsupp_after_rows: &[BrassPartsuppScan],
    Ghost(part_full): Ghost<Seq<(PartRow, int)>>, Ghost(db2): Ghost<Db>,
) -> (result: Result<State, u32>)
    requires
        part_delta_ok(part_delta@, part_full),
        capture_ok(s.db(), db2, nation_delta_seq_view(nation_delta@), part_full,
            partsupp_delta_seq_view(partsupp_delta@), region_delta_seq_view(region_delta@), supplier_delta_seq_view(supplier_delta@)),
        brass_parts_after_keys@ == rescan_brass_parts_after_wanted(*s, nation_delta@, part_delta@, partsupp_delta@, region_delta@, supplier_delta@),
        rescan_brass_parts_after_ok(db2, brass_parts_after_keys@, brass_parts_after_rows@),
        brass_partsupp_after_keys@ == rescan_brass_partsupp_after_wanted(*s, nation_delta@, part_delta@, partsupp_delta@, region_delta@, supplier_delta@),
        rescan_brass_partsupp_after_ok(db2, brass_partsupp_after_keys@, brass_partsupp_after_rows@),
    ensures result matches Ok(after) ==> after.db() == db2,
        envelope(s.db()) && envelope(db2) ==> result is Ok,
{
    proof { use_type_invariant(s); s.database_witness(); }
    let touched_keys = part_keys(part_delta);
    proof {
        assert forall|key: i64| #[trigger] touched_keys@.contains(key) implies
            brass_parts_after_keys@.contains(key) && brass_partsupp_after_keys@.contains(key) by {
            assert(touched_keys@ == brass_parts_after_keys@);
        }
    }
    proof {
        lemma_meaning_positive(s.db(), s.parts);
        lemma_partsupp_signs_unit(partsupp_delta@);
        lemma_supplier_signs_unit(supplier_delta@);
        lemma_nation_signs_unit(nation_delta@);
        lemma_region_signs_unit(region_delta@);
    }
    let parts = apply_parts(&s.parts, touched_keys.as_slice(), brass_parts_after_rows.as_slice());
    proof {
        lemma_parts_transfer(*s, db2, part_delta@, part_full, touched_keys@,
            brass_parts_after_keys@, brass_parts_after_rows@, parts);
        assert forall|key: i64| !touched_keys@.contains(key) implies #[trigger] eligible(db2, key) == eligible(s.db(), key) by {
            lemma_eligible_untouched(s.db(), db2, part_delta@, part_full, touched_keys@, brass_parts_after_keys@, brass_parts_after_rows@, key);
        }
        lemma_apply_representable(*s, db2, touched_keys@, part_delta@, part_full, partsupp_delta@,
            supplier_delta@, nation_delta@, region_delta@, brass_parts_after_keys@, brass_parts_after_rows@,
            brass_partsupp_after_keys@, brass_partsupp_after_rows@);
        assert forall|k: int| 0 <= k < db2.part.len() implies fits_i64((#[trigger] db2.part[k]).p_size) by {
            assert(retained(db2.part[k]));
        }
    }
    let mut touched: Directory<Count> = Directory::new();
    let mut quotes = s.quotes.share();
    let mut suppliers = s.suppliers.share();
    let mut nations = s.nations.share();
    let mut index = 0;
    let mut processed: Ghost<int> = Ghost(0);
    while index < touched_keys.len()
        invariant
            index <= touched_keys.len(), processed@ == index as int, touched.valid(),
            quotes.valid(),
            index_positive(quotes), index_unique(quotes), index_nonempty(quotes),
            index_routed(quotes, quote_routing(s.db())),
            forall|key: i64| (#[trigger] touched.lookup(key) is Some) == touched_keys@.take(processed@).contains(key),
            forall|key: i64| #[trigger] touched_keys@.take(processed@).contains(key) ==> quotes.lookup(key) is None,
            forall|key: i64, supplier: int, cost: int| #[trigger] quote_amount(quotes, key, supplier, cost) ==
                if touched_keys@.take(processed@).contains(key) { 0 } else { quote_amount(s.quotes, key, supplier, cost) },
        decreases touched_keys.len() - index,
    {
        let key = touched_keys[index];
        let ghost previous_touched = touched;
        let ghost previous_quotes = quotes;
        touched.replace(key, Some(Arc::new(Count { value: 1 })));
        quotes.replace(key, None);
        proof {
            assert(touched_keys@.take(index as int + 1) =~= touched_keys@.take(index as int).push(key));
            assert forall|probe: i64| (#[trigger] touched.lookup(probe) is Some) == touched_keys@.take(index as int + 1).contains(probe) by {
                broadcast use vstd::seq_lib::group_seq_properties;
                assert((previous_touched.lookup(probe) is Some) == touched_keys@.take(index as int).contains(probe));
            }
            assert forall|probe: i64| #[trigger] touched_keys@.take(index as int + 1).contains(probe) implies quotes.lookup(probe) is None by {
                broadcast use vstd::seq_lib::group_seq_properties;
                if probe != key { assert(touched_keys@.take(index as int).contains(probe)); }
            }
            assert forall|probe: i64, supplier: int, cost: int| #[trigger] quote_amount(quotes, probe, supplier, cost) ==
                if touched_keys@.take(index as int + 1).contains(probe) { 0 } else { quote_amount(s.quotes, probe, supplier, cost) } by {
                broadcast use vstd::seq_lib::group_seq_properties;
                assert(quote_amount(previous_quotes, probe, supplier, cost) == if touched_keys@.take(index as int).contains(probe) { 0 } else { quote_amount(s.quotes, probe, supplier, cost) });
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    proof {
        assert(touched_keys@.take(processed@) =~= touched_keys@);
        assert forall|probe: i64| touched_keys@.contains(probe) implies mass_at(quotes, probe) <= 0 by {
            assert(quotes.lookup(probe) is None);
        }
    }
    index = 0;
    processed = Ghost(0);
    while index < brass_partsupp_after_rows.len()
        invariant
            index <= brass_partsupp_after_rows.len(), processed@ == index as int,
            forall|probe: i64| touched_keys@.contains(probe) ==> #[trigger] mass_at(quotes, probe) <= index as int,
            rescan_brass_partsupp_after_ok(db2, brass_partsupp_after_keys@, brass_partsupp_after_rows@),
            touched_keys@ == brass_partsupp_after_keys@,
            touched.valid(), quotes.valid(), parts.meaning(db2), supported_db(db2),
            forall|k: int| 0 <= k < db2.part.len() ==> fits_i64((#[trigger] db2.part[k]).p_size),
            index_positive(quotes), index_unique(quotes), index_nonempty(quotes),
            index_routed(quotes, routing_after(touched_keys@, s.db(), db2)),
            forall|key: i64| (#[trigger] touched.lookup(key) is Some) == touched_keys@.contains(key),
            forall|key: i64, supplier: int, cost: int| #[trigger] quote_amount(quotes, key, supplier, cost) == if touched_keys@.contains(key) {
                sum(brass_partsupp_after_rows@.take(processed@), |row: BrassPartsuppScan| if row.ps_partkey == key && row.ps_suppkey as int == supplier && row.ps_supplycost as int == cost { 1 } else { 0 })
            } else { quote_amount(s.quotes, key, supplier, cost) },
        decreases brass_partsupp_after_rows.len() - index,
    {
        let row = brass_partsupp_after_rows[index];
        let ghost previous = quotes;
        if touched.get(row.ps_partkey).is_some() {
            let cells = parts.tp.get((row.ps_partkey, 0, 0));
            proof {
                assert(brass_partsupp_after_rows@[index as int] == row);
                assert(touched_keys@.contains(row.ps_partkey));
                lemma_rescan_row_eligible(db2, brass_partsupp_after_keys@, brass_partsupp_after_rows@, index as int);
                lemma_eligible_cells(db2, parts, row.ps_partkey);
            }
            if cells.len() == 0 { return Err(3); }
            let adjusted = adjust_bag(&mut quotes, row.ps_partkey, Arc::new(row), 1);
            proof { assert(adjusted) by { if !adjusted { assert(mass_at(previous, row.ps_partkey) + 1 > i128_max()); } } }
            if !adjusted { return Err(2); }
        }
        proof {
            assert forall|key: i64, supplier: int, cost: int| #[trigger] quote_amount(quotes, key, supplier, cost) == if touched_keys@.contains(key) {
                sum(brass_partsupp_after_rows@.take(index as int + 1), |value: BrassPartsuppScan| if value.ps_partkey == key && value.ps_suppkey as int == supplier && value.ps_supplycost as int == cost { 1int } else { 0int })
            } else { quote_amount(s.quotes, key, supplier, cost) } by {
                let weight = |value: PartsuppRow| if value.ps_suppkey == supplier && value.ps_supplycost == cost { 1int } else { 0int };
                if touched_keys@.contains(row.ps_partkey) {
                    assert(index_weight(quotes, key, weight) == index_weight(previous, key, weight) + if key == row.ps_partkey { weight(brass_partsupp_scan_view(row)) } else { 0 });
                }
                assert(quote_amount(previous, key, supplier, cost) == if touched_keys@.contains(key) {
                    sum(brass_partsupp_after_rows@.take(index as int), |value: BrassPartsuppScan| if value.ps_partkey == key && value.ps_suppkey as int == supplier && value.ps_supplycost as int == cost { 1int } else { 0int })
                } else { quote_amount(s.quotes, key, supplier, cost) });
                lemma_sum_take(brass_partsupp_after_rows@, index as int, |value: BrassPartsuppScan| if value.ps_partkey == key && value.ps_suppkey as int == supplier && value.ps_supplycost as int == cost { 1int } else { 0int });
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    proof { assert(brass_partsupp_after_rows@.take(processed@) =~= brass_partsupp_after_rows@); }
    let ghost rebuilt_quotes = quotes;
    let mut quote_changes = Directory::new();
    index = 0;
    processed = Ghost(0);
    while index < partsupp_delta.len()
        invariant
            index <= partsupp_delta.len(), processed@ == index as int,
            touched.valid(), parts.meaning(db2), supported_db(db2),
            forall|k: int| 0 <= k < db2.part.len() ==> fits_i64((#[trigger] db2.part[k]).p_size),
            quote_changes.valid(),
            index_unique(quote_changes), index_nonempty(quote_changes), index_nonzero(quote_changes),
            forall|probe: i64| #[trigger] mass_at(quote_changes, probe) <= index as int,
            forall|i: int| 0 <= i < partsupp_delta@.len() ==> partsupp_delta@[i].sign == 1 || partsupp_delta@[i].sign == -1,
            index_routed(quote_changes, quote_routing(db2)),
            forall|key: i64| (#[trigger] touched.lookup(key) is Some) == touched_keys@.contains(key),
            forall|key: i64, supplier: int, cost: int| #[trigger] quote_amount(quote_changes, key, supplier, cost) ==
                if !touched_keys@.contains(key) && eligible(db2, key) > 0 {
                    sum(partsupp_delta@.take(processed@), |delta: PartsuppDelta| if delta.row.ps_partkey == key && delta.row.ps_suppkey as int == supplier && delta.row.ps_supplycost as int == cost { delta.sign as int } else { 0 })
                } else { 0 },
        decreases partsupp_delta.len() - index,
    {
        let delta = &partsupp_delta[index];
        let key = delta.row.ps_partkey;
        let ghost previous = quote_changes;
        proof { lemma_eligible_cells(db2, parts, key); }
        let cells = parts.tp.get((key, 0, 0));
        if touched.get(key).is_none() && cells.len() > 0 {
            let adjusted = adjust_bag(&mut quote_changes, key, Arc::new(delta.row), delta.sign as i128);
            proof {
                assert(adjusted) by {
                    if !adjusted {
                        assert(delta.sign == partsupp_delta@[index as int].sign);
                        assert(magnitude(delta.sign as int) == 1);
                        assert(mass_at(previous, key) + magnitude(delta.sign as int) > i128_max());
                    }
                }
            }
            if !adjusted { return Err(2); }
        }
        proof {
            assert forall|probe: i64, supplier: int, cost: int| #[trigger] quote_amount(quote_changes, probe, supplier, cost) ==
                if !touched_keys@.contains(probe) && eligible(db2, probe) > 0 {
                    sum(partsupp_delta@.take(index as int + 1), |value: PartsuppDelta| if value.row.ps_partkey == probe && value.row.ps_suppkey as int == supplier && value.row.ps_supplycost as int == cost { value.sign as int } else { 0int })
                } else { 0int } by {
                let weight = |value: PartsuppRow| if value.ps_suppkey == supplier && value.ps_supplycost == cost { 1int } else { 0int };
                if !touched_keys@.contains(key) && eligible(db2, key) > 0 {
                    assert(index_weight(quote_changes, probe, weight) == index_weight(previous, probe, weight) + if probe == key { delta.sign as int * weight(brass_partsupp_scan_view(delta.row)) } else { 0int });
                }
                assert(quote_amount(previous, probe, supplier, cost) == if !touched_keys@.contains(probe) && eligible(db2, probe) > 0 {
                    sum(partsupp_delta@.take(index as int), |value: PartsuppDelta| if value.row.ps_partkey == probe && value.row.ps_suppkey as int == supplier && value.row.ps_supplycost as int == cost { value.sign as int } else { 0int })
                } else { 0int });
                lemma_sum_take(partsupp_delta@, index as int, |value: PartsuppDelta| if value.row.ps_partkey == probe && value.row.ps_suppkey as int == supplier && value.row.ps_supplycost as int == cost { value.sign as int } else { 0int });
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    proof { assert(partsupp_delta@.take(processed@) =~= partsupp_delta@); }
    proof { lemma_routing_after(touched_keys@, s.db(), db2, quotes); }
    let partsupp_count = partsupp_delta.len();
    let rescan_count = brass_partsupp_after_rows.len();
    proof {
        assert(partsupp_count as int == partsupp_delta@.len());
        assert(rescan_count as int == brass_partsupp_after_rows@.len());
        assert(index_routed(rebuilt_quotes, quote_routing(db2)));
        assert(index_positive(rebuilt_quotes));
        lemma_positive_facts(rebuilt_quotes);
    }
    if !finish_bags(&mut quotes, &quote_changes) {
        proof {
            lemma_quote_merge_fits(*s, db2, touched_keys@, brass_partsupp_after_rows@, partsupp_delta@, rebuilt_quotes, quote_changes);
        }
        return Err(4);
    }
    proof {
        lemma_quotes_transfer(*s, db2, touched_keys@, brass_partsupp_after_keys@, brass_partsupp_after_rows@,
            partsupp_delta@, rebuilt_quotes, quote_changes, quotes);
        lemma_quotes_positive(quotes, db2);
    }
    let (suppliers, sflat) = match apply_suppliers(s, supplier_delta, Ghost(db2)) {
        Some(result) => result,
        None => {
            proof { assert(!envelope(s.db())); }
            return Err(2);
        },
    };
    let nations = match apply_nations(s, nation_delta, Ghost(db2)) {
        Some(nations) => nations,
        None => {
            proof { assert(!envelope(s.db())); }
            return Err(2);
        },
    };
    let regions = match apply_regions(s, region_delta, Ghost(db2)) {
        Some(regions) => regions,
        None => {
            proof { assert(!envelope(s.db())); }
            return Err(2);
        },
    };
    proof {
        lemma_regions_positive(db2, regions);
        assert(index_positive(quotes));
        assert(index_routed(quotes, quote_routing(db2)));
        assert(index_positive(suppliers));
        assert(index_routed(suppliers, supplier_routing()));
        assert(index_positive(nations));
        assert(index_routed(nations, nation_routing()));
        lemma_state_indexes_meaning(db2, parts, quotes, suppliers, nations, regions, sflat);
    }
    let state = State::new(parts, quotes, suppliers, nations, regions, sflat, Ghost(db2));
    Ok(state)
}

}
