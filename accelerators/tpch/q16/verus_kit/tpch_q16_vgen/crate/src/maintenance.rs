use vstd::prelude::*;
use std::sync::Arc;
use verus_prooflib::ordered::{Index, Key};
use crate::gen_obligations::*;
use crate::model::*;
use crate::counts::*;
use crate::gen_spec::*;
use verus_common::sums::*;
use crate::input_proofs::*;
use crate::maintenance_proofs::*;
use crate::rank_proofs::*;
use crate::maintenance_semantics::*;

verus! {

pub struct Changes {
    parts: Parts,
    edges: Edges,
    blockers: Index<i64>,
}

impl Changes {
    pub closed spec fn part_counts(self) -> Parts { self.parts }
    pub closed spec fn edge_counts(self) -> Edges { self.edges }
    pub closed spec fn blocker_counts(self) -> Index<i64> { self.blockers }

    #[verifier::type_invariant]
    pub closed spec fn valid(self) -> bool {
        parts_keys_valid(self.parts) && edges_keys_valid(self.edges) && integer_keys(self.blockers)
    }
}

pub fn changes(part: &Vec<PartDelta>, edge: &[EdgeDelta], supplier: &[SupplierDelta]) -> (out: Option<Changes>)
    ensures out matches Some(delta) ==> (
        (forall|part_key: int, group: verus_prooflib::ordered::KeyView|
            #[trigger] part_at(delta.part_counts(), part_key, group) == sum(part_delta_seq_view(part@),
                |row: (PartRow, int)| row.1 * part_weight(row.0, part_key, group)))
        && (forall|supplier_key: int, part_key: int|
            #[trigger] edge_at(delta.edge_counts(), supplier_key, part_key) == sum(edge_delta_seq_view(edge@),
                |row: (PartsuppRow, int)| row.1 * edge_weight(row.0, supplier_key, part_key)))
        && (forall|supplier_key: int| #[trigger] integer_at(delta.blocker_counts(), (supplier_key, Seq::empty()))
            == sum(supplier@, |row: SupplierDelta| if row.s_suppkey == supplier_key && row.is_blocker != 0 { row.sign as int } else { 0 }))
    ),
{
    let vocab = crate::vocab::new_vocab();
    let mut parts = Index::new();
    let mut index = 0;
    let mut processed: Ghost<int> = Ghost(0);
    while index < part.len()
        invariant index <= part.len(), processed@ == index as int, parts_keys_valid(parts), vocab.ok(),
            forall|part_key: int, group: verus_prooflib::ordered::KeyView|
                #[trigger] part_at(parts, part_key, group) == part_delta_sum(part@.take(processed@), part_key, group),
        decreases part.len() - index,
    {
        if !bump_part(&vocab, &mut parts, &part[index].row, part[index].sign) { return None; }
        proof { lemma_part_delta_prefix(part@, processed@); }
        index += 1;
        processed = Ghost(index as int);
    }
    let mut edges = Index::new();
    proof { assert(part@.take(processed@) =~= part@); }
    index = 0;
    processed = Ghost(0);
    while index < edge.len()
        invariant index <= edge.len(), processed@ == index as int, edges_keys_valid(edges),
            parts_keys_valid(parts),
            forall|part_key: int, group: verus_prooflib::ordered::KeyView|
                #[trigger] part_at(parts, part_key, group) == part_delta_sum(part@, part_key, group),
            forall|supplier_key: int, part_key: int| #[trigger] edge_at(edges, supplier_key, part_key)
                == edge_delta_sum(edge@.take(processed@), supplier_key, part_key),
        decreases edge.len() - index,
    {
        let row = &edge[index];
        if !bump_edge(&mut edges, row.row.ps_partkey, row.row.ps_suppkey, row.sign) { return None; }
        proof { lemma_edge_delta_prefix(edge@, processed@); }
        index += 1;
        processed = Ghost(index as int);
    }
    let mut blockers = Index::new();
    proof { assert(edge@.take(processed@) =~= edge@); }
    index = 0;
    processed = Ghost(0);
    while index < supplier.len()
        invariant index <= supplier.len(), processed@ == index as int, integer_keys(blockers),
            parts_keys_valid(parts), edges_keys_valid(edges),
            forall|part_key: int, group: verus_prooflib::ordered::KeyView|
                #[trigger] part_at(parts, part_key, group) == part_delta_sum(part@, part_key, group),
            forall|supplier_key: int, part_key: int|
                #[trigger] edge_at(edges, supplier_key, part_key) == edge_delta_sum(edge@, supplier_key, part_key),
            forall|supplier_key: int| #[trigger] integer_at(blockers, (supplier_key, Seq::empty()))
                == sum(supplier@.take(processed@), |row: SupplierDelta|
                    if row.s_suppkey == supplier_key && row.is_blocker != 0 { row.sign as int } else { 0 }),
        decreases supplier.len() - index,
    {
        let row = &supplier[index];
        if row.is_blocker != 0 && !bump(&mut blockers, Key::number(row.s_suppkey), row.sign) { return None; }
        proof {
            assert forall|supplier_key: int| #[trigger] integer_at(blockers, (supplier_key, Seq::empty()))
                == sum(supplier@.take(processed@ + 1), |row: SupplierDelta|
                    if row.s_suppkey == supplier_key && row.is_blocker != 0 { row.sign as int } else { 0int }) by {
                lemma_sum_take(supplier@, processed@, |row: SupplierDelta|
                    if row.s_suppkey == supplier_key && row.is_blocker != 0 { row.sign as int } else { 0int });
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    proof { assert(supplier@.take(processed@) =~= supplier@); }
    Some(Changes { parts, edges, blockers })
}

pub fn part_support(changes: &Changes) -> (out: Index<i64>)
    ensures key_set(out),
        forall|part: int| #[trigger] integer_at(out, (part, Seq::<u8>::empty()))
            == if changes.part_counts().at((part, Seq::<u8>::empty())).is_some() { 1int } else { 0int },
{
    proof { use_type_invariant(changes); }
    let mut keys = Index::new();
    let mut cursor = changes.parts.cursor();
    let mut processed: Ghost<int> = Ghost(0);
    loop
        invariant key_set(keys), parts_keys_valid(changes.parts),
            0 <= processed@ <= changes.parts.entries().len(),
            cursor.remaining() == changes.parts.entries().skip(processed@),
            forall|part: int| #[trigger] integer_at(keys, (part, Seq::<u8>::empty())) ==
                if changes.parts.entries().take(processed@).map_values(
                    |row: (verus_prooflib::ordered::KeyView, Counts)| row.0.0).contains(part) { 1int } else { 0int },
        decreases cursor.remaining().len(),
    {
        let (key, _) = match cursor.next() {
            Some(row) => row,
            None => {
                proof {
                    assert(processed@ == changes.parts.entries().len());
                    assert(changes.parts.entries().take(processed@) =~= changes.parts.entries());
                    changes.parts.model();
                    assert forall|part: int| #[trigger] integer_at(keys, (part, Seq::<u8>::empty()))
                        == if changes.parts.at((part, Seq::<u8>::empty())).is_some() { 1int } else { 0int } by {
                        let projected = changes.parts.entries().map_values(
                            |row: (verus_prooflib::ordered::KeyView, Counts)| row.0.0);
                        if projected.contains(part) {
                            let position = choose|position: int| 0 <= position < projected.len() && #[trigger] projected[position] == part;
                            assert(changes.parts.at(changes.parts.entries()[position].0).is_some());
                        }
                        if changes.parts.at((part, Seq::<u8>::empty())).is_some() {
                            let position = choose|position: int| 0 <= position < changes.parts.entries().len()
                                && (#[trigger] changes.parts.entries()[position]).0 == (part, Seq::<u8>::empty());
                            assert(projected[position] == part);
                        }
                    }
                }
                return keys;
            },
        };
        proof { assert(key.view() == changes.parts.entries()[processed@].0); }
        insert_key(&mut keys, key.number);
        proof {
            let before = changes.parts.entries().take(processed@).map_values(
                |row: (verus_prooflib::ordered::KeyView, Counts)| row.0.0);
            assert(changes.parts.entries().take(processed@ + 1).map_values(
                |row: (verus_prooflib::ordered::KeyView, Counts)| row.0.0) =~= before.push(key.number as int));
            verus_prooflib::lemma_push_membership(before, key.number as int);
        }
        processed = Ghost(processed@ + 1);
    }
}

pub fn edge_supplier_support(changes: &Changes) -> (out: Index<i64>)
    ensures key_set(out),
        forall|supplier: int| #[trigger] integer_at(out, (supplier, Seq::<u8>::empty()))
            == if changes.edge_counts().at((supplier, Seq::<u8>::empty())).is_some() { 1int } else { 0int },
{
    proof { use_type_invariant(changes); }
    let mut keys = Index::new();
    let mut cursor = changes.edges.cursor();
    let mut processed: Ghost<int> = Ghost(0);
    loop
        invariant key_set(keys), edges_keys_valid(changes.edges),
            0 <= processed@ <= changes.edges.entries().len(),
            cursor.remaining() == changes.edges.entries().skip(processed@),
            forall|supplier: int| #[trigger] integer_at(keys, (supplier, Seq::<u8>::empty())) ==
                if changes.edges.entries().take(processed@).map_values(
                    |row: (verus_prooflib::ordered::KeyView, Index<i64>)| row.0.0).contains(supplier) { 1int } else { 0int },
        decreases cursor.remaining().len(),
    {
        let (key, _) = match cursor.next() {
            Some(row) => row,
            None => {
                proof {
                    assert(processed@ == changes.edges.entries().len());
                    assert(changes.edges.entries().take(processed@) =~= changes.edges.entries());
                    changes.edges.model();
                    assert forall|supplier: int| #[trigger] integer_at(keys, (supplier, Seq::<u8>::empty()))
                        == if changes.edges.at((supplier, Seq::<u8>::empty())).is_some() { 1int } else { 0int } by {
                        let projected = changes.edges.entries().map_values(
                            |row: (verus_prooflib::ordered::KeyView, Index<i64>)| row.0.0);
                        if projected.contains(supplier) {
                            let position = choose|position: int| 0 <= position < projected.len() && #[trigger] projected[position] == supplier;
                            assert(changes.edges.at(changes.edges.entries()[position].0).is_some());
                        }
                        if changes.edges.at((supplier, Seq::<u8>::empty())).is_some() {
                            let position = choose|position: int| 0 <= position < changes.edges.entries().len()
                                && (#[trigger] changes.edges.entries()[position]).0 == (supplier, Seq::<u8>::empty());
                            assert(projected[position] == supplier);
                        }
                    }
                }
                return keys;
            },
        };
        proof { assert(key.view() == changes.edges.entries()[processed@].0); }
        insert_key(&mut keys, key.number);
        proof {
            let before = changes.edges.entries().take(processed@).map_values(
                |row: (verus_prooflib::ordered::KeyView, Index<i64>)| row.0.0);
            assert(changes.edges.entries().take(processed@ + 1).map_values(
                |row: (verus_prooflib::ordered::KeyView, Index<i64>)| row.0.0) =~= before.push(key.number as int));
            verus_prooflib::lemma_push_membership(before, key.number as int);
        }
        processed = Ghost(processed@ + 1);
    }
}

pub fn supplier_support(changes: &Changes) -> (out: Index<i64>)
    ensures key_set(out),
        forall|supplier: int| #[trigger] integer_at(out, (supplier, Seq::<u8>::empty()))
            == if changes.edge_counts().at((supplier, Seq::<u8>::empty())).is_some()
                || changes.blocker_counts().at((supplier, Seq::<u8>::empty())).is_some() { 1int } else { 0int },
{
    proof { use_type_invariant(changes); }
    let mut keys = edge_supplier_support(changes);
    let mut cursor = changes.blockers.cursor();
    let mut processed: Ghost<int> = Ghost(0);
    loop
        invariant key_set(keys), integer_keys(changes.blockers),
            0 <= processed@ <= changes.blockers.entries().len(),
            cursor.remaining() == changes.blockers.entries().skip(processed@),
            forall|supplier: int| #[trigger] integer_at(keys, (supplier, Seq::<u8>::empty())) ==
                if changes.edges.at((supplier, Seq::<u8>::empty())).is_some()
                    || changes.blockers.entries().take(processed@).map_values(
                        |row: (verus_prooflib::ordered::KeyView, i64)| row.0.0).contains(supplier) { 1int } else { 0int },
        decreases cursor.remaining().len(),
    {
        let (key, _) = match cursor.next() {
            Some(row) => row,
            None => {
                proof {
                    assert(processed@ == changes.blockers.entries().len());
                    assert(changes.blockers.entries().take(processed@) =~= changes.blockers.entries());
                    changes.blockers.model();
                    assert forall|supplier: int| #[trigger] integer_at(keys, (supplier, Seq::<u8>::empty()))
                        == if changes.edges.at((supplier, Seq::<u8>::empty())).is_some()
                            || changes.blockers.at((supplier, Seq::<u8>::empty())).is_some() { 1int } else { 0int } by {
                        let projected = changes.blockers.entries().map_values(
                            |row: (verus_prooflib::ordered::KeyView, i64)| row.0.0);
                        if projected.contains(supplier) {
                            let position = choose|position: int| 0 <= position < projected.len() && #[trigger] projected[position] == supplier;
                            assert(changes.blockers.at(changes.blockers.entries()[position].0).is_some());
                        }
                        if changes.blockers.at((supplier, Seq::<u8>::empty())).is_some() {
                            let position = choose|position: int| 0 <= position < changes.blockers.entries().len()
                                && (#[trigger] changes.blockers.entries()[position]).0 == (supplier, Seq::<u8>::empty());
                            assert(projected[position] == supplier);
                        }
                    }
                }
                return keys;
            },
        };
        proof { assert(key.view() == changes.blockers.entries()[processed@].0); }
        insert_key(&mut keys, key.number);
        proof {
            let before = changes.blockers.entries().take(processed@).map_values(
                |row: (verus_prooflib::ordered::KeyView, i64)| row.0.0);
            assert(changes.blockers.entries().take(processed@ + 1).map_values(
                |row: (verus_prooflib::ordered::KeyView, i64)| row.0.0) =~= before.push(key.number as int));
            verus_prooflib::lemma_push_membership(before, key.number as int);
        }
        processed = Ghost(processed@ + 1);
    }
}

pub fn neighbor_support(changes: &Changes) -> (out: Index<i64>)
    ensures key_set(out),
{
    let mut keys = part_support(changes);
    let mut cursor = changes.edges.cursor();
    loop
        invariant key_set(keys),
        decreases cursor.remaining().len(),
    {
        let (_, parts) = match cursor.next() { Some(row) => row, None => break };
        let mut part_cursor = parts.cursor();
        loop
            invariant key_set(keys),
            decreases part_cursor.remaining().len(),
        {
            let (part, _) = match part_cursor.next() { Some(row) => row, None => break };
            insert_key(&mut keys, part.number);
        }
    }
    keys
}

pub fn restore_parts(new: &Parts, changes: &Parts, neighbors: &Index<i64>) -> (out: Option<Parts>)
    requires parts_keys_valid(*new), parts_keys_valid(*changes), neighbors.wf(),
    ensures out matches Some(old) ==> parts_keys_valid(old)
        && forall|part: int, key: verus_prooflib::ordered::KeyView| #[trigger] part_at(old, part, key)
            == part_at(*new, part, key) -
                if neighbors.at((part, Seq::<u8>::empty())).is_some() { part_at(*changes, part, key) } else { 0int },
{
    let mut old = share_parts(new);
    proof { changes.map_model(); }
    let mut cursor = changes.cursor();
    let mut processed: Ghost<int> = Ghost(0);
    loop
        invariant parts_keys_valid(old), parts_keys_valid(*changes), parts_keys_valid(*new), neighbors.wf(),
            0 <= processed@ <= changes.entries().len(),
            cursor.remaining() == changes.entries().skip(processed@),
            forall|part: int, key: verus_prooflib::ordered::KeyView| #[trigger] part_at(old, part, key)
                == part_at(*new, part, key) - sum(changes.entries().take(processed@),
                    |row: (verus_prooflib::ordered::KeyView, Counts)|
                        if row.0 == (part, Seq::<u8>::empty()) && neighbors.at(row.0).is_some() { count_at(row.1, key) } else { 0int }),
            forall|row: (verus_prooflib::ordered::KeyView, Counts)| #[trigger] cursor.remaining().contains(row) ==>
                row.0.1 == Seq::<u8>::empty() && cell_keys_valid(row.1),
        decreases cursor.remaining().len(),
    {
        let remaining = Ghost(cursor.remaining());
        let (part, delta) = match cursor.next() {
            Some(row) => row,
            None => {
                proof {
                    assert(processed@ == changes.entries().len());
                    assert(changes.entries().take(processed@) =~= changes.entries());
                    assert forall|part: int, key: verus_prooflib::ordered::KeyView| #[trigger] part_at(old, part, key)
                        == part_at(*new, part, key) -
                            if neighbors.at((part, Seq::<u8>::empty())).is_some() { part_at(*changes, part, key) } else { 0int } by {
                        verus_prooflib::lemma_ordered_selected_weight(changes, (part, Seq::<u8>::empty()),
                            |row: (verus_prooflib::ordered::KeyView, Counts)| count_at(row.1, key),
                            if neighbors.at((part, Seq::<u8>::empty())).is_some() { 1int } else { 0int },
                            |row: (verus_prooflib::ordered::KeyView, Counts)|
                                if row.0 == (part, Seq::<u8>::empty()) && neighbors.at(row.0).is_some() { count_at(row.1, key) } else { 0int });
                    }
                }
                return Some(old);
            },
        };
        proof {
            assert(remaining@.contains((part.view(), *delta)));
            assert((part.view(), *delta) == changes.entries()[processed@]);
            lemma_parts_tail(remaining@);
        }
        if neighbors.get(&part).is_none() {
            proof {
                assert forall|query_part: int, key: verus_prooflib::ordered::KeyView| #[trigger] part_at(old, query_part, key)
                    == part_at(*new, query_part, key) - sum(changes.entries().take(processed@ + 1),
                        |row: (verus_prooflib::ordered::KeyView, Counts)|
                            if row.0 == (query_part, Seq::<u8>::empty()) && neighbors.at(row.0).is_some() { count_at(row.1, key) } else { 0int }) by {
                    lemma_sum_take(changes.entries(), processed@,
                        |row: (verus_prooflib::ordered::KeyView, Counts)|
                            if row.0 == (query_part, Seq::<u8>::empty()) && neighbors.at(row.0).is_some() { count_at(row.1, key) } else { 0int });
                }
            }
            processed = Ghost(processed@ + 1);
            continue;
        }
        let initial = Ghost(old);
        let mut groups = match old.get(&part) { Some(value) => share_counts(&*value), None => Index::new() };
        proof { delta.map_model(); }
        let mut group_cursor = delta.cursor();
        let mut group_processed: Ghost<int> = Ghost(0);
        let mut completed = false;
        while !completed
            invariant cell_keys_valid(groups), parts_keys_valid(old),
                old == initial@, cell_keys_valid(*delta),
                part.view().1 == Seq::<u8>::empty(), neighbors.at(part.view()).is_some(),
                (part.view(), *delta) == changes.entries()[processed@],
                forall|query_part: int, key: verus_prooflib::ordered::KeyView| #[trigger] part_at(initial@, query_part, key)
                    == part_at(*new, query_part, key) - sum(changes.entries().take(processed@),
                        |row: (verus_prooflib::ordered::KeyView, Counts)|
                            if row.0 == (query_part, Seq::<u8>::empty()) && neighbors.at(row.0).is_some() { count_at(row.1, key) } else { 0int }),
                0 <= group_processed@ <= delta.entries().len(),
                group_cursor.remaining() == delta.entries().skip(group_processed@),
                forall|key: verus_prooflib::ordered::KeyView| #[trigger] count_at(groups, key)
                    == part_at(initial@, part.number as int, key) - sum(delta.entries().take(group_processed@),
                        |row: (verus_prooflib::ordered::KeyView, Cell)| if row.0 == key { row.1.count as int } else { 0int }),
                completed ==> forall|key: verus_prooflib::ordered::KeyView| #[trigger] count_at(groups, key)
                    == part_at(initial@, part.number as int, key) - count_at(*delta, key),
                cells_valid_rows(group_cursor.remaining()),
            decreases group_cursor.remaining().len() + if completed { 0int } else { 1int },
        {
            let remaining = Ghost(group_cursor.remaining());
            let (key, cell) = match group_cursor.next() {
                Some(row) => row,
                None => {
                    proof {
                        assert(group_processed@ == delta.entries().len());
                        assert(delta.entries().take(group_processed@) =~= delta.entries());
                        assert forall|key: verus_prooflib::ordered::KeyView| #[trigger] count_at(groups, key)
                            == part_at(initial@, part.number as int, key) - count_at(*delta, key) by {
                            verus_prooflib::lemma_ordered_selected_weight(&*delta, key,
                                |row: (verus_prooflib::ordered::KeyView, Cell)| row.1.count as int, 1int,
                                |row: (verus_prooflib::ordered::KeyView, Cell)| if row.0 == key { row.1.count as int } else { 0int });
                        }
                    }
                    completed = true;
                    continue;
                },
            };
            proof {
                assert(remaining@.contains((key.view(), *cell)));
                assert((key.view(), *cell) == delta.entries()[group_processed@]);
                lemma_cells_tail(remaining@);
            }
            if cell.count == i64::MIN { return None; }
            if !bump_group(&mut groups, cell.group.clone(), -cell.count) { return None; }
            proof {
                assert forall|query: verus_prooflib::ordered::KeyView| #[trigger] count_at(groups, query)
                    == part_at(initial@, part.number as int, query) - sum(delta.entries().take(group_processed@ + 1),
                        |row: (verus_prooflib::ordered::KeyView, Cell)| if row.0 == query { row.1.count as int } else { 0int }) by {
                    lemma_sum_take(delta.entries(), group_processed@,
                        |row: (verus_prooflib::ordered::KeyView, Cell)| if row.0 == query { row.1.count as int } else { 0int });
                }
            }
            group_processed = Ghost(group_processed@ + 1);
        }
        let restored = Ghost(groups);
        if groups.is_empty() {
            proof {
                groups.model();
                assert forall|key: verus_prooflib::ordered::KeyView| #[trigger] count_at(groups, key) == 0 by {};
            }
            old.remove(&part);
        } else { old.put(part, Arc::new(groups)); }
        proof {
            assert forall|key: verus_prooflib::ordered::KeyView| #[trigger] part_at(old, part.number as int, key)
                == count_at(restored@, key) by {};
            assert forall|query_part: int, key: verus_prooflib::ordered::KeyView| #[trigger] part_at(old, query_part, key)
                == if query_part == part.number as int { count_at(restored@, key) } else { part_at(initial@, query_part, key) } by {};
            assert forall|query_part: int, key: verus_prooflib::ordered::KeyView| #[trigger] part_at(old, query_part, key)
                == part_at(*new, query_part, key) - sum(changes.entries().take(processed@ + 1),
                    |row: (verus_prooflib::ordered::KeyView, Counts)|
                        if row.0 == (query_part, Seq::<u8>::empty()) && neighbors.at(row.0).is_some() { count_at(row.1, key) } else { 0int }) by {
                lemma_sum_take(changes.entries(), processed@,
                    |row: (verus_prooflib::ordered::KeyView, Counts)|
                        if row.0 == (query_part, Seq::<u8>::empty()) && neighbors.at(row.0).is_some() { count_at(row.1, key) } else { 0int });
                if query_part == part.number as int && old.at((query_part, Seq::<u8>::empty())).is_none() {
                    restored@.model();
                    assert(count_at(restored@, key) == 0);
                }
            }
        }
        processed = Ghost(processed@ + 1);
    }
}

pub fn restore_edges(new: &Edges, changes: &Edges) -> (out: Option<Edges>)
    requires edges_keys_valid(*new), edges_keys_valid(*changes),
    ensures out matches Some(old) ==> edges_keys_valid(old)
        && forall|supplier: int, part: int| #[trigger] edge_at(old, supplier, part)
            == edge_at(*new, supplier, part) - edge_at(*changes, supplier, part),
{
    let mut old = share_edges(new);
    proof { changes.map_model(); }
    let mut cursor = changes.cursor();
    let mut processed: Ghost<int> = Ghost(0);
    loop
        invariant edges_keys_valid(old), edges_keys_valid(*new), edges_keys_valid(*changes),
            0 <= processed@ <= changes.entries().len(),
            cursor.remaining() == changes.entries().skip(processed@),
            forall|supplier: int, part: int| #[trigger] edge_at(old, supplier, part)
                == edge_at(*new, supplier, part) - sum(changes.entries().take(processed@),
                    |row: (verus_prooflib::ordered::KeyView, Index<i64>)|
                        if row.0 == (supplier, Seq::<u8>::empty()) { integer_at(row.1, (part, Seq::empty())) } else { 0int }),
        decreases cursor.remaining().len(),
    {
        let (supplier, parts) = match cursor.next() {
            Some(row) => row,
            None => {
                proof {
                    assert(processed@ == changes.entries().len());
                    assert(changes.entries().take(processed@) =~= changes.entries());
                    assert forall|supplier: int, part: int| #[trigger] edge_at(old, supplier, part)
                        == edge_at(*new, supplier, part) - edge_at(*changes, supplier, part) by {
                        verus_prooflib::lemma_ordered_selected_weight(changes, (supplier, Seq::<u8>::empty()),
                            |row: (verus_prooflib::ordered::KeyView, Index<i64>)| integer_at(row.1, (part, Seq::<u8>::empty())), 1int,
                            |row: (verus_prooflib::ordered::KeyView, Index<i64>)|
                                if row.0 == (supplier, Seq::<u8>::empty()) { integer_at(row.1, (part, Seq::<u8>::empty())) } else { 0int });
                    }
                }
                return Some(old);
            },
        };
        proof {
            assert((supplier.view(), *parts) == changes.entries()[processed@]);
            changes.model();
            assert(changes.at(supplier.view()) == Some(*parts));
            assert(integer_keys(*parts));
            parts.map_model();
        }
        let initial = Ghost(old);
        let mut part_cursor = parts.cursor();
        let mut part_processed: Ghost<int> = Ghost(0);
        let mut completed = false;
        while !completed
            invariant edges_keys_valid(old), integer_keys(*parts),
                supplier.view().1 == Seq::<u8>::empty(),
                (supplier.view(), *parts) == changes.entries()[processed@],
                forall|query_supplier: int, query_part: int| #[trigger] edge_at(initial@, query_supplier, query_part)
                    == edge_at(*new, query_supplier, query_part) - sum(changes.entries().take(processed@),
                        |row: (verus_prooflib::ordered::KeyView, Index<i64>)|
                            if row.0 == (query_supplier, Seq::<u8>::empty()) { integer_at(row.1, (query_part, Seq::<u8>::empty())) } else { 0int }),
                completed ==> forall|query_supplier: int, query_part: int| #[trigger] edge_at(old, query_supplier, query_part)
                    == edge_at(initial@, query_supplier, query_part) -
                        if query_supplier == supplier.number as int { integer_at(*parts, (query_part, Seq::<u8>::empty())) } else { 0int },
                0 <= part_processed@ <= parts.entries().len(),
                part_cursor.remaining() == parts.entries().skip(part_processed@),
                forall|query_supplier: int, query_part: int| #[trigger] edge_at(old, query_supplier, query_part)
                    == edge_at(initial@, query_supplier, query_part) -
                        if query_supplier == supplier.number as int {
                            sum(parts.entries().take(part_processed@),
                                |row: (verus_prooflib::ordered::KeyView, i64)|
                                    if row.0 == (query_part, Seq::<u8>::empty()) { row.1 as int } else { 0int })
                        } else { 0int },
            decreases part_cursor.remaining().len() + if completed { 0int } else { 1int },
        {
            let (part, count) = match part_cursor.next() {
                Some(row) => row,
                None => {
                    proof {
                        assert(part_processed@ == parts.entries().len());
                        assert(parts.entries().take(part_processed@) =~= parts.entries());
                        assert forall|query_supplier: int, query_part: int| #[trigger] edge_at(old, query_supplier, query_part)
                            == edge_at(initial@, query_supplier, query_part) -
                                if query_supplier == supplier.number as int { integer_at(*parts, (query_part, Seq::<u8>::empty())) } else { 0int } by {
                            verus_prooflib::lemma_ordered_selected_weight(&*parts, (query_part, Seq::<u8>::empty()),
                                |row: (verus_prooflib::ordered::KeyView, i64)| row.1 as int, 1int,
                                |row: (verus_prooflib::ordered::KeyView, i64)|
                                    if row.0 == (query_part, Seq::<u8>::empty()) { row.1 as int } else { 0int });
                        }
                    }
                    completed = true;
                    continue;
                },
            };
            proof {
                assert((part.view(), *count) == parts.entries()[part_processed@]);
                parts.model();
                assert(parts.at(part.view()) == Some(*count));
                assert(part.view().1 == Seq::<u8>::empty());
            }
            if *count == i64::MIN { return None; }
            if !bump_edge(&mut old, part.number, supplier.number, -*count) { return None; }
            proof {
                assert forall|query_supplier: int, query_part: int| #[trigger] edge_at(old, query_supplier, query_part)
                    == edge_at(initial@, query_supplier, query_part) -
                        if query_supplier == supplier.number as int {
                            sum(parts.entries().take(part_processed@ + 1),
                                |row: (verus_prooflib::ordered::KeyView, i64)|
                                    if row.0 == (query_part, Seq::<u8>::empty()) { row.1 as int } else { 0int })
                        } else { 0int } by {
                    lemma_sum_take(parts.entries(), part_processed@,
                        |row: (verus_prooflib::ordered::KeyView, i64)|
                            if row.0 == (query_part, Seq::<u8>::empty()) { row.1 as int } else { 0int });
                }
            }
            part_processed = Ghost(part_processed@ + 1);
        }
        proof {
            assert forall|query_supplier: int, query_part: int| #[trigger] edge_at(old, query_supplier, query_part)
                == edge_at(*new, query_supplier, query_part) - sum(changes.entries().take(processed@ + 1),
                    |row: (verus_prooflib::ordered::KeyView, Index<i64>)|
                        if row.0 == (query_supplier, Seq::<u8>::empty()) { integer_at(row.1, (query_part, Seq::<u8>::empty())) } else { 0int }) by {
                lemma_sum_take(changes.entries(), processed@,
                    |row: (verus_prooflib::ordered::KeyView, Index<i64>)|
                        if row.0 == (query_supplier, Seq::<u8>::empty()) { integer_at(row.1, (query_part, Seq::<u8>::empty())) } else { 0int });
            }
        }
        processed = Ghost(processed@ + 1);
    }
}

pub fn restore_blockers(new: &Index<i64>, changes: &Index<i64>) -> (out: Option<Index<i64>>)
    requires integer_keys(*new), integer_keys(*changes),
    ensures out matches Some(old) ==> integer_keys(old)
        && forall|key: verus_prooflib::ordered::KeyView| #[trigger] integer_at(old, key)
            == integer_at(*new, key) - integer_at(*changes, key),
{
    let mut old = share_integers(new);
    proof { changes.map_model(); }
    let mut cursor = changes.cursor();
    let mut processed: Ghost<int> = Ghost(0);
    loop
        invariant integer_keys(old), integer_keys(*changes), integer_keys(*new),
            0 <= processed@ <= changes.entries().len(),
            cursor.remaining() == changes.entries().skip(processed@),
            forall|key: verus_prooflib::ordered::KeyView| #[trigger] integer_at(old, key)
                == integer_at(*new, key) - sum(changes.entries().take(processed@),
                    |row: (verus_prooflib::ordered::KeyView, i64)| if row.0 == key { row.1 as int } else { 0int }),
            forall|row: (verus_prooflib::ordered::KeyView, i64)| #[trigger] cursor.remaining().contains(row) ==> row.0.1 == Seq::<u8>::empty(),
        decreases cursor.remaining().len(),
    {
        let remaining = Ghost(cursor.remaining());
        let (supplier, count) = match cursor.next() {
            Some(row) => row,
            None => {
                proof {
                    assert(processed@ == changes.entries().len());
                    assert(changes.entries().take(processed@) =~= changes.entries());
                    assert forall|key: verus_prooflib::ordered::KeyView| #[trigger] integer_at(old, key)
                        == integer_at(*new, key) - integer_at(*changes, key) by {
                        verus_prooflib::lemma_ordered_selected_weight(changes, key,
                            |row: (verus_prooflib::ordered::KeyView, i64)| row.1 as int, 1int,
                            |row: (verus_prooflib::ordered::KeyView, i64)| if row.0 == key { row.1 as int } else { 0int });
                    }
                }
                return Some(old);
            },
        };
        proof {
            assert(remaining@.contains((supplier.view(), *count)));
            assert((supplier.view(), *count) == changes.entries()[processed@]);
            lemma_integer_tail(remaining@);
            assert(supplier.view().1 == Seq::<u8>::empty());
        }
        if *count == i64::MIN { return None; }
        if !bump(&mut old, supplier, -*count) { return None; }
        proof {
            assert forall|key: verus_prooflib::ordered::KeyView| #[trigger] integer_at(old, key)
                == integer_at(*new, key) - sum(changes.entries().take(processed@ + 1),
                    |row: (verus_prooflib::ordered::KeyView, i64)| if row.0 == key { row.1 as int } else { 0int }) by {
                lemma_sum_take(changes.entries(), processed@,
                    |row: (verus_prooflib::ordered::KeyView, i64)| if row.0 == key { row.1 as int } else { 0int });
            }
        }
        processed = Ghost(processed@ + 1);
    }
}

pub fn collect_changes(old_parts: &Parts, old_edges: &Edges, old_blockers: &Index<i64>,
    new_parts: &Parts, new_edges: &Edges, new_blockers: &Index<i64>, suppliers: &Index<i64>,
) -> (out: Result<Counts, u32>)
    requires parts_keys_valid(*old_parts), parts_keys_valid(*new_parts),
        edges_keys_valid(*old_edges), edges_keys_valid(*new_edges),
        old_blockers.wf(), new_blockers.wf(), key_set(*suppliers),
    ensures out matches Ok(groups) ==> cell_keys_valid(groups)
        && forall|key: verus_prooflib::ordered::KeyView| #[trigger] count_at(groups, key)
            == sum(suppliers.entries(), |row: (verus_prooflib::ordered::KeyView, i64)|
                supplier_contribution(*new_parts, *new_edges, *new_blockers, row.0.0, key)
                    - supplier_contribution(*old_parts, *old_edges, *old_blockers, row.0.0, key)),
{
    let mut group_changes: Counts = Index::new();
    let mut cursor = suppliers.cursor();
    let mut processed: Ghost<int> = Ghost(0);
    loop
        invariant cell_keys_valid(group_changes), parts_keys_valid(*old_parts), parts_keys_valid(*new_parts),
            edges_keys_valid(*old_edges), edges_keys_valid(*new_edges), key_set(*suppliers),
            0 <= processed@ <= suppliers.entries().len(),
            cursor.remaining() == suppliers.entries().skip(processed@),
            forall|key: verus_prooflib::ordered::KeyView| #[trigger] count_at(group_changes, key)
                == sum(suppliers.entries().take(processed@), |row: (verus_prooflib::ordered::KeyView, i64)|
                    supplier_contribution(*new_parts, *new_edges, *new_blockers, row.0.0, key)
                        - supplier_contribution(*old_parts, *old_edges, *old_blockers, row.0.0, key)),
        decreases cursor.remaining().len(),
    {
        let (supplier, _) = match cursor.next() {
            Some(row) => row,
            None => {
                proof {
                    assert(processed@ == suppliers.entries().len());
                    assert(suppliers.entries().take(processed@) =~= suppliers.entries());
                }
                return Ok(group_changes);
            },
        };
        proof {
            suppliers.model();
            assert(supplier.view() == suppliers.entries()[processed@].0);
            assert(suppliers.at(supplier.view()).is_some());
            assert(supplier.view().1 == Seq::<u8>::empty());
        }
        let initial = Ghost(group_changes);
        let before = match old_blockers.get(&supplier) { Some(value) => *value, None => 0 };
        let after = match new_blockers.get(&supplier) { Some(value) => *value, None => 0 };
        if before < 0 || after < 0 { return Err(6); }
        if before == 0 {
            if let Some(edges) = old_edges.get(&supplier) {
                let groups = join(old_parts, &*edges);
                if !collect_contribution(&mut group_changes, &groups, -1) { return Err(4); }
            }
        }
        proof {
            assert forall|key: verus_prooflib::ordered::KeyView| #[trigger] count_at(group_changes, key)
                == count_at(initial@, key) - supplier_contribution(*old_parts, *old_edges, *old_blockers, supplier.number as int, key) by {};
        }
        if after == 0 {
            if let Some(edges) = new_edges.get(&supplier) {
                let groups = join(new_parts, &*edges);
                if !collect_contribution(&mut group_changes, &groups, 1) { return Err(4); }
            }
        }
        proof {
            assert forall|key: verus_prooflib::ordered::KeyView| #[trigger] count_at(group_changes, key)
                == sum(suppliers.entries().take(processed@ + 1), |row: (verus_prooflib::ordered::KeyView, i64)|
                    supplier_contribution(*new_parts, *new_edges, *new_blockers, row.0.0, key)
                        - supplier_contribution(*old_parts, *old_edges, *old_blockers, row.0.0, key)) by {
                lemma_sum_take(suppliers.entries(), processed@, |row: (verus_prooflib::ordered::KeyView, i64)|
                    supplier_contribution(*new_parts, *new_edges, *new_blockers, row.0.0, key)
                        - supplier_contribution(*old_parts, *old_edges, *old_blockers, row.0.0, key));
            }
        }
        processed = Ghost(processed@ + 1);
    }
}

#[verifier::rlimit(40)]
pub fn publish_changes(source: &Counts, base_ranks: &Ranks, group_changes: &Counts) -> (out: Result<(Counts, Ranks), u32>)
    requires cell_keys_valid(*source), cell_keys_valid(*group_changes),
    ensures out matches Ok(state) ==> cell_keys_valid(state.0),
        out matches Ok(state) ==> (positive_counts(*source) ==> positive_counts(state.0)),
        out matches Ok(state) ==> (ranks_valid(*base_ranks, *source) ==> ranks_valid(state.1, state.0)),
        out matches Ok(state) ==> forall|key: verus_prooflib::ordered::KeyView| #[trigger] count_at(state.0, key)
            == count_at(*source, key) + count_at(*group_changes, key),
{
    let source_counts = Ghost(*source);
    let source_ranks = Ghost(*base_ranks);
    let preserve = Ghost(ranks_valid(*base_ranks, *source));
    let mut counts = share_counts(source);
    let mut ranks = base_ranks.share();
    proof {
        group_changes.model();
        if preserve@ {
            verus_prooflib::lemma_ordered_equal(&source_ranks@, &ranks);
            lemma_ranks_positive(source_ranks@, source_counts@);
            assert(positive_counts(counts));
            lemma_ranks_equal(&source_ranks@, &ranks, source_counts@, counts);
        }
    }
    let mut cursor = group_changes.cursor();
    let mut processed: Ghost<int> = Ghost(0);
    loop
        invariant cell_keys_valid(counts), cell_keys_valid(*group_changes),
            source_counts@ == *source, source_ranks@ == *base_ranks,
            preserve@ == ranks_valid(source_ranks@, source_counts@),
            cells_valid_rows(cursor.remaining()),
            positive_counts(source_counts@) ==> positive_counts(counts),
            preserve@ ==> ranks_valid(ranks, counts),
            0 <= processed@ <= group_changes.entries().len(),
            cursor.remaining() == group_changes.entries().skip(processed@),
            forall|key: verus_prooflib::ordered::KeyView| #[trigger] count_at(counts, key)
                == count_at(source_counts@, key) + sum(group_changes.entries().take(processed@),
                    |row: (verus_prooflib::ordered::KeyView, Cell)| if row.0 == key { row.1.count as int } else { 0int }),
        decreases cursor.remaining().len(),
    {
        let remaining = Ghost(cursor.remaining());
        let (key, cell) = match cursor.next() {
            Some(row) => row,
            None => {
                proof {
                    assert(processed@ == group_changes.entries().len());
                    assert(group_changes.entries().take(processed@) =~= group_changes.entries());
                    assert forall|key: verus_prooflib::ordered::KeyView| #[trigger] count_at(counts, key)
                        == count_at(source_counts@, key) + count_at(*group_changes, key) by {
                        verus_prooflib::lemma_ordered_selected_weight(group_changes, key,
                            |row: (verus_prooflib::ordered::KeyView, Cell)| row.1.count as int, 1int,
                            |row: (verus_prooflib::ordered::KeyView, Cell)| if row.0 == key { row.1.count as int } else { 0int });
                    }
                }
                return Ok((counts, ranks));
            },
        };
        proof {
            assert(remaining@.contains((key.view(), *cell)));
            assert((key.view(), *cell) == group_changes.entries()[processed@]);
            lemma_cells_tail(remaining@);
        }
        if !update_total(&mut counts, &mut ranks, cell.group.clone(), cell.count) { return Err(4); }
        proof {
            assert forall|query: verus_prooflib::ordered::KeyView| #[trigger] count_at(counts, query)
                == count_at(source_counts@, query) + sum(group_changes.entries().take(processed@ + 1),
                    |row: (verus_prooflib::ordered::KeyView, Cell)| if row.0 == query { row.1.count as int } else { 0int }) by {
                lemma_sum_take(group_changes.entries(), processed@,
                    |row: (verus_prooflib::ordered::KeyView, Cell)| if row.0 == query { row.1.count as int } else { 0int });
            }
        }
        processed = Ghost(processed@ + 1);
    }
}

pub fn check_edges_neighbors(edges: &Edges, neighbors: &Index<i64>) -> (ok: bool)
    requires edges_keys_valid(*edges), neighbors.wf(),
    ensures ok ==> neighbors_cover(*neighbors, *edges),
{
    let mut cursor = edges.cursor();
    let mut processed: Ghost<int> = Ghost(0);
    loop
        invariant edges_keys_valid(*edges), neighbors.wf(), 0 <= processed@ <= edges.entries().len(),
            cursor.remaining() == edges.entries().skip(processed@),
            forall|position: int, part: verus_prooflib::ordered::KeyView| 0 <= position < processed@
                && #[trigger] edges.entries()[position].1.at(part).is_some() ==> neighbors.at(part).is_some(),
        decreases cursor.remaining().len(),
    {
        let (supplier, parts) = match cursor.next() {
            Some(row) => row,
            None => {
                proof {
                    assert(processed@ == edges.entries().len());
                    edges.model();
                    assert forall|supplier: int, part: int| #[trigger] edge_at(*edges, supplier, part) != 0 implies
                        neighbors.at((part, Seq::<u8>::empty())).is_some() by {
                        let position = choose|position: int| 0 <= position < edges.entries().len()
                            && (#[trigger] edges.entries()[position]).0 == (supplier, Seq::<u8>::empty());
                        assert(edges.at((supplier, Seq::<u8>::empty())) == Some(edges.entries()[position].1));
                    }
                }
                return true;
            },
        };
        proof {
            edges.model();
            assert((supplier.view(), *parts) == edges.entries()[processed@]);
            assert(edges.at(supplier.view()) == Some(*parts));
        }
        if !covered(&*parts, neighbors) { return false; }
        processed = Ghost(processed@ + 1);
    }
}

pub fn maintain(counts: &Counts, ranks: &Ranks, changes: &Changes,
    edges_for_parts_keys: &[i64], edges_for_parts_rows: &[EdgeInput],
    edges_for_suppliers_keys: &[i64], edges_for_suppliers_rows: &[EdgeInput],
    parts_for_neighbors_keys: &[i64], parts_for_neighbors_rows: &Vec<PartInput>,
    blockers_for_suppliers_keys: &[i64], blockers_for_suppliers_rows: &[BlockerInput],
    Ghost(db): Ghost<Db>, Ghost(db2): Ghost<Db>,
) -> (out: Result<(Counts, Ranks), u32>)
    requires cell_keys_valid(*counts), counts_valid(*counts, db), ranks_valid(*ranks, *counts),
        captured_counts(*changes, db, db2),
        rescan_edges_for_parts_ok(db2, edges_for_parts_keys@, edges_for_parts_rows@),
        rescan_edges_for_suppliers_ok(db2, edges_for_suppliers_keys@, edges_for_suppliers_rows@),
        rescan_parts_for_neighbors_ok(db2, parts_for_neighbors_keys@, parts_for_neighbors_rows@),
        rescan_blockers_for_suppliers_ok(db2, blockers_for_suppliers_keys@, blockers_for_suppliers_rows@),
    ensures out matches Ok(state) ==> cell_keys_valid(state.0)
        && (positive_counts(*counts) ==> positive_counts(state.0))
        && (ranks_valid(*ranks, *counts) ==> ranks_valid(state.1, state.0))
        && counts_valid(state.0, db2),
{
    hide(counts_valid);
    hide(ranks_valid);
    hide(captured_counts);
    hide(affected_valid);
    hide(scoped_edges);
    hide(scoped_parts);
    hide(scoped_blockers);
    hide(neighbors_cover);
    proof { use_type_invariant(changes); }
    let changed_parts = part_support(changes);
    let supplied_parts = keys_from_slice(edges_for_parts_keys);
    if !covered(&changed_parts, &supplied_parts) { return Err(1); }
    proof {
        assert forall|part: int| #[trigger] changed_parts.at((part, Seq::<u8>::empty())).is_some() implies
            seq_view_i64(edges_for_parts_keys@).contains(part) by {
            assert(integer_at(supplied_parts, (part, Seq::<u8>::empty())) > 0);
        }
    }
    let mut suppliers = supplier_support(changes);
    let initial_suppliers = Ghost(suppliers);
    let mut index = 0;
    let mut processed: Ghost<int> = Ghost(0);
    while index < edges_for_parts_rows.len()
        invariant index <= edges_for_parts_rows.len(), processed@ == index as int,
            key_set(suppliers), key_set(changed_parts), key_set(initial_suppliers@),
            forall|part: int| #[trigger] changed_parts.at((part, Seq::<u8>::empty())).is_some() ==> seq_view_i64(edges_for_parts_keys@).contains(part),
            forall|part: int| #[trigger] integer_at(changed_parts, (part, Seq::<u8>::empty()))
                == if changes.part_counts().at((part, Seq::<u8>::empty())).is_some() { 1int } else { 0int },
            forall|supplier: int| #[trigger] integer_at(initial_suppliers@, (supplier, Seq::<u8>::empty()))
                == if changes.edge_counts().at((supplier, Seq::<u8>::empty())).is_some()
                    || changes.blocker_counts().at((supplier, Seq::<u8>::empty())).is_some() { 1int } else { 0int },
            forall|supplier: int| #[trigger] integer_at(suppliers, (supplier, Seq::empty())) ==
                if integer_at(initial_suppliers@, (supplier, Seq::empty())) > 0
                    || sum(edge_input_seq_view(edges_for_parts_rows@.take(processed@)), |row: PartsuppRow|
                        if row.ps_suppkey == supplier
                            && integer_at(changed_parts, (row.ps_partkey, Seq::empty())) > 0 { 1 } else { 0 }) > 0
                { 1int } else { 0int },
        decreases edges_for_parts_rows.len() - index,
    {
        let edge = &edges_for_parts_rows[index];
        if changed_parts.get(&Key::number(edge.ps_partkey)).is_some() {
            insert_key(&mut suppliers, edge.ps_suppkey);
        }
        proof {
            assert forall|supplier: int| #[trigger] integer_at(suppliers, (supplier, Seq::<u8>::empty())) ==
                if integer_at(initial_suppliers@, (supplier, Seq::<u8>::empty())) > 0
                    || sum(edge_input_seq_view(edges_for_parts_rows@.take(processed@ + 1)), |row: PartsuppRow|
                        if row.ps_suppkey == supplier && integer_at(changed_parts, (row.ps_partkey, Seq::<u8>::empty())) > 0 { 1int } else { 0int }) > 0
                    { 1int } else { 0int } by {
                let weight = |row: PartsuppRow| if row.ps_suppkey == supplier
                    && integer_at(changed_parts, (row.ps_partkey, Seq::<u8>::empty())) > 0 { 1int } else { 0int };
                verus_prooflib::lemma_sum_mapped_prefix(edges_for_parts_rows@, processed@,
                    |row: EdgeInput| edge_input_view(row), weight);
                lemma_sum_nonneg(edge_input_seq_view(edges_for_parts_rows@.take(processed@)), weight);
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    proof {
        assert(edges_for_parts_rows@.take(processed@) =~= edges_for_parts_rows@);
        lemma_affected(changes, db2, suppliers, changed_parts, initial_suppliers@, edges_for_parts_keys@, edges_for_parts_rows@);
    }
    let supplied_suppliers = keys_from_slice(edges_for_suppliers_keys);
    let supplied_blockers = keys_from_slice(blockers_for_suppliers_keys);
    if !covered(&suppliers, &supplied_suppliers) { return Err(2); }
    if !covered(&suppliers, &supplied_blockers) { return Err(3); }
    proof {
        assert forall|supplier: int| #[trigger] suppliers.at((supplier, Seq::<u8>::empty())).is_some() implies
            seq_view_i64(edges_for_suppliers_keys@).contains(supplier) && seq_view_i64(blockers_for_suppliers_keys@).contains(supplier) by {
            assert(integer_at(supplied_suppliers, (supplier, Seq::<u8>::empty())) > 0);
            assert(integer_at(supplied_blockers, (supplier, Seq::<u8>::empty())) > 0);
        }
    }
    let mut neighbors: Index<i64> = Index::new();
    let mut new_edges: Edges = Index::new();
    index = 0;
    processed = Ghost(0);
    while index < edges_for_suppliers_rows.len()
        invariant index <= edges_for_suppliers_rows.len(), processed@ == index as int,
            key_set(neighbors), key_set(suppliers), edges_keys_valid(new_edges),
            affected_valid(*changes, db2, suppliers),
            forall|supplier: int| #[trigger] suppliers.at((supplier, Seq::<u8>::empty())).is_some() ==>
                seq_view_i64(edges_for_suppliers_keys@).contains(supplier) && seq_view_i64(blockers_for_suppliers_keys@).contains(supplier),
            forall|part: int| #[trigger] integer_at(neighbors, (part, Seq::empty())) ==
                if sum(edge_input_seq_view(edges_for_suppliers_rows@.take(processed@)), |row: PartsuppRow|
                    if row.ps_partkey == part && integer_at(suppliers, (row.ps_suppkey, Seq::empty())) > 0
                        { 1 } else { 0 }) > 0 { 1int } else { 0int },
            forall|supplier: int, part: int| #[trigger] edge_at(new_edges, supplier, part)
                == sum(edge_input_seq_view(edges_for_suppliers_rows@.take(processed@)), |row: PartsuppRow|
                    if integer_at(suppliers, (supplier, Seq::empty())) > 0 { edge_weight(row, supplier, part) } else { 0 }),
        decreases edges_for_suppliers_rows.len() - index,
    {
        let edge = &edges_for_suppliers_rows[index];
        if suppliers.get(&Key::number(edge.ps_suppkey)).is_some() {
            insert_key(&mut neighbors, edge.ps_partkey);
            if !bump_edge(&mut new_edges, edge.ps_partkey, edge.ps_suppkey, 1) { return Err(4); }
        }
        proof {
            assert forall|supplier: int, part: int| #[trigger] edge_at(new_edges, supplier, part)
                == sum(edge_input_seq_view(edges_for_suppliers_rows@.take(processed@ + 1)), |row: PartsuppRow|
                    if integer_at(suppliers, (supplier, Seq::<u8>::empty())) > 0 { edge_weight(row, supplier, part) } else { 0int }) by {
                verus_prooflib::lemma_sum_mapped_prefix(edges_for_suppliers_rows@, processed@,
                    |row: EdgeInput| edge_input_view(row), |row: PartsuppRow|
                    if integer_at(suppliers, (supplier, Seq::<u8>::empty())) > 0 { edge_weight(row, supplier, part) } else { 0int });
            }
            assert forall|part: int| #[trigger] integer_at(neighbors, (part, Seq::<u8>::empty())) ==
                if sum(edge_input_seq_view(edges_for_suppliers_rows@.take(processed@ + 1)), |row: PartsuppRow|
                    if row.ps_partkey == part && integer_at(suppliers, (row.ps_suppkey, Seq::<u8>::empty())) > 0 { 1int } else { 0int }) > 0 { 1int } else { 0int } by {
                let weight = |row: PartsuppRow| if row.ps_partkey == part
                    && integer_at(suppliers, (row.ps_suppkey, Seq::<u8>::empty())) > 0 { 1int } else { 0int };
                verus_prooflib::lemma_sum_mapped_prefix(edges_for_suppliers_rows@, processed@,
                    |row: EdgeInput| edge_input_view(row), weight);
                lemma_sum_nonneg(edge_input_seq_view(edges_for_suppliers_rows@.take(processed@)), weight);
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    proof {
        assert(edges_for_suppliers_rows@.take(processed@) =~= edges_for_suppliers_rows@);
        lemma_post_edges(db2, suppliers, new_edges, edges_for_suppliers_keys@, edges_for_suppliers_rows@);
    }
    let mut cursor = changes.edges.cursor();
    loop
        invariant key_set(neighbors), affected_valid(*changes, db2, suppliers),
            scoped_edges(new_edges, db2, suppliers),
        decreases cursor.remaining().len(),
    {
        let (_, parts) = match cursor.next() { Some(row) => row, None => break };
        let mut cursor = parts.cursor();
        loop
            invariant key_set(neighbors), affected_valid(*changes, db2, suppliers),
                scoped_edges(new_edges, db2, suppliers),
            decreases cursor.remaining().len(),
        {
            let (part, _) = match cursor.next() { Some(row) => row, None => break };
            insert_key(&mut neighbors, part.number);
        }
    }
    let supplied_neighbors = keys_from_slice(parts_for_neighbors_keys);
    if !covered(&neighbors, &supplied_neighbors) { return Err(5); }
    proof {
        assert forall|part: int| #[trigger] neighbors.at((part, Seq::<u8>::empty())).is_some() implies
            seq_view_i64(parts_for_neighbors_keys@).contains(part) by {
            assert(integer_at(supplied_neighbors, (part, Seq::<u8>::empty())) > 0);
        }
    }
    let vocab = crate::vocab::new_vocab();
    let mut new_parts = Index::new();
    index = 0;
    processed = Ghost(0);
    while index < parts_for_neighbors_rows.len()
        invariant index <= parts_for_neighbors_rows.len(), processed@ == index as int,
            parts_keys_valid(new_parts), vocab.ok(), key_set(neighbors),
            affected_valid(*changes, db2, suppliers), scoped_edges(new_edges, db2, suppliers),
            forall|part: int| #[trigger] neighbors.at((part, Seq::<u8>::empty())).is_some() ==> seq_view_i64(parts_for_neighbors_keys@).contains(part),
            forall|part: int, key: verus_prooflib::ordered::KeyView| #[trigger] part_at(new_parts, part, key)
                == sum(part_input_seq_view(parts_for_neighbors_rows@.take(processed@)), |row: PartRow|
                    if integer_at(neighbors, (part, Seq::empty())) > 0 { part_weight(row, part, key) } else { 0 }),
        decreases parts_for_neighbors_rows.len() - index,
    {
        let row = &parts_for_neighbors_rows[index];
        if neighbors.get(&Key::number(row.p_partkey)).is_some() && !bump_part(&vocab, &mut new_parts, row, 1) { return Err(4); }
        proof {
            assert forall|part: int, key: verus_prooflib::ordered::KeyView| #[trigger] part_at(new_parts, part, key)
                == sum(part_input_seq_view(parts_for_neighbors_rows@.take(processed@ + 1)), |row: PartRow|
                    if integer_at(neighbors, (part, Seq::<u8>::empty())) > 0 { part_weight(row, part, key) } else { 0int }) by {
                verus_prooflib::lemma_sum_mapped_prefix(parts_for_neighbors_rows@, processed@,
                    |row: PartInput| part_input_view(row), |row: PartRow|
                    if integer_at(neighbors, (part, Seq::<u8>::empty())) > 0 { part_weight(row, part, key) } else { 0int });
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    proof {
        assert(parts_for_neighbors_rows@.take(processed@) =~= parts_for_neighbors_rows@);
        lemma_post_parts(db2, neighbors, new_parts, parts_for_neighbors_keys@, parts_for_neighbors_rows@);
    }
    let mut new_blockers = Index::new();
    index = 0;
    processed = Ghost(0);
    while index < blockers_for_suppliers_rows.len()
        invariant index <= blockers_for_suppliers_rows.len(), processed@ == index as int,
            integer_keys(new_blockers), key_set(suppliers),
            affected_valid(*changes, db2, suppliers), scoped_edges(new_edges, db2, suppliers), scoped_parts(new_parts, db2, neighbors),
            forall|supplier: int| #[trigger] suppliers.at((supplier, Seq::<u8>::empty())).is_some() ==> seq_view_i64(blockers_for_suppliers_keys@).contains(supplier),
            forall|supplier: int| #[trigger] integer_at(new_blockers, (supplier, Seq::empty()))
                == sum(blocker_input_seq_view(blockers_for_suppliers_rows@.take(processed@)), |key: int|
                    if key == supplier && integer_at(suppliers, (supplier, Seq::empty())) > 0 { 1 } else { 0 }),
        decreases blockers_for_suppliers_rows.len() - index,
    {
        let row = &blockers_for_suppliers_rows[index];
        if suppliers.get(&Key::number(row.s_suppkey)).is_some() && !bump(&mut new_blockers, Key::number(row.s_suppkey), 1) { return Err(4); }
        proof {
            assert forall|supplier: int| #[trigger] integer_at(new_blockers, (supplier, Seq::<u8>::empty()))
                == sum(blocker_input_seq_view(blockers_for_suppliers_rows@.take(processed@ + 1)), |key: int|
                    if key == supplier && integer_at(suppliers, (supplier, Seq::<u8>::empty())) > 0 { 1int } else { 0int }) by {
                verus_prooflib::lemma_sum_mapped_prefix(blockers_for_suppliers_rows@, processed@,
                    |row: BlockerInput| blocker_input_view(row), |key: int|
                    if key == supplier && integer_at(suppliers, (supplier, Seq::<u8>::empty())) > 0 { 1int } else { 0int });
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    proof {
        assert(blockers_for_suppliers_rows@.take(processed@) =~= blockers_for_suppliers_rows@);
        lemma_post_blockers(db2, suppliers, new_blockers, blockers_for_suppliers_keys@, blockers_for_suppliers_rows@);
    }
    let old_parts = match restore_parts(&new_parts, &changes.parts, &neighbors) { Some(value) => value, None => return Err(4) };
    let old_edges = match restore_edges(&new_edges, &changes.edges) { Some(value) => value, None => return Err(4) };
    let old_blockers = match restore_blockers(&new_blockers, &changes.blockers) { Some(value) => value, None => return Err(4) };
    proof {
        lemma_restore_scopes(db, db2, changes, suppliers, neighbors, old_parts, old_edges, old_blockers, new_parts, new_edges, new_blockers);
    }
    if !check_edges_neighbors(&old_edges, &neighbors) || !check_edges_neighbors(&new_edges, &neighbors) { return Err(5); }
    let group_changes = match collect_changes(&old_parts, &old_edges, &old_blockers,
        &new_parts, &new_edges, &new_blockers, &suppliers) {
        Ok(groups) => groups,
        Err(code) => return Err(code),
    };
    let state = match publish_changes(counts, ranks, &group_changes) {
        Ok(state) => state,
        Err(code) => return Err(code),
    };
    proof {
        lemma_ranks_positive(*ranks, *counts);
        lemma_maintenance_result(db, db2, changes, suppliers, neighbors,
            old_parts, old_edges, old_blockers, new_parts, new_edges, new_blockers,
            *counts, state.0, group_changes, state.1);
    }
    Ok(state)
}

}
