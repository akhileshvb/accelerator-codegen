use vstd::prelude::*;
use verus_common::sums::*;
use verus_common::rel::*;
use verus_common::sqlfns::like_match;
use verus_prooflib::ordered::KeyView;
use crate::gen_obligations::*;
use crate::gen_spec::*;
use crate::model::*;
use crate::input_proofs::*;
use crate::maintenance_semantics::database_suppliers;

verus! {

pub proof fn lemma_unit_sum<A>(rows: Seq<A>, weight: spec_fn(A) -> int)
    requires forall|row: A| 0 <= #[trigger] weight(row) <= 1,
    ensures 0 <= sum(rows, weight) <= rows.len(),
{
    lemma_sum_nonneg(rows, weight);
    lemma_sum_le(rows, weight, |row: A| 1int);
    verus_prooflib::lemma_sum_ones(rows);
}

pub proof fn lemma_dedup_len<A>(rows: Seq<A>)
    ensures dedup(rows).len() <= rows.len(),
    decreases rows.len(),
{
    if rows.len() > 0 { lemma_dedup_len(rows.drop_last()); }
}

/// Distinct keys carrying a unit weight that lies in a distinct list of keys are at most that list long.
pub proof fn lemma_prefix_bound(keys: Seq<int>, support: Seq<int>, weight: spec_fn(int) -> int)
    requires distinct(keys), distinct(support), forall|key: int| 0 <= #[trigger] weight(key) <= 1,
        forall|position: int| 0 <= position < keys.len() && weight(#[trigger] keys[position]) != 0 ==> support.contains(keys[position]),
    ensures sum(keys, weight) <= support.len(),
{
    let per = |key: int| sum(keys, |other: int| if other == key { weight(other) } else { 0int });
    let identity = |key: int| key;
    assert forall|key: int| #[trigger] per(key) == sum(keys, |other: int| if identity(other) == key { weight(other) } else { 0int }) by {
        lemma_sum_ext_all(keys, |other: int| if other == key { weight(other) } else { 0int },
            |other: int| if identity(other) == key { weight(other) } else { 0int });
    }
    lemma_sum_group(keys, support, identity, weight, per);
    assert forall|key: int| #[trigger] per(key) <= 1 by {
        let selected = |other: int| if other == key { weight(other) } else { 0int };
        if keys.contains(key) {
            lemma_sum_indicator_distinct(keys, key, weight(key), selected);
        } else {
            assert forall|position: int| 0 <= position < keys.len() implies selected(#[trigger] keys[position]) == 0 by {}
            lemma_sum_zero(keys, selected);
        }
    }
    lemma_sum_le(support, per, |key: int| 1int);
    verus_prooflib::lemma_sum_ones(support);
}

pub proof fn lemma_edge_supplier_listed(db: Db, part: int, supplier: int)
    requires edge_count(db, part, supplier) != 0,
    ensures database_suppliers(db).contains(supplier),
{
    let weight = |row: PartsuppRow| if row.ps_partkey == part && row.ps_suppkey == supplier { 1int } else { 0int };
    let position = lemma_sum_nonzero_witness(db.partsupp, weight);
    assert(supplier_keys(db.partsupp)[position] == supplier);
    assert(supplier_keys(db.partsupp).contains(supplier));
    lemma_dedup(supplier_keys(db.partsupp));
}

pub proof fn lemma_database_suppliers_len(db: Db)
    ensures database_suppliers(db).len() <= db.partsupp.len(),
{
    lemma_dedup_len(supplier_keys(db.partsupp));
}

pub proof fn lemma_part_sum_bound(db: Db, part: int, key: KeyView)
    ensures 0 <= part_database_sum(db, part, key) <= db.part.len(),
{
    lemma_unit_sum(db.part, |row: PartRow| part_weight(row, part, key));
}

pub proof fn lemma_edge_count_bound(db: Db, part: int, supplier: int)
    ensures 0 <= edge_count(db, part, supplier) <= db.partsupp.len(),
{
    lemma_unit_sum(db.partsupp, |row: PartsuppRow| if row.ps_partkey == part && row.ps_suppkey == supplier { 1int } else { 0int });
}

pub proof fn lemma_blocker_count_bound(db: Db, supplier: int)
    ensures 0 <= blocker_count(db, supplier) <= db.supplier.len(),
{
    lemma_unit_sum(db.supplier, |row: SupplierRow| if row.s_suppkey == supplier && like_match(row.s_comment,
        seq![b'%', b'C', b'u', b's', b't', b'o', b'm', b'e', b'r', b'%', b'C', b'o', b'm', b'p', b'l', b'a', b'i', b'n', b't', b's', b'%']) { 1int } else { 0int });
}

pub proof fn lemma_part_input_bound(rows: Seq<PartInput>, part: int, key: KeyView)
    ensures 0 <= part_input_sum(rows, part, key) <= rows.len(),
{
    lemma_unit_sum(part_input_seq_view(rows), |row: PartRow| part_weight(row, part, key));
}

pub proof fn lemma_edge_input_bound(rows: Seq<EdgeInput>, supplier: int, part: int)
    ensures 0 <= edge_input_sum(rows, supplier, part) <= rows.len(),
{
    lemma_unit_sum(edge_input_seq_view(rows), |row: PartsuppRow| edge_weight(row, supplier, part));
}

pub proof fn lemma_blocker_input_bound(rows: Seq<BlockerInput>, supplier: int)
    ensures 0 <= blocker_input_sum(rows, supplier) <= rows.len(),
{
    lemma_unit_sum(blocker_input_seq_view(rows), |key: int| blocker_weight(key, supplier));
}

pub proof fn lemma_scan_parts_len(db: Db, rows: Seq<PartInput>)
    requires scan_parts_ok(db, rows),
    ensures rows.len() == db.part.len(),
{
    let one = |row: PartRow| 1int;
    verus_prooflib::lemma_sum_ones(part_input_seq_view(rows));
    verus_prooflib::lemma_sum_ones(db.part);
    assert(sum(part_input_seq_view(rows), one) == sum(db.part, one));
}

pub proof fn lemma_scan_edges_len(db: Db, rows: Seq<EdgeInput>)
    requires scan_edges_ok(db, rows),
    ensures rows.len() == db.partsupp.len(),
{
    let one = |row: PartsuppRow| 1int;
    verus_prooflib::lemma_sum_ones(edge_input_seq_view(rows));
    verus_prooflib::lemma_sum_ones(db.partsupp);
    assert(sum(edge_input_seq_view(rows), one) == sum(db.partsupp, one));
}

pub proof fn lemma_scan_blockers_len(db: Db, rows: Seq<BlockerInput>)
    requires scan_blockers_ok(db, rows),
    ensures rows.len() <= db.supplier.len(),
{
    let one = |key: int| 1int;
    verus_prooflib::lemma_sum_ones(blocker_input_seq_view(rows));
    lemma_unit_sum(db.supplier, |row: SupplierRow| if like_match(row.s_comment,
        seq![b'%', b'C', b'u', b's', b't', b'o', b'm', b'e', b'r', b'%', b'C', b'o', b'm', b'p', b'l', b'a', b'i', b'n', b't', b's', b'%']) { 1int } else { 0int });
    assert(sum(blocker_input_seq_view(rows), one) == sum(db.supplier,
        |row: SupplierRow| if like_match(row.s_comment,
            seq![b'%', b'C', b'u', b's', b't', b'o', b'm', b'e', b'r', b'%', b'C', b'o', b'm', b'p', b'l', b'a', b'i', b'n', b't', b's', b'%']) { one(row.s_suppkey) } else { 0int }));
    lemma_sum_ext_all(db.supplier, |row: SupplierRow| if like_match(row.s_comment,
            seq![b'%', b'C', b'u', b's', b't', b'o', b'm', b'e', b'r', b'%', b'C', b'o', b'm', b'p', b'l', b'a', b'i', b'n', b't', b's', b'%']) { one(row.s_suppkey) } else { 0int },
        |row: SupplierRow| if like_match(row.s_comment,
            seq![b'%', b'C', b'u', b's', b't', b'o', b'm', b'e', b'r', b'%', b'C', b'o', b'm', b'p', b'l', b'a', b'i', b'n', b't', b's', b'%']) { 1int } else { 0int });
}

pub proof fn lemma_rescan_parts_len(db: Db, keys: Seq<i64>, rows: Seq<PartInput>)
    requires rescan_parts_for_neighbors_ok(db, keys, rows),
    ensures rows.len() <= db.part.len(),
{
    let one = |row: PartRow| 1int;
    let selected = |row: PartRow| if seq_view_i64(keys).contains(row.p_partkey) { one(row) } else { 0int };
    verus_prooflib::lemma_sum_ones(part_input_seq_view(rows));
    assert(sum(part_input_seq_view(rows), one) == sum(db.part, selected));
    lemma_unit_sum(db.part, selected);
}

pub proof fn lemma_rescan_edges_len(db: Db, keys: Seq<i64>, rows: Seq<EdgeInput>)
    requires rescan_edges_for_suppliers_ok(db, keys, rows),
    ensures rows.len() <= db.partsupp.len(),
{
    let one = |row: PartsuppRow| 1int;
    let selected = |row: PartsuppRow| if seq_view_i64(keys).contains(row.ps_suppkey) { one(row) } else { 0int };
    verus_prooflib::lemma_sum_ones(edge_input_seq_view(rows));
    assert(sum(edge_input_seq_view(rows), one) == sum(db.partsupp, selected));
    lemma_unit_sum(db.partsupp, selected);
}

pub proof fn lemma_rescan_blockers_len(db: Db, keys: Seq<i64>, rows: Seq<BlockerInput>)
    requires rescan_blockers_for_suppliers_ok(db, keys, rows),
    ensures rows.len() <= db.supplier.len(),
{
    let one = |key: int| 1int;
    verus_prooflib::lemma_sum_ones(blocker_input_seq_view(rows));
    let selected = |row: SupplierRow| if like_match(row.s_comment,
        seq![b'%', b'C', b'u', b's', b't', b'o', b'm', b'e', b'r', b'%', b'C', b'o', b'm', b'p', b'l', b'a', b'i', b'n', b't', b's', b'%']) && seq_view_i64(keys).contains(row.s_suppkey) { one(row.s_suppkey) } else { 0int };
    assert(sum(blocker_input_seq_view(rows), one) == sum(db.supplier, selected));
    lemma_unit_sum(db.supplier, selected);
}

/// Supplier keys are 32-bit, so a state holds at most this many distinct suppliers.
pub open spec fn max_suppliers() -> int {
    4294967296
}

pub proof fn lemma_suppliers_in_type(db: Db)
    requires envelope(db),
    ensures database_suppliers(db).len() <= max_suppliers(),
{
    lemma_dedup(supplier_keys(db.partsupp));
    crate::maintenance_semantics::lemma_supplier_partition(db, Seq::empty(), Seq::empty(), 0);
    let list = database_suppliers(db);
    let range = vstd::set_lib::set_int_range(-2147483648, 2147483648);
    vstd::set_lib::lemma_int_range(-2147483648, 2147483648);
    assert forall|supplier: int| list.to_set().contains(supplier) implies range.contains(supplier) by {
        let position = choose|position: int| 0 <= position < db.partsupp.len() && supplier_keys(db.partsupp)[position] == supplier;
        assert(partsupp_in_type(db.partsupp[position]));
    }
    list.unique_seq_to_set();
    vstd::set_lib::lemma_len_subset(list.to_set(), range);
}

pub proof fn lemma_total_bound(db: Db, brand: Seq<u8>, kind: Seq<u8>, size: int)
    ensures 0 <= total(db, brand, kind, size) <= database_suppliers(db).len(),
{
    crate::maintenance_semantics::lemma_supplier_partition(db, brand, kind, size);
    lemma_unit_sum(database_suppliers(db), |supplier: int| crate::maintenance_semantics::qualifier(db, brand, kind, size, supplier));
}

}
