use vstd::prelude::*;
use std::sync::Arc;
use verus_common::sums::*;
use verus_prooflib::ordered::{Index, Key, KeyView};
use crate::gen_obligations::*;
use crate::gen_spec::*;
use crate::model::*;
use crate::counts::*;
use crate::input_proofs::*;

verus! {

pub open spec fn key_seen(entries: Seq<(KeyView, i64)>, count: int, query: KeyView) -> bool {
    exists|position: int| 0 <= position < count && (#[trigger] entries[position]).0 == query
}

pub proof fn lemma_key_seen_step(entries: Seq<(KeyView, i64)>, count: int, query: KeyView)
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
pub fn remove_keys<Value>(map: &mut Index<Value>, keys: &Index<i64>)
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

pub fn replace_parts(parts: &Parts, changed: &Index<i64>, rows: &Vec<PartInput>, Ghost(fits): Ghost<bool>) -> (out: Option<Parts>)
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
        assert forall|key: KeyView| #[trigger] out.at(key).is_some() implies key.1 == Seq::<u8>::empty() && cell_keys_valid(out.at(key).unwrap()) by {
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
                    assert(max_rows() == 1099511627776int);
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

pub fn replace_edges(edges: &Edges, suppliers: &Index<i64>, rows: &[EdgeInput], Ghost(fits): Ghost<bool>) -> (out: Option<Edges>)
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
                    assert(max_rows() == 1099511627776int);
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

pub fn replace_blockers(blocked: &Index<i64>, suppliers: &Index<i64>, rows: &[BlockerInput], Ghost(fits): Ghost<bool>) -> (out: Option<Index<i64>>)
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
                    assert(max_rows() == 1099511627776int);
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

}
