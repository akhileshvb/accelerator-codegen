use vstd::prelude::*;
use verus_common::sums::*;
use verus_common::sqlfns::like_match;
use verus_prooflib::ordered::KeyView;
use crate::gen_obligations::*;
use crate::gen_spec::*;
use crate::model::*;

verus! {

pub open spec fn part_input_sum(rows: Seq<PartInput>, part: int, key: KeyView) -> int {
    sum(part_input_seq_view(rows), |row: PartRow| part_weight(row, part, key))
}

pub open spec fn edge_input_sum(rows: Seq<EdgeInput>, supplier: int, part: int) -> int {
    sum(edge_input_seq_view(rows), |row: PartsuppRow| edge_weight(row, supplier, part))
}

pub open spec fn blocker_weight(key: int, supplier: int) -> int {
    if key == supplier { 1int } else { 0int }
}

pub open spec fn blocker_input_sum(rows: Seq<BlockerInput>, supplier: int) -> int {
    sum(blocker_input_seq_view(rows), |key: int| blocker_weight(key, supplier))
}

pub open spec fn part_delta_sum(rows: Seq<PartDelta>, part: int, key: KeyView) -> int {
    sum(part_delta_seq_view(rows), |row: (PartRow, int)| row.1 * part_weight(row.0, part, key))
}

pub open spec fn edge_delta_sum(rows: Seq<EdgeDelta>, supplier: int, part: int) -> int {
    sum(edge_delta_seq_view(rows), |row: (PartsuppRow, int)| row.1 * edge_weight(row.0, supplier, part))
}

pub open spec fn part_database_sum(db: Db, part: int, key: KeyView) -> int {
    sum(db.part, |row: PartRow| part_weight(row, part, key))
}

pub open spec fn scanned_parts(parts: Parts, db: Db) -> bool {
    forall|part: int, key: KeyView| #[trigger] part_at(parts, part, key)
        == part_database_sum(db, part, key)
}

pub open spec fn scanned_edges(edges: Edges, db: Db) -> bool {
    forall|supplier: int, part: int| #[trigger] edge_at(edges, supplier, part)
        == edge_count(db, part, supplier)
}

pub open spec fn scanned_links(links: Edges, db: Db) -> bool {
    forall|part: int, supplier: int| #[trigger] edge_at(links, part, supplier)
        == edge_count(db, part, supplier)
}

pub open spec fn tables_valid(parts: Parts, edges: Edges, blocked: verus_prooflib::ordered::Index<i128>, links: Edges, db: Db) -> bool {
    &&& parts_keys_valid(parts) && scanned_parts(parts, db)
    &&& edges_keys_valid(edges) && scanned_edges(edges, db)
    &&& integer_keys(blocked) && scanned_blockers(blocked, db)
    &&& edges_keys_valid(links) && scanned_links(links, db)
}

pub open spec fn scanned_blockers(blockers: verus_prooflib::ordered::Index<i128>, db: Db) -> bool {
    forall|supplier: int| #[trigger] integer_at(blockers, (supplier, Seq::empty()))
        == blocker_count(db, supplier)
}

pub open spec fn supplier_delta_sum(rows: Seq<SupplierDelta>, supplier: int) -> int {
    sum(rows, |row: SupplierDelta|
        if row.s_suppkey == supplier && row.is_blocker != 0 { row.sign as int } else { 0int })
}

pub open spec fn supplier_blocker_weight(row: SupplierRow, supplier: int) -> int {
    if row.s_suppkey == supplier && like_match(row.s_comment,
        seq![b'%', b'C', b'u', b's', b't', b'o', b'm', b'e', b'r', b'%', b'C', b'o', b'm', b'p', b'l', b'a', b'i', b'n', b't', b's', b'%']) { 1int } else { 0int }
}

pub proof fn lemma_supplier_projection(rows: Seq<SupplierDelta>, full: Seq<(SupplierRow, int)>, supplier: int)
    requires supplier_delta_ok(rows, full),
    ensures supplier_delta_sum(rows, supplier)
        == sum(full, |row: (SupplierRow, int)| row.1 * supplier_blocker_weight(row.0, supplier)),
{
    let projected = |row: SupplierDelta|
        if row.s_suppkey == supplier && row.is_blocker != 0 { row.sign as int } else { 0int };
    let original = |row: (SupplierRow, int)| row.1 * supplier_blocker_weight(row.0, supplier);
    let indexed = |index: int| original(full[index]);
    assert forall|index: int| 0 <= index < rows.len() implies #[trigger] indexed(index) == projected(rows[index]) by {
        assert(rows[index].sign as int == full[index].1);
    }
    lemma_sum_index(rows, projected, indexed);
    lemma_sum_index(full, original, indexed);
}

pub proof fn lemma_capture_part(db: Db, db2: Db, deltas: Seq<PartDelta>, part: int, key: KeyView)
    requires bag_step(db.part, db2.part, part_delta_seq_view(deltas)),
    ensures part_database_sum(db2, part, key) == part_database_sum(db, part, key) + part_delta_sum(deltas, part, key),
{
    let weight = |row: PartRow| part_weight(row, part, key);
    assert(sum(db2.part, weight) == sum(db.part, weight)
        + sum(part_delta_seq_view(deltas), |row: (PartRow, int)| row.1 * weight(row.0)));
    lemma_sum_ext_all(part_delta_seq_view(deltas),
        |row: (PartRow, int)| row.1 * weight(row.0),
        |row: (PartRow, int)| row.1 * part_weight(row.0, part, key));
}

pub proof fn lemma_capture_edge(db: Db, db2: Db, deltas: Seq<EdgeDelta>, supplier: int, part: int)
    requires bag_step(db.partsupp, db2.partsupp, edge_delta_seq_view(deltas)),
    ensures edge_count(db2, part, supplier) == edge_count(db, part, supplier) + edge_delta_sum(deltas, supplier, part),
{
    let weight = |row: PartsuppRow| edge_weight(row, supplier, part);
    let count = |row: PartsuppRow| if row.ps_partkey == part && row.ps_suppkey == supplier { 1int } else { 0int };
    assert(sum(db2.partsupp, weight) == sum(db.partsupp, weight)
        + sum(edge_delta_seq_view(deltas), |row: (PartsuppRow, int)| row.1 * weight(row.0)));
    lemma_sum_ext_all(db.partsupp, weight, count);
    lemma_sum_ext_all(db2.partsupp, weight, count);
    lemma_sum_ext_all(edge_delta_seq_view(deltas),
        |row: (PartsuppRow, int)| row.1 * weight(row.0),
        |row: (PartsuppRow, int)| row.1 * edge_weight(row.0, supplier, part));
}

pub proof fn lemma_capture_blocker(db: Db, db2: Db, rows: Seq<SupplierDelta>, full: Seq<(SupplierRow, int)>, supplier: int)
    requires supplier_delta_ok(rows, full), bag_step(db.supplier, db2.supplier, full),
    ensures blocker_count(db2, supplier) == blocker_count(db, supplier) + supplier_delta_sum(rows, supplier),
{
    lemma_supplier_projection(rows, full, supplier);
    let weight = |row: SupplierRow| supplier_blocker_weight(row, supplier);
    let count = |row: SupplierRow| if row.s_suppkey == supplier && like_match(row.s_comment,
        seq![b'%', b'C', b'u', b's', b't', b'o', b'm', b'e', b'r', b'%', b'C', b'o', b'm', b'p', b'l', b'a', b'i', b'n', b't', b's', b'%']) { 1int } else { 0int };
    assert(sum(db2.supplier, weight) == sum(db.supplier, weight) + sum(full, |row: (SupplierRow, int)| row.1 * weight(row.0)));
    lemma_sum_ext_all(db.supplier, weight, count);
    lemma_sum_ext_all(db2.supplier, weight, count);
    lemma_sum_ext_all(full, |row: (SupplierRow, int)| row.1 * weight(row.0),
        |row: (SupplierRow, int)| row.1 * supplier_blocker_weight(row.0, supplier));
}

pub proof fn lemma_parts_rescan(db: Db, keys: Seq<i64>, rows: Seq<PartInput>, part: int, key: KeyView)
    requires rescan_parts_for_neighbors_ok(db, keys, rows), seq_view_i64(keys).contains(part),
    ensures part_input_sum(rows, part, key) == part_database_sum(db, part, key),
{
    let weight = |row: PartRow| part_weight(row, part, key);
    let selected = |row: PartRow| if seq_view_i64(keys).contains(row.p_partkey) { weight(row) } else { 0int };
    assert(sum(part_input_seq_view(rows), weight) == sum(db.part, selected));
    lemma_sum_ext_all(db.part, selected, weight);
}

pub proof fn lemma_edges_supplier_rescan(db: Db, keys: Seq<i64>, rows: Seq<EdgeInput>, supplier: int, part: int)
    requires rescan_edges_for_suppliers_ok(db, keys, rows), seq_view_i64(keys).contains(supplier),
    ensures edge_input_sum(rows, supplier, part) == edge_count(db, part, supplier),
{
    let weight = |row: PartsuppRow| edge_weight(row, supplier, part);
    let selected = |row: PartsuppRow| if seq_view_i64(keys).contains(row.ps_suppkey) { weight(row) } else { 0int };
    let count = |row: PartsuppRow| if row.ps_partkey == part && row.ps_suppkey == supplier { 1int } else { 0int };
    assert(sum(edge_input_seq_view(rows), weight) == sum(db.partsupp, selected));
    lemma_sum_ext_all(db.partsupp, selected, count);
}

pub proof fn lemma_blockers_rescan(db: Db, keys: Seq<i64>, rows: Seq<BlockerInput>, supplier: int)
    requires rescan_blockers_for_suppliers_ok(db, keys, rows), seq_view_i64(keys).contains(supplier),
    ensures blocker_input_sum(rows, supplier) == blocker_count(db, supplier),
{
    let weight = |key: int| blocker_weight(key, supplier);
    let selected = |row: SupplierRow| if like_match(row.s_comment,
        seq![b'%', b'C', b'u', b's', b't', b'o', b'm', b'e', b'r', b'%', b'C', b'o', b'm', b'p', b'l', b'a', b'i', b'n', b't', b's', b'%']) && seq_view_i64(keys).contains(row.s_suppkey) { weight(row.s_suppkey) } else { 0int };
    let count = |row: SupplierRow| if row.s_suppkey == supplier && like_match(row.s_comment,
        seq![b'%', b'C', b'u', b's', b't', b'o', b'm', b'e', b'r', b'%', b'C', b'o', b'm', b'p', b'l', b'a', b'i', b'n', b't', b's', b'%']) { 1int } else { 0int };
    assert(sum(blocker_input_seq_view(rows), weight) == sum(db.supplier, selected));
    lemma_sum_ext_all(db.supplier, selected, count);
}

pub proof fn lemma_parts_rescan_absent(db: Db, keys: Seq<i64>, rows: Seq<PartInput>, part: int, key: KeyView)
    requires rescan_parts_for_neighbors_ok(db, keys, rows), !seq_view_i64(keys).contains(part),
    ensures part_input_sum(rows, part, key) == 0,
{
    let weight = |row: PartRow| part_weight(row, part, key);
    let selected = |row: PartRow| if seq_view_i64(keys).contains(row.p_partkey) { weight(row) } else { 0int };
    assert(sum(part_input_seq_view(rows), weight) == sum(db.part, selected));
    assert forall|position: int| 0 <= position < db.part.len() implies selected(#[trigger] db.part[position]) == 0 by {}
    lemma_sum_zero(db.part, selected);
}

pub proof fn lemma_edges_supplier_rescan_absent(db: Db, keys: Seq<i64>, rows: Seq<EdgeInput>, supplier: int, part: int)
    requires rescan_edges_for_suppliers_ok(db, keys, rows), !seq_view_i64(keys).contains(supplier),
    ensures edge_input_sum(rows, supplier, part) == 0,
{
    let weight = |row: PartsuppRow| edge_weight(row, supplier, part);
    let selected = |row: PartsuppRow| if seq_view_i64(keys).contains(row.ps_suppkey) { weight(row) } else { 0int };
    assert(sum(edge_input_seq_view(rows), weight) == sum(db.partsupp, selected));
    assert forall|position: int| 0 <= position < db.partsupp.len() implies selected(#[trigger] db.partsupp[position]) == 0 by {}
    lemma_sum_zero(db.partsupp, selected);
}

pub proof fn lemma_blockers_rescan_absent(db: Db, keys: Seq<i64>, rows: Seq<BlockerInput>, supplier: int)
    requires rescan_blockers_for_suppliers_ok(db, keys, rows), !seq_view_i64(keys).contains(supplier),
    ensures blocker_input_sum(rows, supplier) == 0,
{
    let weight = |key: int| blocker_weight(key, supplier);
    let selected = |row: SupplierRow| if like_match(row.s_comment,
        seq![b'%', b'C', b'u', b's', b't', b'o', b'm', b'e', b'r', b'%', b'C', b'o', b'm', b'p', b'l', b'a', b'i', b'n', b't', b's', b'%']) && seq_view_i64(keys).contains(row.s_suppkey) { weight(row.s_suppkey) } else { 0int };
    assert(sum(blocker_input_seq_view(rows), weight) == sum(db.supplier, selected));
    assert forall|position: int| 0 <= position < db.supplier.len() implies selected(#[trigger] db.supplier[position]) == 0 by {}
    lemma_sum_zero(db.supplier, selected);
}

pub proof fn lemma_parts_scan(db: Db, rows: Seq<PartInput>, part: int, key: KeyView)
    requires scan_parts_ok(db, rows),
    ensures part_input_sum(rows, part, key) == part_database_sum(db, part, key),
{
    let weight = |row: PartRow| part_weight(row, part, key);
    assert(sum(part_input_seq_view(rows), weight) == sum(db.part, weight));
}

pub proof fn lemma_edges_scan(db: Db, rows: Seq<EdgeInput>, supplier: int, part: int)
    requires scan_edges_ok(db, rows),
    ensures edge_input_sum(rows, supplier, part) == edge_count(db, part, supplier),
{
    let weight = |row: PartsuppRow| edge_weight(row, supplier, part);
    let count = |row: PartsuppRow| if row.ps_partkey == part && row.ps_suppkey == supplier { 1int } else { 0int };
    assert(sum(edge_input_seq_view(rows), weight) == sum(db.partsupp, weight));
    assert forall|row: PartsuppRow| #[trigger] weight(row) == count(row) by {}
    lemma_sum_ext_all(db.partsupp, weight, count);
}

pub proof fn lemma_blockers_scan(db: Db, rows: Seq<BlockerInput>, supplier: int)
    requires scan_blockers_ok(db, rows),
    ensures blocker_input_sum(rows, supplier) == blocker_count(db, supplier),
{
    let weight = |key: int| blocker_weight(key, supplier);
    let selected = |row: SupplierRow| if like_match(row.s_comment,
        seq![b'%', b'C', b'u', b's', b't', b'o', b'm', b'e', b'r', b'%', b'C', b'o', b'm', b'p', b'l', b'a', b'i', b'n', b't', b's', b'%']) {
        weight(row.s_suppkey)
    } else { 0int };
    let count = |row: SupplierRow| if row.s_suppkey == supplier && like_match(row.s_comment,
        seq![b'%', b'C', b'u', b's', b't', b'o', b'm', b'e', b'r', b'%', b'C', b'o', b'm', b'p', b'l', b'a', b'i', b'n', b't', b's', b'%']) { 1int } else { 0int };
    assert(sum(blocker_input_seq_view(rows), weight) == sum(db.supplier, selected));
    assert(blocker_input_sum(rows, supplier) == sum(blocker_input_seq_view(rows), weight));
    assert(blocker_count(db, supplier) == sum(db.supplier, count));
    assert forall|row: SupplierRow| #[trigger] selected(row) == count(row) by {}
    lemma_sum_ext_all(db.supplier, selected, count);
}

pub proof fn lemma_parts_prefix(rows: Seq<PartInput>, index: int)
    requires 0 <= index < rows.len(),
    ensures forall|part: int, key: KeyView|
        #[trigger] part_input_sum(rows.take(index + 1), part, key)
            == part_input_sum(rows.take(index), part, key)
                + part_weight(part_input_view(rows[index]), part, key),
{
    assert forall|part: int, key: KeyView|
        #[trigger] part_input_sum(rows.take(index + 1), part, key)
            == part_input_sum(rows.take(index), part, key)
                + part_weight(part_input_view(rows[index]), part, key) by {
        verus_prooflib::lemma_sum_mapped_prefix(rows, index, |row: PartInput| part_input_view(row), |row: PartRow| part_weight(row, part, key));
    }
}

pub proof fn lemma_edges_prefix(rows: Seq<EdgeInput>, index: int)
    requires 0 <= index < rows.len(),
    ensures forall|supplier: int, part: int|
        #[trigger] edge_input_sum(rows.take(index + 1), supplier, part)
            == edge_input_sum(rows.take(index), supplier, part)
                + edge_weight(edge_input_view(rows[index]), supplier, part),
{
    assert forall|supplier: int, part: int|
        #[trigger] edge_input_sum(rows.take(index + 1), supplier, part)
            == edge_input_sum(rows.take(index), supplier, part)
                + edge_weight(edge_input_view(rows[index]), supplier, part) by {
        verus_prooflib::lemma_sum_mapped_prefix(rows, index, |row: EdgeInput| edge_input_view(row), |row: PartsuppRow| edge_weight(row, supplier, part));
    }
}

pub proof fn lemma_blockers_prefix(rows: Seq<BlockerInput>, index: int)
    requires 0 <= index < rows.len(),
    ensures forall|supplier: int|
        #[trigger] blocker_input_sum(rows.take(index + 1), supplier)
            == blocker_input_sum(rows.take(index), supplier)
                + if rows[index].s_suppkey == supplier { 1int } else { 0int },
{
    assert forall|supplier: int|
        #[trigger] blocker_input_sum(rows.take(index + 1), supplier)
            == blocker_input_sum(rows.take(index), supplier)
                + if rows[index].s_suppkey == supplier { 1int } else { 0int } by {
        verus_prooflib::lemma_sum_mapped_prefix(rows, index, |row: BlockerInput| blocker_input_view(row), |key: int| blocker_weight(key, supplier));
    }
}

}
