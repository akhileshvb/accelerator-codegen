use vstd::prelude::*;
use std::sync::Arc;
use verus_common::order::*;
use verus_common::bytes::*;
use verus_prooflib::ordered::{Index, Key, KeyView, key_lt};
use crate::gen_obligations::*;
use crate::model::*;
use crate::counts::*;

verus! {

pub open spec fn increasing(keys: Seq<i64>) -> bool {
    forall|first: int, last: int| 0 <= first < last < keys.len() ==> keys[first] < keys[last]
}

pub open spec fn listed(keys: Seq<i64>, pred: spec_fn(int) -> bool) -> bool {
    forall|key: int| seq_view_i64(keys).contains(key) <==> pred(key)
}

/// The strictly increasing key list of a predicate: the keys a rescan asks for.
pub open spec fn wanted_keys(pred: spec_fn(int) -> bool) -> Seq<i64> {
    choose|keys: Seq<i64>| increasing(keys) && listed(keys, pred)
}

pub proof fn lemma_view_contains(keys: Seq<i64>, key: i64)
    ensures seq_view_i64(keys).contains(key as int) <==> keys.contains(key),
{
    if keys.contains(key) {
        let position = choose|position: int| 0 <= position < keys.len() && keys[position] == key;
        assert(seq_view_i64(keys)[position] == key as int);
    }
    if seq_view_i64(keys).contains(key as int) {
        let position = choose|position: int| 0 <= position < keys.len() && seq_view_i64(keys)[position] == key as int;
        assert(keys[position] == key);
    }
}

pub proof fn lemma_wanted_keys(keys: Seq<i64>, pred: spec_fn(int) -> bool)
    requires increasing(keys), listed(keys, pred),
    ensures wanted_keys(pred) == keys, listed(keys, pred),
{
    let chosen = wanted_keys(pred);
    assert(increasing(chosen) && listed(chosen, pred));
    let less = |left: i64, right: i64| left < right;
    assert(strict_order(less));
    assert forall|first: int, last: int| 0 <= first < last < chosen.len() implies less(#[trigger] chosen[first], #[trigger] chosen[last]) by {}
    assert forall|first: int, last: int| 0 <= first < last < keys.len() implies less(#[trigger] keys[first], #[trigger] keys[last]) by {}
    assert forall|key: i64| chosen.contains(key) <==> keys.contains(key) by {
        lemma_view_contains(chosen, key);
        lemma_view_contains(keys, key);
        assert(seq_view_i64(chosen).contains(key as int) == pred(key as int));
        assert(seq_view_i64(keys).contains(key as int) == pred(key as int));
    }
    lemma_sorted_unique(chosen, keys, less);
}

pub open spec fn edge_seen(edges: Seq<EdgeDelta>, count: int, key: int) -> bool {
    exists|i: int| 0 <= i < count && (#[trigger] edges[i]).row.ps_suppkey as int == key
}

pub open spec fn supplier_seen(suppliers: Seq<SupplierDelta>, count: int, key: int) -> bool {
    exists|i: int| 0 <= i < count && (#[trigger] suppliers[i]).s_suppkey as int == key
}

pub open spec fn part_seen(parts: Seq<PartDelta>, count: int, key: int) -> bool {
    exists|i: int| 0 <= i < count && (#[trigger] parts[i]).row.p_partkey as int == key
}

pub open spec fn linked_seen(links: Edges, parts: Seq<PartDelta>, count: int, key: int) -> bool {
    exists|i: int| 0 <= i < count && edge_at(links, (#[trigger] parts[i]).row.p_partkey as int, key) != 0
}

pub open spec fn entry_seen(entries: Seq<(KeyView, i64)>, count: int, key: int) -> bool {
    exists|position: int| 0 <= position < count && (#[trigger] entries[position]).0 == (key, Seq::<u8>::empty())
        && entries[position].1 != 0
}

pub proof fn lemma_edge_seen_step(edges: Seq<EdgeDelta>, count: int, key: int)
    requires 0 <= count < edges.len(),
    ensures edge_seen(edges, count + 1, key) == (edge_seen(edges, count, key) || edges[count].row.ps_suppkey as int == key),
{
    if edge_seen(edges, count + 1, key) && !edge_seen(edges, count, key) {
        let i = choose|i: int| 0 <= i < count + 1 && (#[trigger] edges[i]).row.ps_suppkey as int == key;
        assert(i == count);
    }
    if edge_seen(edges, count, key) {
        let i = choose|i: int| 0 <= i < count && (#[trigger] edges[i]).row.ps_suppkey as int == key;
        assert(0 <= i < count + 1 && edges[i].row.ps_suppkey as int == key);
    }
    if edges[count].row.ps_suppkey as int == key { assert(edge_seen(edges, count + 1, key)); }
}

pub proof fn lemma_supplier_seen_step(suppliers: Seq<SupplierDelta>, count: int, key: int)
    requires 0 <= count < suppliers.len(),
    ensures supplier_seen(suppliers, count + 1, key) == (supplier_seen(suppliers, count, key) || suppliers[count].s_suppkey as int == key),
{
    if supplier_seen(suppliers, count + 1, key) && !supplier_seen(suppliers, count, key) {
        let i = choose|i: int| 0 <= i < count + 1 && (#[trigger] suppliers[i]).s_suppkey as int == key;
        assert(i == count);
    }
    if supplier_seen(suppliers, count, key) {
        let i = choose|i: int| 0 <= i < count && (#[trigger] suppliers[i]).s_suppkey as int == key;
        assert(0 <= i < count + 1 && suppliers[i].s_suppkey as int == key);
    }
    if suppliers[count].s_suppkey as int == key { assert(supplier_seen(suppliers, count + 1, key)); }
}

pub proof fn lemma_part_seen_step(parts: Seq<PartDelta>, count: int, key: int)
    requires 0 <= count < parts.len(),
    ensures part_seen(parts, count + 1, key) == (part_seen(parts, count, key) || parts[count].row.p_partkey as int == key),
{
    if part_seen(parts, count + 1, key) && !part_seen(parts, count, key) {
        let i = choose|i: int| 0 <= i < count + 1 && (#[trigger] parts[i]).row.p_partkey as int == key;
        assert(i == count);
    }
    if part_seen(parts, count, key) {
        let i = choose|i: int| 0 <= i < count && (#[trigger] parts[i]).row.p_partkey as int == key;
        assert(0 <= i < count + 1 && parts[i].row.p_partkey as int == key);
    }
    if parts[count].row.p_partkey as int == key { assert(part_seen(parts, count + 1, key)); }
}

pub proof fn lemma_linked_seen_step(links: Edges, parts: Seq<PartDelta>, count: int, key: int)
    requires 0 <= count < parts.len(),
    ensures linked_seen(links, parts, count + 1, key)
        == (linked_seen(links, parts, count, key) || edge_at(links, parts[count].row.p_partkey as int, key) != 0),
{
    if linked_seen(links, parts, count + 1, key) && !linked_seen(links, parts, count, key) {
        let i = choose|i: int| 0 <= i < count + 1 && edge_at(links, (#[trigger] parts[i]).row.p_partkey as int, key) != 0;
        assert(i == count);
    }
    if linked_seen(links, parts, count, key) {
        let i = choose|i: int| 0 <= i < count && edge_at(links, (#[trigger] parts[i]).row.p_partkey as int, key) != 0;
        assert(0 <= i < count + 1 && edge_at(links, parts[i].row.p_partkey as int, key) != 0);
    }
    if edge_at(links, parts[count].row.p_partkey as int, key) != 0 { assert(linked_seen(links, parts, count + 1, key)); }
}

pub proof fn lemma_entry_seen_step(entries: Seq<(KeyView, i64)>, count: int, key: int)
    requires 0 <= count < entries.len(),
    ensures entry_seen(entries, count + 1, key)
        == (entry_seen(entries, count, key) || (entries[count].0 == (key, Seq::<u8>::empty()) && entries[count].1 != 0)),
{
    if entry_seen(entries, count + 1, key) && !entry_seen(entries, count, key) {
        let i = choose|i: int| 0 <= i < count + 1 && (#[trigger] entries[i]).0 == (key, Seq::<u8>::empty()) && entries[i].1 != 0;
        assert(i == count);
    }
    if entry_seen(entries, count, key) {
        let i = choose|i: int| 0 <= i < count && (#[trigger] entries[i]).0 == (key, Seq::<u8>::empty()) && entries[i].1 != 0;
        assert(0 <= i < count + 1 && entries[i].0 == (key, Seq::<u8>::empty()) && entries[i].1 != 0);
    }
    if entries[count].0 == (key, Seq::<u8>::empty()) && entries[count].1 != 0 { assert(entry_seen(entries, count + 1, key)); }
}

pub open spec fn supplier_wanted(links: Edges, parts: Seq<PartDelta>, edges: Seq<EdgeDelta>, suppliers: Seq<SupplierDelta>, key: int) -> bool {
    edge_seen(edges, edges.len() as int, key) || supplier_seen(suppliers, suppliers.len() as int, key)
        || linked_seen(links, parts, parts.len() as int, key)
}

pub open spec fn part_wanted(parts: Seq<PartDelta>, key: int) -> bool {
    part_seen(parts, parts.len() as int, key)
}

pub open spec fn supplier_pred(links: Edges, parts: Seq<PartDelta>, edges: Seq<EdgeDelta>, suppliers: Seq<SupplierDelta>) -> spec_fn(int) -> bool {
    |key: int| supplier_wanted(links, parts, edges, suppliers, key)
}

pub open spec fn part_pred(parts: Seq<PartDelta>) -> spec_fn(int) -> bool {
    |key: int| part_wanted(parts, key)
}

pub fn supplier_keys_wanted(links: &Edges, parts: &Vec<PartDelta>, edges: &[EdgeDelta], suppliers: &[SupplierDelta]) -> (out: Vec<i64>)
    requires edges_keys_valid(*links),
    ensures out@ == wanted_keys(supplier_pred(*links, parts@, edges@, suppliers@)),
{
    let set = supplier_set(links, parts, edges, suppliers);
    let out = keys_sorted(&set);
    proof {
        assert forall|key: int| seq_view_i64(out@).contains(key) <==> supplier_pred(*links, parts@, edges@, suppliers@)(key) by {
            assert(set.at((key, Seq::<u8>::empty())).is_some() == (integer_at(set, (key, Seq::<u8>::empty())) > 0));
        }
        lemma_wanted_keys(out@, supplier_pred(*links, parts@, edges@, suppliers@));
    }
    out
}

pub fn part_keys_wanted(parts: &Vec<PartDelta>) -> (out: Vec<i64>)
    ensures out@ == wanted_keys(part_pred(parts@)),
{
    let set = part_set(parts);
    let out = keys_sorted(&set);
    proof {
        assert forall|key: int| seq_view_i64(out@).contains(key) <==> part_pred(parts@)(key) by {
            assert(set.at((key, Seq::<u8>::empty())).is_some() == (integer_at(set, (key, Seq::<u8>::empty())) > 0));
        }
        lemma_wanted_keys(out@, part_pred(parts@));
    }
    out
}

pub fn add_edge_suppliers(keys: &mut Index<i64>, edges: &[EdgeDelta])
    requires key_set(*old(keys)),
    ensures key_set(*final(keys)),
        forall|key: int| (#[trigger] integer_at(*final(keys), (key, Seq::empty())) > 0) == (integer_at(*old(keys), (key, Seq::empty())) > 0
            || edge_seen(edges@, edges@.len() as int, key)),
{
    let initial = Ghost(*keys);
    let mut index = 0;
    while index < edges.len()
        invariant index <= edges.len(), key_set(*keys), initial@ == *old(keys),
            forall|key: int| (#[trigger] integer_at(*keys, (key, Seq::empty())) > 0) == (integer_at(initial@, (key, Seq::empty())) > 0
                || edge_seen(edges@, index as int, key)),
        decreases edges.len() - index,
    {
        insert_key(keys, edges[index].row.ps_suppkey);
        proof {
            assert forall|key: int| (#[trigger] integer_at(*keys, (key, Seq::empty())) > 0) == (integer_at(initial@, (key, Seq::empty())) > 0
                || edge_seen(edges@, index as int + 1, key)) by {
                lemma_edge_seen_step(edges@, index as int, key);
            }
        }
        index += 1;
    }
    proof {
        assert(index == edges.len());
        assert forall|key: int| (#[trigger] integer_at(*keys, (key, Seq::empty())) > 0) == (integer_at(*old(keys), (key, Seq::empty())) > 0
            || edge_seen(edges@, edges@.len() as int, key)) by {
            assert(edge_seen(edges@, index as int, key) == edge_seen(edges@, edges@.len() as int, key));
        }
    }
}

pub fn add_blocker_suppliers(keys: &mut Index<i64>, suppliers: &[SupplierDelta])
    requires key_set(*old(keys)),
    ensures key_set(*final(keys)),
        forall|key: int| (#[trigger] integer_at(*final(keys), (key, Seq::empty())) > 0) == (integer_at(*old(keys), (key, Seq::empty())) > 0
            || supplier_seen(suppliers@, suppliers@.len() as int, key)),
{
    let initial = Ghost(*keys);
    let mut index = 0;
    while index < suppliers.len()
        invariant index <= suppliers.len(), key_set(*keys), initial@ == *old(keys),
            forall|key: int| (#[trigger] integer_at(*keys, (key, Seq::empty())) > 0) == (integer_at(initial@, (key, Seq::empty())) > 0
                || supplier_seen(suppliers@, index as int, key)),
        decreases suppliers.len() - index,
    {
        insert_key(keys, suppliers[index].s_suppkey);
        proof {
            assert forall|key: int| (#[trigger] integer_at(*keys, (key, Seq::empty())) > 0) == (integer_at(initial@, (key, Seq::empty())) > 0
                || supplier_seen(suppliers@, index as int + 1, key)) by {
                lemma_supplier_seen_step(suppliers@, index as int, key);
            }
        }
        index += 1;
    }
    proof {
        assert(index == suppliers.len());
        assert forall|key: int| (#[trigger] integer_at(*keys, (key, Seq::empty())) > 0) == (integer_at(*old(keys), (key, Seq::empty())) > 0
            || supplier_seen(suppliers@, suppliers@.len() as int, key)) by {
            assert(supplier_seen(suppliers@, index as int, key) == supplier_seen(suppliers@, suppliers@.len() as int, key));
        }
    }
}

pub fn add_linked(keys: &mut Index<i64>, links: &Edges, part: i64)
    requires key_set(*old(keys)), edges_keys_valid(*links),
    ensures key_set(*final(keys)),
        forall|key: int| (#[trigger] integer_at(*final(keys), (key, Seq::empty())) > 0) == (integer_at(*old(keys), (key, Seq::empty())) > 0
            || edge_at(*links, part as int, key) != 0),
{
    let initial = Ghost(*keys);
    let inner = match links.get(&Key::number(part)) {
        Some(value) => value,
        None => {
            proof {
                assert(links.at((part as int, Seq::<u8>::empty())).is_none());
                assert forall|key: int| edge_at(*links, part as int, key) == 0 by {}
            }
            return;
        },
    };
    proof {
        assert(links.at((part as int, Seq::<u8>::empty())) == Some(*inner));
        inner.model();
    }
    let mut cursor = inner.cursor();
    let mut processed: Ghost<int> = Ghost(0);
    loop
        invariant key_set(*keys), integer_keys(*inner), initial@ == *old(keys),
            links.at((part as int, Seq::<u8>::empty())) == Some(*inner),
            0 <= processed@ <= inner.entries().len(), cursor.remaining() == inner.entries().skip(processed@),
            forall|position: int| 0 <= position < inner.entries().len() ==>
                #[trigger] inner.at(inner.entries()[position].0) == Some(inner.entries()[position].1),
            forall|key: int| (#[trigger] integer_at(*keys, (key, Seq::empty())) > 0) == (integer_at(initial@, (key, Seq::empty())) > 0
                || entry_seen(inner.entries(), processed@, key)),
        decreases cursor.remaining().len(),
    {
        let (key, value) = match cursor.next() {
            Some(row) => row,
            None => {
                proof {
                    inner.model();
                    assert(processed@ == inner.entries().len());
                    assert forall|key: int| (#[trigger] integer_at(*keys, (key, Seq::empty())) > 0) == (integer_at(initial@, (key, Seq::empty())) > 0
                        || edge_at(*links, part as int, key) != 0) by {
                        assert(edge_at(*links, part as int, key) == integer_at(*inner, (key, Seq::<u8>::empty())));
                        if entry_seen(inner.entries(), processed@, key) {
                            let position = choose|position: int| 0 <= position < processed@ && (#[trigger] inner.entries()[position]).0 == (key, Seq::<u8>::empty())
                                && inner.entries()[position].1 != 0;
                            assert(inner.at(inner.entries()[position].0) == Some(inner.entries()[position].1));
                        }
                        if integer_at(*inner, (key, Seq::<u8>::empty())) != 0 {
                            assert(inner.at((key, Seq::<u8>::empty())).is_some());
                            let position = choose|position: int| #![auto] 0 <= position < inner.entries().len()
                                && inner.entries()[position].0 == (key, Seq::<u8>::empty());
                            assert(inner.at(inner.entries()[position].0) == Some(inner.entries()[position].1));
                            assert(entry_seen(inner.entries(), processed@, key));
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
        if *value != 0 { insert_key(keys, key.number); }
        proof {
            assert forall|query: int| (#[trigger] integer_at(*keys, (query, Seq::empty())) > 0) == (integer_at(initial@, (query, Seq::empty())) > 0
                || entry_seen(inner.entries(), processed@ + 1, query)) by {
                lemma_entry_seen_step(inner.entries(), processed@, query);
            }
        }
        processed = Ghost(processed@ + 1);
    }
}

pub fn supplier_set(links: &Edges, parts: &Vec<PartDelta>, edges: &[EdgeDelta], suppliers: &[SupplierDelta]) -> (out: Index<i64>)
    requires edges_keys_valid(*links),
    ensures key_set(out),
        forall|key: int| (#[trigger] integer_at(out, (key, Seq::empty())) > 0) == supplier_wanted(*links, parts@, edges@, suppliers@, key),
{
    let mut keys: Index<i64> = Index::new();
    add_edge_suppliers(&mut keys, edges);
    add_blocker_suppliers(&mut keys, suppliers);
    let initial = Ghost(keys);
    let mut index = 0;
    while index < parts.len()
        invariant index <= parts.len(), key_set(keys), edges_keys_valid(*links),
            forall|key: int| (#[trigger] integer_at(keys, (key, Seq::empty())) > 0) == ((integer_at(initial@, (key, Seq::empty())) > 0)
                || linked_seen(*links, parts@, index as int, key)),
        decreases parts.len() - index,
    {
        add_linked(&mut keys, links, parts[index].row.p_partkey);
        proof {
            assert forall|key: int| (#[trigger] integer_at(keys, (key, Seq::empty())) > 0) == ((integer_at(initial@, (key, Seq::empty())) > 0)
                || linked_seen(*links, parts@, index as int + 1, key)) by {
                lemma_linked_seen_step(*links, parts@, index as int, key);
            }
        }
        index += 1;
    }
    proof {
        assert(index == parts.len());
        assert forall|key: int| (#[trigger] integer_at(keys, (key, Seq::empty())) > 0) == supplier_wanted(*links, parts@, edges@, suppliers@, key) by {
            assert(linked_seen(*links, parts@, index as int, key) == linked_seen(*links, parts@, parts@.len() as int, key));
        }
    }
    keys
}

pub fn part_set(parts: &Vec<PartDelta>) -> (out: Index<i64>)
    ensures key_set(out),
        forall|key: int| (#[trigger] integer_at(out, (key, Seq::empty())) > 0) == part_wanted(parts@, key),
{
    let mut keys: Index<i64> = Index::new();
    let mut index = 0;
    while index < parts.len()
        invariant index <= parts.len(), key_set(keys),
            forall|key: int| (#[trigger] integer_at(keys, (key, Seq::empty())) > 0) == part_seen(parts@, index as int, key),
        decreases parts.len() - index,
    {
        insert_key(&mut keys, parts[index].row.p_partkey);
        proof {
            assert forall|key: int| (#[trigger] integer_at(keys, (key, Seq::empty())) > 0) == part_seen(parts@, index as int + 1, key) by {
                lemma_part_seen_step(parts@, index as int, key);
            }
        }
        index += 1;
    }
    proof {
        assert(index == parts.len());
        assert forall|key: int| (#[trigger] integer_at(keys, (key, Seq::empty())) > 0) == part_wanted(parts@, key) by {
            assert(part_seen(parts@, index as int, key) == part_seen(parts@, parts@.len() as int, key));
        }
    }
    keys
}

pub fn keys_sorted(keys: &Index<i64>) -> (out: Vec<i64>)
    requires integer_keys(*keys),
    ensures increasing(out@),
        forall|key: int| seq_view_i64(out@).contains(key) <==> keys.at((key, Seq::empty())).is_some(),
{
    let out = keys_vec(keys);
    proof {
        keys.model();
        let projected = keys.entries().map_values(|row: (KeyView, i64)| row.0.0);
        assert(seq_view_i64(out@) == projected);
        assert forall|first: int, last: int| 0 <= first < last < out@.len() implies out@[first] < out@[last] by {
            assert(projected.len() == keys.entries().len());
            assert(projected[first] == (|row: (KeyView, i64)| row.0.0)(keys.entries()[first]));
            assert(projected[last] == (|row: (KeyView, i64)| row.0.0)(keys.entries()[last]));
            assert(seq_view_i64(out@)[first] == out@[first] as int);
            assert(seq_view_i64(out@)[last] == out@[last] as int);
            assert(key_lt(keys.entries()[first].0, keys.entries()[last].0));
            assert(keys.at(keys.entries()[first].0).is_some());
            assert(keys.at(keys.entries()[last].0).is_some());
            assert(keys.entries()[first].0.1 == Seq::<u8>::empty());
            assert(keys.entries()[last].0.1 == Seq::<u8>::empty());
            lemma_bytes_lt_irrefl(Seq::<u8>::empty());
        }
    }
    out
}

}
