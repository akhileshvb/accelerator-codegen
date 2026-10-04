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
use crate::output::{Output, enumerate};
pub use crate::model::State;

verus! {

#[verifier::rlimit(20)]
pub fn build(
    brass_parts: Vec<BrassPartScan>, brass_partsupp: &[BrassPartsuppScan],
    suppliers: Vec<SupplierScan>, nations: Vec<NationScan>, regions: Vec<RegionScan>,
    Ghost(db): Ghost<Db>,
) -> (result: Option<State>)
    requires
        scan_brass_parts_ok(db, brass_parts@), scan_brass_partsupp_ok(db, brass_partsupp@),
        scan_suppliers_ok(db, suppliers@), scan_nations_ok(db, nations@), scan_regions_ok(db, regions@),
    ensures result matches Some(state) ==> state.db() == db,
{
    let mut parts = Directory::new();
    let mut sizes = Directory::new();
    let mut quotes = Directory::new();
    let mut supplier_index = Directory::new();
    let mut nation_index = Directory::new();
    let mut region_index = Directory::new();
    let mut index = 0;
    let mut processed: Ghost<int> = Ghost(0);
    while index < brass_parts.len()
        invariant
            index <= brass_parts.len(), processed@ == index as int, parts_valid(parts), sizes_valid(sizes),
            forall|key: i64, size: i64| #[trigger] part_size_total(parts, key, size) == size_count(sizes, size, key),
            forall|key: i64| #[trigger] part_support(parts, key) ==
                sum(brass_parts@.take(processed@), |row: BrassPartScan| if row.p_partkey == key { 1 } else { 0 }),
            forall|key: i64, size: i64, mfgr: Seq<u8>| #[trigger] part_amount(parts, key, size, mfgr) ==
                sum(brass_parts@.take(processed@), |row: BrassPartScan| if row.p_partkey == key && row.p_size == size && row.p_mfgr@ == mfgr { 1 } else { 0 }),
            forall|size: i64, key: i64| #[trigger] size_count(sizes, size, key) ==
                sum(brass_parts@.take(processed@), |row: BrassPartScan| if row.p_partkey == key && row.p_size == size { 1 } else { 0 }),
        decreases brass_parts.len() - index,
    {
        if !insert_part(&mut parts, &mut sizes, Arc::new(brass_parts[index].dup())) { return None; }
        proof {
            assert forall|key: i64| #[trigger] part_support(parts, key) ==
                sum(brass_parts@.take(index as int + 1), |row: BrassPartScan| if row.p_partkey == key { 1int } else { 0int }) by {
                lemma_sum_take(brass_parts@, index as int, |row: BrassPartScan| if row.p_partkey == key { 1int } else { 0int });
            }
            assert forall|key: i64, size: i64, mfgr: Seq<u8>| #[trigger] part_amount(parts, key, size, mfgr) ==
                sum(brass_parts@.take(index as int + 1), |row: BrassPartScan| if row.p_partkey == key && row.p_size == size && row.p_mfgr@ == mfgr { 1int } else { 0int }) by {
                lemma_sum_take(brass_parts@, index as int, |row: BrassPartScan| if row.p_partkey == key && row.p_size == size && row.p_mfgr@ == mfgr { 1int } else { 0int });
            }
            assert forall|size: i64, key: i64| #[trigger] size_count(sizes, size, key) ==
                sum(brass_parts@.take(index as int + 1), |row: BrassPartScan| if row.p_partkey == key && row.p_size == size { 1int } else { 0int }) by {
                lemma_sum_take(brass_parts@, index as int, |row: BrassPartScan| if row.p_partkey == key && row.p_size == size { 1int } else { 0int });
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    proof {
        assert(brass_parts@.take(processed@) =~= brass_parts@);
        assert forall|key: i64, size: i64, mfgr: Seq<u8>| #[trigger] part_amount(parts, key, size, mfgr) ==
            sum(brass_parts@, |row: BrassPartScan| if row.p_partkey == key && row.p_size == size && row.p_mfgr@ == mfgr { 1int } else { 0int }) by {
            lemma_sum_take_full(brass_parts@, |row: BrassPartScan| if row.p_partkey == key && row.p_size == size && row.p_mfgr@ == mfgr { 1int } else { 0int });
        }
        assert forall|size: i64, key: i64| #[trigger] size_count(sizes, size, key) ==
            sum(brass_parts@, |row: BrassPartScan| if row.p_partkey == key && row.p_size == size { 1int } else { 0int }) by {
            lemma_sum_take_full(brass_parts@, |row: BrassPartScan| if row.p_partkey == key && row.p_size == size { 1int } else { 0int });
        }
    }
    let ghost built_parts = parts;
    let ghost built_sizes = sizes;
    proof {
        assert forall|key: i64, size: i64, mfgr: Seq<u8>| #[trigger] part_amount(built_parts, key, size, mfgr) ==
            sum(brass_parts@, |row: BrassPartScan| if row.p_partkey == key && row.p_size == size && row.p_mfgr@ == mfgr { 1int } else { 0int }) by {}
        assert forall|size: i64, key: i64| #[trigger] size_count(built_sizes, size, key) ==
            sum(brass_parts@, |row: BrassPartScan| if row.p_partkey == key && row.p_size == size { 1int } else { 0int }) by {}
    }
    index = 0;
    processed = Ghost(0);
    while index < brass_partsupp.len()
        invariant
            index <= brass_partsupp.len(), processed@ == index as int, quotes.valid(), parts_valid(parts),
            parts == built_parts, sizes == built_sizes,
            index_positive(quotes),
            index_nonempty(quotes),
            forall|key: i64, size: i64| #[trigger] part_size_total(parts, key, size) == size_count(sizes, size, key),
            index_routed(quotes, |key: i64, row: PartsuppRow| row.ps_partkey == key as int),
            forall|key: i64| #[trigger] quotes.lookup(key) is Some ==> part_support(parts, key) > 0,
            forall|key: i64, supplier: int, cost: int| #[trigger] quote_amount(quotes, key, supplier, cost) ==
                sum(brass_partsupp@.take(processed@), |row: BrassPartsuppScan| if row.ps_partkey == key && row.ps_suppkey as int == supplier && row.ps_supplycost as int == cost { 1 } else { 0 }),
        decreases brass_partsupp.len() - index,
    {
        let row = brass_partsupp[index];
        if parts.get(row.ps_partkey).is_none() { return None; }
        let ghost previous = quotes;
        if !adjust_bag(&mut quotes, row.ps_partkey, Arc::new(row), 1) { return None; }
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
            forall|key: i64, row: SupplierRow| #[trigger] supplier_amount(supplier_index, key, row) ==
                sum(suppliers@.take(processed@), |value: SupplierScan| if value.s_suppkey == key && supplier_scan_view(value) == row { 1 } else { 0 }),
        decreases suppliers.len() - index,
    {
        let ghost previous = supplier_index;
        if !adjust_bag(&mut supplier_index, suppliers[index].s_suppkey, Arc::new(suppliers[index].dup()), 1) { return None; }
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
            forall|key: i64, row: NationRow| #[trigger] nation_amount(nation_index, key, row) ==
                sum(nations@.take(processed@), |value: NationScan| if value.n_nationkey == key && nation_scan_view(value) == row { 1 } else { 0 }),
        decreases nations.len() - index,
    {
        let ghost previous = nation_index;
        if !adjust_bag(&mut nation_index, nations[index].n_nationkey, Arc::new(nations[index].dup()), 1) { return None; }
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
        if !adjust_region(&mut region_index, &regions[index].r_name, regions[index].r_regionkey, 1) { return None; }
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
        lemma_scan_representable(db, brass_parts@, brass_partsupp@, suppliers@, nations@, regions@);
        assert forall|key: i64| #[trigger] part_support(parts, key) == eligible(db, key) by {
            assert(brass_parts@.take(brass_parts@.len() as int) =~= brass_parts@);
            lemma_part_scan_counts(db, brass_parts@, key, 0, Seq::empty());
        }
        assert forall|key: i64, size: i64, mfgr: Seq<u8>| #[trigger] part_amount(parts, key, size, mfgr) ==
            sum(db.part, |row: PartRow| if brass(row) && row.p_partkey == key as int && row.p_size == size as int && row.p_mfgr == mfgr { 1int } else { 0int }) by {
            lemma_sum_take_full(brass_parts@, |row: BrassPartScan| if row.p_partkey == key && row.p_size == size && row.p_mfgr@ == mfgr { 1int } else { 0int });
            assert(parts == built_parts);
            assert(part_amount(built_parts, key, size, mfgr) == sum(brass_parts@, |row: BrassPartScan| if row.p_partkey == key && row.p_size == size && row.p_mfgr@ == mfgr { 1int } else { 0int }));
            lemma_sum_ext_all(brass_parts@, |row: BrassPartScan| if row.p_partkey == key && row.p_size == size && row.p_mfgr@ == mfgr { 1int } else { 0int },
                |row: BrassPartScan| part_scan_weight(row, key, size, mfgr));
            lemma_part_amount_transfer(db, brass_parts@, key, size, mfgr, part_amount(parts, key, size, mfgr));
            lemma_sum_ext_all(db.part, |row: PartRow| part_database_weight(row, key, size, mfgr),
                |row: PartRow| if brass(row) && row.p_partkey == key as int && row.p_size == size as int && row.p_mfgr == mfgr { 1int } else { 0int });
        }
        assert forall|size: i64, key: i64| #[trigger] size_count(sizes, size, key) ==
            sum(db.part, |row: PartRow| if brass(row) && row.p_partkey == key as int && row.p_size == size as int { 1int } else { 0int }) by {
            assert(sizes == built_sizes);
            assert(size_count(built_sizes, size, key) == sum(brass_parts@, |row: BrassPartScan| if row.p_partkey == key && row.p_size == size { 1int } else { 0int }));
            lemma_part_scan_counts(db, brass_parts@, key, size, Seq::empty());
        }
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
    proof {
        assert(parts_valid(parts));
        assert(sizes_valid(sizes));
        assert(index_positive(quotes));
        assert(index_positive(supplier_index));
        assert(index_positive(nation_index));
        assert(regions_valid(region_index));
        assert(regions_positive(region_index));
        assert forall|key: i64, size: i64| #[trigger] part_size_total(parts, key, size) == size_count(sizes, size, key) by {}
        assert forall|key: i64| match #[trigger] quotes.lookup(key) {
            None => true,
            Some(bag) => (*bag).valid() && part_support(parts, key) > 0 &&
                forall|index: int| 0 <= index < (*bag).entries@.len() ==> (*((#[trigger] (*bag).entries@[index]).row)).ps_partkey == key,
        } by {
            if quotes.lookup(key) is Some {
                let bag = quotes.lookup(key)->0;
                assert((*bag).entries@.len() > 0);
                assert(index_routed(quotes, |key: i64, row: PartsuppRow| row.ps_partkey == key as int));
                assert(part_support(parts, key) > 0);
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
        assert(state_meaning(db, parts, sizes, quotes, supplier_index, nation_index, region_index));
    }
    let state = State::new(parts, sizes, quotes, supplier_index, nation_index, region_index, Ghost(db));
    proof { state.database_witness(); }
    Some(state)
}

pub fn execute(s: &State, p1: i64, p2: &[u8]) -> (result: Option<Vec<Output>>)
    ensures result matches Some(out) ==> query_result(s.db(), params(p1 as int, p2@), out_view(out@)),
{ Some(enumerate(s, p1, p2)) }

pub fn rescan_brass_parts_after_keys(
    s: &State, nation_delta: &Vec<NationDelta>, part_delta: &[PartKeyDelta],
    partsupp_delta: &[PartsuppDelta], region_delta: &Vec<RegionDelta>, supplier_delta: &Vec<SupplierDelta>,
) -> Vec<i64>
{ part_keys(part_delta) }

pub fn rescan_brass_partsupp_after_keys(
    s: &State, nation_delta: &Vec<NationDelta>, part_delta: &[PartKeyDelta],
    partsupp_delta: &[PartsuppDelta], region_delta: &Vec<RegionDelta>, supplier_delta: &Vec<SupplierDelta>,
) -> Vec<i64>
{ part_keys(part_delta) }

#[verifier::rlimit(40)]
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
        rescan_brass_parts_after_ok(db2, brass_parts_after_keys@, brass_parts_after_rows@),
        rescan_brass_partsupp_after_ok(db2, brass_partsupp_after_keys@, brass_partsupp_after_rows@),
    ensures result matches Ok(after) ==> after.db() == db2,
{
    proof { use_type_invariant(s); s.database_witness(); }
    let touched_keys = part_keys(part_delta);
    let mut touched: Directory<Count> = Directory::new();
    let mut parts = s.parts.share();
    let mut sizes = s.sizes.share();
    let mut quotes = s.quotes.share();
    let mut suppliers = s.suppliers.share();
    let mut nations = s.nations.share();
    let mut regions = s.regions.share();
    let mut index = 0;
    let mut processed: Ghost<int> = Ghost(0);
    while index < touched_keys.len()
        invariant
            index <= touched_keys.len(), processed@ == index as int, touched.valid(),
            parts_valid(parts), sizes_valid(sizes), quotes.valid(),
            index_positive(quotes), index_unique(quotes), index_nonempty(quotes),
            index_routed(quotes, quote_routing(parts)),
            forall|key: i64| (#[trigger] touched.lookup(key) is Some) == touched_keys@.take(processed@).contains(key),
            forall|key: i64| #[trigger] touched_keys@.take(processed@).contains(key) ==>
                brass_parts_after_keys@.contains(key) && brass_partsupp_after_keys@.contains(key),
            forall|key: i64, size: i64| #[trigger] part_size_total(parts, key, size) == size_count(sizes, size, key),
            forall|key: i64| #[trigger] part_support(parts, key) ==
                if touched_keys@.take(processed@).contains(key) { 0 } else { part_support(s.parts, key) },
            forall|key: i64, size: i64, mfgr: Seq<u8>| #[trigger] part_amount(parts, key, size, mfgr) ==
                if touched_keys@.take(processed@).contains(key) { 0 } else { part_amount(s.parts, key, size, mfgr) },
            forall|size: i64, key: i64| #[trigger] size_count(sizes, size, key) ==
                if touched_keys@.take(processed@).contains(key) { 0 } else { size_count(s.sizes, size, key) },
            forall|key: i64, supplier: int, cost: int| #[trigger] quote_amount(quotes, key, supplier, cost) ==
                if touched_keys@.take(processed@).contains(key) { 0 } else { quote_amount(s.quotes, key, supplier, cost) },
        decreases touched_keys.len() - index,
    {
        let key = touched_keys[index];
        if !contains_key(brass_parts_after_keys, key) || !contains_key(brass_partsupp_after_keys, key) { return Err(1); }
        let ghost previous_touched = touched;
        let ghost previous_parts = parts;
        let ghost previous_sizes = sizes;
        let ghost previous_quotes = quotes;
        touched.replace(key, Some(Arc::new(Count { value: 1 })));
        remove_part(&mut parts, &mut sizes, key);
        quotes.replace(key, None);
        proof {
            assert(touched_keys@.take(index as int + 1) =~= touched_keys@.take(index as int).push(key));
            assert forall|probe: i64| #[trigger] touched_keys@.take(index as int + 1).contains(probe) implies
                brass_parts_after_keys@.contains(probe) && brass_partsupp_after_keys@.contains(probe) by {
                broadcast use vstd::seq_lib::group_seq_properties;
                if probe != key { assert(touched_keys@.take(index as int).contains(probe)); }
            }
            assert forall|probe: i64| (#[trigger] touched.lookup(probe) is Some) == touched_keys@.take(index as int + 1).contains(probe) by {
                broadcast use vstd::seq_lib::group_seq_properties;
                assert((previous_touched.lookup(probe) is Some) == touched_keys@.take(index as int).contains(probe));
            }
            assert forall|probe: i64| #[trigger] part_support(parts, probe) ==
                if touched_keys@.take(index as int + 1).contains(probe) { 0 } else { part_support(s.parts, probe) } by {
                broadcast use vstd::seq_lib::group_seq_properties;
                assert(part_support(previous_parts, probe) == if touched_keys@.take(index as int).contains(probe) { 0 } else { part_support(s.parts, probe) });
            }
            assert forall|probe: i64, size: i64, mfgr: Seq<u8>| #[trigger] part_amount(parts, probe, size, mfgr) ==
                if touched_keys@.take(index as int + 1).contains(probe) { 0 } else { part_amount(s.parts, probe, size, mfgr) } by {
                broadcast use vstd::seq_lib::group_seq_properties;
                assert(part_amount(previous_parts, probe, size, mfgr) == if touched_keys@.take(index as int).contains(probe) { 0 } else { part_amount(s.parts, probe, size, mfgr) });
            }
            assert forall|size: i64, probe: i64| #[trigger] size_count(sizes, size, probe) ==
                if touched_keys@.take(index as int + 1).contains(probe) { 0 } else { size_count(s.sizes, size, probe) } by {
                broadcast use vstd::seq_lib::group_seq_properties;
                assert(size_count(previous_sizes, size, probe) == if touched_keys@.take(index as int).contains(probe) { 0 } else { size_count(s.sizes, size, probe) });
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
    proof { assert(touched_keys@.take(processed@) =~= touched_keys@); }
    index = 0;
    processed = Ghost(0);
    while index < brass_parts_after_rows.len()
        invariant
            index <= brass_parts_after_rows.len(), processed@ == index as int,
            touched.valid(), parts_valid(parts), sizes_valid(sizes),
            index_positive(quotes), index_unique(quotes), index_nonempty(quotes),
            index_routed(quotes, quote_routing(parts)),
            forall|key: i64| (#[trigger] touched.lookup(key) is Some) == touched_keys@.contains(key),
            forall|key: i64, size: i64| #[trigger] part_size_total(parts, key, size) == size_count(sizes, size, key),
            forall|key: i64| #[trigger] part_support(parts, key) == if touched_keys@.contains(key) {
                sum(brass_parts_after_rows@.take(processed@), |row: BrassPartScan| if row.p_partkey == key { 1 } else { 0 })
            } else { part_support(s.parts, key) },
            forall|key: i64, size: i64, mfgr: Seq<u8>| #[trigger] part_amount(parts, key, size, mfgr) == if touched_keys@.contains(key) {
                sum(brass_parts_after_rows@.take(processed@), |row: BrassPartScan| if row.p_partkey == key && row.p_size == size && row.p_mfgr@ == mfgr { 1 } else { 0 })
            } else { part_amount(s.parts, key, size, mfgr) },
            forall|size: i64, key: i64| #[trigger] size_count(sizes, size, key) == if touched_keys@.contains(key) {
                sum(brass_parts_after_rows@.take(processed@), |row: BrassPartScan| if row.p_partkey == key && row.p_size == size { 1 } else { 0 })
            } else { size_count(s.sizes, size, key) },
        decreases brass_parts_after_rows.len() - index,
    {
        let row = &brass_parts_after_rows[index];
        if touched.get(row.p_partkey).is_some() {
            if !insert_part(&mut parts, &mut sizes, Arc::new(row.dup())) { return Err(2); }
        }
        proof {
            assert forall|key: i64| #[trigger] part_support(parts, key) == if touched_keys@.contains(key) {
                sum(brass_parts_after_rows@.take(index as int + 1), |value: BrassPartScan| if value.p_partkey == key { 1int } else { 0int })
            } else { part_support(s.parts, key) } by {
                lemma_sum_take(brass_parts_after_rows@, index as int, |value: BrassPartScan| if value.p_partkey == key { 1int } else { 0int });
            }
            assert forall|key: i64, size: i64, mfgr: Seq<u8>| #[trigger] part_amount(parts, key, size, mfgr) == if touched_keys@.contains(key) {
                sum(brass_parts_after_rows@.take(index as int + 1), |value: BrassPartScan| if value.p_partkey == key && value.p_size == size && value.p_mfgr@ == mfgr { 1int } else { 0int })
            } else { part_amount(s.parts, key, size, mfgr) } by {
                lemma_sum_take(brass_parts_after_rows@, index as int, |value: BrassPartScan| if value.p_partkey == key && value.p_size == size && value.p_mfgr@ == mfgr { 1int } else { 0int });
            }
            assert forall|size: i64, key: i64| #[trigger] size_count(sizes, size, key) == if touched_keys@.contains(key) {
                sum(brass_parts_after_rows@.take(index as int + 1), |value: BrassPartScan| if value.p_partkey == key && value.p_size == size { 1int } else { 0int })
            } else { size_count(s.sizes, size, key) } by {
                lemma_sum_take(brass_parts_after_rows@, index as int, |value: BrassPartScan| if value.p_partkey == key && value.p_size == size { 1int } else { 0int });
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    proof {
        assert(brass_parts_after_rows@.take(processed@) =~= brass_parts_after_rows@);
        lemma_part_indexes_transfer(*s, db2, part_delta@, part_full, touched_keys@,
            brass_parts_after_keys@, brass_parts_after_rows@, parts, sizes);
    }
    index = 0;
    processed = Ghost(0);
    while index < brass_partsupp_after_rows.len()
        invariant
            index <= brass_partsupp_after_rows.len(), processed@ == index as int,
            touched.valid(), parts_valid(parts), quotes.valid(),
            index_positive(quotes), index_unique(quotes), index_nonempty(quotes),
            index_routed(quotes, quote_routing(parts)),
            forall|key: i64| (#[trigger] touched.lookup(key) is Some) == touched_keys@.contains(key),
            forall|key: i64, supplier: int, cost: int| #[trigger] quote_amount(quotes, key, supplier, cost) == if touched_keys@.contains(key) {
                sum(brass_partsupp_after_rows@.take(processed@), |row: BrassPartsuppScan| if row.ps_partkey == key && row.ps_suppkey as int == supplier && row.ps_supplycost as int == cost { 1 } else { 0 })
            } else { quote_amount(s.quotes, key, supplier, cost) },
        decreases brass_partsupp_after_rows.len() - index,
    {
        let row = brass_partsupp_after_rows[index];
        let ghost previous = quotes;
        if touched.get(row.ps_partkey).is_some() {
            if parts.get(row.ps_partkey).is_none() { return Err(3); }
            if !adjust_bag(&mut quotes, row.ps_partkey, Arc::new(row), 1) { return Err(2); }
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
            touched.valid(), parts_valid(parts), quote_changes.valid(),
            index_unique(quote_changes), index_nonempty(quote_changes),
            index_routed(quote_changes, quote_routing(parts)),
            forall|key: i64| (#[trigger] touched.lookup(key) is Some) == touched_keys@.contains(key),
            forall|key: i64, supplier: int, cost: int| #[trigger] quote_amount(quote_changes, key, supplier, cost) ==
                if !touched_keys@.contains(key) && part_support(parts, key) > 0 {
                    sum(partsupp_delta@.take(processed@), |delta: PartsuppDelta| if delta.row.ps_partkey == key && delta.row.ps_suppkey as int == supplier && delta.row.ps_supplycost as int == cost { delta.sign as int } else { 0 })
                } else { 0 },
        decreases partsupp_delta.len() - index,
    {
        let delta = &partsupp_delta[index];
        let key = delta.row.ps_partkey;
        let ghost previous = quote_changes;
        if touched.get(key).is_none() && parts.get(key).is_some() {
            if !adjust_bag(&mut quote_changes, key, Arc::new(delta.row), delta.sign as i128) { return Err(2); }
        }
        proof {
            assert forall|probe: i64, supplier: int, cost: int| #[trigger] quote_amount(quote_changes, probe, supplier, cost) ==
                if !touched_keys@.contains(probe) && part_support(parts, probe) > 0 {
                    sum(partsupp_delta@.take(index as int + 1), |value: PartsuppDelta| if value.row.ps_partkey == probe && value.row.ps_suppkey as int == supplier && value.row.ps_supplycost as int == cost { value.sign as int } else { 0int })
                } else { 0int } by {
                let weight = |value: PartsuppRow| if value.ps_suppkey == supplier && value.ps_supplycost == cost { 1int } else { 0int };
                if !touched_keys@.contains(key) && part_support(parts, key) > 0 {
                    assert(index_weight(quote_changes, probe, weight) == index_weight(previous, probe, weight) + if probe == key { delta.sign as int * weight(brass_partsupp_scan_view(delta.row)) } else { 0int });
                }
                assert(quote_amount(previous, probe, supplier, cost) == if !touched_keys@.contains(probe) && part_support(parts, probe) > 0 {
                    sum(partsupp_delta@.take(index as int), |value: PartsuppDelta| if value.row.ps_partkey == probe && value.row.ps_suppkey as int == supplier && value.row.ps_supplycost as int == cost { value.sign as int } else { 0int })
                } else { 0int });
                lemma_sum_take(partsupp_delta@, index as int, |value: PartsuppDelta| if value.row.ps_partkey == probe && value.row.ps_suppkey as int == supplier && value.row.ps_supplycost as int == cost { value.sign as int } else { 0int });
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    proof { assert(partsupp_delta@.take(processed@) =~= partsupp_delta@); }
    if !finish_bags(&mut quotes, &quote_changes) { return Err(4); }
    proof {
        lemma_quotes_transfer(*s, db2, touched_keys@, brass_partsupp_after_keys@, brass_partsupp_after_rows@,
            partsupp_delta@, parts, rebuilt_quotes, quote_changes, quotes);
    }
    let mut supplier_changes = Directory::new();
    index = 0;
    processed = Ghost(0);
    while index < supplier_delta.len()
        invariant
            index <= supplier_delta.len(), processed@ == index as int, supplier_changes.valid(),
            index_unique(supplier_changes), index_nonempty(supplier_changes),
            index_routed(supplier_changes, supplier_routing()),
            forall|key: i64, row: SupplierRow| #[trigger] supplier_amount(supplier_changes, key, row) ==
                sum(supplier_delta@.take(processed@), |delta: SupplierDelta| if delta.row.s_suppkey == key && supplier_scan_view(delta.row) == row { delta.sign as int } else { 0 }),
        decreases supplier_delta.len() - index,
    {
        let delta = &supplier_delta[index];
        let ghost previous = supplier_changes;
        if !adjust_bag(&mut supplier_changes, delta.row.s_suppkey, Arc::new(delta.row.dup()), delta.sign as i128) { return Err(2); }
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
    if !finish_bags(&mut suppliers, &supplier_changes) { return Err(4); }
    proof {
        assert(index_routed(suppliers, supplier_routing()));
        lemma_supplier_capture(*s, db2, supplier_delta@, supplier_changes, suppliers);
    }
    let mut nation_changes = Directory::new();
    index = 0;
    processed = Ghost(0);
    while index < nation_delta.len()
        invariant
            index <= nation_delta.len(), processed@ == index as int, nation_changes.valid(),
            index_unique(nation_changes), index_nonempty(nation_changes),
            index_routed(nation_changes, nation_routing()),
            forall|key: i64, row: NationRow| #[trigger] nation_amount(nation_changes, key, row) ==
                sum(nation_delta@.take(processed@), |delta: NationDelta| if delta.row.n_nationkey == key && nation_scan_view(delta.row) == row { delta.sign as int } else { 0 }),
        decreases nation_delta.len() - index,
    {
        let delta = &nation_delta[index];
        let ghost previous = nation_changes;
        if !adjust_bag(&mut nation_changes, delta.row.n_nationkey, Arc::new(delta.row.dup()), delta.sign as i128) { return Err(2); }
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
    if !finish_bags(&mut nations, &nation_changes) { return Err(4); }
    proof {
        assert(index_routed(nations, nation_routing()));
        lemma_nation_capture(*s, db2, nation_delta@, nation_changes, nations);
    }
    index = 0;
    processed = Ghost(0);
    while index < region_delta.len()
        invariant
            index <= region_delta.len(), processed@ == index as int, regions_valid(regions),
            regions_nonzero(regions),
            forall|name: Seq<u8>, key: i64| #[trigger] region_amount(regions, name, key) == region_amount(s.regions, name, key) +
                sum(region_delta@.take(processed@), |delta: RegionDelta| if delta.row.r_name@ == name && delta.row.r_regionkey == key { delta.sign as int } else { 0 }),
        decreases region_delta.len() - index,
    {
        let delta = &region_delta[index];
        if !adjust_region(&mut regions, &delta.row.r_name, delta.row.r_regionkey, delta.sign as i128) { return Err(2); }
        proof {
            assert forall|name: Seq<u8>, key: i64| #[trigger] region_amount(regions, name, key) == region_amount(s.regions, name, key) +
                sum(region_delta@.take(index as int + 1), |value: RegionDelta| if value.row.r_name@ == name && value.row.r_regionkey == key { value.sign as int } else { 0int }) by {
                lemma_sum_take(region_delta@, index as int, |value: RegionDelta| if value.row.r_name@ == name && value.row.r_regionkey == key { value.sign as int } else { 0int });
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
            if let Some(count) = slice.get(delta.row.r_regionkey) { if count.value < 0 { return Err(4); } }
        }
        compact_region(&mut regions, hash_bytes(&delta.row.r_name));
        index += 1;
        processed = Ghost(index as int);
    }
    proof {
        lemma_regions_positive(db2, regions);
        lemma_apply_representable(*s, db2, touched_keys@, part_delta@, part_full, partsupp_delta@,
            supplier_delta@, nation_delta@, region_delta@, brass_parts_after_keys@, brass_parts_after_rows@,
            brass_partsupp_after_keys@, brass_partsupp_after_rows@, parts);
        assert(part_indexes_meaning(db2, parts, sizes));
        assert(index_positive(quotes));
        assert(index_routed(quotes, quote_routing(parts)));
        assert(index_positive(suppliers));
        assert(index_routed(suppliers, supplier_routing()));
        assert(index_positive(nations));
        assert(index_routed(nations, nation_routing()));
        lemma_state_indexes_meaning(db2, parts, sizes, quotes, suppliers, nations, regions);
    }
    let state = State::new(parts, sizes, quotes, suppliers, nations, regions, Ghost(db2));
    Ok(state)
}

}
