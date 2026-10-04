use vstd::prelude::*;
use vstd::arithmetic::div_mod::*;
use std::sync::Arc;
use verus_common::rel::*;
use verus_common::sums::*;
use crate::seqfacts::*;

verus! {

pub open spec fn radix_power(depth: nat) -> int
    decreases depth,
{
    if depth == 0 { 1 } else { 16 * radix_power((depth - 1) as nat) }
}

proof fn lemma_power(depth: nat)
    ensures radix_power(depth) > 0,
        depth <= 16 ==> radix_power(depth) <= 18446744073709551616,
    decreases depth,
{
    if depth > 0 { lemma_power((depth - 1) as nat); }
    if depth <= 16 {
        reveal_with_fuel(radix_power, 17);
        assert(depth == 0 || depth == 1 || depth == 2 || depth == 3 || depth == 4 || depth == 5
            || depth == 6 || depth == 7 || depth == 8 || depth == 9 || depth == 10 || depth == 11
            || depth == 12 || depth == 13 || depth == 14 || depth == 15 || depth == 16);
    }
}

pub open spec fn option_weight<Value>(value: Option<Value>, f: spec_fn(Value) -> int) -> int {
    match value { Some(value) => f(value), None => 0 }
}

pub open spec fn arc_value<Value>(value: Option<Arc<Value>>) -> Option<Value> {
    match value { Some(value) => Some(*value), None => None }
}

closed spec fn arc_at<Value>(page: Option<Arc<Page<Value>>>, key: u64, depth: nat) -> Option<Value> {
    match page { Some(page) => page.at(key, depth), None => None }
}

proof fn lemma_lift_key(child: u64, index: int, depth: nat) -> (key: u64)
    requires depth < 16, 0 <= index < 16, (child as int) < radix_power((15 - depth) as nat),
    ensures (key as int) < radix_power((16 - depth) as nat),
        key as int / radix_power((15 - depth) as nat) == index,
        key as int % radix_power((15 - depth) as nat) == child as int,
{
    let power = radix_power((15 - depth) as nat);
    lemma_power((16 - depth) as nat);
    lemma_power((15 - depth) as nat);
    let full = index * power + child as int;
    assert(radix_power((16 - depth) as nat) == 16 * power);
    assert(0 <= index * power + (child as int) < 16 * power) by (nonlinear_arith)
        requires 0 <= index < 16, 0 <= (child as int) < power, power > 0;
    lemma_fundamental_div_mod(full, power);
    lemma_mod_bound(full, power);
    assert(full / power == index) by (nonlinear_arith)
        requires full == index * power + child as int, power > 0,
            0 <= (child as int) < power,
            full == (full / power) * power + full % power, 0 <= full % power < power;
    assert(full % power == child as int) by (nonlinear_arith)
        requires full == index * power + child as int, power > 0,
            full == (full / power) * power + full % power, full / power == index;
    full as u64
}

fn power_exec(depth: u8) -> (result: u64)
    requires depth <= 15,
    ensures result as int == radix_power(depth as nat), result > 0,
{
    proof { reveal_with_fuel(radix_power, 17); }
    match depth {
        0 => 1, 1 => 16, 2 => 256, 3 => 4096,
        4 => 65536, 5 => 1048576, 6 => 16777216, 7 => 268435456,
        8 => 4294967296, 9 => 68719476736, 10 => 1099511627776, 11 => 17592186044416,
        12 => 281474976710656, 13 => 4503599627370496,
        14 => 72057594037927936, _ => 1152921504606846976,
    }
}

proof fn lemma_digit_bound(key: u64, depth: nat)
    requires depth < 16, (key as int) < radix_power((16 - depth) as nat),
    ensures key as int / radix_power((15 - depth) as nat) < 16,
        0 <= key as int % radix_power((15 - depth) as nat) < radix_power((15 - depth) as nat),
{
    let power = radix_power((15 - depth) as nat);
    lemma_power((15 - depth) as nat);
    lemma_fundamental_div_mod(key as int, power);
    lemma_mod_bound(key as int, power);
    assert(radix_power((16 - depth) as nat) == 16 * power);
    assert(key as int / power < 16) by (nonlinear_arith)
        requires 0 <= (key as int) < 16 * power, power > 0,
            key as int == (key as int / power) * power + key as int % power,
            0 <= key as int % power < power;
}

pub struct Page<Value> {
    value: Option<Arc<Value>>,
    branches: Vec<Option<Arc<Page<Value>>>>,
    occupied: u8,
}

closed spec fn branch_values<Value>(depth: nat) -> spec_fn(Option<Arc<Page<Value>>>) -> Seq<Value>
    decreases 2 * (16 - depth) + 1,
{
    if depth <= 16 {
        |branch: Option<Arc<Page<Value>>>| match branch { Some(page) => page.values(depth), None => Seq::empty() }
    } else { |branch: Option<Arc<Page<Value>>>| Seq::empty() }
}

impl<Value> Page<Value> {
    pub closed spec fn wf(self, depth: nat) -> bool
        decreases 16 - depth,
    {
        &&& depth <= 16
        &&& self.occupied as int == sum(self.branches@, |branch: Option<Arc<Page<Value>>>| if branch is Some { 1int } else { 0 })
        &&& if depth >= 16 { self.branches.len() == 0 }
            else {
                &&& self.value is None
                &&& (self.branches.len() == 0 || self.branches.len() == 16)
                &&& forall|index: int| 0 <= index < self.branches.len() ==>
                    ((#[trigger] self.branches@[index]) matches Some(page) ==> page.wf(depth + 1) && !page.empty())
            }
    }

    pub closed spec fn empty(self) -> bool { self.value is None && self.occupied == 0 }

    pub closed spec fn at(self, key: u64, depth: nat) -> Option<Value>
        decreases 16 - depth,
    {
        if depth > 16 || key as int >= radix_power((16 - depth) as nat) { None }
        else if depth == 16 { match self.value { Some(value) => Some(*value), None => None } }
        else if self.branches.len() == 0 { None }
        else { match self.branches@[key as int / radix_power((15 - depth) as nat)] {
            Some(page) => page.at((key as int % radix_power((15 - depth) as nat)) as u64, depth + 1), None => None,
        } }
    }

    pub closed spec fn values(self, depth: nat) -> Seq<Value>
        decreases 2 * (16 - depth),
    {
        if depth >= 16 { match self.value { Some(value) => seq![*value], None => Seq::empty() } }
        else { flat(self.branches@.map_values(branch_values(depth + 1))) }
    }

    proof fn lemma_value_witness(&self, value: Value, depth: nat) -> (key: u64)
        requires depth <= 16, self.wf(depth), self.values(depth).contains(value),
        ensures (key as int) < radix_power((16 - depth) as nat), self.at(key, depth) == Some(value),
        decreases 16 - depth,
    {
        if depth == 16 { 0u64 }
        else {
            let parts = self.branches@.map_values(branch_values(depth + 1));
            assert(self.values(depth) == flat(parts));
            lemma_flat_contains(parts, value);
            let index = choose|index: int| 0 <= index < parts.len() && (#[trigger] parts[index]).contains(value);
            let page = self.branches@[index].get_Some_0();
            let child = page.lemma_value_witness(value, depth + 1);
            lemma_lift_key(child, index, depth)
        }
    }

    proof fn lemma_all_values(&self, depth: nat, predicate: spec_fn(Value) -> bool)
        requires depth <= 16, self.wf(depth),
            forall|key: u64| #[trigger] self.at(key, depth) matches Some(value) ==> predicate(value),
        ensures forall|index: int| 0 <= index < self.values(depth).len() ==> predicate(#[trigger] self.values(depth)[index]),
    {
        assert forall|index: int| 0 <= index < self.values(depth).len() implies predicate(#[trigger] self.values(depth)[index]) by {
            let value = self.values(depth)[index];
            assert(self.values(depth).contains(value));
            self.lemma_value_witness(value, depth);
        }
    }

    proof fn lemma_sum_lookup(&self, key: u64, depth: nat, f: spec_fn(Value) -> int)
        requires depth <= 16, self.wf(depth), (key as int) < radix_power((16 - depth) as nat),
            forall|other: u64| other != key ==> (#[trigger] self.at(other, depth) matches Some(value) ==> f(value) == 0),
        ensures sum(self.values(depth), f) == option_weight(self.at(key, depth), f),
        decreases 16 - depth,
    {
        if depth < 16 && self.branches.len() > 0 {
            let power = radix_power((15 - depth) as nat);
            lemma_digit_bound(key, depth);
            let target = key as int / power;
            let remainder = (key as int % power) as u64;
            let expand = branch_values(depth + 1);
            lemma_power((15 - depth) as nat);
            let parts = self.branches@.map_values(expand);
            lemma_sum_flat_map(self.branches@, expand, f);
            let parts_weight = |branch: Option<Arc<Page<Value>>>| sum(expand(branch), f);
            assert forall|index: int| 0 <= index < self.branches.len() implies
                #[trigger] parts_weight(self.branches@[index])
                    == if index == target { option_weight(self.at(key, depth), f) } else { 0 } by {
                if let Some(page) = self.branches@[index] {
                    if index == target {
                        assert forall|other: u64| other != remainder implies
                            (#[trigger] page.at(other, depth + 1) matches Some(value) ==> f(value) == 0) by {
                            if (other as int) < power {
                                let full = lemma_lift_key(other, index, depth);
                                assert(full != key);
                                assert(self.at(full, depth) == page.at(other, depth + 1));
                            }
                        }
                        page.lemma_sum_lookup(remainder, depth + 1, f);
                    } else {
                        assert forall|other: u64| (#[trigger] page.at(other, depth + 1) matches Some(value) ==> f(value) == 0) by {
                            if page.at(other, depth + 1) is Some {
                                let full = lemma_lift_key(other, index, depth);
                                assert(full != key);
                                assert(self.at(full, depth) == page.at(other, depth + 1));
                            }
                        }
                        page.lemma_all_values(depth + 1, |value: Value| f(value) == 0);
                        lemma_sum_zero(page.values(depth + 1), f);
                    }
                }
            }
            lemma_sum_single_match(self.branches@, parts_weight, target);
        } else if depth == 16 {
            let ghost saved = arc_value(self.value);
            if let Some(value) = saved { lemma_sum_singleton(value, f); }
        }
    }

    proof fn lemma_empty(&self, depth: nat)
        requires depth <= 16, self.wf(depth),
        ensures self.empty() <==> self.values(depth).len() == 0,
            self.empty() ==> forall|key: u64| #[trigger] self.at(key, depth) is None,
        decreases 16 - depth,
    {
        if depth < 16 {
            let count = |branch: Option<Arc<Page<Value>>>| if branch is Some { 1int } else { 0 };
            let parts = self.branches@.map_values(branch_values(depth + 1));
            if self.occupied == 0 {
                assert forall|index: int| 0 <= index < parts.len() implies (#[trigger] parts[index]).len() == 0 by {
                    lemma_sum_ge_term(self.branches@, count, index);
                    assert(self.branches@[index] is None);
                }
                lemma_flat_empty(parts);
                assert forall|key: u64| #[trigger] self.at(key, depth) is None by {
                    if (key as int) < radix_power((16 - depth) as nat) && self.branches.len() > 0 {
                        lemma_digit_bound(key, depth);
                        let index = key as int / radix_power((15 - depth) as nat);
                        lemma_sum_ge_term(self.branches@, count, index);
                        assert(self.branches@[index] is None);
                    }
                }
            } else {
                let index = lemma_sum_nonzero_witness(self.branches@, count);
                let page = self.branches@[index].get_Some_0();
                page.lemma_empty(depth + 1);
                assert(parts[index].len() > 0);
                let value = parts[index][0];
                assert(parts[index].contains(value));
                lemma_flat_contains(parts, value);
            }
        }
    }

    pub fn new() -> (result: Self)
        ensures result.empty(), forall|depth: nat| depth <= 16 ==> #[trigger] result.wf(depth),
            forall|depth: nat, key: u64| depth <= 16 ==> #[trigger] result.at(key, depth) is None,
            forall|depth: nat| depth <= 16 ==> (#[trigger] result.values(depth)).len() == 0,
    { Self { value: None, branches: Vec::new(), occupied: 0 } }

    fn get(&self, key: u64, depth: u8) -> (result: Option<Arc<Value>>)
        requires depth <= 16, self.wf(depth as nat), (key as int) < radix_power((16 - depth) as nat),
        ensures match result { Some(value) => self.at(key, depth as nat) == Some(*value), None => self.at(key, depth as nat) is None },
        decreases 16 - depth,
    {
        if depth == 16 { return match &self.value { Some(value) => Some(value.clone()), None => None }; }
        if self.branches.len() == 0 { return None; }
        let power = power_exec(15 - depth);
        proof { lemma_digit_bound(key, depth as nat); }
        let index = (key / power) as usize;
        match &self.branches[index] { Some(page) => page.get(key % power, depth + 1), None => None }
    }

    fn replace(&self, key: u64, value: Option<Arc<Value>>, depth: u8) -> (result: Self)
        requires depth <= 16, self.wf(depth as nat), (key as int) < radix_power((16 - depth) as nat),
        ensures result.wf(depth as nat),
            result.at(key, depth as nat) == match value { Some(item) => Some(*item), None => None },
            forall|other: u64| other != key ==>
                #[trigger] result.at(other, depth as nat) == self.at(other, depth as nat),
            forall|f: spec_fn(Value) -> int| #[trigger] sum(result.values(depth as nat), f)
                == sum(self.values(depth as nat), f)
                    + option_weight(match value { Some(item) => Some(*item), None => None }, f)
                    - option_weight(self.at(key, depth as nat), f),
        decreases 16 - depth,
    {
        if depth == 16 {
            proof {
                assert(key == 0);
                let ghost wanted = arc_value(value);
                assert forall|f: spec_fn(Value) -> int| #[trigger] option_weight(wanted, f)
                    == sum(match wanted { Some(item) => seq![item], None => Seq::empty() }, f) by {
                    if let Some(item) = wanted { lemma_sum_singleton(item, f); }
                }
                let ghost previous = arc_value(self.value);
                assert forall|f: spec_fn(Value) -> int| #[trigger] option_weight(previous, f)
                    == sum(self.values(depth as nat), f) by {
                    if let Some(item) = previous { lemma_sum_singleton(item, f); }
                }
            }
            return Self { value, branches: Vec::new(), occupied: 0 };
        }
        let power = power_exec(15 - depth);
        proof { lemma_digit_bound(key, depth as nat); }
        let target = (key / power) as usize;
        let remainder = key % power;
        let ghost original = if self.branches.len() == 0 { Seq::new(16, |index: int| None) } else { self.branches@ };
        let ghost wanted = arc_value(value);
        let ghost previous = arc_at(original[target as int], remainder, depth as nat + 1);
        let next = if self.branches.len() == 0 { Self::new().replace(remainder, value, depth + 1) }
            else { match &self.branches[target] {
                Some(page) => page.replace(remainder, value, depth + 1),
                None => Self::new().replace(remainder, value, depth + 1),
            } };
        proof { next.lemma_empty(depth as nat + 1); }
        let child = if next.value.is_none() && next.occupied == 0 { None } else { Some(Arc::new(next)) };
        let mut branches = Vec::new();
        let mut occupied = 0u8;
        let mut index = 0usize;
        while index < 16
            invariant index <= 16, branches.len() == index, depth < 16,
                self.wf(depth as nat), next.wf(depth as nat + 1), target < 16,
                original.len() == 16,
                original == if self.branches.len() == 0 { Seq::new(16, |index: int| None) } else { self.branches@ },
                child is None <==> next.empty(),
                child matches Some(page) ==> *page == next,
                next.empty() <==> next.values(depth as nat + 1).len() == 0,
                next.empty() ==> forall|other: u64| #[trigger] next.at(other, depth as nat + 1) is None,
                next.at(remainder, depth as nat + 1) == wanted,
                forall|other: u64| other != remainder ==> #[trigger] next.at(other, depth as nat + 1)
                    == match original[target as int] { Some(page) => page.at(other, depth as nat + 1), None => None },
                forall|f: spec_fn(Value) -> int| #[trigger] sum(next.values(depth as nat + 1), f)
                    == sum(match original[target as int] { Some(page) => page.values(depth as nat + 1), None => Seq::empty() }, f)
                        + option_weight(wanted, f) - option_weight(previous, f),
                occupied as int == sum(branches@, |branch: Option<Arc<Page<Value>>>| if branch is Some { 1int } else { 0 }),
                occupied <= index,
                forall|position: int| 0 <= position < index ==> #[trigger] branches@[position]
                    == if position == target as int { child } else { original[position] },
            decreases 16 - index,
        {
            let branch = if index == target { match &child { Some(page) => Some(page.clone()), None => None } }
                else if self.branches.len() == 0 { None }
                else { match &self.branches[index] { Some(page) => Some(page.clone()), None => None } };
            proof { lemma_sum_push(branches@, branch, |branch: Option<Arc<Page<Value>>>| if branch is Some { 1int } else { 0 }); }
            if branch.is_some() { occupied += 1; }
            branches.push(branch);
            index += 1;
        }
        let result = Self { value: None, branches, occupied };
        proof {
            assert(result.wf(depth as nat));
            assert forall|other: u64| other != key implies
                #[trigger] result.at(other, depth as nat) == self.at(other, depth as nat) by {
                if (other as int) < radix_power((16 - depth) as nat) {
                    lemma_digit_bound(other, depth as nat);
                    if other as int / power as int == target as int {
                        lemma_fundamental_div_mod(other as int, power as int);
                        lemma_fundamental_div_mod(key as int, power as int);
                        assert(other % power != remainder);
                        assert(next.at(other % power, depth as nat + 1) == arc_at(original[target as int], other % power, depth as nat + 1));
                        assert(result.at(other, depth as nat) == next.at(other % power, depth as nat + 1));
                        assert(self.at(other, depth as nat) == arc_at(original[target as int], other % power, depth as nat + 1));
                    } else {
                        let position = other as int / power as int;
                        assert(result.branches@[position] == original[position]);
                        assert(result.at(other, depth as nat) == arc_at(original[position], other % power, depth as nat + 1));
                        assert(self.at(other, depth as nat) == arc_at(original[position], other % power, depth as nat + 1));
                    }
                }
            }
            assert forall|f: spec_fn(Value) -> int| #[trigger] sum(result.values(depth as nat), f)
                == sum(self.values(depth as nat), f) + option_weight(wanted, f) - option_weight(self.at(key, depth as nat), f) by {
                let expand = branch_values(depth as nat + 1);
                let old_parts = original.map_values(expand);
                let new_parts = result.branches@.map_values(expand);
                assert forall|position: int| 0 <= position < new_parts.len() implies
                    #[trigger] new_parts[position] == old_parts.update(target as int, next.values(depth as nat + 1))[position] by {
                    if position == target as int {
                        if child is None { assert(next.values(depth as nat + 1) =~= Seq::empty()); }
                    }
                }
                assert(new_parts =~= old_parts.update(target as int, next.values(depth as nat + 1)));
                if self.branches.len() == 0 { lemma_flat_empty(old_parts); }
                assert(flat(old_parts) == self.values(depth as nat));
                lemma_sum_flat(old_parts, f);
                lemma_sum_flat(new_parts, f);
                lemma_sum_update(old_parts, target as int, next.values(depth as nat + 1), |part: Seq<Value>| sum(part, f));
            }
        }
        result
    }

    fn collect(&self, depth: u8, output: &mut Vec<Arc<Value>>)
        requires depth <= 16, self.wf(depth as nat),
        ensures final(output)@.map_values(|value: Arc<Value>| *value)
            == old(output)@.map_values(|value: Arc<Value>| *value) + self.values(depth as nat),
        decreases 16 - depth,
    {
        if depth == 16 {
            if let Some(value) = &self.value {
                let ghost previous = output@;
                output.push(value.clone());
                proof { assert(output@.map_values(|item: Arc<Value>| *item) =~= previous.map_values(|item: Arc<Value>| *item).push(**value)); }
            }
        } else {
            let ghost initial = output@.map_values(|value: Arc<Value>| *value);
            let ghost parts = self.branches@.map_values(branch_values(depth as nat + 1));
            let mut index = 0usize;
            while index < self.branches.len()
                invariant index <= self.branches.len(), depth < 16, self.wf(depth as nat),
                    parts == self.branches@.map_values(branch_values(depth as nat + 1)),
                    initial == old(output)@.map_values(|value: Arc<Value>| *value),
                    output@.map_values(|value: Arc<Value>| *value) == initial + flat(parts.take(index as int)),
                decreases self.branches.len() - index,
            {
                if let Some(page) = &self.branches[index] { page.collect(depth + 1, output); }
                proof { lemma_flat_take_step(parts, index as int); }
                index += 1;
            }
            proof { assert(parts.take(index as int) =~= parts); }
        }
    }
}

pub struct Paged<Value> {
    root: Arc<Page<Value>>,
}

impl<Value> Paged<Value> {
    pub closed spec fn wf(self) -> bool { self.root.wf(0) }
    pub closed spec fn at(self, key: u64) -> Option<Value> { self.root.at(key, 0) }
    pub closed spec fn values(self) -> Seq<Value> { self.root.values(0) }

    pub fn new() -> (result: Self)
        ensures result.wf(), result.values().len() == 0,
            forall|key: u64| #[trigger] result.at(key) is None,
    { Self { root: Arc::new(Page::new()) } }

    pub fn share(&self) -> (result: Self)
        ensures result == *self,
    { Self { root: self.root.clone() } }

    pub fn get(&self, key: u64) -> (result: Option<Arc<Value>>)
        requires self.wf(),
        ensures match result { Some(value) => self.at(key) == Some(*value), None => self.at(key) is None },
    {
        proof { reveal_with_fuel(radix_power, 17); }
        self.root.get(key, 0)
    }

    pub fn replace(&self, key: u64, value: Option<Arc<Value>>) -> (result: Self)
        requires self.wf(),
        ensures result.wf(),
            result.at(key) == match value { Some(item) => Some(*item), None => None },
            forall|other: u64| other != key ==> #[trigger] result.at(other) == self.at(other),
            forall|f: spec_fn(Value) -> int| #[trigger] sum(result.values(), f)
                == sum(self.values(), f) + option_weight(match value { Some(item) => Some(*item), None => None }, f)
                    - option_weight(self.at(key), f),
    {
        proof { reveal_with_fuel(radix_power, 17); }
        Self { root: Arc::new(self.root.replace(key, value, 0)) }
    }

    pub fn collect(&self) -> (result: Vec<Arc<Value>>)
        requires self.wf(),
        ensures result@.map_values(|value: Arc<Value>| *value) == self.values(),
    {
        let mut result = Vec::new();
        self.root.collect(0, &mut result);
        result
    }

    pub fn is_empty(&self) -> (result: bool)
        requires self.wf(),
        ensures result == (self.values().len() == 0),
    {
        proof { self.root.lemma_empty(0); }
        self.root.value.is_none() && self.root.occupied == 0
    }

    pub proof fn lemma_all_values(&self, predicate: spec_fn(Value) -> bool)
        requires self.wf(), forall|key: u64| #[trigger] self.at(key) matches Some(value) ==> predicate(value),
        ensures forall|index: int| 0 <= index < self.values().len() ==> predicate(#[trigger] self.values()[index]),
    {
        assert forall|key: u64| (#[trigger] self.root.at(key, 0) matches Some(value) ==> predicate(value)) by {
            assert(self.at(key) == self.root.at(key, 0));
        }
        self.root.lemma_all_values(0, predicate);
    }

    pub proof fn lemma_sum_lookup(&self, key: u64, f: spec_fn(Value) -> int)
        requires self.wf(), forall|other: u64| other != key ==> (#[trigger] self.at(other) matches Some(value) ==> f(value) == 0),
        ensures sum(self.values(), f) == option_weight(self.at(key), f),
    {
        reveal_with_fuel(radix_power, 17);
        assert forall|other: u64| other != key implies (#[trigger] self.root.at(other, 0) matches Some(value) ==> f(value) == 0) by {
            assert(self.at(other) == self.root.at(other, 0));
        }
        self.root.lemma_sum_lookup(key, 0, f);
    }
}

}
