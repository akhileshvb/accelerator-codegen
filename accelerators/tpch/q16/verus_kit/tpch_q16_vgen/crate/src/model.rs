use vstd::prelude::*;
use std::sync::Arc;
use verus_common::rel::*;
use verus_common::sums::*;
use verus_common::sqlfns::*;
use verus_prooflib::ordered::{Index, Key, KeyView};
use crate::gen_spec::*;
use crate::vocab::*;

verus! {

pub struct Group {
    pub brand: Arc<Vec<u8>>,
    pub kind: Arc<Vec<u8>>,
    pub size: i64,
    pub brand_id: u16,
    pub kind_id: u16,
    pub identity: Key,
    pub order: Key,
}

pub struct Cell {
    pub group: Arc<Group>,
    pub count: i128,
    pub brand_id: u16,
    pub kind_id: u16,
    pub size: i64,
}

impl Cell {
    pub open spec fn of(group: Arc<Group>, count: i128) -> Cell {
        Cell { group, count, brand_id: group.brand_id, kind_id: group.kind_id, size: group.size }
    }

    pub open spec fn consistent(self) -> bool {
        self == Cell::of(self.group, self.count)
    }

    pub fn new(group: Arc<Group>, count: i128) -> (cell: Cell)
        ensures cell == Cell::of(group, count),
    {
        let brand_id = group.brand_id;
        let kind_id = group.kind_id;
        let size = group.size;
        Cell { group, count, brand_id, kind_id, size }
    }
}

pub struct PartCell {
    pub group: Arc<Group>,
    pub count: i128,
}

pub type Counts = Index<Cell>;
pub type PartGroups = Index<PartCell>;
pub type Parts = Index<PartGroups>;
pub type Edges = Index<Index<i128>>;
pub struct BrandRanks {
    pub rows: Counts,
}

pub struct SizeRanks {
    pub brands: Index<BrandRanks>,
    pub rows: Counts,
}

pub type Ranks = Index<SizeRanks>;

pub open spec fn escape(bytes: Seq<u8>) -> Seq<u8>
    decreases bytes.len(),
{
    if bytes.len() == 0 { seq![0u8] }
    else { seq![1u8, bytes[0]] + escape(bytes.skip(1)) }
}

pub open spec fn group_key(brand: Seq<u8>, kind: Seq<u8>, size: int) -> KeyView {
    (size, escape(brand) + escape(kind))
}

pub open spec fn size_bytes(size: int) -> Seq<u8> {
    verus_prooflib::be_u64((size - i64::MIN as int) as u64)
}

pub open spec fn pow256(power: int) -> int
    decreases power,
{
    if power <= 0 { 1 } else { 256 * pow256(power - 1) }
}

impl Group {
    pub open spec fn valid(self) -> bool {
        self.identity.view() == self.key()
        && self.brand_id as int == brand_code((*self.brand)@) && self.kind_id as int == type_code((*self.kind)@)
        && self.order.view() == (0int, escape((*self.brand)@) + escape((*self.kind)@) + size_bytes(self.size as int))
    }

    pub open spec fn key(&self) -> KeyView {
        group_key((*self.brand)@, (*self.kind)@, self.size as int)
    }
}

pub open spec fn part_count(db: Db, part: int, brand: Seq<u8>, kind: Seq<u8>, size: int) -> int {
    sum(db.part, |row: PartRow| if row.p_partkey == part && row.p_brand == brand
        && row.p_type == kind && row.p_size == size { 1 } else { 0 })
}

pub open spec fn edge_count(db: Db, part: int, supplier: int) -> int {
    sum(db.partsupp, |row: PartsuppRow| if row.ps_partkey == part && row.ps_suppkey == supplier { 1 } else { 0 })
}

pub open spec fn blocker_count(db: Db, supplier: int) -> int {
    sum(db.supplier, |row: SupplierRow| if row.s_suppkey == supplier && like_match(row.s_comment,
        seq![b'%', b'C', b'u', b's', b't', b'o', b'm', b'e', b'r', b'%', b'C', b'o', b'm', b'p', b'l', b'a', b'i', b'n', b't', b's', b'%']) { 1 } else { 0 })
}

pub open spec fn support_term(db: Db, supplier: int, brand: Seq<u8>, kind: Seq<u8>, size: int, row: PartRow) -> int {
    if row.p_brand == brand && row.p_type == kind && row.p_size == size {
        edge_count(db, row.p_partkey, supplier)
    } else { 0int }
}

pub open spec fn support(db: Db, supplier: int, brand: Seq<u8>, kind: Seq<u8>, size: int) -> int {
    sum(db.part, |row: PartRow| support_term(db, supplier, brand, kind, size, row))
}

pub open spec fn qualifying_supplier(db: Db, brand: Seq<u8>, kind: Seq<u8>, size: int, supplier: int) -> bool {
    support(db, supplier, brand, kind, size) > 0 && blocker_count(db, supplier) == 0
}

pub open spec fn supplier_keys(rows: Seq<PartsuppRow>) -> Seq<int> {
    rows.map_values(|row: PartsuppRow| row.ps_suppkey)
}

pub open spec fn qualified_keys(rows: Seq<PartsuppRow>, db: Db, brand: Seq<u8>, kind: Seq<u8>, size: int) -> Seq<int> {
    supplier_keys(rows.filter(|row: PartsuppRow| qualifying_supplier(db, brand, kind, size, row.ps_suppkey)))
}

pub open spec fn total(db: Db, brand: Seq<u8>, kind: Seq<u8>, size: int) -> int {
    distinct_count(qualified_keys(db.partsupp, db, brand, kind, size))
}

pub open spec fn count_at(counts: Counts, key: KeyView) -> int {
    match counts.at(key) { Some(cell) => cell.count as int, None => 0 }
}

pub open spec fn integer_at(counts: Index<i128>, key: KeyView) -> int {
    match counts.at(key) { Some(value) => value as int, None => 0 }
}

pub open spec fn integer_keys<Value>(index: Index<Value>) -> bool {
    index.wf() && forall|key: KeyView| #[trigger] index.at(key).is_some() ==> key.1 == Seq::<u8>::empty()
}

pub open spec fn part_count_at(groups: PartGroups, key: KeyView) -> int {
    match groups.at(key) { Some(cell) => cell.count as int, None => 0 }
}

pub open spec fn part_at(parts: Parts, part: int, key: KeyView) -> int {
    match parts.at((part, Seq::empty())) {
        Some(groups) => part_count_at(groups, key),
        None => 0,
    }
}

pub open spec fn edge_at(edges: Edges, supplier: int, part: int) -> int {
    match edges.at((supplier, Seq::empty())) {
        Some(parts) => integer_at(parts, (part, Seq::empty())),
        None => 0,
    }
}

pub open spec fn cell_keys_valid(counts: Counts) -> bool {
    counts.wf() && forall|key: KeyView| #[trigger] counts.at(key).is_some() ==>
        counts.at(key).unwrap().group.valid() && counts.at(key).unwrap().group.key() == key
        && counts.at(key).unwrap().consistent()
}

pub open spec fn part_groups_valid(groups: PartGroups) -> bool {
    groups.wf() && forall|key: KeyView| #[trigger] groups.at(key).is_some() ==>
        groups.at(key).unwrap().group.valid() && groups.at(key).unwrap().group.key() == key
}

pub open spec fn parts_keys_valid(parts: Parts) -> bool {
    parts.wf() && integer_keys(parts)
        && forall|key: KeyView| #[trigger] parts.at(key).is_some() ==> part_groups_valid(parts.at(key).unwrap())
}

pub open spec fn edges_keys_valid(edges: Edges) -> bool {
    edges.wf() && integer_keys(edges)
        && forall|key: KeyView| #[trigger] edges.at(key).is_some() ==>
            edges.at(key).unwrap().wf() && integer_keys(edges.at(key).unwrap())
}

pub open spec fn positive_counts(counts: Counts) -> bool {
    cell_keys_valid(counts)
        && forall|key: KeyView| #[trigger] counts.at(key).is_some() ==> counts.at(key).unwrap().count > 0
}

pub open spec fn key_set(keys: Index<i128>) -> bool {
    integer_keys(keys) && forall|key: KeyView| #[trigger] keys.at(key).is_some() ==> keys.at(key).unwrap() == 1
}

pub open spec fn nonzero_integers(keys: Index<i128>) -> bool {
    integer_keys(keys) && forall|key: KeyView| #[trigger] keys.at(key).is_some() ==> keys.at(key).unwrap() != 0
}

pub open spec fn joined_count(parts: Parts, edges: Index<i128>, key: KeyView) -> int {
    sum(edges.entries(), |row: (KeyView, i128)| row.1 as int * part_at(parts, row.0.0, key))
}

pub open spec fn supplier_weight(parts: Parts, blockers: Index<i128>, row: (KeyView, Index<i128>), key: KeyView) -> int {
    if integer_at(blockers, row.0) == 0 && joined_count(parts, row.1, key) > 0 { 1 } else { 0 }
}

pub open spec fn supplier_contribution(parts: Parts, edges: Edges, blockers: Index<i128>, supplier: int, key: KeyView) -> int {
    match edges.at((supplier, Seq::<u8>::empty())) {
        Some(rows) => supplier_weight(parts, blockers, ((supplier, Seq::<u8>::empty()), rows), key),
        None => 0int,
    }
}

pub open spec fn part_weight(row: PartRow, part: int, key: KeyView) -> int {
    if row.p_partkey == part
        && group_key(row.p_brand, row.p_type, row.p_size) == key { 1 } else { 0 }
}

pub open spec fn edge_weight(row: PartsuppRow, supplier: int, part: int) -> int {
    if row.ps_suppkey == supplier && row.ps_partkey == part { 1 } else { 0 }
}

pub open spec fn rank_at(ranks: Ranks, size: int, brand: Seq<u8>, count: int, kind: Seq<u8>) -> Option<i128> {
    match ranks.at((size, Seq::empty())) {
        Some(size_rows) => match size_rows.brands.at((0int, brand)) {
            Some(stream) => match stream.rows.at((-count, kind)) {
                Some(cell) => Some(cell.count),
                None => None,
            },
            None => None,
        },
        None => None,
    }
}

pub open spec fn counts_valid(counts: Counts, db: Db) -> bool {
    &&& counts.wf()
    &&& forall|brand: Seq<u8>, kind: Seq<u8>, size: int|
        #[trigger] count_at(counts, group_key(brand, kind, size)) == total(db, brand, kind, size)
    &&& forall|key: KeyView| #[trigger] counts.at(key).is_some() ==>
        counts.at(key).unwrap().count > 0 && counts.at(key).unwrap().group.key() == key
        && counts.at(key).unwrap().group.valid() && counts.at(key).unwrap().consistent()
}

pub open spec fn ranks_valid(ranks: Ranks, counts: Counts) -> bool {
    &&& ranks.wf()
    &&& positive_counts(counts)
    &&& forall|size: KeyView| #[trigger] ranks.at(size).is_some() ==>
        size.1 == Seq::<u8>::empty() && size_valid(ranks.at(size).unwrap(), counts, size.0)
    &&& forall|key: KeyView| #[trigger] counts.at(key).is_some() ==>
        ranks.at((counts.at(key).unwrap().group.size as int, Seq::empty())).is_some()
}

pub open spec fn stream_matches(group: Group, size: int, brand: Option<Seq<u8>>) -> bool {
    group.size == size && match brand { Some(bytes) => (*group.brand)@ == bytes, None => true }
}

pub open spec fn stream_key(group: Group, count: int, brand: Option<Seq<u8>>) -> KeyView {
    (-count, match brand { Some(_) => (*group.kind)@, None => group.order.view().1 })
}

pub open spec fn stream_valid(rows: Counts, counts: Counts, size: int, brand: Option<Seq<u8>>) -> bool {
    &&& rows.wf()
    &&& forall|key: KeyView| #[trigger] rows.at(key).is_some() ==> {
        let cell = rows.at(key).unwrap();
        stream_matches(*cell.group, size, brand) && key == stream_key(*cell.group, cell.count as int, brand)
            && counts.at(cell.group.key()) == rows.at(key)
    }
    &&& forall|key: KeyView| #[trigger] counts.at(key).is_some()
        && stream_matches(*counts.at(key).unwrap().group, size, brand) ==>
        rows.at(stream_key(*counts.at(key).unwrap().group, counts.at(key).unwrap().count as int, brand)) == counts.at(key)
}

pub open spec fn size_profile_valid(rows: SizeRanks, counts: Counts, size: int) -> bool {
    &&& rows.brands.wf() && stream_valid(rows.rows, counts, size, None)
    &&& brands_valid(rows.brands, counts, size)
}

pub open spec fn brands_valid(brands: Index<BrandRanks>, counts: Counts, size: int) -> bool {
    &&& brands.wf()
    &&& forall|brand: KeyView| #[trigger] brands.at(brand).is_some() ==>
        brand.0 == 0 && stream_valid(brands.at(brand).unwrap().rows, counts, size, Some(brand.1))
        && brands.at(brand).unwrap().rows.entries().len() > 0
    &&& forall|key: KeyView| #[trigger] counts.at(key).is_some() && counts.at(key).unwrap().group.size == size ==>
        brands.at((0int, (*counts.at(key).unwrap().group.brand)@)).is_some()
}

pub open spec fn size_valid(rows: SizeRanks, counts: Counts, size: int) -> bool {
    size_profile_valid(rows, counts, size) && rows.rows.entries().len() > 0
}

}
