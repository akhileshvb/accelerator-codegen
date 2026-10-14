use vstd::prelude::*;
use std::collections::HashMap;
use std::sync::Arc;
use verus_common::bytes::*;
use verus_common::sums::*;
use crate::gen_spec::*;

verus! {

broadcast use vstd::seq_lib::group_seq_properties;

pub type Key = u64;
pub type Bucket = Vec<(Vec<u8>, i128)>;

pub open spec fn key_height(key: Key) -> u32 { (key / 4294967296) as u32 }
pub open spec fn key_bucket(key: Key) -> u64 { (key % 4294967296) as u64 }

pub fn make_key(height: u32, bucket: u64) -> (key: Key)
    requires height <= 32, bucket < 4294967296,
    ensures key_height(key) == height, key_bucket(key) == bucket,
{
    let key = height as u64 * 4294967296 + bucket;
    proof {
        verus_prooflib::lemma_numeric_key_parts(height as int, bucket as int, 4294967296);
    }
    key
}

pub struct Patch {
    pub cells: HashMap<Key, Bucket>,
    pub keys: Vec<Key>,
}

pub struct Layer {
    pub cells: HashMap<Key, Arc<Bucket>>,
    pub keys: Vec<Key>,
}

pub open spec fn bucket_count(bucket: Seq<(Vec<u8>, i128)>, priority: Seq<u8>) -> int {
    sum(bucket, |cell: (Vec<u8>, i128)| if cell.0@ == priority { cell.1 as int } else { 0 })
}

pub open spec fn patch_count(patch: Patch, key: Key, priority: Seq<u8>) -> int {
    if patch.cells@.contains_key(key) { bucket_count(patch.cells@[key]@, priority) } else { 0 }
}

pub open spec fn layer_count(layer: Layer, key: Key, priority: Seq<u8>) -> int {
    if layer.cells@.contains_key(key) { bucket_count(layer.cells@[key]@, priority) } else { 0 }
}

pub open spec fn effective(layers: Seq<Arc<Layer>>, key: Key, priority: Seq<u8>) -> int
    decreases layers.len(),
{
    if layers.len() == 0 { 0 }
    else if layers.last().cells@.contains_key(key) { layer_count(*layers.last(), key, priority) }
    else { effective(layers.drop_last(), key, priority) }
}

pub open spec fn bucket_valid(bucket: Seq<(Vec<u8>, i128)>) -> bool {
    &&& forall|index: int| #![trigger bucket[index].1]
        0 <= index < bucket.len() ==> 0 < bucket[index].1 <= i64::MAX
    &&& forall|left: int, right: int| #![trigger bucket[left], bucket[right]]
        0 <= left < right < bucket.len() ==> bucket[left].0@ != bucket[right].0@
}

pub open spec fn layer_valid(layer: Layer) -> bool {
    &&& forall|key: Key| #[trigger] layer.cells@.contains_key(key) <==> layer.keys@.contains(key)
    &&& forall|key: Key| #[trigger] layer.cells@.contains_key(key) ==>
        key_height(key) <= 32 && bucket_valid(layer.cells@[key]@)
}

pub open spec fn patch_valid(patch: Patch) -> bool {
    &&& forall|key: Key| #[trigger] patch.cells@.contains_key(key) <==> patch.keys@.contains(key)
    &&& forall|key: Key| #[trigger] patch.cells@.contains_key(key) ==>
        key_height(key) <= 32 && bucket_unique(patch.cells@[key]@)
}

pub open spec fn bucket_unique(bucket: Seq<(Vec<u8>, i128)>) -> bool {
    forall|left: int, right: int| #![trigger bucket[left], bucket[right]]
        0 <= left < right < bucket.len() ==> bucket[left].0@ != bucket[right].0@
}

pub open spec fn layers_valid(layers: Seq<Arc<Layer>>) -> bool {
    forall|index: int| #![trigger layers[index]]
        0 <= index < layers.len() ==> layer_valid(*layers[index])
}

pub open spec fn block_width(height: u32) -> int
    decreases height,
{
    if height == 0 { 1 } else { 2 * block_width((height - 1) as u32) }
}

pub open spec fn in_block(date: int, key: Key) -> bool {
    key_bucket(key) * block_width(key_height(key)) <= date + 2147483648 < (key_bucket(key) + 1) * block_width(key_height(key))
}

pub proof fn lemma_width(height: u32)
    ensures block_width(height) > 0,
    decreases height,
{
    if height > 0 { lemma_width((height - 1) as u32); }
}

pub proof fn lemma_key_same(left: Key, right: Key)
    requires key_height(left) == key_height(right), key_bucket(left) == key_bucket(right),
    ensures left == right,
{
    assert(left == (left / 4294967296) * 4294967296 + left % 4294967296) by (nonlinear_arith);
    assert(right == (right / 4294967296) * 4294967296 + right % 4294967296) by (nonlinear_arith);
}

pub open spec fn db_count(db: Db, key: Key, priority: Seq<u8>) -> int {
    sum(db.orders, |order: OrdersRow|
        if in_block(order.o_orderdate, key) && order.o_orderpriority == priority && exists_1(db, order) { 1 } else { 0 })
}

pub fn copy_bytes(bytes: &Vec<u8>) -> (out: Vec<u8>)
    ensures out@ == bytes@,
{
    let mut out = Vec::new();
    let mut index = 0;
    while index < bytes.len()
        invariant index <= bytes.len(), out@ == bytes@.take(index as int),
        decreases bytes.len() - index,
    {
        out.push(bytes[index]);
        index += 1;
    }
    out
}

pub fn copy_bucket(bucket: &Bucket) -> (out: Bucket)
    ensures
        out.len() == bucket.len(),
        bucket_unique(bucket@) ==> bucket_unique(out@),
        bucket_valid(bucket@) ==> bucket_valid(out@),
        forall|index: int| #![trigger out@[index], bucket@[index]]
            0 <= index < out.len() ==> out@[index].0@ == bucket@[index].0@ && out@[index].1 == bucket@[index].1,
        forall|priority: Seq<u8>| #[trigger] bucket_count(out@, priority) == bucket_count(bucket@, priority),
{
    let mut out: Bucket = Vec::new();
    let mut index = 0;
    while index < bucket.len()
        invariant index <= bucket.len(), out.len() == index,
            forall|position: int| #![trigger out@[position]] 0 <= position < index ==>
                out@[position].0@ == bucket@[position].0@ && out@[position].1 == bucket@[position].1,
        decreases bucket.len() - index,
    {
        out.push((copy_bytes(&bucket[index].0), bucket[index].1));
        index += 1;
    }
    proof {
        assert forall|priority: Seq<u8>| #[trigger] bucket_count(out@, priority) == bucket_count(bucket@, priority) by {
            verus_prooflib::lemma_sum_aligned(out@, bucket@,
                |cell: (Vec<u8>, i128)| if cell.0@ == priority { cell.1 as int } else { 0 },
                |cell: (Vec<u8>, i128)| if cell.0@ == priority { cell.1 as int } else { 0 });
        }
    }
    out
}

pub fn add_bucket(bucket: &mut Bucket, priority: &Vec<u8>, amount: i128) -> (ok: bool)
    requires bucket_unique(old(bucket)@),
    ensures
        bucket_unique(final(bucket)@),
        ok ==> forall|group: Seq<u8>| #[trigger] bucket_count(final(bucket)@, group)
            == bucket_count(old(bucket)@, group) + if group == priority@ { amount as int } else { 0 },
{
    let original: Ghost<Bucket> = Ghost(*bucket);
    if amount == 0 { return true; }
    let mut index = 0;
    while index < bucket.len()
        invariant index <= bucket.len(), bucket@ == original@@, original@@ == old(bucket)@, bucket_unique(bucket@),
            forall|position: int| #![trigger bucket@[position]] 0 <= position < index ==> bucket@[position].0@ != priority@,
        decreases bucket.len() - index,
    {
        if bytes_eq_exec(&bucket[index].0, priority) {
            let count = match bucket[index].1.checked_add(amount) { Some(value) => value, None => return false };
            bucket[index].1 = count;
            proof {
                assert forall|group: Seq<u8>| #[trigger] bucket_count(bucket@, group)
                    == bucket_count(original@@, group) + if group == priority@ { amount as int } else { 0 } by {
                    lemma_sum_update(original@@, index as int, (original@@[index as int].0, count),
                        |cell: (Vec<u8>, i128)| if cell.0@ == group { cell.1 as int } else { 0 });
                }
                assert(bucket_unique(bucket@)) by {
                    assert forall|left: int, right: int| #![trigger bucket@[left], bucket@[right]]
                        0 <= left < right < bucket@.len() implies bucket@[left].0@ != bucket@[right].0@ by {
                        assert(bucket@[left].0@ == original@@[left].0@);
                        assert(bucket@[right].0@ == original@@[right].0@);
                    }
                }
            }
            return true;
        }
        index += 1;
    }
    let bytes = copy_bytes(priority);
    bucket.push((bytes, amount));
    proof {
        assert forall|group: Seq<u8>| #[trigger] bucket_count(bucket@, group)
            == bucket_count(original@@, group) + if group == priority@ { amount as int } else { 0 } by {
            lemma_sum_push(original@@, (bytes, amount), |cell: (Vec<u8>, i128)| if cell.0@ == group { cell.1 as int } else { 0 });
        }
        assert(bucket_unique(bucket@)) by {
            assert forall|left: int, right: int| #![trigger bucket@[left], bucket@[right]]
                0 <= left < right < bucket@.len() implies bucket@[left].0@ != bucket@[right].0@ by {
                if right < original@@.len() { assert(original@@[left].0@ != original@@[right].0@); }
                else { assert(original@@[left].0@ != priority@); }
            }
        }
    }
    true
}

pub fn empty_patch() -> (out: Patch)
    ensures patch_valid(out), out.keys@.len() == 0,
        forall|key: Key, priority: Seq<u8>| #[trigger] patch_count(out, key, priority) == 0,
{
    Patch { cells: HashMap::new(), keys: Vec::new() }
}

pub fn add_cell(patch: &mut Patch, key: Key, priority: &Vec<u8>, amount: i128) -> (ok: bool)
    requires patch_valid(*old(patch)), key_height(key) <= 32,
    ensures
        patch_valid(*final(patch)),
        ok ==> forall|other: Key, group: Seq<u8>| #[trigger] patch_count(*final(patch), other, group)
            == patch_count(*old(patch), other, group) + if other == key && group == priority@ { amount as int } else { 0 },
{
    let original: Ghost<Patch> = Ghost(*patch);
    if amount == 0 { return true; }
    let mut bucket = match patch.cells.remove(&key) {
        Some(bucket) => bucket,
        None => { patch.keys.push(key); Vec::new() },
    };
    let ok = add_bucket(&mut bucket, priority, amount);
    patch.cells.insert(key, bucket);
    proof {
        assert(patch_valid(*patch)) by {
            assert forall|other: Key| #[trigger] patch.cells@.contains_key(other) <==> patch.keys@.contains(other) by {
                if other != key { assert(patch.cells@.contains_key(other) == original@.cells@.contains_key(other)); }
            }
        }
        if ok {
            assert forall|other: Key, group: Seq<u8>| #[trigger] patch_count(*patch, other, group)
                == patch_count(original@, other, group) + if other == key && group == priority@ { amount as int } else { 0 } by {
                if other != key { assert(patch.cells@[other] == original@.cells@[other]); }
            }
        }
    }
    ok
}

pub fn add_order(patch: &mut Patch, date: i32, priority: &Vec<u8>, amount: i128) -> (ok: bool)
    requires patch_valid(*old(patch)),
    ensures
        patch_valid(*final(patch)),
        ok ==> forall|key: Key, group: Seq<u8>| #[trigger] patch_count(*final(patch), key, group)
            == patch_count(*old(patch), key, group)
                + if key_height(key) <= 32 && in_block(date as int, key) && group == priority@ { amount as int } else { 0 },
{
    let initial: Ghost<Patch> = Ghost(*patch);
    let mut height: u32 = 0;
    let mut bucket = (date as i64 + 2147483648) as u64;
    while height <= 32
        invariant height <= 33, patch_valid(*patch), initial@ == *old(patch), bucket < 4294967296,
            bucket * block_width(height) <= date as int + 2147483648 < (bucket + 1) * block_width(height),
            forall|key: Key, group: Seq<u8>| #[trigger] patch_count(*patch, key, group)
                == patch_count(initial@, key, group)
                    + if key_height(key) < height && in_block(date as int, key) && group == priority@ { amount as int } else { 0 },
        decreases 33 - height,
    {
        let key = make_key(height, bucket);
        let previous: Ghost<Patch> = Ghost(*patch);
        if !add_cell(patch, key, priority, amount) { return false; }
        proof {
            lemma_width(height);
            assert forall|other: Key, group: Seq<u8>| #[trigger] patch_count(*patch, other, group)
                == patch_count(initial@, other, group)
                    + if key_height(other) < height + 1 && in_block(date as int, other) && group == priority@ { amount as int } else { 0 } by {
                if key_height(other) == height && in_block(date as int, other) {
                    verus_prooflib::lemma_aligned_interval_unique(date as int + 2147483648, block_width(height), key_bucket(other) as int, bucket as int);
                    lemma_key_same(other, key);
                }
            }
            assert(block_width((height + 1) as u32) == 2 * block_width(height));
            verus_prooflib::lemma_binary_parent_interval(date as int + 2147483648, block_width(height), bucket as int);
        }
        bucket = bucket / 2;
        height += 1;
    }
    true
}

pub fn lookup(layers: &Vec<Arc<Layer>>, key: Key) -> (out: Option<Arc<Bucket>>)
    requires layers_valid(layers@),
    ensures
        out matches Some(bucket) ==> bucket_valid(bucket@),
        out matches Some(bucket) ==> forall|priority: Seq<u8>| #[trigger] bucket_count(bucket@, priority)
            == effective(layers@, key, priority),
        out is None ==> forall|priority: Seq<u8>| #[trigger] effective(layers@, key, priority) == 0,
{
    let mut index = layers.len();
    while index > 0
        invariant index <= layers.len(), layers_valid(layers@),
            forall|priority: Seq<u8>| #[trigger] effective(layers@, key, priority)
                == effective(layers@.take(index as int), key, priority),
        decreases index,
    {
        index -= 1;
        let layer = &*layers[index];
        proof {
            assert(layers@.take(index as int + 1).last() == layers@[index as int]);
            assert(layers@.take(index as int + 1).drop_last() =~= layers@.take(index as int));
        }
        match layer.cells.get(&key) {
            Some(bucket) => return Some(Arc::clone(bucket)),
            None => {},
        }
    }
    None
}

pub fn add_all(target: &mut Bucket, source: &Bucket) -> (ok: bool)
    requires bucket_unique(old(target)@),
    ensures bucket_unique(final(target)@),
        ok ==> forall|priority: Seq<u8>| #[trigger] bucket_count(final(target)@, priority)
            == bucket_count(old(target)@, priority) + bucket_count(source@, priority),
{
    let original: Ghost<Bucket> = Ghost(*target);
    let mut index = 0;
    while index < source.len()
        invariant index <= source.len(), bucket_unique(target@), original@@ == old(target)@,
            forall|priority: Seq<u8>| #[trigger] bucket_count(target@, priority)
                == bucket_count(original@@, priority) + bucket_count(source@.take(index as int), priority),
        decreases source.len() - index,
    {
        if !add_bucket(target, &source[index].0, source[index].1) { return false; }
        proof {
            assert forall|priority: Seq<u8>| #[trigger] bucket_count(target@, priority)
                == bucket_count(original@@, priority) + bucket_count(source@.take(index as int + 1), priority) by {
                lemma_sum_take(source@, index as int, |cell: (Vec<u8>, i128)| if cell.0@ == priority { cell.1 as int } else { 0 });
            }
        }
        index += 1;
    }
    true
}

pub fn clean_bucket(bucket: &Bucket) -> (out: Option<Bucket>)
    requires bucket_unique(bucket@),
    ensures out matches Some(clean) ==> bucket_valid(clean@)
        && forall|priority: Seq<u8>| #[trigger] bucket_count(clean@, priority) == bucket_count(bucket@, priority),
{
    let mut clean: Bucket = Vec::new();
    let mut index = 0;
    while index < bucket.len()
        invariant index <= bucket.len(), bucket_valid(clean@), bucket_unique(bucket@),
            forall|priority: Seq<u8>| #[trigger] bucket_count(clean@, priority) == bucket_count(bucket@.take(index as int), priority),
            forall|position: int| #![trigger clean@[position]] 0 <= position < clean@.len() ==>
                exists|source: int| #![trigger bucket@[source]] 0 <= source < index
                    && clean@[position].0@ == bucket@[source].0@ && clean@[position].1 == bucket@[source].1,
        decreases bucket.len() - index,
    {
        let count = bucket[index].1;
        if count < 0 || count > i64::MAX as i128 { return None; }
        let previous: Ghost<Bucket> = Ghost(clean);
        if count > 0 { clean.push((copy_bytes(&bucket[index].0), count)); }
        proof {
            assert forall|priority: Seq<u8>| #[trigger] bucket_count(clean@, priority) == bucket_count(bucket@.take(index as int + 1), priority) by {
                lemma_sum_take(bucket@, index as int, |cell: (Vec<u8>, i128)| if cell.0@ == priority { cell.1 as int } else { 0 });
                if count > 0 { lemma_sum_push(previous@@, clean@.last(), |cell: (Vec<u8>, i128)| if cell.0@ == priority { cell.1 as int } else { 0 }); }
                assert(bucket_count(previous@@, priority) == bucket_count(bucket@.take(index as int), priority));
            }
            if count > 0 {
                assert forall|position: int| #![trigger previous@@[position]] 0 <= position < previous@@.len()
                    implies previous@@[position].0@ != bucket@[index as int].0@ by {
                    let source = choose|source: int| 0 <= source < index && previous@@[position].0@ == bucket@[source].0@ && previous@@[position].1 == bucket@[source].1;
                    assert(bucket@[source].0@ != bucket@[index as int].0@);
                }
            }
        }
        index += 1;
    }
    Some(clean)
}

pub open spec fn replacement(cells: Map<Key, Arc<Bucket>>, layers: Seq<Arc<Layer>>, key: Key, priority: Seq<u8>) -> int {
    if cells.contains_key(key) { bucket_count(cells[key]@, priority) } else { effective(layers, key, priority) }
}

pub fn finish_patch(patch: Patch, layers: &Vec<Arc<Layer>>) -> (out: Option<Layer>)
    requires patch_valid(patch), layers_valid(layers@),
    ensures out matches Some(layer) ==> layer_valid(layer),
        out matches Some(layer) ==> forall|key: Key, priority: Seq<u8>| #[trigger] replacement(layer.cells@, layers@, key, priority)
            == effective(layers@, key, priority) + patch_count(patch, key, priority),
{
    let mut cells: HashMap<Key, Arc<Bucket>> = HashMap::new();
    let mut keys: Vec<Key> = Vec::new();
    let mut index = 0;
    while index < patch.keys.len()
        invariant index <= patch.keys.len(), patch_valid(patch), layers_valid(layers@),
            forall|key: Key| #[trigger] cells@.contains_key(key) <==> keys@.contains(key),
            forall|key: Key| #[trigger] cells@.contains_key(key) ==>
                key_height(key) <= 32 && bucket_valid(cells@[key]@) && patch.keys@.take(index as int).contains(key),
            forall|key: Key, priority: Seq<u8>| #[trigger] replacement(cells@, layers@, key, priority)
                == effective(layers@, key, priority) + if patch.keys@.take(index as int).contains(key) { patch_count(patch, key, priority) } else { 0 },
        decreases patch.keys.len() - index,
    {
        let key = patch.keys[index];
        let changes = match patch.cells.get(&key) { Some(value) => value, None => return None };
        let mut changed = false;
        let mut change_index = 0;
        while change_index < changes.len()
            invariant change_index <= changes.len(),
                !changed ==> forall|position: int| #![trigger changes@[position]] 0 <= position < change_index ==> changes@[position].1 == 0,
            decreases changes.len() - change_index,
        {
            if changes[change_index].1 != 0 { changed = true; }
            change_index += 1;
        }
        let previous_cells: Ghost<Map<Key, Arc<Bucket>>> = Ghost(cells@);
        if changed {
            let mut bucket = match lookup(layers, key) { Some(old) => copy_bucket(&*old), None => Vec::new() };
            proof { assert(bucket_unique(bucket@)); }
            if !add_all(&mut bucket, changes) { return None; }
            let clean = match clean_bucket(&bucket) { Some(value) => value, None => return None };
            keys.push(key);
            cells.insert(key, Arc::new(clean));
            proof {
                assert forall|priority: Seq<u8>| #[trigger] replacement(cells@, layers@, key, priority)
                    == effective(layers@, key, priority) + patch_count(patch, key, priority) by {
                    assert(bucket_count(changes@, priority) == patch_count(patch, key, priority));
                }
            }
        } else {
            proof {
                assert forall|priority: Seq<u8>| #[trigger] patch_count(patch, key, priority) == 0 by {
                    lemma_sum_zero(changes@, |cell: (Vec<u8>, i128)| if cell.0@ == priority { cell.1 as int } else { 0 });
                }
            }
        }
        proof {
            assert(patch.keys@.take(index as int + 1) =~= patch.keys@.take(index as int).push(key));
            assert forall|other: Key, priority: Seq<u8>| #[trigger] replacement(cells@, layers@, other, priority)
                == effective(layers@, other, priority) + if patch.keys@.take(index as int + 1).contains(other) { patch_count(patch, other, priority) } else { 0 } by {
                if other != key { assert(replacement(cells@, layers@, other, priority) == replacement(previous_cells@, layers@, other, priority)); }
            }
        }
        index += 1;
    }
    proof {
        assert forall|key: Key, priority: Seq<u8>| #[trigger] replacement(cells@, layers@, key, priority)
            == effective(layers@, key, priority) + patch_count(patch, key, priority) by {
            if !patch.keys@.contains(key) { assert(!patch.cells@.contains_key(key)); }
        }
    }
    let layer = Layer { cells, keys };
    proof {
        assert(layer_valid(layer));
        assert forall|key: Key, priority: Seq<u8>| #[trigger] replacement(layer.cells@, layers@, key, priority)
            == effective(layers@, key, priority) + patch_count(patch, key, priority) by {
            assert(replacement(layer.cells@, layers@, key, priority)
                == effective(layers@, key, priority) + patch_count(patch, key, priority));
        }
    }
    Some(layer)
}

pub fn merge_layer(newest: Layer, older: &Layer) -> (out: Layer)
    requires layer_valid(newest), layer_valid(*older),
    ensures layer_valid(out),
        forall|key: Key| #[trigger] out.cells@.contains_key(key) <==> newest.cells@.contains_key(key) || older.cells@.contains_key(key),
        forall|key: Key, priority: Seq<u8>| #[trigger] layer_count(out, key, priority)
            == if newest.cells@.contains_key(key) { layer_count(newest, key, priority) } else { layer_count(*older, key, priority) },
{
    let original: Ghost<Layer> = Ghost(newest);
    let mut merged = newest;
    let mut index = 0;
    while index < older.keys.len()
        invariant index <= older.keys.len(), layer_valid(merged), layer_valid(*older), original@ == newest,
            forall|key: Key| #[trigger] merged.cells@.contains_key(key) <==>
                original@.cells@.contains_key(key) || older.keys@.take(index as int).contains(key),
            forall|key: Key, priority: Seq<u8>| #[trigger] layer_count(merged, key, priority)
                == if original@.cells@.contains_key(key) { layer_count(original@, key, priority) }
                    else if older.keys@.take(index as int).contains(key) { layer_count(*older, key, priority) } else { 0 },
        decreases older.keys.len() - index,
    {
        let key = older.keys[index];
        let previous: Ghost<Layer> = Ghost(merged);
        if !merged.cells.contains_key(&key) {
            match older.cells.get(&key) {
                Some(bucket) => {
                    merged.keys.push(key);
                    merged.cells.insert(key, Arc::clone(bucket));
                },
                None => {},
            }
        }
        proof {
            assert(older.keys@.take(index as int + 1) =~= older.keys@.take(index as int).push(key));
            assert forall|other: Key, priority: Seq<u8>| #[trigger] layer_count(merged, other, priority)
                == if original@.cells@.contains_key(other) { layer_count(original@, other, priority) }
                    else if older.keys@.take(index as int + 1).contains(other) { layer_count(*older, other, priority) } else { 0 } by {
                if other != key { assert(layer_count(merged, other, priority) == layer_count(previous@, other, priority)); }
            }
        }
        index += 1;
    }
    merged
}

pub fn trim_layer(layer: &Layer) -> (out: Layer)
    requires layer_valid(*layer),
    ensures layer_valid(out),
        forall|key: Key, priority: Seq<u8>| #[trigger] layer_count(out, key, priority) == layer_count(*layer, key, priority),
{
    let mut cells: HashMap<Key, Arc<Bucket>> = HashMap::new();
    let mut keys: Vec<Key> = Vec::new();
    let mut index = 0;
    while index < layer.keys.len()
        invariant index <= layer.keys.len(), layer_valid(*layer),
            forall|key: Key| #[trigger] cells@.contains_key(key) <==> keys@.contains(key),
            forall|key: Key| #[trigger] cells@.contains_key(key) ==>
                key_height(key) <= 32 && bucket_valid(cells@[key]@),
            forall|key: Key, priority: Seq<u8>| #[trigger] replacement(cells@, Seq::empty(), key, priority)
                == if layer.keys@.take(index as int).contains(key) { layer_count(*layer, key, priority) } else { 0 },
        decreases layer.keys.len() - index,
    {
        let key = layer.keys[index];
        let previous: Ghost<Map<Key, Arc<Bucket>>> = Ghost(cells@);
        match layer.cells.get(&key) {
            Some(bucket) => {
                if bucket.len() > 0 {
                    keys.push(key);
                    cells.insert(key, Arc::clone(bucket));
                } else {
                    proof {
                        assert forall|priority: Seq<u8>| #[trigger] layer_count(*layer, key, priority) == 0 by {
                            lemma_sum_empty(bucket@, |cell: (Vec<u8>, i128)| if cell.0@ == priority { cell.1 as int } else { 0 });
                        }
                    }
                }
            },
            None => {},
        }
        proof {
            assert(layer.keys@.take(index as int + 1) =~= layer.keys@.take(index as int).push(key));
            assert forall|other: Key, priority: Seq<u8>| #[trigger] replacement(cells@, Seq::empty(), other, priority)
                == if layer.keys@.take(index as int + 1).contains(other) { layer_count(*layer, other, priority) } else { 0 } by {
                if other != key { assert(replacement(cells@, Seq::empty(), other, priority) == replacement(previous@, Seq::empty(), other, priority)); }
            }
        }
        index += 1;
    }
    let out = Layer { cells, keys };
    proof {
        assert forall|key: Key, priority: Seq<u8>| #[trigger] layer_count(out, key, priority) == layer_count(*layer, key, priority) by {
            assert(replacement(out.cells@, Seq::empty(), key, priority)
                == if layer.keys@.contains(key) { layer_count(*layer, key, priority) } else { 0 });
        }
    }
    out
}

pub fn publish(layers: &Vec<Arc<Layer>>, newest: Layer) -> (out: Vec<Arc<Layer>>)
    requires layer_valid(newest), layers_valid(layers@),
    ensures
        layers_valid(out@),
        forall|key: Key, priority: Seq<u8>| #[trigger] effective(out@, key, priority)
            == if newest.cells@.contains_key(key) { layer_count(newest, key, priority) }
                else { effective(layers@, key, priority) },
{
    let original: Ghost<Layer> = Ghost(newest);
    let mut current = newest;
    let mut remaining = layers.len();
    if current.keys.len() > 0 {
        while remaining > 0
            invariant remaining <= layers.len(), layers_valid(layers@), layer_valid(current), original@ == newest,
                forall|key: Key, priority: Seq<u8>| #[trigger] replacement(current.cells@, layers@.take(remaining as int), key, priority)
                    == replacement(original@.cells@, layers@, key, priority),
            decreases remaining,
        {
            let previous = &*layers[remaining - 1];
            if previous.keys.len() / 2 > current.keys.len() { break; }
            let before: Ghost<Layer> = Ghost(current);
            current = merge_layer(current, previous);
            proof {
                assert(layers@.take(remaining as int).last() == layers@[remaining as int - 1]);
                assert(layers@.take(remaining as int).drop_last() =~= layers@.take(remaining as int - 1));
                assert forall|key: Key, priority: Seq<u8>| #[trigger] replacement(current.cells@, layers@.take(remaining as int - 1), key, priority)
                    == replacement(before@.cells@, layers@.take(remaining as int), key, priority) by {
                    assert(layer_count(current, key, priority)
                        == if before@.cells@.contains_key(key) { layer_count(before@, key, priority) } else { layer_count(*previous, key, priority) });
                }
            }
            remaining -= 1;
        }
    }
    if remaining == 0 {
        let before: Ghost<Layer> = Ghost(current);
        current = trim_layer(&current);
        proof {
            assert forall|key: Key, priority: Seq<u8>| #[trigger] replacement(current.cells@, layers@.take(remaining as int), key, priority)
                == replacement(original@.cells@, layers@, key, priority) by {
                assert(replacement(before@.cells@, layers@.take(remaining as int), key, priority) == replacement(original@.cells@, layers@, key, priority));
                assert(layer_count(current, key, priority) == layer_count(before@, key, priority));
            }
        }
    }
    let mut out: Vec<Arc<Layer>> = Vec::new();
    let mut index = 0;
    while index < remaining
        invariant index <= remaining <= layers.len(), layers_valid(layers@), layer_valid(current), original@ == newest,
            out@ == layers@.take(index as int),
            forall|key: Key, priority: Seq<u8>| #[trigger] replacement(current.cells@, layers@.take(remaining as int), key, priority)
                == replacement(original@.cells@, layers@, key, priority),
        decreases remaining - index,
    {
        out.push(Arc::clone(&layers[index]));
        index += 1;
    }
    if current.keys.len() > 0 { out.push(Arc::new(current)); }
    proof {
        assert forall|key: Key, priority: Seq<u8>| #[trigger] effective(out@, key, priority)
            == replacement(original@.cells@, layers@, key, priority) by {
            if current.keys@.len() > 0 {
                assert(*out@.last() == current);
                assert(out@.drop_last() =~= layers@.take(remaining as int));
            } else {
                assert(!current.cells@.contains_key(key));
            }
            assert(replacement(current.cells@, layers@.take(remaining as int), key, priority)
                == replacement(original@.cells@, layers@, key, priority));
        }
    }
    out
}

}
