use vstd::prelude::*;
use verus_common::sums::*;
use verus_common::rel::*;
use verus_prooflib::ordered::{Index, KeyView};
use crate::gen_spec::*;
use crate::model::*;
use crate::build_semantics::{lemma_qualified_members, lemma_qualified_witness};
use crate::input_proofs::*;
use crate::gen_obligations::*;

verus! {

pub open spec fn database_suppliers(db: Db) -> Seq<int> {
    dedup(supplier_keys(db.partsupp))
}

pub open spec fn qualifier(db: Db, brand: Seq<u8>, kind: Seq<u8>, size: int, supplier: int) -> int {
    if qualifying_supplier(db, brand, kind, size, supplier) { 1int } else { 0int }
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

pub proof fn lemma_local_contribution(parts: Parts, edges: Edges, blockers: Index<i128>, db: Db,
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

pub proof fn lemma_total_delta(db: Db, db2: Db, suppliers: Index<i128>,
    old_parts: Parts, old_edges: Edges, old_blockers: Index<i128>,
    new_parts: Parts, new_edges: Edges, new_blockers: Index<i128>,
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
        == sum(suppliers.entries(), |row: (KeyView, i128)|
            supplier_contribution(new_parts, new_edges, new_blockers, row.0.0, group_key(brand, kind, size))
                - supplier_contribution(old_parts, old_edges, old_blockers, row.0.0, group_key(brand, kind, size))),
{
    suppliers.model();
    verus_prooflib::ordered::lemma_ordered_unique(suppliers.entries());
    let project = |row: (KeyView, i128)| row.0.0;
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
    let projected = |row: (KeyView, i128)| difference(project(row));
    lemma_sum_map(suppliers.entries(), project, difference, projected);
    lemma_sum_ext_all(affected, |supplier: int| after(supplier) - before(supplier), difference);
    assert forall|position: int| 0 <= position < suppliers.entries().len() implies
        projected(#[trigger] suppliers.entries()[position]) ==
            supplier_contribution(new_parts, new_edges, new_blockers, suppliers.entries()[position].0.0, group_key(brand, kind, size))
                - supplier_contribution(old_parts, old_edges, old_blockers, suppliers.entries()[position].0.0, group_key(brand, kind, size)) by {
        assert(suppliers.at(suppliers.entries()[position].0).is_some());
    }
    lemma_sum_ext(suppliers.entries(), projected, |row: (KeyView, i128)|
        supplier_contribution(new_parts, new_edges, new_blockers, row.0.0, group_key(brand, kind, size))
            - supplier_contribution(old_parts, old_edges, old_blockers, row.0.0, group_key(brand, kind, size)));
}

pub open spec fn unaffected(db: Db, db2: Db, suppliers: Index<i128>) -> bool {
    &&& forall|supplier: int, part: int| suppliers.at((supplier, Seq::<u8>::empty())).is_none() ==>
        #[trigger] edge_count(db2, part, supplier) == edge_count(db, part, supplier)
    &&& forall|supplier: int| suppliers.at((supplier, Seq::<u8>::empty())).is_none() ==>
        #[trigger] blocker_count(db2, supplier) == blocker_count(db, supplier)
    &&& forall|supplier: int, part: int, key: KeyView| #![trigger edge_count(db2, part, supplier), part_database_sum(db, part, key)]
        suppliers.at((supplier, Seq::<u8>::empty())).is_none() && edge_count(db2, part, supplier) != 0
            ==> part_database_sum(db2, part, key) == part_database_sum(db, part, key)
}

pub proof fn lemma_changes_are_totals(db: Db, db2: Db, suppliers: Index<i128>,
    old_parts: Parts, old_edges: Edges, old_blockers: Index<i128>,
    new_parts: Parts, new_edges: Edges, new_blockers: Index<i128>, group_changes: Counts,
)
    requires key_set(suppliers),
        parts_keys_valid(old_parts), parts_keys_valid(new_parts), edges_keys_valid(old_edges), edges_keys_valid(new_edges),
        scanned_parts(old_parts, db), scanned_edges(old_edges, db), scanned_blockers(old_blockers, db),
        scanned_parts(new_parts, db2), scanned_edges(new_edges, db2), scanned_blockers(new_blockers, db2),
        unaffected(db, db2, suppliers),
        forall|key: KeyView| #[trigger] count_at(group_changes, key) == sum(suppliers.entries(), |row: (KeyView, i128)|
            supplier_contribution(new_parts, new_edges, new_blockers, row.0.0, key)
                - supplier_contribution(old_parts, old_edges, old_blockers, row.0.0, key)),
    ensures forall|brand: Seq<u8>, kind: Seq<u8>, size: int|
            total(db2, brand, kind, size) - total(db, brand, kind, size) == #[trigger] count_at(group_changes, group_key(brand, kind, size)),
{
    assert forall|brand: Seq<u8>, kind: Seq<u8>, size: int|
        total(db2, brand, kind, size) - total(db, brand, kind, size) == #[trigger] count_at(group_changes, group_key(brand, kind, size)) by {
        let key = group_key(brand, kind, size);
        assert forall|supplier: int| #[trigger] suppliers.at((supplier, Seq::<u8>::empty())).is_none() implies
            qualifying_supplier(db, brand, kind, size, supplier) == qualifying_supplier(db2, brand, kind, size, supplier) by {
            lemma_unaffected_supplier(db, db2, brand, kind, size, supplier);
        }
        assert forall|supplier: int| #[trigger] suppliers.at((supplier, Seq::<u8>::empty())).is_some() implies
            supplier_contribution(old_parts, old_edges, old_blockers, supplier, key) == qualifier(db, brand, kind, size, supplier)
                && supplier_contribution(new_parts, new_edges, new_blockers, supplier, key) == qualifier(db2, brand, kind, size, supplier) by {
            lemma_local_contribution(old_parts, old_edges, old_blockers, db, supplier, brand, kind, size);
            lemma_local_contribution(new_parts, new_edges, new_blockers, db2, supplier, brand, kind, size);
        }
        lemma_total_delta(db, db2, suppliers, old_parts, old_edges, old_blockers, new_parts, new_edges, new_blockers, brand, kind, size);
    }
}

pub proof fn lemma_apply_correct(db: Db, db2: Db, before: Counts, after: Counts, group_changes: Counts, ranks: Ranks)
    requires counts_valid(before, db), positive_counts(after), ranks_valid(ranks, after),
        forall|key: KeyView| #[trigger] count_at(after, key) == count_at(before, key) + count_at(group_changes, key),
        forall|brand: Seq<u8>, kind: Seq<u8>, size: int|
            total(db2, brand, kind, size) - total(db, brand, kind, size) == #[trigger] count_at(group_changes, group_key(brand, kind, size)),
    ensures counts_valid(after, db2),
{
    assert forall|brand: Seq<u8>, kind: Seq<u8>, size: int|
        #[trigger] count_at(after, group_key(brand, kind, size)) == total(db2, brand, kind, size) by {}
}

}
