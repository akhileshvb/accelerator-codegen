use vstd::prelude::*;
use std::sync::Arc;
use verus_common::sqlfns::like_match;
use verus_common::sums::sum;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::bags::*;
use crate::storage::*;
use crate::parts::*;
use crate::flat::*;

verus! {

pub struct Count { pub value: i128 }

pub struct RegionName {
    pub name: Arc<Vec<u8>>,
    pub keys: Directory<Count>,
}

pub struct RegionBucket { pub names: Vec<RegionName> }

pub open spec fn region_bucket_valid(bucket: RegionBucket, hash: i64) -> bool {
    &&& forall|index: int| 0 <= index < bucket.names@.len() ==>
        text_hash((*((#[trigger] bucket.names@[index]).name))@) == hash as int && bucket.names@[index].keys.valid()
    &&& forall|first: int, second: int| 0 <= first < second < bucket.names@.len() ==>
        (*((#[trigger] bucket.names@[first]).name))@ != (*((#[trigger] bucket.names@[second]).name))@
}

pub open spec fn regions_valid(regions: Directory<RegionBucket>) -> bool {
    &&& regions.valid()
    &&& forall|hash: i64| match #[trigger] regions.lookup(hash) {
        None => true, Some(bucket) => region_bucket_valid(*bucket, hash),
    }
}

pub open spec fn counts_positive(keys: Directory<Count>) -> bool {
    forall|key: i64| match #[trigger] keys.lookup(key) { None => true, Some(count) => (*count).value > 0 }
}

pub open spec fn region_bucket_positive(bucket: RegionBucket) -> bool {
    forall|index: int| 0 <= index < bucket.names@.len() ==> counts_positive((#[trigger] bucket.names@[index]).keys)
}

pub open spec fn regions_positive(regions: Directory<RegionBucket>) -> bool {
    forall|hash: i64| match #[trigger] regions.lookup(hash) { None => true, Some(bucket) => region_bucket_positive(*bucket) }
}

pub open spec fn counts_nonzero(keys: Directory<Count>) -> bool {
    forall|key: i64| match #[trigger] keys.lookup(key) { None => true, Some(count) => (*count).value != 0 }
}

pub open spec fn region_bucket_nonzero(bucket: RegionBucket) -> bool {
    forall|index: int| 0 <= index < bucket.names@.len() ==> counts_nonzero((#[trigger] bucket.names@[index]).keys)
}

pub open spec fn regions_nonzero(regions: Directory<RegionBucket>) -> bool {
    forall|hash: i64| match #[trigger] regions.lookup(hash) { None => true, Some(bucket) => region_bucket_nonzero(*bucket) }
}

/// Every part row is kept (with its type); the LIKE pattern is applied when a query reads.
pub open spec fn retained(row: PartRow) -> bool {
    true
}

pub open spec fn eligible(db: Db, key: i64) -> int {
    sum(db.part, |row: PartRow| if row.p_partkey == key as int && retained(row) { 1 } else { 0 })
}

pub open spec fn blank_part() -> PartRow {
    PartRow { p_partkey: 0, p_mfgr: Seq::empty(), p_type: Seq::empty(), p_size: 0 }
}

pub open spec fn fits_i64(value: int) -> bool {
    i64::MIN as int <= value <= i64::MAX as int
}

pub open spec fn supported_db(db: Db) -> bool {
    &&& forall|index: int| 0 <= index < db.part.len() ==> {
        let row = #[trigger] db.part[index];
        retained(row) ==> fits_i64(row.p_partkey) && fits_i64(row.p_size)
    }
    &&& forall|index: int| 0 <= index < db.partsupp.len() ==> {
        let row = #[trigger] db.partsupp[index];
        scan_brass_partsupp_exists_1(db, row) ==> fits_i64(row.ps_partkey) && fits_i64(row.ps_suppkey) && fits_i64(row.ps_supplycost)
    }
    &&& forall|index: int| 0 <= index < db.supplier.len() ==> {
        let row = #[trigger] db.supplier[index];
        fits_i64(row.s_suppkey) && fits_i64(row.s_nationkey) && fits_i64(row.s_acctbal)
    }
    &&& forall|index: int| 0 <= index < db.nation.len() ==> {
        let row = #[trigger] db.nation[index];
        fits_i64(row.n_nationkey) && fits_i64(row.n_regionkey)
    }
    &&& forall|index: int| 0 <= index < db.region.len() ==> fits_i64((#[trigger] db.region[index]).r_regionkey)
}

pub open spec fn text_hash(bytes: Seq<u8>) -> int
    decreases bytes.len(),
{
    if bytes.len() == 0 { 0 }
    else { (text_hash(bytes.drop_last()) * 257 + bytes.last() as int) % 2147483647 }
}

pub fn hash_bytes(bytes: &[u8]) -> (result: i64)
    ensures result as int == text_hash(bytes@),
{
    let mut value: u64 = 0;
    let mut index = 0;
    while index < bytes.len()
        invariant index <= bytes.len(), value < 2147483647,
            value as int == text_hash(bytes@.take(index as int)),
        decreases bytes.len() - index,
    {
        value = (value * 257 + bytes[index] as u64) % 2147483647;
        proof {
            assert(bytes@.take(index as int + 1).drop_last() =~= bytes@.take(index as int));
            assert(bytes@.take(index as int + 1).last() == bytes@[index as int]);
        }
        index += 1;
    }
    proof { assert(bytes@.take(index as int) =~= bytes@); }
    value as i64
}

pub open spec fn count_at(directory: Directory<Count>, key: i64) -> int {
    match directory.lookup(key) { None => 0, Some(value) => (*value).value as int }
}

pub open spec fn quote_routing(db: Db) -> spec_fn(i64, PartsuppRow) -> bool {
    |key: i64, row: PartsuppRow| row.ps_partkey == key as int && eligible(db, key) > 0
}

pub open spec fn supplier_routing() -> spec_fn(i64, SupplierRow) -> bool {
    |key: i64, row: SupplierRow| row.s_suppkey == key as int
}

pub open spec fn nation_routing() -> spec_fn(i64, NationRow) -> bool {
    |key: i64, row: NationRow| row.n_nationkey == key as int
}

pub open spec fn quote_amount(quotes: Directory<Bag<BrassPartsuppScan>>, key: i64, supplier: int, cost: int) -> int {
    match quotes.lookup(key) {
        None => 0,
        Some(bag) => (*bag).weight(|row: PartsuppRow| if row.ps_suppkey == supplier && row.ps_supplycost == cost { 1int } else { 0int }),
    }
}

pub open spec fn supplier_amount(suppliers: Directory<Bag<SupplierScan>>, key: i64, row: SupplierRow) -> int {
    match suppliers.lookup(key) {
        None => 0,
        Some(bag) => (*bag).amount(row),
    }
}

pub open spec fn nation_amount(nations: Directory<Bag<NationScan>>, key: i64, row: NationRow) -> int {
    match nations.lookup(key) {
        None => 0,
        Some(bag) => (*bag).amount(row),
    }
}

pub open spec fn supplier_row_indicator(value: SupplierRow, key: i64, row: SupplierRow) -> int {
    if value == row && value.s_suppkey == key as int { 1int } else { 0int }
}

pub open spec fn supplier_database_count(db: Db, key: i64, row: SupplierRow) -> int {
    sum(db.supplier, |value: SupplierRow| supplier_row_indicator(value, key, row))
}

pub open spec fn nation_row_indicator(value: NationRow, key: i64, row: NationRow) -> int {
    if value == row && value.n_nationkey == key as int { 1int } else { 0int }
}

pub open spec fn nation_database_count(db: Db, key: i64, row: NationRow) -> int {
    sum(db.nation, |value: NationRow| nation_row_indicator(value, key, row))
}

pub proof fn lemma_supplier_count_weight(db: Db, key: i64, row: SupplierRow, weight: spec_fn(SupplierRow) -> int)
    requires forall|value: SupplierRow| #[trigger] weight(value) == supplier_row_indicator(value, key, row),
    ensures supplier_database_count(db, key, row) == sum(db.supplier, weight),
{
    verus_common::sums::lemma_sum_ext_all(db.supplier,
        |value: SupplierRow| supplier_row_indicator(value, key, row), weight);
}

pub proof fn lemma_nation_count_weight(db: Db, key: i64, row: NationRow, weight: spec_fn(NationRow) -> int)
    requires forall|value: NationRow| #[trigger] weight(value) == nation_row_indicator(value, key, row),
    ensures nation_database_count(db, key, row) == sum(db.nation, weight),
{
    verus_common::sums::lemma_sum_ext_all(db.nation,
        |value: NationRow| nation_row_indicator(value, key, row), weight);
}

pub proof fn lemma_dimension_database_counts(db: Db, suppliers: Directory<Bag<SupplierScan>>, nations: Directory<Bag<NationScan>>)
    requires
        forall|key: i64, row: SupplierRow| #[trigger] supplier_amount(suppliers, key, row) ==
            sum(db.supplier, |value: SupplierRow| if value == row && value.s_suppkey == key as int { 1int } else { 0int }),
        forall|key: i64, row: NationRow| #[trigger] nation_amount(nations, key, row) ==
            sum(db.nation, |value: NationRow| if value == row && value.n_nationkey == key as int { 1int } else { 0int }),
    ensures
        forall|key: i64, row: SupplierRow| #[trigger] supplier_amount(suppliers, key, row) == supplier_database_count(db, key, row),
        forall|key: i64, row: NationRow| #[trigger] nation_amount(nations, key, row) == nation_database_count(db, key, row),
{
    assert forall|key: i64, row: SupplierRow| #[trigger] supplier_amount(suppliers, key, row) == supplier_database_count(db, key, row) by {
        lemma_supplier_count_weight(db, key, row, |value: SupplierRow| if value == row && value.s_suppkey == key as int { 1int } else { 0int });
    }
    assert forall|key: i64, row: NationRow| #[trigger] nation_amount(nations, key, row) == nation_database_count(db, key, row) by {
        lemma_nation_count_weight(db, key, row, |value: NationRow| if value == row && value.n_nationkey == key as int { 1int } else { 0int });
    }
}

pub open spec fn region_amount(regions: Directory<RegionBucket>, name: Seq<u8>, key: i64) -> int {
    match regions.lookup(text_hash(name) as i64) {
        None => 0,
        Some(bucket) => region_names_count((*bucket).names@, name, key),
    }
}

pub open spec fn region_names_count(names: Seq<RegionName>, name: Seq<u8>, key: i64) -> int {
    sum(names, |entry: RegionName| region_name_weight(entry, name, key))
}

pub open spec fn region_name_weight(entry: RegionName, name: Seq<u8>, key: i64) -> int {
    if (*entry.name)@ == name { count_at(entry.keys, key) } else { 0int }
}

pub proof fn lemma_region_names_weight(names: Seq<RegionName>, name: Seq<u8>, key: i64,
    weight: spec_fn(RegionName) -> int)
    requires forall|entry: RegionName| #[trigger] weight(entry) == if (*entry.name)@ == name { count_at(entry.keys, key) } else { 0int },
    ensures region_names_count(names, name, key) == sum(names, weight),
{
    verus_common::sums::lemma_sum_ext_all(names,
        |entry: RegionName| region_name_weight(entry, name, key), weight);
}

pub struct State {
    pub(crate) parts: Parts,
    pub(crate) quotes: Directory<Bag<BrassPartsuppScan>>,
    pub(crate) suppliers: Directory<Bag<SupplierScan>>,
    pub(crate) nations: Directory<Bag<NationScan>>,
    pub(crate) regions: Directory<RegionBucket>,
    pub(crate) sflat: Arc<Vec<SInfo>>,
    pub(crate) database: Ghost<Db>,
}

pub(crate) open spec fn state_meaning(db: Db, parts: Parts,
    quotes: Directory<Bag<BrassPartsuppScan>>, suppliers: Directory<Bag<SupplierScan>>, nations: Directory<Bag<NationScan>>,
    regions: Directory<RegionBucket>, sflat: Arc<Vec<SInfo>>) -> bool
{
    (State { parts, quotes, suppliers, nations, regions, sflat, database: Ghost(db) }).meaning()
}

impl State {
    pub(crate) fn new(parts: Parts,
        quotes: Directory<Bag<BrassPartsuppScan>>, suppliers: Directory<Bag<SupplierScan>>, nations: Directory<Bag<NationScan>>,
        regions: Directory<RegionBucket>, sflat: Arc<Vec<SInfo>>, Ghost(db): Ghost<Db>) -> (result: Self)
        requires state_meaning(db, parts, quotes, suppliers, nations, regions, sflat),
        ensures result.db() == db,
    {
        State { parts, quotes, suppliers, nations, regions, sflat, database: Ghost(db) }
    }
    pub closed spec fn db(self) -> Db { self.database@ }

    pub(crate) open spec fn meaning(self) -> bool {
        let db = self.database@;
        &&& supported_db(db)
        &&& self.quotes.valid() && self.suppliers.valid() && self.nations.valid() && self.regions.valid()
        &&& self.parts.meaning(db)
        &&& flat_ok((*self.sflat)@, self.suppliers)
        &&& forall|key: i64, supplier: int, cost: int| #[trigger] quote_amount(self.quotes, key, supplier, cost) ==
            if eligible(db, key) > 0 {
                sum(db.partsupp, |row: PartsuppRow| if row.ps_partkey == key as int && row.ps_suppkey == supplier && row.ps_supplycost == cost { 1 } else { 0 })
            } else { 0 }
        &&& forall|key: i64, row: SupplierRow| #[trigger] supplier_amount(self.suppliers, key, row) ==
            supplier_database_count(db, key, row)
        &&& forall|key: i64, row: NationRow| #[trigger] nation_amount(self.nations, key, row) ==
            nation_database_count(db, key, row)
        &&& forall|name: Seq<u8>, key: i64| #[trigger] region_amount(self.regions, name, key) ==
            sum(db.region, |row: RegionRow| if row.r_name == name && row.r_regionkey == key as int { 1 } else { 0 })
        &&& forall|key: i64| match #[trigger] self.quotes.lookup(key) {
            None => true,
            Some(bag) => (*bag).valid() && (*bag).entries@.len() > 0 && eligible(db, key) > 0 &&
                forall|index: int| 0 <= index < (*bag).entries@.len() ==> (*((#[trigger] (*bag).entries@[index]).row)).ps_partkey == key,
        }
        &&& forall|key: i64| match #[trigger] self.suppliers.lookup(key) {
            None => true,
            Some(bag) => (*bag).valid() &&
                forall|index: int| 0 <= index < (*bag).entries@.len() ==> (*((#[trigger] (*bag).entries@[index]).row)).s_suppkey == key,
        }
        &&& forall|key: i64| match #[trigger] self.nations.lookup(key) {
            None => true,
            Some(bag) => (*bag).valid() &&
                forall|index: int| 0 <= index < (*bag).entries@.len() ==> (*((#[trigger] (*bag).entries@[index]).row)).n_nationkey == key,
        }
        &&& forall|hash: i64| match #[trigger] self.regions.lookup(hash) {
            None => true,
            Some(bucket) =>
                (forall|index: int| 0 <= index < (*bucket).names@.len() ==>
                    text_hash((*((#[trigger] (*bucket).names@[index]).name))@) == hash as int && (*bucket).names@[index].keys.valid() &&
                    forall|key: i64| match #[trigger] (*bucket).names@[index].keys.lookup(key) {
                        None => true, Some(count) => (*count).value > 0,
                    }) &&
                (forall|first: int, second: int| 0 <= first < second < (*bucket).names@.len() ==>
                    (*((#[trigger] (*bucket).names@[first]).name))@ != (*((#[trigger] (*bucket).names@[second]).name))@),
        }
    }

    #[verifier::type_invariant]
    pub(crate) open spec fn invariant(self) -> bool { self.meaning() }

    pub(crate) proof fn database_witness(&self)
        ensures self.db() == self.database@,
    {}
}

}
