use vstd::prelude::*;
use vstd::hash_map::HashMapWithView;
use std::sync::Arc;
use verus_prooflib::ordered::{Key, KeyView, Index};
use verus_common::sums::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::model::*;
use crate::values::*;
use crate::values::add;

verus! {
#[derive(Clone, Copy)]
pub struct Change { pub revenue: i64, pub date: i32, pub count: i32 }
pub open spec fn changes_shape(days: Seq<Change>, bin: int) -> bool {
    days.len() <= 256 &&
    (forall|position: int| 0 <= position < days.len() ==> bucket((#[trigger] days[position]).date as int) == bin) &&
    (forall|left: int, right: int| #![trigger days[left], days[right]]
        0 <= left < right < days.len() ==> days[left].date < days[right].date)
}
pub open spec fn changes_count(days: Seq<Change>, date: i32) -> int {
    sum(days, |change: Change| if change.date == date { change.count as int } else { 0int })
}
pub open spec fn changes_revenue(days: Seq<Change>, date: i32) -> int {
    sum(days, |change: Change| if change.date == date { change.revenue as int } else { 0int })
}
pub open spec fn change_count_int(days: Seq<Change>, date: int) -> int {
    sum(days, |change: Change| if change.date as int == date { change.count as int } else { 0int })
}
pub open spec fn change_revenue_int(days: Seq<Change>, date: int) -> int {
    sum(days, |change: Change| if change.date as int == date { change.revenue as int } else { 0int })
}
pub open spec fn previous_days(previous: Option<Arc<BucketProfile>>) -> Seq<Day> {
    match previous { Some(value) => value.days@, None => Seq::empty() }
}
pub open spec fn processed_cell(keys: Seq<(i64, i64)>, count: int, supplier: int, date: int) -> bool {
    i64::MIN <= supplier <= i64::MAX && i32::MIN <= date <= i32::MAX &&
        keys.take(count).contains((bucket(date) as i64, supplier as i64))
}
pub struct Scratch {
    pub groups: HashMapWithView<i64, HashMapWithView<i64, Vec<Change>>>,
    pub keys: Vec<(i64, i64)>,
}
impl Scratch {
    pub open spec fn count(self, supplier: i64, date: i32) -> int {
        let bin = bucket(date as int) as i64;
        if self.groups@.contains_key(bin) && self.groups@[bin]@.contains_key(supplier) {
            changes_count(self.groups@[bin]@[supplier]@, date)
        } else { 0 }
    }
    pub open spec fn revenue(self, supplier: i64, date: i32) -> int {
        let bin = bucket(date as int) as i64;
        if self.groups@.contains_key(bin) && self.groups@[bin]@.contains_key(supplier) {
            changes_revenue(self.groups@[bin]@[supplier]@, date)
        } else { 0 }
    }
    pub open spec fn count_int(self, supplier: int, date: int) -> int {
        if i64::MIN <= supplier <= i64::MAX && i32::MIN <= date <= i32::MAX { self.count(supplier as i64, date as i32) } else { 0 }
    }
    pub open spec fn revenue_int(self, supplier: int, date: int) -> int {
        if i64::MIN <= supplier <= i64::MAX && i32::MIN <= date <= i32::MAX { self.revenue(supplier as i64, date as i32) } else { 0 }
    }
    pub open spec fn well_formed(self) -> bool {
        &&& self.keys@.no_duplicates()
        &&& forall|bin: i64, supplier: i64| #[trigger] self.keys@.contains((bin, supplier)) ==
            (self.groups@.contains_key(bin) && self.groups@[bin]@.contains_key(supplier))
        &&& forall|bin: i64, supplier: i64| self.groups@.contains_key(bin) && self.groups@[bin]@.contains_key(supplier) ==> (
            changes_shape((#[trigger] self.groups@[bin]@[supplier])@, bin as int))
    }
    pub open spec fn matches_lines(self, rows: Seq<LineitemRow>) -> bool {
        self.well_formed() &&
        (forall|supplier: i64, date: i32| #[trigger] self.count(supplier, date) == day_count(rows, supplier as int, date as int)) &&
        (forall|supplier: i64, date: i32| #[trigger] self.revenue(supplier, date) == day_revenue(rows, supplier as int, date as int))
    }
    pub open spec fn matches_delta(self, rows: Seq<(LineitemRow, int)>) -> bool {
        self.well_formed() && (forall|supplier: i64, date: i32| #[trigger] self.count(supplier, date) ==
            sum(rows, |change: (LineitemRow, int)| if change.0.l_suppkey == supplier as int && change.0.l_shipdate == date as int { change.1 } else { 0int })) &&
        (forall|supplier: i64, date: i32| #[trigger] self.revenue(supplier, date) ==
            sum(rows, |change: (LineitemRow, int)| if change.0.l_suppkey == supplier as int && change.0.l_shipdate == date as int { change.1 * weight(change.0) } else { 0int }))
    }
    pub fn new() -> (result: Self)
        ensures result.matches_lines(Seq::empty()), result.matches_delta(Seq::empty()),
    {
        Scratch { groups: HashMapWithView::new(), keys: Vec::new() }
    }
    pub fn add(&mut self, row: &LineInput, sign: i64) -> (result: bool)
        requires old(self).well_formed(),
        ensures result ==> final(self).well_formed(),
            result ==> forall|supplier: i64, date: i32| #[trigger] final(self).count(supplier, date) == old(self).count(supplier, date) +
                    if supplier == row.l_suppkey && date == row.l_shipdate { sign as int } else { 0int },
            result ==> forall|supplier: i64, date: i32| #[trigger] final(self).revenue(supplier, date) == old(self).revenue(supplier, date) +
                if supplier == row.l_suppkey && date == row.l_shipdate { sign as int * weight(line_input_view(*row)) } else { 0int },
    {
        let ghost before = *self;
        let bin = bin(row.l_shipdate);
        let revenue = match multiply(row.l_extendedprice as i128, 100i128 - row.l_discount as i128) {
            Some(value) => value, None => return false,
        };
        let ghost product = revenue as int;
        let revenue = match multiply(revenue, sign as i128) { Some(value) => value, None => return false };
        proof {
            assert(product == weight(line_input_view(*row)));
            assert(revenue as int == sign as int * weight(line_input_view(*row))) by (nonlinear_arith)
                requires product == weight(line_input_view(*row)), revenue as int == product * sign as int;
        }
        let mut suppliers = match self.groups.remove(&bin) { Some(inner) => inner, None => HashMapWithView::new() };
        let days = match suppliers.remove(&row.l_suppkey) {
            Some(days) => days,
            None => {
                proof {
                    assert(!self.keys@.contains((bin, row.l_suppkey)));
                    verus_prooflib::lemma_unique_push(self.keys@, (bin, row.l_suppkey));
                    verus_prooflib::lemma_push_membership(self.keys@, (bin, row.l_suppkey));
                }
                self.keys.push((bin, row.l_suppkey));
                Vec::new()
            },
        };
        let ghost previous_days = days@;
        let days = match update_changes(days, row.l_shipdate, sign as i128, revenue, Ghost(bin as int)) {
            Some(days) => days, None => return false,
        };
        let ghost updated_days = days@;
        suppliers.insert(row.l_suppkey, days);
        self.groups.insert(bin, suppliers);
        proof {
            assert forall|query_bin: i64, supplier: i64| #[trigger] self.keys@.contains((query_bin, supplier)) ==
                (self.groups@.contains_key(query_bin) && self.groups@[query_bin]@.contains_key(supplier)) by {
                if query_bin != bin {
                    assert(self.groups@.contains_key(query_bin) == before.groups@.contains_key(query_bin));
                } else if supplier != row.l_suppkey {
                    if before.groups@.contains_key(bin) {
                        assert(self.groups@[bin]@.contains_key(supplier) == before.groups@[bin]@.contains_key(supplier));
                    }
                }
            }
            assert forall|query_bin: i64, supplier: i64| self.groups@.contains_key(query_bin) && self.groups@[query_bin]@.contains_key(supplier) implies
                changes_shape((#[trigger] self.groups@[query_bin]@[supplier])@, query_bin as int) by {
                if query_bin == bin && supplier == row.l_suppkey {
                    assert(self.groups@[bin]@[supplier]@ == updated_days);
                } else {
                    assert(before.groups@.contains_key(query_bin));
                    assert(before.groups@[query_bin]@.contains_key(supplier));
                    assert(self.groups@[query_bin]@[supplier]@ == before.groups@[query_bin]@[supplier]@);
                }
            }
            assert forall|supplier: i64, date: i32| #[trigger] self.count(supplier, date) == before.count(supplier, date) +
                if supplier == row.l_suppkey && date == row.l_shipdate { sign as int } else { 0int } by {
                let query_bin = bucket(date as int) as i64;
                if query_bin == bin && supplier == row.l_suppkey {
                    assert(self.groups@[bin]@[supplier]@ == updated_days);
                    assert(before.count(supplier, date) == changes_count(previous_days, date));
                }
                if supplier == row.l_suppkey && date == row.l_shipdate { assert(query_bin == bin); }
            }
            assert forall|supplier: i64, date: i32| #[trigger] self.revenue(supplier, date) == before.revenue(supplier, date) +
                if supplier == row.l_suppkey && date == row.l_shipdate { sign as int * weight(line_input_view(*row)) } else { 0int } by {
                let query_bin = bucket(date as int) as i64;
                if query_bin == bin && supplier == row.l_suppkey {
                    assert(self.groups@[bin]@[supplier]@ == updated_days);
                    assert(before.revenue(supplier, date) == changes_revenue(previous_days, date));
                }
                if supplier == row.l_suppkey && date == row.l_shipdate { assert(query_bin == bin); }
            }
        }
        true
    }
    pub fn finish(&self, base: &Facts) -> (result: Option<Facts>)
        requires self.well_formed(), facts_shape(*base),
        ensures result matches Some(root) ==> facts_shape(root),
            result matches Some(root) ==> forall|supplier: int, date: int| #[trigger] stored_day_count(root, supplier, date) ==
                stored_day_count(*base, supplier, date) + self.count_int(supplier, date),
            result matches Some(root) ==> forall|supplier: int, date: int| #[trigger] stored_day_revenue(root, supplier, date) ==
                stored_day_revenue(*base, supplier, date) + self.revenue_int(supplier, date),
    {
        let ghost original = *self;
        proof { assert(original.keys@.take(0) =~= Seq::<(i64, i64)>::empty()); }
        let mut result = base.share();
        proof {
            assert forall|key: KeyView| #[trigger] result.at(key) == base.at(key) by { }
            assert forall|bin: int, supplier: int| #[trigger] profile_at(result, bin, supplier) == profile_at(*base, bin, supplier) by { }
            assert(facts_shape(result));
        }
        let mut position = 0;
        while position < self.keys.len()
            invariant position <= self.keys.len(), *self == original, original.well_formed(), facts_shape(*base), facts_shape(result),
                forall|supplier: int, date: int| #[trigger] stored_day_count(result, supplier, date) == stored_day_count(*base, supplier, date) +
                    if processed_cell(original.keys@, position as int, supplier, date) { original.count_int(supplier, date) } else { 0int },
                forall|supplier: int, date: int| #[trigger] stored_day_revenue(result, supplier, date) == stored_day_revenue(*base, supplier, date) +
                    if processed_cell(original.keys@, position as int, supplier, date) { original.revenue_int(supplier, date) } else { 0int },
            decreases self.keys.len() - position,
        {
            let (bin, supplier) = self.keys[position];
            proof {
                assert(original.keys@.contains((bin, supplier)));
                if original.keys@.take(position as int).contains((bin, supplier)) {
                    let other = choose|other: int| 0 <= other < position && (#[trigger] original.keys@[other]) == (bin, supplier);
                    assert(original.keys@[other] != original.keys@[position as int]);
                    assert(false);
                }
            }
            let scratch = match self.groups.get(&bin) { Some(value) => value, None => return None };
            let changes = match scratch.get(&supplier) { Some(value) => value, None => return None };
            let bin_key = Key::number(bin);
            let supplier_key = Key::number(supplier);
            let previous = match result.get(&bin_key) { Some(value) => value.get(&supplier_key), None => None };
            let ghost before = result;
            let ghost selected = changes@;
            proof {
                assert(selected == original.groups@[bin]@[supplier]@);
                assert(changes_shape(selected, bin as int));
                assert(profile_at(result, bin as int, supplier as int) == match previous { Some(value) => Some(*value), None => None });
                assert(previous is Some ==> profile_shape(*previous->0, bin as int));
            }
            let profile = match merge_profile(&previous, changes, Ghost(bin as int)) { Some(value) => value, None => return None };
            let ghost replacement = profile;
            install_profile(&mut result, bin, supplier, profile);
            proof {
                assert(original.keys@.take(position as int + 1) =~= original.keys@.take(position as int).push((bin, supplier)));
                verus_prooflib::lemma_push_membership(original.keys@.take(position as int), (bin, supplier));
                assert forall|query_supplier: int, date: int| #[trigger] stored_day_count(result, query_supplier, date) == stored_day_count(*base, query_supplier, date) +
                    if processed_cell(original.keys@, position as int + 1, query_supplier, date) { original.count_int(query_supplier, date) } else { 0int } by {
                    if query_supplier == supplier as int && bucket(date) == bin as int {
                        assert(stored_day_count(before, query_supplier, date) == days_count(previous_days(previous), date));
                        assert(stored_day_count(result, query_supplier, date) == days_count(replacement.days@, date));
                        assert(days_count(replacement.days@, date) == days_count(previous_days(previous), date) + change_count_int(selected, date));
                        if i32::MIN <= date <= i32::MAX {
                            lemma_sum_ext_all(selected, |change: Change| if change.date == date as i32 { change.count as int } else { 0int },
                                |change: Change| if change.date as int == date { change.count as int } else { 0int });
                            assert(original.count_int(query_supplier, date) == change_count_int(selected, date));
                        } else {
                            lemma_changes_outside(selected, date);
                        }
                    } else {
                        assert(profile_at(result, bucket(date), query_supplier) == profile_at(before, bucket(date), query_supplier));
                        assert(stored_day_count(result, query_supplier, date) == stored_day_count(before, query_supplier, date));
                    }
                }
                assert forall|query_supplier: int, date: int| #[trigger] stored_day_revenue(result, query_supplier, date) == stored_day_revenue(*base, query_supplier, date) +
                    if processed_cell(original.keys@, position as int + 1, query_supplier, date) { original.revenue_int(query_supplier, date) } else { 0int } by {
                    if query_supplier == supplier as int && bucket(date) == bin as int {
                        assert(stored_day_revenue(before, query_supplier, date) == days_revenue(previous_days(previous), date));
                        assert(stored_day_revenue(result, query_supplier, date) == days_revenue(replacement.days@, date));
                        assert(days_revenue(replacement.days@, date) == days_revenue(previous_days(previous), date) + change_revenue_int(selected, date));
                        if i32::MIN <= date <= i32::MAX {
                            lemma_sum_ext_all(selected, |change: Change| if change.date == date as i32 { change.revenue as int } else { 0int },
                                |change: Change| if change.date as int == date { change.revenue as int } else { 0int });
                            assert(original.revenue_int(query_supplier, date) == change_revenue_int(selected, date));
                        } else {
                            lemma_changes_outside(selected, date);
                        }
                    } else {
                        assert(profile_at(result, bucket(date), query_supplier) == profile_at(before, bucket(date), query_supplier));
                        assert(stored_day_revenue(result, query_supplier, date) == stored_day_revenue(before, query_supplier, date));
                    }
                }
            }
            position += 1;
        }
        proof {
            assert(original.keys@.take(position as int) =~= original.keys@);
            assert forall|supplier: int, date: int| #[trigger] stored_day_count(result, supplier, date) == stored_day_count(*base, supplier, date) + original.count_int(supplier, date) by {
                if !original.keys@.contains((bucket(date) as i64, supplier as i64)) && i64::MIN <= supplier <= i64::MAX && i32::MIN <= date <= i32::MAX {
                    assert(original.count_int(supplier, date) == 0);
                }
            }
            assert forall|supplier: int, date: int| #[trigger] stored_day_revenue(result, supplier, date) == stored_day_revenue(*base, supplier, date) + original.revenue_int(supplier, date) by {
                if !original.keys@.contains((bucket(date) as i64, supplier as i64)) && i64::MIN <= supplier <= i64::MAX && i32::MIN <= date <= i32::MAX {
                    assert(original.revenue_int(supplier, date) == 0);
                }
            }
        }
        Some(result)
    }
}

#[verifier::rlimit(60)]
#[verifier::spinoff_prover]
pub fn update_changes(mut days: Vec<Change>, date: i32, count_delta: i128, revenue_delta: i128, Ghost(bin): Ghost<int>)
    -> (result: Option<Vec<Change>>)
    requires changes_shape(days@, bin), bucket(date as int) == bin,
    ensures result matches Some(after) ==> changes_shape(after@, bin),
        result matches Some(after) ==> forall|other: i32| #[trigger] changes_count(after@, other) == changes_count(days@, other) +
            if other == date { count_delta as int } else { 0int },
        result matches Some(after) ==> forall|other: i32| #[trigger] changes_revenue(after@, other) == changes_revenue(days@, other) +
            if other == date { revenue_delta as int } else { 0int },
{
    let ghost before = days@;
    let mut position = 0;
    while position < days.len() && days[position].date < date
        invariant position <= days.len(), days@ == before, changes_shape(days@, bin),
            forall|index: int| 0 <= index < position ==> (#[trigger] days@[index]).date < date,
        decreases days.len() - position,
    { position += 1; }
    if position < days.len() && days[position].date == date {
        let count = match add(days[position].count as i128, count_delta) { Some(value) => value, None => return None };
        let revenue = match add(days[position].revenue as i128, revenue_delta) { Some(value) => value, None => return None };
        if count < i32::MIN as i128 || count > i32::MAX as i128 || revenue < i64::MIN as i128 || revenue > i64::MAX as i128 { return None; }
        let value = Change { date, count: count as i32, revenue: revenue as i64 };
        proof {
            assert forall|other: i32| #[trigger] changes_count(before.update(position as int, value), other) == changes_count(before, other) +
                if other == date { count_delta as int } else { 0int } by {
                lemma_sum_update(before, position as int, value, |change: Change| if change.date == other { change.count as int } else { 0int });
            }
            assert forall|other: i32| #[trigger] changes_revenue(before.update(position as int, value), other) == changes_revenue(before, other) +
                if other == date { revenue_delta as int } else { 0int } by {
                lemma_sum_update(before, position as int, value, |change: Change| if change.date == other { change.revenue as int } else { 0int });
            }
            assert forall|index: int| 0 <= index < before.len() implies (#[trigger] before.update(position as int, value)[index]).date == before[index].date by { }
        }
        days.set(position, value);
    } else {
        if days.len() >= 256 { return None; }
        if count_delta < i32::MIN as i128 || count_delta > i32::MAX as i128 || revenue_delta < i64::MIN as i128 || revenue_delta > i64::MAX as i128 { return None; }
        let value = Change { date, count: count_delta as i32, revenue: revenue_delta as i64 };
        days.insert(position, value);
        proof {
            assert(days@.remove(position as int) =~= before);
            assert forall|other: i32| #[trigger] changes_count(days@, other) == changes_count(before, other) +
                if other == date { count_delta as int } else { 0int } by {
                lemma_sum_remove(days@, position as int, |change: Change| if change.date == other { change.count as int } else { 0int });
            }
            assert forall|other: i32| #[trigger] changes_revenue(days@, other) == changes_revenue(before, other) +
                if other == date { revenue_delta as int } else { 0int } by {
                lemma_sum_remove(days@, position as int, |change: Change| if change.date == other { change.revenue as int } else { 0int });
            }
            assert forall|index: int| 0 <= index < days.len() implies bucket((#[trigger] days@[index]).date as int) == bin by {
                if index < position { assert(days@[index] == before[index]); }
                else if index > position { assert(days@[index] == before[index - 1]); }
            }
            assert forall|left: int, right: int| #![trigger days@[left], days@[right]]
                0 <= left < right < days.len() implies days@[left].date < days@[right].date by {
                let old_left = if left < position { left } else { left - 1 };
                let old_right = if right < position { right } else { right - 1 };
                if right == position { assert(before[left].date < date); }
                else if left == position {
                    assert(position < before.len());
                    assert(before[position as int].date > date);
                    if old_right > position { assert(before[position as int].date < before[old_right].date); }
                } else {
                    assert(days@[left] == before[old_left]);
                    assert(days@[right] == before[old_right]);
                    assert(before[old_left].date < before[old_right].date);
                }
            }
        }
    }
    Some(days)
}

#[verifier::rlimit(30)]
pub fn merge_profile(previous: &Option<Arc<BucketProfile>>, changes: &Vec<Change>, Ghost(bin): Ghost<int>) -> (result: Option<BucketProfile>)
    requires changes_shape(changes@, bin), previous is Some ==> profile_shape(*previous->0, bin),
    ensures result matches Some(profile) ==> days_shape(profile.days@, bin),
        result matches Some(profile) ==> (profile.count > 0 ==> profile_shape(profile, bin)),
        result matches Some(profile) ==> (profile.count == 0 ==> profile.days.len() == 0 && profile.revenue == 0),
        result matches Some(profile) ==> profile.count as int == sum(profile.days@, |day: Day| day.count as int)
            && profile.revenue as int == sum(profile.days@, |day: Day| day.revenue as int),
        result matches Some(profile) ==> profile.count as int == sum(previous_days(*previous), |day: Day| day.count as int) + sum(changes@, |change: Change| change.count as int),
        result matches Some(profile) ==> profile.revenue as int == sum(previous_days(*previous), |day: Day| day.revenue as int) + sum(changes@, |change: Change| change.revenue as int),
        result matches Some(profile) ==> forall|date: int| #[trigger] days_count(profile.days@, date) == days_count(previous_days(*previous), date) + change_count_int(changes@, date),
        result matches Some(profile) ==> forall|date: int| #[trigger] days_revenue(profile.days@, date) == days_revenue(previous_days(*previous), date) + change_revenue_int(changes@, date),
{
    let ghost original = previous_days(*previous);
    let old_length = match previous { Some(value) => value.days.len(), None => 0 };
    let mut old_position = 0;
    let mut change_position = 0;
    let mut days = Vec::with_capacity(old_length + changes.len());
    let mut count = 0i128;
    let mut revenue = 0i128;
    while old_position < old_length || change_position < changes.len()
        invariant old_position <= old_length, change_position <= changes.len(), days.len() <= 256,
            original == previous_days(*previous), original.len() == old_length,
            days_shape(original, bin), changes_shape(changes@, bin), days_shape(days@, bin),
            old_length == match previous { Some(value) => value.days.len(), None => 0 },
            count == sum(days@, |day: Day| day.count as int),
            revenue == sum(days@, |day: Day| day.revenue as int),
            count == sum(original.take(old_position as int), |day: Day| day.count as int) + sum(changes@.take(change_position as int), |change: Change| change.count as int),
            revenue == sum(original.take(old_position as int), |day: Day| day.revenue as int) + sum(changes@.take(change_position as int), |change: Change| change.revenue as int),
            forall|query: int| #[trigger] days_count(days@, query) == days_count(original.take(old_position as int), query) + change_count_int(changes@.take(change_position as int), query),
            forall|query: int| #[trigger] days_revenue(days@, query) == days_revenue(original.take(old_position as int), query) + change_revenue_int(changes@.take(change_position as int), query),
            forall|position: int| 0 <= position < days.len() ==> (
                (old_position < old_length ==> (#[trigger] days@[position]).shipdate < original[old_position as int].shipdate) &&
                (change_position < changes.len() ==> days@[position].shipdate < changes@[change_position as int].date)),
        decreases (old_length - old_position) + (changes.len() - change_position),
    {
        let ghost old_before = old_position as int;
        let ghost change_before = change_position as int;
        let ghost output_before = days@;
        let ghost count_before = count as int;
        let ghost revenue_before = revenue as int;
        let mut date = 0i32;
        let mut day_count = 0i128;
        let mut day_revenue = 0i128;
        if old_position < old_length {
            match previous {
                Some(value) => {
                    let day = value.days[old_position];
                    if change_position >= changes.len() || day.shipdate <= changes[change_position].date {
                        date = day.shipdate;
                        day_count = day.count as i128;
                        day_revenue = day.revenue as i128;
                        old_position += 1;
                    } else { date = changes[change_position].date; }
                },
                None => return None,
            }
        } else { date = changes[change_position].date; }
        if change_position < changes.len() && changes[change_position].date == date {
            day_count = match add(day_count, changes[change_position].count as i128) { Some(value) => value, None => return None };
            day_revenue = match add(day_revenue, changes[change_position].revenue as i128) { Some(value) => value, None => return None };
            change_position += 1;
        }
        if day_count < 0 || day_count > u32::MAX as i128 || day_revenue < i64::MIN as i128 || day_revenue > i64::MAX as i128 { return None; }
        proof {
            assert(old_before <= old_position <= old_before + 1);
            assert(change_before <= change_position <= change_before + 1);
            assert(old_before < old_position || change_before < change_position);
            if old_before < old_position {
                assert(original[old_before].shipdate == date);
                lemma_sum_take(original, old_before, |day: Day| day.count as int);
                lemma_sum_take(original, old_before, |day: Day| day.revenue as int);
            }
            if change_before < change_position {
                assert(changes@[change_before].date == date);
                lemma_sum_take(changes@, change_before, |change: Change| change.count as int);
                lemma_sum_take(changes@, change_before, |change: Change| change.revenue as int);
            }
            assert(count_before + day_count as int == sum(original.take(old_position as int), |day: Day| day.count as int) + sum(changes@.take(change_position as int), |change: Change| change.count as int));
            assert(revenue_before + day_revenue as int == sum(original.take(old_position as int), |day: Day| day.revenue as int) + sum(changes@.take(change_position as int), |change: Change| change.revenue as int));
            assert forall|query: int| #[trigger] days_count(output_before, query) + (if date as int == query { day_count as int } else { 0int }) ==
                days_count(original.take(old_position as int), query) + change_count_int(changes@.take(change_position as int), query) by {
                if old_before < old_position { lemma_sum_take(original, old_before, |day: Day| if day.shipdate as int == query { day.count as int } else { 0int }); }
                if change_before < change_position { lemma_sum_take(changes@, change_before, |change: Change| if change.date as int == query { change.count as int } else { 0int }); }
            }
            assert forall|query: int| #[trigger] days_revenue(output_before, query) + (if date as int == query { day_revenue as int } else { 0int }) ==
                days_revenue(original.take(old_position as int), query) + change_revenue_int(changes@.take(change_position as int), query) by {
                if old_before < old_position { lemma_sum_take(original, old_before, |day: Day| if day.shipdate as int == query { day.revenue as int } else { 0int }); }
                if change_before < change_position { lemma_sum_take(changes@, change_before, |change: Change| if change.date as int == query { change.revenue as int } else { 0int }); }
            }
            assert(bucket(date as int) == bin);
            assert forall|position: int| 0 <= position < output_before.len() implies (#[trigger] output_before[position]).shipdate < date by {
                if old_before < old_position { assert(output_before[position].shipdate < original[old_before].shipdate); }
                else { assert(output_before[position].shipdate < changes@[change_before].date); }
            }
            if old_position < old_length { assert(date < original[old_position as int].shipdate); }
            if change_position < changes.len() { assert(date < changes@[change_position as int].date); }
        }
        if day_count == 0 {
            if day_revenue != 0 { return None; }
        } else {
            if days.len() >= 256 { return None; }
            count = match add(count, day_count) { Some(value) => value, None => return None };
            revenue = match add(revenue, day_revenue) { Some(value) => value, None => return None };
            proof {
                lemma_sum_push(days@, Day { shipdate: date, count: day_count as u32, revenue: day_revenue as i64 }, |day: Day| day.count as int);
                lemma_sum_push(days@, Day { shipdate: date, count: day_count as u32, revenue: day_revenue as i64 }, |day: Day| day.revenue as int);
                let value = Day { shipdate: date, count: day_count as u32, revenue: day_revenue as i64 };
                assert forall|query: int| #[trigger] days_count(days@.push(value), query) == days_count(days@, query) + (if date as int == query { day_count as int } else { 0int }) by {
                    lemma_sum_push(days@, value, |day: Day| if day.shipdate as int == query { day.count as int } else { 0int });
                }
                assert forall|query: int| #[trigger] days_revenue(days@.push(value), query) == days_revenue(days@, query) + (if date as int == query { day_revenue as int } else { 0int }) by {
                    lemma_sum_push(days@, value, |day: Day| if day.shipdate as int == query { day.revenue as int } else { 0int });
                }
            }
            days.push(Day { shipdate: date, count: day_count as u32, revenue: day_revenue as i64 });
            proof {
                assert forall|left: int, right: int| #![trigger days@[left], days@[right]] 0 <= left < right < days.len() implies days@[left].shipdate < days@[right].shipdate by {
                    if right == output_before.len() { assert(days@[left] == output_before[left]); }
                }
            }
        }
    }
    if count > u64::MAX as i128 { return None; }
    proof {
        lemma_sum_nonneg(days@, |day: Day| day.count as int);
        assert(count >= 0);
        if count == 0 && days.len() > 0 {
            lemma_sum_pos(days@, |day: Day| day.count as int, 0);
            assert(false);
        }
        assert(original.take(old_position as int) =~= original);
        assert(changes@.take(change_position as int) =~= changes@);
    }
    Some(BucketProfile { count: count as u64, revenue, days })
}
pub proof fn lemma_changes_outside(changes: Seq<Change>, date: int)
    requires date < i32::MIN || date > i32::MAX,
    ensures change_count_int(changes, date) == 0, change_revenue_int(changes, date) == 0,
{
    lemma_sum_zero(changes, |change: Change| if change.date as int == date { change.count as int } else { 0int });
    lemma_sum_zero(changes, |change: Change| if change.date as int == date { change.revenue as int } else { 0int });
}
pub proof fn lemma_facts_outside(facts: Facts, supplier: int, date: int)
    requires facts_shape(facts), !(i64::MIN <= supplier <= i64::MAX && i32::MIN <= date <= i32::MAX),
    ensures stored_day_count(facts, supplier, date) == 0, stored_day_revenue(facts, supplier, date) == 0,
{
    if let Some(profile) = profile_at(facts, bucket(date), supplier) {
        assert(i64::MIN <= supplier <= i64::MAX);
        lemma_sum_zero(profile.days@, |day: Day| if day.shipdate as int == date { day.count as int } else { 0int });
        lemma_sum_zero(profile.days@, |day: Day| if day.shipdate as int == date { day.revenue as int } else { 0int });
    }
}
#[verifier::rlimit(20)]
pub fn install_profile(facts: &mut Facts, bin: i64, supplier: i64, profile: BucketProfile)
    requires facts_shape(*old(facts)), profile.count > 0 ==> profile_shape(profile, bin as int), profile.count == 0 ==> profile.days.len() == 0,
    ensures facts_shape(*final(facts)),
        forall|query_bin: int, query_supplier: int| #[trigger] profile_at(*final(facts), query_bin, query_supplier) ==
            if query_bin == bin as int && query_supplier == supplier as int {
                if profile.count > 0 { Some(profile) } else { None }
            } else { profile_at(*old(facts), query_bin, query_supplier) },
{
    let ghost before = *facts;
    let ghost replacement = profile;
    let bin_key = Key::number(bin);
    let supplier_key = Key::number(supplier);
    let mut inner = match facts.get(&bin_key) { Some(value) => value.share(), None => Index::new() };
    let ghost previous_inner = inner;
    if profile.count == 0 { inner.remove(&supplier_key); }
    else { inner.put(supplier_key, Arc::new(profile)); }
    proof { inner.model(); }
    let ghost updated_inner = inner;
    if inner.is_empty() { facts.remove(&bin_key); }
    else { facts.put(bin_key, Arc::new(inner)); }
    proof {
        assert forall|query_bin: int, query_supplier: int| #[trigger] profile_at(*facts, query_bin, query_supplier) ==
            if query_bin == bin as int && query_supplier == supplier as int {
                if replacement.count > 0 { Some(replacement) } else { None }
            } else { profile_at(before, query_bin, query_supplier) } by {
            if query_bin == bin as int {
                assert(previous_inner.at(number_key(query_supplier)) == profile_at(before, query_bin, query_supplier));
                if updated_inner.entries().len() == 0 { assert(updated_inner.at(number_key(query_supplier)) is None); }
            }
        }
        assert forall|query_bin: int, query_supplier: int| #[trigger] profile_at(*facts, query_bin, query_supplier) is Some implies profile_shape(profile_at(*facts, query_bin, query_supplier).unwrap(), query_bin) by {
            let value = profile_at(*facts, query_bin, query_supplier).unwrap();
            if query_bin == bin as int && query_supplier == supplier as int { assert(value == replacement); }
        }
        assert forall|key: KeyView| #[trigger] facts.at(key) is Some implies (
            key.1 == Seq::<u8>::empty() && i64::MIN <= key.0 <= i64::MAX && facts.at(key).unwrap().entries().len() > 0 &&
            forall|supplier_key: KeyView| #[trigger] facts.at(key).unwrap().at(supplier_key) is Some ==> supplier_key.1 == Seq::<u8>::empty() && i64::MIN <= supplier_key.0 <= i64::MAX) by {
            let suppliers = facts.at(key).unwrap();
            if key == number_key(bin as int) {
                assert(suppliers == updated_inner);
                assert forall|query: KeyView| #[trigger] suppliers.at(query) is Some implies query.1 == Seq::<u8>::empty() && i64::MIN <= query.0 <= i64::MAX by {
                    if query != number_key(supplier as int) {
                        assert(previous_inner.at(query) is Some);
                        assert(before.at(number_key(bin as int)) is Some);
                        assert(previous_inner.at(query) == before.at(number_key(bin as int)).unwrap().at(query));
                    }
                }
            }
        }
    }
}

pub struct VariantChange {
    pub name: Vec<u8>, pub address: Vec<u8>, pub phone: Vec<u8>, pub count: i128,
}
pub type DimensionChanges = Index<Index<VariantChange>>;
pub open spec fn variant_count(variants: Index<Variant>, key: KeyView) -> int {
    match variants.at(key) { Some(value) => value.count as int, None => 0 }
}
pub open spec fn variant_change_count(variants: Index<VariantChange>, key: KeyView) -> int {
    match variants.at(key) { Some(value) => value.count as int, None => 0 }
}
pub open spec fn variant_count_weight(key: KeyView) -> spec_fn((KeyView, VariantChange)) -> int {
    |entry: (KeyView, VariantChange)| if entry.0 == key { entry.1.count as int } else { 0int }
}
pub open spec fn variant_prefix_count(rows: Seq<(KeyView, VariantChange)>, key: KeyView) -> int {
    sum(rows, variant_count_weight(key))
}
pub proof fn lemma_variant_change_sum(changes: &Index<VariantChange>, key: KeyView)
    requires changes.wf(),
    ensures variant_prefix_count(changes.entries(), key) == variant_change_count(*changes, key),
{
    changes.model();
    let rows = changes.entries();
    let weight = variant_count_weight(key);
    if changes.at(key) is Some {
        let position = choose|position: int| #[trigger] rows[position] == (key, changes.at(key).unwrap()) && 0 <= position < rows.len();
        assert forall|other: int| 0 <= other < rows.len() && other != position implies weight(#[trigger] rows[other]) == 0 by {
            verus_prooflib::ordered::lemma_key_order(key, key);
            if other < position {
                assert(verus_prooflib::ordered::key_lt(rows[other].0, rows[position].0));
            } else {
                assert(verus_prooflib::ordered::key_lt(rows[position].0, rows[other].0));
            }
        }
        lemma_sum_single_match(rows, weight, position);
        assert(rows[position].1 == changes.at(key).unwrap());
        assert(weight(rows[position]) == variant_change_count(*changes, key));
    } else {
        assert forall|position: int| 0 <= position < rows.len() implies weight(#[trigger] rows[position]) == 0 by {
            assert(changes.at(rows[position].0) is Some);
        }
        lemma_sum_zero(rows, weight);
    }
}
pub open spec fn variants_shape(variants: Index<Variant>) -> bool {
    forall|key: KeyView| #[trigger] variants.at(key) matches Some(value) ==> value.count > 0 && key == variant_key(value.name@, value.address@, value.phone@)
}
pub open spec fn variant_changes_shape(variants: Index<VariantChange>) -> bool {
    forall|key: KeyView| #[trigger] variants.at(key) matches Some(value) ==> key == variant_key(value.name@, value.address@, value.phone@)
}
pub open spec fn dimension_changes_shape(changes: DimensionChanges) -> bool {
    forall|key: KeyView| #[trigger] changes.at(key) matches Some(variants) ==> key.1 == Seq::<u8>::empty() && variant_changes_shape(variants)
}
pub open spec fn change_at(changes: DimensionChanges, supplier: int, key: KeyView) -> int {
    match changes.at(number_key(supplier)) {
        Some(inner) => match inner.at(key) { Some(value) => value.count as int, None => 0 },
        None => 0,
    }
}
pub open spec fn dimension_changes_match(changes: DimensionChanges, rows: Seq<SupplierRow>) -> bool {
    forall|supplier: int, key: KeyView| #[trigger] change_at(changes, supplier, key) == supplier_count(rows, supplier, key)
}
pub open spec fn dimension_delta_match(changes: DimensionChanges, rows: Seq<(SupplierRow, int)>) -> bool {
    forall|supplier: int, key: KeyView| #[trigger] change_at(changes, supplier, key) ==
        sum(rows, |change: (SupplierRow, int)| if change.0.s_suppkey == supplier &&
            variant_key(change.0.s_name, change.0.s_address, change.0.s_phone) == key { change.1 } else { 0int })
}
pub fn add_supplier(changes: &mut DimensionChanges, row: &SupplierInput, sign: i64) -> (result: bool)
    requires dimension_changes_shape(*old(changes)),
    ensures result ==> dimension_changes_shape(*final(changes)),
        result ==> forall|supplier: int, key: KeyView| #[trigger] change_at(*final(changes), supplier, key) ==
        change_at(*old(changes), supplier, key) + if supplier == row.s_suppkey as int &&
            key == variant_key(row.s_name@, row.s_address@, row.s_phone@) { sign as int } else { 0int },
{
    let ghost before = *changes;
    let supplier_key = Key::number(row.s_suppkey);
    let key = text_key(&row.s_name, &row.s_address, &row.s_phone);
    let ghost changed_key = key.view();
    let mut inner = match changes.get(&supplier_key) { Some(value) => value.share(), None => Index::new() };
    let ghost old_inner = inner;
    let previous = match inner.get(&key) { Some(value) => value.count, None => 0 };
    let count = match add(previous, sign as i128) { Some(value) => value, None => return false };
    inner.put(key, Arc::new(VariantChange {
        name: copy_bytes(&row.s_name), address: copy_bytes(&row.s_address), phone: copy_bytes(&row.s_phone), count,
    }));
    proof {
        assert forall|query: KeyView| #[trigger] inner.at(query) is Some implies
            query == variant_key(inner.at(query).unwrap().name@, inner.at(query).unwrap().address@, inner.at(query).unwrap().phone@) by {
            if query != changed_key {
                assert(before.at(number_key(row.s_suppkey as int)) is Some);
                assert(inner.at(query) == old_inner.at(query));
                assert(inner.at(query) == before.at(number_key(row.s_suppkey as int)).unwrap().at(query));
            }
        }
    }
    let ghost updated = inner;
    changes.put(supplier_key, Arc::new(inner));
    proof {
        assert forall|query: KeyView| #[trigger] changes.at(query) is Some implies query.1 == Seq::<u8>::empty() && variant_changes_shape(changes.at(query).unwrap()) by {
            if query == number_key(row.s_suppkey as int) { assert(changes.at(query) == Some(updated)); }
        }
    }
    true
}
pub fn finish_suppliers(old: &Dimensions, changes: &DimensionChanges) -> (result: Option<Dimensions>)
    requires dimensions_shape(*old), dimension_changes_shape(*changes),
    ensures result matches Some(root) ==> dimensions_shape(root),
        result matches Some(root) ==> forall|supplier: int, key: KeyView| #[trigger] stored_supplier_count(root, supplier, key) ==
        stored_supplier_count(*old, supplier, key) + change_at(*changes, supplier, key),
{
    let mut result = old.share();
    let mut cursor = changes.cursor();
    let mut processed = Ghost(0int);
    let shared_changes = changes.share();
    proof {
        changes.model();
        assert(changes.entries().take(0) =~= Seq::<(KeyView, Index<VariantChange>)>::empty());
        assert forall|key: KeyView| #[trigger] result.at(key) == old.at(key) by { }
        assert forall|supplier: int, key: KeyView| #[trigger] variant_at(result, supplier, key) == variant_at(*old, supplier, key) by { }
        assert(dimensions_shape(result));
    }
    loop
        invariant 0 <= processed@ <= changes.entries().len(), cursor.remaining() == changes.entries().skip(processed@),
            changes.wf(),
            dimensions_shape(*old), dimension_changes_shape(*changes), dimensions_shape(result),
            forall|supplier: int, key: KeyView| #[trigger] stored_supplier_count(result, supplier, key) == stored_supplier_count(*old, supplier, key) +
                sum(changes.entries().take(processed@), |entry: (KeyView, Index<VariantChange>)|
                    if entry.0 == number_key(supplier) { variant_change_count(entry.1, key) } else { 0int }),
        decreases cursor.work(),
    {
        let ghost pending = cursor.remaining();
        let (supplier_key, variants) = match cursor.next() {
            Some(value) => value,
            None => {
                proof {
                    assert(pending.len() == 0);
                    assert(processed@ == changes.entries().len());
                    assert(changes.entries().take(processed@) =~= changes.entries());
                    assert forall|supplier: int, key: KeyView| #[trigger] stored_supplier_count(result, supplier, key) == stored_supplier_count(*old, supplier, key) + change_at(*changes, supplier, key) by {
                        let weight = |entry: (KeyView, Index<VariantChange>)| variant_change_count(entry.1, key);
                        verus_prooflib::ordered::lemma_key_contribution(changes, number_key(supplier), weight);
                        lemma_sum_ext_all(changes.entries(), |entry: (KeyView, Index<VariantChange>)| if entry.0 == number_key(supplier) { weight(entry) } else { 0int },
                            |entry: (KeyView, Index<VariantChange>)| if entry.0 == number_key(supplier) { variant_change_count(entry.1, key) } else { 0int });
                    }
                }
                return Some(result);
            },
        };
        let ghost before = result;
        let inner = match result.get(&supplier_key) { Some(value) => value.share(), None => Index::new() };
        let ghost original_inner = inner;
        proof {
            changes.model();
            assert(processed@ < changes.entries().len());
            assert(changes.entries()[processed@] == (supplier_key.view(), *variants));
            assert(supplier_key.view().1 == Seq::<u8>::empty());
            assert forall|key: KeyView| #[trigger] inner.at(key) is Some implies inner.at(key).unwrap().count > 0 &&
                key == variant_key(inner.at(key).unwrap().name@, inner.at(key).unwrap().address@, inner.at(key).unwrap().phone@) by {
                assert(inner.at(key) == variant_at(result, supplier_key.number as int, key));
                assert(variant_at(result, supplier_key.number as int, key) is Some);
            }
            assert(variant_changes_shape(*variants));
        }
        let inner = match finish_variants(&inner, &*variants) { Some(value) => value, None => return None };
        let ghost updated_inner = inner;
        proof { inner.model(); }
        if inner.is_empty() { result.remove(&supplier_key); }
        else { result.put(supplier_key, Arc::new(inner)); }
        proof {
            assert forall|supplier: int, key: KeyView| #[trigger] stored_supplier_count(result, supplier, key) == stored_supplier_count(*old, supplier, key) +
                sum(changes.entries().take(processed@ + 1), |entry: (KeyView, Index<VariantChange>)|
                    if entry.0 == number_key(supplier) { variant_change_count(entry.1, key) } else { 0int }) by {
                let weight = |entry: (KeyView, Index<VariantChange>)| if entry.0 == number_key(supplier) { variant_change_count(entry.1, key) } else { 0int };
                assert(stored_supplier_count(before, supplier, key) == stored_supplier_count(*old, supplier, key) + sum(changes.entries().take(processed@), weight));
                lemma_sum_take(changes.entries(), processed@, weight);
                if supplier == supplier_key.number as int {
                    assert(variant_count(original_inner, key) == stored_supplier_count(before, supplier, key));
                    assert(stored_supplier_count(result, supplier, key) == variant_count(updated_inner, key));
                }
            }
            assert forall|supplier: int, key: KeyView| #[trigger] variant_at(result, supplier, key) is Some implies
                variant_at(result, supplier, key).unwrap().count > 0 && key == variant_key(variant_at(result, supplier, key).unwrap().name@, variant_at(result, supplier, key).unwrap().address@, variant_at(result, supplier, key).unwrap().phone@) by {
                if supplier == supplier_key.number as int {
                    if updated_inner.entries().len() > 0 {
                        assert(variant_at(result, supplier, key) == updated_inner.at(key));
                        assert(variants_shape(updated_inner));
                    } else { assert(variant_at(result, supplier, key) is None); }
                } else {
                    assert(result.at(number_key(supplier)) == before.at(number_key(supplier)));
                    assert(variant_at(result, supplier, key) == variant_at(before, supplier, key));
                }
            }
        }
        processed = Ghost(processed@ + 1);
    }
}
pub fn finish_variants(base: &Index<Variant>, changes: &Index<VariantChange>) -> (result: Option<Index<Variant>>)
    requires variants_shape(*base), variant_changes_shape(*changes),
    ensures result matches Some(root) ==> variants_shape(root) && root.wf(),
        result matches Some(root) ==> forall|key: KeyView| #[trigger] variant_count(root, key) == variant_count(*base, key) + variant_change_count(*changes, key),
{
    let mut result = base.share();
    let mut cursor = changes.cursor();
    let mut processed = Ghost(0int);
    let shared_changes = changes.share();
    proof {
        changes.model();
        assert(changes.entries().take(0) =~= Seq::<(KeyView, VariantChange)>::empty());
        assert forall|key: KeyView| #[trigger] result.at(key) == base.at(key) by { }
        assert(variants_shape(result));
    }
    loop
        invariant 0 <= processed@ <= changes.entries().len(), cursor.remaining() == changes.entries().skip(processed@),
            changes.wf(), result.wf(),
            variants_shape(*base), variant_changes_shape(*changes), variants_shape(result),
            forall|key: KeyView| #[trigger] variant_count(result, key) == variant_count(*base, key) +
                variant_prefix_count(changes.entries().take(processed@), key),
        decreases cursor.work(),
    {
        let ghost pending = cursor.remaining();
        let (key, change) = match cursor.next() {
            Some(value) => value,
            None => {
                proof {
                    assert(pending.len() == 0);
                    assert(processed@ == changes.entries().len());
                    assert(changes.entries().take(processed@) =~= changes.entries());
                    assert forall|key: KeyView| #[trigger] variant_count(result, key) == variant_count(*base, key) + variant_change_count(*changes, key) by {
                        lemma_variant_change_sum(changes, key);
                        assert(variant_count(result, key) == variant_count(*base, key) + variant_prefix_count(changes.entries(), key));
                        assert(variant_prefix_count(changes.entries(), key) == variant_change_count(*changes, key));
                        assert(variant_count(result, key) == variant_count(*base, key) + variant_change_count(*changes, key));
                    }
                }
                return Some(result);
            },
        };
        let ghost before = result;
        proof {
            changes.model();
            assert(changes.entries()[processed@] == (key.view(), *change));
            assert(changes.at(key.view()) == Some(*change));
        }
        let previous = match result.get(&key) { Some(value) => value.count as i128, None => 0 };
        proof { assert(previous as int == variant_count(before, key.view())); }
        let count = match add(previous, change.count) { Some(value) => value, None => return None };
        if count < 0 || count > u64::MAX as i128 { return None; }
        let ghost changed_key = key.view();
        if count == 0 { result.remove(&key); }
        else {
            result.put(key, Arc::new(Variant {
                name: copy_bytes(&change.name), address: copy_bytes(&change.address), phone: copy_bytes(&change.phone), count: count as u64,
            }));
        }
        proof {
            assert forall|query: KeyView| #[trigger] variant_count(result, query) == variant_count(*base, query) +
                sum(changes.entries().take(processed@ + 1), |entry: (KeyView, VariantChange)| if entry.0 == query { entry.1.count as int } else { 0int }) by {
                let weight = |entry: (KeyView, VariantChange)| if entry.0 == query { entry.1.count as int } else { 0int };
                assert(variant_count(before, query) == variant_count(*base, query) + sum(changes.entries().take(processed@), weight));
                lemma_sum_take(changes.entries(), processed@, weight);
                if query == changed_key { assert(variant_count(result, query) == count as int); }
                else { assert(result.at(query) == before.at(query)); }
            }
        }
        processed = Ghost(processed@ + 1);
    }
}
}
