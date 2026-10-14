use vstd::prelude::*;
use verus_common::sums::*;
use verus_common::rel::*;
use verus_prooflib::ordered::{KeyView, Index};
use crate::gen_spec::*;
use crate::model::*;
use crate::input_proofs::*;
use crate::encoding::*;

verus! {

pub proof fn lemma_part_group_weight(row: PartRow, part: int, brand: Seq<u8>, kind: Seq<u8>, size: int)
    ensures part_weight(row, part, group_key(brand, kind, size)) ==
        if row.p_partkey == part && row.p_brand == brand && row.p_type == kind && row.p_size == size { 1int } else { 0int },
{
    if group_key(row.p_brand, row.p_type, row.p_size) == group_key(brand, kind, size) {
        lemma_group_text_identity(row.p_brand, row.p_type, brand, kind);
    }
}

pub open spec fn supplier_join(parts: Parts, edges: Edges, supplier: int, key: KeyView) -> int {
    match edges.at((supplier, Seq::empty())) {
        Some(neighborhood) => joined_count(parts, neighborhood, key),
        None => 0int,
    }
}

pub proof fn lemma_join_support(parts: Parts, edges: Edges, db: Db, supplier: int, brand: Seq<u8>, kind: Seq<u8>, size: int)
    requires parts_keys_valid(parts), edges_keys_valid(edges),
        forall|part: int| #[trigger] edge_at(edges, supplier, part) == edge_count(db, part, supplier),
        forall|part: int| #[trigger] edge_at(edges, supplier, part) != 0 ==>
            part_at(parts, part, group_key(brand, kind, size)) == part_database_sum(db, part, group_key(brand, kind, size)),
    ensures supplier_join(parts, edges, supplier, group_key(brand, kind, size)) == support(db, supplier, brand, kind, size),
{
    let key = group_key(brand, kind, size);
    let matching = |row: PartRow| row.p_brand == brand && row.p_type == kind && row.p_size == size;
    let target = |row: PartRow| support_term(db, supplier, brand, kind, size, row);
    if edges.at((supplier, Seq::empty())).is_some() {
        let neighborhood = edges.at((supplier, Seq::empty())).unwrap();
        neighborhood.model();
        let pair = |edge: (KeyView, i64), row: PartRow| if edge.0 == (row.p_partkey, Seq::<u8>::empty()) && matching(row) { edge.1 as int } else { 0int };
        let horizontal = |edge: (KeyView, i64)| sum(db.part, |row: PartRow| pair(edge, row));
        let vertical = |row: PartRow| sum(neighborhood.entries(), |edge: (KeyView, i64)| pair(edge, row));
        lemma_sum_swap(neighborhood.entries(), db.part, pair, horizontal, vertical);
        let original = |edge: (KeyView, i64)| edge.1 as int * part_at(parts, edge.0.0, key);
        assert forall|position: int| 0 <= position < neighborhood.entries().len() implies
            horizontal(#[trigger] neighborhood.entries()[position]) == original(neighborhood.entries()[position]) by {
            let edge = neighborhood.entries()[position];
            assert(neighborhood.at(edge.0).is_some());
            assert(edge.0.1 == Seq::<u8>::empty());
            let raw = |row: PartRow| part_weight(row, edge.0.0, key);
            let scaled = |row: PartRow| pair(edge, row);
            assert forall|row: PartRow| #[trigger] scaled(row) == edge.1 as int * raw(row) by {
                lemma_part_group_weight(row, edge.0.0, brand, kind, size);
            }
            lemma_sum_scale(db.part, raw, scaled, edge.1 as int);
            assert(edge_at(edges, supplier, edge.0.0) == edge.1 as int);
            assert(part_database_sum(db, edge.0.0, key) == sum(db.part, raw));
            assert(horizontal(edge) == sum(db.part, scaled));
            if edge.1 != 0 {
                assert(part_at(parts, edge.0.0, key) == part_database_sum(db, edge.0.0, key));
            }
            assert(original(edge) == edge.1 as int * part_at(parts, edge.0.0, key));
            assert(sum(db.part, scaled) == edge.1 as int * sum(db.part, raw));
            if edge.1 == 0 {
                let a = sum(db.part, raw);
                let b = part_at(parts, edge.0.0, key);
                assert(edge.1 as int * a == 0) by(nonlinear_arith) requires edge.1 == 0;
                assert(edge.1 as int * b == 0) by(nonlinear_arith) requires edge.1 == 0;
            } else {
                assert(part_at(parts, edge.0.0, key) == sum(db.part, raw));
            }
            assert(horizontal(edge) == original(edge));
        }
        lemma_sum_ext(neighborhood.entries(), horizontal, original);
        assert forall|row: PartRow| #[trigger] vertical(row) == target(row) by {
            let factor = if matching(row) { 1int } else { 0int };
            verus_prooflib::lemma_ordered_selected_weight(&neighborhood, (row.p_partkey, Seq::empty()),
                |edge: (KeyView, i64)| edge.1 as int, factor, |edge: (KeyView, i64)| pair(edge, row));
            assert(verus_prooflib::ordered_weight_at(neighborhood, (row.p_partkey, Seq::empty()),
                |edge: (KeyView, i64)| edge.1 as int) == integer_at(neighborhood, (row.p_partkey, Seq::empty())));
            assert(edge_at(edges, supplier, row.p_partkey) == integer_at(neighborhood, (row.p_partkey, Seq::empty())));
            assert(edge_at(edges, supplier, row.p_partkey) == edge_count(db, row.p_partkey, supplier));
        }
        lemma_sum_ext_all(db.part, vertical, target);
    } else {
        assert forall|position: int| 0 <= position < db.part.len() implies target(#[trigger] db.part[position]) == 0int by {
            assert(edge_at(edges, supplier, db.part[position].p_partkey) == 0int);
        }
        lemma_sum_zero(db.part, target);
    }
}

pub proof fn lemma_qualified_witness(db: Db, brand: Seq<u8>, kind: Seq<u8>, size: int, supplier: int)
    requires qualifying_supplier(db, brand, kind, size, supplier),
    ensures supplier_keys(db.partsupp).contains(supplier),
{
    let part_weight = |row: PartRow| support_term(db, supplier, brand, kind, size, row);
    let part_position = lemma_sum_nonzero_witness(db.part, part_weight);
    let part = db.part[part_position].p_partkey;
    let edge_weight = |row: PartsuppRow| if row.ps_partkey == part && row.ps_suppkey == supplier { 1int } else { 0int };
    assert(sum(db.partsupp, edge_weight) != 0);
    let edge_position = lemma_sum_nonzero_witness(db.partsupp, edge_weight);
    assert(supplier_keys(db.partsupp)[edge_position] == supplier);
}

pub proof fn lemma_qualified_members(rows: Seq<PartsuppRow>, db: Db, brand: Seq<u8>, kind: Seq<u8>, size: int, supplier: int)
    ensures qualified_keys(rows, db, brand, kind, size).contains(supplier)
        <==> qualifying_supplier(db, brand, kind, size, supplier) && supplier_keys(rows).contains(supplier),
    decreases rows.len(),
{
    broadcast use vstd::seq_lib::group_seq_properties;
    reveal_with_fuel(Seq::filter, 2);
    let predicate = |row: PartsuppRow| qualifying_supplier(db, brand, kind, size, row.ps_suppkey);
    if rows.len() == 0 {
        assert(qualified_keys(rows, db, brand, kind, size).len() == 0);
        assert(supplier_keys(rows).len() == 0);
    } else {
        lemma_qualified_members(rows.drop_last(), db, brand, kind, size, supplier);
        let previous = rows.drop_last().filter(predicate);
        assert(supplier_keys(rows) =~= supplier_keys(rows.drop_last()).push(rows.last().ps_suppkey));
        verus_prooflib::lemma_push_membership(supplier_keys(rows.drop_last()), rows.last().ps_suppkey);
        if predicate(rows.last()) {
            assert(rows.filter(predicate) =~= previous.push(rows.last()));
            assert(qualified_keys(rows, db, brand, kind, size)
                =~= qualified_keys(rows.drop_last(), db, brand, kind, size).push(rows.last().ps_suppkey));
            verus_prooflib::lemma_push_membership(qualified_keys(rows.drop_last(), db, brand, kind, size), rows.last().ps_suppkey);
        } else {
            assert(rows.filter(predicate) =~= previous);
        }
    }
}

pub proof fn lemma_directory_contains(edges: Edges, db: Db, position: int)
    requires edges_keys_valid(edges), scanned_edges(edges, db), 0 <= position < db.partsupp.len(),
    ensures edges.at((db.partsupp[position].ps_suppkey, Seq::<u8>::empty())).is_some(),
{
    let row = db.partsupp[position];
    let weight = |edge: PartsuppRow| if edge.ps_partkey == row.ps_partkey && edge.ps_suppkey == row.ps_suppkey { 1int } else { 0int };
    lemma_sum_pos(db.partsupp, weight, position);
    assert(edge_count(db, row.ps_partkey, row.ps_suppkey) > 0);
    assert(edge_at(edges, row.ps_suppkey, row.ps_partkey) > 0);
}

pub proof fn lemma_build_total(parts: Parts, edges: Edges, blockers: Index<i64>, db: Db, brand: Seq<u8>, kind: Seq<u8>, size: int)
    requires parts_keys_valid(parts), edges_keys_valid(edges), integer_keys(blockers),
        scanned_parts(parts, db), scanned_edges(edges, db), scanned_blockers(blockers, db),
    ensures total(db, brand, kind, size) == sum(edges.entries(),
        |row: (KeyView, Index<i64>)| supplier_weight(parts, blockers, row, group_key(brand, kind, size))),
{
    edges.model();
    let key = group_key(brand, kind, size);
    let selected = qualified_keys(db.partsupp, db, brand, kind, size);
    let keys = dedup(selected);
    lemma_dedup(selected);
    assert(distinct(keys));
    let qualified = |supplier: int| if qualifying_supplier(db, brand, kind, size, supplier) { 1int } else { 0int };
    let weight = |row: (KeyView, Index<i64>)| supplier_weight(parts, blockers, row, key);
    let project = |row: (KeyView, Index<i64>)| row.0.0;
    assert forall|position: int| 0 <= position < edges.entries().len() implies
        weight(#[trigger] edges.entries()[position]) == qualified(edges.entries()[position].0.0) by {
        let row = edges.entries()[position];
        assert(edges.at(row.0) == Some(row.1));
        assert(row.0.1 == Seq::<u8>::empty());
        lemma_join_support(parts, edges, db, row.0.0, brand, kind, size);
        assert(integer_at(blockers, row.0) == blocker_count(db, row.0.0));
    }
    assert forall|supplier: int| #[trigger] qualified(supplier) ==
        sum(edges.entries(), |row: (KeyView, Index<i64>)| if project(row) == supplier { weight(row) } else { 0int }) by {
        if qualified(supplier) != 0 {
            lemma_qualified_witness(db, brand, kind, size, supplier);
            let position = choose|position: int| 0 <= position < db.partsupp.len()
                && #[trigger] supplier_keys(db.partsupp)[position] == supplier;
            lemma_directory_contains(edges, db, position);
        }
        let selected_weight = |row: (KeyView, Index<i64>)| if row.0 == (supplier, Seq::<u8>::empty()) { qualified(row.0.0) } else { 0int };
        verus_prooflib::lemma_ordered_selected_weight(&edges, (supplier, Seq::empty()),
            |row: (KeyView, Index<i64>)| qualified(row.0.0), 1int, selected_weight);
        assert(sum(edges.entries(), selected_weight) == qualified(supplier));
        assert forall|position: int| 0 <= position < edges.entries().len() implies
            selected_weight(#[trigger] edges.entries()[position]) ==
                (if project(edges.entries()[position]) == supplier { weight(edges.entries()[position]) } else { 0int }) by {
            assert(edges.at(edges.entries()[position].0).is_some());
            assert(edges.entries()[position].0.1 == Seq::<u8>::empty());
        }
        lemma_sum_ext(edges.entries(), selected_weight,
            |row: (KeyView, Index<i64>)| if project(row) == supplier { weight(row) } else { 0int });
    }
    assert forall|position: int| 0 <= position < edges.entries().len() && weight(edges.entries()[position]) != 0 implies
        keys.contains(project(#[trigger] edges.entries()[position])) by {
        let supplier = edges.entries()[position].0.0;
        lemma_qualified_witness(db, brand, kind, size, supplier);
        lemma_qualified_members(db.partsupp, db, brand, kind, size, supplier);
    }
    lemma_sum_group(edges.entries(), keys, project, weight, qualified);
    assert forall|position: int| 0 <= position < keys.len() implies qualified(#[trigger] keys[position]) == 1int by {
        assert(keys.contains(keys[position]));
        assert(selected.contains(keys[position]));
        lemma_qualified_members(db.partsupp, db, brand, kind, size, keys[position]);
    }
    lemma_sum_ext(keys, qualified, |supplier: int| 1int);
    verus_prooflib::lemma_sum_ones(keys);
    assert(total(db, brand, kind, size) == keys.len() as int);
}

}
