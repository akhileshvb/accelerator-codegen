use vstd::prelude::*;
use std::collections::HashMap;
use std::sync::Arc;

verus! {

pub struct Sparse {
    values: HashMap<i128, i128>,
    keys: Vec<i128>,
}

impl Sparse {
    pub proof fn lemma_keys(self)
        requires self.wf(),
        ensures verus_common::rel::no_dups(self.keys()),
            forall|key: i128| #[trigger] self.keys().contains(key) == self.contains(key as int),
    {}

    pub proof fn lemma_support(self, key: int)
        requires self.wf(),
        ensures self.at(key) != 0 ==> i128::MIN <= key <= i128::MAX
            && self.keys().contains(key as i128),
            !self.contains(key) ==> self.at(key) == 0,
            self.contains(key) <==> has_key(self.keys(), key),
    {}

    pub closed spec fn keys(self) -> Seq<i128> { self.keys@ }

    pub closed spec fn contains(self, key: int) -> bool {
        i128::MIN <= key <= i128::MAX && self.values@.contains_key(key as i128)
    }

    pub closed spec fn at(self, key: int) -> int {
        if self.contains(key) { self.values@[key as i128] as int } else { 0 }
    }

    pub closed spec fn wf(self) -> bool {
        &&& forall|key: i128| #[trigger] self.keys@.contains(key)
            <==> self.values@.contains_key(key)
        &&& forall|first: int, second: int| #![trigger self.keys@[first], self.keys@[second]]
            0 <= first < second < self.keys@.len() ==> self.keys@[first] != self.keys@[second]
    }

    pub fn new() -> (result: Self)
        ensures result.wf(), result.keys().len() == 0,
            forall|key: int| !#[trigger] result.contains(key),
            forall|key: int| #[trigger] result.at(key) == 0,
    { Sparse { values: HashMap::new(), keys: Vec::new() } }

    pub fn len(&self) -> (length: usize)
        ensures length == self.keys().len(),
    { self.keys.len() }

    pub fn key(&self, index: usize) -> (key: i128)
        requires index < self.keys().len(),
        ensures key == self.keys()[index as int],
    { self.keys[index] }

    pub fn get(&self, key: i128) -> (value: i128)
        ensures value as int == self.at(key as int),
    { match self.values.get(&key) { Some(value) => *value, None => 0 } }

    pub fn has(&self, key: i128) -> (result: bool)
        ensures result == self.contains(key as int),
    { self.values.contains_key(&key) }

    pub fn set(&mut self, key: i128, value: i128)
        requires old(self).wf(),
        ensures final(self).wf(), final(self).contains(key as int), final(self).at(key as int) == value as int,
            final(self).keys() == if old(self).contains(key as int) { old(self).keys() }
                else { old(self).keys().push(key) },
            forall|other: int| #[trigger] final(self).at(other)
                == if other == key as int { value as int } else { old(self).at(other) },
            forall|other: int| #[trigger] final(self).contains(other)
                == (other == key as int || old(self).contains(other)),
    {
        if !self.values.contains_key(&key) { self.keys.push(key); }
        self.values.insert(key, value);
        proof {
            crate::facts::lemma_contains_push(old(self).keys@, key);
            assert forall|candidate: i128| #[trigger] self.keys@.contains(candidate)
                <==> self.values@.contains_key(candidate) by {}
            assert forall|first: int, second: int| #![trigger self.keys@[first], self.keys@[second]]
                0 <= first < second < self.keys@.len() implies self.keys@[first] != self.keys@[second] by {
                if second == old(self).keys@.len() {
                    assert(self.keys@[second] == key);
                    assert(old(self).keys@.contains(self.keys@[first]));
                }
            }
        }
    }

    pub fn add(&mut self, key: i128, change: i128) -> (success: bool)
        requires old(self).wf(),
        ensures final(self).wf(), success ==> final(self).at(key as int) == old(self).at(key as int) + change,
            success ==> final(self).contains(key as int),
            final(self).keys() == if success && !old(self).contains(key as int) { old(self).keys().push(key) }
                else { old(self).keys() },
            forall|other: int| #[trigger] final(self).contains(other)
                == ((success && other == key as int) || old(self).contains(other)),
            forall|other: int| #[trigger] final(self).at(other)
                == if success && other == key as int { old(self).at(other) + change }
                    else { old(self).at(other) },
    {
        match self.get(key).checked_add(change) {
            Some(value) => { self.set(key, value); true },
            None => false,
        }
    }

    pub fn valid_counts(&self) -> (valid: bool)
        requires self.wf(),
        ensures valid ==> forall|key: int| #[trigger] self.at(key) >= 0 && self.at(key) <= i64::MAX,
    {
        let mut index = 0usize;
        while index < self.keys.len()
            invariant self.wf(), index <= self.keys.len(),
                forall|position: int| 0 <= position < index ==>
                    0 <= #[trigger] self.at(self.keys@[position] as int) <= i64::MAX,
            decreases self.keys.len() - index,
        {
            let value = self.get(self.keys[index]);
            if value < 0 || value > i64::MAX as i128 { return false; }
            index += 1;
        }
        proof {
            assert forall|key: int| #[trigger] self.at(key) >= 0 && self.at(key) <= i64::MAX by {
                self.lemma_support(key);
                if self.contains(key) {
                    let position = choose|position: int| 0 <= position < self.keys@.len()
                        && self.keys@[position] == key as i128;
                    assert(self.at(self.keys@[position] as int) == self.at(key));
                }
            }
        }
        true
    }
}

pub struct Layers {
    base: Arc<Sparse>,
    patches: Vec<Arc<Sparse>>,
}

impl Layers {
    proof fn lemma_prefix_same(self, other: Self, key: int, length: nat)
        requires length <= self.depth(), length <= other.depth(),
            *self.base == *other.base,
            self.patches@.take(length as int) == other.patches@.take(length as int),
        ensures self.prefix(key, length) == other.prefix(key, length),
        decreases length,
    {
        if length > 0 {
            assert(self.patches@.take(length as int)[length as int - 1] == self.patches@[length as int - 1]);
            assert(other.patches@.take(length as int)[length as int - 1] == other.patches@[length as int - 1]);
            assert(*self.patches@[length as int - 1] == *other.patches@[length as int - 1]);
            assert(self.patches@.take(length as int - 1) =~= other.patches@.take(length as int - 1));
            self.lemma_prefix_same(other, key, (length - 1) as nat);
        }
    }
    pub closed spec fn depth(self) -> nat { self.patches@.len() }

    pub closed spec fn wf(self) -> bool {
        &&& (*self.base).wf()
        &&& forall|index: int| 0 <= index < self.patches@.len() ==>
            #[trigger] (*self.patches@[index]).wf()
    }

    pub closed spec fn prefix(self, key: int, length: nat) -> int
        recommends length <= self.depth(),
        decreases length,
    {
        if length == 0 { (*self.base).at(key) }
        else if (*self.patches@[length as int - 1]).contains(key) {
            (*self.patches@[length as int - 1]).at(key)
        } else { self.prefix(key, (length - 1) as nat) }
    }

    pub closed spec fn at(self, key: int) -> int {
        self.prefix(key, self.patches@.len())
    }

    pub fn new(base: Sparse) -> (result: Self)
        requires base.wf(),
        ensures result.wf(), forall|key: int| #[trigger] result.at(key) == base.at(key),
    { Layers { base: Arc::new(base), patches: Vec::new() } }

    pub fn get(&self, key: i128) -> (value: i128)
        requires self.wf(),
        ensures value as int == self.at(key as int),
    {
        let mut length = self.patches.len();
        while length > 0
            invariant length <= self.patches.len(),
                self.prefix(key as int, length as nat) == self.at(key as int),
            decreases length,
        {
            length -= 1;
            let patch = &*self.patches[length];
            if patch.has(key) { return patch.get(key); }
        }
        self.base.get(key)
    }

    pub fn extend(&self, patch: Sparse) -> (result: Self)
        requires self.wf(), patch.wf(),
        ensures result.wf(), forall|key: int| #[trigger] result.at(key)
            == if patch.contains(key) { patch.at(key) } else { self.at(key) },
    {
        let mut patches = Vec::new();
        let mut index = 0usize;
        while index < self.patches.len()
            invariant index <= self.patches.len(), patches@ == self.patches@.take(index as int),
            decreases self.patches.len() - index,
        {
            patches.push(self.patches[index].clone());
            index += 1;
        }
        let ghost replacement = patch;
        if patch.len() > 0 { patches.push(Arc::new(patch)); }
        let result = Layers { base: self.base.clone(), patches };
        proof {
            assert forall|key: int| #[trigger] result.at(key)
                == if replacement.contains(key) { replacement.at(key) } else { self.at(key) } by {
                assert(result.patches@.take(self.depth() as int) =~= self.patches@);
                assert(self.patches@.take(self.depth() as int) =~= self.patches@);
                self.lemma_prefix_same(result, key, self.depth());
                if replacement.keys().len() == 0 { replacement.lemma_support(key); }
            }
        }
        result
    }

    pub fn materialize(&self) -> (result: Sparse)
        requires self.wf(),
        ensures result.wf(), forall|key: int| #[trigger] result.at(key) == self.at(key),
    {
        let mut result = Sparse::new();
        let base = &*self.base;
        let ghost base_model = *base;
        let mut index = 0usize;
        while index < self.base.len()
            invariant index <= (*self.base).keys().len(), result.wf(),
                self.wf(),
                base_model == *base, *base == *self.base,
                forall|key: int| #[trigger] result.at(key)
                    == if has_key((*self.base).keys().take(index as int), key) { (*self.base).at(key) } else { 0 },
            decreases (*self.base).keys().len() - index,
        {
            let key = self.base.key(index);
            result.set(key, self.base.get(key));
            proof { lemma_has_key_step(base_model.keys(), index as int); }
            index += 1;
        }
        proof {
            assert forall|key: int| #[trigger] result.at(key) == self.prefix(key, 0) by {
                assert(base_model.keys().take(index as int) =~= base_model.keys());
                base_model.lemma_support(key);
            }
        }
        let mut layer = 0usize;
        while layer < self.patches.len()
            invariant self.wf(), layer <= self.patches.len(), result.wf(),
                forall|key: int| #[trigger] result.at(key) == self.prefix(key, layer as nat),
            decreases self.patches.len() - layer,
        {
            let patch = &*self.patches[layer];
            let ghost patch_model = *patch;
            let ghost previous = result;
            index = 0;
            while index < patch.len()
                invariant self.wf(), (*patch).wf(), index <= (*patch).keys().len(), result.wf(),
                    patch_model == *patch,
                    layer < self.patches.len(), *patch == *self.patches@[layer as int],
                    forall|key: int| #[trigger] previous.at(key) == self.prefix(key, layer as nat),
                    forall|key: int| #[trigger] result.at(key)
                        == if has_key((*patch).keys().take(index as int), key) { (*patch).at(key) } else { previous.at(key) },
                decreases (*patch).keys().len() - index,
            {
                let key = patch.key(index);
                result.set(key, patch.get(key));
                proof { lemma_has_key_step(patch_model.keys(), index as int); }
                index += 1;
            }
            proof {
                assert(patch_model.keys().take(index as int) =~= patch_model.keys());
                assert forall|key: int| #[trigger] result.at(key) == self.prefix(key, layer as nat + 1) by {
                    patch_model.lemma_support(key);
                }
            }
            layer += 1;
        }
        result
    }
}

pub open spec fn has_key(keys: Seq<i128>, key: int) -> bool {
    i128::MIN <= key <= i128::MAX && keys.contains(key as i128)
}

pub proof fn lemma_has_key_step(keys: Seq<i128>, index: int)
    requires 0 <= index < keys.len(),
    ensures forall|key: int| #[trigger] has_key(keys.take(index + 1), key)
        == (has_key(keys.take(index), key) || key == keys[index] as int),
{
    assert(keys.take(index + 1) =~= keys.take(index).push(keys[index]));
    crate::facts::lemma_contains_push(keys.take(index), keys[index]);
}

}
