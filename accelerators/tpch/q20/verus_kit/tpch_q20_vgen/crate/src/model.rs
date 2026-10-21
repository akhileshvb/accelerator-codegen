use vstd::prelude::*;
use verus_prooflib::wide::Wide;
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
    &&& forall|index: int| #![trigger db.part[index]] 0 <= index < db.part.len() ==>
            machine_key(db.part[index].p_partkey)
    &&& forall|index: int| #![trigger db.partsupp[index]] 0 <= index < db.partsupp.len()
        && eligible(db, db.partsupp[index].ps_partkey) > 0 ==>
            machine_key(db.partsupp[index].ps_partkey) && machine_key(db.partsupp[index].ps_suppkey)
    &&& forall|index: int| #![trigger db.lineitem[index]] 0 <= index < db.lineitem.len()
        && eligible(db, db.lineitem[index].l_partkey) > 0 ==>
            machine_key(db.lineitem[index].l_partkey) && machine_key(db.lineitem[index].l_suppkey)
}

/// The build processes parts in 16 chunks by key, bounding its transient work state.
pub open spec fn chunk_of(key: i64) -> int { ((key as u64) % 16) as int }
pub open spec fn eligible(db: Db, part: int) -> int {
    sum(db.part, |row: PartRow| if row.p_partkey == part { 1 } else { 0 })
}
/// Some part row with this key has a name matching the LIKE pattern.
pub open spec fn part_matches(db: Db, pattern: Seq<u8>, key: int) -> bool {
    exists|index: int| #![trigger db.part[index]] 0 <= index < db.part.len()
        && db.part[index].p_partkey == key && like_match(db.part[index].p_name, pattern)
}
pub open spec fn name_count(names: PartNames, key: i64, name: Seq<u8>) -> int {
    match names.lookup(name_slot(key, name)) { None => 0, Some(rows) => nation_measure(rows@, name, key) }
}
/// Some stored name of this part key matches the LIKE pattern.
pub open spec fn names_match(names: PartNames, key: i64, pattern: Seq<u8>) -> bool {
    exists|name: Seq<u8>| #![trigger name_count(names, key, name)] name_count(names, key, name) > 0 && like_match(name, pattern)
}
/// The pattern starts with three literal bytes (no wildcard), so a matching name shares its first three bytes.
pub open spec fn pattern_literal3(pattern: Seq<u8>) -> bool {
    pattern.len() >= 3 && pattern[0] != 37u8 && pattern[0] != 95u8 && pattern[1] != 37u8 && pattern[1] != 95u8
        && pattern[2] != 37u8 && pattern[2] != 95u8
}
pub proof fn lemma_like_prefix(name: Seq<u8>, pattern: Seq<u8>)
    requires pattern_literal3(pattern), like_match(name, pattern),
    ensures name.len() >= 3, name[0] == pattern[0], name[1] == pattern[1], name[2] == pattern[2],
        name_code(name) == name_code(pattern),
{
    reveal_with_fuel(like_match, 4);
    assert(name.len() > 0 && name[0] == pattern[0] && like_match(name.drop_first(), pattern.drop_first()));
    let n1 = name.drop_first(); let p1 = pattern.drop_first();
    assert(n1.len() > 0 && n1[0] == p1[0] && like_match(n1.drop_first(), p1.drop_first()));
    let n2 = n1.drop_first(); let p2 = p1.drop_first();
    assert(n2.len() > 0 && n2[0] == p2[0]);
}
/// The pattern is `lp%`: a literal prefix (no wildcard) followed by one final `%`.
pub open spec fn prefix_pattern(p: Seq<u8>) -> bool {
    p.len() >= 1 && p[p.len() - 1] == 37u8
        && forall|i: int| #![trigger p[i]] 0 <= i < p.len() - 1 ==> p[i] != 37u8 && p[i] != 95u8
}
pub proof fn lemma_like_anything(s: Seq<u8>)
    ensures like_match(s, seq![37u8]),
    decreases s.len(),
{
    reveal_with_fuel(like_match, 2);
    let p = seq![37u8];
    assert(p.drop_first() =~= Seq::<u8>::empty());
    if s.len() > 0 {
        lemma_like_anything(s.drop_first());
        assert(like_match(s.drop_first(), p));
    } else {
        assert(like_match(s, p.drop_first()));
    }
}
pub proof fn lemma_like_prefix_pattern(s: Seq<u8>, p: Seq<u8>)
    requires prefix_pattern(p),
    ensures like_match(s, p) == (s.len() + 1 >= p.len() && s.subrange(0, p.len() - 1) =~= p.subrange(0, p.len() - 1)),
    decreases p.len(),
{
    reveal_with_fuel(like_match, 2);
    if p.len() == 1 {
        assert(p =~= seq![37u8]);
        lemma_like_anything(s);
        assert(s.subrange(0, 0) =~= p.subrange(0, 0));
    } else {
        let q = p.drop_first();
        assert(prefix_pattern(q)) by {
            assert(q.len() >= 1 && q[q.len() - 1] == 37u8);
            assert forall|i: int| #![trigger q[i]] 0 <= i < q.len() - 1 implies q[i] != 37u8 && q[i] != 95u8 by {
                assert(q[i] == p[i + 1]);
            }
        }
        assert(p[0] != 37u8 && p[0] != 95u8);
        if s.len() > 0 {
            lemma_like_prefix_pattern(s.drop_first(), q);
            let k = p.len() - 1;
            if like_match(s, p) {
                assert(s[0] == p[0]);
                assert(s.len() + 1 >= p.len());
                assert(s.subrange(0, k) =~= p.subrange(0, k)) by {
                    assert forall|i: int| 0 <= i < k implies #[trigger] s.subrange(0, k)[i] == p.subrange(0, k)[i] by {
                        if i > 0 { assert(s.drop_first().subrange(0, k - 1)[i - 1] == q.subrange(0, k - 1)[i - 1]); }
                    }
                }
            }
            if s.len() + 1 >= p.len() && s.subrange(0, k) =~= p.subrange(0, k) {
                assert(s.subrange(0, k)[0] == p.subrange(0, k)[0]);
                assert(s[0] == p[0]);
                assert(s.drop_first().subrange(0, k - 1) =~= q.subrange(0, k - 1)) by {
                    assert forall|i: int| 0 <= i < k - 1 implies #[trigger] s.drop_first().subrange(0, k - 1)[i] == q.subrange(0, k - 1)[i] by {
                        assert(s.subrange(0, k)[i + 1] == p.subrange(0, k)[i + 1]);
                    }
                }
                assert(like_match(s, p));
            }
        } else {
            assert(!like_match(s, p));
        }
    }
}
pub proof fn lemma_slot_split(key: i64, name: Seq<u8>)
    ensures name_slot(key, name) as int == name_code(name) * 1024 + key_low(key), 0 <= key_low(key) < 1024,
        0 <= name_code(name) < 16777216,
{
    assert(0 <= byte_or_zero(name, 0) <= 255 && 0 <= byte_or_zero(name, 1) <= 255 && 0 <= byte_or_zero(name, 2) <= 255);
}
pub open spec fn names_correct(db: Db, names: PartNames) -> bool {
    names_positive(names) && forall|key: i64, name: Seq<u8>| #[trigger] name_count(names, key, name) ==
        sum(db.part, |row: PartRow| if row.p_partkey == key && row.p_name == name { 1 } else { 0 })
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
        pair.ship@[index].count@ - if index == 0 { 0 } else { pair.ship@[index - 1].count@ }
    } else { 0 })
}
#[verifier::opaque]
pub open spec fn stored_before_quantity(pair: Pair, date: int) -> int {
    sum(rng(pair.ship@.len() as int), |index: int| if pair.ship@[index].date < date {
        pair.ship@[index].quantity@ - if index == 0 { 0 } else { pair.ship@[index - 1].quantity@ }
    } else { 0 })
}
pub proof fn lemma_before_cut(pair: &Pair, date: int, cutoff: int)
    requires 0 <= cutoff <= pair.ship@.len(),
        forall|index: int| #![trigger pair.ship@[index]] 0 <= index < pair.ship@.len() ==>
            (pair.ship@[index].date < date) == (index < cutoff),
    ensures stored_before_count(*pair, date) == if cutoff == 0 { 0 } else { pair.ship@[cutoff - 1].count@ },
        stored_before_quantity(*pair, date) == if cutoff == 0 { 0 } else { pair.ship@[cutoff - 1].quantity@ },
{
    reveal(stored_before_count);
    reveal(stored_before_quantity);
    let keep = |index: int| pair.ship@[index].date < date;
    let counts = pair.ship@.map_values(|cell: Prefix| cell.count@);
    let quantities = pair.ship@.map_values(|cell: Prefix| cell.quantity@);
    assert forall|index: int| #![trigger keep(index)] 0 <= index < pair.ship@.len() implies keep(index) == (index < cutoff) by {
        assert(pair.ship@[index].date < date <==> index < cutoff);
    }
    verus_prooflib::lemma_prefix_cutoff(counts, keep, cutoff, pair.ship@.len() as int);
    verus_prooflib::lemma_prefix_cutoff(quantities, keep, cutoff, pair.ship@.len() as int);
    verus_common::sums::lemma_sum_ext(rng(pair.ship@.len() as int),
        |index: int| if keep(index) { counts[index] - if index == 0 { 0 } else { counts[index - 1] } } else { 0 },
        |index: int| if pair.ship@[index].date < date { pair.ship@[index].count@ - if index == 0 { 0 } else { pair.ship@[index - 1].count@ } } else { 0 });
    verus_common::sums::lemma_sum_ext(rng(pair.ship@.len() as int),
        |index: int| if keep(index) { quantities[index] - if index == 0 { 0 } else { quantities[index - 1] } } else { 0 },
        |index: int| if pair.ship@[index].date < date { pair.ship@[index].quantity@ - if index == 0 { 0 } else { pair.ship@[index - 1].quantity@ } } else { 0 });
}
pub open spec fn part_count(parts: Table<Part>, key: i64) -> int {
    match parts.lookup(key) { Some(part) => part.count@, None => 0 }
}
pub open spec fn work_part_count(work: Map<i64, PartWork>, key: i64) -> int {
    if work.contains_key(key) { work[key].count@ } else { 0 }
}
pub open spec fn coalesced_part_count(parts: Table<Part>, counts: Map<i64, Wide>, key: i64) -> int {
    if counts.contains_key(key) { counts[key]@ } else { part_count(parts, key) }
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
pub open spec fn work_ready(work: Map<i64, PartWork>, keys: Seq<i64>, parts: Table<Part>, counts: Map<i64, Wide>) -> bool {
    work_shape(work) && keys.no_duplicates() && work.dom() == keys.to_set()
        && forall|key: i64| #![trigger work[key]] work.contains_key(key) ==>
            work[key].count@ > 0 && work[key].count@ == coalesced_part_count(parts, counts, key)
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
        Some(rows) => sum(rows@, |row: SupplierVariant| if row.name@ == name && row.address@ == address { row.count@ } else { 0 }),
    } }
}
pub open spec fn nation_count(state: Nations, name: Seq<u8>, key: i64) -> int {
    match state.lookup(name_hash(name) as i64) { None => 0,
        Some(rows) => sum(rows@, |row: NationVariant| if row.name@ == name && row.key == key { row.count@ } else { 0 }),
    }
}
pub open spec fn dimensions_empty(suppliers: Suppliers, nations: Nations, names: PartNames) -> bool {
    (forall|key: i64| #[trigger] suppliers.lookup(key).is_none()) && (forall|key: i64| #[trigger] nations.lookup(key).is_none())
        && (forall|key: i64| #[trigger] names.lookup(key).is_none())
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
pub open spec fn stock_support(rows: Seq<(i64, Pair)>, supplier: i64) -> bool {
    exists|index: int| #![trigger rows[index]] 0 <= index < rows.len() && rows[index].0 == supplier && rows[index].1.stocks@.len() > 0
}
pub proof fn lemma_stock_support_step(rows: Seq<(i64, Pair)>, index: int, supplier: i64)
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
/// The supplier-to-parts directory was dropped: reads go through the part-name index, so the state keeps
/// (and maintains) no directory.  These stay as trivially true specs for the maintenance interfaces.
pub open spec fn directory_unique(directory: Table<Vec<i64>>) -> bool { true }
pub open spec fn directory_matches(parts: Table<Part>, directory: Table<Vec<i64>>) -> bool { true }
pub open spec fn part_correct(db: Db, key: i64, part: Option<Part>) -> bool {
    match part {
        None => eligible(db, key as int) == 0,
        Some(data) => data.count@ == eligible(db, key as int) && data.count@ > 0 && crate::profile_proofs::part_shape(data)
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
        0 <= index < pair.stocks@.len() ==> pair.stocks@[index].count@ > 0
            && (index > 0 ==> pair.stocks@[index - 1].value < pair.stocks@[index].value)
    &&& forall|index: int| #![trigger pair.ship@[index]]
        0 <= index < pair.ship@.len() ==> pair.ship@[index].count@ > (if index == 0 { 0 } else { pair.ship@[index - 1].count@ })
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
pub open spec fn same_pair(a: Pair, b: Pair) -> bool { a.stocks@ == b.stocks@ && a.ship@ == b.ship@ }
pub proof fn lemma_same_pair(a: Pair, b: Pair)
    requires same_pair(a, b),
    ensures pair_valid(a) == pair_valid(b),
{
    reveal(pair_valid);
}
pub open spec fn read_shape(parts: Table<Part>) -> bool {
    &&& forall|key: i64| #![trigger parts.lookup(key)] parts.lookup(key) matches Some(part) ==>
        part.pairs@.map_values(|entry: (i64, Pair)| entry.0).no_duplicates()
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
pub open spec fn any_cand_passes(parts: Table<Part>, cands: Seq<i64>, supplier: i64, start: int, end: int) -> bool {
    exists|j: int| #![trigger cands[j]] 0 <= j < cands.len() && cached_part_passes(parts, cands[j], supplier, start, end)
}
pub open spec fn cached_part_selected(parts: Table<Part>, names: PartNames, part: i64, supplier: i64, start: int, end: int, pattern: Seq<u8>) -> bool {
    cached_part_passes(parts, part, supplier, start, end) && names_match(names, part, pattern)
}
pub open spec fn cached_supplier_passes(parts: Table<Part>, directory: Table<Vec<i64>>, names: PartNames, supplier: i64, start: int, end: int, pattern: Seq<u8>) -> bool {
    exists|part: i64| #![trigger cached_part_selected(parts, names, part, supplier, start, end, pattern)]
        cached_part_selected(parts, names, part, supplier, start, end, pattern)
}
pub proof fn lemma_cands_equiv(db: Db, parts: Table<Part>, directory: Table<Vec<i64>>, suppliers: Suppliers, nations: Nations, names: PartNames,
    cands: Seq<i64>, supplier: i64, start: int, end: int, pattern: Seq<u8>)
    requires meaning(db, parts, directory, suppliers, nations, names),
        forall|key: i64| #![trigger names_match(names, key, pattern)] names_match(names, key, pattern) ==> cands.contains(key),
        forall|key: i64| #![trigger cands.contains(key)] cands.contains(key) ==> names_match(names, key, pattern),
    ensures any_cand_passes(parts, cands, supplier, start, end) == cached_supplier_passes(parts, directory, names, supplier, start, end, pattern),
{
    if any_cand_passes(parts, cands, supplier, start, end) {
        let j = choose|j: int| #![trigger cands[j]] 0 <= j < cands.len() && cached_part_passes(parts, cands[j], supplier, start, end);
        let part = cands[j];
        assert(cands.contains(part));
        assert(names_match(names, part, pattern));
        assert(cached_part_selected(parts, names, part, supplier, start, end, pattern));
    }
    if cached_supplier_passes(parts, directory, names, supplier, start, end, pattern) {
        let part = choose|part: i64| #![trigger cached_part_selected(parts, names, part, supplier, start, end, pattern)]
            cached_part_selected(parts, names, part, supplier, start, end, pattern);
        assert(names_match(names, part, pattern));
        assert(cands.contains(part));
        let j = choose|j: int| #![trigger cands[j]] 0 <= j < cands.len() && cands[j] == part;
        assert(cached_part_passes(parts, cands[j], supplier, start, end));
    }
}
pub proof fn lemma_pair_at(parts: Table<Part>, key: i64, supplier: i64, part: &Part, index: int)
    requires parts.lookup(key) == Some(*part),
        part.pairs@.map_values(|entry: (i64, Pair)| entry.0).no_duplicates(),
        0 <= index < part.pairs@.len(), part.pairs@[index].0 == supplier,
    ensures pair_of(parts, key, supplier) == Some(part.pairs@[index].1),
{
    verus_prooflib::lemma_unique_key_lookup(part.pairs@, supplier);
    assert(verus_prooflib::unique_key_lookup(part.pairs@, supplier) == Some(part.pairs@[index].1));
}
pub open spec fn meaning(db: Db, parts: Table<Part>, directory: Table<Vec<i64>>, suppliers: Suppliers, nations: Nations, names: PartNames) -> bool {
    &&& parts.well_formed() && directory.well_formed() && suppliers.well_formed() && nations.well_formed() && names.well_formed()
    &&& names_correct(db, names)
    &&& forall|key: i64| #![trigger part_count(parts, key)] part_count(parts, key) == eligible(db, key as int)
    &&& forall|key: i64| #![trigger parts.lookup(key)] parts.lookup(key) matches Some(part) ==>
        part.count@ > 0 && part.pairs@.map_values(|entry: (i64, Pair)| entry.0).no_duplicates()
    &&& forall|part: i64, supplier: i64| #![trigger pair_of(parts, part, supplier)]
        match pair_of(parts, part, supplier) {
            Some(pair) => pair_valid(pair) && eligible(db, part as int) > 0
                && (pair.stocks@.len() > 0 || pair.ship@.len() > 0)
                && (forall|index: int| #![trigger pair.ship@[index]]
                    0 <= index < pair.ship@.len() ==>
                        pair.ship@[index].count@ == sum(db.lineitem, |row: LineitemRow| if row.l_partkey == part && row.l_suppkey == supplier && row.l_shipdate <= pair.ship@[index].date { 1 } else { 0 })
                        && pair.ship@[index].quantity@ == sum(db.lineitem, |row: LineitemRow| if row.l_partkey == part && row.l_suppkey == supplier && row.l_shipdate <= pair.ship@[index].date { row.l_quantity } else { 0 }))
                && forall|value: int| #![trigger stored_stock(pair, value)]
                    stored_stock(pair, value) == stock_count(db, part as int, supplier as int, value)
                && forall|date: int| #![trigger stored_day_count(pair, date)]
                    stored_day_count(pair, date) == daily_count(db, part as int, supplier as int, date)
                    && stored_day_quantity(pair, date) == daily_quantity(db, part as int, supplier as int, date),
            None => eligible(db, part as int) > 0 ==> (
                (forall|value: int| #![trigger stock_count(db, part as int, supplier as int, value)] stock_count(db, part as int, supplier as int, value) == 0)
                && (forall|date: int| #![trigger daily_count(db, part as int, supplier as int, date)] daily_count(db, part as int, supplier as int, date) == 0)),
        }
    &&& forall|nation: i64| #![trigger suppliers.lookup(nation)]
        suppliers.lookup(nation) matches Some(bucket) ==> bucket.well_formed()
            && forall|supplier: i64| #![trigger bucket.lookup(supplier)]
                bucket.lookup(supplier) matches Some(rows) ==>
                    forall|index: int| #![trigger rows@[index]] 0 <= index < rows@.len() ==> rows@[index].count@ > 0
    &&& forall|nation: i64, supplier: i64, name: Seq<u8>, address: Seq<u8>| #![trigger supplier_count(suppliers, nation, supplier, name, address)]
        supplier_count(suppliers, nation, supplier, name, address) == sum(db.supplier, |row: SupplierRow| if row.s_nationkey == nation && row.s_suppkey == supplier && row.s_name == name && row.s_address == address { 1 } else { 0 })
    &&& forall|name: Seq<u8>, key: i64| #![trigger nation_count(nations, name, key)]
        nation_count(nations, name, key) == sum(db.nation, |row: NationRow| if row.n_name == name && row.n_nationkey == key { 1 } else { 0 })
    &&& forall|hash: i64| #![trigger nations.lookup(hash)]
        nations.lookup(hash) matches Some(rows) ==>
            forall|index: int| #![trigger rows@[index]] 0 <= index < rows@.len() ==>
                rows@[index].count@ > 0 && name_hash(rows@[index].name@) == hash
}
}
