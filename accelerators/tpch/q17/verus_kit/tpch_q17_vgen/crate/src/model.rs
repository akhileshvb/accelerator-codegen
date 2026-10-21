use vstd::prelude::*;
use std::sync::Arc;
use verus_common::sums::{sum, ind};
use crate::gen_spec::*;
use crate::storage::*;
use verus_prooflib::wide::Wide;

verus! {

#[derive(Clone, Copy)]
pub struct Moment {
    pub count: i128,
    pub price: i128,
}

pub struct Identity {
    pub brand: Vec<u8>,
    pub container: Vec<u8>,
    pub fingerprint: i64,
}

pub struct Variant {
    pub identity: Arc<Identity>,
    pub multiplicity: Wide,
}

pub struct Joined {
    pub count: Wide,
    pub price: Wide,
}

impl Joined {
    pub fn dup(&self) -> (result: Joined)
        ensures result == *self,
    { Joined { count: self.count.dup(), price: self.price.dup() } }
}

pub struct Group {
    pub identity: Arc<Identity>,
    pub references: Wide,
    pub moment: Joined,
}

#[verifier::opaque]
pub open spec fn hash_bytes(bytes: Seq<u8>, seed: int) -> int
    decreases bytes.len(),
{
    if bytes.len() == 0 { seed } else {
        (hash_bytes(bytes.drop_last(), seed) * 257 + bytes.last() as int + 1) % 1000000007
    }
}

pub open spec fn pair_hash(brand: Seq<u8>, container: Seq<u8>) -> int {
    hash_bytes(container, (hash_bytes(brand, 0) * 257 + 256) % 1000000007)
}

pub open spec fn same(identity: Identity, brand: Seq<u8>, container: Seq<u8>) -> bool {
    identity.brand@ == brand && identity.container@ == container
}

#[verifier::opaque]
pub open spec fn number(rows: Seq<LineitemRow>, key: int) -> int {
    sum(rows, |row: LineitemRow| ind(row.l_partkey == key))
}

#[verifier::opaque]
pub open spec fn quantity(rows: Seq<LineitemRow>, key: int) -> int {
    sum(rows, |row: LineitemRow| if row.l_partkey == key { row.l_quantity } else { 0 })
}

#[verifier::opaque]
pub open spec fn qualifies(rows: Seq<LineitemRow>, row: LineitemRow) -> bool {
    number(rows, row.l_partkey) > 0 && 5 * number(rows, row.l_partkey) * row.l_quantity < quantity(rows, row.l_partkey)
}

#[verifier::opaque]
pub open spec fn line_count(rows: Seq<LineitemRow>, key: int) -> int {
    sum(rows, |row: LineitemRow| ind(row.l_partkey == key && qualifies(rows, row)))
}

#[verifier::opaque]
pub open spec fn line_price(rows: Seq<LineitemRow>, key: int) -> int {
    sum(rows, |row: LineitemRow| if row.l_partkey == key && qualifies(rows, row) { row.l_extendedprice } else { 0 })
}

#[verifier::opaque]
pub open spec fn part_count(rows: Seq<PartRow>, key: int, brand: Seq<u8>, container: Seq<u8>) -> int {
    sum(rows, |row: PartRow| ind(row.p_partkey == key && row.p_brand == brand && row.p_container == container))
}

#[verifier::opaque]
pub open spec fn references(rows: Seq<PartRow>, brand: Seq<u8>, container: Seq<u8>) -> int {
    sum(rows, |row: PartRow| ind(row.p_brand == brand && row.p_container == container))
}

#[verifier::opaque]
pub open spec fn joined_count(db: Db, brand: Seq<u8>, container: Seq<u8>) -> int {
    sum(db.part, |row: PartRow| if row.p_brand == brand && row.p_container == container {
        line_count(db.lineitem, row.p_partkey)
    } else { 0 })
}

#[verifier::opaque]
pub open spec fn joined_price(db: Db, brand: Seq<u8>, container: Seq<u8>) -> int {
    sum(db.part, |row: PartRow| if row.p_brand == brand && row.p_container == container {
        line_price(db.lineitem, row.p_partkey)
    } else { 0 })
}

pub open spec fn moment_at(store: Store<Moment>, key: i64) -> Moment {
    match store.lookup(key) { Some(value) => value, None => Moment { count: 0, price: 0 } }
}

pub open spec fn variants_at(store: Store<Vec<Variant>>, key: i64) -> Seq<Variant> {
    match store.lookup(key) { Some(value) => value@, None => Seq::empty() }
}

#[verifier::opaque]
pub open spec fn variant_total(rows: Seq<Variant>, brand: Seq<u8>, container: Seq<u8>) -> int {
    sum(rows, |row: Variant| if same(*row.identity, brand, container) { row.multiplicity@ } else { 0 })
}

#[verifier::opaque]
pub open spec fn multiplicity(store: Store<Vec<Variant>>, key: i64, brand: Seq<u8>, container: Seq<u8>) -> int {
    variant_total(variants_at(store, key), brand, container)
}

pub open spec fn groups_at(store: Store<Vec<Group>>, fingerprint: i64) -> Seq<Group> {
    match store.lookup(fingerprint) { Some(value) => value@, None => Seq::empty() }
}

pub open spec fn group_value(group: Group, column: nat) -> int {
    if column == 0 { group.references@ } else if column == 1 { group.moment.count@ } else { group.moment.price@ }
}

#[verifier::opaque]
pub open spec fn group_total(groups: Seq<Group>, brand: Seq<u8>, container: Seq<u8>, column: nat) -> int {
    sum(groups, |group: Group| if same(*group.identity, brand, container) { group_value(group, column) } else { 0 })
}

pub open spec fn group_total_at(store: Store<Vec<Group>>, brand: Seq<u8>, container: Seq<u8>, column: nat) -> int {
    group_total(groups_at(store, pair_hash(brand, container) as i64), brand, container, column)
}

#[verifier::opaque]
pub open spec fn group_count_at(store: Store<Vec<Group>>, brand: Seq<u8>, container: Seq<u8>) -> int {
    group_total_at(store, brand, container, 1)
}

#[verifier::opaque]
pub open spec fn group_price_at(store: Store<Vec<Group>>, brand: Seq<u8>, container: Seq<u8>) -> int {
    group_total_at(store, brand, container, 2)
}

#[verifier::opaque]
pub open spec fn group_refs_at(store: Store<Vec<Group>>, brand: Seq<u8>, container: Seq<u8>) -> int {
    group_total_at(store, brand, container, 0)
}

#[verifier::opaque]
pub open spec fn variants_wf(variants: Seq<Variant>) -> bool {
    &&& forall|index: int| #![trigger variants[index]] 0 <= index < variants.len() ==> {
        &&& variants[index].identity.fingerprint as int == pair_hash(variants[index].identity.brand@, variants[index].identity.container@)
    }
    &&& forall|first: int, second: int| #![trigger variants[first], variants[second]]
        0 <= first < second < variants.len() ==> !same(*variants[first].identity, variants[second].identity.brand@, variants[second].identity.container@)
}

#[verifier::opaque]
pub open spec fn groups_wf(groups: Seq<Group>, fingerprint: i64) -> bool {
    &&& forall|index: int| #![trigger groups[index]] 0 <= index < groups.len() ==> {
        &&& groups[index].identity.fingerprint == fingerprint
        &&& fingerprint as int == pair_hash(groups[index].identity.brand@, groups[index].identity.container@)
    }
    &&& forall|first: int, second: int| #![trigger groups[first], groups[second]]
        0 <= first < second < groups.len() ==> !same(*groups[first].identity, groups[second].identity.brand@, groups[second].identity.container@)
}

pub open spec fn part_store_wf(store: Store<Vec<Variant>>) -> bool {
    store.wf() && forall|key: i64| #[trigger] variants_wf(variants_at(store, key))
}

pub open spec fn group_store_wf(store: Store<Vec<Group>>) -> bool {
    store.wf() && forall|hash: i64| #[trigger] groups_wf(groups_at(store, hash), hash)
}

pub proof fn lemma_group_entry(groups: Seq<Group>, hash: i64, index: int)
    requires groups_wf(groups, hash), 0 <= index < groups.len(),
    ensures groups[index].identity.fingerprint == hash,
        hash as int == pair_hash(groups[index].identity.brand@, groups[index].identity.container@),
{ reveal(groups_wf); }

pub proof fn lemma_group_distinct(groups: Seq<Group>, hash: i64, first: int, second: int)
    requires groups_wf(groups, hash), 0 <= first < second < groups.len(),
    ensures !same(*groups[first].identity, groups[second].identity.brand@, groups[second].identity.container@),
{ reveal(groups_wf); }

pub proof fn lemma_variant_entry(variants: Seq<Variant>, index: int)
    requires variants_wf(variants), 0 <= index < variants.len(),
    ensures variants[index].identity.fingerprint as int == pair_hash(variants[index].identity.brand@, variants[index].identity.container@),
{ reveal(variants_wf); }

pub proof fn lemma_empty_groups(hash: i64)
    ensures groups_wf(Seq::empty(), hash),
{ reveal(groups_wf); }

pub proof fn lemma_empty_variants()
    ensures variants_wf(Seq::empty()),
{ reveal(variants_wf); }

pub open spec fn line_model(store: Store<Moment>, rows: Seq<LineitemRow>) -> bool {
    &&& store.wf()
    &&& forall|key: i64| #[trigger] moment_at(store, key).count as int == line_count(rows, key as int)
    &&& forall|key: i64| #[trigger] moment_at(store, key).price as int == line_price(rows, key as int)
    &&& forall|key: i64| match #[trigger] store.lookup(key) { Some(value) => value.count > 0, None => true }
}

pub open spec fn part_model(store: Store<Vec<Variant>>, rows: Seq<PartRow>) -> bool {
    part_store_wf(store) && forall|key: i64, brand: Seq<u8>, container: Seq<u8>| #[trigger] multiplicity(store, key, brand, container)
        == part_count(rows, key as int, brand, container)
}

pub open spec fn group_model(store: Store<Vec<Group>>, db: Db) -> bool {
    &&& group_store_wf(store)
    &&& forall|brand: Seq<u8>, container: Seq<u8>| #[trigger] group_count_at(store, brand, container) == joined_count(db, brand, container)
    &&& forall|brand: Seq<u8>, container: Seq<u8>| #[trigger] group_price_at(store, brand, container) == joined_price(db, brand, container)
    &&& forall|brand: Seq<u8>, container: Seq<u8>| #[trigger] group_refs_at(store, brand, container) == references(db.part, brand, container)
}

#[verifier::opaque]
pub open spec fn part_keys_fit(rows: Seq<PartRow>) -> bool {
    forall|index: int| #![trigger rows[index]] 0 <= index < rows.len() ==> i64::MIN <= rows[index].p_partkey <= i64::MAX
}

pub open spec fn state_meaning(db: Db, lines: Store<Moment>, parts: Store<Vec<Variant>>, groups: Store<Vec<Group>>) -> bool {
    line_model(lines, db.lineitem) && part_model(parts, db.part) && group_model(groups, db) && part_keys_fit(db.part)
}

pub proof fn lemma_part_publication(before: Store<Vec<Variant>>, after: Store<Vec<Variant>>, key: i64, rows: Seq<Variant>)
    requires part_store_wf(before), after.wf(), variants_wf(rows),
        forall|other: i64| #[trigger] variants_at(after, other) == if other == key { rows } else { variants_at(before, other) },
    ensures part_store_wf(after),
        forall|other: i64, brand: Seq<u8>, container: Seq<u8>| #[trigger] multiplicity(after, other, brand, container)
            == if other == key { variant_total(rows, brand, container) } else { multiplicity(before, other, brand, container) },
{
    reveal(multiplicity);
    assert forall|other: i64| #[trigger] variants_wf(variants_at(after, other)) by {
        if other != key { assert(variants_wf(variants_at(before, other))); }
    }
}

pub proof fn lemma_empty_part_store(store: Store<Vec<Variant>>)
    requires store.wf(), forall|key: i64| #[trigger] store.lookup(key).is_none(),
    ensures part_store_wf(store), forall|key: i64, brand: Seq<u8>, container: Seq<u8>| #[trigger] multiplicity(store, key, brand, container) == 0,
{
    assert forall|key: i64| #[trigger] variants_wf(variants_at(store, key)) by {
        assert(store.lookup(key).is_none());
        assert(variants_at(store, key) =~= Seq::empty());
        lemma_empty_variants();
    }
    assert forall|key: i64, brand: Seq<u8>, container: Seq<u8>| #[trigger] multiplicity(store, key, brand, container) == 0 by {
        reveal(multiplicity); reveal(variant_total);
        assert(store.lookup(key).is_none());
    }
}

pub proof fn lemma_empty_group_store(store: Store<Vec<Group>>)
    requires store.wf(), forall|key: i64| #[trigger] store.lookup(key).is_none(),
    ensures group_store_wf(store),
        forall|brand: Seq<u8>, container: Seq<u8>| #[trigger] group_count_at(store, brand, container) == 0,
        forall|brand: Seq<u8>, container: Seq<u8>| #[trigger] group_price_at(store, brand, container) == 0,
        forall|brand: Seq<u8>, container: Seq<u8>| #[trigger] group_refs_at(store, brand, container) == 0,
{
    assert forall|hash: i64| #[trigger] groups_wf(groups_at(store, hash), hash) by {
        assert(store.lookup(hash).is_none());
        assert(groups_at(store, hash) =~= Seq::empty());
        lemma_empty_groups(hash);
    }
    assert forall|brand: Seq<u8>, container: Seq<u8>, column: nat| #[trigger] group_total_at(store, brand, container, column) == 0 by {
        reveal(group_total);
        assert(store.lookup(pair_hash(brand, container) as i64).is_none());
    }
    reveal(group_count_at); reveal(group_price_at); reveal(group_refs_at);
}

pub proof fn lemma_part_fork(before: Store<Vec<Variant>>, after: Store<Vec<Variant>>)
    requires part_store_wf(before), after.wf(), forall|key: i64| #[trigger] after.lookup(key) == before.lookup(key),
    ensures part_store_wf(after), forall|key: i64, brand: Seq<u8>, container: Seq<u8>| #[trigger] multiplicity(after, key, brand, container) == multiplicity(before, key, brand, container),
{
    assert forall|key: i64| #[trigger] variants_at(after, key) == variants_at(before, key) by {}
    assert forall|key: i64| #[trigger] variants_wf(variants_at(after, key)) by { assert(variants_wf(variants_at(before, key))); }
    reveal(multiplicity);
}

pub proof fn lemma_group_fork(before: Store<Vec<Group>>, after: Store<Vec<Group>>)
    requires group_store_wf(before), after.wf(), forall|key: i64| #[trigger] after.lookup(key) == before.lookup(key),
    ensures group_store_wf(after),
        forall|brand: Seq<u8>, container: Seq<u8>| #[trigger] group_count_at(after, brand, container) == group_count_at(before, brand, container),
        forall|brand: Seq<u8>, container: Seq<u8>| #[trigger] group_price_at(after, brand, container) == group_price_at(before, brand, container),
        forall|brand: Seq<u8>, container: Seq<u8>| #[trigger] group_refs_at(after, brand, container) == group_refs_at(before, brand, container),
{
    assert forall|key: i64| #[trigger] groups_at(after, key) == groups_at(before, key) by {}
    assert forall|key: i64| #[trigger] groups_wf(groups_at(after, key), key) by { assert(groups_wf(groups_at(before, key), key)); }
    reveal(group_count_at); reveal(group_price_at); reveal(group_refs_at);
}

pub proof fn lemma_line_fork(before: Store<Moment>, after: Store<Moment>, rows: Seq<LineitemRow>)
    requires line_model(before, rows), after.wf(), forall|key: i64| #[trigger] after.lookup(key) == before.lookup(key),
    ensures line_model(after, rows),
{
    assert forall|key: i64| #[trigger] moment_at(after, key) == moment_at(before, key) by {}
    assert forall|key: i64| match #[trigger] after.lookup(key) { Some(value) => value.count > 0, None => true } by {
        assert(after.lookup(key) == before.lookup(key));
    }
}

pub open spec fn moment_value(store: Store<Moment>, key: i64, price: bool) -> int {
    if price { moment_at(store, key).price as int } else { moment_at(store, key).count as int }
}

pub open spec fn line_value(rows: Seq<LineitemRow>, key: int, price: bool) -> int {
    if price { line_price(rows, key) } else { line_count(rows, key) }
}

pub open spec fn joined_value(db: Db, brand: Seq<u8>, container: Seq<u8>, price: bool) -> int {
    if price { joined_price(db, brand, container) } else { joined_count(db, brand, container) }
}

}
