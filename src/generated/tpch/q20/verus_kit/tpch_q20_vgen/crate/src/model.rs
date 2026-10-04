use vstd::prelude::*;
use verus_common::sums::{sum, rng};
use verus_common::sqlfns::like_match;
use crate::gen_spec::*;
use crate::profiles::*;
use crate::dimensions::*;
use crate::storage::*;

verus! {
pub open spec fn machine_key(key: int) -> bool { i64::MIN as int <= key <= i64::MAX as int }

pub open spec fn domain_valid(db: Db) -> bool {
    &&& forall|index: int| #![trigger db.supplier[index]] 0 <= index < db.supplier.len() ==>
        machine_key(db.supplier[index].s_suppkey) && machine_key(db.supplier[index].s_nationkey)
    &&& forall|index: int| #![trigger db.nation[index]] 0 <= index < db.nation.len() ==>
        machine_key(db.nation[index].n_nationkey)
    &&& forall|index: int| #![trigger db.part[index]] 0 <= index < db.part.len()
        && like_match(db.part[index].p_name, seq![b'g', b'a', b'i', b'n', b's', b'b', b'o', b'r', b'o', b'%']) ==>
            machine_key(db.part[index].p_partkey)
    &&& forall|index: int| #![trigger db.partsupp[index]] 0 <= index < db.partsupp.len()
        && eligible(db, db.partsupp[index].ps_partkey) > 0 ==>
            machine_key(db.partsupp[index].ps_partkey) && machine_key(db.partsupp[index].ps_suppkey)
    &&& forall|index: int| #![trigger db.lineitem[index]] 0 <= index < db.lineitem.len()
        && eligible(db, db.lineitem[index].l_partkey) > 0 ==>
            machine_key(db.lineitem[index].l_partkey) && machine_key(db.lineitem[index].l_suppkey)
}

pub open spec fn eligible(db: Db, part: int) -> int {
    sum(db.part, |row: PartRow| if row.p_partkey == part && like_match(row.p_name, seq![b'g', b'a', b'i', b'n', b's', b'b', b'o', b'r', b'o', b'%']) { 1 } else { 0 })
}
pub open spec fn stock_count(db: Db, part: int, supplier: int, availability: int) -> int {
    sum(db.partsupp, |row: PartsuppRow| if row.ps_partkey == part && row.ps_suppkey == supplier && row.ps_availqty == availability { 1 } else { 0 })
}
pub open spec fn daily_count(db: Db, part: int, supplier: int, date: int) -> int {
    sum(db.lineitem, |row: LineitemRow| if row.l_partkey == part && row.l_suppkey == supplier && row.l_shipdate == date { 1 } else { 0 })
}
pub open spec fn daily_quantity(db: Db, part: int, supplier: int, date: int) -> int {
    sum(db.lineitem, |row: LineitemRow| if row.l_partkey == part && row.l_suppkey == supplier && row.l_shipdate == date { row.l_quantity } else { 0 })
}
pub open spec fn stored_stock(pair: Pair, availability: int) -> int {
    crate::profile_proofs::stock_measure(pair.stocks@, availability)
}
pub open spec fn stored_day_count(pair: Pair, date: int) -> int {
    crate::profile_proofs::ship_measure(pair.ship@, date, false)
}
pub open spec fn stored_day_quantity(pair: Pair, date: int) -> int {
    crate::profile_proofs::ship_measure(pair.ship@, date, true)
}
#[verifier::opaque]
pub open spec fn stored_before_count(pair: Pair, date: int) -> int {
    sum(rng(pair.ship@.len() as int), |index: int| if pair.ship@[index].date < date {
        pair.ship@[index].count as int - if index == 0 { 0 } else { pair.ship@[index - 1].count as int }
    } else { 0 })
}
#[verifier::opaque]
pub open spec fn stored_before_quantity(pair: Pair, date: int) -> int {
    sum(rng(pair.ship@.len() as int), |index: int| if pair.ship@[index].date < date {
        pair.ship@[index].quantity as int - if index == 0 { 0 } else { pair.ship@[index - 1].quantity as int }
    } else { 0 })
}
pub proof fn lemma_before_cut(pair: &Pair, date: int, cutoff: int)
    requires 0 <= cutoff <= pair.ship@.len(),
        forall|index: int| #![trigger pair.ship@[index]] 0 <= index < pair.ship@.len() ==>
            (pair.ship@[index].date < date) == (index < cutoff),
    ensures stored_before_count(*pair, date) == if cutoff == 0 { 0 } else { pair.ship@[cutoff - 1].count as int },
        stored_before_quantity(*pair, date) == if cutoff == 0 { 0 } else { pair.ship@[cutoff - 1].quantity as int },
{
    reveal(stored_before_count);
    reveal(stored_before_quantity);
    let keep = |index: int| pair.ship@[index].date < date;
    let counts = pair.ship@.map_values(|cell: Prefix| cell.count as int);
    let quantities = pair.ship@.map_values(|cell: Prefix| cell.quantity as int);
    assert forall|index: int| #![trigger keep(index)] 0 <= index < pair.ship@.len() implies keep(index) == (index < cutoff) by {
        assert(pair.ship@[index].date < date <==> index < cutoff);
    }
    verus_prooflib::lemma_prefix_cutoff(counts, keep, cutoff, pair.ship@.len() as int);
    verus_prooflib::lemma_prefix_cutoff(quantities, keep, cutoff, pair.ship@.len() as int);
    verus_common::sums::lemma_sum_ext(rng(pair.ship@.len() as int),
        |index: int| if keep(index) { counts[index] - if index == 0 { 0 } else { counts[index - 1] } } else { 0 },
        |index: int| if pair.ship@[index].date < date { pair.ship@[index].count as int - if index == 0 { 0 } else { pair.ship@[index - 1].count as int } } else { 0 });
    verus_common::sums::lemma_sum_ext(rng(pair.ship@.len() as int),
        |index: int| if keep(index) { quantities[index] - if index == 0 { 0 } else { quantities[index - 1] } } else { 0 },
        |index: int| if pair.ship@[index].date < date { pair.ship@[index].quantity as int - if index == 0 { 0 } else { pair.ship@[index - 1].quantity as int } } else { 0 });
}
pub open spec fn part_count(parts: Table<Part>, key: i64) -> int {
    match parts.lookup(key) { Some(part) => part.count as int, None => 0 }
}
pub open spec fn work_part_count(work: Map<i64, PartWork>, key: i64) -> int {
    if work.contains_key(key) { work[key].count as int } else { 0 }
}
pub open spec fn coalesced_part_count(parts: Table<Part>, counts: Map<i64, i128>, key: i64) -> int {
    if counts.contains_key(key) { counts[key] as int } else { part_count(parts, key) }
}
pub open spec fn empty_originals(work: Map<i64, PartWork>) -> bool {
    forall|key: i64| #![trigger work[key]] work.contains_key(key) ==> work[key].original@.is_empty()
}
pub open spec fn work_shape(work: Map<i64, PartWork>) -> bool {
    forall|key: i64| #![trigger work[key]] work.contains_key(key) ==> work[key].shape()
}
pub open spec fn parts_shape(parts: Table<Part>) -> bool {
    forall|key: i64| #![trigger parts.lookup(key)] parts.lookup(key) matches Some(part) ==> crate::profile_proofs::part_shape(part)
}
pub open spec fn work_ready(work: Map<i64, PartWork>, keys: Seq<i64>, parts: Table<Part>, counts: Map<i64, i128>) -> bool {
    work_shape(work) && keys.no_duplicates() && work.dom() == keys.to_set()
        && forall|key: i64| #![trigger work[key]] work.contains_key(key) ==>
            work[key].count > 0 && work[key].count == coalesced_part_count(parts, counts, key)
}
pub open spec fn combined_stock(work: Map<i64, PartWork>, parts: Table<Part>, key: i64, supplier: i64, value: int) -> int {
    if work.contains_key(key) { work[key].stock_for(supplier, value) }
    else { match parts.lookup(key) { Some(part) => crate::profile_proofs::part_stock(part.pairs@, supplier, value), None => 0 } }
}
pub open spec fn combined_day(work: Map<i64, PartWork>, parts: Table<Part>, key: i64, supplier: i64, date: int, quantity: bool) -> int {
    if work.contains_key(key) { work[key].day_for(supplier, date, quantity) }
    else { match parts.lookup(key) { Some(part) => crate::profile_proofs::part_day(part.pairs@, supplier, date, quantity), None => 0 } }
}
pub open spec fn work_stock_count(work: Map<i64, PartWork>, part: i64, supplier: i64, value: int) -> int {
    if !work.contains_key(part) { 0 } else { work[part].stock_for(supplier, value as int) }
}
pub open spec fn work_day_count(work: Map<i64, PartWork>, part: i64, supplier: i64, date: int) -> int {
    if !work.contains_key(part) { 0 } else { work[part].day_for(supplier, date as int, false) }
}
pub open spec fn work_day_quantity(work: Map<i64, PartWork>, part: i64, supplier: i64, date: int) -> int {
    if !work.contains_key(part) { 0 } else { work[part].day_for(supplier, date as int, true) }
}
pub open spec fn pair_of(parts: Table<Part>, part: i64, supplier: i64) -> Option<Pair> {
    match parts.lookup(part) {
        None => None,
        Some(data) => crate::profile_proofs::pair_lookup(data.pairs@, supplier),
    }
}
pub open spec fn supplier_count(state: Suppliers, nation: i64, supplier: i64, name: Seq<u8>, address: Seq<u8>) -> int {
    match state.lookup(nation) { None => 0, Some(bucket) => match bucket.lookup(supplier) {
        None => 0,
        Some(rows) => sum(rows@, |row: SupplierVariant| if row.name@ == name && row.address@ == address { row.count as int } else { 0 }),
    } }
}
pub open spec fn nation_count(state: Nations, name: Seq<u8>, key: i64) -> int {
    match state.lookup(name_hash(name) as i64) { None => 0,
        Some(rows) => sum(rows@, |row: NationVariant| if row.name@ == name && row.key == key { row.count as int } else { 0 }),
    }
}
pub open spec fn dimensions_empty(suppliers: Suppliers, nations: Nations) -> bool {
    (forall|key: i64| #[trigger] suppliers.lookup(key).is_none()) && (forall|key: i64| #[trigger] nations.lookup(key).is_none())
}
pub open spec fn supplier_correct(db: Db, suppliers: Suppliers) -> bool {
    suppliers_positive(suppliers) && forall|nation: i64, supplier: i64, name: Seq<u8>, address: Seq<u8>| #[trigger] supplier_count(suppliers, nation, supplier, name, address) ==
        sum(db.supplier, |row: SupplierRow| if row.s_nationkey == nation && row.s_suppkey == supplier && row.s_name == name && row.s_address == address { 1 } else { 0 })
}
pub open spec fn nation_correct(db: Db, nations: Nations) -> bool {
    nations_positive(nations) && forall|name: Seq<u8>, key: i64| #[trigger] nation_count(nations, name, key) ==
        sum(db.nation, |row: NationRow| if row.n_name == name && row.n_nationkey == key { 1 } else { 0 })
}
pub open spec fn directory_contains(directory: Table<Vec<i64>>, supplier: i64, part: i64) -> bool {
    match directory.lookup(supplier) { None => false, Some(keys) => keys@.contains(part) }
}
pub open spec fn stock_support(rows: Seq<(i64, std::sync::Arc<Pair>)>, supplier: i64) -> bool {
    exists|index: int| #![trigger rows[index]] 0 <= index < rows.len() && rows[index].0 == supplier && rows[index].1.stocks@.len() > 0
}
pub proof fn lemma_stock_support_step(rows: Seq<(i64, std::sync::Arc<Pair>)>, index: int, supplier: i64)
    requires 0 <= index < rows.len(),
    ensures stock_support(rows.take(index + 1), supplier) == (stock_support(rows.take(index), supplier)
        || rows[index].0 == supplier && rows[index].1.stocks@.len() > 0),
{
    if stock_support(rows.take(index + 1), supplier) {
        let offset = choose|offset: int| #![trigger rows.take(index + 1)[offset]] 0 <= offset < index + 1
            && rows.take(index + 1)[offset].0 == supplier && rows.take(index + 1)[offset].1.stocks@.len() > 0;
        if offset < index { assert(rows.take(index)[offset] == rows.take(index + 1)[offset]); }
    }
    if stock_support(rows.take(index), supplier) {
        let offset = choose|offset: int| #![trigger rows.take(index)[offset]] 0 <= offset < index
            && rows.take(index)[offset].0 == supplier && rows.take(index)[offset].1.stocks@.len() > 0;
        assert(rows.take(index + 1)[offset] == rows.take(index)[offset]);
    }
    if rows[index].0 == supplier && rows[index].1.stocks@.len() > 0 { assert(rows.take(index + 1)[index] == rows[index]); }
}
pub open spec fn directory_unique(directory: Table<Vec<i64>>) -> bool {
    forall|supplier: i64| #![trigger directory.lookup(supplier)] directory.lookup(supplier) matches Some(keys) ==> keys@.no_duplicates()
}
pub open spec fn directory_matches(parts: Table<Part>, directory: Table<Vec<i64>>) -> bool {
    directory_unique(directory) && forall|supplier: i64, part: i64| #![trigger directory_contains(directory, supplier, part)]
        directory_contains(directory, supplier, part) == match parts.lookup(part) { Some(data) => stock_support(data.pairs@, supplier), None => false }
}
pub open spec fn part_correct(db: Db, key: i64, part: Option<Part>) -> bool {
    match part {
        None => eligible(db, key as int) == 0,
        Some(data) => data.count == eligible(db, key as int) && data.count > 0 && crate::profile_proofs::part_shape(data)
            && (forall|supplier: i64, value: int| #![trigger crate::profile_proofs::part_stock(data.pairs@, supplier, value)]
                crate::profile_proofs::part_stock(data.pairs@, supplier, value) == stock_count(db, key as int, supplier as int, value))
            && (forall|supplier: i64, date: int| #![trigger crate::profile_proofs::part_day(data.pairs@, supplier, date, false)]
                crate::profile_proofs::part_day(data.pairs@, supplier, date, false) == daily_count(db, key as int, supplier as int, date)
                && crate::profile_proofs::part_day(data.pairs@, supplier, date, true) == daily_quantity(db, key as int, supplier as int, date)),
    }
}
#[verifier::opaque]
pub open spec fn pair_valid(pair: Pair) -> bool {
    &&& forall|index: int| #![trigger pair.stocks@[index]]
        0 <= index < pair.stocks@.len() ==> pair.stocks@[index].count > 0
            && (index > 0 ==> pair.stocks@[index - 1].value < pair.stocks@[index].value)
    &&& forall|index: int| #![trigger pair.ship@[index]]
        0 <= index < pair.ship@.len() ==> pair.ship@[index].count > (if index == 0 { 0 } else { pair.ship@[index - 1].count })
            && (index > 0 ==> pair.ship@[index - 1].date < pair.ship@[index].date)
}
pub proof fn lemma_pair_dates(pair: &Pair)
    ensures pair_valid(*pair) ==> forall|index: int| #![trigger pair.ship@[index]] 0 < index < pair.ship@.len() ==>
        pair.ship@[index - 1].date < pair.ship@[index].date,
{
    reveal(pair_valid);
}
pub proof fn lemma_pair_order(pair: &Pair)
    ensures pair_valid(*pair) ==>
        verus_common::order::sorted_by(pair.stocks@, |left: StockCell, right: StockCell| left.value < right.value)
            && verus_common::order::sorted_by(pair.ship@, |left: Prefix, right: Prefix| left.date < right.date),
{
    reveal(pair_valid);
    if pair_valid(*pair) {
        assert forall|left: int, right: int| 0 <= left < right < pair.stocks@.len() implies
            (#[trigger] pair.stocks@[left]).value < (#[trigger] pair.stocks@[right]).value by {
            verus_prooflib::lemma_adjacent_int_order(pair.stocks@, |cell: StockCell| cell.value as int, left, right);
        }
        assert forall|left: int, right: int| 0 <= left < right < pair.ship@.len() implies
            (#[trigger] pair.ship@[left]).date < (#[trigger] pair.ship@[right]).date by {
            verus_prooflib::lemma_adjacent_int_order(pair.ship@, |cell: Prefix| cell.date as int, left, right);
        }
    }
}
pub open spec fn read_shape(parts: Table<Part>) -> bool {
    &&& forall|key: i64| #![trigger parts.lookup(key)] parts.lookup(key) matches Some(part) ==>
        part.pairs@.map_values(|entry: (i64, std::sync::Arc<Pair>)| entry.0).no_duplicates()
    &&& forall|part: i64, supplier: i64| #![trigger pair_of(parts, part, supplier)]
        pair_of(parts, part, supplier) matches Some(pair) ==> pair_valid(pair)
}
pub open spec fn cached_pair_passes(pair: Pair, start: int, end: int) -> bool {
    pair.stocks@.len() > 0
        && stored_before_count(pair, end) - stored_before_count(pair, start) > 0
        && 200 * pair.stocks@[pair.stocks@.len() - 1].value as int
            > stored_before_quantity(pair, end) - stored_before_quantity(pair, start)
}
pub open spec fn cached_part_passes(parts: Table<Part>, part: i64, supplier: i64, start: int, end: int) -> bool {
    match pair_of(parts, part, supplier) { None => false, Some(pair) => cached_pair_passes(pair, start, end) }
}
pub open spec fn cached_supplier_passes(parts: Table<Part>, directory: Table<Vec<i64>>, supplier: i64, start: int, end: int) -> bool {
    exists|part: i64| #![trigger cached_part_passes(parts, part, supplier, start, end)]
        directory_contains(directory, supplier, part) && cached_part_passes(parts, part, supplier, start, end)
}
pub proof fn lemma_pair_at(parts: Table<Part>, key: i64, supplier: i64, part: &Part, index: int)
    requires parts.lookup(key) == Some(*part),
        part.pairs@.map_values(|entry: (i64, std::sync::Arc<Pair>)| entry.0).no_duplicates(),
        0 <= index < part.pairs@.len(), part.pairs@[index].0 == supplier,
    ensures pair_of(parts, key, supplier) == Some(*part.pairs@[index].1),
{
    verus_prooflib::lemma_unique_key_lookup(part.pairs@, supplier);
    assert(verus_prooflib::unique_key_lookup(part.pairs@, supplier) == Some(part.pairs@[index].1));
}
pub open spec fn meaning(db: Db, parts: Table<Part>, directory: Table<Vec<i64>>, suppliers: Suppliers, nations: Nations) -> bool {
    &&& parts.well_formed() && directory.well_formed() && suppliers.well_formed() && nations.well_formed()
    &&& forall|key: i64| #![trigger part_count(parts, key)] part_count(parts, key) == eligible(db, key as int)
    &&& forall|key: i64| #![trigger parts.lookup(key)] parts.lookup(key) matches Some(part) ==>
        part.count > 0 && part.pairs@.map_values(|entry: (i64, std::sync::Arc<Pair>)| entry.0).no_duplicates()
    &&& forall|part: i64, supplier: i64| #![trigger pair_of(parts, part, supplier)]
        match pair_of(parts, part, supplier) {
            Some(pair) => pair_valid(pair) && eligible(db, part as int) > 0
                && (pair.stocks@.len() > 0 || pair.ship@.len() > 0)
                && (forall|index: int| #![trigger pair.ship@[index]]
                    0 <= index < pair.ship@.len() ==>
                        pair.ship@[index].count == sum(db.lineitem, |row: LineitemRow| if row.l_partkey == part && row.l_suppkey == supplier && row.l_shipdate <= pair.ship@[index].date { 1 } else { 0 })
                        && pair.ship@[index].quantity == sum(db.lineitem, |row: LineitemRow| if row.l_partkey == part && row.l_suppkey == supplier && row.l_shipdate <= pair.ship@[index].date { row.l_quantity } else { 0 }))
                && forall|value: int| #![trigger stored_stock(pair, value)]
                    stored_stock(pair, value) == stock_count(db, part as int, supplier as int, value)
                && forall|date: int| #![trigger stored_day_count(pair, date)]
                    stored_day_count(pair, date) == daily_count(db, part as int, supplier as int, date)
                    && stored_day_quantity(pair, date) == daily_quantity(db, part as int, supplier as int, date),
            None => eligible(db, part as int) > 0 ==> (
                (forall|value: int| #![trigger stock_count(db, part as int, supplier as int, value)] stock_count(db, part as int, supplier as int, value) == 0)
                && (forall|date: int| #![trigger daily_count(db, part as int, supplier as int, date)] daily_count(db, part as int, supplier as int, date) == 0)),
        }
    &&& forall|supplier: i64, part: i64| #![trigger directory_contains(directory, supplier, part)]
        directory_contains(directory, supplier, part) == match pair_of(parts, part, supplier) { Some(pair) => pair.stocks@.len() > 0, None => false }
    &&& forall|supplier: i64| #![trigger directory.lookup(supplier)]
        directory.lookup(supplier) matches Some(keys) ==> keys@.no_duplicates()
    &&& forall|nation: i64| #![trigger suppliers.lookup(nation)]
        suppliers.lookup(nation) matches Some(bucket) ==> bucket.well_formed()
            && forall|supplier: i64| #![trigger bucket.lookup(supplier)]
                bucket.lookup(supplier) matches Some(rows) ==>
                    forall|index: int| #![trigger rows@[index]] 0 <= index < rows@.len() ==> rows@[index].count > 0
    &&& forall|nation: i64, supplier: i64, name: Seq<u8>, address: Seq<u8>| #![trigger supplier_count(suppliers, nation, supplier, name, address)]
        supplier_count(suppliers, nation, supplier, name, address) == sum(db.supplier, |row: SupplierRow| if row.s_nationkey == nation && row.s_suppkey == supplier && row.s_name == name && row.s_address == address { 1 } else { 0 })
    &&& forall|name: Seq<u8>, key: i64| #![trigger nation_count(nations, name, key)]
        nation_count(nations, name, key) == sum(db.nation, |row: NationRow| if row.n_name == name && row.n_nationkey == key { 1 } else { 0 })
    &&& forall|hash: i64| #![trigger nations.lookup(hash)]
        nations.lookup(hash) matches Some(rows) ==>
            forall|index: int| #![trigger rows@[index]] 0 <= index < rows@.len() ==>
                rows@[index].count > 0 && name_hash(rows@[index].name@) == hash
}
}
