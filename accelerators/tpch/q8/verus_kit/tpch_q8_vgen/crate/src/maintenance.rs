use vstd::prelude::*;
use vstd::hash_map::HashMapWithView as HashMap;
use verus_common::sums::*;
use crate::table::*;
use crate::gen_obligations::*;
use crate::compound_map::CompoundMap;
pub use crate::contributions::line_contribution;
pub use crate::routing::{route_nation, route_pair};

verus! {

broadcast use { vstd::std_specs::hash::group_hash_axioms, vstd::seq_lib::group_seq_properties };

pub struct Keys {
    pub map: HashMap<i64, bool>,
    pub values: Vec<i64>,
}

impl Keys {
    pub open spec fn has(self, key: i64) -> bool { self.map@.contains_key(key) }
    pub open spec fn valid(self) -> bool {
        self.values@.no_duplicates()
        && forall|key: i64| #[trigger] self.has(key) == self.values@.contains(key)
    }
    pub fn new() -> (result: Self)
        ensures result.valid(), result.values@ == Seq::<i64>::empty(),
            forall|key: i64| !#[trigger] result.has(key),
    { Keys { map: HashMap::new(), values: Vec::new() } }
    pub fn add(&mut self, key: i64)
        ensures old(self).valid() ==> final(self).valid(),
            forall|other: i64| #[trigger] final(self).has(other) == (old(self).has(other) || other == key),
            final(self).values@ == if old(self).has(key) { old(self).values@ } else { old(self).values@.push(key) },
    {
        let ghost before = *self;
        if !self.map.contains_key(&key) { self.map.insert(key, true); self.values.push(key); }
        proof {
            assert(self.map@ == if before.has(key) { before.map@ } else { before.map@.insert(key, true) });
            assert(self.values@ == if before.has(key) { before.values@ } else { before.values@.push(key) });
            if before.valid() && !before.has(key) {
                verus_prooflib::lemma_unique_push(before.values@, key);
            }
            if before.valid() {
                assert forall|other: i64| #[trigger] self.has(other) == self.values@.contains(other) by {
                    assert(before.has(other) == before.values@.contains(other));
                    assert(self.has(other) == (before.has(other) || other == key));
                    if !before.has(key) { verus_prooflib::lemma_push_membership(before.values@, key); }
                }
            }
        }
    }
    pub fn contains(&self, key: i64) -> (result: bool)
        ensures result == self.has(key),
    { self.map.contains_key(&key) }
    pub fn covered(&self, supplied: &[i64]) -> (result: bool)
        requires self.valid(),
        ensures result == (forall|key: i64| #[trigger] self.has(key) ==> supplied@.contains(key)),
    {
        let mut delivered = Keys::new();
        let mut index = 0;
        while index < supplied.len()
            invariant index <= supplied.len(), delivered.valid(),
                forall|key: i64| #[trigger] delivered.has(key) == supplied@.take(index as int).contains(key),
            decreases supplied.len() - index,
        {
            delivered.add(supplied[index]);
            proof { assert(supplied@.take(index as int + 1) =~= supplied@.take(index as int).push(supplied@[index as int])); }
            index += 1;
        }
        proof { assert(supplied@.take(supplied@.len() as int) =~= supplied@); }
        index = 0;
        while index < self.values.len()
            invariant index <= self.values.len(), self.valid(), delivered.valid(),
                forall|key: i64| #[trigger] delivered.has(key) == supplied@.contains(key),
                forall|offset: int| 0 <= offset < index ==> delivered.has(#[trigger] self.values@[offset]),
            decreases self.values.len() - index,
        {
            if !delivered.contains(self.values[index]) {
                proof {
                    assert(self.values@.contains(self.values@[index as int]));
                    assert(self.has(self.values@[index as int]));
                    assert(!supplied@.contains(self.values@[index as int]));
                }
                return false;
            }
            index += 1;
        }
        proof {
            assert forall|key: i64| #[trigger] self.has(key) implies supplied@.contains(key) by {
                let offset = choose|offset: int| 0 <= offset < self.values@.len() && #[trigger] self.values@[offset] == key;
            }
        }
        true
    }
}

pub struct Pairs {
    pub map: CompoundMap<bool>,
    pub values: Vec<Major>,
}

impl Pairs {
    pub open spec fn has(self, key: Major) -> bool { self.map@.contains_key(key) }
    pub open spec fn valid(self) -> bool {
        self.values@.no_duplicates()
        && forall|key: Major| #[trigger] self.has(key) == self.values@.contains(key)
    }
    pub fn new() -> (result: Self)
        ensures result.valid(), result.values@ == Seq::<Major>::empty(),
            forall|key: Major| !#[trigger] result.has(key),
    { Pairs { map: CompoundMap::new(), values: Vec::new() } }
    pub fn add(&mut self, key: Major)
        ensures old(self).valid() ==> final(self).valid(),
            forall|other: Major| #[trigger] final(self).has(other) == (old(self).has(other) || other == key),
            final(self).values@ == if old(self).has(key) { old(self).values@ } else { old(self).values@.push(key) },
    {
        let ghost before = *self;
        if !self.map.contains_key(&key) { self.map.insert(key, true); self.values.push(key); }
        proof {
            assert(self.map@ == if before.has(key) { before.map@ } else { before.map@.insert(key, true) });
            assert(self.values@ == if before.has(key) { before.values@ } else { before.values@.push(key) });
            if before.valid() && !before.has(key) { verus_prooflib::lemma_unique_push(before.values@, key); }
            if before.valid() {
                assert forall|other: Major| #[trigger] self.has(other) == self.values@.contains(other) by {
                    assert(before.has(other) == before.values@.contains(other));
                    assert(self.has(other) == (before.has(other) || other == key));
                    if !before.has(key) { verus_prooflib::lemma_push_membership(before.values@, key); }
                }
            }
        }
    }
}

pub fn include_region_nations(reverse: &Table, region: i64, keys: &mut Keys)
    requires reverse.valid(), old(keys).valid(),
    ensures final(keys).valid(),
        forall|key: i64| #[trigger] old(keys).has(key) ==> final(keys).has(key),
        forall|index: int| 0 <= index < reverse.rows((region, 0, 0)).len() ==>
            final(keys).has((#[trigger] reverse.rows((region, 0, 0))[index]).first),
{
    let ghost before = *keys;
    let rows = reverse.get((region, 0, 0));
    let mut index = 0;
    while index < rows.len()
        invariant index <= rows.len(), keys.valid(), (*rows)@ == reverse.rows((region, 0, 0)),
            forall|key: i64| #[trigger] before.has(key) ==> keys.has(key),
            forall|offset: int| 0 <= offset < index ==> keys.has((#[trigger] (*rows)@[offset]).first),
        decreases rows.len() - index,
    {
        keys.add(rows[index].first);
        index += 1;
    }
}

pub fn include_table_pairs(changes: &Table, pairs: &mut Pairs)
    requires changes.valid(), old(pairs).valid(),
    ensures final(pairs).valid(),
        forall|key: Major| #[trigger] old(pairs).has(key) ==> final(pairs).has(key),
        forall|key: Major| #[trigger] changes.keys_view().contains(key) ==> final(pairs).has(key),
{
    let ghost before = *pairs;
    let keys = changes.keys();
    let mut index = 0;
    while index < keys.len()
        invariant index <= keys.len(), pairs.valid(), keys@ == changes.keys_view(),
            forall|key: Major| #[trigger] before.has(key) ==> pairs.has(key),
            forall|offset: int| 0 <= offset < index ==> pairs.has(#[trigger] keys@[offset]),
        decreases keys.len() - index,
    {
        pairs.add(keys[index]);
        index += 1;
    }
    proof {
        assert forall|key: Major| #[trigger] keys@.contains(key) implies pairs.has(key) by {
            let offset = choose|offset: int| 0 <= offset < keys@.len() && #[trigger] keys@[offset] == key;
        }
    }
}

pub open spec fn reverse_pairs_covered(pairs: Pairs, keys: Keys, reverse: Table, limit: int, supplier: bool) -> bool {
    forall|index: int, offset: int| 0 <= index < limit && index < keys.values@.len()
        && 0 <= offset < reverse.rows((keys.values@[index], 0, 0)).len() ==>
        pairs.has(if supplier { ((#[trigger] reverse.rows((keys.values@[index], 0, 0))[offset]).first, keys.values@[index], 0) }
            else { (keys.values@[index], (#[trigger] reverse.rows((keys.values@[index], 0, 0))[offset]).first, 0) })
}

pub fn include_nation_pairs(reverse: &Table, nation: i64, supplier: bool, pairs: &mut Pairs)
    requires reverse.valid(), old(pairs).valid(),
    ensures final(pairs).valid(),
        forall|key: Major| #[trigger] old(pairs).has(key) ==> final(pairs).has(key),
        forall|index: int| 0 <= index < reverse.rows((nation, 0, 0)).len() ==>
            final(pairs).has(if supplier { ((#[trigger] reverse.rows((nation, 0, 0))[index]).first, nation, 0) }
                else { (nation, (#[trigger] reverse.rows((nation, 0, 0))[index]).first, 0) }),
{
    let ghost before = *pairs;
    let rows = reverse.get((nation, 0, 0));
    let mut index = 0;
    while index < rows.len()
        invariant index <= rows.len(), pairs.valid(), (*rows)@ == reverse.rows((nation, 0, 0)),
            forall|key: Major| #[trigger] before.has(key) ==> pairs.has(key),
            forall|offset: int| 0 <= offset < index ==>
                pairs.has(if supplier { ((#[trigger] (*rows)@[offset]).first, nation, 0) }
                    else { (nation, (#[trigger] (*rows)@[offset]).first, 0) }),
        decreases rows.len() - index,
    {
        if supplier { pairs.add((rows[index].first, nation, 0)); }
        else { pairs.add((nation, rows[index].first, 0)); }
        index += 1;
    }
}

pub proof fn lemma_reverse_pairs_step(before: Pairs, after: Pairs, keys: Keys, reverse: Table, index: int, supplier: bool)
    requires 0 <= index < keys.values@.len(), reverse_pairs_covered(before, keys, reverse, index, supplier),
        forall|key: Major| #[trigger] before.has(key) ==> after.has(key),
        forall|offset: int| 0 <= offset < reverse.rows((keys.values@[index], 0, 0)).len() ==>
            after.has(if supplier { ((#[trigger] reverse.rows((keys.values@[index], 0, 0))[offset]).first, keys.values@[index], 0) }
                else { (keys.values@[index], (#[trigger] reverse.rows((keys.values@[index], 0, 0))[offset]).first, 0) }),
    ensures reverse_pairs_covered(after, keys, reverse, index + 1, supplier),
{
    assert forall|earlier: int, offset: int| 0 <= earlier < index + 1 && earlier < keys.values@.len()
        && 0 <= offset < reverse.rows((keys.values@[earlier], 0, 0)).len() implies
        after.has(if supplier { ((#[trigger] reverse.rows((keys.values@[earlier], 0, 0))[offset]).first, keys.values@[earlier], 0) }
            else { (keys.values@[earlier], (#[trigger] reverse.rows((keys.values@[earlier], 0, 0))[offset]).first, 0) }) by {
        if earlier < index {
            let pair = if supplier { (reverse.rows((keys.values@[earlier], 0, 0))[offset].first, keys.values@[earlier], 0i64) }
                else { (keys.values@[earlier], reverse.rows((keys.values@[earlier], 0, 0))[offset].first, 0i64) };
            assert(before.has(pair));
        } else { assert(earlier == index); }
    }
}

pub fn order_keys(reverse: &Table, customers: &[CustomerDelta], orders: &[OrderDelta]) -> (result: Keys)
    requires reverse.valid(),
    ensures result.valid(),
        forall|customer_index: int, offset: int| 0 <= customer_index < customers@.len()
            && 0 <= offset < reverse.rows((customers@[customer_index].row.c_custkey, 0, 0)).len() ==>
            result.has((#[trigger] reverse.rows((customers@[customer_index].row.c_custkey, 0, 0))[offset]).first),
        forall|index: int| 0 <= index < orders@.len() && ((#[trigger] orders@[index]).in_1995 != 0
            || orders@[index].in_1996 != 0) ==> result.has(orders@[index].o_orderkey),
{
    let mut keys = Keys::new();
    let mut index = 0;
    let ghost customer_rows = customers@;
    while index < customers.len()
        invariant index <= customers.len(), reverse.valid(), keys.valid(),
            forall|customer_index: int, offset: int| 0 <= customer_index < index
                && 0 <= offset < reverse.rows((customers@[customer_index].row.c_custkey, 0, 0)).len() ==>
                keys.has((#[trigger] reverse.rows((customers@[customer_index].row.c_custkey, 0, 0))[offset]).first),
        decreases customers.len() - index,
    {
        let ghost processed = customer_rows.take(index as int);
        let rows = reverse.get((customers[index].row.c_custkey, 0, 0));
        let mut offset = 0;
        while offset < rows.len()
            invariant reverse.valid(), keys.valid(), index < customers.len(), offset <= rows.len(),
                (*rows)@ == reverse.rows((customers@[index as int].row.c_custkey, 0, 0)),
                forall|customer_index: int, earlier: int| 0 <= customer_index < index
                    && 0 <= earlier < reverse.rows((customers@[customer_index].row.c_custkey, 0, 0)).len() ==>
                    keys.has((#[trigger] reverse.rows((customers@[customer_index].row.c_custkey, 0, 0))[earlier]).first),
                forall|earlier: int| 0 <= earlier < offset ==> keys.has((#[trigger] (*rows)@[earlier]).first),
            decreases rows.len() - offset,
        { keys.add(rows[offset].first); offset += 1; }
        index += 1;
    }
    index = 0;
    let ghost order_rows = orders@;
    while index < orders.len()
        invariant index <= orders.len(), reverse.valid(), keys.valid(),
            forall|customer_index: int, offset: int| 0 <= customer_index < customers@.len()
                && 0 <= offset < reverse.rows((customers@[customer_index].row.c_custkey, 0, 0)).len() ==>
                keys.has((#[trigger] reverse.rows((customers@[customer_index].row.c_custkey, 0, 0))[offset]).first),
            forall|earlier: int| 0 <= earlier < index && ((#[trigger] orders@[earlier]).in_1995 != 0
                || orders@[earlier].in_1996 != 0) ==> keys.has(orders@[earlier].o_orderkey),
        decreases orders.len() - index,
    {
        let ghost processed = order_rows.take(index as int);
        if orders[index].in_1995 != 0 || orders[index].in_1996 != 0 { keys.add(orders[index].o_orderkey); }
        index += 1;
    }
    keys
}

pub fn part_keys(parts: &Vec<PartDelta>) -> (result: Keys)
    ensures result.valid(),
        forall|index: int| 0 <= index < parts@.len() ==> result.has((#[trigger] parts@[index]).row.p_partkey),
{
    let mut keys = Keys::new();
    let mut index = 0;
    let ghost input = parts@;
    while index < parts.len()
        invariant index <= parts.len(), keys.valid(),
            forall|earlier: int| 0 <= earlier < index ==> keys.has((#[trigger] parts@[earlier]).row.p_partkey),
        decreases parts.len() - index,
    {
        let ghost processed = input.take(index as int);
        keys.add(parts[index].row.p_partkey); index += 1;
    }
    keys
}

pub fn supplier_keys(suppliers: &[SupplierDelta]) -> (result: Keys)
    ensures result.valid(),
        forall|index: int| 0 <= index < suppliers@.len() ==> result.has((#[trigger] suppliers@[index]).row.s_suppkey),
{
    let mut keys = Keys::new();
    let mut index = 0;
    let ghost input = suppliers@;
    while index < suppliers.len()
        invariant index <= suppliers.len(), keys.valid(),
            forall|earlier: int| 0 <= earlier < index ==> keys.has((#[trigger] suppliers@[earlier]).row.s_suppkey),
        decreases suppliers.len() - index,
    {
        let ghost processed = input.take(index as int);
        keys.add(suppliers[index].row.s_suppkey); index += 1;
    }
    keys
}

pub fn index_pairs(changes: &Table, customer_index: &mut Work, supplier_index: &mut Work) -> (result: Option<()>)
    requires changes.valid(), old(customer_index).valid(), old(supplier_index).valid(),
    ensures result is Some ==> final(customer_index).valid() && final(supplier_index).valid(),
        result is Some ==> forall|key: Major, minor: Major, measure: bool| #[trigger] final(customer_index).at(key, minor, measure)
            == old(customer_index).at(key, minor, measure) + pair_index_effect(changes.keys_view(), *changes, key, minor, false, measure),
        result is Some ==> forall|key: Major, minor: Major, measure: bool| #[trigger] final(supplier_index).at(key, minor, measure)
            == old(supplier_index).at(key, minor, measure) + pair_index_effect(changes.keys_view(), *changes, key, minor, true, measure),
{
    let keys = changes.keys();
    let ghost initial_customer = *customer_index;
    let ghost initial_supplier = *supplier_index;
    let mut index = 0;
    proof { reveal(pair_index_effect); }
    while index < keys.len()
        invariant index <= keys.len(), changes.valid(), keys@ == changes.keys_view(), customer_index.valid(), supplier_index.valid(),
            forall|key: Major, minor: Major, measure: bool| #[trigger] customer_index.at(key, minor, measure)
                == initial_customer.at(key, minor, measure) + pair_index_effect(keys@.take(index as int), *changes, key, minor, false, measure),
            forall|key: Major, minor: Major, measure: bool| #[trigger] supplier_index.at(key, minor, measure)
                == initial_supplier.at(key, minor, measure) + pair_index_effect(keys@.take(index as int), *changes, key, minor, true, measure),
        decreases keys.len() - index,
    {
        let key = keys[index];
        let rows = changes.get(key);
        let mut offset = 0;
        let mut count: i128 = 0;
        while offset < rows.len()
            invariant offset <= rows.len(), count as int == sum((*rows)@.take(offset as int), |cell: Cell| cell.count as int),
            decreases rows.len() - offset,
        {
            count = count.checked_add(rows[offset].count)?;
            proof { lemma_sum_take((*rows)@, offset as int, |cell: Cell| cell.count as int); }
            offset += 1;
        }
        proof { assert((*rows)@.take((*rows)@.len() as int) =~= (*rows)@); }
        customer_index.add((key.0, 0, 0), (key.1, 0, 0), count, 0)?;
        supplier_index.add((key.1, 0, 0), (key.0, 0, 0), count, 0)?;
        proof {
            assert forall|target: Major, minor: Major, measure: bool| #[trigger] customer_index.at(target, minor, measure)
                == initial_customer.at(target, minor, measure) + pair_index_effect(keys@.take(index as int + 1), *changes, target, minor, false, measure) by {
                reveal(pair_index_effect);
                lemma_sum_take(keys@, index as int, |entry: Major| index_weight(entry, sum(changes.rows(entry), |cell: Cell| cell.count as int), target, minor, false, measure));
            }
            assert forall|target: Major, minor: Major, measure: bool| #[trigger] supplier_index.at(target, minor, measure)
                == initial_supplier.at(target, minor, measure) + pair_index_effect(keys@.take(index as int + 1), *changes, target, minor, true, measure) by {
                reveal(pair_index_effect);
                lemma_sum_take(keys@, index as int, |entry: Major| index_weight(entry, sum(changes.rows(entry), |cell: Cell| cell.count as int), target, minor, true, measure));
            }
        }
        index += 1;
    }
    proof { assert(keys@.take(keys@.len() as int) =~= keys@); }
    Some(())
}

pub open spec fn index_weight(entry: Major, count: int, key: Major, minor: Major, supplier: bool, measure: bool) -> int {
    if key == (if supplier { entry.1 } else { entry.0 }, 0, 0)
        && minor == (if supplier { entry.0 } else { entry.1 }, 0, 0) && !measure { count } else { 0 }
}

#[verifier::opaque]
pub open spec fn pair_index_effect(keys: Seq<Major>, changes: Table, key: Major, minor: Major, supplier: bool, measure: bool) -> int {
    sum(keys, |entry: Major| index_weight(entry, sum(changes.rows(entry), |cell: Cell| cell.count as int), key, minor, supplier, measure))
}

#[verifier::opaque]
pub open spec fn table_effect(keys: Seq<Major>, changes: Table, key: Major, minor: Major, measure: bool) -> int {
    sum(keys, |entry: Major| row_effect(changes.rows(entry), entry, key, minor, measure))
}

#[verifier::opaque]
pub open spec fn row_effect(rows: Seq<Cell>, entry: Major, key: Major, minor: Major, measure: bool) -> int {
    sum(rows, |cell: Cell| if entry == key { cell_weight(cell, minor, measure) } else { 0 })
}

pub proof fn lemma_table_effect(changes: Table, key: Major, minor: Major, measure: bool)
    requires changes.valid(),
    ensures table_effect(changes.keys_view(), changes, key, minor, measure) == changes.at(key, minor, measure),
{
    hide(sum);
    lemma_keys_complete(changes);
    lemma_keys_unique(changes);
    reveal(table_effect);
    let keys = changes.keys_view();
    let value = changes.at(key, minor, measure);
    let raw = |entry: Major| row_effect(changes.rows(entry), entry, key, minor, measure);
    let selected = |entry: Major| if entry == key { value } else { 0int };
    assert forall|entry: Major| #[trigger] raw(entry) == selected(entry) by {
        reveal(row_effect);
        if entry == key {
            reveal(Table::at);
            reveal(cell_sum);
            lemma_sum_ext_all(changes.rows(entry), |cell: Cell| if entry == key { cell_weight(cell, minor, measure) } else { 0int },
                |cell: Cell| cell_weight(cell, minor, measure));
        } else {
            lemma_sum_zero(changes.rows(entry), |cell: Cell| if entry == key { cell_weight(cell, minor, measure) } else { 0int });
        }
    }
    lemma_sum_ext_all(keys, raw, selected);
    assert(distinct(keys));
    if !keys.contains(key) {
        reveal(Table::at);
        reveal(cell_sum);
        assert(changes.rows(key).len() == 0);
        lemma_sum_empty(changes.rows(key), |cell: Cell| cell_weight(cell, minor, measure));
    }
    lemma_sum_indicator_distinct(keys, key, value, selected);
}

fn add_rows(rows: &[Cell], entry: Major, target: &mut Work) -> (result: Option<()>)
    requires old(target).valid(),
    ensures result is Some ==> final(target).valid(),
        result is Some ==> forall|key: Major, minor: Major, measure: bool| #[trigger] final(target).at(key, minor, measure)
            == old(target).at(key, minor, measure) + row_effect(rows@, entry, key, minor, measure),
{
    let ghost initial = *target;
    let mut index = 0;
    proof { reveal(row_effect); }
    while index < rows.len()
        invariant index <= rows.len(), target.valid(),
            forall|key: Major, minor: Major, measure: bool| #[trigger] target.at(key, minor, measure)
                == initial.at(key, minor, measure) + row_effect(rows@.take(index as int), entry, key, minor, measure),
        decreases rows.len() - index,
    {
        let cell = rows[index];
        target.add(entry, (cell.first, cell.second, cell.third), cell.count, cell.volume)?;
        proof {
            assert forall|key: Major, minor: Major, measure: bool| #[trigger] target.at(key, minor, measure)
                == initial.at(key, minor, measure) + row_effect(rows@.take(index as int + 1), entry, key, minor, measure) by {
                reveal(row_effect);
                lemma_sum_take(rows@, index as int, |cell: Cell| if entry == key { cell_weight(cell, minor, measure) } else { 0 });
            }
        }
        index += 1;
    }
    proof { assert(rows@.take(rows@.len() as int) =~= rows@); }
    Some(())
}

pub fn add_table(changes: &Table, target: &mut Work) -> (result: Option<()>)
    requires changes.valid(), old(target).valid(),
    ensures result is Some ==> final(target).valid(),
        result is Some ==> forall|key: Major, minor: Major, measure: bool| #[trigger] final(target).at(key, minor, measure)
            == old(target).at(key, minor, measure) + table_effect(changes.keys_view(), *changes, key, minor, measure),
{
    let keys = changes.keys();
    let ghost initial = *target;
    let mut index = 0;
    proof { reveal(table_effect); }
    while index < keys.len()
        invariant index <= keys.len(), changes.valid(), keys@ == changes.keys_view(), target.valid(),
            forall|key: Major, minor: Major, measure: bool| #[trigger] target.at(key, minor, measure)
                == initial.at(key, minor, measure) + table_effect(keys@.take(index as int), *changes, key, minor, measure),
        decreases keys.len() - index,
    {
        let rows = changes.get(keys[index]);
        add_rows(rows.as_slice(), keys[index], target)?;
        proof {
            assert forall|key: Major, minor: Major, measure: bool| #[trigger] target.at(key, minor, measure)
                == initial.at(key, minor, measure) + table_effect(keys@.take(index as int + 1), *changes, key, minor, measure) by {
                reveal(table_effect);
                lemma_sum_take(keys@, index as int, |entry: Major| row_effect(changes.rows(entry), entry, key, minor, measure));
            }
        }
        index += 1;
    }
    proof { assert(keys@.take(keys@.len() as int) =~= keys@); }
    Some(())
}

}
