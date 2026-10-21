use vstd::prelude::*;
use verus_common::sums::*;
use verus_common::rel::*;
use verus_prooflib::ordered::{Index, KeyView};
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::model::*;
use crate::input_proofs::*;
use crate::counts::*;
use crate::bounds::*;
use crate::maintenance_semantics::database_suppliers;

verus! {

pub proof fn lemma_suppliers_distinct(db: Db)
    ensures distinct(database_suppliers(db)),
{
    lemma_dedup(supplier_keys(db.partsupp));
    let list = database_suppliers(db);
    assert forall|i: int, j: int| 0 <= i < list.len() && 0 <= j < list.len() && i != j implies list[i] != list[j] by {
        if i < j { assert(list[i] != list[j]); } else { assert(list[j] != list[i]); }
    }
}

pub open spec fn contributes(parts: Parts, edges: Edges, blockers: Index<i128>, key: KeyView) -> spec_fn(int) -> int {
    |supplier: int| supplier_contribution(parts, edges, blockers, supplier, key)
}

pub proof fn lemma_contribution_unit(parts: Parts, edges: Edges, blockers: Index<i128>, supplier: int, key: KeyView)
    ensures 0 <= supplier_contribution(parts, edges, blockers, supplier, key) <= 1,
{
}

pub proof fn lemma_contribution_listed(parts: Parts, edges: Edges, blockers: Index<i128>, db: Db, supplier: int, key: KeyView)
    requires nonneg_parts(parts), edges_keys_valid(edges), scanned_edges(edges, db),
    ensures supplier_contribution(parts, edges, blockers, supplier, key) != 0 ==> database_suppliers(db).contains(supplier),
{
    if supplier_contribution(parts, edges, blockers, supplier, key) != 0 {
        let rows = edges.at((supplier, Seq::<u8>::empty())).unwrap();
        rows.model();
        let weight = |row: (KeyView, i128)| row.1 as int * part_at(parts, row.0.0, key);
        assert(joined_count(parts, rows, key) > 0);
        let position = lemma_sum_nonzero_witness(rows.entries(), weight);
        let entry = rows.entries()[position];
        assert(entry.1 != 0) by {
            if entry.1 == 0 { assert(weight(entry) == 0); }
        }
        assert(rows.at(entry.0) == Some(entry.1));
        assert(entry.0.1 == Seq::<u8>::empty());
        assert(edge_at(edges, supplier, entry.0.0) == entry.1 as int);
        lemma_edge_supplier_listed(db, entry.0.0, supplier);
    }
}

pub proof fn lemma_contribution_bound(parts: Parts, edges: Edges, blockers: Index<i128>, db: Db, keys: Seq<int>, key: KeyView)
    requires nonneg_parts(parts), edges_keys_valid(edges), scanned_edges(edges, db), distinct(keys),
    ensures 0 <= sum(keys, contributes(parts, edges, blockers, key)) <= database_suppliers(db).len(),
{
    let weight = contributes(parts, edges, blockers, key);
    assert forall|supplier: int| 0 <= #[trigger] weight(supplier) <= 1 by {
        lemma_contribution_unit(parts, edges, blockers, supplier, key);
    }
    assert forall|position: int| 0 <= position < keys.len() && weight(#[trigger] keys[position]) != 0
        implies database_suppliers(db).contains(keys[position]) by {
        lemma_contribution_listed(parts, edges, blockers, db, keys[position], key);
    }
    lemma_suppliers_distinct(db);
    lemma_prefix_bound(keys, database_suppliers(db), weight);
    lemma_sum_nonneg(keys, weight);
}

pub proof fn lemma_edges_prefix_bound(parts: Parts, edges: Edges, blockers: Index<i128>, db: Db, count: int, key: KeyView)
    requires nonneg_parts(parts), edges_keys_valid(edges), scanned_edges(edges, db), 0 <= count <= edges.entries().len(),
    ensures 0 <= sum(edges.entries().take(count),
            |row: (KeyView, Index<i128>)| supplier_weight(parts, blockers, row, key)) <= database_suppliers(db).len(),
{
    edges.model();
    verus_prooflib::ordered::lemma_ordered_unique(edges.entries());
    let rows = edges.entries().take(count);
    let project = |row: (KeyView, Index<i128>)| row.0.0;
    let f = contributes(parts, edges, blockers, key);
    let h = |row: (KeyView, Index<i128>)| supplier_weight(parts, blockers, row, key);
    let h2 = |row: (KeyView, Index<i128>)| f(project(row));
    assert forall|position: int| 0 <= position < rows.len() implies h(#[trigger] rows[position]) == h2(rows[position]) by {
        let row = rows[position];
        assert(row == edges.entries()[position]);
        assert(edges.at(row.0) == Some(row.1));
        assert(row.0.1 == Seq::<u8>::empty());
        assert(row.0 == (row.0.0, Seq::<u8>::empty()));
    }
    lemma_sum_ext(rows, h, h2);
    lemma_sum_map(rows, project, f, h2);
    let keys = rows.map_values(project);
    assert(distinct(keys)) by {
        assert forall|first: int, last: int| 0 <= first < keys.len() && 0 <= last < keys.len() && first != last implies keys[first] != keys[last] by {
            let low = if first < last { first } else { last };
            let high = if first < last { last } else { first };
            assert(edges.entries()[low].0 != edges.entries()[high].0);
            assert(edges.at(edges.entries()[low].0).is_some());
            assert(edges.at(edges.entries()[high].0).is_some());
            assert(rows[low] == edges.entries()[low] && rows[high] == edges.entries()[high]);
        }
    }
    lemma_contribution_bound(parts, edges, blockers, db, keys, key);
}

pub open spec fn partial(parts: Parts, edges: Edges, blockers: Index<i128>, entries: Seq<(KeyView, i128)>, count: int, key: KeyView) -> int {
    sum(entries.take(count), |row: (KeyView, i128)| supplier_contribution(parts, edges, blockers, row.0.0, key))
}

pub proof fn lemma_partial_step(parts: Parts, edges: Edges, blockers: Index<i128>, entries: Seq<(KeyView, i128)>, count: int, key: KeyView)
    requires 0 <= count < entries.len(),
    ensures partial(parts, edges, blockers, entries, count + 1, key)
        == partial(parts, edges, blockers, entries, count, key) + supplier_contribution(parts, edges, blockers, entries[count].0.0, key),
{
    lemma_sum_take(entries, count, |row: (KeyView, i128)| supplier_contribution(parts, edges, blockers, row.0.0, key));
}

pub proof fn lemma_partial_bound(parts: Parts, edges: Edges, blockers: Index<i128>, db: Db, entries: Seq<(KeyView, i128)>, count: int, key: KeyView)
    requires nonneg_parts(parts), edges_keys_valid(edges), scanned_edges(edges, db),
        verus_prooflib::ordered::ordered_rows(entries), forall|position: int| 0 <= position < entries.len() ==> (#[trigger] entries[position]).0.1 == Seq::<u8>::empty(),
        0 <= count <= entries.len(),
    ensures 0 <= partial(parts, edges, blockers, entries, count, key) <= database_suppliers(db).len(),
{
    verus_prooflib::ordered::lemma_ordered_unique(entries);
    let rows = entries.take(count);
    let project = |row: (KeyView, i128)| row.0.0;
    let f = contributes(parts, edges, blockers, key);
    let h = |row: (KeyView, i128)| supplier_contribution(parts, edges, blockers, row.0.0, key);
    let h2 = |row: (KeyView, i128)| f(project(row));
    assert forall|position: int| 0 <= position < rows.len() implies h(#[trigger] rows[position]) == h2(rows[position]) by {}
    lemma_sum_ext(rows, h, h2);
    lemma_sum_map(rows, project, f, h2);
    let keys = rows.map_values(project);
    assert(distinct(keys)) by {
        assert forall|first: int, last: int| 0 <= first < keys.len() && 0 <= last < keys.len() && first != last implies keys[first] != keys[last] by {
            let low = if first < last { first } else { last };
            let high = if first < last { last } else { first };
            assert(entries[low].0 != entries[high].0);
            assert(rows[low] == entries[low] && rows[high] == entries[high]);
        }
    }
    lemma_contribution_bound(parts, edges, blockers, db, keys, key);
}

pub proof fn lemma_difference_split(old_parts: Parts, old_edges: Edges, old_blockers: Index<i128>,
    new_parts: Parts, new_edges: Edges, new_blockers: Index<i128>, entries: Seq<(KeyView, i128)>, count: int, key: KeyView)
    ensures sum(entries.take(count), |row: (KeyView, i128)|
            supplier_contribution(new_parts, new_edges, new_blockers, row.0.0, key)
                - supplier_contribution(old_parts, old_edges, old_blockers, row.0.0, key))
        == partial(new_parts, new_edges, new_blockers, entries, count, key) - partial(old_parts, old_edges, old_blockers, entries, count, key),
{
    lemma_sum_sub(entries.take(count),
        |row: (KeyView, i128)| supplier_contribution(new_parts, new_edges, new_blockers, row.0.0, key),
        |row: (KeyView, i128)| supplier_contribution(old_parts, old_edges, old_blockers, row.0.0, key),
        |row: (KeyView, i128)| supplier_contribution(new_parts, new_edges, new_blockers, row.0.0, key)
            - supplier_contribution(old_parts, old_edges, old_blockers, row.0.0, key));
}

pub proof fn lemma_old_call_fits(old_parts: Parts, old_edges: Edges, old_blockers: Index<i128>,
    new_parts: Parts, new_edges: Edges, new_blockers: Index<i128>, db: Db, db2: Db,
    entries: Seq<(KeyView, i128)>, count: int, key: KeyView)
    requires nonneg_parts(old_parts), nonneg_parts(new_parts), edges_keys_valid(old_edges), edges_keys_valid(new_edges),
        scanned_edges(old_edges, db), scanned_edges(new_edges, db2),
        verus_prooflib::ordered::ordered_rows(entries), forall|position: int| 0 <= position < entries.len() ==> (#[trigger] entries[position]).0.1 == Seq::<u8>::empty(),
        0 <= count < entries.len(), envelope(db), envelope(db2),
    ensures !crate::counts::out_of_range(
        sum(entries.take(count), |row: (KeyView, i128)|
            supplier_contribution(new_parts, new_edges, new_blockers, row.0.0, key)
                - supplier_contribution(old_parts, old_edges, old_blockers, row.0.0, key))
        - supplier_contribution(old_parts, old_edges, old_blockers, entries[count].0.0, key)),
{
    lemma_difference_split(old_parts, old_edges, old_blockers, new_parts, new_edges, new_blockers, entries, count, key);
    lemma_partial_bound(new_parts, new_edges, new_blockers, db2, entries, count, key);
    lemma_partial_bound(old_parts, old_edges, old_blockers, db, entries, count + 1, key);
    lemma_partial_step(old_parts, old_edges, old_blockers, entries, count, key);
    lemma_suppliers_in_type(db);
    lemma_suppliers_in_type(db2);
}

pub proof fn lemma_new_call_fits(old_parts: Parts, old_edges: Edges, old_blockers: Index<i128>,
    new_parts: Parts, new_edges: Edges, new_blockers: Index<i128>, db: Db, db2: Db,
    entries: Seq<(KeyView, i128)>, count: int, key: KeyView)
    requires nonneg_parts(old_parts), nonneg_parts(new_parts), edges_keys_valid(old_edges), edges_keys_valid(new_edges),
        scanned_edges(old_edges, db), scanned_edges(new_edges, db2),
        verus_prooflib::ordered::ordered_rows(entries), forall|position: int| 0 <= position < entries.len() ==> (#[trigger] entries[position]).0.1 == Seq::<u8>::empty(),
        0 <= count < entries.len(), envelope(db), envelope(db2),
    ensures !crate::counts::out_of_range(
        sum(entries.take(count), |row: (KeyView, i128)|
            supplier_contribution(new_parts, new_edges, new_blockers, row.0.0, key)
                - supplier_contribution(old_parts, old_edges, old_blockers, row.0.0, key))
        - supplier_contribution(old_parts, old_edges, old_blockers, entries[count].0.0, key)
        + supplier_contribution(new_parts, new_edges, new_blockers, entries[count].0.0, key)),
{
    lemma_difference_split(old_parts, old_edges, old_blockers, new_parts, new_edges, new_blockers, entries, count, key);
    lemma_partial_bound(new_parts, new_edges, new_blockers, db2, entries, count + 1, key);
    lemma_partial_bound(old_parts, old_edges, old_blockers, db, entries, count + 1, key);
    lemma_partial_step(old_parts, old_edges, old_blockers, entries, count, key);
    lemma_partial_step(new_parts, new_edges, new_blockers, entries, count, key);
    lemma_suppliers_in_type(db);
    lemma_suppliers_in_type(db2);
}

}
