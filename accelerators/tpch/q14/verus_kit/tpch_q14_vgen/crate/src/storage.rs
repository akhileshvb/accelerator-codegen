use vstd::prelude::*;
use std::collections::HashMap;
use std::sync::Arc;
use verus_common::rel::pow2;
use crate::moments::*;
use crate::moments::add;
use crate::addressing::*;
use crate::prefixes::{lemma_prefix_contains, lemma_push_contains};

verus! {

pub enum Directory {
    Branch(Vec<Option<Arc<Directory>>>),
    Page(HashMap<u64, Moments>),
}

pub open spec fn valid(root: Option<Arc<Directory>>, depth: nat) -> bool
    decreases depth,
{
    match root {
        None => true,
        Some(node) => match *node {
            Directory::Page(map) => depth == 0 && map@.dom().len() > 0 &&
                forall|key: u64| #[trigger] map@.dom().contains(key) ==> key < 64 && map@[key].count > 0,
            Directory::Branch(children) => depth > 0 && children.len() == 16 &&
                (forall|index: int| 0 <= index < 16 ==>
                    valid(#[trigger] children@[index], (depth - 1) as nat)),
        },
    }
}

pub open spec fn lookup(root: Option<Arc<Directory>>, depth: nat, id: u64) -> Moments
    decreases depth,
{
    match root {
        None => zero(),
        Some(node) => match *node {
            Directory::Page(map) => block_value(map@, id % 64),
            Directory::Branch(children) => if depth > 0 {
                let digit = ((id as int / 64) / pow2((4 * (depth - 1)) as nat)) % 16;
                lookup(children@[digit], (depth - 1) as nat, id)
            } else { zero() },
        },
    }
}

pub open spec fn child(root: Option<Arc<Directory>>, digit: int) -> Option<Arc<Directory>> {
    match root {
        None => None,
        Some(node) => match *node {
            Directory::Branch(children) => children@[digit],
            Directory::Page(_) => None,
        },
    }
}

pub proof fn lemma_child(root: Option<Arc<Directory>>, depth: nat, digit: int)
    requires valid(root, depth), depth > 0, 0 <= digit < 16,
    ensures valid(child(root, digit), (depth - 1) as nat),
{
    match root {
        None => {},
        Some(node) => match *node {
            Directory::Page(_) => {},
            Directory::Branch(children) => {
                assert(valid(root, depth));
                assert(valid(children@[digit], (depth - 1) as nat));
            },
        },
    }
}

pub proof fn lemma_lookup_child(root: Option<Arc<Directory>>, depth: nat, id: u64)
    requires valid(root, depth), depth > 0,
    ensures lookup(root, depth, id) == lookup(child(root,
        ((id as int / 64) / pow2((4 * (depth - 1)) as nat)) % 16), (depth - 1) as nat, id),
{
    lemma_pow_positive((4 * (depth - 1)) as nat);
}

fn digit_stride(depth: u32) -> (out: u64)
    requires 1 <= depth <= 7,
    ensures out as int == pow2((4 * (depth - 1)) as nat), out > 0,
{
    match depth {
        1 => {
            proof { reveal_with_fuel(pow2, 35); assert(pow2(0) == 1); }
            1
        }
        2 => {
            proof { reveal_with_fuel(pow2, 35); assert(pow2(4) == 16); }
            16
        }
        3 => {
            proof { reveal_with_fuel(pow2, 35); assert(pow2(8) == 256); }
            256
        }
        4 => {
            proof { reveal_with_fuel(pow2, 35); assert(pow2(12) == 4096); }
            4096
        }
        5 => {
            proof { reveal_with_fuel(pow2, 35); assert(pow2(16) == 65536); }
            65536
        }
        6 => {
            proof { reveal_with_fuel(pow2, 35); assert(pow2(20) == 1048576); }
            1048576
        }
        7 => {
            proof { reveal_with_fuel(pow2, 35); assert(pow2(24) == 16777216); }
            16777216
        }
        _ => 0,
    }
}

pub fn get(root: &Option<Arc<Directory>>, depth: u32, id: u64) -> (out: Moments)
    requires depth <= 7, valid(*root, depth as nat),
    ensures same(out, lookup(*root, depth as nat, id)),
    decreases depth,
    {
    match root {
        None => empty(),
        Some(node) => match &**node {
            Directory::Page(map) => get_block(map, id % 64),
            Directory::Branch(children) => {
                if depth == 0 { return empty(); }
                let stride = digit_stride(depth);
                let digit = ((id / 64 / stride) % 16) as usize;
                proof {
                    assert(children.len() == 16);
                    lemma_child(*root, depth as nat, digit as int);
                }
                get(&children[digit], depth - 1, id)
            },
        },
    }
}

pub fn share(root: &Option<Arc<Directory>>) -> (out: Option<Arc<Directory>>)
    ensures out == *root,
{ match root { None => None, Some(node) => Some(node.clone()) } }

pub fn replace_page(root: &Option<Arc<Directory>>, depth: u32, page: u64,
    replacement: &Option<Arc<Directory>>) -> (out: Option<Arc<Directory>>)
    requires depth <= 7, valid(*root, depth as nat), valid(*replacement, 0),
    ensures valid(out, depth as nat),
        forall|id: u64| #[trigger] lookup(out, depth as nat, id) ==
            if (id as int / 64) % pow2((4 * depth) as nat) == (page as int) % pow2((4 * depth) as nat) {
                lookup(*replacement, 0, id)
            } else { lookup(*root, depth as nat, id) },
    decreases depth,
{
    if depth == 0 { return share(replacement); }
    let stride = digit_stride(depth);
    let digit = ((page / stride) % 16) as usize;
    let mut children: Vec<Option<Arc<Directory>>> = Vec::new();
    let mut index: usize = 0;
    while index < 16
        invariant index <= 16, children.len() == index, 0 < depth <= 7, valid(*root, depth as nat),
            valid(*replacement, 0),
            forall|position: int| 0 <= position < index ==> (#[trigger] children@[position]) == child(*root, position),
        decreases 16 - index,
    {
        let child = match root {
            Some(node) => match &**node {
                Directory::Branch(previous) => share(&previous[index]),
                Directory::Page(_) => None,
            },
            None => None,
        };
        children.push(child);
        index += 1;
    }
    proof { lemma_child(*root, depth as nat, digit as int); }
    let child = replace_page(&children[digit], depth - 1, page, replacement);
    children.set(digit, child);
    proof {
        assert forall|position: int| 0 <= position < 16 implies
            #[trigger] valid(children@[position], (depth - 1) as nat) by {
            if position != digit as int { lemma_child(*root, depth as nat, position); }
        }
    }
    let mut has_children = false;
    index = 0;
    while index < 16
        invariant index <= 16, children.len() == 16,
            digit < 16, digit as int == (page as int / pow2((4 * (depth - 1)) as nat)) % 16,
            forall|position: int| 0 <= position < index && !has_children ==> (#[trigger] children@[position]) is None,
            forall|position: int| 0 <= position < 16 ==> #[trigger] valid(children@[position], (depth - 1) as nat),
            forall|position: int| 0 <= position < 16 && position != digit as int ==>
                (#[trigger] children@[position]) == crate::storage::child(*root, position),
            children@[digit as int] == child, 0 < depth <= 7, valid(*root, depth as nat),
            forall|id: u64| #[trigger] lookup(child, (depth - 1) as nat, id) ==
                if (id as int / 64) % pow2((4 * (depth - 1)) as nat) == (page as int) % pow2((4 * (depth - 1)) as nat) {
                    lookup(*replacement, 0, id)
                } else { lookup(crate::storage::child(*root, digit as int), (depth - 1) as nat, id) },
        decreases 16 - index,
    {
        if children[index].is_some() { has_children = true; }
        index += 1;
    }
    let ghost saved_children = children@;
    let result = if has_children { Some(Arc::new(Directory::Branch(children))) } else { None };
    proof {
        lemma_pow_constants();
        lemma_pow_add((4 * (depth - 1)) as nat, 4);
        assert forall|id: u64| #[trigger] lookup(result, depth as nat, id) ==
            if (id as int / 64) % pow2((4 * depth) as nat) == (page as int) % pow2((4 * depth) as nat) {
                lookup(*replacement, 0, id)
            } else { lookup(*root, depth as nat, id) } by {
            let selected = ((id as int / 64) / pow2((4 * (depth - 1)) as nat)) % 16;
            lemma_same_radix(id as int / 64, page as int, pow2((4 * (depth - 1)) as nat));
            lemma_lookup_child(*root, depth as nat, id);
            lemma_lookup_child(result, depth as nat, id);
            if selected == digit as int {
                assert(lookup(child, (depth - 1) as nat, id) == lookup(crate::storage::child(result, selected), (depth - 1) as nat, id));
            } else {
                assert(saved_children[selected] == crate::storage::child(*root, selected));
                if !has_children { assert(saved_children[selected] is None); }
                assert(crate::storage::child(result, selected) == saved_children[selected]);
                assert(crate::storage::child(result, selected) == crate::storage::child(*root, selected));
            }
        }
    }
    result
}

pub struct Changes {
    pub blocks: HashMap<u64, Moments>,
    pub pages: Vec<u64>,
    pub seen: HashMap<u64, bool>,
}

impl Changes {
    pub open spec fn wf(self) -> bool {
        &&& self.pages@.no_duplicates()
        &&& forall|page: u64| #[trigger] self.seen@.dom().contains(page) == self.pages@.contains(page)
        &&& forall|index: int| 0 <= index < self.pages.len() ==> (#[trigger] self.pages@[index]) < 134217728u64
        &&& forall|id: u64| #[trigger] self.blocks@.dom().contains(id) ==>
            0 < id < 8589934592u64 && self.seen@.dom().contains(id / 64)
    }

    pub fn new() -> (out: Changes)
        ensures out.wf(), out.blocks@ == Map::<u64, Moments>::empty(), out.pages@ == Seq::<u64>::empty(),
    { Changes { blocks: HashMap::new(), pages: Vec::new(), seen: HashMap::new() } }

    pub fn add_day(&mut self, date: i32, change: Moments) -> (out: bool)
        requires old(self).wf(),
        ensures out ==> final(self).wf(),
            out ==> forall|id: u64| (#[trigger] block_value(final(self).blocks@, id)).count as int ==
            block_value(old(self).blocks@, id).count as int +
                if in_block(id, date as int) { change.count as int } else { 0 },
            out ==> forall|id: u64| (#[trigger] block_value(final(self).blocks@, id)).total as int ==
            block_value(old(self).blocks@, id).total as int +
                if in_block(id, date as int) { change.total as int } else { 0 },
            out ==> forall|id: u64| (#[trigger] block_value(final(self).blocks@, id)).promo as int ==
            block_value(old(self).blocks@, id).promo as int +
                if in_block(id, date as int) { change.promo as int } else { 0 },
    {
        if change.count == 0 && change.total == 0 && change.promo == 0 { return true; }
        let original_date: Ghost<int> = Ghost(date as int);
        let initial: Ghost<Map<u64, Moments>> = Ghost(self.blocks@);
        let date = date_key(date);
        let mut level: u32 = 0;
        while level <= 32
            invariant level <= 33, self.wf(),
                original_date@ == date as int - 2147483648, date < 4294967296u64,
                forall|id: u64| (#[trigger] block_value(self.blocks@, id)).count as int ==
                    block_value(initial@, id).count as int +
                        if in_block_prefix(id, original_date@, level as nat) { change.count as int } else { 0 },
                forall|id: u64| (#[trigger] block_value(self.blocks@, id)).total as int ==
                    block_value(initial@, id).total as int +
                        if in_block_prefix(id, original_date@, level as nat) { change.total as int } else { 0 },
                forall|id: u64| (#[trigger] block_value(self.blocks@, id)).promo as int ==
                    block_value(initial@, id).promo as int +
                        if in_block_prefix(id, original_date@, level as nat) { change.promo as int } else { 0 },
            decreases 33 - level,
        {
            let width = power(level);
            let base = power(32 - level);
            proof { lemma_block_bounds(original_date@, level as nat); }
            let id = base + date / width;
            let before = get_block(&self.blocks, id);
            let after = match add(before, change) { Some(value) => value, None => return false };
            let ghost previous = self.blocks@;
            let ghost previous_pages = self.pages@;
            let ghost previous_seen = self.seen@;
            self.blocks.insert(id, after);
            let page = id / 64;
            let add_page = !self.seen.contains_key(&page);
            if add_page {
                self.seen.insert(page, true);
                self.pages.push(page);
            }
            proof {
                assert forall|key: u64| (#[trigger] block_value(self.blocks@, key)).count as int == block_value(initial@, key).count as int +
                    if in_block_prefix(key, original_date@, level as nat + 1) { change.count as int } else { 0 } by {
                    lemma_block_prefix(key, original_date@, level as nat);
                    assert(block_value(previous, key).count as int == block_value(initial@, key).count as int +
                        if in_block_prefix(key, original_date@, level as nat) { change.count as int } else { 0 });
                }
                assert forall|key: u64| (#[trigger] block_value(self.blocks@, key)).total as int == block_value(initial@, key).total as int +
                    if in_block_prefix(key, original_date@, level as nat + 1) { change.total as int } else { 0 } by {
                    lemma_block_prefix(key, original_date@, level as nat);
                    assert(block_value(previous, key).total as int == block_value(initial@, key).total as int +
                        if in_block_prefix(key, original_date@, level as nat) { change.total as int } else { 0 });
                }
                assert forall|key: u64| (#[trigger] block_value(self.blocks@, key)).promo as int == block_value(initial@, key).promo as int +
                    if in_block_prefix(key, original_date@, level as nat + 1) { change.promo as int } else { 0 } by {
                    lemma_block_prefix(key, original_date@, level as nat);
                    assert(block_value(previous, key).promo as int == block_value(initial@, key).promo as int +
                        if in_block_prefix(key, original_date@, level as nat) { change.promo as int } else { 0 });
                }
                assert forall|candidate: u64| #[trigger] self.seen@.dom().contains(candidate) == self.pages@.contains(candidate) by {
                    if add_page { lemma_push_contains(previous_pages, page, candidate); }
                    assert(previous_seen.dom().contains(candidate) == previous_pages.contains(candidate));
                }
                assert forall|key: u64| #[trigger] self.blocks@.dom().contains(key) implies
                    0 < key < 8589934592u64 && self.seen@.dom().contains(key / 64) by {
                    if key != id {
                        assert(previous.dom().contains(key));
                        assert(previous_seen.dom().contains(key / 64));
                    }
                }
            }
            level += 1;
        }
        true
    }

    pub fn publish(&self, root: &Option<Arc<Directory>>) -> (out: Option<Option<Arc<Directory>>>)
        requires self.wf(), valid(*root, 7),
        ensures out matches Some(result) ==> valid(result, 7),
            out matches Some(result) ==> forall|id: u64| id < 17179869184u64 ==> {
                &&& (#[trigger] lookup(result, 7, id)).count as int ==
                    lookup(*root, 7, id).count as int + block_value(self.blocks@, id).count as int
                &&& lookup(result, 7, id).total as int ==
                    lookup(*root, 7, id).total as int + block_value(self.blocks@, id).total as int
                &&& lookup(result, 7, id).promo as int ==
                    lookup(*root, 7, id).promo as int + block_value(self.blocks@, id).promo as int
            },
    {
        let mut result = share(root);
        let mut index: usize = 0;
        while index < self.pages.len()
            invariant index <= self.pages.len(), valid(result, 7), self.wf(), valid(*root, 7),
                forall|id: u64| id < 17179869184u64 ==> {
                    &&& (#[trigger] lookup(result, 7, id)).count as int == lookup(*root, 7, id).count as int +
                        if self.pages@.take(index as int).contains(id / 64) { block_value(self.blocks@, id).count as int } else { 0 }
                    &&& lookup(result, 7, id).total as int == lookup(*root, 7, id).total as int +
                        if self.pages@.take(index as int).contains(id / 64) { block_value(self.blocks@, id).total as int } else { 0 }
                    &&& lookup(result, 7, id).promo as int == lookup(*root, 7, id).promo as int +
                        if self.pages@.take(index as int).contains(id / 64) { block_value(self.blocks@, id).promo as int } else { 0 }
                },
            decreases self.pages.len() - index,
        {
            let page = self.pages[index];
            let mut map: HashMap<u64, Moments> = HashMap::new();
            let mut offset: u64 = 0;
            while offset < 64
                invariant offset <= 64, page < 134217728u64, valid(*root, 7),
                    forall|slot: u64| (#[trigger] map@.dom().contains(slot)) ==> slot < offset && map@[slot].count > 0,
                    forall|slot: u64| slot < offset ==> {
                        &&& (#[trigger] block_value(map@, slot)).count as int ==
                            lookup(*root, 7, (page * 64 + slot) as u64).count as int + block_value(self.blocks@, (page * 64 + slot) as u64).count as int
                        &&& block_value(map@, slot).total as int ==
                            lookup(*root, 7, (page * 64 + slot) as u64).total as int + block_value(self.blocks@, (page * 64 + slot) as u64).total as int
                        &&& block_value(map@, slot).promo as int ==
                            lookup(*root, 7, (page * 64 + slot) as u64).promo as int + block_value(self.blocks@, (page * 64 + slot) as u64).promo as int
                    },
                decreases 64 - offset,
            {
                let id = page * 64 + offset;
                let before = get(root, 7, id);
                let change = get_block(&self.blocks, id);
                let after = match add(before, change) { Some(value) => value, None => return None };
                if after.count < 0 { return None; }
                let ghost earlier = map@;
                if after.count == 0 {
                    if after.total != 0 || after.promo != 0 { return None; }
                } else { map.insert(offset, after); }
                proof {
                    assert forall|slot: u64| slot < offset + 1 implies {
                        &&& (#[trigger] block_value(map@, slot)).count as int ==
                            lookup(*root, 7, (page * 64 + slot) as u64).count as int + block_value(self.blocks@, (page * 64 + slot) as u64).count as int
                        &&& block_value(map@, slot).total as int ==
                            lookup(*root, 7, (page * 64 + slot) as u64).total as int + block_value(self.blocks@, (page * 64 + slot) as u64).total as int
                        &&& block_value(map@, slot).promo as int ==
                            lookup(*root, 7, (page * 64 + slot) as u64).promo as int + block_value(self.blocks@, (page * 64 + slot) as u64).promo as int
                    } by {
                        if slot < offset {
                            assert(same(block_value(map@, slot), block_value(earlier, slot)));
                        } else {
                            assert(slot == offset);
                            assert(!earlier.dom().contains(slot));
                            assert(same(block_value(map@, slot), after));
                        }
                    }
                }
                offset += 1;
            }
            let replacement = if map.len() == 0 { None } else { Some(Arc::new(Directory::Page(map))) };
            proof {
                assert(valid(replacement, 0));
                assert forall|id: u64| id < 17179869184u64 && id / 64 == page implies {
                    &&& (#[trigger] lookup(replacement, 0, id)).count as int == lookup(*root, 7, id).count as int + block_value(self.blocks@, id).count as int
                    &&& lookup(replacement, 0, id).total as int == lookup(*root, 7, id).total as int + block_value(self.blocks@, id).total as int
                    &&& lookup(replacement, 0, id).promo as int == lookup(*root, 7, id).promo as int + block_value(self.blocks@, id).promo as int
                } by {
                    lemma_division(id as int, 64);
                    assert(id == page * 64 + id % 64);
                    assert(same(lookup(replacement, 0, id), block_value(map@, id % 64)));
                }
            }
            let ghost earlier = result;
            result = replace_page(&result, 7, page, &replacement);
            proof {
                lemma_pow_constants();
                assert(pow2(28) == 268435456);
                assert(!self.pages@.take(index as int).contains(page)) by {
                    if self.pages@.take(index as int).contains(page) {
                        let position = choose|position: int| 0 <= position < index && #[trigger] self.pages@.take(index as int)[position] == page;
                        assert(self.pages@[position] == self.pages@[index as int]);
                    }
                }
                assert forall|id: u64| id < 17179869184u64 implies {
                    &&& (#[trigger] lookup(result, 7, id)).count as int == lookup(*root, 7, id).count as int +
                        if self.pages@.take(index as int + 1).contains(id / 64) { block_value(self.blocks@, id).count as int } else { 0 }
                    &&& lookup(result, 7, id).total as int == lookup(*root, 7, id).total as int +
                        if self.pages@.take(index as int + 1).contains(id / 64) { block_value(self.blocks@, id).total as int } else { 0 }
                    &&& lookup(result, 7, id).promo as int == lookup(*root, 7, id).promo as int +
                        if self.pages@.take(index as int + 1).contains(id / 64) { block_value(self.blocks@, id).promo as int } else { 0 }
                } by {
                    lemma_prefix_contains(self.pages@, index as int, id / 64);
                    assert(id as int / 64 < 268435456);
                    assert((id as int / 64) % pow2(28) == id as int / 64);
                    assert(page as int % pow2(28) == page as int);
                    if id / 64 != page {
                        assert(lookup(result, 7, id) == lookup(earlier, 7, id));
                    }
                }
            }
            index += 1;
        }
        proof {
            assert(self.pages@.take(index as int) =~= self.pages@);
            assert forall|id: u64| id < 17179869184u64 implies {
                &&& (#[trigger] lookup(result, 7, id)).count as int == lookup(*root, 7, id).count as int + block_value(self.blocks@, id).count as int
                &&& lookup(result, 7, id).total as int == lookup(*root, 7, id).total as int + block_value(self.blocks@, id).total as int
                &&& lookup(result, 7, id).promo as int == lookup(*root, 7, id).promo as int + block_value(self.blocks@, id).promo as int
            } by {
                if !self.pages@.contains(id / 64) { assert(!self.blocks@.dom().contains(id)); }
            }
        }
        Some(result)
    }
}

}
