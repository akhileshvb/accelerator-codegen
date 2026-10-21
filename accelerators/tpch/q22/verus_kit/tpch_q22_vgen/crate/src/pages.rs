use vstd::prelude::*;
use std::sync::Arc;

verus! {

pub open spec fn fits(x: int) -> bool { -170141183460469231731687303715884105728 <= x <= 170141183460469231731687303715884105727 }

pub const KEY_LIMIT: u128 = 75557863725914323419136;

#[derive(Clone, Copy)]
pub struct Moment {
    pub count: i128,
    pub total: i128,
}

impl Moment {
    pub open spec fn model(self) -> (int, int) { (self.count as int, self.total as int) }

    pub fn zero() -> (result: Self)
        ensures result.model() == (0int, 0int),
    { Moment { count: 0, total: 0 } }

    pub fn add(self, other: Self) -> (result: Option<Self>)
        ensures result matches Some(value) ==> value.model() ==
            (self.count + other.count, self.total + other.total),
            fits(self.count + other.count) && fits(self.total + other.total) ==> result is Some,
    {
        let count = match self.count.checked_add(other.count) { Some(value) => value, None => return None };
        let total = match self.total.checked_add(other.total) { Some(value) => value, None => return None };
        Some(Moment { count, total })
    }
}

pub struct Page {
    children: Vec<Option<Arc<Page>>>,
    values: Vec<Moment>,
}

#[verifier::inline]
pub open spec fn digit(key: u128, depth: u32) -> usize {
    ((key >> (((depth - 1) * 4) as u32)) & 15) as usize
}

pub closed spec fn page_wf(page: Page, depth: u32) -> bool
    decreases depth,
{
    &&& 1 <= depth <= 19
    &&& if depth == 1 { page.values.len() == 16 && page.children.len() == 0 }
        else { page.children.len() == 16 && page.values.len() == 0 }
    &&& forall|index: int| 0 <= index < page.children.len() ==>
        ((#[trigger] page.children[index]) matches Some(child) ==> page_wf(*child, (depth - 1) as u32))
}

pub closed spec fn page_value(page: Page, depth: u32, key: u128) -> (int, int)
    decreases depth,
{
    if depth == 0 { (0, 0) }
    else if depth == 1 { page.values[digit(key, depth) as int].model() }
    else {
        match page.children[digit(key, depth) as int] {
            Some(child) => page_value(*child, (depth - 1) as u32, key),
            None => (0, 0),
        }
    }
}

pub open spec fn optional_value(root: Option<Arc<Page>>, depth: u32, key: u128) -> (int, int) {
    match root { Some(page) => page_value(*page, depth, key), None => (0, 0) }
}

pub open spec fn optional_wf(root: Option<Arc<Page>>, depth: u32) -> bool {
    root matches Some(page) ==> page_wf(*page, depth)
}

#[verifier::inline]
pub open spec fn address_mask(depth: u32) -> u128 {
    ((1u128 << ((depth * 4) as u32)) - 1) as u128
}

proof fn lemma_address(depth: u32, key: u128, other: u128)
    requires 1 <= depth <= 19,
    ensures digit(key, depth) < 16,
        depth == 1 ==> ((other & address_mask(depth)) == (key & address_mask(depth)) <==> digit(other, depth) == digit(key, depth)),
        depth > 1 ==> ((other & address_mask(depth)) == (key & address_mask(depth)) <==>
            digit(other, depth) == digit(key, depth) &&
            (other & address_mask((depth - 1) as u32)) == (key & address_mask((depth - 1) as u32))),
{
    assert(((key >> (((depth - 1) * 4) as u32)) & 15) < 16) by (bit_vector)
        requires 1 <= depth <= 19;
    assert(depth == 1 ==> ((other & address_mask(depth)) == (key & address_mask(depth)) <==> digit(other, depth) == digit(key, depth))) by (bit_vector)
        requires 1 <= depth <= 19;
    assert(depth > 1 ==> ((other & address_mask(depth)) == (key & address_mask(depth)) <==>
        digit(other, depth) == digit(key, depth) && (other & address_mask((depth - 1) as u32)) == (key & address_mask((depth - 1) as u32)))) by (bit_vector)
        requires 1 <= depth <= 19;
}

proof fn lemma_leaf_index(index: u128)
    requires index < 16,
    ensures digit(index, 1) == index,
{
    assert(digit(index, 1) == index) by (bit_vector) requires index < 16;
}

fn read_page(page: &Page, depth: u32, key: u128) -> (result: Moment)
    requires page_wf(*page, depth),
    ensures result.model() == page_value(*page, depth, key),
    decreases depth,
{
    proof { reveal(page_wf); reveal(page_value); lemma_address(depth, key, key); }
    let slot = ((key >> ((depth - 1) * 4)) & 15) as usize;
    if depth == 1 { page.values[slot] }
    else {
        match &page.children[slot] {
            Some(child) => read_page(child, depth - 1, key),
            None => Moment::zero(),
        }
    }
}

fn write_page(root: &Option<Arc<Page>>, depth: u32, key: u128, value: Moment) -> (result: Option<Arc<Page>>)
    requires 1 <= depth <= 19, optional_wf(*root, depth),
    ensures optional_wf(result, depth),
        forall|other: u128| #[trigger] optional_value(result, depth, other) ==
            if (other & address_mask(depth)) == (key & address_mask(depth)) { value.model() }
            else { optional_value(*root, depth, other) },
    decreases depth,
{
    proof { reveal(page_wf); reveal(page_value); lemma_address(depth, key, key); }
    let slot = ((key >> ((depth - 1) * 4)) & 15) as usize;
    let mut page = Page { children: Vec::new(), values: Vec::new() };
    let mut live = false;
    if depth == 1 {
        let mut index = 0usize;
        while index < 16
            invariant index <= 16, page.values.len() == index, page.children.len() == 0,
                1 == depth, slot < 16, slot == digit(key, depth), optional_wf(*root, depth),
                !live ==> forall|position: int| 0 <= position < page.values.len() ==> (#[trigger] page.values[position]).model() == (0int, 0int),
                forall|position: int| 0 <= position < page.values.len() ==>
                    (#[trigger] page.values[position]).model() == if position == slot {
                        value.model()
                    } else { optional_value(*root, depth, position as u128) },
            decreases 16 - index,
        {
            proof { lemma_leaf_index(index as u128); }
            let entry = if index == slot { value } else {
                match root { Some(old_page) => old_page.values[index], None => Moment::zero() }
            };
            if entry.count != 0 || entry.total != 0 { live = true; }
            page.values.push(entry);
            proof {
                assert forall|position: int| 0 <= position < page.values.len() implies
                    (#[trigger] page.values[position]).model() == if position == slot { value.model() }
                    else { optional_value(*root, depth, position as u128) } by { }
                if !live {
                    assert forall|position: int| 0 <= position < page.values.len() implies (#[trigger] page.values[position]).model() == (0int, 0int) by { }
                }
            }
            index += 1;
        }
        proof {
            assert forall|other: u128| #[trigger] page_value(page, depth, other) ==
                if (other & address_mask(depth)) == (key & address_mask(depth)) { value.model() }
                else { optional_value(*root, depth, other) } by {
                lemma_address(depth, key, other);
                lemma_address(depth, other, other);
                lemma_leaf_index(digit(other, depth) as u128);
                assert(optional_value(*root, depth, digit(other, depth) as u128) == optional_value(*root, depth, other));
            }
        }
    } else {
        let child = match root {
            Some(old_page) => match &old_page.children[slot] { Some(child) => Some(child.clone()), None => None },
            None => None,
        };
        let updated = write_page(&child, depth - 1, key, value);
        let mut index = 0usize;
        while index < 16
            invariant index <= 16, page.children.len() == index, page.values.len() == 0,
                1 < depth <= 19, slot < 16, slot == digit(key, depth), optional_wf(*root, depth), optional_wf(updated, (depth - 1) as u32),
                forall|other: u128| #[trigger] optional_value(updated, (depth - 1) as u32, other) ==
                    if (other & address_mask((depth - 1) as u32)) == (key & address_mask((depth - 1) as u32)) { value.model() }
                    else { optional_value(child, (depth - 1) as u32, other) },
                child == match *root { Some(old_page) => old_page.children[slot as int], None => None },
                !live ==> forall|position: int| 0 <= position < page.children.len() ==> (#[trigger] page.children[position]) is None,
                forall|position: int| 0 <= position < page.children.len() ==>
                    optional_wf(#[trigger] page.children[position], (depth - 1) as u32),
                forall|position: int| 0 <= position < page.children.len() ==>
                    (#[trigger] page.children[position]) == if position == slot { updated }
                    else { match *root { Some(old_page) => old_page.children[position], None => None } },
            decreases 16 - index,
        {
            let entry = if index == slot {
                match &updated { Some(child) => Some(child.clone()), None => None }
            } else {
                match root {
                    Some(old_page) => match &old_page.children[index] { Some(child) => Some(child.clone()), None => None },
                    None => None,
                }
            };
            if let Some(_) = &entry { live = true; }
            page.children.push(entry);
            proof {
                if !live {
                    assert forall|position: int| 0 <= position < page.children.len() implies (#[trigger] page.children[position]) is None by { }
                }
            }
            index += 1;
        }
        proof {
            assert forall|other: u128| #[trigger] page_value(page, depth, other) ==
                if (other & address_mask(depth)) == (key & address_mask(depth)) { value.model() }
                else { optional_value(*root, depth, other) } by {
                lemma_address(depth, key, other);
                lemma_address(depth, other, other);
                let position = digit(other, depth) as int;
                assert(optional_value(page.children[position], (depth - 1) as u32, other) ==
                    if position == slot {
                        if (other & address_mask((depth - 1) as u32)) == (key & address_mask((depth - 1) as u32)) { value.model() }
                        else { optional_value(child, (depth - 1) as u32, other) }
                    } else { optional_value(match *root { Some(old_page) => old_page.children[position], None => None }, (depth - 1) as u32, other) });
            }
        }
    }
    proof {
        assert(page_wf(page, depth));
        if !live {
            assert forall|other: u128| #[trigger] page_value(page, depth, other) == (0int, 0int) by {
                lemma_address(depth, other, other);
            }
        }
    }
    let finished = Ghost(page);
    let result = if live { Some(Arc::new(page)) } else {
        proof {
            assert forall|other: u128| (if (other & address_mask(depth)) == (key & address_mask(depth)) { value.model() }
                else { #[trigger] optional_value(*root, depth, other) }) == (0int, 0int) by {
                assert(page_value(page, depth, other) == (0int, 0int));
            }
        }
        None
    };
    proof {
        assert forall|other: u128| #[trigger] optional_value(result, depth, other) ==
            if (other & address_mask(depth)) == (key & address_mask(depth)) { value.model() }
            else { optional_value(*root, depth, other) } by {
            if let Some(new_page) = result {
                assert(*new_page == finished@);
                assert(page_value(finished@, depth, other) ==
                    if (other & address_mask(depth)) == (key & address_mask(depth)) { value.model() }
                    else { optional_value(*root, depth, other) });
            } else {
                assert(page_value(finished@, depth, other) == (0int, 0int));
            }
        }
    }
    result
}

pub struct Pages { root: Option<Arc<Page>> }

impl Pages {
    #[verifier::type_invariant]
    pub closed spec fn wf(self) -> bool { optional_wf(self.root, 19) }

    pub closed spec fn value(self, key: u128) -> (int, int) { optional_value(self.root, 19, key) }

    pub fn new() -> (result: Self)
        ensures forall|key: u128| #[trigger] result.value(key) == (0int, 0int),
    { Pages { root: None } }

    pub fn share(&self) -> (result: Self)
        ensures result == *self,
    {
        proof { use_type_invariant(&*self); }
        let root = match &self.root { Some(page) => Some(page.clone()), None => None };
        Pages { root }
    }

    pub fn get(&self, key: u128) -> (result: Moment)
        requires key < KEY_LIMIT,
        ensures result.model() == self.value(key),
    {
        proof { use_type_invariant(&*self); }
        match &self.root { Some(page) => read_page(page, 19, key), None => Moment::zero() }
    }

    pub fn set(&mut self, key: u128, value: Moment)
        requires key < KEY_LIMIT,
        ensures forall|other: u128| other < KEY_LIMIT ==>
            #[trigger] final(self).value(other) == if other == key { value.model() } else { old(self).value(other) },
    {
        proof { use_type_invariant(&*self); }
        let original = Ghost(self.root);
        self.root = write_page(&self.root, 19, key, value);
        proof {
            assert forall|other: u128| other < KEY_LIMIT implies #[trigger] self.value(other) ==
                if other == key { value.model() } else { optional_value(original@, 19, other) } by {
                assert((key & address_mask(19)) == key && (other & address_mask(19)) == other) by (bit_vector)
                    requires key < KEY_LIMIT, other < KEY_LIMIT;
            }
        }
    }

    pub fn add(&mut self, key: u128, change: Moment) -> (result: bool)
        requires key < KEY_LIMIT,
        ensures result ==> forall|other: u128| other < KEY_LIMIT ==>
            #[trigger] final(self).value(other) == if other == key {
                (old(self).value(other).0 + change.count, old(self).value(other).1 + change.total)
            } else { old(self).value(other) },
            fits(old(self).value(key).0 + change.count) && fits(old(self).value(key).1 + change.total) ==> result,
    {
        match self.get(key).add(change) { Some(value) => { self.set(key, value); true }, None => false }
    }
}

}
