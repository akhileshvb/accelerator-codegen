use crate::packed::{self, Packed, PackedCursor};
use std::sync::Arc;
use vstd::prelude::*;
use verus_common::sums::sum;
use verus_prooflib::ordered::{Index, Key, KeyView};
use crate::values::*;
use verus_prooflib::wide::Wide;
use crate::ranks::{self, Ranked, Pages};
use verus_prooflib::lemma_ordered_equal;

verus! {

pub struct BigRow {
    pub part: i64,
    pub value: Wide,
}

pub struct Inventory {
    pub groups: Packed,
    pub ranks: Pages,
    pub big: Vec<BigRow>,
    pub total: Mom,
}

pub open spec fn moment_at(groups: Packed, part: int) -> Moment {
    match groups.at((part, Seq::<u8>::empty())) {
        Some(moment) => moment,
        None => Moment { count: 0, value: 0 },
    }
}

pub open spec fn in_range(value: int) -> bool {
    i128::MIN <= value <= i128::MAX
}

pub open spec fn big_before(left: BigRow, right: BigRow) -> bool {
    left.value@ > right.value@ || (left.value@ == right.value@ && left.part < right.part)
}

pub open spec fn big_sorted(rows: Seq<BigRow>) -> bool {
    forall|left: int, right: int| 0 <= left < right < rows.len() ==> big_before(#[trigger] rows[left], #[trigger] rows[right])
}

pub open spec fn big_distinct(rows: Seq<BigRow>) -> bool {
    forall|left: int, right: int| 0 <= left < right < rows.len() ==> (#[trigger] rows[left]).part != (#[trigger] rows[right]).part
}

pub open spec fn big_has_part(rows: Seq<BigRow>, part: i64) -> bool {
    exists|index: int| 0 <= index < rows.len() && (#[trigger] rows[index]).part == part
}

pub open spec fn big_has(rows: Seq<BigRow>, part: i64, value: int) -> bool {
    exists|index: int| 0 <= index < rows.len() && (#[trigger] rows[index]).part == part && rows[index].value@ == value
}

pub fn big_before_exec(left: &BigRow, right: &BigRow) -> (result: bool)
    ensures result == big_before(*left, *right),
{
    right.value.lt(&left.value) || (right.value.eq(&left.value) && left.part < right.part)
}

pub fn big_insert(rows: &mut Vec<BigRow>, row: BigRow)
    requires big_sorted(old(rows)@), big_distinct(old(rows)@), !big_has_part(old(rows)@, row.part),
    ensures big_sorted(final(rows)@), big_distinct(final(rows)@),
        forall|part: i64, value: int| #[trigger] big_has(final(rows)@, part, value)
            == (big_has(old(rows)@, part, value) || (part == row.part && value == row.value@)),
        forall|part: i64| #[trigger] big_has_part(final(rows)@, part) == (big_has_part(old(rows)@, part) || part == row.part),
{
    let ghost original = rows@;
    let mut position: usize = 0;
    while position < rows.len() && big_before_exec(&rows[position], &row)
        invariant position <= rows.len(), rows@ == original,
            forall|index: int| 0 <= index < position ==> big_before(#[trigger] rows@[index], row),
        decreases rows.len() - position,
    {
        position += 1;
    }
    let ghost row_view = row;
    rows.insert(position, row);
    proof {
        let result = rows@;
        assert(result == original.insert(position as int, row_view));
        assert forall|left: int, right: int| 0 <= left < right < result.len() implies big_before(#[trigger] result[left], #[trigger] result[right]) by {
            if right < position as int {
                assert(result[left] == original[left]);
                assert(result[right] == original[right]);
            } else if right == position as int {
                assert(result[left] == original[left]);
            } else if left < position as int {
                assert(result[left] == original[left]);
                assert(result[right] == original[right - 1]);
            } else if left == position as int {
                assert(result[right] == original[right - 1]);
                assert(!big_before(original[right - 1], row_view));
                assert(original[right - 1].part != row_view.part);
            } else {
                assert(result[left] == original[left - 1]);
                assert(result[right] == original[right - 1]);
            }
        }
        assert forall|left: int, right: int| 0 <= left < right < result.len() implies (#[trigger] result[left]).part != (#[trigger] result[right]).part by {
            if left == position as int { assert(big_has_part(original, result[right].part) || true); }
            if right == position as int { assert(big_has_part(original, result[left].part) || true); }
        }
        assert forall|part: i64, value: int| #[trigger] big_has(result, part, value)
            == (big_has(original, part, value) || (part == row_view.part && value == row_view.value@)) by {
            if big_has(result, part, value) {
                let index = choose|index: int| 0 <= index < result.len() && result[index].part == part && result[index].value@ == value;
                if index < position as int { assert(original[index] == result[index]); }
                else if index > position as int { assert(original[index - 1] == result[index]); }
            }
            if big_has(original, part, value) {
                let index = choose|index: int| 0 <= index < original.len() && original[index].part == part && original[index].value@ == value;
                if index < position as int { assert(result[index] == original[index]); } else { assert(result[index + 1] == original[index]); }
            }
            if part == row_view.part && value == row_view.value@ { assert(result[position as int] == row_view); }
        }
        assert forall|part: i64| #[trigger] big_has_part(result, part) == (big_has_part(original, part) || part == row_view.part) by {
            if big_has_part(result, part) {
                let index = choose|index: int| 0 <= index < result.len() && result[index].part == part;
                if index < position as int { assert(original[index] == result[index]); }
                else if index > position as int { assert(original[index - 1] == result[index]); }
            }
            if big_has_part(original, part) {
                let index = choose|index: int| 0 <= index < original.len() && original[index].part == part;
                if index < position as int { assert(result[index] == original[index]); } else { assert(result[index + 1] == original[index]); }
            }
            if part == row_view.part { assert(result[position as int] == row_view); }
        }
    }
}

pub fn big_remove(rows: &mut Vec<BigRow>, part: i64)
    requires big_sorted(old(rows)@), big_distinct(old(rows)@), big_has_part(old(rows)@, part),
    ensures big_sorted(final(rows)@), big_distinct(final(rows)@),
        forall|other: i64, value: int| #[trigger] big_has(final(rows)@, other, value)
            == (big_has(old(rows)@, other, value) && other != part),
        forall|other: i64| #[trigger] big_has_part(final(rows)@, other) == (big_has_part(old(rows)@, other) && other != part),
{
    let ghost original = rows@;
    let mut position: usize = 0;
    while position < rows.len() && rows[position].part != part
        invariant position <= rows.len(), rows@ == original,
            forall|index: int| 0 <= index < position ==> (#[trigger] rows@[index]).part != part,
            big_has_part(original, part),
        decreases rows.len() - position,
    {
        position += 1;
    }
    proof { assert(position < rows.len()); }
    rows.remove(position);
    proof {
        let result = rows@;
        assert(result == original.remove(position as int));
        assert forall|left: int, right: int| 0 <= left < right < result.len() implies
            big_before(#[trigger] result[left], #[trigger] result[right]) && result[left].part != result[right].part by {
            let l = if left < position as int { left } else { left + 1 };
            let r = if right < position as int { right } else { right + 1 };
            assert(result[left] == original[l]);
            assert(result[right] == original[r]);
        }
        assert forall|other: i64, value: int| #[trigger] big_has(result, other, value)
            == (big_has(original, other, value) && other != part) by {
            if big_has(result, other, value) {
                let index = choose|index: int| 0 <= index < result.len() && result[index].part == other && result[index].value@ == value;
                let source = if index < position as int { index } else { index + 1 };
                assert(original[source] == result[index]);
                assert(source != position as int);
            }
            if big_has(original, other, value) && other != part {
                let index = choose|index: int| 0 <= index < original.len() && original[index].part == other && original[index].value@ == value;
                assert(index != position as int);
                if index < position as int { assert(result[index] == original[index]); } else { assert(result[index - 1] == original[index]); }
            }
        }
        assert forall|other: i64| #[trigger] big_has_part(result, other) == (big_has_part(original, other) && other != part) by {
            if big_has_part(result, other) {
                let index = choose|index: int| 0 <= index < result.len() && result[index].part == other;
                let source = if index < position as int { index } else { index + 1 };
                assert(original[source] == result[index]);
            }
            if big_has_part(original, other) && other != part {
                let index = choose|index: int| 0 <= index < original.len() && original[index].part == other;
                assert(index != position as int);
                if index < position as int { assert(result[index] == original[index]); } else { assert(result[index - 1] == original[index]); }
            }
        }
    }
}

impl Inventory {
    pub open spec fn valid(self) -> bool {
        &&& self.groups.wf()
        &&& self.ranks.wf()
        &&& self.total.count@ == sum(self.groups.entries(), |row: (KeyView, Moment)| row.1.count)
        &&& self.total.value@ == sum(self.groups.entries(), |row: (KeyView, Moment)| row.1.value)
        &&& ranks::valid(self.ranks)
        &&& big_sorted(self.big@)
        &&& big_distinct(self.big@)
        &&& forall|key: KeyView| #[trigger] self.groups.at(key).is_some() ==> {
            let moment = self.groups.at(key).unwrap();
            &&& key.1 == Seq::<u8>::empty()
            &&& i64::MIN <= key.0 <= i64::MAX
            &&& moment.count != 0 || moment.value != 0
            &&& moment.count > 0 ==> if in_range(moment.value) {
                ranks::rows(self.ranks).contains(Ranked { part: key.0 as i64, value: moment.value as i128 })
            } else { big_has(self.big@, key.0 as i64, moment.value) }
        }
        &&& forall|ranked: Ranked| #[trigger] ranks::rows(self.ranks).contains(ranked) ==> {
            let moment = moment_at(self.groups, ranked.part as int);
            &&& moment.count > 0
            &&& moment.value == ranked.value as int
        }
        &&& forall|index: int| 0 <= index < self.big@.len() ==> {
            let moment = moment_at(self.groups, (#[trigger] self.big@[index]).part as int);
            &&& moment.count > 0
            &&& moment.value == self.big@[index].value@
            &&& !in_range(moment.value)
        }
    }

    pub fn new() -> (result: Self)
        ensures result.valid(), result.groups.entries().len() == 0, result.total@ == zero_moment(),
            forall|part: int| #[trigger] moment_at(result.groups, part) == zero_moment(),
    {
        Inventory { groups: Packed::new(8), ranks: ranks::new(), big: Vec::new(), total: zero() }
    }

    pub fn share(&self) -> (result: Self)
        requires self.valid(),
        ensures result.valid(), result.groups.view() == self.groups.view(), result.ranks.view() == self.ranks.view(), result.total@ == self.total@,
            forall|part: int| #[trigger] moment_at(result.groups, part) == moment_at(self.groups, part),
    {
        let groups = self.groups.share();
        let ranks = self.ranks.share();
        let mut big: Vec<BigRow> = Vec::new();
        let mut index: usize = 0;
        while index < self.big.len()
            invariant index <= self.big.len(), big.len() == index,
                forall|position: int| 0 <= position < index ==> big@[position].part == self.big@[position].part
                    && big@[position].value@ == self.big@[position].value@,
            decreases self.big.len() - index,
        {
            big.push(BigRow { part: self.big[index].part, value: self.big[index].value.dup() });
            index += 1;
        }
        let result = Inventory { groups, ranks, big, total: self.total.dup() };
        proof {
            lemma_ordered_equal(&self.ranks, &result.ranks);
            assert(result.groups.entries() == self.groups.entries());
            assert(result.ranks.entries() == self.ranks.entries());
            assert(ranks::rows(result.ranks) == ranks::rows(self.ranks));
            ranks::lemma_valid_equal(&self.ranks, &result.ranks);
            assert(big_sorted(result.big@)) by {
                assert forall|left: int, right: int| 0 <= left < right < result.big@.len() implies big_before(#[trigger] result.big@[left], #[trigger] result.big@[right]) by {}
            }
            assert(big_distinct(result.big@));
            assert forall|key: KeyView| #[trigger] result.groups.at(key).is_some() implies {
                let moment = result.groups.at(key).unwrap();
                &&& key.1 == Seq::<u8>::empty()
                &&& i64::MIN <= key.0 <= i64::MAX
                &&& moment.count != 0 || moment.value != 0
                &&& moment.count > 0 ==> if in_range(moment.value) {
                    ranks::rows(result.ranks).contains(Ranked { part: key.0 as i64, value: moment.value as i128 })
                } else { big_has(result.big@, key.0 as i64, moment.value) }
            } by {
                assert(result.groups.at(key) == self.groups.at(key));
                let moment = self.groups.at(key).unwrap();
                if moment.count > 0 && !in_range(moment.value) {
                    let position = choose|position: int| 0 <= position < self.big@.len() && self.big@[position].part == key.0 as i64 && self.big@[position].value@ == moment.value;
                    assert(result.big@[position].part == key.0 as i64 && result.big@[position].value@ == moment.value);
                }
            }
            assert forall|ranked: Ranked| #[trigger] ranks::rows(result.ranks).contains(ranked) implies {
                let moment = moment_at(result.groups, ranked.part as int);
                &&& moment.count > 0
                &&& moment.value == ranked.value as int
            } by { assert(result.groups.at((ranked.part as int, Seq::<u8>::empty())) == self.groups.at((ranked.part as int, Seq::<u8>::empty()))); }
            assert forall|position: int| 0 <= position < result.big@.len() implies {
                let moment = moment_at(result.groups, (#[trigger] result.big@[position]).part as int);
                &&& moment.count > 0
                &&& moment.value == result.big@[position].value@
                &&& !in_range(moment.value)
            } by { assert(result.groups.at((result.big@[position].part as int, Seq::<u8>::empty())) == self.groups.at((self.big@[position].part as int, Seq::<u8>::empty()))); }
        }
        result
    }
}


pub open spec fn partial_valid(inventory: Inventory, part: i64) -> bool {
    &&& inventory.groups.wf()
    &&& inventory.ranks.wf()
    &&& ranks::valid(inventory.ranks)
    &&& big_sorted(inventory.big@)
    &&& big_distinct(inventory.big@)
    &&& forall|key: KeyView| #[trigger] inventory.groups.at(key).is_some() ==> {
        let moment = inventory.groups.at(key).unwrap();
        &&& key.1 == Seq::<u8>::empty()
        &&& i64::MIN <= key.0 <= i64::MAX
        &&& moment.count != 0 || moment.value != 0
        &&& (key != (part as int, Seq::<u8>::empty()) && moment.count > 0) ==> if in_range(moment.value) {
            ranks::rows(inventory.ranks).contains(Ranked { part: key.0 as i64, value: moment.value as i128 })
        } else { big_has(inventory.big@, key.0 as i64, moment.value) }
    }
    &&& forall|ranked: Ranked| #[trigger] ranks::rows(inventory.ranks).contains(ranked) ==> {
        let moment = moment_at(inventory.groups, ranked.part as int);
        &&& ranked.part != part
        &&& moment.count > 0
        &&& moment.value == ranked.value as int
    }
    &&& forall|index: int| 0 <= index < inventory.big@.len() ==> {
        let moment = moment_at(inventory.groups, (#[trigger] inventory.big@[index]).part as int);
        &&& inventory.big@[index].part != part
        &&& moment.count > 0
        &&& moment.value == inventory.big@[index].value@
        &&& !in_range(moment.value)
    }
}

impl Inventory {
    /// Drops the rank entry of a part's previous moment.
    fn detach(&mut self, part: i64, previous: &Mom)
        requires old(self).valid(), previous@ == moment_at(old(self).groups, part as int),
        ensures partial_valid(*final(self), part), final(self).groups == old(self).groups,
    {
        let ghost original = *self;
        if previous.count.is_pos() {
            let value = previous.value.to_i128();
            match value {
                Some(value) => {
                    proof { assert(ranks::rows(self.ranks).contains(Ranked { part, value })); }
                    let removed = ranks::remove(&mut self.ranks, Ranked { part, value });
                },
                None => {
                    proof { assert(big_has(self.big@, part, previous.value@)); }
                    big_remove(&mut self.big, part);
                },
            }
        }
        proof {
            assert forall|key: KeyView| #[trigger] self.groups.at(key).is_some() implies {
                let moment = self.groups.at(key).unwrap();
                &&& key.1 == Seq::<u8>::empty()
                &&& i64::MIN <= key.0 <= i64::MAX
                &&& moment.count != 0 || moment.value != 0
                &&& (key != (part as int, Seq::<u8>::empty()) && moment.count > 0) ==> if in_range(moment.value) {
                    ranks::rows(self.ranks).contains(Ranked { part: key.0 as i64, value: moment.value as i128 })
                } else { big_has(self.big@, key.0 as i64, moment.value) }
            } by {
                let moment = self.groups.at(key).unwrap();
                if key != (part as int, Seq::<u8>::empty()) && moment.count > 0 && !in_range(moment.value) {
                    assert(big_has(original.big@, key.0 as i64, moment.value));
                }
            }
            assert forall|row: Ranked| #[trigger] ranks::rows(self.ranks).contains(row) implies {
                let moment = moment_at(self.groups, row.part as int);
                &&& row.part != part
                &&& moment.count > 0
                &&& moment.value == row.value as int
            } by {
                assert(ranks::rows(original.ranks).contains(row));
                if row.part == part && previous.count@ > 0 && in_range(previous.value@) {
                    assert(row == (Ranked { part, value: previous.value@ as i128 }));
                }
            }
            assert forall|index: int| 0 <= index < self.big@.len() implies {
                let moment = moment_at(self.groups, (#[trigger] self.big@[index]).part as int);
                &&& self.big@[index].part != part
                &&& moment.count > 0
                &&& moment.value == self.big@[index].value@
                &&& !in_range(moment.value)
            } by {
                let row = self.big@[index];
                assert(big_has(self.big@, row.part, row.value@));
                assert(big_has(original.big@, row.part, row.value@));
            }
        }
    }

    /// Adds the rank entry of a part's new moment.
    fn attach(&mut self, part: i64, next: &Mom)
        requires partial_valid(*old(self), part), moment_at(old(self).groups, part as int) == next@,
        ensures final(self).groups == old(self).groups,
            final(self).groups.wf(), final(self).ranks.wf(), ranks::valid(final(self).ranks),
            big_sorted(final(self).big@), big_distinct(final(self).big@),
            forall|key: KeyView| #[trigger] final(self).groups.at(key).is_some() ==> {
                let moment = final(self).groups.at(key).unwrap();
                &&& key.1 == Seq::<u8>::empty()
                &&& i64::MIN <= key.0 <= i64::MAX
                &&& moment.count != 0 || moment.value != 0
                &&& moment.count > 0 ==> if in_range(moment.value) {
                    ranks::rows(final(self).ranks).contains(Ranked { part: key.0 as i64, value: moment.value as i128 })
                } else { big_has(final(self).big@, key.0 as i64, moment.value) }
            },
            forall|ranked: Ranked| #[trigger] ranks::rows(final(self).ranks).contains(ranked) ==> {
                let moment = moment_at(final(self).groups, ranked.part as int);
                &&& moment.count > 0
                &&& moment.value == ranked.value as int
            },
            forall|index: int| 0 <= index < final(self).big@.len() ==> {
                let moment = moment_at(final(self).groups, (#[trigger] final(self).big@[index]).part as int);
                &&& moment.count > 0
                &&& moment.value == final(self).big@[index].value@
                &&& !in_range(moment.value)
            },
    {
        let ghost cleared = *self;
        if next.count.is_pos() {
            match next.value.to_i128() {
                Some(value) => {
                    proof { assert(!ranks::rows(self.ranks).contains(Ranked { part, value })); }
                    let inserted = ranks::insert(&mut self.ranks, Ranked { part, value });
                },
                None => {
                    proof { assert(!big_has_part(self.big@, part)); }
                    big_insert(&mut self.big, BigRow { part, value: next.value.dup() });
                },
            }
        }
        proof {
            assert forall|key: KeyView| #[trigger] self.groups.at(key).is_some() implies {
                let moment = self.groups.at(key).unwrap();
                &&& key.1 == Seq::<u8>::empty()
                &&& i64::MIN <= key.0 <= i64::MAX
                &&& moment.count != 0 || moment.value != 0
                &&& moment.count > 0 ==> if in_range(moment.value) {
                    ranks::rows(self.ranks).contains(Ranked { part: key.0 as i64, value: moment.value as i128 })
                } else { big_has(self.big@, key.0 as i64, moment.value) }
            } by {
                let moment = self.groups.at(key).unwrap();
                if key == (part as int, Seq::<u8>::empty()) {
                    assert(moment == next@);
                } else if moment.count > 0 && !in_range(moment.value) {
                    assert(big_has(cleared.big@, key.0 as i64, moment.value));
                }
            }
            assert forall|row: Ranked| #[trigger] ranks::rows(self.ranks).contains(row) implies {
                let moment = moment_at(self.groups, row.part as int);
                &&& moment.count > 0
                &&& moment.value == row.value as int
            } by {
                if !ranks::rows(cleared.ranks).contains(row) { assert(row.part == part); }
            }
            assert forall|index: int| 0 <= index < self.big@.len() implies {
                let moment = moment_at(self.groups, (#[trigger] self.big@[index]).part as int);
                &&& moment.count > 0
                &&& moment.value == self.big@[index].value@
                &&& !in_range(moment.value)
            } by {
                let row = self.big@[index];
                assert(big_has(self.big@, row.part, row.value@));
                if row.part != part { assert(big_has(cleared.big@, row.part, row.value@)); }
            }
        }
    }

    /// Rebinds a part's group to its new moment (the rank entries were detached).
    fn store_group(&mut self, part: i64, next: &Mom)
        requires partial_valid(*old(self), part),
        ensures partial_valid(*final(self), part), final(self).ranks == old(self).ranks, final(self).big@ == old(self).big@,
            moment_at(final(self).groups, part as int) == next@,
            forall|other: int| other != part ==> #[trigger] moment_at(final(self).groups, other) == moment_at(old(self).groups, other),
            sum(final(self).groups.entries(), |row: (KeyView, Moment)| row.1.count)
                == sum(old(self).groups.entries(), |row: (KeyView, Moment)| row.1.count) - moment_at(old(self).groups, part as int).count + next.count@,
            sum(final(self).groups.entries(), |row: (KeyView, Moment)| row.1.value)
                == sum(old(self).groups.entries(), |row: (KeyView, Moment)| row.1.value) - moment_at(old(self).groups, part as int).value + next.value@,
    {
        let ghost cleared = *self;
        if next.count.is_zero() && next.value.is_zero() {
            self.groups.remove(part);
            proof {
                packed::lemma_sum_remove(&cleared.groups, &self.groups, (part as int, Seq::<u8>::empty()), |row: (KeyView, Moment)| row.1.count);
                packed::lemma_sum_remove(&cleared.groups, &self.groups, (part as int, Seq::<u8>::empty()), |row: (KeyView, Moment)| row.1.value);
            }
        } else {
            self.groups.put(part, next);
            proof {
                packed::lemma_sum_put(&cleared.groups, &self.groups, (part as int, Seq::<u8>::empty()), next@, |row: (KeyView, Moment)| row.1.count);
                packed::lemma_sum_put(&cleared.groups, &self.groups, (part as int, Seq::<u8>::empty()), next@, |row: (KeyView, Moment)| row.1.value);
            }
        }
        proof {
            assert forall|other: int| other != part implies #[trigger] moment_at(self.groups, other) == moment_at(cleared.groups, other) by {}
            assert(moment_at(self.groups, part as int) == next@);
            assert forall|key: KeyView| #[trigger] self.groups.at(key).is_some() implies {
                let moment = self.groups.at(key).unwrap();
                &&& key.1 == Seq::<u8>::empty()
                &&& i64::MIN <= key.0 <= i64::MAX
                &&& moment.count != 0 || moment.value != 0
                &&& (key != (part as int, Seq::<u8>::empty()) && moment.count > 0) ==> if in_range(moment.value) {
                    ranks::rows(self.ranks).contains(Ranked { part: key.0 as i64, value: moment.value as i128 })
                } else { big_has(self.big@, key.0 as i64, moment.value) }
            } by {
                if key != (part as int, Seq::<u8>::empty()) {
                    assert(self.groups.at(key) == cleared.groups.at(key));
                    assert(cleared.groups.at(key).is_some());
                }
            }
            assert forall|ranked: Ranked| #[trigger] ranks::rows(self.ranks).contains(ranked) implies {
                let moment = moment_at(self.groups, ranked.part as int);
                &&& ranked.part != part
                &&& moment.count > 0
                &&& moment.value == ranked.value as int
            } by {
                assert(ranked.part != part);
                assert(moment_at(self.groups, ranked.part as int) == moment_at(cleared.groups, ranked.part as int));
            }
            assert forall|index: int| 0 <= index < self.big@.len() implies {
                let moment = moment_at(self.groups, (#[trigger] self.big@[index]).part as int);
                &&& self.big@[index].part != part
                &&& moment.count > 0
                &&& moment.value == self.big@[index].value@
                &&& !in_range(moment.value)
            } by {
                assert(self.big@[index].part != part);
                assert(moment_at(self.groups, self.big@[index].part as int) == moment_at(cleared.groups, self.big@[index].part as int));
            }
        }
    }

    pub fn update(&mut self, part: i64, change: &Mom)
        requires old(self).valid(),
        ensures final(self).valid(),
            final(self).total.count@ == old(self).total.count@ + change.count@,
            final(self).total.value@ == old(self).total.value@ + change.value@,
            forall|other: int| other != part ==> #[trigger] moment_at(final(self).groups, other) == moment_at(old(self).groups, other),
            moment_at(final(self).groups, part as int).count == moment_at(old(self).groups, part as int).count + change.count@,
            moment_at(final(self).groups, part as int).value == moment_at(old(self).groups, part as int).value + change.value@,
    {
        let ghost original = *self;
        let previous = match self.groups.get(part) { Some(moment) => moment, None => zero() };
        let next = plus(&previous, change);
        let total = plus(&self.total, change);
        proof { assert(previous@ == moment_at(original.groups, part as int)); }
        self.detach(part, &previous);
        self.store_group(part, &next);
        self.attach(part, &next);
        self.total = total;
    }
}

pub open spec fn nonneg(inventory: Inventory) -> bool {
    forall|key: KeyView| #[trigger] inventory.groups.at(key).is_some() ==> inventory.groups.at(key).unwrap().count >= 0
}

pub open spec fn ranked_view(rows: Seq<Ranked>) -> Seq<(i64, int)> {
    rows.map_values(|row: Ranked| (row.part, row.value as int))
}

pub open spec fn big_view(rows: Seq<BigRow>) -> Seq<(i64, int)> {
    rows.map_values(|row: BigRow| (row.part, row.value@))
}

pub open spec fn row_before(left: (i64, int), right: (i64, int)) -> bool {
    left.1 > right.1 || (left.1 == right.1 && left.0 < right.0)
}

pub open spec fn split_ok(big: Seq<BigRow>, split: int) -> bool {
    &&& 0 <= split <= big.len()
    &&& forall|index: int| 0 <= index < split ==> (#[trigger] big[index]).value@ > i128::MAX
    &&& forall|index: int| split <= index < big.len() ==> (#[trigger] big[index]).value@ < i128::MIN
}

pub open spec fn all_rows(inventory: Inventory, split: int) -> Seq<(i64, int)> {
    big_view(inventory.big@).take(split) + ranked_view(ranks::rows(inventory.ranks)) + big_view(inventory.big@).skip(split)
}

pub open spec fn emission_prefix(inventory: Inventory, split: int, weight: int, output: Seq<(i64, Wide)>, limit: int) -> bool {
    &&& output.len() <= all_rows(inventory, split).len()
    &&& forall|position: int| 0 <= position < output.len() ==> {
        let source = all_rows(inventory, split)[position];
        let row = #[trigger] output[position];
        &&& row.0 == source.0
        &&& row.1@ == weight * source.1
        &&& ranks::passes(source.1, limit)
    }
}

pub open spec fn exact_rows(output: Seq<(i64, Wide)>) -> Seq<(i64, int)> {
    output.map_values(|row: (i64, Wide)| (row.0, row.1@))
}


pub proof fn lemma_all_len(inventory: Inventory, split: int)
    requires 0 <= split <= inventory.big@.len(),
    ensures all_rows(inventory, split).len() == inventory.big@.len() + ranks::rows(inventory.ranks).len(),
{
    assert(big_view(inventory.big@).len() == inventory.big@.len());
}

pub proof fn lemma_all_low(inventory: Inventory, split: int, index: int)
    requires 0 <= index < split <= inventory.big@.len(),
    ensures all_rows(inventory, split)[index] == (inventory.big@[index].part, inventory.big@[index].value@),
{
    assert(big_view(inventory.big@).len() == inventory.big@.len());
}

pub proof fn lemma_all_ranked(inventory: Inventory, split: int, offset: int)
    requires 0 <= split <= inventory.big@.len(), 0 <= offset < ranks::rows(inventory.ranks).len(),
    ensures all_rows(inventory, split)[split + offset]
        == (ranks::rows(inventory.ranks)[offset].part, ranks::rows(inventory.ranks)[offset].value as int),
{
    assert(big_view(inventory.big@).len() == inventory.big@.len());
}

pub proof fn lemma_all_high(inventory: Inventory, split: int, index: int)
    requires 0 <= split <= index < inventory.big@.len(),
    ensures all_rows(inventory, split)[ranks::rows(inventory.ranks).len() + index]
        == (inventory.big@[index].part, inventory.big@[index].value@),
{
    assert(big_view(inventory.big@).len() == inventory.big@.len());
}

pub proof fn lemma_split_exists(inventory: Inventory, split: int)
    requires inventory.valid(), 0 <= split <= inventory.big@.len(),
        forall|index: int| 0 <= index < split ==> (#[trigger] inventory.big@[index]).value@ > i128::MAX,
        split < inventory.big@.len() ==> inventory.big@[split].value@ < i128::MIN,
    ensures split_ok(inventory.big@, split),
{
    let big = inventory.big@;
    assert forall|index: int| split <= index < big.len() implies (#[trigger] big[index]).value@ < i128::MIN by {
        let moment = moment_at(inventory.groups, big[index].part as int);
        if index > split { assert(big_before(big[split], big[index])); }
    }
}

pub proof fn lemma_all_sorted(inventory: Inventory, split: int)
    requires inventory.valid(), split_ok(inventory.big@, split),
    ensures forall|left: int, right: int| 0 <= left < right < all_rows(inventory, split).len() ==>
        row_before(#[trigger] all_rows(inventory, split)[left], #[trigger] all_rows(inventory, split)[right]),
{
    ranks::lemma_rows_sorted(&inventory.ranks);
    let big = inventory.big@;
    let ranked = ranks::rows(inventory.ranks);
    let all = all_rows(inventory, split);
    let n = ranked.len() as int;
    lemma_all_len(inventory, split);
    assert forall|index: int| 0 <= index < all.len() implies {
        &&& index < split ==> all[index] == (big[index].part, big[index].value@)
        &&& split <= index < split + n ==> all[index] == (ranked[index - split].part, ranked[index - split].value as int)
        &&& index >= split + n ==> all[index] == (big[index - n].part, big[index - n].value@)
    } by {
        if index < split { lemma_all_low(inventory, split, index); }
        else if index < split + n { lemma_all_ranked(inventory, split, index - split); }
        else { lemma_all_high(inventory, split, index - n); }
    }
    assert forall|left: int, right: int| 0 <= left < right < all.len() implies row_before(#[trigger] all[left], #[trigger] all[right]) by {
        if right < split {
            assert(big_before(big[left], big[right]));
        } else if left >= split + n {
            assert(big_before(big[left - n], big[right - n]));
        } else if left < split && right >= split + n {
            assert(big[left].value@ > i128::MAX);
            assert(big[right - n].value@ < i128::MIN);
        } else if left < split {
            assert(big[left].value@ > i128::MAX);
            assert(ranked.contains(ranked[right - split]));
        } else if right >= split + n {
            assert(big[right - n].value@ < i128::MIN);
            assert(ranked.contains(ranked[left - split]));
        } else {
            assert(ranks::before(ranked[left - split], ranked[right - split]));
        }
    }
}

pub proof fn lemma_positive_group(inventory: &Inventory, part: int)
    requires inventory.valid(), nonneg(*inventory), moment_at(inventory.groups, part).count > 0,
    ensures inventory.total.count@ > 0,
{
    inventory.groups.model();
    let key = (part, Seq::<u8>::empty());
    assert(inventory.groups.at(key).is_some());
    let position = choose|position: int| 0 <= position < inventory.groups.entries().len()
        && (#[trigger] inventory.groups.entries()[position]).0 == key;
    assert forall|offset: int| 0 <= offset < inventory.groups.entries().len() implies
        (#[trigger] inventory.groups.entries()[offset]).1.count >= 0 by {
        assert(inventory.groups.at(inventory.groups.entries()[offset].0) == Some(inventory.groups.entries()[offset].1));
    }
    verus_common::sums::lemma_sum_pos(inventory.groups.entries(), |row: (KeyView, Moment)| row.1.count, position);
}

pub proof fn lemma_empty_inventory(inventory: &Inventory)
    requires inventory.valid(), inventory.groups.entries().len() == 0,
    ensures inventory.total@ == zero_moment(),
        forall|part: int| #[trigger] moment_at(inventory.groups, part) == zero_moment(),
{
    inventory.groups.model();
    assert(inventory.groups.entries() =~= Seq::<(KeyView, Moment)>::empty());
    assert forall|part: int| #[trigger] moment_at(inventory.groups, part) == zero_moment() by {
        assert(inventory.groups.at((part, Seq::<u8>::empty())).is_none());
    }
}

pub proof fn lemma_all_members(inventory: Inventory, split: int, position: int)
    requires inventory.valid(), split_ok(inventory.big@, split), 0 <= position < all_rows(inventory, split).len(),
    ensures {
        let row = all_rows(inventory, split)[position];
        let moment = moment_at(inventory.groups, row.0 as int);
        &&& moment.count > 0
        &&& moment.value == row.1
    },
{
    let big = inventory.big@;
    let ranked = ranks::rows(inventory.ranks);
    let n = ranked.len() as int;
    lemma_all_len(inventory, split);
    if position < split {
        lemma_all_low(inventory, split, position);
    } else if position < split + n {
        lemma_all_ranked(inventory, split, position - split);
        assert(ranked.contains(ranked[position - split]));
    } else {
        lemma_all_high(inventory, split, position - n);
    }
}

pub proof fn lemma_part_position(inventory: Inventory, split: int, part: i64)
    requires inventory.valid(), split_ok(inventory.big@, split), moment_at(inventory.groups, part as int).count > 0,
    ensures exists|position: int| 0 <= position < all_rows(inventory, split).len() && (#[trigger] all_rows(inventory, split)[position]).0 == part,
{
    let big = inventory.big@;
    let ranked = ranks::rows(inventory.ranks);
    let n = ranked.len() as int;
    let moment = moment_at(inventory.groups, part as int);
    lemma_all_len(inventory, split);
    assert(inventory.groups.at((part as int, Seq::<u8>::empty())).is_some());
    if in_range(moment.value) {
        let index = choose|index: int| 0 <= index < ranked.len() && ranked[index] == (Ranked { part, value: moment.value as i128 });
        lemma_all_ranked(inventory, split, index);
        assert(0 <= split + index < all_rows(inventory, split).len());
    } else {
        let index = choose|index: int| 0 <= index < big.len() && big[index].part == part && big[index].value@ == moment.value;
        if index < split { lemma_all_low(inventory, split, index); }
        else { lemma_all_high(inventory, split, index); }
    }
}

#[verifier::rlimit(100)]
pub proof fn lemma_emission_answer(inventory: Inventory, split: int, weight: int, output: Seq<(i64, Wide)>, fraction: int)
    requires inventory.valid(), nonneg(inventory), weight > 0, split_ok(inventory.big@, split),
        emission_prefix(inventory, split, weight, output, inventory.total.value@ * fraction),
        output.len() == all_rows(inventory, split).len()
            || !ranks::passes(all_rows(inventory, split)[output.len() as int].1, inventory.total.value@ * fraction),
    ensures crate::semantics::inventory_answer(inventory, weight, exact_rows(output), fraction),
{
    lemma_all_sorted(inventory, split);
    let all = all_rows(inventory, split);
    let limit = inventory.total.value@ * fraction;
    let rows = exact_rows(output);
    assert forall|position: int| 0 <= position < rows.len() implies {
        let row = #[trigger] rows[position];
        &&& moment_at(inventory.groups, row.0 as int).count > 0
        &&& row.1 == weight * moment_at(inventory.groups, row.0 as int).value
        &&& inventory.total.count@ > 0
        &&& 1000000000000 * moment_at(inventory.groups, row.0 as int).value > limit
    } by {
        lemma_all_members(inventory, split, position);
        lemma_positive_group(&inventory, output[position].0 as int);
    }
    assert forall|left: int, right: int| 0 <= left < right < rows.len() implies
        (#[trigger] rows[left]).0 != (#[trigger] rows[right]).0 && rows[left].1 >= rows[right].1 by {
        lemma_all_members(inventory, split, left);
        lemma_all_members(inventory, split, right);
        assert(row_before(all[left], all[right]));
        if rows[left].0 == rows[right].0 { assert(all[left].1 == all[right].1); }
        assert(rows[left].1 >= rows[right].1) by (nonlinear_arith)
            requires weight > 0, all[left].1 >= all[right].1, rows[left].1 == weight * all[left].1, rows[right].1 == weight * all[right].1;
    }
    assert forall|part: i64| #[trigger] moment_at(inventory.groups, part as int).count > 0
        && 1000000000000 * moment_at(inventory.groups, part as int).value > limit implies
        exists|position: int| 0 <= position < rows.len() && (#[trigger] rows[position]).0 == part by {
        lemma_part_position(inventory, split, part);
        let position = choose|position: int| 0 <= position < all.len() && (#[trigger] all[position]).0 == part;
        lemma_all_members(inventory, split, position);
        if position >= output.len() {
            assert(output.len() < all.len());
            let first = output.len() as int;
            if first < position { assert(row_before(all[first], all[position])); }
            assert(ranks::passes(all[first].1, limit));
        }
        assert(position < output.len());
        assert(rows[position].0 == part);
    }
}


pub fn passes_wide(value: &Wide, limit: &Wide) -> (result: bool)
    ensures result == ranks::passes(value@, limit@),
{
    let scaled = value.mul(&Wide::from_i128(1000000000000i128));
    limit.lt(&scaled)
}

pub fn positive_count(big: &Vec<BigRow>) -> (split: usize)
    requires big_sorted(big@), forall|index: int| 0 <= index < big@.len() ==> !in_range((#[trigger] big@[index]).value@),
    ensures split_ok(big@, split as int),
{
    let mut split: usize = 0;
    while split < big.len() && big[split].value.is_pos()
        invariant split <= big.len(), forall|index: int| 0 <= index < split ==> (#[trigger] big@[index]).value@ > i128::MAX,
            forall|index: int| 0 <= index < big@.len() ==> !in_range((#[trigger] big@[index]).value@),
        decreases big.len() - split,
    {
        split += 1;
    }
    proof {
        assert forall|index: int| split <= index < big@.len() implies (#[trigger] big@[index]).value@ < i128::MIN by {
            if index > split as int { assert(big_before(big@[split as int], big@[index])); }
        }
    }
    split
}

pub fn emit(inventory: &Inventory, weight: &Wide, fraction: i64) -> (result: Vec<(i64, Wide)>)
    requires inventory.valid(), nonneg(*inventory), weight@ > 0,
    ensures crate::semantics::inventory_answer(*inventory, weight@, exact_rows(result@), fraction as int),
{
    proof {
        assert forall|index: int| 0 <= index < inventory.big@.len() implies !in_range((#[trigger] inventory.big@[index]).value@) by {}
    }
    let split = positive_count(&inventory.big);
    let limit = inventory.total.value.mul(&Wide::from_i128(fraction as i128));
    let ghost all = all_rows(*inventory, split as int);
    let ghost total_limit = inventory.total.value@ * fraction as int;
    proof { assert(total_limit == inventory.total.value@ * fraction as int); }
    let mut result: Vec<(i64, Wide)> = Vec::new();
    let mut index: usize = 0;
    while index < split
        invariant index <= split, split <= inventory.big.len(), split_ok(inventory.big@, split as int),
            inventory.valid(), nonneg(*inventory), weight@ > 0, limit@ == total_limit, total_limit == inventory.total.value@ * fraction as int, all == all_rows(*inventory, split as int),
            result.len() == index,
            emission_prefix(*inventory, split as int, weight@, result@, total_limit),
        decreases split - index,
    {
        let row = &inventory.big[index];
        proof {
            lemma_all_len(*inventory, split as int);
            lemma_all_low(*inventory, split as int, index as int);
        }
        if !passes_wide(&row.value, &limit) {
            proof {
                assert(!ranks::passes(all[result.len() as int].1, total_limit));
                assert(total_limit == inventory.total.value@ * fraction as int);
                assert(all == all_rows(*inventory, split as int));
                assert(result@.len() == result.len());
                assert(all[result@.len() as int] == all_rows(*inventory, split as int)[result@.len() as int]);
                assert(!ranks::passes(all_rows(*inventory, split as int)[result@.len() as int].1, inventory.total.value@ * fraction as int));
                assert(result@.len() == all_rows(*inventory, split as int).len() || !ranks::passes(all_rows(*inventory, split as int)[result@.len() as int].1, inventory.total.value@ * fraction as int));
                assert(emission_prefix(*inventory, split as int, weight@, result@, inventory.total.value@ * fraction as int));
                lemma_emission_answer(*inventory, split as int, weight@, result@, fraction as int);
            }
            return result;
        }
        let value = weight.mul(&row.value);
        result.push((row.part, value));
        index += 1;
    }
    proof { ranks::lemma_rows_sorted(&inventory.ranks); }
    let ghost mapped = inventory.ranks.entries().map_values(|entry: (KeyView, crate::ranks::Page)| entry.1.rows@);
    let mut cursor = inventory.ranks.cursor();
    let mut processed = Ghost(0int);
    loop
        invariant
            inventory.valid(), nonneg(*inventory), inventory.ranks.wf(), weight@ > 0, split_ok(inventory.big@, split as int),
            limit@ == total_limit, total_limit == inventory.total.value@ * fraction as int, all == all_rows(*inventory, split as int),
            mapped == inventory.ranks.entries().map_values(|entry: (KeyView, crate::ranks::Page)| entry.1.rows@),
            0 <= processed@ <= inventory.ranks.entries().len(),
            cursor.remaining() == inventory.ranks.entries().skip(processed@),
            result.len() == split + verus_common::rel::flat(mapped.take(processed@)).len(),
            emission_prefix(*inventory, split as int, weight@, result@, total_limit),
        ensures result.len() == split + ranks::rows(inventory.ranks).len(),
            emission_prefix(*inventory, split as int, weight@, result@, total_limit),
        decreases cursor.remaining().len(),
    {
        let (fence, page) = match cursor.next() {
            Some(row) => row,
            None => {
                proof {
                    assert(processed@ == mapped.len());
                    assert(mapped.take(processed@) =~= mapped);
                    assert(ranks::rows(inventory.ranks) == verus_common::rel::flat(mapped));
                }
                break;
            },
        };
        proof {
            inventory.ranks.model();
            assert(inventory.ranks.at(fence.view()) == Some(*page));
        }
        let count = ranks::qualifying_prefix(&page.rows, &limit);
        let mut position: usize = 0;
        let ghost prefix = result@;
        proof {
            verus_prooflib::lemma_flat_take_step(mapped, processed@);
            verus_prooflib::lemma_flat_take_prefix(mapped, processed@ + 1);
            assert(mapped[processed@] == page.rows@);
            assert forall|offset: int| 0 <= offset < page.rows.len() implies
                #[trigger] ranks::rows(inventory.ranks)[prefix.len() - split as int + offset] == page.rows@[offset] by {
                assert(verus_common::rel::flat(mapped.take(processed@ + 1))[prefix.len() - split as int + offset] == page.rows@[offset]);
            }
        }
        while position < count
            invariant
                position <= count <= page.rows.len(), inventory.valid(), nonneg(*inventory), weight@ > 0,
                split_ok(inventory.big@, split as int),
                limit@ == total_limit, total_limit == inventory.total.value@ * fraction as int, all == all_rows(*inventory, split as int),
                result.len() == prefix.len() + position, prefix.len() >= split,
                emission_prefix(*inventory, split as int, weight@, result@, total_limit),
                prefix.len() - split + page.rows.len() <= ranks::rows(inventory.ranks).len(),
                forall|offset: int| 0 <= offset < page.rows.len() ==>
                    #[trigger] ranks::rows(inventory.ranks)[prefix.len() - split as int + offset] == page.rows@[offset],
                forall|offset: int| 0 <= offset < count ==> ranks::passes((#[trigger] page.rows@[offset]).value as int, total_limit),
            decreases count - position,
        {
            let ranked = &page.rows[position];
            let value = weight.mul(&Wide::from_i128(ranked.value));
            let ghost source_index = prefix.len() - split as int + position as int;
            proof {
                assert(0 <= source_index < ranks::rows(inventory.ranks).len());
                assert(ranks::rows(inventory.ranks)[source_index] == page.rows@[position as int]);
                lemma_all_ranked(*inventory, split as int, source_index);
            }
            result.push((ranked.part, value));
            position += 1;
        }
        if count < page.rows.len() {
            proof {
                assert(ranks::rows(inventory.ranks)[prefix.len() - split as int + count as int] == page.rows@[count as int]);
                lemma_all_ranked(*inventory, split as int, prefix.len() - split as int + count as int);
                assert(!ranks::passes(all[result.len() as int].1, total_limit));
                lemma_emission_answer(*inventory, split as int, weight@, result@, fraction as int);
            }
            return result;
        }
        processed = Ghost(processed@ + 1);
    }
    proof { lemma_all_len(*inventory, split as int); }
    let mut index = split;
    while index < inventory.big.len()
        invariant split <= index <= inventory.big.len(), split_ok(inventory.big@, split as int),
            inventory.valid(), nonneg(*inventory), weight@ > 0, limit@ == total_limit, total_limit == inventory.total.value@ * fraction as int, all == all_rows(*inventory, split as int),
            result.len() == index + (all.len() - inventory.big.len()),
            emission_prefix(*inventory, split as int, weight@, result@, total_limit),
        decreases inventory.big.len() - index,
    {
        let row = &inventory.big[index];
        proof {
            lemma_all_len(*inventory, split as int);
            lemma_all_high(*inventory, split as int, index as int);
        }
        if !passes_wide(&row.value, &limit) {
            proof { assert(!ranks::passes(all[result.len() as int].1, total_limit)); lemma_emission_answer(*inventory, split as int, weight@, result@, fraction as int); }
            return result;
        }
        let value = weight.mul(&row.value);
        result.push((row.part, value));
        index += 1;
    }
    proof { lemma_all_len(*inventory, split as int); lemma_emission_answer(*inventory, split as int, weight@, result@, fraction as int); }
    result
}

}
