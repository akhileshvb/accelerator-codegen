use vstd::prelude::*;
use std::sync::Arc;
use verus_common::sums::*;
use verus_prooflib::ordered::{Index, Key, KeyView};
use crate::gen_obligations::*;
use crate::gen_spec::*;
use crate::model::*;
use crate::counts::*;
use crate::input_proofs::*;
use crate::maintenance_proofs::*;
use crate::rank_proofs::*;
use crate::bounds::*;
use crate::contrib::*;
use crate::maintenance_semantics::*;

verus! {

pub open spec fn key_seen(entries: Seq<(KeyView, i128)>, count: int, query: KeyView) -> bool {
    exists|position: int| 0 <= position < count && (#[trigger] entries[position]).0 == query
}

pub proof fn lemma_key_seen_step(entries: Seq<(KeyView, i128)>, count: int, query: KeyView)
    requires 0 <= count < entries.len(),
    ensures key_seen(entries, count + 1, query) == (key_seen(entries, count, query) || entries[count].0 == query),
{
    if key_seen(entries, count + 1, query) && !key_seen(entries, count, query) {
        let position = choose|position: int| 0 <= position < count + 1 && (#[trigger] entries[position]).0 == query;
        assert(position == count);
    }
    if key_seen(entries, count, query) {
        let position = choose|position: int| 0 <= position < count && (#[trigger] entries[position]).0 == query;
        assert(0 <= position < count + 1 && entries[position].0 == query);
    }
    if entries[count].0 == query { assert(key_seen(entries, count + 1, query)); }
}

/// Removes every key of a key set from a map.
#[verifier::rlimit(60)]
pub fn remove_keys<Value>(map: &mut Index<Value>, keys: &Index<i128>)
    requires old(map).wf(), key_set(*keys),
    ensures final(map).wf(),
        forall|query: KeyView| #[trigger] final(map).at(query) == if keys.at(query).is_some() { None } else { old(map).at(query) },
{
    let initial = Ghost(*map);
    proof { keys.model(); }
    let mut cursor = keys.cursor();
    let mut processed: Ghost<int> = Ghost(0);
    loop
        invariant map.wf(), key_set(*keys), initial@ == *old(map),
            0 <= processed@ <= keys.entries().len(), cursor.remaining() == keys.entries().skip(processed@),
            forall|position: int| 0 <= position < keys.entries().len() ==>
                #[trigger] keys.at(keys.entries()[position].0) == Some(keys.entries()[position].1),
            forall|query: KeyView| #[trigger] map.at(query) == if key_seen(keys.entries(), processed@, query) { None } else { initial@.at(query) },
        decreases cursor.remaining().len(),
    {
        let (key, _) = match cursor.next() {
            Some(row) => row,
            None => {
                proof {
                    keys.model();
                    assert(processed@ == keys.entries().len());
                    assert forall|query: KeyView| #[trigger] map.at(query) == if keys.at(query).is_some() { None } else { initial@.at(query) } by {
                        if key_seen(keys.entries(), processed@, query) {
                            let position = choose|position: int| 0 <= position < processed@ && (#[trigger] keys.entries()[position]).0 == query;
                            assert(keys.at(keys.entries()[position].0) == Some(keys.entries()[position].1));
                        }
                        if keys.at(query).is_some() {
                            let position = choose|position: int| #![auto] 0 <= position < keys.entries().len() && keys.entries()[position].0 == query;
                            assert(key_seen(keys.entries(), processed@, query));
                        }
                    }
                }
                return;
            },
        };
        proof { assert(key.view() == keys.entries()[processed@].0); }
        map.remove(&key);
        proof {
            assert forall|query: KeyView| #[trigger] map.at(query) == if key_seen(keys.entries(), processed@ + 1, query) { None } else { initial@.at(query) } by {
                lemma_key_seen_step(keys.entries(), processed@, query);
            }
        }
        processed = Ghost(processed@ + 1);
    }
}

#[verifier::rlimit(60)]
pub fn replace_parts(parts: &Parts, changed: &Index<i128>, rows: &Vec<PartInput>, Ghost(fits): Ghost<bool>) -> (out: Option<Parts>)
    requires parts_keys_valid(*parts), key_set(*changed),
        fits ==> rows@.len() <= max_rows(),
        fits ==> forall|part: int, key: KeyView| #[trigger] part_at(*parts, part, key) <= max_rows(),
    ensures out matches Some(result) ==> parts_keys_valid(result)
            && forall|part: int, key: KeyView| #[trigger] part_at(result, part, key)
                == (if changed.at((part, Seq::<u8>::empty())).is_some() { 0int } else { part_at(*parts, part, key) })
                    + part_input_sum(rows@, part, key),
        out is None ==> !fits,
{
    let mut out = share_parts(parts);
    remove_keys(&mut out, changed);
    let base = Ghost(out);
    proof {
        assert forall|key: KeyView| #[trigger] out.at(key).is_some() implies key.1 == Seq::<u8>::empty() && part_groups_valid(out.at(key).unwrap()) by {
            assert(parts.at(key) == out.at(key));
        }
        assert forall|part: int, key: KeyView| #[trigger] part_at(base@, part, key)
            == if changed.at((part, Seq::<u8>::empty())).is_some() { 0int } else { part_at(*parts, part, key) } by {
            assert(changed.at((part, Seq::<u8>::empty())).is_some() ==> base@.at((part, Seq::<u8>::empty())).is_none());
            if changed.at((part, Seq::<u8>::empty())).is_some() { assert(part_at(base@, part, key) == 0int); }
        }
    }
    let vocab = crate::vocab::new_vocab();
    let mut index = 0;
    let mut processed: Ghost<int> = Ghost(0);
    while index < rows.len()
        invariant index <= rows.len(), processed@ == index as int, parts_keys_valid(out), vocab.ok(), parts_keys_valid(*parts),
            key_set(*changed),
            fits ==> rows@.len() <= max_rows(),
            fits ==> forall|part: int, key: KeyView| #[trigger] part_at(*parts, part, key) <= max_rows(),
            forall|part: int, key: KeyView| #[trigger] part_at(base@, part, key)
                == if changed.at((part, Seq::<u8>::empty())).is_some() { 0int } else { part_at(*parts, part, key) },
            forall|part: int, key: KeyView| #[trigger] part_at(out, part, key)
                == part_at(base@, part, key) + part_input_sum(rows@.take(processed@), part, key),
        decreases rows.len() - index,
    {
        if !bump_part(&vocab, &mut out, &rows[index], 1) {
            proof {
                if fits {
                    let row = rows@[index as int];
                    let key = group_key(row.p_brand@, row.p_type@, row.p_size as int);
                    crate::bounds::lemma_part_input_bound(rows@.take(processed@), row.p_partkey as int, key);
                    assert(max_rows() == 9223372036854775808int);
                    assert(part_at(base@, row.p_partkey as int, key) <= max_rows());
                }
            }
            return None;
        }
        proof { lemma_parts_prefix(rows@, processed@); }
        index += 1;
        processed = Ghost(index as int);
    }
    proof { assert(rows@.take(processed@) =~= rows@); }
    Some(out)
}

#[verifier::rlimit(60)]
pub fn replace_edges(edges: &Edges, suppliers: &Index<i128>, rows: &[EdgeInput], Ghost(fits): Ghost<bool>) -> (out: Option<Edges>)
    requires edges_keys_valid(*edges), key_set(*suppliers),
        fits ==> rows@.len() <= max_rows(),
        fits ==> forall|supplier: int, part: int| #[trigger] edge_at(*edges, supplier, part) <= max_rows(),
    ensures out matches Some(result) ==> edges_keys_valid(result)
            && forall|supplier: int, part: int| #[trigger] edge_at(result, supplier, part)
                == (if suppliers.at((supplier, Seq::<u8>::empty())).is_some() { 0int } else { edge_at(*edges, supplier, part) })
                    + edge_input_sum(rows@, supplier, part),
        out is None ==> !fits,
{
    let mut out = share_edges(edges);
    remove_keys(&mut out, suppliers);
    let base = Ghost(out);
    proof {
        assert forall|key: KeyView| #[trigger] out.at(key).is_some() implies
            key.1 == Seq::<u8>::empty() && out.at(key).unwrap().wf() && integer_keys(out.at(key).unwrap()) by {
            assert(edges.at(key) == out.at(key));
        }
        assert forall|supplier: int, part: int| #[trigger] edge_at(base@, supplier, part)
            == if suppliers.at((supplier, Seq::<u8>::empty())).is_some() { 0int } else { edge_at(*edges, supplier, part) } by {
            if suppliers.at((supplier, Seq::<u8>::empty())).is_some() { assert(base@.at((supplier, Seq::<u8>::empty())).is_none()); }
        }
    }
    let mut index = 0;
    let mut processed: Ghost<int> = Ghost(0);
    while index < rows.len()
        invariant index <= rows.len(), processed@ == index as int, edges_keys_valid(out), edges_keys_valid(*edges),
            key_set(*suppliers),
            fits ==> rows@.len() <= max_rows(),
            fits ==> forall|supplier: int, part: int| #[trigger] edge_at(*edges, supplier, part) <= max_rows(),
            forall|supplier: int, part: int| #[trigger] edge_at(base@, supplier, part)
                == if suppliers.at((supplier, Seq::<u8>::empty())).is_some() { 0int } else { edge_at(*edges, supplier, part) },
            forall|supplier: int, part: int| #[trigger] edge_at(out, supplier, part)
                == edge_at(base@, supplier, part) + edge_input_sum(rows@.take(processed@), supplier, part),
        decreases rows.len() - index,
    {
        let row = &rows[index];
        if !bump_edge(&mut out, row.ps_partkey, row.ps_suppkey, 1) {
            proof {
                if fits {
                    crate::bounds::lemma_edge_input_bound(rows@.take(processed@), row.ps_suppkey as int, row.ps_partkey as int);
                    assert(max_rows() == 9223372036854775808int);
                    assert(edge_at(base@, row.ps_suppkey as int, row.ps_partkey as int) <= max_rows());
                }
            }
            return None;
        }
        proof { lemma_edges_prefix(rows@, processed@); }
        index += 1;
        processed = Ghost(index as int);
    }
    proof { assert(rows@.take(processed@) =~= rows@); }
    Some(out)
}

pub fn replace_blockers(blocked: &Index<i128>, suppliers: &Index<i128>, rows: &[BlockerInput], Ghost(fits): Ghost<bool>) -> (out: Option<Index<i128>>)
    requires integer_keys(*blocked), key_set(*suppliers),
        fits ==> rows@.len() <= max_rows(),
        fits ==> forall|supplier: int| #[trigger] integer_at(*blocked, (supplier, Seq::empty())) <= max_rows(),
    ensures out matches Some(result) ==> integer_keys(result)
            && forall|supplier: int| #[trigger] integer_at(result, (supplier, Seq::empty()))
                == (if suppliers.at((supplier, Seq::<u8>::empty())).is_some() { 0int } else { integer_at(*blocked, (supplier, Seq::empty())) })
                    + blocker_input_sum(rows@, supplier),
        out is None ==> !fits,
{
    let mut out = share_integers(blocked);
    remove_keys(&mut out, suppliers);
    let base = Ghost(out);
    proof {
        assert forall|key: KeyView| #[trigger] out.at(key).is_some() implies key.1 == Seq::<u8>::empty() by {
            assert(blocked.at(key) == out.at(key));
        }
        assert forall|supplier: int| #[trigger] integer_at(base@, (supplier, Seq::empty()))
            == if suppliers.at((supplier, Seq::<u8>::empty())).is_some() { 0int } else { integer_at(*blocked, (supplier, Seq::empty())) } by {
            if suppliers.at((supplier, Seq::<u8>::empty())).is_some() { assert(base@.at((supplier, Seq::<u8>::empty())).is_none()); }
        }
    }
    let mut index = 0;
    let mut processed: Ghost<int> = Ghost(0);
    while index < rows.len()
        invariant index <= rows.len(), processed@ == index as int, integer_keys(out), integer_keys(*blocked), key_set(*suppliers),
            fits ==> rows@.len() <= max_rows(),
            fits ==> forall|supplier: int| #[trigger] integer_at(*blocked, (supplier, Seq::empty())) <= max_rows(),
            forall|supplier: int| #[trigger] integer_at(base@, (supplier, Seq::empty()))
                == if suppliers.at((supplier, Seq::<u8>::empty())).is_some() { 0int } else { integer_at(*blocked, (supplier, Seq::empty())) },
            forall|supplier: int| #[trigger] integer_at(out, (supplier, Seq::empty()))
                == integer_at(base@, (supplier, Seq::empty())) + blocker_input_sum(rows@.take(processed@), supplier),
        decreases rows.len() - index,
    {
        if !bump(&mut out, Key::number(rows[index].s_suppkey), 1) {
            proof {
                if fits {
                    crate::bounds::lemma_blocker_input_bound(rows@.take(processed@), rows@[index as int].s_suppkey as int);
                    assert(max_rows() == 9223372036854775808int);
                    assert(integer_at(base@, (rows@[index as int].s_suppkey as int, Seq::<u8>::empty())) <= max_rows());
                }
            }
            return None;
        }
        proof { lemma_blockers_prefix(rows@, processed@); }
        index += 1;
        processed = Ghost(index as int);
    }
    proof { assert(rows@.take(processed@) =~= rows@); }
    Some(out)
}

pub open spec fn edge_entry(edges: Edges, supplier: int, part: int) -> bool {
    match edges.at((supplier, Seq::<u8>::empty())) {
        Some(inner) => inner.at((part, Seq::<u8>::empty())).is_some(),
        None => false,
    }
}

/// Zeroes the cell (outer, inner_key) of a two-level table.
#[verifier::rlimit(60)]
pub fn clear_edge(edges: &mut Edges, outer: i64, inner_key: i64)
    requires edges_keys_valid(*old(edges)),
    ensures edges_keys_valid(*final(edges)),
        forall|query_outer: int, query_inner: int| #[trigger] edge_at(*final(edges), query_outer, query_inner)
            == if query_outer == outer as int && query_inner == inner_key as int { 0int } else { edge_at(*old(edges), query_outer, query_inner) },
{
    let initial = Ghost(*edges);
    let outer_key = Key::number(outer);
    let found = edges.get(&outer_key);
    match found {
        None => {
            proof {
                assert(edges.at((outer as int, Seq::<u8>::empty())).is_none());
                assert forall|query_outer: int, query_inner: int| #[trigger] edge_at(*edges, query_outer, query_inner)
                    == if query_outer == outer as int && query_inner == inner_key as int { 0int } else { edge_at(initial@, query_outer, query_inner) } by {}
            }
        },
        Some(value) => {
            proof { assert(integer_keys(*value)); }
            let mut inner = share_integers(&*value);
            proof {
                assert(edges.at((outer as int, Seq::<u8>::empty())) == Some(*value));
                assert forall|query: int| #[trigger] integer_at(inner, (query, Seq::<u8>::empty())) == integer_at(*value, (query, Seq::<u8>::empty())) by {}
            }
            let before_inner = Ghost(inner);
            inner.remove(&Key::number(inner_key));
            let after_inner = Ghost(inner);
            proof { inner.map_model(); }
            if inner.is_empty() { edges.remove(&outer_key); }
            else { edges.put(outer_key, Arc::new(inner)); }
            proof {
                assert forall|query_outer: int, query_inner: int| #[trigger] edge_at(*edges, query_outer, query_inner)
                    == if query_outer == outer as int && query_inner == inner_key as int { 0int } else { edge_at(initial@, query_outer, query_inner) } by {
                    if query_outer == outer as int {
                        if after_inner@.entries().len() == 0 {
                            assert(edges.at((query_outer, Seq::<u8>::empty())).is_none());
                            assert(after_inner@.at((query_inner, Seq::<u8>::empty())).is_none());
                        } else {
                            assert(edges.at((query_outer, Seq::<u8>::empty())) == Some(after_inner@));
                        }
                        assert(edge_at(initial@, query_outer, query_inner) == integer_at(before_inner@, (query_inner, Seq::<u8>::empty())));
                    } else {
                        assert(edges.at((query_outer, Seq::<u8>::empty())) == initial@.at((query_outer, Seq::<u8>::empty())));
                    }
                }
                assert forall|key: KeyView| #[trigger] edges.at(key).is_some() implies
                    key.1 == Seq::<u8>::empty() && edges.at(key).unwrap().wf() && integer_keys(edges.at(key).unwrap()) by {
                    if key == (outer as int, Seq::<u8>::empty()) {
                        assert(edges.at(key) == Some(after_inner@));
                    } else {
                        assert(edges.at(key) == initial@.at(key));
                    }
                }
            }
        },
    }
}

/// Sets the cell (outer, inner_key) of a two-level table.
pub fn put_edge(edges: &mut Edges, outer: i64, inner_key: i64, value: i128)
    requires edges_keys_valid(*old(edges)),
    ensures edges_keys_valid(*final(edges)),
        forall|query_outer: int, query_inner: int| #[trigger] edge_at(*final(edges), query_outer, query_inner)
            == if query_outer == outer as int && query_inner == inner_key as int { value as int } else { edge_at(*old(edges), query_outer, query_inner) },
{
    clear_edge(edges, outer, inner_key);
    let cleared = Ghost(*edges);
    if value != 0 {
        if !bump_edge(edges, inner_key, outer, value) { proof { assert(false); } return; }
    }
}

/// Rewrites the cells of one supplier's column in a part-first table from its row in a supplier-first table.
#[verifier::rlimit(60)]
pub fn rewrite_column(links: &mut Edges, supplier: i64, inner: &Index<i128>, keep: bool)
    requires edges_keys_valid(*old(links)), integer_keys(*inner),
    ensures edges_keys_valid(*final(links)),
        forall|part: int, query_supplier: int| #[trigger] edge_at(*final(links), part, query_supplier)
            == if query_supplier == supplier as int && inner.at((part, Seq::<u8>::empty())).is_some() {
                if keep { integer_at(*inner, (part, Seq::<u8>::empty())) } else { 0int }
            } else { edge_at(*old(links), part, query_supplier) },
{
    let initial = Ghost(*links);
    proof { inner.model(); }
    let mut cursor = inner.cursor();
    let mut processed: Ghost<int> = Ghost(0);
    loop
        invariant edges_keys_valid(*links), integer_keys(*inner), initial@ == *old(links),
            0 <= processed@ <= inner.entries().len(), cursor.remaining() == inner.entries().skip(processed@),
            forall|position: int| 0 <= position < inner.entries().len() ==>
                #[trigger] inner.at(inner.entries()[position].0) == Some(inner.entries()[position].1),
            forall|part: int, query_supplier: int| #[trigger] edge_at(*links, part, query_supplier)
                == if query_supplier == supplier as int && key_seen(inner.entries(), processed@, (part, Seq::<u8>::empty())) {
                    if keep { integer_at(*inner, (part, Seq::<u8>::empty())) } else { 0int }
                } else { edge_at(initial@, part, query_supplier) },
        decreases cursor.remaining().len(),
    {
        let (key, value) = match cursor.next() {
            Some(row) => row,
            None => {
                proof {
                    inner.model();
                    assert(processed@ == inner.entries().len());
                    assert forall|part: int, query_supplier: int| #[trigger] edge_at(*links, part, query_supplier)
                        == if query_supplier == supplier as int && inner.at((part, Seq::<u8>::empty())).is_some() {
                            if keep { integer_at(*inner, (part, Seq::<u8>::empty())) } else { 0int }
                        } else { edge_at(initial@, part, query_supplier) } by {
                        if key_seen(inner.entries(), processed@, (part, Seq::<u8>::empty())) {
                            let position = choose|position: int| 0 <= position < processed@ && (#[trigger] inner.entries()[position]).0 == (part, Seq::<u8>::empty());
                            assert(inner.at(inner.entries()[position].0) == Some(inner.entries()[position].1));
                        }
                        if inner.at((part, Seq::<u8>::empty())).is_some() {
                            let position = choose|position: int| #![auto] 0 <= position < inner.entries().len() && inner.entries()[position].0 == (part, Seq::<u8>::empty());
                            assert(key_seen(inner.entries(), processed@, (part, Seq::<u8>::empty())));
                        }
                    }
                }
                return;
            },
        };
        proof {
            assert((key.view(), *value) == inner.entries()[processed@]);
            assert(inner.at(inner.entries()[processed@].0) == Some(inner.entries()[processed@].1));
            assert(inner.at(key.view()).is_some());
            assert(key.view() == (key.number as int, Seq::<u8>::empty()));
        }
        if keep { put_edge(links, key.number, supplier, *value); } else { clear_edge(links, key.number, supplier); }
        proof {
            assert forall|part: int, query_supplier: int| #[trigger] edge_at(*links, part, query_supplier)
                == if query_supplier == supplier as int && key_seen(inner.entries(), processed@ + 1, (part, Seq::<u8>::empty())) {
                    if keep { integer_at(*inner, (part, Seq::<u8>::empty())) } else { 0int }
                } else { edge_at(initial@, part, query_supplier) } by {
                lemma_key_seen_step(inner.entries(), processed@, (part, Seq::<u8>::empty()));
                if part == key.number as int && query_supplier == supplier as int {
                    assert(inner.entries()[processed@].0 == (part, Seq::<u8>::empty()));
                    assert(integer_at(*inner, (part, Seq::<u8>::empty())) == *value as int);
                }
            }
        }
        processed = Ghost(processed@ + 1);
    }
}

pub open spec fn within(db: Db, db2: Db) -> bool {
    envelope(db) && envelope(db2)
}

#[verifier::rlimit(200)]
pub fn collect_changes(old_parts: &Parts, old_edges: &Edges, old_blockers: &Index<i128>,
    new_parts: &Parts, new_edges: &Edges, new_blockers: &Index<i128>, suppliers: &Index<i128>,
    Ghost(db): Ghost<Db>, Ghost(db2): Ghost<Db>,
) -> (out: Result<Counts, u32>)
    requires parts_keys_valid(*old_parts), parts_keys_valid(*new_parts),
        edges_keys_valid(*old_edges), edges_keys_valid(*new_edges),
        integer_keys(*old_blockers), integer_keys(*new_blockers), key_set(*suppliers),
        scanned_parts(*old_parts, db), scanned_edges(*old_edges, db), scanned_blockers(*old_blockers, db),
        scanned_parts(*new_parts, db2), scanned_edges(*new_edges, db2), scanned_blockers(*new_blockers, db2),
    ensures out matches Ok(groups) ==> cell_keys_valid(groups)
        && forall|key: KeyView| #[trigger] count_at(groups, key)
            == sum(suppliers.entries(), |row: (KeyView, i128)|
                supplier_contribution(*new_parts, *new_edges, *new_blockers, row.0.0, key)
                    - supplier_contribution(*old_parts, *old_edges, *old_blockers, row.0.0, key)),
        out is Err ==> !within(db, db2),
{
    let fits = Ghost(within(db, db2));
    proof {
        assert forall|part: int, key: KeyView| #[trigger] part_at(*old_parts, part, key) >= 0 by { lemma_part_sum_bound(db, part, key); }
        assert forall|part: int, key: KeyView| #[trigger] part_at(*new_parts, part, key) >= 0 by { lemma_part_sum_bound(db2, part, key); }
        suppliers.model();
        lemma_database_suppliers_len(db);
        lemma_database_suppliers_len(db2);
    }
    let mut group_changes: Counts = Index::new();
    let mut cursor = suppliers.cursor();
    let mut processed: Ghost<int> = Ghost(0);
    loop
        invariant cell_keys_valid(group_changes), parts_keys_valid(*old_parts), parts_keys_valid(*new_parts),
            edges_keys_valid(*old_edges), edges_keys_valid(*new_edges), key_set(*suppliers),
            integer_keys(*old_blockers), integer_keys(*new_blockers),
            scanned_parts(*old_parts, db), scanned_edges(*old_edges, db), scanned_blockers(*old_blockers, db),
            scanned_parts(*new_parts, db2), scanned_edges(*new_edges, db2), scanned_blockers(*new_blockers, db2),
            nonneg_parts(*old_parts), nonneg_parts(*new_parts),
            fits@ == within(db, db2),
            database_suppliers(db).len() <= db.partsupp.len(), database_suppliers(db2).len() <= db2.partsupp.len(),
            0 <= processed@ <= suppliers.entries().len(),
            cursor.remaining() == suppliers.entries().skip(processed@),
            verus_prooflib::ordered::ordered_rows(suppliers.entries()),
            forall|position: int| 0 <= position < suppliers.entries().len() ==> (#[trigger] suppliers.entries()[position]).0.1 == Seq::<u8>::empty(),
            forall|key: KeyView| #[trigger] count_at(group_changes, key)
                == sum(suppliers.entries().take(processed@), |row: (KeyView, i128)|
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
            assert(supplier.view() == (supplier.number as int, Seq::<u8>::empty()));
        }
        let initial = Ghost(group_changes);
        let before = match old_blockers.get(&supplier) { Some(value) => *value, None => 0 };
        let after = match new_blockers.get(&supplier) { Some(value) => *value, None => 0 };
        proof {
            assert(before as int == integer_at(*old_blockers, supplier.view()));
            assert(after as int == integer_at(*new_blockers, supplier.view()));
            lemma_blocker_count_bound(db, supplier.number as int);
            lemma_blocker_count_bound(db2, supplier.number as int);
        }
        if before == 0 {
            if let Some(inner) = old_edges.get(&supplier) {
                proof {
                    inner.model();
                    assert(old_edges.at(supplier.view()) == Some(*inner));
                    assert forall|position: int| 0 <= position < inner.entries().len() implies (#[trigger] inner.entries()[position]).1 >= 0 by {
                        let entry = inner.entries()[position];
                        assert(inner.at(entry.0) == Some(entry.1));
                        assert(edge_at(*old_edges, supplier.number as int, entry.0.0) == entry.1 as int);
                        lemma_edge_count_bound(db, entry.0.0, supplier.number as int);
                    }
                }
                let groups = join(old_parts, &*inner);
                proof {
                    assert forall|key: KeyView| contribution(groups, key, -1i64) == -supplier_contribution(*old_parts, *old_edges, *old_blockers, supplier.number as int, key) by {
                        lemma_hit_joined(*old_parts, inner.entries(), key);
                        assert(integer_at(*old_blockers, supplier.view()) == 0);
                    }
                    if within(db, db2) {
                        assert forall|key: KeyView| !out_of_range(count_at(group_changes, key) + contribution(groups, key, -1i64)) by {
                            lemma_old_call_fits(*old_parts, *old_edges, *old_blockers, *new_parts, *new_edges, *new_blockers, db, db2,
                                suppliers.entries(), processed@, key);
                        }
                    }
                }
                if !collect_contribution(&mut group_changes, &groups, -1, fits) { return Err(4); }
            }
        }
        proof {
            assert forall|key: KeyView| #[trigger] count_at(group_changes, key)
                == count_at(initial@, key) - supplier_contribution(*old_parts, *old_edges, *old_blockers, supplier.number as int, key) by {
                if before == 0 {
                    if old_edges.at(supplier.view()).is_none() { assert(supplier_contribution(*old_parts, *old_edges, *old_blockers, supplier.number as int, key) == 0); }
                } else {
                    assert(supplier_contribution(*old_parts, *old_edges, *old_blockers, supplier.number as int, key) == 0);
                }
            }
        }
        let middle = Ghost(group_changes);
        if after == 0 {
            if let Some(inner) = new_edges.get(&supplier) {
                proof {
                    inner.model();
                    assert(new_edges.at(supplier.view()) == Some(*inner));
                    assert forall|position: int| 0 <= position < inner.entries().len() implies (#[trigger] inner.entries()[position]).1 >= 0 by {
                        let entry = inner.entries()[position];
                        assert(inner.at(entry.0) == Some(entry.1));
                        assert(edge_at(*new_edges, supplier.number as int, entry.0.0) == entry.1 as int);
                        lemma_edge_count_bound(db2, entry.0.0, supplier.number as int);
                    }
                }
                let groups = join(new_parts, &*inner);
                proof {
                    assert forall|key: KeyView| contribution(groups, key, 1i64) == supplier_contribution(*new_parts, *new_edges, *new_blockers, supplier.number as int, key) by {
                        lemma_hit_joined(*new_parts, inner.entries(), key);
                        assert(integer_at(*new_blockers, supplier.view()) == 0);
                    }
                    if within(db, db2) {
                        assert forall|key: KeyView| !out_of_range(count_at(group_changes, key) + contribution(groups, key, 1i64)) by {
                            lemma_new_call_fits(*old_parts, *old_edges, *old_blockers, *new_parts, *new_edges, *new_blockers, db, db2,
                                suppliers.entries(), processed@, key);
                        }
                    }
                }
                if !collect_contribution(&mut group_changes, &groups, 1, fits) { return Err(4); }
            }
        }
        proof {
            assert forall|key: KeyView| #[trigger] count_at(group_changes, key)
                == count_at(middle@, key) + supplier_contribution(*new_parts, *new_edges, *new_blockers, supplier.number as int, key) by {
                if after == 0 {
                    if new_edges.at(supplier.view()).is_none() { assert(supplier_contribution(*new_parts, *new_edges, *new_blockers, supplier.number as int, key) == 0); }
                } else {
                    assert(supplier_contribution(*new_parts, *new_edges, *new_blockers, supplier.number as int, key) == 0);
                }
            }
            assert forall|key: KeyView| #[trigger] count_at(group_changes, key)
                == sum(suppliers.entries().take(processed@ + 1), |row: (KeyView, i128)|
                    supplier_contribution(*new_parts, *new_edges, *new_blockers, row.0.0, key)
                        - supplier_contribution(*old_parts, *old_edges, *old_blockers, row.0.0, key)) by {
                lemma_sum_take(suppliers.entries(), processed@, |row: (KeyView, i128)|
                    supplier_contribution(*new_parts, *new_edges, *new_blockers, row.0.0, key)
                        - supplier_contribution(*old_parts, *old_edges, *old_blockers, row.0.0, key));
            }
        }
        processed = Ghost(processed@ + 1);
    }
}

#[verifier::rlimit(60)]
pub fn publish_changes(source: &Counts, base_ranks: &Ranks, group_changes: &Counts, Ghost(fits): Ghost<bool>) -> (out: Result<(Counts, Ranks), u32>)
    requires cell_keys_valid(*source), cell_keys_valid(*group_changes),
        fits ==> forall|brand: Seq<u8>, kind: Seq<u8>, size: int|
            0 <= count_at(*source, #[trigger] group_key(brand, kind, size)) + count_at(*group_changes, group_key(brand, kind, size)) <= i64::MAX as int,
    ensures out matches Ok(state) ==> cell_keys_valid(state.0),
        out matches Ok(state) ==> (positive_counts(*source) ==> positive_counts(state.0)),
        out matches Ok(state) ==> (ranks_valid(*base_ranks, *source) ==> ranks_valid(state.1, state.0)),
        out matches Ok(state) ==> forall|key: KeyView| #[trigger] count_at(state.0, key)
            == count_at(*source, key) + count_at(*group_changes, key),
        out is Err ==> !fits,
{
    let source_counts = Ghost(*source);
    let source_ranks = Ghost(*base_ranks);
    let preserve = Ghost(ranks_valid(*base_ranks, *source));
    let mut counts = share_counts(source);
    let mut ranks = base_ranks.share();
    proof {
        group_changes.model();
        verus_prooflib::ordered::lemma_ordered_unique(group_changes.entries());
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
            fits ==> forall|brand: Seq<u8>, kind: Seq<u8>, size: int|
                0 <= count_at(source_counts@, #[trigger] group_key(brand, kind, size)) + count_at(*group_changes, group_key(brand, kind, size)) <= i64::MAX as int,
            forall|position: int| 0 <= position < group_changes.entries().len() ==>
                #[trigger] group_changes.at(group_changes.entries()[position].0) == Some(group_changes.entries()[position].1),
            forall|first: int, last: int| 0 <= first < last < group_changes.entries().len() ==>
                (#[trigger] group_changes.entries()[first]).0 != (#[trigger] group_changes.entries()[last]).0,
            forall|key: KeyView| #[trigger] count_at(counts, key)
                == count_at(source_counts@, key) + sum(group_changes.entries().take(processed@),
                    |row: (KeyView, Cell)| if row.0 == key { row.1.count as int } else { 0int }),
        decreases cursor.remaining().len(),
    {
        let remaining = Ghost(cursor.remaining());
        let (key, cell) = match cursor.next() {
            Some(row) => row,
            None => {
                proof {
                    assert(processed@ == group_changes.entries().len());
                    assert(group_changes.entries().take(processed@) =~= group_changes.entries());
                    assert forall|key: KeyView| #[trigger] count_at(counts, key)
                        == count_at(source_counts@, key) + count_at(*group_changes, key) by {
                        verus_prooflib::lemma_ordered_selected_weight(group_changes, key,
                            |row: (KeyView, Cell)| row.1.count as int, 1int,
                            |row: (KeyView, Cell)| if row.0 == key { row.1.count as int } else { 0int });
                    }
                }
                return Ok((counts, ranks));
            },
        };
        proof {
            assert(remaining@.contains((key.view(), *cell)));
            assert((key.view(), *cell) == group_changes.entries()[processed@]);
            lemma_cells_tail(remaining@);
            assert(group_changes.at(group_changes.entries()[processed@].0) == Some(group_changes.entries()[processed@].1));
            assert(cell.group.key() == key.view());
            assert forall|query: KeyView| query == key.view() implies
                #[trigger] count_at(counts, query) == count_at(source_counts@, query) by {
                assert forall|position: int| 0 <= position < group_changes.entries().take(processed@).len() implies
                    (|row: (KeyView, Cell)| if row.0 == query { row.1.count as int } else { 0int })(#[trigger] group_changes.entries().take(processed@)[position]) == 0 by {
                    assert(group_changes.entries().take(processed@)[position] == group_changes.entries()[position]);
                    assert(group_changes.entries()[position].0 != group_changes.entries()[processed@].0);
                }
                lemma_sum_zero(group_changes.entries().take(processed@),
                    |row: (KeyView, Cell)| if row.0 == query { row.1.count as int } else { 0int });
            }
        }
        if !update_total(&mut counts, &mut ranks, cell.group.clone(), cell.count) {
            proof {
                if fits {
                    assert(count_at(*group_changes, key.view()) == cell.count as int);
                    assert(cell.group.key() == group_key((*cell.group.brand)@, (*cell.group.kind)@, cell.group.size as int));
                }
            }
            return Err(4);
        }
        proof {
            assert forall|query: KeyView| #[trigger] count_at(counts, query)
                == count_at(source_counts@, query) + sum(group_changes.entries().take(processed@ + 1),
                    |row: (KeyView, Cell)| if row.0 == query { row.1.count as int } else { 0int }) by {
                lemma_sum_take(group_changes.entries(), processed@,
                    |row: (KeyView, Cell)| if row.0 == query { row.1.count as int } else { 0int });
            }
        }
        processed = Ghost(processed@ + 1);
    }
}

#[verifier::rlimit(100)]
pub fn rewrite_links(links: &mut Edges, source: &Edges, suppliers: &Index<i128>, keep: bool)
    requires edges_keys_valid(*old(links)), edges_keys_valid(*source), key_set(*suppliers),
    ensures edges_keys_valid(*final(links)),
        forall|part: int, supplier: int| #[trigger] edge_at(*final(links), part, supplier)
            == if suppliers.at((supplier, Seq::<u8>::empty())).is_some() && edge_entry(*source, supplier, part) {
                if keep { edge_at(*source, supplier, part) } else { 0int }
            } else { edge_at(*old(links), part, supplier) },
{
    let initial = Ghost(*links);
    proof { suppliers.model(); }
    let mut cursor = suppliers.cursor();
    let mut processed: Ghost<int> = Ghost(0);
    loop
        invariant edges_keys_valid(*links), edges_keys_valid(*source), key_set(*suppliers), initial@ == *old(links),
            0 <= processed@ <= suppliers.entries().len(), cursor.remaining() == suppliers.entries().skip(processed@),
            forall|position: int| 0 <= position < suppliers.entries().len() ==>
                #[trigger] suppliers.at(suppliers.entries()[position].0) == Some(suppliers.entries()[position].1),
            forall|part: int, supplier: int| #[trigger] edge_at(*links, part, supplier)
                == if key_seen(suppliers.entries(), processed@, (supplier, Seq::<u8>::empty())) && edge_entry(*source, supplier, part) {
                    if keep { edge_at(*source, supplier, part) } else { 0int }
                } else { edge_at(initial@, part, supplier) },
        decreases cursor.remaining().len(),
    {
        let (key, _) = match cursor.next() {
            Some(row) => row,
            None => {
                proof {
                    suppliers.model();
                    assert(processed@ == suppliers.entries().len());
                    assert forall|part: int, supplier: int| #[trigger] edge_at(*links, part, supplier)
                        == if suppliers.at((supplier, Seq::<u8>::empty())).is_some() && edge_entry(*source, supplier, part) {
                            if keep { edge_at(*source, supplier, part) } else { 0int }
                        } else { edge_at(initial@, part, supplier) } by {
                        if key_seen(suppliers.entries(), processed@, (supplier, Seq::<u8>::empty())) {
                            let position = choose|position: int| 0 <= position < processed@ && (#[trigger] suppliers.entries()[position]).0 == (supplier, Seq::<u8>::empty());
                            assert(suppliers.at(suppliers.entries()[position].0) == Some(suppliers.entries()[position].1));
                        }
                        if suppliers.at((supplier, Seq::<u8>::empty())).is_some() {
                            let position = choose|position: int| #![auto] 0 <= position < suppliers.entries().len() && suppliers.entries()[position].0 == (supplier, Seq::<u8>::empty());
                            assert(key_seen(suppliers.entries(), processed@, (supplier, Seq::<u8>::empty())));
                        }
                    }
                }
                return;
            },
        };
        proof {
            assert(key.view() == suppliers.entries()[processed@].0);
            assert(suppliers.at(key.view()).is_some());
            assert(key.view() == (key.number as int, Seq::<u8>::empty()));
        }
        let before = Ghost(*links);
        match source.get(&key) {
            Some(inner) => {
                proof { assert(source.at(key.view()) == Some(*inner)); assert(integer_keys(*inner)); }
                rewrite_column(links, key.number, &*inner, keep);
                proof {
                    assert forall|part: int, supplier: int| #[trigger] edge_at(*links, part, supplier)
                        == if key_seen(suppliers.entries(), processed@ + 1, (supplier, Seq::<u8>::empty())) && edge_entry(*source, supplier, part) {
                            if keep { edge_at(*source, supplier, part) } else { 0int }
                        } else { edge_at(initial@, part, supplier) } by {
                        lemma_key_seen_step(suppliers.entries(), processed@, (supplier, Seq::<u8>::empty()));
                        if supplier == key.number as int {
                            assert(suppliers.entries()[processed@].0 == (supplier, Seq::<u8>::empty()));
                            assert(edge_entry(*source, supplier, part) == inner.at((part, Seq::<u8>::empty())).is_some());
                            assert(edge_at(*source, supplier, part) == integer_at(*inner, (part, Seq::<u8>::empty())));
                        }
                    }
                }
            },
            None => {
                proof {
                    assert(source.at(key.view()).is_none());
                    assert forall|part: int, supplier: int| #[trigger] edge_at(*links, part, supplier)
                        == if key_seen(suppliers.entries(), processed@ + 1, (supplier, Seq::<u8>::empty())) && edge_entry(*source, supplier, part) {
                            if keep { edge_at(*source, supplier, part) } else { 0int }
                        } else { edge_at(initial@, part, supplier) } by {
                        lemma_key_seen_step(suppliers.entries(), processed@, (supplier, Seq::<u8>::empty()));
                        if supplier == key.number as int { assert(!edge_entry(*source, supplier, part)); }
                    }
                }
            },
        }
        processed = Ghost(processed@ + 1);
    }
}

pub open spec fn part_unchanged(db: Db, db2: Db, changed: Index<i128>) -> bool {
    forall|part: int, key: KeyView| changed.at((part, Seq::<u8>::empty())).is_none() ==>
        #[trigger] part_database_sum(db2, part, key) == part_database_sum(db, part, key)
}

pub proof fn lemma_no_entry_no_edge(edges: Edges, supplier: int, part: int)
    requires !edge_entry(edges, supplier, part),
    ensures edge_at(edges, supplier, part) == 0,
{
}

#[verifier::rlimit(400)]
pub fn maintain(counts: &Counts, ranks: &Ranks, parts: &Parts, edges: &Edges, blocked: &Index<i128>, links: &Edges,
    suppliers: &Index<i128>, changed: &Index<i128>,
    edge_rows: &[EdgeInput], block_rows: &[BlockerInput], part_rows: &Vec<PartInput>,
    Ghost(db): Ghost<Db>, Ghost(db2): Ghost<Db>, Ghost(supplier_keys): Ghost<Seq<i64>>, Ghost(part_keys): Ghost<Seq<i64>>,
) -> (out: Result<(Counts, Ranks, Parts, Edges, Index<i128>, Edges), u32>)
    requires counts_valid(*counts, db), ranks_valid(*ranks, *counts), tables_valid(*parts, *edges, *blocked, *links, db),
        key_set(*suppliers), key_set(*changed),
        rescan_edges_for_suppliers_ok(db2, supplier_keys, edge_rows@),
        rescan_blockers_for_suppliers_ok(db2, supplier_keys, block_rows@),
        rescan_parts_for_neighbors_ok(db2, part_keys, part_rows@),
        forall|key: int| seq_view_i64(supplier_keys).contains(key) <==> suppliers.at((key, Seq::<u8>::empty())).is_some(),
        forall|key: int| seq_view_i64(part_keys).contains(key) <==> changed.at((key, Seq::<u8>::empty())).is_some(),
        unaffected(db, db2, *suppliers), part_unchanged(db, db2, *changed),
    ensures out matches Ok(state) ==> counts_valid(state.0, db2) && ranks_valid(state.1, state.0)
            && tables_valid(state.2, state.3, state.4, state.5, db2),
        out is Err ==> !within(db, db2),
{
    hide(counts_valid);
    hide(ranks_valid);
    let fits = Ghost(within(db, db2));
    proof {
        reveal(counts_valid);
        reveal(ranks_valid);
        lemma_rescan_parts_len(db2, part_keys, part_rows@);
        lemma_rescan_edges_len(db2, supplier_keys, edge_rows@);
        lemma_rescan_blockers_len(db2, supplier_keys, block_rows@);
        assert forall|part: int, key: KeyView| #[trigger] part_at(*parts, part, key) <= db.part.len() by { lemma_part_sum_bound(db, part, key); }
        assert forall|supplier: int, part: int| #[trigger] edge_at(*edges, supplier, part) <= db.partsupp.len() by { lemma_edge_count_bound(db, part, supplier); }
        assert forall|supplier: int| #[trigger] integer_at(*blocked, (supplier, Seq::<u8>::empty())) <= db.supplier.len() by { lemma_blocker_count_bound(db, supplier); }
    }
    let new_parts = match replace_parts(parts, changed, part_rows, fits) { Some(value) => value, None => return Err(4) };
    let new_edges = match replace_edges(edges, suppliers, edge_rows, fits) { Some(value) => value, None => return Err(4) };
    let new_blocked = match replace_blockers(blocked, suppliers, block_rows, fits) { Some(value) => value, None => return Err(4) };
    proof {
        assert forall|part: int, key: KeyView| #[trigger] part_at(new_parts, part, key) == part_database_sum(db2, part, key) by {
            if changed.at((part, Seq::<u8>::empty())).is_some() {
                assert(seq_view_i64(part_keys).contains(part));
                lemma_parts_rescan(db2, part_keys, part_rows@, part, key);
            } else {
                assert(!seq_view_i64(part_keys).contains(part));
                lemma_parts_rescan_absent(db2, part_keys, part_rows@, part, key);
            }
        }
        assert forall|supplier: int, part: int| #[trigger] edge_at(new_edges, supplier, part) == edge_count(db2, part, supplier) by {
            if suppliers.at((supplier, Seq::<u8>::empty())).is_some() {
                assert(seq_view_i64(supplier_keys).contains(supplier));
                lemma_edges_supplier_rescan(db2, supplier_keys, edge_rows@, supplier, part);
            } else {
                assert(!seq_view_i64(supplier_keys).contains(supplier));
                lemma_edges_supplier_rescan_absent(db2, supplier_keys, edge_rows@, supplier, part);
            }
        }
        assert forall|supplier: int| #[trigger] integer_at(new_blocked, (supplier, Seq::<u8>::empty())) == blocker_count(db2, supplier) by {
            if suppliers.at((supplier, Seq::<u8>::empty())).is_some() {
                assert(seq_view_i64(supplier_keys).contains(supplier));
                lemma_blockers_rescan(db2, supplier_keys, block_rows@, supplier);
            } else {
                assert(!seq_view_i64(supplier_keys).contains(supplier));
                lemma_blockers_rescan_absent(db2, supplier_keys, block_rows@, supplier);
            }
        }
    }
    let mut new_links = share_edges(links);
    rewrite_links(&mut new_links, edges, suppliers, false);
    let cleared = Ghost(new_links);
    rewrite_links(&mut new_links, &new_edges, suppliers, true);
    proof {
        assert forall|part: int, supplier: int| #[trigger] edge_at(new_links, part, supplier) == edge_count(db2, part, supplier) by {
            assert(edge_at(*links, part, supplier) == edge_count(db, part, supplier));
            assert(edge_at(*edges, supplier, part) == edge_count(db, part, supplier));
            assert(edge_at(new_edges, supplier, part) == edge_count(db2, part, supplier));
            if suppliers.at((supplier, Seq::<u8>::empty())).is_some() {
                if !edge_entry(*edges, supplier, part) { lemma_no_entry_no_edge(*edges, supplier, part); }
                if !edge_entry(new_edges, supplier, part) { lemma_no_entry_no_edge(new_edges, supplier, part); }
                assert(edge_at(cleared@, part, supplier) == 0);
            } else {
                assert(edge_at(cleared@, part, supplier) == edge_at(*links, part, supplier));
                assert(edge_count(db2, part, supplier) == edge_count(db, part, supplier));
            }
        }
    }
    let changes = match collect_changes(parts, edges, blocked, &new_parts, &new_edges, &new_blocked, suppliers, Ghost(db), Ghost(db2)) {
        Ok(value) => value,
        Err(code) => return Err(code),
    };
    proof {
        lemma_changes_are_totals(db, db2, *suppliers, *parts, *edges, *blocked, new_parts, new_edges, new_blocked, changes);
        lemma_ranks_positive(*ranks, *counts);
        assert forall|brand: Seq<u8>, kind: Seq<u8>, size: int| fits@ implies
            0 <= count_at(*counts, #[trigger] group_key(brand, kind, size)) + count_at(changes, group_key(brand, kind, size)) <= i64::MAX as int by {
            if fits@ {
                lemma_total_bound(db2, brand, kind, size);
                lemma_suppliers_in_type(db2);
            }
        }
    }
    let state = match publish_changes(counts, ranks, &changes, fits) {
        Ok(value) => value,
        Err(code) => return Err(code),
    };
    proof {
        lemma_apply_correct(db, db2, *counts, state.0, changes, state.1);
    }
    Ok((state.0, state.1, new_parts, new_edges, new_blocked, new_links))
}

}
