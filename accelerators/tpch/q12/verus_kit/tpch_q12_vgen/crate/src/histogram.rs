use vstd::prelude::*;
use std::collections::HashMap;
use std::sync::Arc;
use crate::bytes;
use crate::counts::{Counts, Tally};
use verus_common::sums::{sum, rng};

verus! {

broadcast use vstd::seq_lib::group_seq_properties;

pub proof fn lemma_set_len_i32(days: Set<i32>)
    ensures days.len() <= 4294967296,
{
    let span = vstd::set_lib::set_int_range(-2147483648, 2147483648);
    vstd::set_lib::lemma_int_range(-2147483648, 2147483648);
    let widen = |day: i32| day as int;
    let image = days.map(widen);
    assert(days.injective_on(widen));
    assert forall|value: int| image.contains(value) implies span.contains(value) by {
        broadcast use Set::lemma_map_contains;
    }
    vstd::set_lib::lemma_map_size(days, image, widen);
    vstd::set_lib::lemma_len_subset(image, span);
}

pub proof fn lemma_keys_len(keys: Seq<i32>)
    requires keys.no_duplicates(),
    ensures keys.len() <= 4294967296,
{
    keys.unique_seq_to_set();
    lemma_set_len_i32(keys.to_set());
}

pub proof fn lemma_tier_prefix(tiers: Seq<Arc<Sparse>>, index: int)
    requires 0 <= index <= tiers.len(),
    ensures sum(tiers.take(index), |tier: Arc<Sparse>| tier.keys@.len() as int)
        <= sum(tiers, |tier: Arc<Sparse>| tier.keys@.len() as int),
    decreases tiers.len() - index,
{
    let length = |tier: Arc<Sparse>| tier.keys@.len() as int;
    if index == tiers.len() {
        verus_common::sums::lemma_sum_take_full(tiers, length);
    } else {
        verus_common::sums::lemma_sum_take(tiers, index, length);
        lemma_tier_prefix(tiers, index + 1);
    }
}

pub struct Sparse {
    pub values: HashMap<i32, Tally>,
    pub keys: Vec<i32>,
}

impl Sparse {
    pub open spec fn at(self, day: i32) -> Counts {
        if self.values@.contains_key(day) { self.values@[day]@ }
        else { Counts { high: 0, low: 0 } }
    }

    pub open spec fn wf(self) -> bool {
        &&& self.keys@.no_duplicates()
        &&& self.keys@.to_set() == self.values@.dom()
    }

    pub open spec fn range_prefix(self, start: int, end: int, length: int, high: bool) -> int {
        sum(self.keys@.take(length), self.range_day_weight(start, end, high))
    }

    pub open spec fn range_day_weight(self, start: int, end: int, high: bool) -> spec_fn(i32) -> int {
        |day: i32| if start <= day < end {
            if high { self.at(day).high as int } else { self.at(day).low as int }
        } else { 0 }
    }

    pub fn new() -> (out: Self)
        ensures out.wf(), out.keys@.len() == 0,
    {
        let out = Sparse { values: HashMap::new(), keys: Vec::new() };
        proof {
            out.keys@.to_set_ensures();
            assert(out.keys@.no_duplicates());
            assert(out.keys@.to_set() =~= out.values@.dom());
        }
        out
    }

    pub fn get(&self, day: i32) -> (out: Tally)
        ensures out@ == self.at(day),
    {
        match self.values.get(&day) { Some(value) => value.dup(), None => Tally::zero() }
    }

    pub fn put(&mut self, day: i32, value: Tally)
        requires old(self).wf(),
        ensures final(self).wf(), final(self).at(day) == value@,
            final(self).values@ == old(self).values@.insert(day, value),
            forall|other: i32| #![trigger final(self).at(other)]
                other != day ==> final(self).at(other) == old(self).at(other),
    {
        let ghost before = *self;
        proof { self.keys@.to_set_ensures(); }
        if !self.values.contains_key(&day) {
            proof { verus_prooflib::lemma_unique_push(self.keys@, day); }
            self.keys.push(day);
        }
        self.values.insert(day, value);
        proof {
            self.keys@.to_set_ensures();
            assert(self.values@ == before.values@.insert(day, value));
            assert(self.keys@ == if before.values@.contains_key(day) { before.keys@ } else { before.keys@.push(day) });
            assert(self.keys@.to_set() =~= self.values@.dom());
        }
    }

    pub fn add(&mut self, day: i32, value: &Tally)
        requires old(self).wf(),
        ensures final(self).wf(),
            final(self).at(day).high == old(self).at(day).high + value@.high
                && final(self).at(day).low == old(self).at(day).low + value@.low,
            forall|other: i32| #![trigger final(self).at(other)]
                other != day ==> final(self).at(other) == old(self).at(other),
    {
        let next = self.get(day).add(value);
        self.put(day, next);
    }

    pub fn absorb_older(&mut self, older: &Sparse)
        requires old(self).wf(), older.wf(),
        ensures final(self).wf(),
            final(self).values@.dom() == old(self).values@.dom().union(older.values@.dom()),
            forall|day: i32| #![trigger final(self).at(day)]
                final(self).at(day) == if old(self).values@.contains_key(day) {
                    old(self).at(day)
                } else { older.at(day) },
    {
        let ghost before = *self;
        proof { older.keys@.to_set_ensures(); older.keys@.take(0).to_set_ensures(); }
        let mut index = 0;
        while index < older.keys.len()
            invariant index <= older.keys@.len(), self.wf(), older.wf(), before == *old(self),
                self.values@.dom() == before.values@.dom().union(older.keys@.take(index as int).to_set()),
                forall|day: i32| #![trigger self.at(day)] self.at(day) ==
                    if before.values@.contains_key(day) { before.at(day) }
                    else if older.keys@.take(index as int).contains(day) { older.at(day) }
                    else { Counts { high: 0, low: 0 } },
            decreases older.keys@.len() - index,
        {
            let day = older.keys[index];
            if !self.values.contains_key(&day) { self.put(day, older.get(day)); }
            proof {
                older.keys@.take(index as int + 1).to_set_ensures();
                older.keys@.take(index as int).to_set_ensures();
                assert(older.keys@.take(index as int + 1) =~= older.keys@.take(index as int).push(day));
                assert(self.values@.dom() =~= before.values@.dom().union(older.keys@.take(index as int + 1).to_set()));
            }
            index += 1;
        }
        proof { assert(older.keys@.take(index as int) =~= older.keys@); }
    }
}

pub struct Layers {
    pub base: Arc<Sparse>,
    pub tiers: Vec<Arc<Sparse>>,
    pub live: usize,
    pub physical: usize,
}

impl Layers {
    pub open spec fn structure_wf(self) -> bool {
        self.base.wf() && forall|index: int| #![trigger self.tiers@[index]]
            0 <= index < self.tiers@.len() ==> self.tiers@[index].wf()
    }
    pub open spec fn through(self, day: i32, depth: nat) -> Counts
        recommends depth <= self.tiers@.len(),
        decreases depth,
    {
        if depth == 0 { self.base.at(day) }
        else if self.tiers@[depth as int - 1].values@.contains_key(day) {
            self.tiers@[depth as int - 1].at(day)
        } else { self.through(day, (depth - 1) as nat) }
    }

    #[verifier::opaque]
    pub open spec fn at(self, day: i32) -> Counts {
        self.through(day, self.tiers@.len())
    }

    pub open spec fn range_count(self, start: int, end: int, high: bool) -> int {
        sum(rng(if end > start { end - start } else { 0 }), self.range_weight(start, high))
    }

    pub open spec fn range_weight(self, start: int, high: bool) -> spec_fn(int) -> int {
        |offset: int| {
            let day = start + offset;
            if i32::MIN <= day <= i32::MAX {
                if high { self.at(day as i32).high as int } else { self.at(day as i32).low as int }
            } else { 0 }
        }
    }

    pub open spec fn physical_support(self, depth: nat) -> Set<i32>
        recommends depth <= self.tiers@.len(),
        decreases depth,
    {
        if depth == 0 { self.base.keys@.to_set() }
        else { self.physical_support((depth - 1) as nat)
            .union(self.tiers@[depth as int - 1].keys@.to_set()) }
    }

    pub open spec fn support(self) -> Set<i32> {
        self.physical_support(self.tiers@.len()).filter(|day: i32| self.at(day).positive())
    }

    #[verifier::opaque]
    pub open spec fn wf(self) -> bool {
        &&& self.base.wf()
        &&& forall|index: int| #![trigger self.tiers@[index]]
            0 <= index < self.tiers@.len() ==> self.tiers@[index].wf()
        &&& forall|day: i32| #![trigger self.at(day)] self.at(day).nonnegative()
        &&& self.live == self.support().len()
        &&& self.physical == self.base.keys@.len()
            + sum(self.tiers@, |tier: Arc<Sparse>| tier.keys@.len() as int)
        &&& self.physical <= 4 * self.live
        &&& forall|index: int| #![trigger self.tiers@[index]]
            0 <= index < self.tiers@.len() - 1 ==>
                self.tiers@[index].keys@.len() > 2 * self.tiers@[index + 1].keys@.len()
    }

    pub proof fn lemma_zero_outside(self, day: i32, depth: nat)
        requires self.structure_wf(), depth <= self.tiers@.len(),
            !self.physical_support(depth).contains(day),
        ensures self.through(day, depth).high == 0, self.through(day, depth).low == 0,
        decreases depth,
    {
        if depth == 0 {
            self.base.keys@.to_set_ensures();
        } else {
            self.tiers@[depth as int - 1].keys@.to_set_ensures();
            self.lemma_zero_outside(day, (depth - 1) as nat);
        }
    }

    pub proof fn lemma_support(self, day: i32)
        requires self.structure_wf(),
        ensures self.support().contains(day) == self.at(day).positive(),
    {
        reveal(Layers::at);
        if !self.physical_support(self.tiers@.len()).contains(day) {
            self.lemma_zero_outside(day, self.tiers@.len());
        }
    }

    pub proof fn lemma_empty(self, day: i32)
        requires self.wf(), self.live == 0,
        ensures self.at(day).high == 0, self.at(day).low == 0,
    {
        reveal(Layers::wf);
        self.lemma_support(day);
        assert(self.support() =~= Set::empty());
    }

    pub proof fn lemma_wf(self)
        requires self.wf(),
        ensures self.structure_wf(), self.live == self.support().len(), self.physical <= 4 * self.live,
            self.physical as int == self.base.keys@.len() as int + sum(self.tiers@, |tier: Arc<Sparse>| tier.keys@.len() as int),
            forall|day: i32| #![trigger self.at(day)] self.at(day).nonnegative(),
            forall|index: int| #![trigger self.tiers@[index]] 0 <= index < self.tiers@.len() - 1 ==>
                self.tiers@[index].keys@.len() > 2 * self.tiers@[index + 1].keys@.len(),
    {
        reveal(Layers::wf);
    }

    pub proof fn lemma_overlay(self, patch: Sparse, day: i32)
        ensures self.overlay(patch, self.tiers@.len(), day) == if patch.values@.contains_key(day) { patch.at(day) } else { self.at(day) },
    {
        reveal(Layers::at);
    }

    pub proof fn lemma_prefix(self, other: Layers, depth: nat, day: i32)
        requires depth <= self.tiers@.len(), depth <= other.tiers@.len(), self.base == other.base,
            forall|index: int| #![trigger self.tiers@[index], other.tiers@[index]]
                0 <= index < depth ==> self.tiers@[index] == other.tiers@[index],
        ensures self.through(day, depth) == other.through(day, depth),
        decreases depth,
    {
        if depth > 0 { self.lemma_prefix(other, (depth - 1) as nat, day); }
    }

    pub open spec fn support_change(self, patch: Sparse) -> spec_fn(i32) -> int {
        |day: i32| (if patch.at(day).positive() { 1int } else { 0int })
            - (if self.at(day).positive() { 1int } else { 0int })
    }

    pub open spec fn overlay(self, patch: Sparse, depth: nat, day: i32) -> Counts {
        if patch.values@.contains_key(day) { patch.at(day) } else { self.through(day, depth) }
    }

    pub proof fn lemma_absorb_step(self, previous: Sparse, next: Sparse, depth: nat, day: i32)
        requires depth < self.tiers@.len(),
            next.values@.dom() == previous.values@.dom().union(self.tiers@[depth as int].values@.dom()),
            next.at(day) == if previous.values@.contains_key(day) { previous.at(day) } else { self.tiers@[depth as int].at(day) },
        ensures self.overlay(next, depth, day) == self.overlay(previous, depth + 1, day),
    {
    }

    pub fn get(&self, day: i32) -> (out: Tally)
        requires self.wf(),
        ensures out@ == self.at(day), out@.nonnegative(),
    {
        proof { reveal(Layers::at); self.lemma_wf(); }
        let mut depth = self.tiers.len();
        while depth > 0
            invariant depth <= self.tiers@.len(), self.at(day) == self.through(day, depth as nat), self.at(day).nonnegative(),
            decreases depth,
        {
            depth -= 1;
            let tier = &*self.tiers[depth];
            match tier.values.get(&day) {
                Some(value) => { proof { assert(value@ == tier.at(day)); } return value.dup(); },
                None => {}
            }
        }
        self.base.get(day)
    }

    pub fn from_base(base: Sparse) -> (out: Self)
        requires base.wf(),
            forall|day: i32| #![trigger base.values@[day]]
                base.values@.contains_key(day) ==> base.at(day).positive(),
        ensures out.wf(),
            forall|day: i32| #![trigger out.at(day)] out.at(day) == base.at(day),
    {
        proof { reveal(Layers::wf); }
        let size = base.keys.len();
        let out = Layers { base: Arc::new(base), tiers: Vec::new(), live: size, physical: size };
        proof {
            reveal(Layers::at);
            out.base.keys@.to_set_ensures();
            assert forall|day: i32| #![trigger out.at(day)] out.at(day) == out.base.at(day) by {}
            assert(out.support() =~= out.base.keys@.to_set());
            let empty = Set::<i32>::empty();
            let present = |day: i32| true;
            verus_prooflib::lemma_replacement_support(empty, out.base.keys@, present);
            verus_prooflib::lemma_sum_ones(out.base.keys@);
            verus_common::sums::lemma_sum_ext_all(out.base.keys@,
                |day: i32| (if present(day) { 1int } else { 0int }) - (if empty.contains(day) { 1int } else { 0int }),
                |day: i32| 1int);
            assert(empty.difference(out.base.keys@.to_set()).union(out.base.keys@.to_set().filter(present)) =~= out.base.keys@.to_set());
            assert(out.live == out.support().len());
            assert(out.physical == out.base.keys@.len() + sum(out.tiers@, |tier: Arc<Sparse>| tier.keys@.len() as int));
            assert forall|day: i32| #![trigger out.at(day)] out.at(day).nonnegative() by {}
            assert(out.wf());
        }
        out
    }

    pub fn materialize(&self) -> (out: Sparse)
        requires self.structure_wf(),
        ensures out.wf(),
            out.values@.dom() == self.physical_support(self.tiers@.len()),
            forall|day: i32| #![trigger out.at(day)] out.at(day) == self.at(day),
    {
        proof { reveal(Layers::at); }
        let mut out = Sparse::new();
        let mut depth = self.tiers.len();
        while depth > 0
            invariant depth <= self.tiers@.len(), out.wf(), self.structure_wf(),
                out.values@.dom().union(self.physical_support(depth as nat)) == self.physical_support(self.tiers@.len()),
                forall|day: i32| #![trigger self.at(day)]
                    if out.values@.contains_key(day) { out.at(day) == self.at(day) }
                    else { self.at(day) == self.through(day, depth as nat) },
            decreases depth,
        {
            let ghost prior = out;
            depth -= 1;
            out.absorb_older(&*self.tiers[depth]);
            proof {
                self.tiers@[depth as int].keys@.to_set_ensures();
                assert forall|day: i32| #![trigger self.at(day)]
                    if out.values@.contains_key(day) { out.at(day) == self.at(day) }
                    else { self.at(day) == self.through(day, depth as nat) } by {
                    if !prior.values@.contains_key(day) {
                        assert(self.at(day) == self.through(day, depth as nat + 1));
                    }
                }
                assert(out.values@.dom().union(self.physical_support(depth as nat)) =~= self.physical_support(self.tiers@.len()));
            }
        }
        out.absorb_older(&*self.base);
        out
    }

    pub proof fn lemma_sparse_range(self, full: Sparse, start: i32, end: i32, high: bool)
        requires full.wf(),
            forall|day: i32| #![trigger full.at(day)] full.at(day) == self.at(day),
        ensures self.range_count(start as int, end as int, high)
            == full.range_prefix(start as int, end as int, full.keys@.len() as int, high),
    {
        use verus_common::sums::*;
        let length = if end > start { end as int - start as int } else { 0 };
        let offsets = rng(length);
        let cross = |offset: int, day: i32| if start as int + offset == day as int {
            if high { full.at(day).high as int } else { full.at(day).low as int }
        } else { 0 };
        let by_offset = |offset: int| sum(full.keys@, |day: i32| cross(offset, day));
        let by_day = |day: i32| sum(offsets, |offset: int| cross(offset, day));
        lemma_sum_swap(offsets, full.keys@, cross, by_offset, by_day);
        assert forall|index: int| #![trigger offsets[index]] 0 <= index < offsets.len() implies
            by_offset(offsets[index]) == self.range_weight(start as int, high)(offsets[index]) by {
            let offset = offsets[index];
            let day = (start as int + offset) as i32;
            assert(start as int + offset == day as int);
            assert(full.at(day) == self.at(day));
            full.keys@.to_set_ensures();
            if full.keys@.contains(day) {
                let found = choose|found: int| #![trigger full.keys@[found]]
                    0 <= found < full.keys@.len() && full.keys@[found] == day;
                assert forall|position: int| #![trigger full.keys@[position]]
                    0 <= position < full.keys@.len() && position != found implies
                        cross(offset, full.keys@[position]) == 0 by {}
                lemma_sum_single_match(full.keys@, |date: i32| cross(offset, date), found);
            } else {
                lemma_sum_zero(full.keys@, |date: i32| cross(offset, date));
            }
        }
        lemma_sum_ext(offsets, by_offset, self.range_weight(start as int, high));
        let output = full.range_day_weight(start as int, end as int, high);
        assert forall|day: i32| #[trigger] by_day(day) == output(day) by {
            let value = if high { full.at(day).high as int } else { full.at(day).low as int };
            lemma_sum_ext_all(offsets, |offset: int| cross(offset, day),
                |offset: int| if start as int + offset == day as int { value } else { 0 });
            verus_prooflib::lemma_range_single_cell(length, day as int, start as int, value);
        }
        lemma_sum_ext_all(full.keys@, by_day, output);
        assert(full.keys@.take(full.keys@.len() as int) =~= full.keys@);
    }

    #[verifier::rlimit(60)]
    pub fn replace(&self, patch: Sparse) -> (next: Self)
        requires self.wf(), patch.wf(),
            forall|day: i32| #![trigger patch.values@[day]]
                patch.values@.contains_key(day) ==> patch.at(day).nonnegative(),
        ensures next.wf()
            && forall|day: i32| #![trigger next.at(day)]
                next.at(day) == if patch.values@.contains_key(day) { patch.at(day) } else { self.at(day) },
    {
        proof { reveal(Layers::wf); }
        let ghost replacements = patch;
        let mut live_change = 0i128;
        let mut index = 0;
        while index < patch.keys.len()
            invariant index <= patch.keys@.len(), self.wf(), patch.wf(), replacements == patch,
                forall|day: i32| #![trigger patch.values@[day]] patch.values@.contains_key(day) ==> patch.at(day).nonnegative(),
                live_change as int == sum(patch.keys@.take(index as int), self.support_change(patch)),
                -(index as int) <= live_change as int <= index as int,
            decreases patch.keys@.len() - index,
        {
            let day = patch.keys[index];
            let before = self.get(day);
            let after = patch.get(day);
            proof {
                patch.keys@.to_set_ensures();
                verus_common::sums::lemma_sum_take(patch.keys@, index as int, self.support_change(patch));
                assert(before@.nonnegative());
                assert(after@.nonnegative());
            }
            if before.is_zero() && !after.is_zero() {
                live_change += 1;
            } else if !before.is_zero() && after.is_zero() {
                live_change -= 1;
            }
            index += 1;
        }
        proof {
            assert(patch.keys@.take(index as int) =~= patch.keys@);
            patch.keys@.to_set_ensures();
            self.lemma_wf();
            let present = |day: i32| replacements.at(day).positive();
            verus_prooflib::lemma_replacement_support(self.support(), replacements.keys@, present);
            assert forall|day: i32| #[trigger] self.support_change(replacements)(day)
                == (if present(day) { 1int } else { 0int }) - (if self.support().contains(day) { 1int } else { 0int }) by {
                self.lemma_support(day);
            }
            verus_common::sums::lemma_sum_ext_all(replacements.keys@, self.support_change(replacements),
                |day: i32| (if present(day) { 1int } else { 0int })
                    - (if self.support().contains(day) { 1int } else { 0int }));
            lemma_set_len_i32(self.support().difference(replacements.keys@.to_set())
                .union(replacements.keys@.to_set().filter(present)));
            lemma_set_len_i32(self.support());
        }
        proof {
            let after = self.support().difference(replacements.keys@.to_set()).union(replacements.keys@.to_set().filter(|day: i32| replacements.at(day).positive()));
            assert(patch == replacements);
            assert(live_change as int == sum(replacements.keys@, self.support_change(replacements)));
            assert(self.live == self.support().len());
            assert(self.live as int + live_change as int == after.len() as int);
            assert(0 <= self.live as int + live_change as int <= 4294967296);
        }
        let total = self.live as i128 + live_change;
        proof { assert(total >= 0 && total <= 4294967296); }
        let live = total as usize;
        proof {
            assert(live as int == self.live as int + sum(replacements.keys@, self.support_change(replacements)));
        }
        let mut depth = self.tiers.len();
        let mut merged = patch;
        while depth > 0 && 2 * merged.keys.len() as u128 >= self.tiers[depth - 1].keys.len() as u128
            invariant depth <= self.tiers@.len(), merged.wf(), self.wf(), self.structure_wf(), replacements.wf(),
                live as int == self.live as int + sum(replacements.keys@, self.support_change(replacements)),
                forall|day: i32| #![trigger replacements.values@[day]]
                    replacements.values@.contains_key(day) ==> replacements.at(day).nonnegative(),
                forall|day: i32| #![trigger self.overlay(merged, depth as nat, day)] #![trigger self.overlay(replacements, self.tiers@.len(), day)]
                    self.overlay(merged, depth as nat, day) == self.overlay(replacements, self.tiers@.len(), day),
            decreases depth,
        {
            let ghost previous = merged;
            let ghost previous_depth = depth as nat;
            proof {
                assert(previous == merged);
                assert forall|day: i32| #![trigger self.overlay(previous, previous_depth, day)]
                    self.overlay(previous, previous_depth, day) == self.overlay(replacements, self.tiers@.len(), day) by {}
            }
            depth -= 1;
            merged.absorb_older(&*self.tiers[depth]);
            proof {
                assert forall|day: i32| #![trigger self.overlay(merged, depth as nat, day)]
                    self.overlay(merged, depth as nat, day) == self.overlay(replacements, self.tiers@.len(), day) by {
                    self.lemma_absorb_step(previous, merged, depth as nat, day);
                }
            }
        }
        proof {
            self.lemma_wf();
            lemma_set_len_i32(self.support());
            lemma_tier_prefix(self.tiers@, 0);
            assert(self.tiers@.take(0) =~= Seq::<Arc<Sparse>>::empty());
        }
        let mut tiers = Vec::new();
        let mut physical = self.base.keys.len();
        index = 0;
        while index < depth
            invariant index <= depth, depth <= self.tiers@.len(), self.wf(), merged.wf(), replacements.wf(),
                tiers@ == self.tiers@.take(index as int),
                physical as int == self.base.keys@.len() as int
                    + sum(self.tiers@.take(index as int), |tier: Arc<Sparse>| tier.keys@.len() as int),
                physical <= self.physical, self.physical <= 4 * self.live, self.live <= 4294967296,
                live as int == self.live as int + sum(replacements.keys@, self.support_change(replacements)),
                forall|day: i32| #![trigger replacements.values@[day]]
                    replacements.values@.contains_key(day) ==> replacements.at(day).nonnegative(),
                forall|day: i32| #![trigger self.overlay(merged, depth as nat, day)] #![trigger self.overlay(replacements, self.tiers@.len(), day)]
                    self.overlay(merged, depth as nat, day) == self.overlay(replacements, self.tiers@.len(), day),
                depth > 0 ==> self.tiers@[depth as int - 1].keys@.len() > 2 * merged.keys@.len(),
            decreases depth - index,
        {
            proof {
                self.lemma_wf();
                verus_common::sums::lemma_sum_take(self.tiers@, index as int, |tier: Arc<Sparse>| tier.keys@.len() as int);
                lemma_tier_prefix(self.tiers@, index as int + 1);
            }
            physical = physical + self.tiers[index].keys.len();
            tiers.push(self.tiers[index].clone());
            proof {
                assert(tiers@ =~= self.tiers@.take(index as int + 1));
            }
            index += 1;
        }
        proof {
            assert(merged.keys@.no_duplicates());
            lemma_keys_len(merged.keys@);
            assert(merged.keys.len() <= 4294967296);
            assert(physical <= self.physical);
            assert(self.physical <= 4 * self.live);
            assert(self.live <= 4294967296);
        }
        physical = physical + merged.keys.len();
        let ghost latest = merged;
        if merged.keys.len() > 0 {
            let newest = Arc::new(merged);
            proof { verus_common::sums::lemma_sum_push(tiers@, newest, |tier: Arc<Sparse>| tier.keys@.len() as int); }
            tiers.push(newest);
        }
        let next = Layers { base: self.base.clone(), tiers, live, physical };
        proof {
            assert(next.structure_wf());
            reveal(Layers::at);
            self.lemma_wf();
            assert forall|day: i32| #![trigger next.at(day)] next.at(day)
                == if replacements.values@.contains_key(day) { replacements.at(day) } else { self.at(day) } by {
                next.lemma_prefix(*self, depth as nat, day);
                if latest.keys@.len() == 0 { latest.keys@.to_set_ensures(); }
                assert(next.at(day) == self.overlay(latest, depth as nat, day));
                assert(self.overlay(latest, depth as nat, day) == self.overlay(replacements, self.tiers@.len(), day));
            }
            assert forall|day: i32| #![trigger next.at(day)] next.at(day).nonnegative() by {}
            assert(next.support() =~= self.support().difference(replacements.values@.dom())
                .union(replacements.values@.dom().filter(|day: i32| replacements.at(day).positive()))) by {
                assert forall|day: i32| #![trigger next.support().contains(day)]
                    next.support().contains(day) == self.support().difference(replacements.values@.dom())
                        .union(replacements.values@.dom().filter(|day: i32| replacements.at(day).positive())).contains(day) by {
                    next.lemma_support(day);
                    self.lemma_support(day);
                }
            }
            replacements.keys@.to_set_ensures();
            let before_support = self.support();
            let present = |day: i32| replacements.at(day).positive();
            verus_prooflib::lemma_replacement_support(before_support, replacements.keys@, present);
            assert forall|day: i32| #![trigger self.support().contains(day)]
                self.support().contains(day) == self.at(day).positive() by { self.lemma_support(day); }
            verus_common::sums::lemma_sum_ext_all(replacements.keys@, self.support_change(replacements),
                |day: i32| (if present(day) { 1int } else { 0int })
                    - (if before_support.contains(day) { 1int } else { 0int }));
            assert(next.live == next.support().len());
            assert(next.physical as int == next.base.keys@.len() as int
                + sum(next.tiers@, |tier: Arc<Sparse>| tier.keys@.len() as int));
            assert forall|offset: int| #![trigger next.tiers@[offset]]
                0 <= offset < next.tiers@.len() - 1 implies
                    next.tiers@[offset].keys@.len() > 2 * next.tiers@[offset + 1].keys@.len() by {}
        }
        if physical as u128 > 4 * live as u128 {
            let full = next.materialize();
            let mut clean = Sparse::new();
            index = 0;
            while index < full.keys.len()
                invariant index <= full.keys@.len(), clean.wf(), full.wf(), next.structure_wf(),
                    forall|day: i32| #![trigger full.at(day)] full.at(day) == next.at(day) && full.at(day).nonnegative(),
                    forall|day: i32| #![trigger clean.at(day)] clean.at(day) == if clean.values@.contains_key(day) { full.at(day) } else { Counts { high: 0, low: 0 } },
                    forall|day: i32| #![trigger clean.values@[day]] clean.values@.contains_key(day) ==> clean.at(day).positive(),
                    clean.values@.dom() == full.keys@.take(index as int).to_set().filter(|day: i32| full.at(day).positive()),
                decreases full.keys@.len() - index,
            {
                let day = full.keys[index];
                let value = full.get(day);
                if !value.is_zero() { clean.put(day, value); }
                proof {
                    full.keys@.take(index as int).to_set_ensures();
                    full.keys@.take(index as int + 1).to_set_ensures();
                    assert(full.keys@.take(index as int + 1) =~= full.keys@.take(index as int).push(day));
                    assert(clean.values@.dom() =~= full.keys@.take(index as int + 1).to_set().filter(|date: i32| full.at(date).positive()));
                }
                index += 1;
            }
            proof {
                full.keys@.to_set_ensures();
                assert(full.keys@.take(index as int) =~= full.keys@);
                assert forall|day: i32| #![trigger clean.at(day)] clean.at(day) == next.at(day) by {
                    assert(full.at(day) == next.at(day));
                }
            }
            Layers::from_base(clean)
        } else { next }
    }

    pub fn range(&self, start: i32, end: i32) -> (total: Tally)
        requires self.wf(),
        ensures total@.nonnegative()
            && total@.high == self.range_count(start as int, end as int, true)
            && total@.low == self.range_count(start as int, end as int, false),
    {
        proof { reveal(Layers::wf); }
        let mut total = Tally::zero();
        if end <= start { return total; }
        let width = end as i64 - start as i64;
        if width as u128 <= self.physical as u128 / (self.tiers.len() as u128 + 1) {
            let mut day = start as i64;
            while day < end as i64
                invariant start as i64 <= day <= end as i64, total@.nonnegative(), self.wf(),
                    total@.high == self.range_count(start as int, day as int, true),
                    total@.low == self.range_count(start as int, day as int, false),
                decreases end as i64 - day,
            {
                proof {
                    self.lemma_wf();
                    verus_prooflib::lemma_range_step(day as int - start as int, self.range_weight(start as int, true));
                    verus_prooflib::lemma_range_step(day as int - start as int, self.range_weight(start as int, false));
                }
                total = total.add(&self.get(day as i32));
                day += 1;
            }
        } else {
            let full = self.materialize();
            proof {
                self.lemma_sparse_range(full, start, end, true);
                self.lemma_sparse_range(full, start, end, false);
            }
            let mut index = 0;
            while index < full.keys.len()
                invariant index <= full.keys@.len(), total@.nonnegative(), self.wf(), full.wf(),
                    forall|day: i32| #![trigger full.at(day)] full.at(day) == self.at(day),
                    self.range_count(start as int, end as int, true) == full.range_prefix(start as int, end as int, full.keys@.len() as int, true),
                    self.range_count(start as int, end as int, false) == full.range_prefix(start as int, end as int, full.keys@.len() as int, false),
                    total@.high == full.range_prefix(start as int, end as int, index as int, true),
                    total@.low == full.range_prefix(start as int, end as int, index as int, false),
                decreases full.keys@.len() - index,
            {
                let day = full.keys[index];
                proof {
                    self.lemma_wf();
                    verus_common::sums::lemma_sum_take(full.keys@, index as int, full.range_day_weight(start as int, end as int, true));
                    verus_common::sums::lemma_sum_take(full.keys@, index as int, full.range_day_weight(start as int, end as int, false));
                }
                if start <= day && day < end {
                    let value = full.get(day);
                    total = total.add(&value);
                }
                index += 1;
            }
        }
        total
    }
}

pub struct ModeEntry {
    pub mode: Arc<Vec<u8>>,
    pub days: Arc<Layers>,
}

pub struct Directory {
    pub buckets: HashMap<u64, Arc<Vec<ModeEntry>>>,
    pub hashes: Vec<u64>,
}

fn remove_hash(hashes: &mut Vec<u64>, code: u64)
    requires old(hashes)@.no_duplicates(), old(hashes)@.contains(code),
    ensures final(hashes)@.no_duplicates(), final(hashes)@.to_set() == old(hashes)@.to_set().remove(code),
{
    let ghost before = hashes@;
    let mut position = 0;
    while position < hashes.len() && hashes[position] != code
        invariant position <= hashes@.len(), hashes@ == before, before == old(hashes)@,
            before.no_duplicates(), before.contains(code),
            forall|offset: int| #![trigger before[offset]] 0 <= offset < position ==> before[offset] != code,
        decreases hashes@.len() - position,
    {
        position += 1;
    }
    proof { assert(position < hashes@.len()); }
    hashes.remove(position);
    proof {
        hashes@.to_set_ensures();
        before.to_set_ensures();
        assert(hashes@.to_set() =~= before.to_set().remove(code));
    }
}

impl Directory {
    pub open spec fn range_count(self, mode: Seq<u8>, start: int, end: int, high: bool) -> int {
        sum(rng(if end > start { end - start } else { 0 }), self.range_weight(mode, start, high))
    }

    pub open spec fn range_weight(self, mode: Seq<u8>, start: int, high: bool) -> spec_fn(int) -> int {
        |offset: int| {
            let day = start + offset;
            if i32::MIN <= day <= i32::MAX {
                if high { self.at(mode, day as i32).high as int } else { self.at(mode, day as i32).low as int }
            } else { 0 }
        }
    }

    pub proof fn lemma_range_mode(self, mode: Seq<u8>, start: int, end: int, days: Layers, high: bool)
        requires forall|day: i32| #![trigger days.at(day)] days.at(day) == self.at(mode, day),
        ensures self.range_count(mode, start, end, high) == days.range_count(start, end, high),
    {
        verus_common::sums::lemma_sum_ext_all(rng(if end > start { end - start } else { 0 }),
            self.range_weight(mode, start, high), days.range_weight(start, high));
    }

    pub proof fn lemma_range_missing(self, mode: Seq<u8>, start: int, end: int, high: bool)
        requires forall|day: i32| #![trigger self.at(mode, day)] self.at(mode, day).high == 0 && self.at(mode, day).low == 0,
        ensures self.range_count(mode, start, end, high) == 0,
    {
        verus_common::sums::lemma_sum_zero(rng(if end > start { end - start } else { 0 }), self.range_weight(mode, start, high));
    }

    #[verifier::opaque]
    pub open spec fn at(self, mode: Seq<u8>, day: i32) -> Counts {
        let code = bytes::hash_bytes(mode);
        if self.buckets@.contains_key(code) {
            let bucket = self.buckets@[code];
            if exists|index: int| #![trigger bucket@[index]]
                0 <= index < bucket@.len() && bucket@[index].mode@ == mode {
                let index = choose|index: int| #![trigger bucket@[index]]
                    0 <= index < bucket@.len() && bucket@[index].mode@ == mode;
                bucket@[index].days.at(day)
            } else { Counts { high: 0, low: 0 } }
        } else { Counts { high: 0, low: 0 } }
    }

    pub proof fn lemma_index(self, code: u64, index: int, day: i32)
        requires self.wf(), self.buckets@.contains_key(code),
            0 <= index < self.buckets@[code]@.len(),
        ensures self.at(self.buckets@[code]@[index].mode@, day) == self.buckets@[code]@[index].days.at(day),
    {
        reveal(Directory::at);
        let bucket = self.buckets@[code];
        let mode = bucket@[index].mode@;
        assert(bytes::hash_bytes(mode) == code);
        assert(exists|offset: int| #![trigger bucket@[offset]]
            0 <= offset < bucket@.len() && bucket@[offset].mode@ == mode);
        let found = choose|offset: int| #![trigger bucket@[offset]]
            0 <= offset < bucket@.len() && bucket@[offset].mode@ == mode;
        if found < index { assert(bucket@[found].mode@ != bucket@[index].mode@); }
        if index < found { assert(bucket@[index].mode@ != bucket@[found].mode@); }
        assert(found == index);
    }

    pub open spec fn wf(self) -> bool {
        &&& self.hashes@.no_duplicates()
        &&& self.hashes@.to_set() == self.buckets@.dom()
        &&& forall|code: u64| #![trigger self.buckets@[code]]
            self.buckets@.contains_key(code) ==> {
                let bucket = self.buckets@[code];
                &&& bucket@.len() > 0
                &&& forall|index: int| #![trigger bucket@[index]]
                    0 <= index < bucket@.len() ==> {
                        &&& bytes::hash_bytes(bucket@[index].mode@) == code
                        &&& bucket@[index].days.wf()
                        &&& bucket@[index].days.live > 0
                    }
                &&& forall|left: int, right: int| #![trigger bucket@[left], bucket@[right]]
                    0 <= left < right < bucket@.len() ==>
                        bucket@[left].mode@ != bucket@[right].mode@
            }
    }

    pub fn new() -> (out: Self)
        ensures out.wf(), out.hashes@.len() == 0,
            forall|mode: Seq<u8>, day: i32| #![trigger out.at(mode, day)]
                out.at(mode, day).high == 0 && out.at(mode, day).low == 0,
    {
        let out = Directory { buckets: HashMap::new(), hashes: Vec::new() };
        proof {
            reveal(Directory::at);
            out.hashes@.to_set_ensures();
            assert(out.hashes@.no_duplicates());
            assert(out.hashes@.to_set() =~= out.buckets@.dom());
        }
        out
    }

    pub fn find(&self, mode: &[u8]) -> (out: Option<Arc<Layers>>)
        requires self.wf(),
        ensures out matches Some(days) ==> days.wf()
            && forall|day: i32| #![trigger days.at(day)] days.at(day) == self.at(mode@, day),
            out.is_none() ==> forall|day: i32| #![trigger self.at(mode@, day)]
                self.at(mode@, day).high == 0 && self.at(mode@, day).low == 0,
    {
        let code = bytes::hash(mode);
        let bucket = match self.buckets.get(&code) {
            Some(bucket) => bucket,
            None => { proof { reveal(Directory::at); } return None; },
        };
        let mut index = 0;
        while index < bucket.len()
            invariant index <= bucket@.len(), self.wf(), self.buckets@.contains_key(code),
                bucket@ == self.buckets@[code]@, code == bytes::hash_bytes(mode@),
                forall|offset: int| #![trigger bucket@[offset]]
                    0 <= offset < index ==> bucket@[offset].mode@ != mode@,
            decreases bucket@.len() - index,
        {
            if bytes::equal(&bucket[index].mode, mode) {
                proof {
                    assert forall|day: i32| #![trigger self.at(mode@, day)]
                        self.at(mode@, day) == bucket@[index as int].days.at(day) by {
                        self.lemma_index(code, index as int, day);
                    }
                }
                return Some(bucket[index].days.clone());
            }
            index += 1;
        }
        proof { reveal(Directory::at); }
        None
    }

    pub fn share(&self) -> (out: Self)
        requires self.wf(),
        ensures out.wf(),
            forall|mode: Seq<u8>, day: i32| #![trigger out.at(mode, day)]
                out.at(mode, day) == self.at(mode, day),
    {
        let mut out = Directory::new();
        let mut index = 0;
        while index < self.hashes.len()
            invariant index <= self.hashes@.len(), self.wf(), out.wf(),
                out.hashes@ == self.hashes@.take(index as int),
                forall|code: u64| #![trigger out.buckets@[code]] out.buckets@.contains_key(code) ==>
                    out.buckets@[code] == self.buckets@[code],
            decreases self.hashes@.len() - index,
        {
            let code = self.hashes[index];
            match self.buckets.get(&code) {
                Some(bucket) => {
                    proof { verus_prooflib::lemma_prefix_fresh(self.hashes@, index as int); }
                    out.buckets.insert(code, bucket.clone()); out.hashes.push(code);
                    proof {
                        out.hashes@.to_set_ensures();
                        assert(out.hashes@.to_set() =~= out.buckets@.dom());
                    }
                },
                None => {},
            }
            index += 1;
        }
        proof {
            self.hashes@.to_set_ensures();
            assert(out.hashes@ =~= self.hashes@);
            assert(out.buckets@ =~= self.buckets@);
            reveal(Directory::at);
        }
        out
    }

    proof fn lemma_installed(self, before: Directory, code: u64, index: int,
        original: Seq<ModeEntry>, updated: Seq<ModeEntry>, mode: Seq<u8>, days: Layers)
        requires self.wf(), before.wf(), days.wf(), code == bytes::hash_bytes(mode),
            original == if before.buckets@.contains_key(code) { before.buckets@[code]@ } else { Seq::empty() },
            self.buckets@.dom() == if updated.len() > 0 { before.buckets@.dom().insert(code) } else { before.buckets@.dom().remove(code) },
            updated.len() > 0 ==> self.buckets@[code]@ == updated,
            forall|other: u64| #![trigger self.buckets@[other]] other != code && self.buckets@.contains_key(other) ==> self.buckets@[other] == before.buckets@[other],
            0 <= index <= original.len(),
            forall|offset: int| #![trigger original[offset]] 0 <= offset < index ==> original[offset].mode@ != mode,
            index < original.len() ==> original[index].mode@ == mode,
            updated.len() == original.len() + if days.live > 0 { if index < original.len() { 0int } else { 1int } } else { if index < original.len() { -1int } else { 0int } },
            forall|offset: int| #![trigger updated[offset]] 0 <= offset < updated.len() ==> {
                &&& (days.live > 0 && offset == index ==> updated[offset].mode@ == mode && *updated[offset].days == days)
                &&& (days.live == 0 || offset != index ==> updated[offset] == original[if days.live == 0 && offset >= index { offset + 1 } else { offset }])
            },
        ensures forall|text: Seq<u8>, day: i32| #![trigger self.at(text, day)]
            self.at(text, day) == if text == mode { days.at(day) } else { before.at(text, day) },
    {
            assert forall|text: Seq<u8>, day: i32| #![trigger self.at(text, day)]
                self.at(text, day) == if text == mode { days.at(day) } else { before.at(text, day) } by {
                if text == mode {
                    if days.live > 0 {
                        self.lemma_index(code, index as int, day);
                        assert(self.buckets@[code]@[index].mode@ == mode);
                        assert(*self.buckets@[code]@[index].days == days);
                        assert(self.at(mode, day) == days.at(day));
                    }
                    else {
                        days.lemma_empty(day);
                        reveal(Directory::at);
                        assert forall|offset: int| #![trigger updated[offset]]
                            0 <= offset < updated.len() implies updated[offset].mode@ != mode by {}
                        assert(self.at(mode, day).high == 0 && self.at(mode, day).low == 0);
                    }
                } else {
                    reveal(Directory::at);
                    if bytes::hash_bytes(text) == code {
                        if exists|offset: int| #![trigger original[offset]]
                            0 <= offset < original.len() && original[offset].mode@ == text {
                            let previous = choose|offset: int| #![trigger original[offset]]
                                0 <= offset < original.len() && original[offset].mode@ == text;
                            assert(previous != index);
                            let position = if days.live == 0 && previous > index { previous - 1 } else { previous };
                            assert(0 <= position < updated.len());
                            before.lemma_index(code, previous, day);
                            self.lemma_index(code, position, day);
                            assert(self.at(text, day) == before.at(text, day));
                        } else {
                            assert forall|offset: int| #![trigger updated[offset]]
                                0 <= offset < updated.len() implies updated[offset].mode@ != text by {}
                            assert(self.at(text, day).high == 0 && before.at(text, day).high == 0);
                            assert(self.at(text, day).low == 0 && before.at(text, day).low == 0);
                        }
                    } else {
                        assert(self.buckets@.contains_key(bytes::hash_bytes(text)) == before.buckets@.contains_key(bytes::hash_bytes(text)));
                        assert(self.at(text, day) == before.at(text, day));
                    }
                }
            }
    }

    #[verifier::rlimit(60)]
    pub fn install(&mut self, mode: Arc<Vec<u8>>, days: Arc<Layers>)
        requires old(self).wf(), days.wf(),
        ensures final(self).wf(),
            forall|other: Seq<u8>, day: i32| #![trigger final(self).at(other, day)]
                final(self).at(other, day) == if other == mode@ { days.at(day) }
                    else { old(self).at(other, day) },
    {
        let ghost before = *self;
        let code = bytes::hash(&mode);
        proof { self.hashes@.to_set_ensures(); }
        let mut bucket = Vec::new();
        let old_bucket = self.buckets.remove(&code);
        let existed = old_bucket.is_some();
        match old_bucket {
            Some(old_bucket) => {
                let mut index = 0;
                while index < old_bucket.len()
                    invariant index <= old_bucket@.len(), before.wf(), days.wf(), before == *old(self),
                        bucket@ == old_bucket@.take(index as int), old_bucket == before.buckets@[code],
                        before.buckets@.contains_key(code), existed,
                        self.buckets@ == before.buckets@.remove(code), self.hashes@ == before.hashes@,
                        code == bytes::hash_bytes(mode@),
                    decreases old_bucket@.len() - index,
                {
                    bucket.push(ModeEntry { mode: old_bucket[index].mode.clone(), days: old_bucket[index].days.clone() });
                    proof { assert(bucket@ =~= old_bucket@.take(index as int + 1)); }
                    index += 1;
                }
                proof { assert(bucket@ =~= old_bucket@); }
            },
            None => {},
        }
        let ghost original = bucket@;
        let mut index = 0;
        let mut found = false;
        while index < bucket.len() && !found
            invariant index <= bucket@.len(), bucket@ == original, before.wf(), days.wf(), before == *old(self),
                original == if before.buckets@.contains_key(code) { before.buckets@[code]@ } else { Seq::empty() },
                existed == before.buckets@.contains_key(code), self.buckets@ == before.buckets@.remove(code),
                self.hashes@ == before.hashes@, code == bytes::hash_bytes(mode@),
                forall|offset: int| #![trigger bucket@[offset]] 0 <= offset < index ==> bucket@[offset].mode@ != mode@,
                found ==> index < bucket@.len() && bucket@[index as int].mode@ == mode@,
            decreases bucket@.len() - index, if found { 0int } else { 1int },
        {
            found = bytes::equal(&bucket[index].mode, &mode);
            if !found { index += 1; }
        }
        let mut stored_mode = mode.clone();
        if index < bucket.len() {
            let removed = bucket.remove(index);
            stored_mode = removed.mode;
        }
        if days.live > 0 { bucket.insert(index, ModeEntry { mode: stored_mode, days: days.clone() }); }
        let ghost updated = bucket@;
        proof {
            assert forall|offset: int| #![trigger updated[offset]] 0 <= offset < updated.len() implies {
                &&& updated[offset].days.wf()
                &&& updated[offset].days.live > 0
                &&& bytes::hash_bytes(updated[offset].mode@) == code
                &&& (days.live > 0 && offset == index ==> updated[offset].mode@ == mode@ && updated[offset].days == days)
                &&& (days.live == 0 || offset != index ==> updated[offset] == original[if days.live == 0 && offset >= index { offset + 1 } else { offset }])
            } by {}
            assert forall|left: int, right: int| #![trigger updated[left], updated[right]]
                0 <= left < right < updated.len() implies updated[left].mode@ != updated[right].mode@ by {
                if days.live > 0 && left == index {
                    assert(original[index as int].mode@ == mode@);
                } else if days.live > 0 && right == index {
                    if index < original.len() { assert(original[index as int].mode@ == mode@); }
                }
            }
        }
        if bucket.len() > 0 {
            self.buckets.insert(code, Arc::new(bucket));
            if !existed {
                proof { verus_prooflib::lemma_unique_push(self.hashes@, code); }
                self.hashes.push(code);
            }
        } else if existed {
            proof { before.hashes@.to_set_ensures(); assert(self.hashes@.contains(code)); }
            remove_hash(&mut self.hashes, code);
        }
        proof {
            self.hashes@.to_set_ensures();
            before.hashes@.to_set_ensures();
            assert(self.hashes@.to_set() =~= self.buckets@.dom());
            assert(self.wf());
            assert(self.buckets@.dom() =~= if updated.len() > 0 {
                before.buckets@.dom().insert(code)
            } else { before.buckets@.dom().remove(code) });
            let ghost payload = *days;
            self.lemma_installed(before, code, index as int, original, updated, mode@, payload);
        }
    }
}

}
