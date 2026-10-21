use vstd::prelude::*;
use std::sync::Arc;
use verus_prooflib::ordered::{Index, Key};
use crate::gen_obligations::*;
use crate::model::*;
use verus_common::sums::*;
use crate::encoding::*;
use crate::rank_proofs::*;
use crate::vocab::*;

verus! {

pub fn copy_bytes(bytes: &[u8]) -> (out: Vec<u8>)
    ensures out@ == bytes@,
{
    vstd::slice::slice_to_vec(bytes)
}

pub fn append_escaped(out: &mut Vec<u8>, bytes: &[u8])
    ensures final(out)@ == old(out)@ + escape(bytes@),
{
    let ghost before = out@;
    let mut index = 0;
    while index < bytes.len()
        invariant index <= bytes.len(),
            out@ == before + escape(bytes@.take(index as int)).drop_last(),
        decreases bytes.len() - index,
    {
        proof {
            lemma_escape_push(bytes@.take(index as int), bytes@[index as int]);
            assert(bytes@.take(index as int + 1) =~= bytes@.take(index as int).push(bytes@[index as int]));
        }
        out.push(1);
        out.push(bytes[index]);
        proof {
            assert(out@ =~= before + escape(bytes@.take(index as int + 1)).drop_last());
        }
        index += 1;
    }
    proof {
        assert(bytes@.take(index as int) =~= bytes@);
        lemma_escape_end(bytes@);
    }
    out.push(0);
    proof {
        assert(out@ == (before + escape(bytes@).drop_last()).push(0u8));
        assert(out@ =~= before + escape(bytes@));
    }
}

pub fn make_group(vocab: &Vocab, row: &PartInput) -> (group: Arc<Group>)
    requires vocab.ok(),
    ensures group.key() == group_key(row.p_brand@, row.p_type@, row.p_size as int),
        group.valid(),
{
    let mut text = Vec::new();
    append_escaped(&mut text, row.p_brand.as_slice());
    append_escaped(&mut text, row.p_type.as_slice());
    let identity = Key::composite(row.p_size, copy_bytes(text.as_slice()));
    append_size(&mut text, row.p_size);
    Arc::new(Group {
        brand: Arc::new(copy_bytes(row.p_brand.as_slice())),
        kind: Arc::new(copy_bytes(row.p_type.as_slice())),
        size: row.p_size,
        brand_id: brand_code_exec(vocab, row.p_brand.as_slice()),
        kind_id: type_code_exec(vocab, row.p_type.as_slice()),
        identity,
        order: Key::text(text),
    })
}

pub fn append_size(out: &mut Vec<u8>, size: i64)
    ensures final(out)@ == old(out)@ + size_bytes(size as int),
{
    let initial = Ghost(out@);
    let biased = (size as i128 - i64::MIN as i128) as u64;
    let mut index: usize = 0;
    while index < 8
        invariant index <= 8, out@ == initial@ + size_bytes(size as int).take(index as int),
            biased == (size as int - i64::MIN as int) as u64,
        decreases 8 - index,
    {
        out.push((biased >> (((7 - index) * 8) as u32)) as u8);
        proof { assert(out@ =~= initial@ + size_bytes(size as int).take(index as int + 1)); }
        index += 1;
    }
}

pub fn key_for(group: &Group) -> (key: Key)
    requires group.valid(),
    ensures key.view() == group.key(),
{
    group.identity.share()
}

pub fn share_counts(source: &Counts) -> (out: Counts)
    requires cell_keys_valid(*source),
    ensures cell_keys_valid(out), out.entries() == source.entries(),
        positive_counts(*source) ==> positive_counts(out),
        forall|key: verus_prooflib::ordered::KeyView| #[trigger] out.at(key) == source.at(key),
{
    let out = source.share();
    proof {
        verus_prooflib::lemma_ordered_equal(source, &out);
        assert forall|key: verus_prooflib::ordered::KeyView| #[trigger] out.at(key).is_some() implies
            out.at(key).unwrap().group.valid() && out.at(key).unwrap().group.key() == key && out.at(key).unwrap().consistent() by {
            assert(source.at(key) == out.at(key));
        }
        if positive_counts(*source) {
            assert forall|key: verus_prooflib::ordered::KeyView| #[trigger] out.at(key).is_some()
                implies out.at(key).unwrap().count > 0 by {
                assert(source.at(key).is_some());
            }
        }
    }
    out
}

pub fn share_parts(source: &Parts) -> (out: Parts)
    requires parts_keys_valid(*source),
    ensures parts_keys_valid(out),
        forall|key: verus_prooflib::ordered::KeyView| #[trigger] out.at(key) == source.at(key),
{
    let out = source.share();
    proof {
        verus_prooflib::lemma_ordered_equal(source, &out);
        assert forall|key: verus_prooflib::ordered::KeyView| #[trigger] out.at(key).is_some() implies
            key.1 == Seq::<u8>::empty() && cell_keys_valid(out.at(key).unwrap()) by {
            assert(source.at(key) == out.at(key));
        }
    }
    out
}

pub fn share_edges(source: &Edges) -> (out: Edges)
    requires edges_keys_valid(*source),
    ensures edges_keys_valid(out),
        forall|key: verus_prooflib::ordered::KeyView| #[trigger] out.at(key) == source.at(key),
{
    let out = source.share();
    proof {
        verus_prooflib::lemma_ordered_equal(source, &out);
        assert forall|key: verus_prooflib::ordered::KeyView| #[trigger] out.at(key).is_some() implies
            key.1 == Seq::<u8>::empty() && out.at(key).unwrap().wf() && integer_keys(out.at(key).unwrap()) by {
            assert(source.at(key) == out.at(key));
        }
    }
    out
}

pub fn share_integers(source: &Index<i64>) -> (out: Index<i64>)
    requires integer_keys(*source),
    ensures integer_keys(out),
        forall|key: verus_prooflib::ordered::KeyView| #[trigger] out.at(key) == source.at(key),
{
    let out = source.share();
    proof {
        verus_prooflib::lemma_ordered_equal(source, &out);
        assert forall|key: verus_prooflib::ordered::KeyView| #[trigger] out.at(key).is_some() implies
            key.1 == Seq::<u8>::empty() by { assert(source.at(key) == out.at(key)); }
    }
    out
}

pub fn add(left: i64, right: i64) -> (out: Option<i64>)
    ensures out matches Some(value) ==> value as int == left as int + right as int,
{
    let value = left as i128 + right as i128;
    if value < i64::MIN as i128 || value > i64::MAX as i128 { None }
    else { Some(value as i64) }
}

pub fn bump(map: &mut Index<i64>, key: Key, change: i64) -> (ok: bool)
    requires map.wf(),
    ensures final(map).wf(),
        integer_keys(*old(map)) && key.view().1 == Seq::<u8>::empty() ==> integer_keys(*final(map)),
        nonzero_integers(*old(map)) && key.view().1 == Seq::<u8>::empty() ==> nonzero_integers(*final(map)),
        ok ==> forall|query: verus_prooflib::ordered::KeyView|
        #[trigger] integer_at(*final(map), query) == integer_at(*old(map), query)
            + if query == key.view() { change as int } else { 0 },
{
    let initial = Ghost(*map);
    let changed_key = Ghost(key.view());
    let before = match map.get(&key) { Some(value) => *value, None => 0 };
    let after = match add(before, change) { Some(value) => value, None => return false };
    if after == 0 { map.remove(&key); }
    else { map.put(key, Arc::new(after)); }
    proof {
        if nonzero_integers(initial@) && changed_key@.1 == Seq::<u8>::empty() {
            assert forall|query: verus_prooflib::ordered::KeyView| #[trigger] map.at(query).is_some()
                implies map.at(query).unwrap() != 0 by {
                if query == changed_key@ {
                    assert(after != 0);
                } else {
                    assert(map.at(query) == initial@.at(query));
                    assert(initial@.at(query).is_some());
                }
            }
        }
    }
    true
}

pub fn bump_group(map: &mut Counts, group: Arc<Group>, change: i64) -> (ok: bool)
    requires group.valid(), cell_keys_valid(*map),
    ensures cell_keys_valid(*final(map)),
        positive_counts(*old(map)) && change >= 0 ==> positive_counts(*final(map)),
        ok ==> forall|query: verus_prooflib::ordered::KeyView|
        #[trigger] count_at(*final(map), query) == count_at(*old(map), query)
            + if query == group.key() { change as int } else { 0 },
{
    let key = key_for(&*group);
    let before = match map.get(&key) { Some(value) => value.count, None => 0 };
    let after = match add(before, change) { Some(value) => value, None => return false };
    if after == 0 { map.remove(&key); }
    else { map.put(key, Arc::new(Cell::new(group, after))); }
    true
}

pub fn bump_part(vocab: &Vocab, parts: &mut Parts, row: &PartInput, change: i64) -> (ok: bool)
    requires parts_keys_valid(*parts), vocab.ok(),
    ensures parts_keys_valid(*final(parts)),
        ok ==> forall|part: int, query: verus_prooflib::ordered::KeyView|
        #[trigger] part_at(*final(parts), part, query) == part_at(*old(parts), part, query)
            + part_weight(part_input_view(*row), part, query) * change as int,
{
    let initial = Ghost(*parts);
    let part_key = Key::number(row.p_partkey);
    let mut groups = match parts.get(&part_key) {
        Some(value) => {
            proof { assert(cell_keys_valid(*value)); }
            let copy = value.share();
            proof {
                verus_prooflib::lemma_ordered_equal(&*value, &copy);
                assert forall|key: verus_prooflib::ordered::KeyView| #[trigger] copy.at(key).is_some() implies
                    copy.at(key).unwrap().group.valid() && copy.at(key).unwrap().group.key() == key && copy.at(key).unwrap().consistent() by {
                    assert(value.at(key) == copy.at(key));
                }
                assert(cell_keys_valid(copy));
            }
            copy
        },
        None => Index::new(),
    };
    let before_groups = Ghost(groups);
    proof {
        assert(cell_keys_valid(groups));
        assert forall|query: verus_prooflib::ordered::KeyView| #[trigger] count_at(groups, query)
            == part_at(initial@, row.p_partkey as int, query) by {}
    }
    let group = make_group(vocab, row);
    if !bump_group(&mut groups, group, change) { return false; }
    let updated = Ghost(groups);
    proof { groups.map_model(); }
    if groups.is_empty() { parts.remove(&part_key); }
    else { parts.put(part_key, Arc::new(groups)); }
    proof {
        assert forall|query: verus_prooflib::ordered::KeyView| #[trigger] count_at(updated@, query)
            == count_at(before_groups@, query)
                + if query == group_key(row.p_brand@, row.p_type@, row.p_size as int) { change as int } else { 0int } by {}
        assert forall|part: int, query: verus_prooflib::ordered::KeyView|
            #[trigger] part_at(*parts, part, query) == part_at(initial@, part, query)
                + part_weight(part_input_view(*row), part, query) * change as int by {
            if part == row.p_partkey as int {
                assert(part_at(initial@, part, query) == count_at(before_groups@, query));
                if updated@.entries().len() == 0 {
                    assert(updated@.at(query).is_none());
                    assert(parts.at((part, Seq::<u8>::empty())).is_none());
                    assert(count_at(updated@, query) == 0);
                } else {
                    assert(parts.at((part, Seq::<u8>::empty())) == Some(updated@));
                    assert(part_at(*parts, part, query) == count_at(updated@, query));
                }
                assert(part_weight(part_input_view(*row), part, query)
                    == if query == group_key(row.p_brand@, row.p_type@, row.p_size as int) { 1int } else { 0int });
            } else {
                assert(parts.at((part, Seq::<u8>::empty())) == initial@.at((part, Seq::<u8>::empty())));
                assert(part_weight(part_input_view(*row), part, query) == 0);
            }
        }
    }
    true
}

#[verifier::rlimit(100)]
pub fn bump_edge(edges: &mut Edges, part: i64, supplier: i64, change: i64) -> (ok: bool)
    requires edges_keys_valid(*edges),
    ensures edges_keys_valid(*final(edges)),
        ok ==> forall|query_supplier: int, query_part: int|
        #[trigger] edge_at(*final(edges), query_supplier, query_part) == edge_at(*old(edges), query_supplier, query_part)
            + if query_supplier == supplier as int && query_part == part as int { change as int } else { 0 },
{
    let initial = Ghost(*edges);
    let supplier_key = Key::number(supplier);
    let mut parts = match edges.get(&supplier_key) {
        Some(value) => {
            proof { assert(integer_keys(*value)); }
            let copy = value.share();
            proof {
                verus_prooflib::lemma_ordered_equal(&*value, &copy);
                assert forall|key: verus_prooflib::ordered::KeyView| #[trigger] copy.at(key).is_some() implies
                    key.1 == Seq::<u8>::empty() by { assert(value.at(key) == copy.at(key)); }
            }
            copy
        },
        None => Index::new(),
    };
    let before_parts = Ghost(parts);
    proof {
        assert forall|query: int| #[trigger] integer_at(parts, (query, Seq::<u8>::empty()))
            == edge_at(initial@, supplier as int, query) by {}
    }
    let part_key = Key::number(part);
    if !bump(&mut parts, part_key, change) { return false; }
    let updated = Ghost(parts);
    proof { parts.map_model(); }
    if parts.is_empty() { edges.remove(&supplier_key); }
    else { edges.put(supplier_key, Arc::new(parts)); }
    proof {
        assert forall|query: int| #[trigger] integer_at(updated@, (query, Seq::<u8>::empty()))
            == integer_at(before_parts@, (query, Seq::<u8>::empty()))
                + if query == part as int { change as int } else { 0int } by {}
        assert forall|query_supplier: int, query_part: int|
            #[trigger] edge_at(*edges, query_supplier, query_part) == edge_at(initial@, query_supplier, query_part)
                + if query_supplier == supplier as int && query_part == part as int { change as int } else { 0 } by {
            if query_supplier == supplier as int {
                assert(edge_at(initial@, query_supplier, query_part) == integer_at(before_parts@, (query_part, Seq::<u8>::empty())));
                if updated@.entries().len() == 0 {
                    assert(updated@.at((query_part, Seq::empty())).is_none());
                    assert(edges.at((query_supplier, Seq::<u8>::empty())).is_none());
                } else {
                    assert(edges.at((query_supplier, Seq::<u8>::empty())) == Some(updated@));
                    assert(edge_at(*edges, query_supplier, query_part) == integer_at(updated@, (query_part, Seq::<u8>::empty())));
                }
                assert(edge_at(*edges, query_supplier, query_part) == integer_at(updated@, (query_part, Seq::<u8>::empty())));
                assert(integer_at(updated@, (query_part, Seq::<u8>::empty()))
                    == integer_at(before_parts@, (query_part, Seq::<u8>::empty()))
                        + if query_part == part as int { change as int } else { 0int });
            } else {
                assert(edges.at((query_supplier, Seq::<u8>::empty())) == initial@.at((query_supplier, Seq::<u8>::empty())));
            }
        }
    }
    true
}

pub fn insert_key(keys: &mut Index<i64>, key: i64)
    ensures integer_keys(*old(keys)) ==> integer_keys(*final(keys)),
        key_set(*old(keys)) ==> key_set(*final(keys)),
        forall|query: verus_prooflib::ordered::KeyView| #[trigger] integer_at(*final(keys), query)
            == if query == (key as int, Seq::<u8>::empty()) { 1int } else { integer_at(*old(keys), query) },
{
    keys.put(Key::number(key), Arc::new(1));
}

pub fn keys_from_slice(rows: &[i64]) -> (out: Index<i64>)
    ensures integer_keys(out), key_set(out),
        forall|key: int| #[trigger] integer_at(out, (key, Seq::<u8>::empty()))
            == if seq_view_i64(rows@).contains(key) { 1int } else { 0int },
{
    let mut keys = Index::new();
    let mut index = 0;
    let mut processed: Ghost<int> = Ghost(0);
    while index < rows.len()
        invariant index <= rows.len(), processed@ == index as int, integer_keys(keys), key_set(keys),
            forall|key: int| #[trigger] integer_at(keys, (key, Seq::empty()))
                == if seq_view_i64(rows@.take(processed@)).contains(key) { 1int } else { 0int },
        decreases rows.len() - index,
    {
        insert_key(&mut keys, rows[index]);
        proof {
            assert(rows@.take(processed@ + 1) =~= rows@.take(processed@).push(rows@[processed@]));
            assert forall|key: int| #[trigger] integer_at(keys, (key, Seq::<u8>::empty()))
                == if seq_view_i64(rows@.take(processed@ + 1)).contains(key) { 1int } else { 0int } by {
                let prefix = seq_view_i64(rows@.take(processed@));
                assert(seq_view_i64(rows@.take(processed@ + 1)) =~= prefix.push(rows@[processed@] as int));
                verus_prooflib::lemma_push_membership(prefix, rows@[processed@] as int);
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    proof { assert(rows@.take(processed@) =~= rows@); }
    keys
}

pub fn keys_vec(keys: &Index<i64>) -> (out: Vec<i64>)
    requires keys.wf(),
    ensures seq_view_i64(out@) == keys.entries().map_values(|row: (verus_prooflib::ordered::KeyView, i64)| row.0.0),
        integer_keys(*keys) ==> forall|key: int| #[trigger] seq_view_i64(out@).contains(key)
            <==> keys.at((key, Seq::empty())).is_some(),
{
    let mut out = Vec::new();
    proof { keys.model(); }
    let mut cursor = keys.cursor();
    let mut processed: Ghost<int> = Ghost(0);
    loop
        invariant keys.wf(), 0 <= processed@ <= keys.entries().len(),
            cursor.remaining() == keys.entries().skip(processed@),
            seq_view_i64(out@) == keys.entries().take(processed@).map_values(
                |row: (verus_prooflib::ordered::KeyView, i64)| row.0.0),
        decreases cursor.remaining().len(),
    {
        proof { keys.model(); }
        match cursor.next() {
            Some((key, value)) => {
                proof { assert((key.view(), *value) == keys.entries()[processed@]); }
                let previous = Ghost(out@);
                out.push(key.number);
                proof {
                    assert(seq_view_i64(out@) =~= seq_view_i64(previous@).push(key.number as int));
                    assert(keys.entries().take(processed@ + 1)
                        =~= keys.entries().take(processed@).push(keys.entries()[processed@]));
                    assert(seq_view_i64(out@) =~= keys.entries().take(processed@ + 1).map_values(
                        |row: (verus_prooflib::ordered::KeyView, i64)| row.0.0));
                }
                processed = Ghost(processed@ + 1);
            },
            None => {
                proof {
                    assert(processed@ == keys.entries().len());
                    assert(keys.entries().take(processed@) =~= keys.entries());
                    if integer_keys(*keys) {
                        assert forall|key: int| #[trigger] seq_view_i64(out@).contains(key)
                            <==> keys.at((key, Seq::empty())).is_some() by {
                            if seq_view_i64(out@).contains(key) {
                                let position = choose|position: int| #[trigger] seq_view_i64(out@)[position] == key
                                    && 0 <= position < seq_view_i64(out@).len();
                                assert(keys.at(keys.entries()[position].0).is_some());
                                assert(keys.entries()[position].0.1 == Seq::<u8>::empty());
                            }
                            if keys.at((key, Seq::empty())).is_some() {
                                let position = choose|position: int| #[trigger] keys.entries()[position].0 == (key, Seq::<u8>::empty())
                                    && 0 <= position < keys.entries().len();
                                assert(seq_view_i64(out@)[position] == key);
                            }
                        }
                    }
                }
                return out;
            },
        }
    }
}

pub fn covered(needed: &Index<i64>, supplied: &Index<i64>) -> (ok: bool)
    requires needed.wf(),
    ensures ok <==> forall|key: verus_prooflib::ordered::KeyView|
        #[trigger] needed.at(key).is_some() ==> supplied.at(key).is_some(),
{
    proof { needed.model(); }
    let mut cursor = needed.cursor();
    let mut processed: Ghost<int> = Ghost(0);
    loop
        invariant needed.wf(), 0 <= processed@ <= needed.entries().len(),
            cursor.remaining() == needed.entries().skip(processed@),
            forall|position: int| 0 <= position < processed@ ==>
                #[trigger] supplied.at(needed.entries()[position].0).is_some(),
        decreases cursor.remaining().len(),
    {
        proof { needed.model(); }
        match cursor.next() {
            Some((key, value)) => {
                proof {
                    assert((key.view(), *value) == needed.entries()[processed@]);
                    assert(needed.at(key.view()).is_some());
                }
                if supplied.get(&key).is_none() { return false; }
                processed = Ghost(processed@ + 1);
            },
            None => {
                proof {
                    assert(processed@ == needed.entries().len());
                    assert forall|key: verus_prooflib::ordered::KeyView|
                        #[trigger] needed.at(key).is_some() implies supplied.at(key).is_some() by {
                        let position = choose|position: int| #[trigger] needed.entries()[position].0 == key
                            && 0 <= position < needed.entries().len();
                        assert(supplied.at(needed.entries()[position].0).is_some());
                    }
                }
                return true;
            },
        }
    }
}

pub fn accumulate_counts(target: &mut Counts, source: &Counts, factor: i64) -> (ok: bool)
    requires cell_keys_valid(*target), cell_keys_valid(*source), factor >= 0,
    ensures cell_keys_valid(*final(target)),
        ok ==> forall|key: verus_prooflib::ordered::KeyView| #[trigger] count_at(*final(target), key)
            == count_at(*old(target), key) + count_at(*source, key) * factor as int,
{
    let initial = Ghost(*target);
    proof { source.model(); }
    let mut cursor = source.cursor();
    let mut processed: Ghost<int> = Ghost(0);
    loop
        invariant cell_keys_valid(*target), cell_keys_valid(*source), factor >= 0,
            initial@ == *old(target),
            0 <= processed@ <= source.entries().len(), cursor.remaining() == source.entries().skip(processed@),
            forall|position: int| 0 <= position < source.entries().len() ==>
                #[trigger] source.at(source.entries()[position].0) == Some(source.entries()[position].1),
            forall|key: verus_prooflib::ordered::KeyView| #[trigger] count_at(*target, key)
                == count_at(initial@, key) + sum(source.entries().take(processed@),
                    |row: (verus_prooflib::ordered::KeyView, Cell)| if row.0 == key { row.1.count as int * factor as int } else { 0int }),
        decreases cursor.remaining().len(),
    {
        let remaining = Ghost(cursor.remaining());
        let (key, cell) = match cursor.next() {
            Some(row) => row,
            None => {
                proof {
                    assert(remaining@.len() == 0);
                    assert(processed@ == source.entries().len());
                    assert(source.entries().take(processed@) =~= source.entries());
                    assert forall|key: verus_prooflib::ordered::KeyView| #[trigger] count_at(*target, key)
                        == count_at(initial@, key) + count_at(*source, key) * factor as int by {
                        verus_prooflib::lemma_ordered_selected_weight(source, key,
                            |row: (verus_prooflib::ordered::KeyView, Cell)| row.1.count as int, factor as int,
                            |row: (verus_prooflib::ordered::KeyView, Cell)| if row.0 == key { row.1.count as int * factor as int } else { 0int });
                    }
                }
                return true;
            },
        };
        if cell.count < 0 { return false; }
        proof {
            assert((key.view(), *cell) == source.entries()[processed@]);
            assert(source.at(source.entries()[processed@].0) == Some(source.entries()[processed@].1));
            assert(source.at(key.view()) == Some(*cell));
            assert(source.at(key.view()).is_some());
            assert(source.at(key.view()).unwrap().group.valid());
            verus_prooflib::lemma_i64_product(cell.count, factor);
        }
        let product = cell.count as i128 * factor as i128;
        proof {
            assert(product >= 0) by (nonlinear_arith)
                requires product as int == cell.count as int * factor as int, cell.count >= 0, factor >= 0;
        }
        if product > i64::MAX as i128 { return false; }
        if !bump_group(target, cell.group.clone(), product as i64) { return false; }
        proof {
            assert forall|query: verus_prooflib::ordered::KeyView| #[trigger] count_at(*target, query)
                == count_at(initial@, query) + sum(source.entries().take(processed@ + 1),
                    |row: (verus_prooflib::ordered::KeyView, Cell)| if row.0 == query { row.1.count as int * factor as int } else { 0int }) by {
                lemma_sum_take(source.entries(), processed@,
                    |row: (verus_prooflib::ordered::KeyView, Cell)| if row.0 == query { row.1.count as int * factor as int } else { 0int });
            }
        }
        processed = Ghost(processed@ + 1);
    }
}

pub fn join(parts: &Parts, edges: &Index<i64>) -> (out: Option<Counts>)
    requires parts_keys_valid(*parts), integer_keys(*edges),
    ensures out matches Some(groups) ==> cell_keys_valid(groups)
        && forall|key: verus_prooflib::ordered::KeyView|
            #[trigger] count_at(groups, key) == joined_count(*parts, *edges, key),
{
    let mut groups = Index::new();
    proof { edges.model(); }
    let mut cursor = edges.cursor();
    let mut processed: Ghost<int> = Ghost(0);
    loop
        invariant 0 <= processed@ <= edges.entries().len(), cell_keys_valid(groups),
            parts_keys_valid(*parts), integer_keys(*edges),
            forall|position: int| 0 <= position < edges.entries().len() ==>
                #[trigger] edges.at(edges.entries()[position].0) == Some(edges.entries()[position].1),
            cursor.remaining() == edges.entries().skip(processed@),
            forall|key: verus_prooflib::ordered::KeyView| #[trigger] count_at(groups, key)
                == sum(edges.entries().take(processed@), |row: (verus_prooflib::ordered::KeyView, i64)|
                    row.1 as int * part_at(*parts, row.0.0, key)),
        decreases cursor.remaining().len(),
    {
        let remaining = Ghost(cursor.remaining());
        let (part, multiplicity) = match cursor.next() {
            Some(row) => row,
            None => {
                proof {
                    assert(remaining@.len() == 0);
                    assert(processed@ == edges.entries().len());
                    assert(edges.entries().take(processed@) =~= edges.entries());
                }
                return Some(groups);
            },
        };
        if *multiplicity < 0 { return None; }
        let before = Ghost(groups);
        proof {
            assert((part.view(), *multiplicity) == edges.entries()[processed@]);
            assert(edges.at(edges.entries()[processed@].0) == Some(edges.entries()[processed@].1));
            assert(edges.at(part.view()) == Some(*multiplicity));
            assert(edges.at(part.view()).is_some());
            assert(part.view().1 == Seq::<u8>::empty());
        }
        if let Some(variants) = parts.get(&part) {
            if !accumulate_counts(&mut groups, &*variants, *multiplicity) { return None; }
            proof {
                assert forall|key: verus_prooflib::ordered::KeyView| #[trigger] count_at(groups, key)
                    == count_at(before@, key) + *multiplicity as int * part_at(*parts, part.number as int, key) by {
                    assert(part_at(*parts, part.number as int, key) == count_at(*variants, key));
                    assert(count_at(*variants, key) * *multiplicity as int == *multiplicity as int * count_at(*variants, key)) by (nonlinear_arith);
                }
            }
        }
        proof {
            assert forall|key: verus_prooflib::ordered::KeyView| #[trigger] count_at(groups, key)
                == sum(edges.entries().take(processed@ + 1), |row: (verus_prooflib::ordered::KeyView, i64)|
                    row.1 as int * part_at(*parts, row.0.0, key)) by {
                lemma_sum_take(edges.entries(), processed@, |row: (verus_prooflib::ordered::KeyView, i64)| row.1 as int * part_at(*parts, row.0.0, key));
            }
        }
        processed = Ghost(processed@ + 1);
    }
}

pub fn load_size(ranks: &Ranks, counts: &Counts, size: &Key, Ghost(preserve): Ghost<bool>) -> (out: (Index<BrandRanks>, Counts))
    requires size.view().1 == Seq::<u8>::empty(), preserve ==> ranks_valid(*ranks, *counts),
    ensures out.0.wf(), out.1.wf(), preserve ==> brands_valid(out.0, *counts, size.view().0)
        && stream_valid(out.1, *counts, size.view().0, None),
{
    hide(stream_valid);
    hide(brands_valid);
    match ranks.get(size) {
        Some(value) => {
            proof { if preserve { assert(size_valid(*value, *counts, size.view().0)); } }
            let brands = value.brands.share();
            let rows = value.rows.share();
            proof {
                if preserve {
                    verus_prooflib::lemma_ordered_equal(&value.brands, &brands);
                    verus_prooflib::lemma_ordered_equal(&value.rows, &rows);
                    lemma_stream_equal(&value.rows, &rows, *counts, size.view().0, None);
                    lemma_brands_equal(&value.brands, &brands, *counts, size.view().0);
                }
            }
            (brands, rows)
        },
        None => {
            let brands = Index::new();
            let rows = Index::new();
            proof {
                if preserve {
                    assert forall|key: verus_prooflib::ordered::KeyView| #[trigger] counts.at(key).is_some()
                        implies counts.at(key).unwrap().group.size != size.view().0 by {
                        if counts.at(key).unwrap().group.size == size.view().0 {
                            assert(ranks.at((counts.at(key).unwrap().group.size as int, Seq::empty())).is_some());
                            assert(false);
                        }
                    }
                    lemma_empty_stream(rows, *counts, size.view().0, None);
                    lemma_empty_brands(brands, *counts, size.view().0);
                }
            }
            (brands, rows)
        },
    }
}

pub fn load_brand(brands: &Index<BrandRanks>, counts: &Counts, size: i64, brand: &Key, Ghost(preserve): Ghost<bool>) -> (out: Counts)
    requires brand.view().0 == 0, preserve ==> brands_valid(*brands, *counts, size as int),
    ensures out.wf(), preserve ==> stream_valid(out, *counts, size as int, Some(brand.view().1)),
{
    hide(stream_valid);
    match brands.get(brand) {
        Some(value) => {
            proof { if preserve { assert(stream_valid(value.rows, *counts, size as int, Some(brand.view().1))); } }
            let rows = value.rows.share();
            proof {
                if preserve {
                    verus_prooflib::lemma_ordered_equal(&value.rows, &rows);
                    lemma_stream_equal(&value.rows, &rows, *counts, size as int, Some(brand.view().1));
                }
            }
            rows
        },
        None => {
            let rows = Index::new();
            proof {
                if preserve {
                    assert forall|key: verus_prooflib::ordered::KeyView| #[trigger] counts.at(key).is_some()
                        implies !stream_matches(*counts.at(key).unwrap().group, size as int, Some(brand.view().1)) by {
                        if stream_matches(*counts.at(key).unwrap().group, size as int, Some(brand.view().1)) {
                            assert(brands.at((0int, (*counts.at(key).unwrap().group.brand)@)).is_some());
                            assert(false);
                        }
                    }
                    lemma_empty_stream(rows, *counts, size as int, Some(brand.view().1));
                }
            }
            rows
        },
    }
}

#[verifier::rlimit(20)]
pub fn update_total(counts: &mut Counts, ranks: &mut Ranks, group: Arc<Group>, change: i64) -> (ok: bool)
    requires group.valid(), cell_keys_valid(*counts),
    ensures cell_keys_valid(*final(counts)),
        positive_counts(*old(counts)) ==> positive_counts(*final(counts)),
        ok && ranks_valid(*old(ranks), *old(counts)) ==> ranks_valid(*final(ranks), *final(counts)),
        ok ==> forall|key: verus_prooflib::ordered::KeyView| #[trigger] count_at(*final(counts), key)
            == count_at(*old(counts), key) + if key == group.key() { change as int } else { 0int },
{
    hide(stream_valid);
    hide(brands_valid);
    hide(size_profile_valid);
    hide(ranks_valid);
    let previous_counts = Ghost(*counts);
    let previous_ranks = Ghost(*ranks);
    let target = Ghost(group);
    let preserve = Ghost(ranks_valid(*ranks, *counts));
    let group_key = key_for(&*group);
    let before = match counts.get(&group_key) { Some(cell) => cell.count, None => 0 };
    let after = match add(before, change) { Some(value) => value, None => return false };
    if after < 0 { return false; }
    let size_key = Key::number(group.size);
    let brand_key = Key::composite(0, copy_bytes(group.brand.as_slice()));
    let (mut brands, mut size_rows) = load_size(ranks, counts, &size_key, preserve);
    let mut stream = load_brand(&brands, counts, group.size, &brand_key, preserve);
    proof {
        if preserve@ {
            lemma_ranks_positive(previous_ranks@, previous_counts@);
            if before > 0 {
                lemma_rank_present(size_rows, previous_counts@, target@, group.size as int, None, before);
                lemma_rank_present(stream, previous_counts@, target@, group.size as int, Some((*group.brand)@), before);
            }
            if after > 0 && after != before {
                lemma_rank_fresh(size_rows, previous_counts@, target@, group.size as int, None, before, after);
                lemma_rank_fresh(stream, previous_counts@, target@, group.size as int, Some((*group.brand)@), before, after);
            }
        }
    }
    let previous_size_rows = Ghost(size_rows);
    let previous_stream = Ghost(stream);
    let previous_brands = Ghost(brands);
    if before > 0 {
        stream.remove(&Key::composite(-before, copy_bytes(group.kind.as_slice())));
        size_rows.remove(&group.order.with_number(-before));
        proof {
            if preserve@ {
                lemma_rank_remove(&previous_stream@, &stream, group_stream_key(&*target@, before as int, Some((*target@.brand)@)));
                lemma_rank_remove(&previous_size_rows@, &size_rows, group_stream_key(&*target@, before as int, None));
            }
        }
    }
    let unranked_size_rows = Ghost(size_rows);
    let unranked_stream = Ghost(stream);
    if after > 0 {
        let rank = group.order.with_number(-after);
        let kind = Key::composite(-after, copy_bytes(group.kind.as_slice()));
        let cell = Arc::new(Cell::new(group, after));
        stream.put(kind, cell.clone());
        size_rows.put(rank, cell.clone());
        proof {
            if preserve@ {
                assert(unranked_stream@.at(stream_key(*target@, after as int, Some((*target@.brand)@))).is_none());
                assert(unranked_size_rows@.at(stream_key(*target@, after as int, None)).is_none());
                lemma_rank_put(&unranked_stream@, &stream, group_stream_key(&*target@, after as int, Some((*target@.brand)@)), &*cell);
                lemma_rank_put(&unranked_size_rows@, &size_rows, group_stream_key(&*target@, after as int, None), &*cell);
            }
        }
        counts.put(group_key, cell);
    } else { counts.remove(&group_key); }
    proof {
        if preserve@ {
            assert(count_step(previous_counts@, *counts, target@, after));
            lemma_counts_step_positive(previous_counts@, *counts, target@, after);
            assert(positive_counts(*counts));
            assert(stream_step(previous_size_rows@, size_rows, target@, before, after, None));
            assert(stream_step(previous_stream@, stream, target@, before, after, Some((*target@.brand)@)));
            lemma_stream_update(previous_size_rows@, size_rows, previous_counts@, *counts, target@, before, after, target@.size as int, None);
            lemma_stream_update(previous_stream@, stream, previous_counts@, *counts, target@, before, after, target@.size as int, Some((*target@.brand)@));
        }
    }
    let updated_stream = Ghost(stream);
    if stream.is_empty() { brands.remove(&brand_key); }
    else { brands.put(brand_key, Arc::new(BrandRanks { rows: stream })); }
    proof {
        if preserve@ {
            assert(brand_step(previous_brands@, brands, target@, updated_stream@));
            lemma_brands_update(previous_brands@, brands, previous_counts@, *counts, target@, after, updated_stream@);
        }
    }
    let updated_profile = Ghost(SizeRanks { brands, rows: size_rows });
    if brands.is_empty() { ranks.remove(&size_key); }
    else { ranks.put(size_key, Arc::new(SizeRanks { brands, rows: size_rows })); }
    proof {
        if preserve@ {
            assert(size_profile_valid(updated_profile@, *counts, target@.size as int)) by { reveal(size_profile_valid); }
            assert(rank_step(previous_ranks@, *ranks, target@.size as int, updated_profile@));
            lemma_ranks_update(previous_ranks@, *ranks, previous_counts@, *counts, target@, after, updated_profile@);
        }
    }
    true
}

pub fn collect_contribution(counts: &mut Counts, groups: &Counts, sign: i64) -> (ok: bool)
    requires cell_keys_valid(*groups), cell_keys_valid(*counts),
    ensures cell_keys_valid(*final(counts)),
        positive_counts(*old(counts)) && sign >= 0 ==> positive_counts(*final(counts)),
        ok ==> forall|key: verus_prooflib::ordered::KeyView|
        #[trigger] count_at(*final(counts), key) == count_at(*old(counts), key)
            + if count_at(*groups, key) > 0 { sign as int } else { 0 },
{
    let initial = Ghost(*counts);
    proof { groups.model(); }
    let mut cursor = groups.cursor();
    let mut processed: Ghost<int> = Ghost(0);
    loop
        invariant 0 <= processed@ <= groups.entries().len(), cell_keys_valid(*groups), cell_keys_valid(*counts),
            initial@ == *old(counts),
            positive_counts(initial@) && sign >= 0 ==> positive_counts(*counts),
            forall|position: int| 0 <= position < groups.entries().len() ==>
                #[trigger] groups.at(groups.entries()[position].0) == Some(groups.entries()[position].1),
            cursor.remaining() == groups.entries().skip(processed@),
            forall|key: verus_prooflib::ordered::KeyView| #[trigger] count_at(*counts, key)
                == count_at(initial@, key) + sum(groups.entries().take(processed@),
                    |row: (verus_prooflib::ordered::KeyView, Cell)|
                        if row.0 == key && row.1.count > 0 { sign as int } else { 0 }),
        decreases cursor.remaining().len(),
    {
        let remaining = Ghost(cursor.remaining());
        let (key, cell) = match cursor.next() {
            Some(row) => row,
            None => {
                proof {
                    assert(remaining@.len() == 0);
                    assert(processed@ == groups.entries().len());
                    assert(groups.entries().take(processed@) =~= groups.entries());
                    assert forall|key: verus_prooflib::ordered::KeyView| #[trigger] count_at(*counts, key)
                        == count_at(initial@, key) + if count_at(*groups, key) > 0 { sign as int } else { 0int } by {
                        verus_prooflib::lemma_ordered_selected_weight(groups, key,
                            |row: (verus_prooflib::ordered::KeyView, Cell)| if row.1.count > 0 { 1int } else { 0int }, sign as int,
                            |row: (verus_prooflib::ordered::KeyView, Cell)| if row.0 == key && row.1.count > 0 { sign as int } else { 0int });
                        match groups.at(key) {
                            Some(cell) => {
                                assert(verus_prooflib::ordered_weight_at(*groups, key,
                                    |row: (verus_prooflib::ordered::KeyView, Cell)| if row.1.count > 0 { 1int } else { 0int })
                                    == if cell.count > 0 { 1int } else { 0int });
                            },
                            None => {},
                        }
                    }
                }
                return true;
            },
        };
        proof {
            assert((key.view(), *cell) == groups.entries()[processed@]);
            assert(groups.at(groups.entries()[processed@].0) == Some(groups.entries()[processed@].1));
            assert(groups.at(key.view()) == Some(*cell));
            assert(groups.at(key.view()).is_some());
            assert(groups.at(key.view()).unwrap().group.valid());
        }
        if cell.count > 0 && !bump_group(counts, cell.group.clone(), sign) { return false; }
        proof {
            assert forall|query: verus_prooflib::ordered::KeyView| #[trigger] count_at(*counts, query)
                == count_at(initial@, query) + sum(groups.entries().take(processed@ + 1),
                    |row: (verus_prooflib::ordered::KeyView, Cell)| if row.0 == query && row.1.count > 0 { sign as int } else { 0int }) by {
                lemma_sum_take(groups.entries(), processed@,
                    |row: (verus_prooflib::ordered::KeyView, Cell)| if row.0 == query && row.1.count > 0 { sign as int } else { 0int });
            }
        }
        processed = Ghost(processed@ + 1);
    }
}

}
