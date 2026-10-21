use vstd::prelude::*;
use verus_common::sums::*;
use crate::profiles::*;
use crate::profile_proofs::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::read_proofs::*;
use crate::model::*;
use crate::storage::*;
use crate::dimensions::*;
use crate::activation::*;
use crate::semantic_proofs::part_capture_weight;

verus! {

/// A bag with no matching row has no matching quantity either.
pub proof fn lemma_empty_indicator<A>(rows: Seq<A>, count: spec_fn(A) -> int, quantity: spec_fn(A) -> int)
    requires forall|row: A| #[trigger] count(row) >= 0,
        forall|row: A| #![trigger quantity(row)] count(row) == 0 ==> quantity(row) == 0,
        sum(rows, count) == 0,
    ensures sum(rows, quantity) == 0,
{
    assert forall|index: int| 0 <= index < rows.len() implies count(#[trigger] rows[index]) >= 0 by {}
    assert forall|index: int| 0 <= index < rows.len() implies quantity(#[trigger] rows[index]) == 0 by {
        if count(rows[index]) > 0 { lemma_sum_pos(rows, count, index); }
    }
    lemma_sum_zero(rows, quantity);
}

/// A work part whose measures are those of real (non-negative) rows can be finished.
pub proof fn lemma_part_finish_ready(source: PartWork)
    requires source.shape(), source.count@ > 0,
        forall|supplier: i64, value: int| #[trigger] source.stock_for(supplier, value) >= 0,
        forall|supplier: i64, date: int| #![trigger source.day_for(supplier, date, false)]
            source.day_for(supplier, date, false) >= 0
                && (source.day_for(supplier, date, false) == 0 ==> source.day_for(supplier, date, true) == 0),
    ensures source.finish_ready(),
{
    assert forall|key: i64| #![trigger source.pairs@[key]] source.pairs@.contains_key(key) implies source.pairs@[key].finish_ready() by {
        let pair = source.pairs@[key];
        assert(pair.shape());
        assert forall|value: int| #![trigger pair.stock_count(value)] pair.stock_count(value) >= 0 by {
            assert(source.stock_for(key, value) == pair.stock_count(value));
        }
        assert forall|date: int| #![trigger day_measure(pair.days@, date, false)]
            day_measure(pair.days@, date, false) >= 0
                && (day_measure(pair.days@, date, false) == 0 ==> day_measure(pair.days@, date, true) == 0) by {
            assert(source.day_for(key, date, false) == day_measure(pair.days@, date, false));
            assert(source.day_for(key, date, true) == day_measure(pair.days@, date, true));
        }
    }
}


/// Every output row counts once.
pub open spec fn one_row() -> spec_fn(OutRow) -> int { |row: OutRow| 1int }

pub proof fn lemma_len_from_sum<A>(rows: Seq<A>)
    ensures sum(rows, |row: A| 1int) == rows.len(),
    decreases rows.len(),
{
    if rows.len() > 0 {
        lemma_len_from_sum(rows.drop_last());
        lemma_sum_push(rows.drop_last(), rows.last(), |row: A| 1int);
        assert(rows =~= rows.drop_last().push(rows.last()));
    }
}

pub proof fn lemma_out_len(out: Seq<(Vec<u8>, Vec<u8>)>)
    ensures sum(out_view(out), one_row()) == out.len(),
{
    lemma_len_from_sum(out_view(out));
    assert(out_view(out).len() == out.len());
    lemma_sum_ext_all(out_view(out), one_row(), |row: OutRow| 1int);
}

pub proof fn lemma_sum_prefix_le<A>(rows: Seq<A>, term: spec_fn(A) -> int, cut: int)
    requires 0 <= cut <= rows.len(), forall|index: int| 0 <= index < rows.len() ==> term(#[trigger] rows[index]) >= 0,
    ensures sum(rows.take(cut), term) <= sum(rows, term),
    decreases rows.len() - cut,
{
    if cut < rows.len() {
        lemma_sum_take(rows, cut, term);
        lemma_sum_prefix_le(rows, term, cut + 1);
    } else {
        assert(rows.take(cut) =~= rows);
    }
}

pub proof fn lemma_variant_prefix_le(rows: Seq<SupplierVariant>, cut: int, weight: spec_fn(OutRow) -> int)
    requires 0 <= cut <= rows.len(), forall|o: OutRow| #[trigger] weight(o) >= 0,
        forall|index: int| 0 <= index < rows.len() ==> (#[trigger] rows[index]).count@ >= 0,
    ensures variant_weight(rows.take(cut), weight) <= variant_weight(rows, weight),
        variant_weight(rows, weight) >= 0,
{
    let term = |row: SupplierVariant| row.count@ * weight(OutRow { s_name: row.name@, s_address: row.address@ });
    assert forall|index: int| 0 <= index < rows.len() implies term(#[trigger] rows[index]) >= 0 by {
        let count = rows[index].count@;
        let mass = weight(OutRow { s_name: rows[index].name@, s_address: rows[index].address@ });
        assert(count * mass >= 0) by (nonlinear_arith) requires count >= 0, mass >= 0;
    }
    lemma_sum_prefix_le(rows, term, cut);
    lemma_sum_nonneg(rows, term);
}

pub proof fn lemma_nation_weight_nonneg(db: Db, parameters: Params, nation: int, weight: spec_fn(OutRow) -> int)
    requires forall|o: OutRow| #[trigger] weight(o) >= 0,
    ensures nation_weight(db, parameters, nation, weight) >= 0,
{
    let term = |row: SupplierRow| if row.s_nationkey == nation && in_1(db, parameters, row.s_suppkey) { weight(OutRow { s_name: row.s_name, s_address: row.s_address }) } else { 0 };
    assert forall|index: int| 0 <= index < db.supplier.len() implies term(#[trigger] db.supplier[index]) >= 0 by {}
    lemma_sum_nonneg(db.supplier, term);
}

pub proof fn lemma_bucket_prefix_le(parts: Table<Part>, directory: Table<Vec<i64>>, names: PartNames, bucket: Table<Vec<SupplierVariant>>,
    keys: Seq<i64>, cut: int, parameters: Params, weight: spec_fn(OutRow) -> int)
    requires 0 <= cut <= keys.len(), forall|o: OutRow| #[trigger] weight(o) >= 0,
        forall|key: i64| #![trigger bucket.lookup(key)] bucket.lookup(key) matches Some(rows) ==>
            forall|index: int| 0 <= index < rows@.len() ==> (#[trigger] rows@[index]).count@ >= 0,
    ensures bucket_weight(parts, directory, names, bucket, keys.take(cut), parameters, weight)
        <= bucket_weight(parts, directory, names, bucket, keys, parameters, weight),
{
    let term = |key: i64| if cached_supplier_passes(parts, directory, names, key, parameters.p1, parameters.p4, parameters.p2) { variant_weight(bucket_rows(bucket, key), weight) } else { 0 };
    assert forall|index: int| 0 <= index < keys.len() implies term(#[trigger] keys[index]) >= 0 by {
        let key = keys[index];
        if let Some(rows) = bucket.lookup(key) { lemma_variant_prefix_le(rows@, 0, weight); }
    }
    lemma_sum_prefix_le(keys, term, cut);
}

pub proof fn lemma_nation_prefix_le(db: Db, parameters: Params, rows: Seq<NationVariant>, cut: int, weight: spec_fn(OutRow) -> int)
    requires 0 <= cut <= rows.len(), forall|o: OutRow| #[trigger] weight(o) >= 0,
        forall|index: int| 0 <= index < rows.len() ==> (#[trigger] rows[index]).count@ >= 0,
    ensures nation_output_weight(db, parameters, rows.take(cut), weight) <= nation_output_weight(db, parameters, rows, weight),
{
    let term = |row: NationVariant| if row.name@ == parameters.p3 { row.count@ * nation_weight(db, parameters, row.key as int, weight) } else { 0int };
    assert forall|index: int| 0 <= index < rows.len() implies term(#[trigger] rows[index]) >= 0 by {
        lemma_nation_weight_nonneg(db, parameters, rows[index].key as int, weight);
        let count = rows[index].count@;
        let mass = nation_weight(db, parameters, rows[index].key as int, weight);
        if rows[index].name@ == parameters.p3 { assert(count * mass >= 0) by (nonlinear_arith) requires count >= 0, mass >= 0; }
    }
    lemma_sum_prefix_le(rows, term, cut);
}


/// More than max_rows() result rows: no correct answer fits.
pub proof fn lemma_too_long(db: Db, parameters: Params)
    requires sum(crate::gen_spec::rows(db, parameters), one_row()) > max_rows(),
    ensures !answer_fits(db, parameters),
{
    let all = crate::gen_spec::rows(db, parameters);
    lemma_len_from_sum(all);
    lemma_sum_ext_all(all, one_row(), |row: OutRow| 1int);
    lemma_answer_ok(db, parameters);
    let witness = answer(db, parameters);
    assert(witness.to_multiset() =~= all.to_multiset());
    witness.to_multiset_ensures();
    all.to_multiset_ensures();
    assert(witness.len() == all.len());
    assert(query_result(db, parameters, witness));
}


pub proof fn lemma_eligible_nonneg(db: Db, part: int)
    ensures eligible(db, part) >= 0,
{
    let weight = |row: PartRow| if row.p_partkey == part { 1int } else { 0int };
    assert forall|index: int| 0 <= index < db.part.len() implies weight(#[trigger] db.part[index]) >= 0 by {}
    lemma_sum_nonneg(db.part, weight);
}

pub proof fn lemma_stock_nonneg(db: Db, part: int, supplier: int, availability: int)
    ensures stock_count(db, part, supplier, availability) >= 0,
{
    let weight = |row: PartsuppRow| if row.ps_partkey == part && row.ps_suppkey == supplier && row.ps_availqty == availability { 1int } else { 0int };
    assert forall|index: int| 0 <= index < db.partsupp.len() implies weight(#[trigger] db.partsupp[index]) >= 0 by {}
    lemma_sum_nonneg(db.partsupp, weight);
}

pub proof fn lemma_daily_nonneg(db: Db, part: int, supplier: int, date: int)
    ensures daily_count(db, part, supplier, date) >= 0,
        daily_count(db, part, supplier, date) == 0 ==> daily_quantity(db, part, supplier, date) == 0,
{
    let count = |row: LineitemRow| if row.l_partkey == part && row.l_suppkey == supplier && row.l_shipdate == date { 1int } else { 0int };
    let quantity = |row: LineitemRow| if row.l_partkey == part && row.l_suppkey == supplier && row.l_shipdate == date { row.l_quantity } else { 0int };
    assert forall|row: LineitemRow| #[trigger] count(row) >= 0 by {}
    assert forall|row: LineitemRow| #![trigger quantity(row)] count(row) == 0 ==> quantity(row) == 0 by {}
    assert forall|index: int| 0 <= index < db.lineitem.len() implies count(#[trigger] db.lineitem[index]) >= 0 by {}
    lemma_sum_nonneg(db.lineitem, count);
    if daily_count(db, part, supplier, date) == 0 { lemma_empty_indicator(db.lineitem, count, quantity); }
}

pub proof fn lemma_first_seen_contains(rows: Seq<PartDelta>, key: i64)
    requires rows.map_values(|row: PartDelta| row.row.p_partkey).contains(key),
    ensures first_seen(rows).contains(key),
    decreases rows.len(),
{
    let keys = rows.map_values(|row: PartDelta| row.row.p_partkey);
    let index = choose|index: int| 0 <= index < keys.len() && keys[index] == key;
    assert(rows.len() > 0);
    let before = first_seen(rows.drop_last());
    let last = rows.last().row.p_partkey;
    assert(first_seen(rows) == if before.contains(last) { before } else { before.push(last) });
    if index == rows.len() - 1 {
        assert(last == key);
        if !before.contains(last) { assert(before.push(last)[before.len() as int] == last); }
    } else {
        assert(rows.drop_last().map_values(|row: PartDelta| row.row.p_partkey)[index] == key);
        lemma_first_seen_contains(rows.drop_last(), key);
        if !before.contains(last) {
            let found = choose|found: int| 0 <= found < before.len() && before[found] == key;
            assert(before.push(last)[found] == key);
        }
    }
}

pub proof fn lemma_activated_contains(parts: Table<Part>, rows: Seq<PartDelta>, keys: Seq<i64>, key: i64)
    requires keys.contains(key), activates(parts, rows, key),
    ensures activated_keys(parts, rows, keys).contains(key),
    decreases keys.len(),
{
    let before = activated_keys(parts, rows, keys.drop_last());
    if keys.last() == key {
        assert(activates(parts, rows, keys.last()));
        assert(activated_keys(parts, rows, keys) == before.push(key));
        assert(activated_keys(parts, rows, keys)[before.len() as int] == key);
    } else {
        let index = choose|index: int| 0 <= index < keys.len() && keys[index] == key;
        assert(keys.drop_last()[index] == key);
        lemma_activated_contains(parts, rows, keys.drop_last(), key);
        if activates(parts, rows, keys.last()) {
            assert(activated_keys(parts, rows, keys) == before.push(keys.last()));
            let found = choose|found: int| 0 <= found < before.len() && before[found] == key;
            assert(activated_keys(parts, rows, keys)[found] == key);
        }
    }
}

pub proof fn lemma_activation_member(parts: Table<Part>, rows: Seq<PartDelta>, changed: Seq<i64>, key: i64)
    requires changed.to_set() == rows.map_values(|row: PartDelta| row.row.p_partkey).to_set(),
        changed.contains(key), parts.lookup(key).is_none(), sum(rows, part_capture_weight(key as int)) > 0,
    ensures activation_spec(parts, rows).contains(key),
{
    changed.to_set_ensures();
    let keys = rows.map_values(|row: PartDelta| row.row.p_partkey);
    keys.to_set_ensures();
    assert(changed.to_set().contains(key));
    assert(keys.contains(key));
    lemma_first_seen_contains(rows, key);
    lemma_activated_contains(parts, rows, first_seen(rows), key);
}


pub proof fn lemma_meaning_nation_row(db: Db, parts: Table<Part>, directory: Table<Vec<i64>>, suppliers: Suppliers, nations: Nations, names: PartNames,
    hash: i64, rows: Vec<NationVariant>, index: int)
    requires meaning(db, parts, directory, suppliers, nations, names), nations.lookup(hash) == Some(rows), 0 <= index < rows@.len(),
    ensures rows@[index].count@ > 0,
{
    assert(nations.lookup(hash) matches Some(r) && r == rows);
    assert(forall|i: int| #![trigger rows@[i]] 0 <= i < rows@.len() ==> rows@[i].count@ > 0 && name_hash(rows@[i].name@) == hash);
}

}
