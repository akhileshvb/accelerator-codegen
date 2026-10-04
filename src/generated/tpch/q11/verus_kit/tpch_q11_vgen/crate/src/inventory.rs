use std::sync::Arc;
use vstd::prelude::*;
use verus_common::sums::sum;
use verus_prooflib::ordered::{Index, Key, KeyView};
use crate::values::*;
use crate::ranks::{self, Ranked, Pages};
use verus_prooflib::lemma_ordered_equal;

verus! {

pub struct Inventory {
    pub groups: Index<Moment>,
    pub ranks: Pages,
    pub total: Moment,
}

pub open spec fn moment_at(groups: Index<Moment>, part: int) -> Moment {
    match groups.at((part, Seq::<u8>::empty())) {
        Some(moment) => moment,
        None => Moment { count: 0, value: 0 },
    }
}

impl Inventory {
    pub open spec fn valid(self) -> bool {
        &&& self.groups.wf()
        &&& self.ranks.wf()
        &&& self.total.count as int == sum(self.groups.entries(), |row: (KeyView, Moment)| row.1.count as int)
        &&& self.total.value as int == sum(self.groups.entries(), |row: (KeyView, Moment)| row.1.value as int)
        &&& self.total.count >= 0
        &&& ranks::valid(self.ranks)
        &&& forall|key: KeyView| #[trigger] self.groups.at(key).is_some() ==> {
            let moment = self.groups.at(key).unwrap();
            &&& key.1 == Seq::<u8>::empty()
            &&& i64::MIN <= key.0 <= i64::MAX
            &&& moment.count > 0
            &&& ranks::rows(self.ranks).contains(Ranked { part: key.0 as i64, value: moment.value })
        }
        &&& forall|ranked: Ranked| #[trigger] ranks::rows(self.ranks).contains(ranked) ==> {
            let moment = moment_at(self.groups, ranked.part as int);
            &&& moment.count > 0
            &&& moment.value == ranked.value
        }
    }

    pub fn new() -> (result: Self)
        ensures result.valid(), result.groups.entries().len() == 0, result.total.count == 0, result.total.value == 0,
            forall|part: int| #[trigger] moment_at(result.groups, part) == zero_moment(),
    {
        Inventory { groups: Index::new(), ranks: ranks::new(), total: zero() }
    }

    pub fn share(&self) -> (result: Self)
        requires self.valid(),
        ensures result.valid(), result.groups.view() == self.groups.view(), result.ranks.view() == self.ranks.view(), result.total == self.total,
            forall|part: int| #[trigger] moment_at(result.groups, part) == moment_at(self.groups, part),
    {
        let groups = self.groups.share();
        let ranks = self.ranks.share();
        let result = Inventory { groups, ranks, total: self.total };
        proof {
            lemma_ordered_equal(&self.groups, &result.groups);
            lemma_ordered_equal(&self.ranks, &result.ranks);
            assert(result.groups.entries() == self.groups.entries());
            assert(result.ranks.entries() == self.ranks.entries());
            assert(ranks::rows(result.ranks) == ranks::rows(self.ranks));
            ranks::lemma_valid_equal(&self.ranks, &result.ranks);
            assert forall|key: KeyView| #[trigger] result.groups.at(key).is_some() implies {
                let moment = result.groups.at(key).unwrap();
                &&& key.1 == Seq::<u8>::empty()
                &&& i64::MIN <= key.0 <= i64::MAX
                &&& moment.count > 0
                &&& ranks::rows(result.ranks).contains(Ranked { part: key.0 as i64, value: moment.value })
            } by { assert(result.groups.at(key) == self.groups.at(key)); }
            assert forall|ranked: Ranked| #[trigger] ranks::rows(result.ranks).contains(ranked) implies {
                let moment = moment_at(result.groups, ranked.part as int);
                &&& moment.count > 0
                &&& moment.value == ranked.value
            } by { assert(result.groups.at((ranked.part as int, Seq::<u8>::empty())) == self.groups.at((ranked.part as int, Seq::<u8>::empty()))); }
        }
        result
    }

    pub fn update(&mut self, part: i64, change: Moment) -> (result: bool)
        requires old(self).valid(),
        ensures final(self).valid(),
            result ==> final(self).total.count as int == old(self).total.count as int + change.count as int,
            result ==> final(self).total.value as int == old(self).total.value as int + change.value as int,
            result ==> forall|other: int| other != part ==> #[trigger] moment_at(final(self).groups, other) == moment_at(old(self).groups, other),
            result ==> moment_at(final(self).groups, part as int).count as int
                == moment_at(old(self).groups, part as int).count as int + change.count as int,
            result ==> moment_at(final(self).groups, part as int).value as int
                == moment_at(old(self).groups, part as int).value as int + change.value as int,
    {
        let ghost original = *self;
        let key = Key::number(part);
        let previous = match self.groups.get(&key) { Some(moment) => *moment, None => zero() };
        let next = match plus(previous, change) { Some(moment) => moment, None => return false };
        let total = match plus(self.total, change) { Some(moment) => moment, None => return false };
        if next.count < 0 || total.count < 0 || (next.count == 0 && next.value != 0) {
            return false;
        }
        let rank_changed = previous.count == 0 || next.count == 0 || previous.value != next.value;
        proof {
            assert(previous == moment_at(original.groups, part as int));
            assert(previous.count >= 0);
        }
        if previous.count > 0 && rank_changed {
            proof { assert(ranks::rows(self.ranks).contains(Ranked { part, value: previous.value })); }
            if !ranks::remove(&mut self.ranks, Ranked { part, value: previous.value }) { return false; }
        }
        proof {
            assert forall|row: Ranked| #[trigger] ranks::rows(self.ranks).contains(row) <==>
                ranks::rows(original.ranks).contains(row) && !(previous.count > 0 && rank_changed && row == Ranked { part, value: previous.value }) by {}
            if rank_changed {
                assert(!ranks::rows(self.ranks).contains(Ranked { part, value: next.value }));
            }
        }
        if next.count == 0 {
            self.groups.remove(&key);
            proof {
                verus_prooflib::ordered::lemma_sum_remove(&original.groups, &self.groups, key.view(), |row: (KeyView, Moment)| row.1.count as int);
                verus_prooflib::ordered::lemma_sum_remove(&original.groups, &self.groups, key.view(), |row: (KeyView, Moment)| row.1.value as int);
            }
        } else {
            self.groups.put(key, Arc::new(next));
            proof {
                verus_prooflib::ordered::lemma_sum_put(&original.groups, &self.groups, (part as int, Seq::<u8>::empty()), next, |row: (KeyView, Moment)| row.1.count as int);
                verus_prooflib::ordered::lemma_sum_put(&original.groups, &self.groups, (part as int, Seq::<u8>::empty()), next, |row: (KeyView, Moment)| row.1.value as int);
            }
            if rank_changed && !ranks::insert(&mut self.ranks, Ranked { part, value: next.value }) { return false; }
        }
        self.total = total;
        proof {
            assert(self.groups.wf());
            assert(self.ranks.wf());
            assert(self.total.count as int == sum(self.groups.entries(), |row: (KeyView, Moment)| row.1.count as int));
            assert(self.total.value as int == sum(self.groups.entries(), |row: (KeyView, Moment)| row.1.value as int));
            assert forall|other: int| other != part implies #[trigger] moment_at(self.groups, other) == moment_at(original.groups, other) by {}
            assert forall|group_key: KeyView| #[trigger] self.groups.at(group_key).is_some() implies {
                let moment = self.groups.at(group_key).unwrap();
                &&& group_key.1 == Seq::<u8>::empty()
                &&& i64::MIN <= group_key.0 <= i64::MAX
                &&& moment.count > 0
                &&& ranks::rows(self.ranks).contains(Ranked { part: group_key.0 as i64, value: moment.value })
            } by {
                if group_key != (part as int, Seq::<u8>::empty()) {
                    assert(self.groups.at(group_key) == original.groups.at(group_key));
                    assert(original.groups.at(group_key).unwrap().count > 0);
                }
            }
            assert forall|row: Ranked| #[trigger] ranks::rows(self.ranks).contains(row) implies {
                let moment = moment_at(self.groups, row.part as int);
                &&& moment.count > 0
                &&& moment.value == row.value
            } by {
                if row.part != part {
                    assert(ranks::rows(original.ranks).contains(row));
                } else if row != (Ranked { part, value: next.value }) {
                    assert(ranks::rows(original.ranks).contains(row));
                    assert(row.value == previous.value);
                    assert(!rank_changed);
                }
            }
        }
        true
    }
}

pub open spec fn emission_prefix(inventory: Inventory, weight: int, output: Seq<(i64, i128)>, cutoff: i128) -> bool {
    &&& output.len() <= ranks::rows(inventory.ranks).len()
    &&& forall|position: int| 0 <= position < output.len() ==> {
        let ranked = ranks::rows(inventory.ranks)[position];
        let row = #[trigger] output[position];
        &&& row.0 == ranked.part
        &&& row.1 as int == weight * ranked.value as int
        &&& ranked.value >= cutoff
    }
}

pub proof fn lemma_emission_push(inventory: &Inventory, weight: int, output: Seq<(i64, i128)>, cutoff: i128, ranked: Ranked, value: i128)
    requires emission_prefix(*inventory, weight, output, cutoff), output.len() < ranks::rows(inventory.ranks).len(),
        ranks::rows(inventory.ranks)[output.len() as int] == ranked,
        value as int == weight * ranked.value as int, ranked.value >= cutoff,
    ensures emission_prefix(*inventory, weight, output.push((ranked.part, value)), cutoff),
{
    assert forall|position: int| 0 <= position < output.push((ranked.part, value)).len() implies {
        let source = ranks::rows(inventory.ranks)[position];
        let row = #[trigger] output.push((ranked.part, value))[position];
        &&& row.0 == source.part
        &&& row.1 as int == weight * source.value as int
        &&& source.value >= cutoff
    } by { if position < output.len() { assert(output.push((ranked.part, value))[position] == output[position]); } }
}

pub proof fn lemma_positive_group(inventory: &Inventory, part: int)
    requires inventory.valid(), moment_at(inventory.groups, part).count > 0,
    ensures inventory.total.count > 0,
{
    inventory.groups.model();
    let key = (part, Seq::<u8>::empty());
    assert(inventory.groups.at(key).is_some());
    let position = choose|position: int| 0 <= position < inventory.groups.entries().len()
        && (#[trigger] inventory.groups.entries()[position]).0 == key;
    assert forall|offset: int| 0 <= offset < inventory.groups.entries().len() implies
        (#[trigger] inventory.groups.entries()[offset]).1.count as int >= 0 by {
        assert(inventory.groups.at(inventory.groups.entries()[offset].0) == Some(inventory.groups.entries()[offset].1));
    }
    verus_common::sums::lemma_sum_pos(inventory.groups.entries(), |row: (KeyView, Moment)| row.1.count as int, position);
}

pub proof fn lemma_empty_inventory(inventory: &Inventory)
    requires inventory.valid(), inventory.total.count == 0,
    ensures inventory.groups.entries().len() == 0, inventory.total.value == 0,
        forall|part: int| #[trigger] moment_at(inventory.groups, part) == zero_moment(),
{
    inventory.groups.model();
    if inventory.groups.entries().len() > 0 {
        let key = inventory.groups.entries()[0].0;
        assert(inventory.groups.at(key).is_some());
        assert(key.1 == Seq::<u8>::empty());
        assert(moment_at(inventory.groups, key.0).count > 0);
        lemma_positive_group(inventory, key.0);
    }
    assert(inventory.groups.entries() =~= Seq::<(KeyView, Moment)>::empty());
    assert forall|part: int| #[trigger] moment_at(inventory.groups, part) == zero_moment() by {
        assert(inventory.groups.at((part, Seq::<u8>::empty())).is_none());
    }
}

pub proof fn lemma_emission_answer(inventory: &Inventory, weight: int, output: Seq<(i64, i128)>, cutoff: i128)
    requires inventory.valid(), weight > 0, emission_prefix(*inventory, weight, output, cutoff),
        cutoff as int * 10000 > inventory.total.value as int,
        (cutoff as int - 1) * 10000 <= inventory.total.value as int,
        forall|position: int| output.len() <= position < ranks::rows(inventory.ranks).len() ==>
            (#[trigger] ranks::rows(inventory.ranks)[position]).value < cutoff,
    ensures crate::semantics::inventory_answer(*inventory, weight, output),
{
    ranks::lemma_rows_sorted(&inventory.ranks);
    let ranked_rows = ranks::rows(inventory.ranks);
    assert forall|position: int| 0 <= position < output.len() implies {
        let row = #[trigger] output[position];
        &&& moment_at(inventory.groups, row.0 as int).count > 0
        &&& row.1 as int == weight * moment_at(inventory.groups, row.0 as int).value as int
        &&& inventory.total.count > 0
        &&& 10000 * moment_at(inventory.groups, row.0 as int).value as int > inventory.total.value as int
    } by {
        assert(ranked_rows.contains(ranked_rows[position]));
        lemma_positive_group(inventory, output[position].0 as int);
    }
    assert forall|left: int, right: int| 0 <= left < right < output.len() implies
        (#[trigger] output[left]).0 != (#[trigger] output[right]).0 && output[left].1 >= output[right].1 by {
        assert(ranks::before(ranked_rows[left], ranked_rows[right]));
        assert(ranked_rows.contains(ranked_rows[left]));
        assert(ranked_rows.contains(ranked_rows[right]));
        if output[left].0 == output[right].0 {
            assert(ranked_rows[left].value == ranked_rows[right].value);
        }
        assert(output[left].1 as int >= output[right].1 as int) by (nonlinear_arith)
            requires weight > 0, ranked_rows[left].value >= ranked_rows[right].value,
                output[left].1 as int == weight * ranked_rows[left].value as int,
                output[right].1 as int == weight * ranked_rows[right].value as int;
    }
    assert forall|part: i64| #[trigger] moment_at(inventory.groups, part as int).count > 0
        && 10000 * moment_at(inventory.groups, part as int).value as int > inventory.total.value as int implies
        exists|position: int| 0 <= position < output.len() && (#[trigger] output[position]).0 == part by {
        let moment = moment_at(inventory.groups, part as int);
        assert(inventory.groups.at((part as int, Seq::<u8>::empty())).is_some());
        let ranked = Ranked { part, value: moment.value };
        assert(ranked_rows.contains(ranked));
        let position = choose|position: int| 0 <= position < ranked_rows.len() && #[trigger] ranked_rows[position] == ranked;
        assert(position < output.len());
        assert(output[position].0 == part);
    }
}

pub fn emit(inventory: &Inventory, weight: i128) -> (result: Option<Vec<(i64, i128)>>)
    requires inventory.valid(), weight > 0,
    ensures result matches Some(rows) ==> forall|index: int| 0 <= index < rows.len() ==> {
        let row = #[trigger] rows[index];
        &&& moment_at(inventory.groups, row.0 as int).count > 0
        &&& row.1 as int == weight as int * moment_at(inventory.groups, row.0 as int).value as int
        &&& 10000 * moment_at(inventory.groups, row.0 as int).value as int > inventory.total.value as int
    },
        result matches Some(rows) ==> crate::semantics::inventory_answer(*inventory, weight as int, rows@),
{
    let mut result = Vec::new();
    if inventory.total.count == 0 {
        proof {
            assert forall|part: i64| #[trigger] moment_at(inventory.groups, part as int).count <= 0 by {
                if moment_at(inventory.groups, part as int).count > 0 { lemma_positive_group(inventory, part as int); }
            }
        }
        return Some(result);
    }
    let cutoff = threshold(inventory.total.value);
    proof { ranks::lemma_rows_sorted(&inventory.ranks); }
    let ghost mapped = inventory.ranks.entries().map_values(|entry: (KeyView, crate::ranks::Page)| entry.1.rows@);
    let mut cursor = inventory.ranks.cursor();
    let mut processed = Ghost(0int);
    loop
        invariant
            inventory.valid(), inventory.ranks.wf(), weight > 0,
            cutoff as int * 10000 > inventory.total.value as int,
            (cutoff as int - 1) * 10000 <= inventory.total.value as int,
            mapped == inventory.ranks.entries().map_values(|entry: (KeyView, crate::ranks::Page)| entry.1.rows@),
            0 <= processed@ <= inventory.ranks.entries().len(),
            cursor.remaining() == inventory.ranks.entries().skip(processed@),
            result.len() == verus_common::rel::flat(mapped.take(processed@)).len(),
            emission_prefix(*inventory, weight as int, result@, cutoff),
            forall|index: int| 0 <= index < result.len() ==> {
                let row = #[trigger] result@[index];
                &&& moment_at(inventory.groups, row.0 as int).count > 0
                &&& row.1 as int == weight as int * moment_at(inventory.groups, row.0 as int).value as int
                &&& 10000 * moment_at(inventory.groups, row.0 as int).value as int > inventory.total.value as int
            },
        decreases cursor.remaining().len(),
    {
        let (fence, page) = match cursor.next() {
            Some(row) => row,
            None => {
                proof {
                    assert(processed@ == mapped.len());
                    assert(mapped.take(processed@) =~= mapped);
                    lemma_emission_answer(inventory, weight as int, result@, cutoff);
                }
                return Some(result);
            },
        };
        proof {
            inventory.ranks.model();
            assert(inventory.ranks.at(fence.view()) == Some(*page));
            assert forall|position: int| 0 <= position < page.rows.len() implies
                #[trigger] ranks::rows(inventory.ranks).contains(page.rows@[position]) by {
                let mapper = |entry: (KeyView, crate::ranks::Page)| entry.1.rows@;
                assert(inventory.ranks.entries()[processed@].1.rows@.contains(page.rows@[position]));
                assert(mapper(inventory.ranks.entries()[processed@]).contains(page.rows@[position]));
                assert(exists|offset: int| 0 <= offset < inventory.ranks.entries().len()
                    && #[trigger] mapper(inventory.ranks.entries()[offset]).contains(page.rows@[position]));
                verus_prooflib::lemma_flat_map_contains(inventory.ranks.entries(), mapper, page.rows@[position]);
            }
        }
        let count = ranks::qualifying_prefix(&page.rows, cutoff);
        let mut position: usize = 0;
        let ghost prefix = result@;
        proof {
            verus_prooflib::lemma_flat_take_step(mapped, processed@);
            verus_prooflib::lemma_flat_take_prefix(mapped, processed@ + 1);
            assert(mapped[processed@] == page.rows@);
            assert forall|offset: int| 0 <= offset < page.rows.len() implies
                #[trigger] ranks::rows(inventory.ranks)[prefix.len() as int + offset] == page.rows@[offset] by {
                assert(verus_common::rel::flat(mapped.take(processed@ + 1))[prefix.len() as int + offset] == page.rows@[offset]);
            }
        }
        if weight == 1 {
            while position < count
                invariant
                    position <= count <= page.rows.len(), inventory.valid(), weight == 1,
                    result.len() == prefix.len() + position,
                    emission_prefix(*inventory, weight as int, result@, cutoff),
                    prefix.len() + page.rows.len() <= ranks::rows(inventory.ranks).len(),
                    forall|offset: int| 0 <= offset < page.rows.len() ==>
                        #[trigger] ranks::rows(inventory.ranks)[prefix.len() as int + offset] == page.rows@[offset],
                    cutoff as int * 10000 > inventory.total.value as int,
                    forall|offset: int| 0 <= offset < count ==> {
                        let ranked = #[trigger] page.rows@[offset];
                        &&& ranked.value >= cutoff
                        &&& ranks::rows(inventory.ranks).contains(ranked)
                    },
                    forall|index: int| 0 <= index < result.len() ==> {
                        let row = #[trigger] result@[index];
                        &&& moment_at(inventory.groups, row.0 as int).count > 0
                        &&& row.1 as int == weight as int * moment_at(inventory.groups, row.0 as int).value as int
                        &&& 10000 * moment_at(inventory.groups, row.0 as int).value as int > inventory.total.value as int
                    },
                decreases count - position,
            {
                let ranked = page.rows[position];
                proof { lemma_emission_push(inventory, weight as int, result@, cutoff, ranked, ranked.value); }
                result.push((ranked.part, ranked.value));
                position += 1;
            }
        } else {
            while position < count
                invariant
                    position <= count <= page.rows.len(), inventory.valid(), weight > 0,
                    result.len() == prefix.len() + position,
                    emission_prefix(*inventory, weight as int, result@, cutoff),
                    prefix.len() + page.rows.len() <= ranks::rows(inventory.ranks).len(),
                    forall|offset: int| 0 <= offset < page.rows.len() ==>
                        #[trigger] ranks::rows(inventory.ranks)[prefix.len() as int + offset] == page.rows@[offset],
                    cutoff as int * 10000 > inventory.total.value as int,
                    forall|offset: int| 0 <= offset < count ==> {
                        let ranked = #[trigger] page.rows@[offset];
                        &&& ranked.value >= cutoff
                        &&& ranks::rows(inventory.ranks).contains(ranked)
                    },
                    forall|index: int| 0 <= index < result.len() ==> {
                        let row = #[trigger] result@[index];
                        &&& moment_at(inventory.groups, row.0 as int).count > 0
                        &&& row.1 as int == weight as int * moment_at(inventory.groups, row.0 as int).value as int
                        &&& 10000 * moment_at(inventory.groups, row.0 as int).value as int > inventory.total.value as int
                    },
                decreases count - position,
            {
                let ranked = page.rows[position];
                let value = match multiply(ranked.value, weight) { Some(value) => value, None => return None };
                proof {
                    assert(moment_at(inventory.groups, ranked.part as int).value == ranked.value);
                    assert(value as int == weight as int * ranked.value as int) by (nonlinear_arith)
                        requires value as int == ranked.value as int * weight as int;
                    lemma_emission_push(inventory, weight as int, result@, cutoff, ranked, value);
                }
                result.push((ranked.part, value));
                position += 1;
            }
        }
        if count < page.rows.len() {
            proof {
                ranks::lemma_rows_sorted(&inventory.ranks);
                assert(ranks::rows(inventory.ranks)[result.len() as int] == page.rows@[count as int]);
                assert forall|offset: int| result.len() <= offset < ranks::rows(inventory.ranks).len() implies
                    (#[trigger] ranks::rows(inventory.ranks)[offset]).value < cutoff by {
                    if offset > result.len() {
                        assert(ranks::before(ranks::rows(inventory.ranks)[result.len() as int], ranks::rows(inventory.ranks)[offset]));
                    }
                }
                lemma_emission_answer(inventory, weight as int, result@, cutoff);
            }
            return Some(result);
        }
        processed = Ghost(processed@ + 1);
    }
}

}
