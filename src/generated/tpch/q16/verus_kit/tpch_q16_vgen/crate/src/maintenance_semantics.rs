use vstd::prelude::*;
use verus_common::sums::*;
use verus_common::rel::*;
use verus_prooflib::ordered::{Index, KeyView};
use crate::gen_spec::*;
use crate::model::*;
use crate::build_semantics::{lemma_qualified_members, lemma_qualified_witness};
use crate::input_proofs::*;
use crate::gen_obligations::*;
use crate::maintenance::Changes;

verus! {

pub open spec fn database_suppliers(db: Db) -> Seq<int> {
    dedup(supplier_keys(db.partsupp))
}

pub open spec fn qualifier(db: Db, brand: Seq<u8>, kind: Seq<u8>, size: int, supplier: int) -> int {
    if qualifying_supplier(db, brand, kind, size, supplier) { 1int } else { 0int }
}

pub open spec fn captured_counts(changes: Changes, db: Db, db2: Db) -> bool {
    &&& forall|part: int, key: KeyView| #[trigger] part_at(changes.part_counts(), part, key)
        == part_database_sum(db2, part, key) - part_database_sum(db, part, key)
    &&& forall|supplier: int, part: int| #[trigger] edge_at(changes.edge_counts(), supplier, part)
        == edge_count(db2, part, supplier) - edge_count(db, part, supplier)
    &&& forall|supplier: int| #[trigger] integer_at(changes.blocker_counts(), (supplier, Seq::<u8>::empty()))
        == blocker_count(db2, supplier) - blocker_count(db, supplier)
}

pub open spec fn affected_valid(changes: Changes, db2: Db, suppliers: Index<i64>) -> bool {
    &&& forall|supplier: int| #[trigger] suppliers.at((supplier, Seq::<u8>::empty())).is_none() ==>
        changes.edge_counts().at((supplier, Seq::<u8>::empty())).is_none()
            && changes.blocker_counts().at((supplier, Seq::<u8>::empty())).is_none()
    &&& forall|supplier: int, part: int| #[trigger] edge_count(db2, part, supplier) != 0
        && suppliers.at((supplier, Seq::<u8>::empty())).is_none() ==>
            changes.part_counts().at((part, Seq::<u8>::empty())).is_none()
}

pub open spec fn scoped_edges(edges: Edges, db: Db, suppliers: Index<i64>) -> bool {
    forall|supplier: int, part: int| #[trigger] edge_at(edges, supplier, part)
        == if suppliers.at((supplier, Seq::<u8>::empty())).is_some() { edge_count(db, part, supplier) } else { 0int }
}

pub open spec fn scoped_parts(parts: Parts, db: Db, neighbors: Index<i64>) -> bool {
    forall|part: int, key: KeyView| #[trigger] part_at(parts, part, key)
        == if neighbors.at((part, Seq::<u8>::empty())).is_some() { part_database_sum(db, part, key) } else { 0int }
}

pub open spec fn scoped_blockers(blockers: Index<i64>, db: Db, suppliers: Index<i64>) -> bool {
    forall|supplier: int| #[trigger] integer_at(blockers, (supplier, Seq::<u8>::empty()))
        == if suppliers.at((supplier, Seq::<u8>::empty())).is_some() { blocker_count(db, supplier) } else { 0int }
}

pub open spec fn neighbors_cover(neighbors: Index<i64>, edges: Edges) -> bool {
    forall|supplier: int, part: int| #[trigger] edge_at(edges, supplier, part) != 0 ==>
        neighbors.at((part, Seq::<u8>::empty())).is_some()
}

pub proof fn lemma_affected(changes: &Changes, db2: Db, suppliers: Index<i64>, changed_parts: Index<i64>, initial: Index<i64>,
    keys: Seq<i64>, rows: Seq<EdgeInput>,
)
    requires key_set(changed_parts), key_set(suppliers), key_set(initial),
        rescan_edges_for_parts_ok(db2, keys, rows),
        forall|part: int| #[trigger] integer_at(changed_parts, (part, Seq::<u8>::empty()))
            == if changes.part_counts().at((part, Seq::<u8>::empty())).is_some() { 1int } else { 0int },
        forall|part: int| #[trigger] changed_parts.at((part, Seq::<u8>::empty())).is_some() ==> seq_view_i64(keys).contains(part),
        forall|supplier: int| #[trigger] integer_at(initial, (supplier, Seq::<u8>::empty()))
            == if changes.edge_counts().at((supplier, Seq::<u8>::empty())).is_some()
                || changes.blocker_counts().at((supplier, Seq::<u8>::empty())).is_some() { 1int } else { 0int },
        forall|supplier: int| #[trigger] integer_at(suppliers, (supplier, Seq::<u8>::empty()))
            == if integer_at(initial, (supplier, Seq::<u8>::empty())) > 0
                || sum(edge_input_seq_view(rows), |row: PartsuppRow| if row.ps_suppkey == supplier
                    && integer_at(changed_parts, (row.ps_partkey, Seq::<u8>::empty())) > 0 { 1int } else { 0int }) > 0 { 1int } else { 0int },
    ensures affected_valid(*changes, db2, suppliers),
{
    assert forall|supplier: int| #[trigger] suppliers.at((supplier, Seq::<u8>::empty())).is_none() implies
        changes.edge_counts().at((supplier, Seq::<u8>::empty())).is_none()
            && changes.blocker_counts().at((supplier, Seq::<u8>::empty())).is_none() by {
        assert(integer_at(suppliers, (supplier, Seq::<u8>::empty())) == 0);
        assert(integer_at(initial, (supplier, Seq::<u8>::empty())) <= 0);
    };
    assert forall|supplier: int, part: int| #[trigger] edge_count(db2, part, supplier) != 0
        && suppliers.at((supplier, Seq::<u8>::empty())).is_none() implies
            changes.part_counts().at((part, Seq::<u8>::empty())).is_none() by {
        if changes.part_counts().at((part, Seq::<u8>::empty())).is_some() {
            assert(integer_at(suppliers, (supplier, Seq::<u8>::empty())) == 0);
            assert(integer_at(changed_parts, (part, Seq::<u8>::empty())) == 1);
            assert(changed_parts.at((part, Seq::<u8>::empty())).is_some());
            lemma_edges_part_rescan(db2, keys, rows, supplier, part);
            let matching = |row: PartsuppRow| edge_weight(row, supplier, part);
            let affected = |row: PartsuppRow| if row.ps_suppkey == supplier
                && integer_at(changed_parts, (row.ps_partkey, Seq::<u8>::empty())) > 0 { 1int } else { 0int };
            lemma_sum_le(edge_input_seq_view(rows), matching, affected);
            lemma_sum_nonneg(edge_input_seq_view(rows), matching);
            assert(sum(edge_input_seq_view(rows), affected) <= 0);
            assert(edge_count(db2, part, supplier) == 0);
        }
    }
}

pub proof fn lemma_post_edges(db: Db, suppliers: Index<i64>, edges: Edges, keys: Seq<i64>, rows: Seq<EdgeInput>)
    requires key_set(suppliers), rescan_edges_for_suppliers_ok(db, keys, rows),
        forall|supplier: int| #[trigger] suppliers.at((supplier, Seq::<u8>::empty())).is_some() ==> seq_view_i64(keys).contains(supplier),
        forall|supplier: int, part: int| #[trigger] edge_at(edges, supplier, part) == sum(edge_input_seq_view(rows),
            |row: PartsuppRow| if integer_at(suppliers, (supplier, Seq::<u8>::empty())) > 0 { edge_weight(row, supplier, part) } else { 0int }),
    ensures scoped_edges(edges, db, suppliers),
{
    assert forall|supplier: int, part: int| #[trigger] edge_at(edges, supplier, part)
        == if suppliers.at((supplier, Seq::<u8>::empty())).is_some() { edge_count(db, part, supplier) } else { 0int } by {
        let filtered = |row: PartsuppRow| if integer_at(suppliers, (supplier, Seq::<u8>::empty())) > 0 { edge_weight(row, supplier, part) } else { 0int };
        if suppliers.at((supplier, Seq::<u8>::empty())).is_some() {
            lemma_edges_supplier_rescan(db, keys, rows, supplier, part);
            lemma_sum_ext_all(edge_input_seq_view(rows), filtered, |row: PartsuppRow| edge_weight(row, supplier, part));
        } else { lemma_sum_zero(edge_input_seq_view(rows), filtered); }
    }
}

pub proof fn lemma_post_parts(db: Db, neighbors: Index<i64>, parts: Parts, keys: Seq<i64>, rows: Seq<PartInput>)
    requires key_set(neighbors), rescan_parts_for_neighbors_ok(db, keys, rows),
        forall|part: int| #[trigger] neighbors.at((part, Seq::<u8>::empty())).is_some() ==> seq_view_i64(keys).contains(part),
        forall|part: int, key: KeyView| #[trigger] part_at(parts, part, key) == sum(part_input_seq_view(rows),
            |row: PartRow| if integer_at(neighbors, (part, Seq::<u8>::empty())) > 0 { part_weight(row, part, key) } else { 0int }),
    ensures scoped_parts(parts, db, neighbors),
{
    assert forall|part: int, key: KeyView| #[trigger] part_at(parts, part, key)
        == if neighbors.at((part, Seq::<u8>::empty())).is_some() { part_database_sum(db, part, key) } else { 0int } by {
        let filtered = |row: PartRow| if integer_at(neighbors, (part, Seq::<u8>::empty())) > 0 { part_weight(row, part, key) } else { 0int };
        if neighbors.at((part, Seq::<u8>::empty())).is_some() {
            lemma_parts_rescan(db, keys, rows, part, key);
            lemma_sum_ext_all(part_input_seq_view(rows), filtered, |row: PartRow| part_weight(row, part, key));
        } else { lemma_sum_zero(part_input_seq_view(rows), filtered); }
    }
}

pub proof fn lemma_post_blockers(db: Db, suppliers: Index<i64>, blockers: Index<i64>, keys: Seq<i64>, rows: Seq<BlockerInput>)
    requires key_set(suppliers), rescan_blockers_for_suppliers_ok(db, keys, rows),
        forall|supplier: int| #[trigger] suppliers.at((supplier, Seq::<u8>::empty())).is_some() ==> seq_view_i64(keys).contains(supplier),
        forall|supplier: int| #[trigger] integer_at(blockers, (supplier, Seq::<u8>::empty())) == sum(blocker_input_seq_view(rows),
            |key: int| if key == supplier && integer_at(suppliers, (supplier, Seq::<u8>::empty())) > 0 { 1int } else { 0int }),
    ensures scoped_blockers(blockers, db, suppliers),
{
    assert forall|supplier: int| #[trigger] integer_at(blockers, (supplier, Seq::<u8>::empty()))
        == if suppliers.at((supplier, Seq::<u8>::empty())).is_some() { blocker_count(db, supplier) } else { 0int } by {
        let filtered = |key: int| if key == supplier && integer_at(suppliers, (supplier, Seq::<u8>::empty())) > 0 { 1int } else { 0int };
        if suppliers.at((supplier, Seq::<u8>::empty())).is_some() {
            lemma_blockers_rescan(db, keys, rows, supplier);
            lemma_sum_ext_all(blocker_input_seq_view(rows), filtered, |key: int| blocker_weight(key, supplier));
        } else { lemma_sum_zero(blocker_input_seq_view(rows), filtered); }
    }
}

pub proof fn lemma_restore_scopes(db: Db, db2: Db, changes: &Changes, suppliers: Index<i64>, neighbors: Index<i64>,
    old_parts: Parts, old_edges: Edges, old_blockers: Index<i64>,
    new_parts: Parts, new_edges: Edges, new_blockers: Index<i64>,
)
    requires captured_counts(*changes, db, db2), affected_valid(*changes, db2, suppliers),
        scoped_edges(new_edges, db2, suppliers), scoped_parts(new_parts, db2, neighbors), scoped_blockers(new_blockers, db2, suppliers),
        forall|supplier: int, part: int| #[trigger] edge_at(old_edges, supplier, part)
            == edge_at(new_edges, supplier, part) - edge_at(changes.edge_counts(), supplier, part),
        forall|part: int, key: KeyView| #[trigger] part_at(old_parts, part, key)
            == part_at(new_parts, part, key) - if neighbors.at((part, Seq::<u8>::empty())).is_some()
                { part_at(changes.part_counts(), part, key) } else { 0int },
        forall|supplier: int| #[trigger] integer_at(old_blockers, (supplier, Seq::<u8>::empty()))
            == integer_at(new_blockers, (supplier, Seq::<u8>::empty())) - integer_at(changes.blocker_counts(), (supplier, Seq::<u8>::empty())),
    ensures scoped_edges(old_edges, db, suppliers), scoped_parts(old_parts, db, neighbors), scoped_blockers(old_blockers, db, suppliers),
{
    assert forall|supplier: int, part: int| #[trigger] edge_at(old_edges, supplier, part)
        == if suppliers.at((supplier, Seq::<u8>::empty())).is_some() { edge_count(db, part, supplier) } else { 0int } by {
        if suppliers.at((supplier, Seq::<u8>::empty())).is_none() {
            assert(edge_at(changes.edge_counts(), supplier, part) == 0);
        }
    }
    assert forall|part: int, key: KeyView| #[trigger] part_at(old_parts, part, key)
        == if neighbors.at((part, Seq::<u8>::empty())).is_some() { part_database_sum(db, part, key) } else { 0int } by {
        if neighbors.at((part, Seq::<u8>::empty())).is_some() {
            assert(part_at(changes.part_counts(), part, key) == part_database_sum(db2, part, key) - part_database_sum(db, part, key));
        }
    }
    assert forall|supplier: int| #[trigger] integer_at(old_blockers, (supplier, Seq::<u8>::empty()))
        == if suppliers.at((supplier, Seq::<u8>::empty())).is_some() { blocker_count(db, supplier) } else { 0int } by {
        if suppliers.at((supplier, Seq::<u8>::empty())).is_none() {
            assert(integer_at(changes.blocker_counts(), (supplier, Seq::<u8>::empty())) == 0);
        }
    }
}

pub proof fn lemma_changes_capture(changes: &Changes, db: Db, db2: Db,
    parts: Seq<PartDelta>, edges: Seq<EdgeDelta>, suppliers: Seq<SupplierDelta>, full: Seq<(SupplierRow, int)>,
)
    requires supplier_delta_ok(suppliers, full),
        capture_ok(db, db2, part_delta_seq_view(parts), edge_delta_seq_view(edges), full),
        forall|part: int, key: KeyView| #[trigger] part_at(changes.part_counts(), part, key) == part_delta_sum(parts, part, key),
        forall|supplier: int, part: int| #[trigger] edge_at(changes.edge_counts(), supplier, part) == edge_delta_sum(edges, supplier, part),
        forall|supplier: int| #[trigger] integer_at(changes.blocker_counts(), (supplier, Seq::<u8>::empty())) == supplier_delta_sum(suppliers, supplier),
    ensures captured_counts(*changes, db, db2),
{
    assert forall|part: int, key: KeyView| #[trigger] part_at(changes.part_counts(), part, key)
        == part_database_sum(db2, part, key) - part_database_sum(db, part, key) by {
        lemma_capture_part(db, db2, parts, part, key);
    }
    assert forall|supplier: int, part: int| #[trigger] edge_at(changes.edge_counts(), supplier, part)
        == edge_count(db2, part, supplier) - edge_count(db, part, supplier) by {
        lemma_capture_edge(db, db2, edges, supplier, part);
    }
    assert forall|supplier: int| #[trigger] integer_at(changes.blocker_counts(), (supplier, Seq::<u8>::empty()))
        == blocker_count(db2, supplier) - blocker_count(db, supplier) by {
        lemma_capture_blocker(db, db2, suppliers, full, supplier);
    }
}

pub open spec fn part_support_by_key(db: Db, supplier: int, part: int, key: KeyView) -> int {
    part_database_sum(db, part, key) * edge_count(db, part, supplier)
}

pub proof fn lemma_support_grouping(db: Db, keys: Seq<int>, brand: Seq<u8>, kind: Seq<u8>, size: int, supplier: int)
    requires distinct(keys),
        forall|position: int| 0 <= position < db.part.len() ==> keys.contains((#[trigger] db.part[position]).p_partkey),
    ensures support(db, supplier, brand, kind, size) == sum(keys,
        |part: int| part_support_by_key(db, supplier, part, group_key(brand, kind, size))),
{
    let key = group_key(brand, kind, size);
    let project = |row: PartRow| row.p_partkey;
    let weight = |row: PartRow| support_term(db, supplier, brand, kind, size, row);
    let per = |part: int| part_support_by_key(db, supplier, part, key);
    assert forall|part: int| #[trigger] per(part) == sum(db.part,
        |row: PartRow| if project(row) == part { weight(row) } else { 0int }) by {
        let raw = |row: PartRow| part_weight(row, part, key);
        let scaled = |row: PartRow| if project(row) == part { weight(row) } else { 0int };
        let factor = edge_count(db, part, supplier);
        assert forall|row: PartRow| #[trigger] scaled(row) == factor * raw(row) by {
            crate::build_semantics::lemma_part_group_weight(row, part, brand, kind, size);
        }
        lemma_sum_scale(db.part, raw, scaled, factor);
        assert(per(part) == part_database_sum(db, part, key) * factor);
        let multiplicity = part_database_sum(db, part, key);
        assert(factor * multiplicity == multiplicity * factor) by (nonlinear_arith);
    }
    lemma_sum_group(db.part, keys, project, weight, per);
}

pub proof fn lemma_unaffected_supplier(db: Db, db2: Db, brand: Seq<u8>, kind: Seq<u8>, size: int, supplier: int)
    requires
        forall|part: int| #[trigger] edge_count(db2, part, supplier) == edge_count(db, part, supplier),
        forall|part: int| #[trigger] edge_count(db2, part, supplier) != 0 ==>
            part_database_sum(db2, part, group_key(brand, kind, size)) == part_database_sum(db, part, group_key(brand, kind, size)),
        blocker_count(db2, supplier) == blocker_count(db, supplier),
    ensures qualifying_supplier(db2, brand, kind, size, supplier) == qualifying_supplier(db, brand, kind, size, supplier),
{
    let source = db.part.map_values(|row: PartRow| row.p_partkey)
        + db2.part.map_values(|row: PartRow| row.p_partkey);
    let keys = dedup(source);
    lemma_dedup(source);
    assert(distinct(keys));
    assert forall|position: int| 0 <= position < db.part.len() implies keys.contains((#[trigger] db.part[position]).p_partkey) by {
        assert(source[position] == db.part[position].p_partkey);
        assert(source.contains(db.part[position].p_partkey));
    }
    assert forall|position: int| 0 <= position < db2.part.len() implies keys.contains((#[trigger] db2.part[position]).p_partkey) by {
        assert(source[db.part.len() + position] == db2.part[position].p_partkey);
        assert(source.contains(db2.part[position].p_partkey));
    }
    lemma_support_grouping(db, keys, brand, kind, size, supplier);
    lemma_support_grouping(db2, keys, brand, kind, size, supplier);
    lemma_sum_ext_all(keys,
        |part: int| part_support_by_key(db, supplier, part, group_key(brand, kind, size)),
        |part: int| part_support_by_key(db2, supplier, part, group_key(brand, kind, size)));
}

pub proof fn lemma_local_contribution(parts: Parts, edges: Edges, blockers: Index<i64>, db: Db,
    supplier: int, brand: Seq<u8>, kind: Seq<u8>, size: int,
)
    requires parts_keys_valid(parts), edges_keys_valid(edges),
        forall|part: int| #[trigger] edge_at(edges, supplier, part) == edge_count(db, part, supplier),
        forall|part: int| #[trigger] edge_at(edges, supplier, part) != 0 ==>
            part_at(parts, part, group_key(brand, kind, size)) == part_database_sum(db, part, group_key(brand, kind, size)),
        integer_at(blockers, (supplier, Seq::<u8>::empty())) == blocker_count(db, supplier),
    ensures supplier_contribution(parts, edges, blockers, supplier, group_key(brand, kind, size)) == qualifier(db, brand, kind, size, supplier),
{
    crate::build_semantics::lemma_join_support(parts, edges, db, supplier, brand, kind, size);
}

pub proof fn lemma_supplier_partition(db: Db, brand: Seq<u8>, kind: Seq<u8>, size: int)
    ensures distinct(database_suppliers(db)), database_suppliers(db).no_duplicates(),
        total(db, brand, kind, size) == sum(database_suppliers(db), |supplier: int| qualifier(db, brand, kind, size, supplier)),
        forall|supplier: int| #[trigger] qualifier(db, brand, kind, size, supplier) == sum(database_suppliers(db),
            |other: int| if other == supplier { qualifier(db, brand, kind, size, other) } else { 0int }),
{
    let source = supplier_keys(db.partsupp);
    let selected = qualified_keys(db.partsupp, db, brand, kind, size);
    let keys = dedup(source);
    let live = dedup(selected);
    lemma_dedup(source);
    lemma_dedup(selected);
    assert(keys.no_duplicates());
    assert(distinct(keys));
    assert(distinct(live));
    let indicator = |supplier: int| qualifier(db, brand, kind, size, supplier);
    assert forall|supplier: int| #[trigger] indicator(supplier) == sum(live,
        |other: int| if other == supplier { 1int } else { 0int }) by {
        lemma_qualified_members(db.partsupp, db, brand, kind, size, supplier);
        if indicator(supplier) != 0 { lemma_qualified_witness(db, brand, kind, size, supplier); }
        let matching = |other: int| if other == supplier { indicator(supplier) } else { 0int };
        lemma_sum_indicator_distinct(live, supplier, indicator(supplier), matching);
        assert forall|position: int| 0 <= position < live.len() implies
            matching(#[trigger] live[position]) == if live[position] == supplier { 1int } else { 0int } by {
            if live[position] == supplier {
                assert(live.contains(supplier));
                assert(selected.contains(supplier));
            }
        }
        lemma_sum_ext(live, matching, |other: int| if other == supplier { 1int } else { 0int });
    }
    assert forall|position: int| 0 <= position < live.len() implies keys.contains(#[trigger] live[position]) by {
        assert(live.contains(live[position]));
        assert(selected.contains(live[position]));
        lemma_qualified_members(db.partsupp, db, brand, kind, size, live[position]);
    }
    let identity = |supplier: int| supplier;
    let one = |supplier: int| 1int;
    assert forall|supplier: int| #[trigger] indicator(supplier) == sum(live,
        |other: int| if identity(other) == supplier { one(other) } else { 0int }) by {
        lemma_sum_ext_all(live, |other: int| if other == supplier { 1int } else { 0int },
            |other: int| if identity(other) == supplier { one(other) } else { 0int });
    }
    lemma_sum_group(live, keys, identity, one, indicator);
    verus_prooflib::lemma_sum_ones(live);
    assert forall|supplier: int| #[trigger] qualifier(db, brand, kind, size, supplier) == sum(keys,
        |other: int| if other == supplier { qualifier(db, brand, kind, size, other) } else { 0int }) by {
        if indicator(supplier) != 0 {
            lemma_qualified_witness(db, brand, kind, size, supplier);
            assert(keys.contains(supplier));
        }
        let matching = |other: int| if other == supplier { indicator(supplier) } else { 0int };
        lemma_sum_indicator_distinct(keys, supplier, indicator(supplier), matching);
        lemma_sum_ext_all(keys, matching,
            |other: int| if other == supplier { qualifier(db, brand, kind, size, other) } else { 0int });
    }
}

pub proof fn lemma_total_delta(db: Db, db2: Db, suppliers: Index<i64>,
    old_parts: Parts, old_edges: Edges, old_blockers: Index<i64>,
    new_parts: Parts, new_edges: Edges, new_blockers: Index<i64>,
    brand: Seq<u8>, kind: Seq<u8>, size: int,
)
    requires key_set(suppliers),
        forall|supplier: int| #[trigger] suppliers.at((supplier, Seq::<u8>::empty())).is_none() ==>
            qualifying_supplier(db, brand, kind, size, supplier) == qualifying_supplier(db2, brand, kind, size, supplier),
        forall|supplier: int| #[trigger] suppliers.at((supplier, Seq::<u8>::empty())).is_some() ==>
            supplier_contribution(old_parts, old_edges, old_blockers, supplier, group_key(brand, kind, size))
                == qualifier(db, brand, kind, size, supplier)
            && supplier_contribution(new_parts, new_edges, new_blockers, supplier, group_key(brand, kind, size))
                == qualifier(db2, brand, kind, size, supplier),
    ensures total(db2, brand, kind, size) - total(db, brand, kind, size)
        == sum(suppliers.entries(), |row: (KeyView, i64)|
            supplier_contribution(new_parts, new_edges, new_blockers, row.0.0, group_key(brand, kind, size))
                - supplier_contribution(old_parts, old_edges, old_blockers, row.0.0, group_key(brand, kind, size))),
{
    suppliers.model();
    verus_prooflib::ordered::lemma_ordered_unique(suppliers.entries());
    let project = |row: (KeyView, i64)| row.0.0;
    let affected = suppliers.entries().map_values(project);
    assert(affected.no_duplicates()) by {
        assert forall|first: int, last: int| 0 <= first < last < affected.len() implies
            (#[trigger] affected[first]) != (#[trigger] affected[last]) by {
            assert(suppliers.at(suppliers.entries()[first].0).is_some());
            assert(suppliers.at(suppliers.entries()[last].0).is_some());
        }
    }
    lemma_supplier_partition(db, brand, kind, size);
    lemma_supplier_partition(db2, brand, kind, size);
    let before = |supplier: int| qualifier(db, brand, kind, size, supplier);
    let after = |supplier: int| qualifier(db2, brand, kind, size, supplier);
    let identity = |supplier: int| supplier;
    assert forall|supplier: int| #[trigger] before(supplier) == sum(database_suppliers(db),
        |other: int| if identity(other) == supplier { before(other) } else { 0int }) by {
        lemma_sum_ext_all(database_suppliers(db),
            |other: int| if other == supplier { qualifier(db, brand, kind, size, other) } else { 0int },
            |other: int| if identity(other) == supplier { before(other) } else { 0int });
    }
    assert forall|supplier: int| #[trigger] after(supplier) == sum(database_suppliers(db2),
        |other: int| if identity(other) == supplier { after(other) } else { 0int }) by {
        lemma_sum_ext_all(database_suppliers(db2),
            |other: int| if other == supplier { qualifier(db2, brand, kind, size, other) } else { 0int },
            |other: int| if identity(other) == supplier { after(other) } else { 0int });
    }
    assert forall|supplier: int| !affected.contains(supplier) implies #[trigger] after(supplier) == before(supplier) by {
        if suppliers.at((supplier, Seq::<u8>::empty())).is_some() {
            let position = choose|position: int| 0 <= position < suppliers.entries().len()
                && (#[trigger] suppliers.entries()[position]).0 == (supplier, Seq::<u8>::empty());
            assert(affected[position] == supplier);
            assert(affected.contains(supplier));
        }
    }
    verus_prooflib::lemma_sparse_partition_delta(database_suppliers(db), database_suppliers(db2), affected,
        identity, before, after, before, after);
    let difference = |supplier: int| after(supplier) - before(supplier);
    let projected = |row: (KeyView, i64)| difference(project(row));
    lemma_sum_map(suppliers.entries(), project, difference, projected);
    lemma_sum_ext_all(affected, |supplier: int| after(supplier) - before(supplier), difference);
    assert forall|position: int| 0 <= position < suppliers.entries().len() implies
        projected(#[trigger] suppliers.entries()[position]) ==
            supplier_contribution(new_parts, new_edges, new_blockers, suppliers.entries()[position].0.0, group_key(brand, kind, size))
                - supplier_contribution(old_parts, old_edges, old_blockers, suppliers.entries()[position].0.0, group_key(brand, kind, size)) by {
        assert(suppliers.at(suppliers.entries()[position].0).is_some());
    }
    lemma_sum_ext(suppliers.entries(), projected, |row: (KeyView, i64)|
        supplier_contribution(new_parts, new_edges, new_blockers, row.0.0, group_key(brand, kind, size))
            - supplier_contribution(old_parts, old_edges, old_blockers, row.0.0, group_key(brand, kind, size)));
}

pub proof fn lemma_maintenance_result(db: Db, db2: Db, changes: &Changes, suppliers: Index<i64>, neighbors: Index<i64>,
    old_parts: Parts, old_edges: Edges, old_blockers: Index<i64>,
    new_parts: Parts, new_edges: Edges, new_blockers: Index<i64>,
    before: Counts, after: Counts, group_changes: Counts, ranks: Ranks,
)
    requires counts_valid(before, db), positive_counts(after), ranks_valid(ranks, after),
        captured_counts(*changes, db, db2), affected_valid(*changes, db2, suppliers), key_set(suppliers),
        parts_keys_valid(old_parts), parts_keys_valid(new_parts), edges_keys_valid(old_edges), edges_keys_valid(new_edges),
        scoped_edges(old_edges, db, suppliers), scoped_edges(new_edges, db2, suppliers),
        scoped_parts(old_parts, db, neighbors), scoped_parts(new_parts, db2, neighbors),
        scoped_blockers(old_blockers, db, suppliers), scoped_blockers(new_blockers, db2, suppliers),
        neighbors_cover(neighbors, old_edges), neighbors_cover(neighbors, new_edges),
        forall|key: KeyView| #[trigger] count_at(after, key) == count_at(before, key) + count_at(group_changes, key),
        forall|key: KeyView| #[trigger] count_at(group_changes, key) == sum(suppliers.entries(), |row: (KeyView, i64)|
            supplier_contribution(new_parts, new_edges, new_blockers, row.0.0, key)
                - supplier_contribution(old_parts, old_edges, old_blockers, row.0.0, key)),
    ensures counts_valid(after, db2),
{
    assert forall|brand: Seq<u8>, kind: Seq<u8>, size: int|
        #[trigger] count_at(after, group_key(brand, kind, size)) == total(db2, brand, kind, size) by {
        let key = group_key(brand, kind, size);
        assert forall|supplier: int| #[trigger] suppliers.at((supplier, Seq::<u8>::empty())).is_none() implies
            qualifying_supplier(db, brand, kind, size, supplier) == qualifying_supplier(db2, brand, kind, size, supplier) by {
            assert forall|part: int| #[trigger] edge_count(db2, part, supplier) == edge_count(db, part, supplier) by {
                assert(edge_at(changes.edge_counts(), supplier, part) == 0);
            }
            assert forall|part: int| #[trigger] edge_count(db2, part, supplier) != 0 implies
                part_database_sum(db2, part, key) == part_database_sum(db, part, key) by {
                assert(changes.part_counts().at((part, Seq::<u8>::empty())).is_none());
                assert(part_at(changes.part_counts(), part, key) == 0);
            }
            assert(integer_at(changes.blocker_counts(), (supplier, Seq::<u8>::empty())) == 0);
            lemma_unaffected_supplier(db, db2, brand, kind, size, supplier);
        }
        assert forall|supplier: int| #[trigger] suppliers.at((supplier, Seq::<u8>::empty())).is_some() implies
            supplier_contribution(old_parts, old_edges, old_blockers, supplier, key) == qualifier(db, brand, kind, size, supplier)
                && supplier_contribution(new_parts, new_edges, new_blockers, supplier, key) == qualifier(db2, brand, kind, size, supplier) by {
            assert forall|part: int| #[trigger] edge_at(old_edges, supplier, part) != 0 implies
                part_at(old_parts, part, key) == part_database_sum(db, part, key) by {
                assert(neighbors.at((part, Seq::<u8>::empty())).is_some());
            }
            assert forall|part: int| #[trigger] edge_at(new_edges, supplier, part) != 0 implies
                part_at(new_parts, part, key) == part_database_sum(db2, part, key) by {
                assert(neighbors.at((part, Seq::<u8>::empty())).is_some());
            }
            lemma_local_contribution(old_parts, old_edges, old_blockers, db, supplier, brand, kind, size);
            lemma_local_contribution(new_parts, new_edges, new_blockers, db2, supplier, brand, kind, size);
        }
        lemma_total_delta(db, db2, suppliers, old_parts, old_edges, old_blockers, new_parts, new_edges, new_blockers, brand, kind, size);
    }
}

}
