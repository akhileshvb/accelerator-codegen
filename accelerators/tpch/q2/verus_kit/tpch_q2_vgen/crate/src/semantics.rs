use vstd::prelude::*;
use std::sync::Arc;
use verus_common::sums::*;
use verus_common::sqlfns::like_match;
use verus_common::rel::flat;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::model::*;
use crate::maintenance::*;
use crate::bags::*;
use crate::storage::*;
use crate::parts::*;

verus! {

pub(crate) proof fn lemma_state_indexes_meaning(db: Db, parts: Parts,
    quotes: Directory<Bag<BrassPartsuppScan>>, suppliers: Directory<Bag<SupplierScan>>, nations: Directory<Bag<NationScan>>,
    regions: Directory<RegionBucket>, sflat: Arc<Vec<crate::flat::SInfo>>)
    requires supported_db(db), parts.meaning(db), crate::flat::flat_ok((*sflat)@, suppliers),
        quotes.valid(), index_positive(quotes), index_nonempty(quotes), index_routed(quotes, quote_routing(db)),
        suppliers.valid(), index_positive(suppliers), index_routed(suppliers, supplier_routing()),
        nations.valid(), index_positive(nations), index_routed(nations, nation_routing()),
        regions_valid(regions), regions_positive(regions),
        forall|key: i64, supplier: int, cost: int| #[trigger] quote_amount(quotes, key, supplier, cost) ==
            if eligible(db, key) > 0 { sum(db.partsupp, |row: PartsuppRow| if row.ps_partkey == key as int && row.ps_suppkey == supplier && row.ps_supplycost == cost { 1int } else { 0int }) } else { 0int },
        forall|key: i64, row: SupplierRow| #[trigger] supplier_amount(suppliers, key, row) ==
            sum(db.supplier, |value: SupplierRow| if value == row && value.s_suppkey == key as int { 1int } else { 0int }),
        forall|key: i64, row: NationRow| #[trigger] nation_amount(nations, key, row) ==
            sum(db.nation, |value: NationRow| if value == row && value.n_nationkey == key as int { 1int } else { 0int }),
        forall|name: Seq<u8>, key: i64| #[trigger] region_amount(regions, name, key) ==
            sum(db.region, |row: RegionRow| if row.r_name == name && row.r_regionkey == key as int { 1int } else { 0int }),
    ensures state_meaning(db, parts, quotes, suppliers, nations, regions, sflat),
{
    lemma_dimension_database_counts(db, suppliers, nations);
    assert forall|key: i64| match #[trigger] quotes.lookup(key) {
        None => true,
        Some(bag) => (*bag).valid() && (*bag).entries@.len() > 0 && eligible(db, key) > 0 &&
            forall|index: int| 0 <= index < (*bag).entries@.len() ==> (*((#[trigger] (*bag).entries@[index]).row)).ps_partkey == key,
    } by {
        if quotes.lookup(key) is Some {
            let bag = quotes.lookup(key)->0;
            assert(quote_routing(db)(key, (*(*bag).entries@[0].row).rv()));
        }
    }
}

#[verifier::rlimit(20)]
pub(crate) proof fn lemma_apply_representable(before: State, after: Db, touched: Seq<i64>,
    part_delta: Seq<PartKeyDelta>, part_full: Seq<(PartRow, int)>, partsupp_delta: Seq<PartsuppDelta>,
    supplier_delta: Seq<SupplierDelta>, nation_delta: Seq<NationDelta>, region_delta: Seq<RegionDelta>,
    part_keys: Seq<i64>, part_rows: Seq<BrassPartScan>, quote_keys: Seq<i64>, quote_rows: Seq<BrassPartsuppScan>)
    requires before.meaning(), before.db() == before.database@,
        part_delta_ok(part_delta, part_full),
        capture_ok(before.db(), after, nation_delta_seq_view(nation_delta), part_full,
            partsupp_delta_seq_view(partsupp_delta), region_delta_seq_view(region_delta), supplier_delta_seq_view(supplier_delta)),
        rescan_brass_parts_after_ok(after, part_keys, part_rows),
        rescan_brass_partsupp_after_ok(after, quote_keys, quote_rows),
        forall|key: i64| #[trigger] touched.contains(key) == part_delta.map_values(|delta: PartKeyDelta| delta.p_partkey).contains(key),
        forall|key: i64| #[trigger] touched.contains(key) ==> part_keys.contains(key) && quote_keys.contains(key),
        forall|key: i64| !touched.contains(key) ==> #[trigger] eligible(after, key) == eligible(before.db(), key),
    ensures supported_db(after),
{
    let supplier_property = |row: SupplierRow| fits_i64(row.s_suppkey) && fits_i64(row.s_nationkey) && fits_i64(row.s_acctbal);
    verus_prooflib::lemma_bag_step_property(before.db().supplier, after.supplier, supplier_delta_seq_view(supplier_delta), supplier_property);
    let nation_property = |row: NationRow| fits_i64(row.n_nationkey) && fits_i64(row.n_regionkey);
    verus_prooflib::lemma_bag_step_property(before.db().nation, after.nation, nation_delta_seq_view(nation_delta), nation_property);
    let region_property = |row: RegionRow| fits_i64(row.r_regionkey);
    verus_prooflib::lemma_bag_step_property(before.db().region, after.region, region_delta_seq_view(region_delta), region_property);
    assert forall|value: int| #[trigger] seq_view_i64(touched).contains(value) implies
        seq_view_i64(part_keys).contains(value) && seq_view_i64(quote_keys).contains(value) by {
        let position = choose|position: int| 0 <= position < touched.len() && (#[trigger] seq_view_i64(touched)[position]) == value;
        let key = touched[position];
        assert(touched.contains(key));
        let part_position = choose|part_position: int| 0 <= part_position < part_keys.len() && #[trigger] part_keys[part_position] == key;
        let quote_position = choose|quote_position: int| 0 <= quote_position < quote_keys.len() && #[trigger] quote_keys[quote_position] == key;
        assert(seq_view_i64(part_keys)[part_position] == value);
        assert(seq_view_i64(quote_keys)[quote_position] == value);
    }
    let bad_part = |row: PartRow| if fits_i64(row.p_partkey) && fits_i64(row.p_size) { 0int } else { 1int };
    let outside_part = |row: PartRow| if retained(row) && !seq_view_i64(touched).contains(row.p_partkey) { bad_part(row) } else { 0int };
    lemma_sum_zero(before.db().part, outside_part);
    assert forall|position: int| 0 <= position < part_full.len() implies
        (#[trigger] part_full[position]).1 * outside_part(part_full[position].0) == 0 by {
        let key = part_delta[position].p_partkey;
        assert(part_delta.map_values(|delta: PartKeyDelta| delta.p_partkey)[position] == key);
        assert(part_delta.map_values(|delta: PartKeyDelta| delta.p_partkey).contains(key));
        assert(touched.contains(key));
        assert(part_delta[position].p_partkey as int == part_full[position].0.p_partkey);
        let witness = choose|witness: int| 0 <= witness < touched.len() && #[trigger] touched[witness] == key;
        assert(seq_view_i64(touched)[witness] == part_full[position].0.p_partkey);
        assert(seq_view_i64(touched).contains(part_full[position].0.p_partkey));
    }
    lemma_sum_zero(part_full, |change: (PartRow, int)| change.1 * outside_part(change.0));
    assert(sum(after.part, outside_part) == 0);
    lemma_sum_zero(brass_part_scan_seq_view(part_rows), bad_part);
    let rescanned_part = |row: PartRow| if seq_view_i64(part_keys).contains(row.p_partkey) {
        bad_part(row)
    } else { 0int };
    assert(sum(after.part, rescanned_part) == 0);
    assert forall|position: int| 0 <= position < after.part.len() implies {
        let row = #[trigger] after.part[position];
        retained(row) ==> fits_i64(row.p_partkey) && fits_i64(row.p_size)
    } by {
        let row = after.part[position];
        if retained(row) && bad_part(row) != 0 {
            if seq_view_i64(touched).contains(row.p_partkey) { lemma_sum_pos(after.part, rescanned_part, position); }
            else { lemma_sum_pos(after.part, outside_part, position); }
        }
    }
    let bad_quote = |row: PartsuppRow| if fits_i64(row.ps_partkey) && fits_i64(row.ps_suppkey) && fits_i64(row.ps_supplycost) { 0int } else { 1int };
    let outside_quote = |row: PartsuppRow| if scan_brass_partsupp_exists_1(before.db(), row) && !seq_view_i64(touched).contains(row.ps_partkey) { bad_quote(row) } else { 0int };
    lemma_sum_zero(before.db().partsupp, outside_quote);
    lemma_sum_zero(partsupp_delta_seq_view(partsupp_delta), |change: (PartsuppRow, int)| change.1 * outside_quote(change.0));
    assert(sum(after.partsupp, outside_quote) == 0);
    lemma_sum_zero(brass_partsupp_scan_seq_view(quote_rows), bad_quote);
    let rescanned_quote = |row: PartsuppRow| if rescan_brass_partsupp_after_exists_1(after, row) && seq_view_i64(quote_keys).contains(row.ps_partkey) { bad_quote(row) } else { 0int };
    assert(sum(after.partsupp, rescanned_quote) == 0);
    assert forall|position: int| 0 <= position < after.partsupp.len() implies {
        let row = #[trigger] after.partsupp[position];
        scan_brass_partsupp_exists_1(after, row) ==> fits_i64(row.ps_partkey) && fits_i64(row.ps_suppkey) && fits_i64(row.ps_supplycost)
    } by {
        let row = after.partsupp[position];
        if scan_brass_partsupp_exists_1(after, row) && bad_quote(row) != 0 {
            let witness = choose|witness: int| 0 <= witness < after.part.len() &&
                (#[trigger] after.part[witness]).p_partkey == row.ps_partkey && retained(after.part[witness]);
            assert(fits_i64(row.ps_partkey));
            let key = row.ps_partkey as i64;
            assert(key as int == row.ps_partkey);
            if seq_view_i64(touched).contains(row.ps_partkey) {
                assert(rescan_brass_partsupp_after_exists_1(after, row));
                lemma_sum_pos(after.partsupp, rescanned_quote, position);
            } else {
                assert(!touched.contains(key)) by {
                    if touched.contains(key) {
                        let touched_position = choose|touched_position: int| 0 <= touched_position < touched.len() && #[trigger] touched[touched_position] == key;
                        assert(seq_view_i64(touched)[touched_position] == row.ps_partkey);
                    }
                }
                lemma_eligible_exists(after, key);
                lemma_eligible_exists(before.db(), key);
                assert(eligible(after, key) > 0);
                assert(eligible(before.db(), key) == eligible(after, key));
                assert(scan_brass_partsupp_exists_1(before.db(), row));
                lemma_sum_pos(after.partsupp, outside_quote, position);
            }
        }
    }
}

#[verifier::rlimit(20)]
pub proof fn lemma_regions_positive(db: Db, regions: Directory<RegionBucket>)
    requires regions_valid(regions), regions_nonzero(regions),
        forall|name: Seq<u8>, key: i64| #[trigger] region_amount(regions, name, key) ==
            sum(db.region, |row: RegionRow| if row.r_name == name && row.r_regionkey == key as int { 1int } else { 0int }),
    ensures regions_positive(regions),
{
    assert forall|hash: i64| match #[trigger] regions.lookup(hash) {
        None => true, Some(bucket) => region_bucket_positive(*bucket),
    } by {
        if regions.lookup(hash) is Some {
            let bucket = regions.lookup(hash)->0;
            assert forall|index: int| 0 <= index < (*bucket).names@.len() implies counts_positive((#[trigger] (*bucket).names@[index]).keys) by {
                let name = (*(*bucket).names@[index].name)@;
                assert forall|key: i64| match #[trigger] (*bucket).names@[index].keys.lookup(key) {
                    None => true, Some(count) => (*count).value > 0,
                } by {
                    if (*bucket).names@[index].keys.lookup(key) is Some {
                        let weight = |entry: RegionName| if (*entry.name)@ == name { count_at(entry.keys, key) } else { 0int };
                        assert forall|other: int| 0 <= other < (*bucket).names@.len() && other != index implies weight(#[trigger] (*bucket).names@[other]) == 0 by {
                            if other < index { assert((*(*bucket).names@[other].name)@ != name); }
                            else { assert(name != (*(*bucket).names@[other].name)@); }
                        }
                        lemma_sum_single_match((*bucket).names@, weight, index);
                        assert(weight((*bucket).names@[index]) == count_at((*bucket).names@[index].keys, key));
                        assert(sum((*bucket).names@, weight) == count_at((*bucket).names@[index].keys, key));
                        lemma_region_names_weight((*bucket).names@, name, key, weight);
                        assert(text_hash(name) == hash as int);
                        assert(text_hash(name) as i64 == hash);
                        assert(region_names_count((*bucket).names@, name, key) == sum((*bucket).names@, weight));
                        assert(region_names_count((*bucket).names@, name, key) == count_at((*bucket).names@[index].keys, key));
                        assert(region_amount(regions, name, key) == count_at((*bucket).names@[index].keys, key));
                        lemma_sum_nonneg(db.region, |row: RegionRow| if row.r_name == name && row.r_regionkey == key as int { 1int } else { 0int });
                        assert(count_at((*bucket).names@[index].keys, key) != 0);
                    }
                }
            }
        }
    }
}

pub(crate) proof fn lemma_supplier_capture(before: State, after: Db, deltas: Seq<SupplierDelta>,
    changes: Directory<Bag<SupplierScan>>, suppliers: Directory<Bag<SupplierScan>>)
    requires before.meaning(), before.db() == before.database@,
        bag_step(before.db().supplier, after.supplier, supplier_delta_seq_view(deltas)),
        forall|key: i64, row: SupplierRow| #[trigger] supplier_amount(changes, key, row) ==
            sum(deltas, |delta: SupplierDelta| if delta.row.s_suppkey == key && supplier_scan_view(delta.row) == row { delta.sign as int } else { 0int }),
        forall|key: i64, row: SupplierRow| #[trigger] index_amount(suppliers, key, row) ==
            index_amount(before.suppliers, key, row) + index_amount(changes, key, row),
    ensures forall|key: i64, row: SupplierRow| #[trigger] supplier_amount(suppliers, key, row) ==
        sum(after.supplier, |value: SupplierRow| if value == row && value.s_suppkey == key as int { 1int } else { 0int }),
{
    assert forall|key: i64, row: SupplierRow| #[trigger] supplier_amount(suppliers, key, row) ==
        sum(after.supplier, |value: SupplierRow| if value == row && value.s_suppkey == key as int { 1int } else { 0int }) by {
        let weight = |value: SupplierRow| if value == row && value.s_suppkey == key as int { 1int } else { 0int };
        let signed = |change: (SupplierRow, int)| change.1 * weight(change.0);
        lemma_sum_map(deltas, |delta: SupplierDelta| (supplier_scan_view(delta.row), delta.sign as int), signed,
            |delta: SupplierDelta| if delta.row.s_suppkey == key && supplier_scan_view(delta.row) == row { delta.sign as int } else { 0int });
        assert(sum(after.supplier, weight) == sum(before.db().supplier, weight) + sum(supplier_delta_seq_view(deltas), signed));
        assert(index_amount(suppliers, key, row) == index_amount(before.suppliers, key, row) + index_amount(changes, key, row));
        lemma_supplier_count_weight(before.db(), key, row, weight);
        assert(supplier_amount(before.suppliers, key, row) == sum(before.db().supplier, weight));
        assert(supplier_amount(changes, key, row) == sum(deltas, |delta: SupplierDelta| if delta.row.s_suppkey == key && supplier_scan_view(delta.row) == row { delta.sign as int } else { 0int }));
        assert(supplier_amount(changes, key, row) == sum(supplier_delta_seq_view(deltas), signed));
    }
}

pub(crate) proof fn lemma_nation_capture(before: State, after: Db, deltas: Seq<NationDelta>,
    changes: Directory<Bag<NationScan>>, nations: Directory<Bag<NationScan>>)
    requires before.meaning(), before.db() == before.database@,
        bag_step(before.db().nation, after.nation, nation_delta_seq_view(deltas)),
        forall|key: i64, row: NationRow| #[trigger] nation_amount(changes, key, row) ==
            sum(deltas, |delta: NationDelta| if delta.row.n_nationkey == key && nation_scan_view(delta.row) == row { delta.sign as int } else { 0int }),
        forall|key: i64, row: NationRow| #[trigger] index_amount(nations, key, row) ==
            index_amount(before.nations, key, row) + index_amount(changes, key, row),
    ensures forall|key: i64, row: NationRow| #[trigger] nation_amount(nations, key, row) ==
        sum(after.nation, |value: NationRow| if value == row && value.n_nationkey == key as int { 1int } else { 0int }),
{
    assert forall|key: i64, row: NationRow| #[trigger] nation_amount(nations, key, row) ==
        sum(after.nation, |value: NationRow| if value == row && value.n_nationkey == key as int { 1int } else { 0int }) by {
        let weight = |value: NationRow| if value == row && value.n_nationkey == key as int { 1int } else { 0int };
        let signed = |change: (NationRow, int)| change.1 * weight(change.0);
        lemma_sum_map(deltas, |delta: NationDelta| (nation_scan_view(delta.row), delta.sign as int), signed,
            |delta: NationDelta| if delta.row.n_nationkey == key && nation_scan_view(delta.row) == row { delta.sign as int } else { 0int });
        assert(sum(after.nation, weight) == sum(before.db().nation, weight) + sum(nation_delta_seq_view(deltas), signed));
        assert(index_amount(nations, key, row) == index_amount(before.nations, key, row) + index_amount(changes, key, row));
        lemma_nation_count_weight(before.db(), key, row, weight);
        assert(nation_amount(before.nations, key, row) == sum(before.db().nation, weight));
        assert(nation_amount(changes, key, row) == sum(deltas, |delta: NationDelta| if delta.row.n_nationkey == key && nation_scan_view(delta.row) == row { delta.sign as int } else { 0int }));
        assert(nation_amount(changes, key, row) == sum(nation_delta_seq_view(deltas), signed));
    }
}

pub proof fn lemma_quote_rescan_count(db: Db, keys: Seq<i64>, scanned: Seq<BrassPartsuppScan>, key: i64, supplier: int, cost: int)
    requires rescan_brass_partsupp_after_ok(db, keys, scanned), keys.contains(key),
    ensures sum(scanned, |row: BrassPartsuppScan| if row.ps_partkey == key && row.ps_suppkey as int == supplier && row.ps_supplycost as int == cost { 1int } else { 0int }) ==
        if eligible(db, key) > 0 { sum(db.partsupp, |row: PartsuppRow| if row.ps_partkey == key as int && row.ps_suppkey == supplier && row.ps_supplycost == cost { 1int } else { 0int }) } else { 0int },
{
    lemma_eligible_exists(db, key);
    let position = choose|position: int| 0 <= position < keys.len() && #[trigger] keys[position] == key;
    assert(seq_view_i64(keys)[position] == key as int);
    assert(seq_view_i64(keys).contains(key as int));
    let weight = |row: PartsuppRow| if row.ps_partkey == key as int && row.ps_suppkey == supplier && row.ps_supplycost == cost { 1int } else { 0int };
    let restricted = |row: PartsuppRow| if rescan_brass_partsupp_after_exists_1(db, row) && seq_view_i64(keys).contains(row.ps_partkey) { weight(row) } else { 0int };
    let source = |row: PartsuppRow| if eligible(db, key) > 0 { weight(row) } else { 0int };
    assert forall|row: PartsuppRow| #[trigger] restricted(row) == source(row) by {
        if row.ps_partkey == key as int {
            assert(rescan_brass_partsupp_after_exists_1(db, row) == (eligible(db, key) > 0));
        }
    }
    lemma_sum_ext_all(db.partsupp, restricted, source);
    if eligible(db, key) <= 0 { lemma_sum_zero(db.partsupp, source); }
    else { lemma_sum_ext_all(db.partsupp, source, weight); }
    assert(sum(brass_partsupp_scan_seq_view(scanned), weight) == sum(db.partsupp, restricted));
    lemma_sum_map(scanned, |row: BrassPartsuppScan| brass_partsupp_scan_view(row), weight,
        |row: BrassPartsuppScan| if row.ps_partkey == key && row.ps_suppkey as int == supplier && row.ps_supplycost as int == cost { 1int } else { 0int });
}

pub(crate) proof fn lemma_quotes_transfer(before: State, after: Db, touched: Seq<i64>, keys: Seq<i64>, scanned: Seq<BrassPartsuppScan>,
    deltas: Seq<PartsuppDelta>, rebuilt: Directory<Bag<BrassPartsuppScan>>,
    changes: Directory<Bag<BrassPartsuppScan>>, quotes: Directory<Bag<BrassPartsuppScan>>)
    requires
        before.meaning(), before.db() == before.database@,
        bag_step(before.db().partsupp, after.partsupp, partsupp_delta_seq_view(deltas)),
        rescan_brass_partsupp_after_ok(after, keys, scanned),
        forall|key: i64| #[trigger] touched.contains(key) ==> keys.contains(key),
        forall|key: i64| !touched.contains(key) ==> #[trigger] eligible(before.db(), key) == eligible(after, key),
        forall|key: i64, supplier: int, cost: int| #[trigger] quote_amount(rebuilt, key, supplier, cost) == if touched.contains(key) {
            sum(scanned, |row: BrassPartsuppScan| if row.ps_partkey == key && row.ps_suppkey as int == supplier && row.ps_supplycost as int == cost { 1int } else { 0int })
        } else { quote_amount(before.quotes, key, supplier, cost) },
        forall|key: i64, supplier: int, cost: int| #[trigger] quote_amount(changes, key, supplier, cost) == if !touched.contains(key) && eligible(after, key) > 0 {
            sum(deltas, |delta: PartsuppDelta| if delta.row.ps_partkey == key && delta.row.ps_suppkey as int == supplier && delta.row.ps_supplycost as int == cost { delta.sign as int } else { 0int })
        } else { 0int },
        forall|key: i64, weight: spec_fn(PartsuppRow) -> int| #[trigger] index_weight(quotes, key, weight) == index_weight(rebuilt, key, weight) + index_weight(changes, key, weight),
    ensures forall|key: i64, supplier: int, cost: int| #[trigger] quote_amount(quotes, key, supplier, cost) ==
        if eligible(after, key) > 0 { sum(after.partsupp, |row: PartsuppRow| if row.ps_partkey == key as int && row.ps_suppkey == supplier && row.ps_supplycost == cost { 1int } else { 0int }) } else { 0int },
{
    assert forall|key: i64, supplier: int, cost: int| #[trigger] quote_amount(quotes, key, supplier, cost) ==
        if eligible(after, key) > 0 { sum(after.partsupp, |row: PartsuppRow| if row.ps_partkey == key as int && row.ps_suppkey == supplier && row.ps_supplycost == cost { 1int } else { 0int }) } else { 0int } by {
        let local_weight = |row: PartsuppRow| if row.ps_suppkey == supplier && row.ps_supplycost == cost { 1int } else { 0int };
        assert(index_weight(quotes, key, local_weight) == index_weight(rebuilt, key, local_weight) + index_weight(changes, key, local_weight));
        assert(quote_amount(quotes, key, supplier, cost) == quote_amount(rebuilt, key, supplier, cost) + quote_amount(changes, key, supplier, cost));
        assert(quote_amount(changes, key, supplier, cost) == if !touched.contains(key) && eligible(after, key) > 0 {
            sum(deltas, |delta: PartsuppDelta| if delta.row.ps_partkey == key && delta.row.ps_suppkey as int == supplier && delta.row.ps_supplycost as int == cost { delta.sign as int } else { 0int })
        } else { 0int });
        assert(quote_amount(rebuilt, key, supplier, cost) == if touched.contains(key) {
            sum(scanned, |row: BrassPartsuppScan| if row.ps_partkey == key && row.ps_suppkey as int == supplier && row.ps_supplycost as int == cost { 1int } else { 0int })
        } else { quote_amount(before.quotes, key, supplier, cost) });
        if touched.contains(key) {
            lemma_quote_rescan_count(after, keys, scanned, key, supplier, cost);
        } else {
            assert(eligible(before.db(), key) == eligible(after, key));
            assert(quote_amount(before.quotes, key, supplier, cost) == if eligible(before.db(), key) > 0 {
                sum(before.db().partsupp, |row: PartsuppRow| if row.ps_partkey == key as int && row.ps_suppkey == supplier && row.ps_supplycost == cost { 1int } else { 0int })
            } else { 0int });
            if eligible(after, key) > 0 {
                let weight = |row: PartsuppRow| if row.ps_partkey == key as int && row.ps_suppkey == supplier && row.ps_supplycost == cost { 1int } else { 0int };
                let signed = |change: (PartsuppRow, int)| change.1 * weight(change.0);
                lemma_sum_map(deltas, |delta: PartsuppDelta| (brass_partsupp_scan_view(delta.row), delta.sign as int), signed,
                    |delta: PartsuppDelta| if delta.row.ps_partkey == key && delta.row.ps_suppkey as int == supplier && delta.row.ps_supplycost as int == cost { delta.sign as int } else { 0int });
                assert(sum(after.partsupp, weight) == sum(before.db().partsupp, weight) + sum(partsupp_delta_seq_view(deltas), signed));
            }
        }
    }
}

pub proof fn lemma_part_update_weight(before: Db, after: Db, deltas: Seq<PartKeyDelta>,
    changes: Seq<(PartRow, int)>, touched: Seq<i64>, keys: Seq<i64>, scanned: Seq<BrassPartScan>,
    key: i64, weight: spec_fn(PartRow) -> int)
    requires
        part_delta_ok(deltas, changes),
        bag_step(before.part, after.part, changes),
        rescan_brass_parts_after_ok(after, keys, scanned),
        forall|probe: i64| #[trigger] touched.contains(probe) == deltas.map_values(|delta: PartKeyDelta| delta.p_partkey).contains(probe),
        touched.contains(key) ==> keys.contains(key),
        forall|row: PartRow| row.p_partkey != key as int ==> #[trigger] weight(row) == 0,
    ensures
        (if touched.contains(key) { sum(brass_part_scan_seq_view(scanned), weight) }
         else { sum(before.part, |row: PartRow| if retained(row) { weight(row) } else { 0int }) }) ==
        sum(after.part, |row: PartRow| if retained(row) { weight(row) } else { 0int }),
{
    let source = |row: PartRow| if retained(row) { weight(row) } else { 0int };
    if touched.contains(key) {
        assert(seq_view_i64(keys).contains(key as int)) by {
            let position = choose|position: int| 0 <= position < keys.len() && #[trigger] keys[position] == key;
            assert(seq_view_i64(keys)[position] == key as int);
        }
        let restricted = |row: PartRow| if seq_view_i64(keys).contains(row.p_partkey) { weight(row) } else { 0int };
        assert(sum(brass_part_scan_seq_view(scanned), weight) == sum(after.part, restricted));
        lemma_sum_ext_all(after.part, restricted, source);
    } else {
        assert forall|position: int| 0 <= position < changes.len() implies
            (#[trigger] changes[position]).1 * source(changes[position].0) == 0 by {
            assert(deltas[position].p_partkey as int == changes[position].0.p_partkey);
            assert(deltas.map_values(|delta: PartKeyDelta| delta.p_partkey)[position] == deltas[position].p_partkey);
            assert(deltas.map_values(|delta: PartKeyDelta| delta.p_partkey).contains(deltas[position].p_partkey));
            assert(changes[position].0.p_partkey != key as int);
        }
        lemma_sum_zero(changes, |change: (PartRow, int)| change.1 * source(change.0));
        assert(sum(after.part, source) == sum(before.part, source) + sum(changes, |change: (PartRow, int)| change.1 * source(change.0)));
    }
}

/// Rows of untouched part keys keep their count.
pub proof fn lemma_eligible_untouched(before: Db, after: Db, deltas: Seq<PartKeyDelta>,
    changes: Seq<(PartRow, int)>, touched: Seq<i64>, keys: Seq<i64>, scanned: Seq<BrassPartScan>, key: i64)
    requires
        part_delta_ok(deltas, changes), bag_step(before.part, after.part, changes),
        rescan_brass_parts_after_ok(after, keys, scanned),
        forall|probe: i64| #[trigger] touched.contains(probe) == deltas.map_values(|delta: PartKeyDelta| delta.p_partkey).contains(probe),
        forall|probe: i64| #[trigger] touched.contains(probe) ==> keys.contains(probe),
        !touched.contains(key),
    ensures eligible(after, key) == eligible(before, key),
{
    let weight = |row: PartRow| if row.p_partkey == key as int { 1int } else { 0int };
    lemma_part_update_weight(before, after, deltas, changes, touched, keys, scanned, key, weight);
    lemma_sum_ext_all(before.part,
        |row: PartRow| if retained(row) { weight(row) } else { 0int },
        |row: PartRow| if row.p_partkey == key as int && retained(row) { 1int } else { 0int });
    lemma_sum_ext_all(after.part,
        |row: PartRow| if retained(row) { weight(row) } else { 0int },
        |row: PartRow| if row.p_partkey == key as int && retained(row) { 1int } else { 0int });
}

#[verifier::rlimit(40)]
pub(crate) proof fn lemma_parts_transfer(before: State, after: Db, deltas: Seq<PartKeyDelta>,
    changes: Seq<(PartRow, int)>, touched: Seq<i64>, keys: Seq<i64>, scanned: Seq<BrassPartScan>, ps: Parts)
    requires
        before.meaning(), before.db() == before.database@,
        part_delta_ok(deltas, changes), bag_step(before.db().part, after.part, changes),
        rescan_brass_parts_after_ok(after, keys, scanned),
        forall|probe: i64| #[trigger] touched.contains(probe) == deltas.map_values(|delta: PartKeyDelta| delta.p_partkey).contains(probe),
        forall|probe: i64| #[trigger] touched.contains(probe) ==> keys.contains(probe),
        ps.structure_ok(),
        forall|key: i64, size: i64, target: PartRow| #[trigger] ps.amount(key, size, target) == if touched.contains(key) {
            part_scan_amount(scanned, key, size, target)
        } else { before.parts.amount(key, size, target) },
        forall|index: int| 0 <= index < scanned.len() ==>
            ps.dict_has((#[trigger] scanned[index]).p_mfgr@) && ps.dict_has(scanned[index].p_type@),
        forall|text: Seq<u8>| before.parts.dict_has(text) ==> #[trigger] ps.dict_has(text),
    ensures ps.meaning(after),
{
    assert forall|key: i64, size: i64, target: PartRow| #[trigger] ps.amount(key, size, target) == db_amount(after, key, size, target) by {
        let weight = |row: PartRow| if row.p_partkey == key as int && row.p_size == size as int && row == target { 1int } else { 0int };
        lemma_part_update_weight(before.db(), after, deltas, changes, touched, keys, scanned, key, weight);
        lemma_sum_map(scanned, |row: BrassPartScan| brass_part_scan_view(row), weight,
            |row: BrassPartScan| part_scan_weight(row, key, size, target));
        lemma_sum_ext_all(before.db().part,
            |row: PartRow| if retained(row) { weight(row) } else { 0int }, amount_weight(key, size, target));
        lemma_sum_ext_all(after.part,
            |row: PartRow| if retained(row) { weight(row) } else { 0int }, amount_weight(key, size, target));
        assert(before.parts.amount(key, size, target) == db_amount(before.db(), key, size, target));
    }
    // every row of the new database has its strings in the dictionary
    let covered = |row: PartRow| if ps.dict_has(row.p_mfgr) && ps.dict_has(row.p_type) { 0int } else { 1int };
    assert forall|index: int| 0 <= index < brass_part_scan_seq_view(scanned).len() implies
        covered(#[trigger] brass_part_scan_seq_view(scanned)[index]) == 0 by {
        assert(brass_part_scan_seq_view(scanned)[index] == brass_part_scan_view(scanned[index]));
        assert(ps.dict_has(scanned[index].p_mfgr@) && ps.dict_has(scanned[index].p_type@));
    }
    lemma_sum_zero(brass_part_scan_seq_view(scanned), covered);
    let restricted = |row: PartRow| if seq_view_i64(keys).contains(row.p_partkey) { covered(row) } else { 0int };
    assert(sum(brass_part_scan_seq_view(scanned), covered) == sum(after.part, restricted));
    let outside = |row: PartRow| if !seq_view_i64(keys).contains(row.p_partkey) { covered(row) } else { 0int };
    assert forall|index: int| 0 <= index < before.db().part.len() implies outside(#[trigger] before.db().part[index]) == 0 by {
        let row = before.db().part[index];
        assert(before.parts.dict_has(row.p_mfgr) && before.parts.dict_has(row.p_type));
    }
    lemma_sum_zero(before.db().part, outside);
    assert forall|position: int| 0 <= position < changes.len() implies
        (#[trigger] changes[position]).1 * outside(changes[position].0) == 0 by {
        let key = deltas[position].p_partkey;
        assert(deltas.map_values(|delta: PartKeyDelta| delta.p_partkey)[position] == key);
        assert(deltas.map_values(|delta: PartKeyDelta| delta.p_partkey).contains(key));
        assert(touched.contains(key));
        assert(keys.contains(key));
        assert(deltas[position].p_partkey as int == changes[position].0.p_partkey);
        let witness = choose|witness: int| 0 <= witness < keys.len() && #[trigger] keys[witness] == key;
        assert(seq_view_i64(keys)[witness] == changes[position].0.p_partkey);
        assert(seq_view_i64(keys).contains(changes[position].0.p_partkey));
    }
    lemma_sum_zero(changes, |change: (PartRow, int)| change.1 * outside(change.0));
    assert(sum(after.part, outside) == sum(before.db().part, outside) + sum(changes, |change: (PartRow, int)| change.1 * outside(change.0)));
    assert forall|index: int| 0 <= index < after.part.len() implies
        ps.dict_has((#[trigger] after.part[index]).p_mfgr) && ps.dict_has(after.part[index].p_type) by {
        let row = after.part[index];
        if !(ps.dict_has(row.p_mfgr) && ps.dict_has(row.p_type)) {
            assert(covered(row) == 1);
            if seq_view_i64(keys).contains(row.p_partkey) { lemma_sum_pos(after.part, restricted, index); }
            else { lemma_sum_pos(after.part, outside, index); }
        }
    }
}

pub proof fn lemma_scan_representable(db: Db, parts: Seq<BrassPartScan>, quotes: Seq<BrassPartsuppScan>,
    suppliers: Seq<SupplierScan>, nations: Seq<NationScan>, regions: Seq<RegionScan>)
    requires scan_brass_parts_ok(db, parts), scan_brass_partsupp_ok(db, quotes),
        scan_suppliers_ok(db, suppliers), scan_nations_ok(db, nations), scan_regions_ok(db, regions),
    ensures supported_db(db),
{
    let part_weight = |row: PartRow| if fits_i64(row.p_partkey) && fits_i64(row.p_size) { 0int } else { 1int };
    lemma_sum_zero(brass_part_scan_seq_view(parts), part_weight);
    let part_source = |row: PartRow| if retained(row) { part_weight(row) } else { 0int };
    assert(sum(brass_part_scan_seq_view(parts), part_weight) == sum(db.part, part_weight));
    lemma_sum_ext_all(db.part, part_weight, part_source);
    assert(sum(db.part, part_source) == 0);
    assert forall|index: int| 0 <= index < db.part.len() implies {
        let row = #[trigger] db.part[index];
        retained(row) ==> fits_i64(row.p_partkey) && fits_i64(row.p_size)
    } by {
        if part_source(db.part[index]) != 0 { lemma_sum_pos(db.part, part_source, index); }
    }
    let quote_weight = |row: PartsuppRow| if fits_i64(row.ps_partkey) && fits_i64(row.ps_suppkey) && fits_i64(row.ps_supplycost) { 0int } else { 1int };
    lemma_sum_zero(brass_partsupp_scan_seq_view(quotes), quote_weight);
    let quote_source = |row: PartsuppRow| if scan_brass_partsupp_exists_1(db, row) { quote_weight(row) } else { 0int };
    assert(sum(db.partsupp, quote_source) == 0);
    assert forall|index: int| 0 <= index < db.partsupp.len() implies {
        let row = #[trigger] db.partsupp[index];
        scan_brass_partsupp_exists_1(db, row) ==> fits_i64(row.ps_partkey) && fits_i64(row.ps_suppkey) && fits_i64(row.ps_supplycost)
    } by {
        if quote_source(db.partsupp[index]) != 0 { lemma_sum_pos(db.partsupp, quote_source, index); }
    }
    let supplier_weight = |row: SupplierRow| if fits_i64(row.s_suppkey) && fits_i64(row.s_nationkey) && fits_i64(row.s_acctbal) { 0int } else { 1int };
    lemma_sum_zero(supplier_scan_seq_view(suppliers), supplier_weight);
    assert(sum(db.supplier, supplier_weight) == 0);
    assert forall|index: int| 0 <= index < db.supplier.len() implies {
        let row = #[trigger] db.supplier[index];
        fits_i64(row.s_suppkey) && fits_i64(row.s_nationkey) && fits_i64(row.s_acctbal)
    } by {
        if supplier_weight(db.supplier[index]) != 0 { lemma_sum_pos(db.supplier, supplier_weight, index); }
    }
    let nation_weight = |row: NationRow| if fits_i64(row.n_nationkey) && fits_i64(row.n_regionkey) { 0int } else { 1int };
    lemma_sum_zero(nation_scan_seq_view(nations), nation_weight);
    assert(sum(db.nation, nation_weight) == 0);
    assert forall|index: int| 0 <= index < db.nation.len() implies {
        let row = #[trigger] db.nation[index];
        fits_i64(row.n_nationkey) && fits_i64(row.n_regionkey)
    } by {
        if nation_weight(db.nation[index]) != 0 { lemma_sum_pos(db.nation, nation_weight, index); }
    }
    let region_weight = |row: RegionRow| if fits_i64(row.r_regionkey) { 0int } else { 1int };
    lemma_sum_zero(region_scan_seq_view(regions), region_weight);
    assert(sum(db.region, region_weight) == 0);
    assert forall|index: int| 0 <= index < db.region.len() implies fits_i64((#[trigger] db.region[index]).r_regionkey) by {
        if region_weight(db.region[index]) != 0 { lemma_sum_pos(db.region, region_weight, index); }
    }
}

pub proof fn lemma_eligible_exists(db: Db, key: i64)
    ensures eligible(db, key) >= 0,
        (eligible(db, key) > 0) == (exists|index: int| 0 <= index < db.part.len() &&
            (#[trigger] db.part[index]).p_partkey == key as int && retained(db.part[index])),
{
    let weight = |row: PartRow| if row.p_partkey == key as int && retained(row) { 1int } else { 0int };
    lemma_sum_nonneg(db.part, weight);
    if eligible(db, key) > 0 {
        let witness = lemma_sum_nonzero_witness(db.part, weight);
        assert(db.part[witness].p_partkey == key as int && retained(db.part[witness]));
    } else {
        assert forall|index: int| 0 <= index < db.part.len() implies
            (#[trigger] db.part[index]).p_partkey != key as int || !retained(db.part[index]) by {
            if weight(db.part[index]) > 0 { lemma_sum_pos(db.part, weight, index); }
        }
    }
}

pub proof fn lemma_part_scan_counts(db: Db, rows: Seq<BrassPartScan>, key: i64, size: i64, target: PartRow)
    requires scan_brass_parts_ok(db, rows),
    ensures
        sum(rows, |row: BrassPartScan| if row.p_partkey == key { 1int } else { 0int }) == eligible(db, key),
        sum(rows, |row: BrassPartScan| if row.p_partkey == key && row.p_size == size && brass_part_scan_view(row) == target { 1int } else { 0int }) ==
            sum(db.part, |row: PartRow| if retained(row) && row.p_partkey == key as int && row.p_size == size as int && row == target { 1int } else { 0int }),
        sum(rows, |row: BrassPartScan| if row.p_partkey == key && row.p_size == size { 1int } else { 0int }) ==
            sum(db.part, |row: PartRow| if retained(row) && row.p_partkey == key as int && row.p_size == size as int { 1int } else { 0int }),
{
    let projection = |row: BrassPartScan| brass_part_scan_view(row);
    let support = |row: PartRow| if row.p_partkey == key as int { 1int } else { 0int };
    lemma_sum_map(rows, projection, support, |row: BrassPartScan| if row.p_partkey == key { 1int } else { 0int });
    assert(sum(brass_part_scan_seq_view(rows), support) == sum(db.part, support));
    lemma_sum_ext_all(db.part, support,
        |row: PartRow| if row.p_partkey == key as int && retained(row) { 1int } else { 0int });
    let amount = |row: PartRow| if row.p_partkey == key as int && row.p_size == size as int && row == target { 1int } else { 0int };
    lemma_sum_map(rows, projection, amount, |row: BrassPartScan| if row.p_partkey == key && row.p_size == size && brass_part_scan_view(row) == target { 1int } else { 0int });
    assert(sum(brass_part_scan_seq_view(rows), amount) == sum(db.part, amount));
    lemma_sum_ext_all(db.part, amount,
        |row: PartRow| if retained(row) && row.p_partkey == key as int && row.p_size == size as int && row == target { 1int } else { 0int });
    let sized = |row: PartRow| if row.p_partkey == key as int && row.p_size == size as int { 1int } else { 0int };
    lemma_sum_map(rows, projection, sized, |row: BrassPartScan| if row.p_partkey == key && row.p_size == size { 1int } else { 0int });
    assert(sum(brass_part_scan_seq_view(rows), sized) == sum(db.part, sized));
    lemma_sum_ext_all(db.part, sized,
        |row: PartRow| if retained(row) && row.p_partkey == key as int && row.p_size == size as int { 1int } else { 0int });
}

pub proof fn lemma_quote_scan_count(db: Db, rows: Seq<BrassPartsuppScan>, key: i64, supplier: int, cost: int)
    requires scan_brass_partsupp_ok(db, rows),
    ensures sum(rows, |row: BrassPartsuppScan| if row.ps_partkey == key && row.ps_suppkey as int == supplier && row.ps_supplycost as int == cost { 1int } else { 0int }) ==
        if eligible(db, key) > 0 { sum(db.partsupp, |row: PartsuppRow| if row.ps_partkey == key as int && row.ps_suppkey == supplier && row.ps_supplycost == cost { 1int } else { 0int }) } else { 0int },
{
    let weight = |row: PartsuppRow| if row.ps_partkey == key as int && row.ps_suppkey == supplier && row.ps_supplycost == cost { 1int } else { 0int };
    lemma_sum_map(rows, |row: BrassPartsuppScan| brass_partsupp_scan_view(row), weight,
        |row: BrassPartsuppScan| if row.ps_partkey == key && row.ps_suppkey as int == supplier && row.ps_supplycost as int == cost { 1int } else { 0int });
    assert(sum(brass_partsupp_scan_seq_view(rows), weight) == sum(db.partsupp, |row: PartsuppRow| if scan_brass_partsupp_exists_1(db, row) { weight(row) } else { 0int }));
    lemma_eligible_exists(db, key);
    assert forall|row: PartsuppRow| #[trigger] weight(row) != 0 implies scan_brass_partsupp_exists_1(db, row) == (eligible(db, key) > 0) by {}
    if eligible(db, key) > 0 {
        lemma_sum_ext_all(db.partsupp, |row: PartsuppRow| if scan_brass_partsupp_exists_1(db, row) { weight(row) } else { 0int }, weight);
    } else {
        lemma_sum_zero(db.partsupp, |row: PartsuppRow| if scan_brass_partsupp_exists_1(db, row) { weight(row) } else { 0int });
    }
}

pub open spec fn part_scan_weight(row: BrassPartScan, key: i64, size: i64, target: PartRow) -> int {
    if row.p_partkey == key && row.p_size == size && brass_part_scan_view(row) == target { 1 } else { 0 }
}

pub open spec fn part_scan_amount(rows: Seq<BrassPartScan>, key: i64, size: i64, target: PartRow) -> int {
    sum(rows, |row: BrassPartScan| part_scan_weight(row, key, size, target))
}

pub open spec fn part_database_weight(row: PartRow, key: i64, size: i64, target: PartRow) -> int {
    if retained(row) && row.p_partkey == key as int && row.p_size == size as int && row == target { 1 } else { 0 }
}

pub open spec fn part_database_amount(db: Db, key: i64, size: i64, target: PartRow) -> int {
    sum(db.part, |row: PartRow| part_database_weight(row, key, size, target))
}

pub proof fn lemma_part_amount_transfer(db: Db, rows: Seq<BrassPartScan>, key: i64, size: i64, target: PartRow, amount: int)
    requires scan_brass_parts_ok(db, rows),
        amount == part_scan_amount(rows, key, size, target),
    ensures amount == part_database_amount(db, key, size, target),
{
    let weight = |row: PartRow| if row.p_partkey == key as int && row.p_size == size as int && row == target { 1int } else { 0int };
    lemma_sum_map(rows, |row: BrassPartScan| brass_part_scan_view(row), weight,
        |row: BrassPartScan| part_scan_weight(row, key, size, target));
    assert(sum(brass_part_scan_seq_view(rows), weight) == sum(db.part, weight));
    lemma_sum_ext_all(db.part, weight, |row: PartRow| part_database_weight(row, key, size, target));
}

pub proof fn lemma_empty_tuple_flat(children: Seq<Seq<Tuple>>)
    requires forall|index: int| 0 <= index < children.len() ==> (#[trigger] children[index]).len() == 0,
    ensures flat(children).len() == 0,
    decreases children.len(),
{
    if children.len() > 0 {
        lemma_empty_tuple_flat(children.drop_last());
        assert(children.last().len() == 0);
    }
}

pub proof fn lemma_no_query_tuples(db: Db, parameters: Params)
    requires forall|part_index: int, supplier_index: int, quote_index: int, nation_index: int, region_index: int|
        0 <= part_index < db.part.len() && 0 <= supplier_index < db.supplier.len() && 0 <= quote_index < db.partsupp.len() &&
        0 <= nation_index < db.nation.len() && 0 <= region_index < db.region.len() ==>
        !#[trigger] where_clause(db, parameters, db.part[part_index], db.supplier[supplier_index], db.partsupp[quote_index], db.nation[nation_index], db.region[region_index]),
    ensures rows(db, parameters).len() == 0, query_result(db, parameters, Seq::empty()),
{
    assert forall|part_index: int| 0 <= part_index < db.part.len() implies
        (#[trigger] tuples_1(db, parameters, db.part[part_index])).len() == 0 by {
        let part = db.part[part_index];
        assert forall|supplier_index: int| 0 <= supplier_index < db.supplier.len() implies
            (#[trigger] tuples_2(db, parameters, part, db.supplier[supplier_index])).len() == 0 by {
            let supplier = db.supplier[supplier_index];
            assert forall|quote_index: int| 0 <= quote_index < db.partsupp.len() implies
                (#[trigger] tuples_3(db, parameters, part, supplier, db.partsupp[quote_index])).len() == 0 by {
                let quote = db.partsupp[quote_index];
                assert forall|nation_index: int| 0 <= nation_index < db.nation.len() implies
                    (#[trigger] tuples_4(db, parameters, part, supplier, quote, db.nation[nation_index])).len() == 0 by {
                    let nation = db.nation[nation_index];
                    let children = db.region.map_values(|region: RegionRow| tuples_5(db, parameters, part, supplier, quote, nation, region));
                    assert forall|region_index: int| 0 <= region_index < children.len() implies
                        (#[trigger] children[region_index]).len() == 0 by {
                        assert(!where_clause(db, parameters, part, supplier, quote, nation, db.region[region_index]));
                    }
                    lemma_empty_tuple_flat(children);
                }
                lemma_empty_tuple_flat(db.nation.map_values(|nation: NationRow| tuples_4(db, parameters, part, supplier, quote, nation)));
            }
            lemma_empty_tuple_flat(db.partsupp.map_values(|quote: PartsuppRow| tuples_3(db, parameters, part, supplier, quote)));
        }
        lemma_empty_tuple_flat(db.supplier.map_values(|supplier: SupplierRow| tuples_2(db, parameters, part, supplier)));
    }
    lemma_empty_tuple_flat(db.part.map_values(|part: PartRow| tuples_1(db, parameters, part)));
    assert(rows(db, parameters) =~= Seq::<OutRow>::empty());
    assert(full_result(db, parameters, Seq::empty()));
}

pub proof fn lemma_no_region_result(db: Db, size: int, pat: Seq<u8>, name: Seq<u8>)
    requires forall|index: int| 0 <= index < db.region.len() ==> (#[trigger] db.region[index]).r_name != name,
    ensures query_result(db, params(size, pat, name), Seq::empty()),
{
    lemma_no_query_tuples(db, params(size, pat, name));
}

pub proof fn lemma_no_size_result(db: Db, size: int, pat: Seq<u8>, name: Seq<u8>)
    requires forall|index: int| 0 <= index < db.part.len() ==> !retained(#[trigger] db.part[index]) || db.part[index].p_size != size,
    ensures query_result(db, params(size, pat, name), Seq::empty()),
{
    lemma_no_query_tuples(db, params(size, pat, name));
}

pub proof fn lemma_region_absent(db: Db, regions: crate::storage::Directory<RegionBucket>, name: Seq<u8>)
    requires supported_db(db),
        forall|key: i64| #[trigger] region_amount(regions, name, key) == 0,
        forall|key: i64| #[trigger] region_amount(regions, name, key) == sum(db.region, |row: RegionRow|
            if row.r_name == name && row.r_regionkey == key as int { 1int } else { 0int }),
    ensures forall|index: int| 0 <= index < db.region.len() ==> (#[trigger] db.region[index]).r_name != name,
{
    assert forall|index: int| 0 <= index < db.region.len() implies (#[trigger] db.region[index]).r_name != name by {
        let row = db.region[index];
        if row.r_name == name {
            let key = row.r_regionkey as i64;
            assert(key as int == row.r_regionkey);
            let weight = |entry: RegionRow| if entry.r_name == name && entry.r_regionkey == key as int { 1int } else { 0int };
            assert(sum(db.region, weight) == region_amount(regions, name, key));
            lemma_sum_pos(db.region, weight, index);
        }
    }
}


pub proof fn lemma_build_parts_meaning(db: Db, rows: Seq<BrassPartScan>, ps: Parts)
    requires scan_brass_parts_ok(db, rows), ps.structure_ok(),
        forall|key: i64, size: i64, target: PartRow| #[trigger] ps.amount(key, size, target) == part_scan_amount(rows, key, size, target),
        forall|j: int| 0 <= j < rows.len() ==> ps.dict_has((#[trigger] rows[j]).p_mfgr@) && ps.dict_has(rows[j].p_type@),
    ensures ps.meaning(db),
{
    assert forall|key: i64, size: i64, target: PartRow| #[trigger] ps.amount(key, size, target) == db_amount(db, key, size, target) by {
        lemma_part_amount_transfer(db, rows, key, size, target, ps.amount(key, size, target));
        lemma_sum_ext_all(db.part, |row: PartRow| part_database_weight(row, key, size, target), amount_weight(key, size, target));
    }
    let covered = |row: PartRow| if ps.dict_has(row.p_mfgr) && ps.dict_has(row.p_type) { 0int } else { 1int };
    assert forall|index: int| 0 <= index < brass_part_scan_seq_view(rows).len() implies
        covered(#[trigger] brass_part_scan_seq_view(rows)[index]) == 0 by {
        assert(brass_part_scan_seq_view(rows)[index] == brass_part_scan_view(rows[index]));
    }
    lemma_sum_zero(brass_part_scan_seq_view(rows), covered);
    assert(sum(db.part, covered) == 0);
    assert forall|index: int| 0 <= index < db.part.len() implies
        ps.dict_has((#[trigger] db.part[index]).p_mfgr) && ps.dict_has(db.part[index].p_type) by {
        if !(ps.dict_has(db.part[index].p_mfgr) && ps.dict_has(db.part[index].p_type)) {
            lemma_sum_pos(db.part, covered, index);
        }
    }
}


pub open spec fn routing_after(touched: Seq<i64>, before: Db, after: Db) -> spec_fn(i64, PartsuppRow) -> bool {
    |key: i64, row: PartsuppRow| row.ps_partkey == key as int && (if touched.contains(key) { eligible(after, key) > 0 } else { eligible(before, key) > 0 })
}

pub proof fn lemma_routing_after(touched: Seq<i64>, before: Db, after: Db, quotes: Directory<Bag<BrassPartsuppScan>>)
    requires index_routed(quotes, routing_after(touched, before, after)),
        forall|key: i64| !touched.contains(key) ==> #[trigger] eligible(before, key) == eligible(after, key),
    ensures index_routed(quotes, quote_routing(after)),
{
    assert forall|key: i64| match #[trigger] quotes.lookup(key) {
        None => true,
        Some(bag) => forall|index: int| 0 <= index < (*bag).entries@.len() ==>
            quote_routing(after)(key, (*((#[trigger] (*bag).entries@[index]).row)).rv()),
    } by {
        if let Some(bag) = quotes.lookup(key) {
            assert forall|index: int| 0 <= index < (*bag).entries@.len() implies
                quote_routing(after)(key, (*((#[trigger] (*bag).entries@[index]).row)).rv()) by {
                assert(routing_after(touched, before, after)(key, (*(*bag).entries@[index].row).rv()));
            }
        }
    }
}

}
