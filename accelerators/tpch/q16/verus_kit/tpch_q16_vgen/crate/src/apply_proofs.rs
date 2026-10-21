use vstd::prelude::*;
use verus_common::sums::*;
use verus_prooflib::ordered::{Index, KeyView};
use crate::gen_obligations::*;
use crate::gen_spec::*;
use crate::model::*;
use crate::input_proofs::*;
use crate::wanted::*;
use crate::maintenance_semantics::*;
use crate::maintenance::*;

verus! {

pub proof fn lemma_edge_delta_unseen(edges: Seq<EdgeDelta>, supplier: int, part: int)
    requires !edge_seen(edges, edges.len() as int, supplier),
    ensures edge_delta_sum(edges, supplier, part) == 0,
{
    let weight = |row: (PartsuppRow, int)| row.1 * edge_weight(row.0, supplier, part);
    assert forall|position: int| 0 <= position < edge_delta_seq_view(edges).len() implies weight(#[trigger] edge_delta_seq_view(edges)[position]) == 0 by {
        assert(edge_delta_seq_view(edges)[position] == (edge_input_view(edges[position].row), edges[position].sign as int));
        assert(edges[position].row.ps_suppkey as int != supplier);
    }
    lemma_sum_zero(edge_delta_seq_view(edges), weight);
}

pub proof fn lemma_supplier_delta_unseen(suppliers: Seq<SupplierDelta>, supplier: int)
    requires !supplier_seen(suppliers, suppliers.len() as int, supplier),
    ensures supplier_delta_sum(suppliers, supplier) == 0,
{
    let weight = |row: SupplierDelta| if row.s_suppkey as int == supplier && row.is_blocker != 0 { row.sign as int } else { 0int };
    assert forall|position: int| 0 <= position < suppliers.len() implies weight(#[trigger] suppliers[position]) == 0 by {
        assert(suppliers[position].s_suppkey as int != supplier);
    }
    lemma_sum_zero(suppliers, weight);
}

pub proof fn lemma_part_delta_unseen(parts: Seq<PartDelta>, part: int, key: KeyView)
    requires !part_seen(parts, parts.len() as int, part),
    ensures part_delta_sum(parts, part, key) == 0,
{
    let weight = |row: (PartRow, int)| row.1 * part_weight(row.0, part, key);
    assert forall|position: int| 0 <= position < part_delta_seq_view(parts).len() implies weight(#[trigger] part_delta_seq_view(parts)[position]) == 0 by {
        assert(part_delta_seq_view(parts)[position] == (part_input_view(parts[position].row), parts[position].sign as int));
        assert(parts[position].row.p_partkey as int != part);
    }
    lemma_sum_zero(part_delta_seq_view(parts), weight);
}

pub proof fn lemma_coverage(db: Db, db2: Db, links: Edges, parts: Seq<PartDelta>, edges: Seq<EdgeDelta>, suppliers_delta: Seq<SupplierDelta>,
    full: Seq<(SupplierRow, int)>, suppliers: Index<i64>, changed: Index<i64>,
)
    requires supplier_delta_ok(suppliers_delta, full),
        capture_ok(db, db2, part_delta_seq_view(parts), edge_delta_seq_view(edges), full),
        scanned_links(links, db),
        forall|key: int| suppliers.at((key, Seq::<u8>::empty())).is_some() == supplier_wanted(links, parts, edges, suppliers_delta, key),
        forall|key: int| changed.at((key, Seq::<u8>::empty())).is_some() == part_wanted(parts, key),
    ensures unaffected(db, db2, suppliers), part_unchanged(db, db2, changed),
{
    assert forall|supplier: int, part: int| suppliers.at((supplier, Seq::<u8>::empty())).is_none() implies
        #[trigger] edge_count(db2, part, supplier) == edge_count(db, part, supplier) by {
        assert(!supplier_wanted(links, parts, edges, suppliers_delta, supplier));
        lemma_edge_delta_unseen(edges, supplier, part);
        lemma_capture_edge(db, db2, edges, supplier, part);
    }
    assert forall|supplier: int| suppliers.at((supplier, Seq::<u8>::empty())).is_none() implies
        #[trigger] blocker_count(db2, supplier) == blocker_count(db, supplier) by {
        assert(!supplier_wanted(links, parts, edges, suppliers_delta, supplier));
        lemma_supplier_delta_unseen(suppliers_delta, supplier);
        lemma_capture_blocker(db, db2, suppliers_delta, full, supplier);
    }
    assert forall|supplier: int, part: int, key: KeyView| #![trigger edge_count(db2, part, supplier), part_database_sum(db, part, key)]
        suppliers.at((supplier, Seq::<u8>::empty())).is_none() && edge_count(db2, part, supplier) != 0
            implies part_database_sum(db2, part, key) == part_database_sum(db, part, key) by {
        assert(!supplier_wanted(links, parts, edges, suppliers_delta, supplier));
        lemma_edge_delta_unseen(edges, supplier, part);
        lemma_capture_edge(db, db2, edges, supplier, part);
        assert(edge_at(links, part, supplier) == edge_count(db, part, supplier));
        if part_seen(parts, parts.len() as int, part) {
            let position = choose|position: int| 0 <= position < parts.len() && (#[trigger] parts[position]).row.p_partkey as int == part;
            assert(edge_at(links, parts[position].row.p_partkey as int, supplier) != 0);
            assert(linked_seen(links, parts, parts.len() as int, supplier));
        }
        lemma_part_delta_unseen(parts, part, key);
        lemma_capture_part(db, db2, parts, part, key);
    }
    assert forall|part: int, key: KeyView| changed.at((part, Seq::<u8>::empty())).is_none() implies
        #[trigger] part_database_sum(db2, part, key) == part_database_sum(db, part, key) by {
        assert(!part_wanted(parts, part));
        lemma_part_delta_unseen(parts, part, key);
        lemma_capture_part(db, db2, parts, part, key);
    }
}

}
