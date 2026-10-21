use vstd::prelude::*;
use vstd::hash_map::HashMapWithView;
use std::sync::Arc;
use verus_prooflib::ordered::{Key, KeyView, Index};
use verus_prooflib::wide::Wide;
use verus_common::sums::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::model::*;
use crate::values::*;
use crate::query_proofs::lemma_day_outside;

verus! {
pub struct Change { pub revenue: Wide, pub date: i32, pub count: i64 }
pub open spec fn changes_shape(days: Seq<Change>, bin: int) -> bool {
    days.len() <= 256 &&
    (forall|position: int| 0 <= position < days.len() ==> bucket((#[trigger] days[position]).date as int) == bin) &&
    (forall|left: int, right: int| #![trigger days[left], days[right]]
        0 <= left < right < days.len() ==> days[left].date < days[right].date)
}
pub open spec fn changes_bounded(days: Seq<Change>, bound: int) -> bool {
    forall|position: int| 0 <= position < days.len() ==> -bound <= (#[trigger] days[position]).count <= bound
}
pub open spec fn changes_count(days: Seq<Change>, date: i32) -> int {
    sum(days, |change: Change| if change.date == date { change.count as int } else { 0int })
}
pub open spec fn changes_revenue(days: Seq<Change>, date: i32) -> int {
    sum(days, |change: Change| if change.date == date { change.revenue@ } else { 0int })
}
pub open spec fn change_count_int(days: Seq<Change>, date: int) -> int {
    sum(days, |change: Change| if change.date as int == date { change.count as int } else { 0int })
}
pub open spec fn change_revenue_int(days: Seq<Change>, date: int) -> int {
    sum(days, |change: Change| if change.date as int == date { change.revenue@ } else { 0int })
}
pub open spec fn previous_days(previous: Option<Arc<BucketProfile>>) -> Seq<Day> {
    match previous { Some(value) => value.days@, None => Seq::empty() }
}
pub open spec fn processed_cell(keys: Seq<(i64, i64)>, count: int, supplier: int, date: int) -> bool {
    i64::MIN <= supplier <= i64::MAX && i32::MIN <= date <= i32::MAX &&
        keys.take(count).contains((bucket(date) as i64, supplier as i64))
}

/// Strictly increasing integers differ by at least their distance in the list.
pub proof fn lemma_gap(dates: Seq<int>, first: int, last: int)
    requires 0 <= first <= last < dates.len(),
        forall|left: int, right: int| #![trigger dates[left], dates[right]] 0 <= left < right < dates.len() ==> dates[left] < dates[right],
    ensures dates[last] - dates[first] >= last - first,
    decreases last - first,
{
    if first < last { lemma_gap(dates, first, last - 1); assert(dates[last - 1] < dates[last]); }
}

/// A bucket's 256 dates are all used once a strictly increasing list of them has 256 entries.
pub proof fn lemma_no_room(dates: Seq<int>, bin: int, date: int)
    requires dates.len() >= 256, 256 * bin <= date < 256 * bin + 256,
        forall|index: int| 0 <= index < dates.len() ==> 256 * bin <= (#[trigger] dates[index]) < 256 * bin + 256,
        forall|left: int, right: int| #![trigger dates[left], dates[right]] 0 <= left < right < dates.len() ==> dates[left] < dates[right],
    ensures dates.contains(date),
{
    let lo = 256 * bin;
    let offset = date - lo;
    lemma_gap(dates, 0, offset);
    lemma_gap(dates, offset, 255);
    assert(dates[offset] == date);
}

/// Dates of a list lying in a bucket with a date missing leave room for it.
pub proof fn lemma_room_changes(days: Seq<Change>, bin: int, date: int)
    requires changes_shape(days, bin), bucket(date) == bin, forall|index: int| 0 <= index < days.len() ==> (#[trigger] days[index]).date as int != date,
    ensures days.len() < 256,
{
    let dates = days.map_values(|change: Change| change.date as int);
    if days.len() >= 256 {
        assert forall|index: int| 0 <= index < dates.len() implies 256 * bin <= (#[trigger] dates[index]) < 256 * bin + 256 by {
            assert(bucket(days[index].date as int) == bin);
            vstd::arithmetic::div_mod::lemma_fundamental_div_mod(days[index].date as int, 256);
            vstd::arithmetic::div_mod::lemma_mod_bound(days[index].date as int, 256);
        }
        vstd::arithmetic::div_mod::lemma_fundamental_div_mod(date, 256);
        vstd::arithmetic::div_mod::lemma_mod_bound(date, 256);
        lemma_no_room(dates, bin, date);
        let index = choose|index: int| 0 <= index < dates.len() && dates[index] == date;
        assert(days[index].date as int == date);
    }
}
pub proof fn lemma_room_days(days: Seq<Day>, bin: int, date: int)
    requires days_shape(days, bin), bucket(date) == bin, forall|index: int| 0 <= index < days.len() ==> (#[trigger] days[index]).shipdate as int != date,
    ensures days.len() < 256,
{
    let dates = days.map_values(|day: Day| day.shipdate as int);
    if days.len() >= 256 {
        assert forall|index: int| 0 <= index < dates.len() implies 256 * bin <= (#[trigger] dates[index]) < 256 * bin + 256 by {
            assert(bucket(days[index].shipdate as int) == bin);
            vstd::arithmetic::div_mod::lemma_fundamental_div_mod(days[index].shipdate as int, 256);
            vstd::arithmetic::div_mod::lemma_mod_bound(days[index].shipdate as int, 256);
        }
        vstd::arithmetic::div_mod::lemma_fundamental_div_mod(date, 256);
        vstd::arithmetic::div_mod::lemma_mod_bound(date, 256);
        lemma_no_room(dates, bin, date);
        let index = choose|index: int| 0 <= index < dates.len() && dates[index] == date;
        assert(days[index].shipdate as int == date);
    }
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
    pub open spec fn bounded(self, bound: int) -> bool {
        forall|bin: i64, supplier: i64| self.groups@.contains_key(bin) && self.groups@[bin]@.contains_key(supplier) ==>
            changes_bounded((#[trigger] self.groups@[bin]@[supplier])@, bound)
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
        ensures result.matches_lines(Seq::empty()), result.matches_delta(Seq::empty()), result.bounded(0),
    {
        Scratch { groups: HashMapWithView::new(), keys: Vec::new() }
    }
    pub fn add(&mut self, row: &LineInput, sign: i64, Ghost(bound): Ghost<int>)
        requires old(self).well_formed(), old(self).bounded(bound), 0 <= bound < 0x4000000000000000, sign == 1 || sign == -1,
        ensures final(self).well_formed(), final(self).bounded(bound + 1),
            forall|supplier: i64, date: i32| #[trigger] final(self).count(supplier, date) == old(self).count(supplier, date) +
                    if supplier == row.l_suppkey && date == row.l_shipdate { sign as int } else { 0int },
            forall|supplier: i64, date: i32| #[trigger] final(self).revenue(supplier, date) == old(self).revenue(supplier, date) +
                if supplier == row.l_suppkey && date == row.l_shipdate { sign as int * weight(line_input_view(*row)) } else { 0int },
    {
        let ghost before = *self;
        let bin = bin(row.l_shipdate);
        let product = Wide::from_i128(row.l_extendedprice as i128).mul(&Wide::from_i128(100i128 - row.l_discount as i128));
        let revenue = if sign == 1 { product } else { product.neg() };
        proof { assert(revenue@ == sign as int * weight(line_input_view(*row))); }
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
        let days = update_changes(days, row.l_shipdate, sign, revenue, Ghost(bin as int), Ghost(bound));
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
            assert forall|query_bin: i64, supplier: i64| self.groups@.contains_key(query_bin) && self.groups@[query_bin]@.contains_key(supplier) implies
                changes_bounded((#[trigger] self.groups@[query_bin]@[supplier])@, bound + 1) by {
                if query_bin == bin && supplier == row.l_suppkey {
                    assert(self.groups@[bin]@[supplier]@ == updated_days);
                } else {
                    assert(before.groups@.contains_key(query_bin));
                    assert(before.groups@[query_bin]@.contains_key(supplier));
                    assert(self.groups@[query_bin]@[supplier]@ == before.groups@[query_bin]@[supplier]@);
                    assert(changes_bounded(before.groups@[query_bin]@[supplier]@, bound));
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
    }
    pub fn finish(&self, base: &Facts, Ghost(target): Ghost<Seq<LineitemRow>>) -> (result: Facts)
        requires self.well_formed(), facts_shape(*base), target.len() <= max_rows(),
            forall|supplier: int, date: int| #[trigger] stored_day_count(*base, supplier, date) + self.count_int(supplier, date) == day_count(target, supplier, date),
            forall|supplier: int, date: int| #[trigger] stored_day_revenue(*base, supplier, date) + self.revenue_int(supplier, date) == day_revenue(target, supplier, date),
        ensures facts_shape(result),
            forall|supplier: int, date: int| #[trigger] stored_day_count(result, supplier, date) ==
                stored_day_count(*base, supplier, date) + self.count_int(supplier, date),
            forall|supplier: int, date: int| #[trigger] stored_day_revenue(result, supplier, date) ==
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
            invariant position <= self.keys.len(), *self == original, original.well_formed(), facts_shape(*base), facts_shape(result), target.len() <= max_rows(),
                forall|supplier: int, date: int| #[trigger] stored_day_count(*base, supplier, date) + original.count_int(supplier, date) == day_count(target, supplier, date),
                forall|supplier: int, date: int| #[trigger] stored_day_revenue(*base, supplier, date) + original.revenue_int(supplier, date) == day_revenue(target, supplier, date),
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
            proof { assert(self.groups@.contains_key(bin) && self.groups@[bin]@.contains_key(supplier)); }
            let scratch = self.groups.get(&bin).unwrap();
            let changes = scratch.get(&supplier).unwrap();
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
            proof {
                assert forall|date: int| bucket(date) == bin as int ==> #[trigger] days_count(previous_days(previous), date) + change_count_int(selected, date) == day_count(target, supplier as int, date) by {
                    if bucket(date) == bin as int {
                        assert(stored_day_count(result, supplier as int, date) == days_count(previous_days(previous), date));
                        assert(!processed_cell(original.keys@, position as int, supplier as int, date));
                        assert(stored_day_count(result, supplier as int, date) == stored_day_count(*base, supplier as int, date) + if processed_cell(original.keys@, position as int, supplier as int, date) { original.count_int(supplier as int, date) } else { 0int });
                        assert(stored_day_count(*base, supplier as int, date) + original.count_int(supplier as int, date) == day_count(target, supplier as int, date));
                        if i32::MIN <= date <= i32::MAX {
                            lemma_sum_ext_all(selected, |change: Change| if change.date == date as i32 { change.count as int } else { 0int },
                                |change: Change| if change.date as int == date { change.count as int } else { 0int });
                            assert(original.count_int(supplier as int, date) == change_count_int(selected, date));
                        } else {
                            lemma_changes_outside(selected, date);
                        }
                    }
                }
                assert forall|date: int| bucket(date) == bin as int ==> #[trigger] days_revenue(previous_days(previous), date) + change_revenue_int(selected, date) == day_revenue(target, supplier as int, date) by {
                    if bucket(date) == bin as int {
                        assert(stored_day_revenue(result, supplier as int, date) == days_revenue(previous_days(previous), date));
                        assert(!processed_cell(original.keys@, position as int, supplier as int, date));
                        assert(stored_day_revenue(result, supplier as int, date) == stored_day_revenue(*base, supplier as int, date) + if processed_cell(original.keys@, position as int, supplier as int, date) { original.revenue_int(supplier as int, date) } else { 0int });
                        assert(stored_day_revenue(*base, supplier as int, date) + original.revenue_int(supplier as int, date) == day_revenue(target, supplier as int, date));
                        if i32::MIN <= date <= i32::MAX {
                            lemma_sum_ext_all(selected, |change: Change| if change.date == date as i32 { change.revenue@ } else { 0int },
                                |change: Change| if change.date as int == date { change.revenue@ } else { 0int });
                            assert(original.revenue_int(supplier as int, date) == change_revenue_int(selected, date));
                        } else {
                            lemma_changes_outside(selected, date);
                        }
                    }
                }
            }
            let profile = merge_profile(&previous, changes, Ghost(bin as int), Ghost(target), Ghost(supplier as int));
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
                            lemma_sum_ext_all(selected, |change: Change| if change.date == date as i32 { change.revenue@ } else { 0int },
                                |change: Change| if change.date as int == date { change.revenue@ } else { 0int });
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
        result
    }
}


#[verifier::rlimit(60)]
#[verifier::spinoff_prover]
pub fn update_changes(mut days: Vec<Change>, date: i32, count_delta: i64, revenue_delta: Wide, Ghost(bin): Ghost<int>, Ghost(bound): Ghost<int>)
    -> (result: Vec<Change>)
    requires changes_shape(days@, bin), bucket(date as int) == bin, changes_bounded(days@, bound),
        0 <= bound < 0x4000000000000000, count_delta == 1 || count_delta == -1,
    ensures changes_shape(result@, bin), changes_bounded(result@, bound + 1),
        forall|other: i32| #[trigger] changes_count(result@, other) == changes_count(days@, other) +
            if other == date { count_delta as int } else { 0int },
        forall|other: i32| #[trigger] changes_revenue(result@, other) == changes_revenue(days@, other) +
            if other == date { revenue_delta@ } else { 0int },
{
    let ghost before = days@;
    let mut position = 0;
    while position < days.len() && days[position].date < date
        invariant position <= days.len(), days@ == before, changes_shape(days@, bin),
            forall|index: int| 0 <= index < position ==> (#[trigger] days@[index]).date < date,
        decreases days.len() - position,
    { position += 1; }
    if position < days.len() && days[position].date == date {
        let count = days[position].count + count_delta;
        let revenue = days[position].revenue.add(&revenue_delta);
        let value = Change { date, count, revenue };
        proof {
            assert forall|other: i32| #[trigger] changes_count(before.update(position as int, value), other) == changes_count(before, other) +
                if other == date { count_delta as int } else { 0int } by {
                lemma_sum_update(before, position as int, value, |change: Change| if change.date == other { change.count as int } else { 0int });
            }
            assert forall|other: i32| #[trigger] changes_revenue(before.update(position as int, value), other) == changes_revenue(before, other) +
                if other == date { revenue_delta@ } else { 0int } by {
                lemma_sum_update(before, position as int, value, |change: Change| if change.date == other { change.revenue@ } else { 0int });
            }
            assert forall|index: int| 0 <= index < before.len() implies (#[trigger] before.update(position as int, value)[index]).date == before[index].date by { }
            assert forall|index: int| 0 <= index < before.len() implies -(bound + 1) <= (#[trigger] before.update(position as int, value)[index]).count <= bound + 1 by {
                if index != position as int { assert(before.update(position as int, value)[index] == before[index]); }
            }
        }
        days.set(position, value);
    } else {
        proof {
            assert forall|index: int| 0 <= index < days.len() implies (#[trigger] days@[index]).date as int != date as int by {
                if index < position as int { assert(days@[index].date < date); }
                else if position < days.len() {
                    assert(days@[position as int].date > date);
                    if index > position as int { assert(days@[position as int].date < days@[index].date); }
                }
            }
            lemma_room_changes(days@, bin, date as int);
        }
        let value = Change { date, count: count_delta, revenue: revenue_delta };
        days.insert(position, value);
        proof {
            assert(days@.remove(position as int) =~= before);
            assert forall|other: i32| #[trigger] changes_count(days@, other) == changes_count(before, other) +
                if other == date { count_delta as int } else { 0int } by {
                lemma_sum_remove(days@, position as int, |change: Change| if change.date == other { change.count as int } else { 0int });
            }
            assert forall|other: i32| #[trigger] changes_revenue(days@, other) == changes_revenue(before, other) +
                if other == date { revenue_delta@ } else { 0int } by {
                lemma_sum_remove(days@, position as int, |change: Change| if change.date == other { change.revenue@ } else { 0int });
            }
            assert forall|index: int| 0 <= index < days.len() implies bucket((#[trigger] days@[index]).date as int) == bin by {
                if index < position { assert(days@[index] == before[index]); }
                else if index > position { assert(days@[index] == before[index - 1]); }
            }
            assert forall|index: int| 0 <= index < days.len() implies -(bound + 1) <= (#[trigger] days@[index]).count <= bound + 1 by {
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
    days
}

pub proof fn lemma_day_unique(days: Seq<Day>, bin: int, index: int)
    requires days_shape(days, bin), 0 <= index < days.len(),
    ensures days_count(days, days[index].shipdate as int) == days[index].count as int,
        days_revenue(days, days[index].shipdate as int) == days[index].revenue@,
{
    let date = days[index].shipdate as int;
    let count = |day: Day| if day.shipdate as int == date { day.count as int } else { 0int };
    let revenue = |day: Day| if day.shipdate as int == date { day.revenue@ } else { 0int };
    assert forall|other: int| 0 <= other < days.len() && other != index implies count(#[trigger] days[other]) == 0 && revenue(days[other]) == 0 by {
        if other < index { assert(days[other].shipdate < days[index].shipdate); } else { assert(days[index].shipdate < days[other].shipdate); }
    }
    lemma_sum_single_match(days, count, index);
    lemma_sum_single_match(days, revenue, index);
}
pub proof fn lemma_change_unique(changes: Seq<Change>, bin: int, index: int)
    requires changes_shape(changes, bin), 0 <= index < changes.len(),
    ensures change_count_int(changes, changes[index].date as int) == changes[index].count as int,
        change_revenue_int(changes, changes[index].date as int) == changes[index].revenue@,
{
    let date = changes[index].date as int;
    let count = |change: Change| if change.date as int == date { change.count as int } else { 0int };
    let revenue = |change: Change| if change.date as int == date { change.revenue@ } else { 0int };
    assert forall|other: int| 0 <= other < changes.len() && other != index implies count(#[trigger] changes[other]) == 0 && revenue(changes[other]) == 0 by {
        if other < index { assert(changes[other].date < changes[index].date); } else { assert(changes[index].date < changes[other].date); }
    }
    lemma_sum_single_match(changes, count, index);
    lemma_sum_single_match(changes, revenue, index);
}
pub proof fn lemma_changes_before(changes: Seq<Change>, bin: int, taken: int, date: int)
    requires changes_shape(changes, bin), 0 <= taken <= changes.len(), taken < changes.len() ==> date == changes[taken].date as int,
        taken == changes.len() ==> true,
    ensures taken < changes.len() ==> change_count_int(changes.take(taken), date) == 0 && change_revenue_int(changes.take(taken), date) == 0,
{
    if taken < changes.len() {
        let prefix = changes.take(taken);
        assert forall|index: int| 0 <= index < prefix.len() implies (#[trigger] prefix[index]).date as int != date by {
            assert(prefix[index] == changes[index]);
            assert(changes[index].date < changes[taken].date);
        }
        lemma_sum_zero(prefix, |change: Change| if change.date as int == date { change.count as int } else { 0int });
        lemma_sum_zero(prefix, |change: Change| if change.date as int == date { change.revenue@ } else { 0int });
    }
}
/// A day's rows in the database: a nonnegative count at most the row count, and no revenue without rows.
pub proof fn lemma_day_rows(rows: Seq<LineitemRow>, supplier: int, date: int)
    ensures 0 <= day_count(rows, supplier, date) <= rows.len(),
        day_count(rows, supplier, date) == 0 ==> day_revenue(rows, supplier, date) == 0,
{
    let count = |row: LineitemRow| if row.l_shipdate == date && row.l_suppkey == supplier { 1int } else { 0int };
    let one = |row: LineitemRow| 1int;
    verus_prooflib::lemma_sum_ones(rows);
    lemma_sum_nonneg(rows, count);
    lemma_sum_le(rows, count, one);
    if day_count(rows, supplier, date) == 0 {
        assert forall|index: int| 0 <= index < rows.len() implies !(rows[index].l_shipdate == date && rows[index].l_suppkey == supplier) by {
            if rows[index].l_shipdate == date && rows[index].l_suppkey == supplier { lemma_sum_pos(rows, count, index); }
        }
        lemma_sum_zero(rows, |row: LineitemRow| if row.l_shipdate == date && row.l_suppkey == supplier { weight(row) } else { 0int });
    }
}

/// Drops the day at `position`, whose count and revenue the change cancels.
#[verifier::rlimit(100)]
#[verifier::spinoff_prover]
pub fn drop_day(days: &mut Vec<Day>, position: usize, count: i64, revenue: &Wide, Ghost(bin): Ghost<int>)
    requires days_shape(old(days)@, bin), position < old(days).len(),
        old(days)@[position as int].count as int + count as int == 0, old(days)@[position as int].revenue@ + revenue@ == 0,
    ensures days_shape(final(days)@, bin),
        forall|query: int| #[trigger] days_count(final(days)@, query) == days_count(old(days)@, query) + if query == old(days)@[position as int].shipdate as int { count as int } else { 0int },
        forall|query: int| #[trigger] days_revenue(final(days)@, query) == days_revenue(old(days)@, query) + if query == old(days)@[position as int].shipdate as int { revenue@ } else { 0int },
        final(days)@ == old(days)@.remove(position as int),
{
    let ghost before = days@;
    days.remove(position);
    proof {
        assert(days@ =~= before.remove(position as int));
        assert forall|query: int| #[trigger] days_count(days@, query) == days_count(before, query) + if query == before[position as int].shipdate as int { count as int } else { 0int } by {
            lemma_sum_remove(before, position as int, |day: Day| if day.shipdate as int == query { day.count as int } else { 0int });
        }
        assert forall|query: int| #[trigger] days_revenue(days@, query) == days_revenue(before, query) + if query == before[position as int].shipdate as int { revenue@ } else { 0int } by {
            lemma_sum_remove(before, position as int, |day: Day| if day.shipdate as int == query { day.revenue@ } else { 0int });
        }
        assert forall|index: int| 0 <= index < days.len() implies #[trigger] days@[index].count > 0 && bucket(days@[index].shipdate as int) == bin by {
            assert(days@[index] == before[if index < position as int { index } else { index + 1 }]);
        }
        assert forall|left: int, right: int| #![trigger days@[left], days@[right]] 0 <= left < right < days.len() implies days@[left].shipdate < days@[right].shipdate by {
            assert(days@[left] == before[if left < position as int { left } else { left + 1 }]);
            assert(days@[right] == before[if right < position as int { right } else { right + 1 }]);
        }
    }
}
/// Adds the change to the day at `position`, which stays present with a positive count.
#[verifier::rlimit(100)]
#[verifier::spinoff_prover]
pub fn set_day(days: &mut Vec<Day>, position: usize, count: i64, revenue: &Wide, Ghost(bin): Ghost<int>)
    requires days_shape(old(days)@, bin), position < old(days).len(),
        0 < old(days)@[position as int].count as int + count as int <= 0xffffffffffffffffint,
    ensures days_shape(final(days)@, bin), final(days).len() == old(days).len(),
        forall|query: int| #[trigger] days_count(final(days)@, query) == days_count(old(days)@, query) + if query == old(days)@[position as int].shipdate as int { count as int } else { 0int },
        forall|query: int| #[trigger] days_revenue(final(days)@, query) == days_revenue(old(days)@, query) + if query == old(days)@[position as int].shipdate as int { revenue@ } else { 0int },
        forall|index: int| 0 <= index < final(days).len() ==> final(days)@[index].shipdate == old(days)@[index].shipdate,
{
    let ghost before = days@;
    let total = days[position].count as i128 + count as i128;
    let merged = days[position].revenue.add(revenue);
    let value = Day { shipdate: days[position].shipdate, count: total as u64, revenue: merged };
    proof {
        assert forall|query: int| #[trigger] days_count(before.update(position as int, value), query) == days_count(before, query) + if query == value.shipdate as int { count as int } else { 0int } by {
            lemma_sum_update(before, position as int, value, |day: Day| if day.shipdate as int == query { day.count as int } else { 0int });
        }
        assert forall|query: int| #[trigger] days_revenue(before.update(position as int, value), query) == days_revenue(before, query) + if query == value.shipdate as int { revenue@ } else { 0int } by {
            lemma_sum_update(before, position as int, value, |day: Day| if day.shipdate as int == query { day.revenue@ } else { 0int });
        }
        assert forall|index: int| 0 <= index < before.len() implies #[trigger] before.update(position as int, value)[index].count > 0
            && bucket(before.update(position as int, value)[index].shipdate as int) == bin && before.update(position as int, value)[index].shipdate == before[index].shipdate by {
            if index != position as int { assert(before.update(position as int, value)[index] == before[index]); }
        }
        assert forall|left: int, right: int| #![trigger before.update(position as int, value)[left], before.update(position as int, value)[right]]
            0 <= left < right < before.len() implies before.update(position as int, value)[left].shipdate < before.update(position as int, value)[right].shipdate by {
            if left != position as int { assert(before.update(position as int, value)[left] == before[left]); }
            if right != position as int { assert(before.update(position as int, value)[right] == before[right]); }
        }
    }
    days.set(position, value);
}
/// Inserts a new day at `position` among the days.
#[verifier::rlimit(100)]
#[verifier::spinoff_prover]
pub fn add_day(days: &mut Vec<Day>, position: usize, date: i32, count: i64, revenue: &Wide, Ghost(bin): Ghost<int>)
    requires days_shape(old(days)@, bin), position <= old(days).len(), old(days).len() < 256, bucket(date as int) == bin, count > 0,
        forall|index: int| 0 <= index < position ==> (#[trigger] old(days)@[index]).shipdate < date,
        forall|index: int| position <= index < old(days).len() ==> (#[trigger] old(days)@[index]).shipdate > date,
    ensures days_shape(final(days)@, bin),
        forall|query: int| #[trigger] days_count(final(days)@, query) == days_count(old(days)@, query) + if query == date as int { count as int } else { 0int },
        forall|query: int| #[trigger] days_revenue(final(days)@, query) == days_revenue(old(days)@, query) + if query == date as int { revenue@ } else { 0int },
        final(days).len() == old(days).len() + 1,
        forall|index: int| 0 <= index <= position ==> (#[trigger] final(days)@[index]).shipdate <= date,
{
    let ghost before = days@;
    let value = Day { shipdate: date, count: count as u64, revenue: revenue.dup() };
    days.insert(position, value);
    proof {
        assert(days@.remove(position as int) =~= before);
        assert forall|query: int| #[trigger] days_count(days@, query) == days_count(before, query) + if query == date as int { count as int } else { 0int } by {
            lemma_sum_remove(days@, position as int, |day: Day| if day.shipdate as int == query { day.count as int } else { 0int });
        }
        assert forall|query: int| #[trigger] days_revenue(days@, query) == days_revenue(before, query) + if query == date as int { revenue@ } else { 0int } by {
            lemma_sum_remove(days@, position as int, |day: Day| if day.shipdate as int == query { day.revenue@ } else { 0int });
        }
        assert forall|index: int| 0 <= index < days.len() implies #[trigger] days@[index].count > 0 && bucket(days@[index].shipdate as int) == bin by {
            if index < position as int { assert(days@[index] == before[index]); }
            else if index > position as int { assert(days@[index] == before[index - 1]); }
        }
        assert forall|left: int, right: int| #![trigger days@[left], days@[right]]
            0 <= left < right < days.len() implies days@[left].shipdate < days@[right].shipdate by {
            let old_left = if left < position as int { left } else { left - 1 };
            let old_right = if right < position as int { right } else { right - 1 };
            if right == position as int { assert(before[left].shipdate < date); }
            else if left == position as int {
                assert(before[old_right].shipdate > date);
            } else {
                assert(days@[left] == before[old_left]);
                assert(days@[right] == before[old_right]);
            }
        }
        assert forall|index: int| 0 <= index <= position implies (#[trigger] days@[index]).shipdate <= date by {
            if index < position as int { assert(days@[index] == before[index]); }
        }
    }
}
/// Adds a change to the day `date` of a bucket's days, inserting or dropping the day as its count requires.
#[verifier::rlimit(100)]
pub fn apply_change(days: &mut Vec<Day>, from: usize, date: i32, count: i64, revenue: &Wide, Ghost(bin): Ghost<int>) -> (next: usize)
    requires days_shape(old(days)@, bin), bucket(date as int) == bin, from <= old(days).len(),
        forall|index: int| 0 <= index < from ==> (#[trigger] old(days)@[index]).shipdate < date,
        0 <= days_count(old(days)@, date as int) + count as int <= 0xffffffffffffffffint,
        days_count(old(days)@, date as int) + count as int == 0 ==> days_revenue(old(days)@, date as int) + revenue@ == 0,
    ensures days_shape(final(days)@, bin),
        forall|query: int| #[trigger] days_count(final(days)@, query) == days_count(old(days)@, query) + if query == date as int { count as int } else { 0int },
        forall|query: int| #[trigger] days_revenue(final(days)@, query) == days_revenue(old(days)@, query) + if query == date as int { revenue@ } else { 0int },
        next <= final(days).len(), forall|index: int| 0 <= index < next ==> (#[trigger] final(days)@[index]).shipdate <= date,
{
    let ghost before = days@;
    let mut position = from;
    while position < days.len() && days[position].shipdate < date
        invariant from <= position <= days.len(), days@ == before, days_shape(before, bin),
            forall|index: int| 0 <= index < position ==> before[index].shipdate < date,
        decreases days.len() - position,
    { position += 1; }
    if position < days.len() && days[position].shipdate == date {
        proof { lemma_day_unique(before, bin, position as int); }
        let total = days[position].count as i128 + count as i128;
        if total == 0 {
            drop_day(days, position, count, revenue, Ghost(bin));
            position
        } else {
            set_day(days, position, count, revenue, Ghost(bin));
            position + 1
        }
    } else {
        proof {
            assert forall|index: int| 0 <= index < days.len() implies (#[trigger] days@[index]).shipdate as int != date as int by {
                if index < position as int { assert(days@[index].shipdate < date); }
                else if position < days.len() {
                    assert(days@[position as int].shipdate > date);
                    if index > position as int { assert(days@[position as int].shipdate < days@[index].shipdate); }
                }
            }
            lemma_room_days(before, bin, date as int);
            lemma_sum_zero(before, |day: Day| if day.shipdate as int == date as int { day.count as int } else { 0int });
            lemma_sum_zero(before, |day: Day| if day.shipdate as int == date as int { day.revenue@ } else { 0int });
            assert forall|index: int| position <= index < days.len() implies (#[trigger] days@[index]).shipdate > date by {
                if index > position as int { assert(days@[position as int].shipdate < days@[index].shipdate); }
            }
        }
        if count == 0 {
            proof { assert(revenue@ == 0); }
            position
        } else {
            add_day(days, position, date, count, revenue, Ghost(bin));
            position + 1
        }
    }
}
pub fn copy_days(source: &Vec<Day>) -> (out: Vec<Day>)
    ensures out@ == source@,
{
    let mut out: Vec<Day> = Vec::with_capacity(source.len());
    let mut index = 0usize;
    while index < source.len()
        invariant index <= source.len(), out@ == source@.take(index as int),
        decreases source.len() - index,
    {
        out.push(source[index].dup());
        index += 1;
    }
    proof { assert(source@.take(source.len() as int) =~= source@); }
    out
}
#[verifier::rlimit(150)]
#[verifier::spinoff_prover]
pub fn merge_profile(previous: &Option<Arc<BucketProfile>>, changes: &Vec<Change>, Ghost(bin): Ghost<int>,
    Ghost(target): Ghost<Seq<LineitemRow>>, Ghost(supplier): Ghost<int>) -> (result: BucketProfile)
    requires changes_shape(changes@, bin), previous is Some ==> profile_shape(*previous->0, bin), target.len() <= max_rows(),
        forall|date: int| bucket(date) == bin ==> #[trigger] days_count(previous_days(*previous), date) + change_count_int(changes@, date) == day_count(target, supplier, date),
        forall|date: int| bucket(date) == bin ==> #[trigger] days_revenue(previous_days(*previous), date) + change_revenue_int(changes@, date) == day_revenue(target, supplier, date),
    ensures days_shape(result.days@, bin),
        result.count > 0 ==> profile_shape(result, bin),
        result.count == 0 ==> result.days.len() == 0 && result.revenue@ == 0,
        result.count as int == sum(result.days@, |day: Day| day.count as int),
        result.revenue@ == sum(result.days@, |day: Day| day.revenue@),
        forall|date: int| #[trigger] days_count(result.days@, date) == days_count(previous_days(*previous), date) + change_count_int(changes@, date),
        forall|date: int| #[trigger] days_revenue(result.days@, date) == days_revenue(previous_days(*previous), date) + change_revenue_int(changes@, date),
{
    let mut days: Vec<Day> = match previous { Some(profile) => copy_days(&profile.days), None => Vec::new() };
    let ghost original = previous_days(*previous);
    proof { assert(days@ == original); }
    let mut start = 0usize;
    let mut done = 0usize;
    proof {
        assert(changes@.take(0) =~= Seq::<Change>::empty());
        assert forall|query: int| change_count_int(changes@.take(0), query) == 0 && change_revenue_int(changes@.take(0), query) == 0 by {
            lemma_sum_empty(changes@.take(0), |change: Change| if change.date as int == query { change.count as int } else { 0int });
            lemma_sum_empty(changes@.take(0), |change: Change| if change.date as int == query { change.revenue@ } else { 0int });
        }
    }
    while done < changes.len()
        invariant done <= changes.len(), start <= days.len(), changes_shape(changes@, bin), days_shape(days@, bin), days_shape(original, bin),
            original == previous_days(*previous), target.len() <= max_rows(),
            forall|date: int| bucket(date) == bin ==> #[trigger] days_count(original, date) + change_count_int(changes@, date) == day_count(target, supplier, date),
            forall|date: int| bucket(date) == bin ==> #[trigger] days_revenue(original, date) + change_revenue_int(changes@, date) == day_revenue(target, supplier, date),
            forall|query: int| #[trigger] days_count(days@, query) == days_count(original, query) + change_count_int(changes@.take(done as int), query),
            forall|query: int| #[trigger] days_revenue(days@, query) == days_revenue(original, query) + change_revenue_int(changes@.take(done as int), query),
            done < changes.len() ==> forall|index: int| 0 <= index < start ==> (#[trigger] days@[index]).shipdate < changes@[done as int].date,
        decreases changes.len() - done,
    {
        let change = &changes[done];
        let ghost date = change.date as int;
        let ghost before = days@;
        proof {
            assert(bucket(date) == bin);
            lemma_changes_before(changes@, bin, done as int, date);
            lemma_change_unique(changes@, bin, done as int);
            lemma_day_rows(target, supplier, date);
            assert(days_count(before, date) == days_count(original, date));
            assert(days_revenue(before, date) == days_revenue(original, date));
            assert(days_count(before, date) + change.count as int == day_count(target, supplier, date));
        }
        let next = apply_change(&mut days, start, change.date, change.count, &change.revenue, Ghost(bin));
        proof {
            assert forall|query: int| #[trigger] days_count(days@, query) == days_count(original, query) + change_count_int(changes@.take(done as int + 1), query) by {
                lemma_sum_take(changes@, done as int, |c: Change| if c.date as int == query { c.count as int } else { 0int });
            }
            assert forall|query: int| #[trigger] days_revenue(days@, query) == days_revenue(original, query) + change_revenue_int(changes@.take(done as int + 1), query) by {
                lemma_sum_take(changes@, done as int, |c: Change| if c.date as int == query { c.revenue@ } else { 0int });
            }
            if done + 1 < changes.len() {
                assert(changes@[done as int].date < changes@[done as int + 1].date);
            }
        }
        start = next;
        done += 1;
    }
    proof {
        assert(changes@.take(done as int) =~= changes@);
        assert forall|index: int| 0 <= index < days.len() implies (#[trigger] days@[index]).count as int <= 0x10000000000int by {
            lemma_day_unique(days@, bin, index);
            let date = days@[index].shipdate as int;
            assert(bucket(date) == bin);
            lemma_day_rows(target, supplier, date);
            assert(days_count(days@, date) == day_count(target, supplier, date));
        }
    }
    let mut count = 0i128;
    let mut revenue = Wide::zero();
    let mut index = 0usize;
    proof {
        assert(days@.take(0) =~= Seq::<Day>::empty());
        lemma_sum_empty(days@.take(0), |day: Day| day.count as int);
        lemma_sum_empty(days@.take(0), |day: Day| day.revenue@);
    }
    while index < days.len()
        invariant index <= days.len(), days.len() <= 256, days_shape(days@, bin),
            forall|position: int| 0 <= position < days.len() ==> (#[trigger] days@[position]).count as int <= 0x10000000000int,
            count == sum(days@.take(index as int), |day: Day| day.count as int), revenue@ == sum(days@.take(index as int), |day: Day| day.revenue@),
            0 <= count <= index as int * 0x10000000000int,
        decreases days.len() - index,
    {
        proof {
            lemma_sum_take(days@, index as int, |day: Day| day.count as int);
            lemma_sum_take(days@, index as int, |day: Day| day.revenue@);
        }
        count = count + days[index].count as i128;
        revenue = revenue.add(&days[index].revenue);
        index += 1;
    }
    proof {
        assert(days@.take(days.len() as int) =~= days@);
        lemma_sum_nonneg(days@, |day: Day| day.count as int);
        if count == 0 && days.len() > 0 {
            lemma_sum_pos(days@, |day: Day| day.count as int, 0);
            assert(false);
        }
    }
    BucketProfile { count: count as u64, revenue, days }
}

pub proof fn lemma_changes_outside(changes: Seq<Change>, date: int)
    requires date < i32::MIN || date > i32::MAX,
    ensures change_count_int(changes, date) == 0, change_revenue_int(changes, date) == 0,
{
    lemma_sum_zero(changes, |change: Change| if change.date as int == date { change.count as int } else { 0int });
    lemma_sum_zero(changes, |change: Change| if change.date as int == date { change.revenue@ } else { 0int });
}
pub proof fn lemma_facts_outside(facts: Facts, supplier: int, date: int)
    requires facts_shape(facts), !(i64::MIN <= supplier <= i64::MAX && i32::MIN <= date <= i32::MAX),
    ensures stored_day_count(facts, supplier, date) == 0, stored_day_revenue(facts, supplier, date) == 0,
{
    if let Some(profile) = profile_at(facts, bucket(date), supplier) {
        assert(i64::MIN <= supplier <= i64::MAX);
        lemma_sum_zero(profile.days@, |day: Day| if day.shipdate as int == date { day.count as int } else { 0int });
        lemma_sum_zero(profile.days@, |day: Day| if day.shipdate as int == date { day.revenue@ } else { 0int });
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


pub proof fn lemma_supplier_rows(rows: Seq<SupplierRow>, supplier: int, key: KeyView)
    ensures 0 <= supplier_count(rows, supplier, key) <= rows.len(),
{
    let count = |row: SupplierRow| if row.s_suppkey == supplier && variant_key(row.s_name, row.s_address, row.s_phone) == key { 1int } else { 0int };
    let one = |row: SupplierRow| 1int;
    verus_prooflib::lemma_sum_ones(rows);
    lemma_sum_nonneg(rows, count);
    lemma_sum_le(rows, count, one);
}
pub struct VariantChange {
    pub name: Vec<u8>, pub address: Vec<u8>, pub phone: Vec<u8>, pub count: Wide,
}
pub type DimensionChanges = Index<Index<VariantChange>>;
pub open spec fn variant_count(variants: Index<Variant>, key: KeyView) -> int {
    match variants.at(key) { Some(value) => value.count as int, None => 0 }
}
pub open spec fn variant_change_count(variants: Index<VariantChange>, key: KeyView) -> int {
    match variants.at(key) { Some(value) => value.count@, None => 0 }
}
pub open spec fn variant_count_weight(key: KeyView) -> spec_fn((KeyView, VariantChange)) -> int {
    |entry: (KeyView, VariantChange)| if entry.0 == key { entry.1.count@ } else { 0int }
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
        Some(inner) => match inner.at(key) { Some(value) => value.count@, None => 0 },
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
pub fn add_supplier(changes: &mut DimensionChanges, row: &SupplierInput, sign: i64)
    requires dimension_changes_shape(*old(changes)),
    ensures dimension_changes_shape(*final(changes)),
        forall|supplier: int, key: KeyView| #[trigger] change_at(*final(changes), supplier, key) ==
        change_at(*old(changes), supplier, key) + if supplier == row.s_suppkey as int &&
            key == variant_key(row.s_name@, row.s_address@, row.s_phone@) { sign as int } else { 0int },
{
    let ghost before = *changes;
    let supplier_key = Key::number(row.s_suppkey);
    let key = text_key(&row.s_name, &row.s_address, &row.s_phone);
    let ghost changed_key = key.view();
    let mut inner = match changes.get(&supplier_key) { Some(value) => value.share(), None => Index::new() };
    let ghost old_inner = inner;
    let previous = match inner.get(&key) { Some(value) => value.count.dup(), None => Wide::zero() };
    let count = previous.add(&Wide::from_i128(sign as i128));
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
}
#[verifier::rlimit(100)]
#[verifier::spinoff_prover]
pub fn finish_suppliers(old: &Dimensions, changes: &DimensionChanges, Ghost(target): Ghost<Seq<SupplierRow>>) -> (result: Dimensions)
    requires dimensions_shape(*old), dimension_changes_shape(*changes), target.len() <= max_rows(),
        forall|supplier: int, key: KeyView| #[trigger] stored_supplier_count(*old, supplier, key) + change_at(*changes, supplier, key) == supplier_count(target, supplier, key),
    ensures dimensions_shape(result),
        forall|supplier: int, key: KeyView| #[trigger] stored_supplier_count(result, supplier, key) ==
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
            changes.wf(), target.len() <= max_rows(),
            forall|supplier: int, key: KeyView| #[trigger] stored_supplier_count(*old, supplier, key) + change_at(*changes, supplier, key) == supplier_count(target, supplier, key),
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
                return result;
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
        proof {
            let supplier = supplier_key.number as int;
            assert forall|key: KeyView| #[trigger] variant_count(inner, key) + variant_change_count(*variants, key) == supplier_count(target, supplier, key) by {
                let weight = |entry: (KeyView, Index<VariantChange>)|
                    if entry.0 == number_key(supplier) { variant_change_count(entry.1, key) } else { 0int };
                assert forall|index: int| 0 <= index < changes.entries().take(processed@).len() implies weight(#[trigger] changes.entries().take(processed@)[index]) == 0 by {
                    assert(changes.entries().take(processed@)[index] == changes.entries()[index]);
                    verus_prooflib::ordered::lemma_key_order(changes.entries()[index].0, supplier_key.view());
                    assert(verus_prooflib::ordered::key_lt(changes.entries()[index].0, changes.entries()[processed@].0));
                }
                lemma_sum_zero(changes.entries().take(processed@), weight);
                assert(stored_supplier_count(result, supplier, key) == stored_supplier_count(*old, supplier, key) + sum(changes.entries().take(processed@), weight));
                assert(stored_supplier_count(*old, supplier, key) + change_at(*changes, supplier, key) == supplier_count(target, supplier, key));
                assert(variant_count(inner, key) == stored_supplier_count(result, supplier, key));
                assert(variant_change_count(*variants, key) == change_at(*changes, supplier, key));
            }
        }
        let inner = finish_variants(&inner, &*variants, Ghost(target), Ghost(supplier_key.number as int));
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
#[verifier::rlimit(100)]
#[verifier::spinoff_prover]
pub fn finish_variants(base: &Index<Variant>, changes: &Index<VariantChange>, Ghost(target): Ghost<Seq<SupplierRow>>, Ghost(supplier): Ghost<int>) -> (result: Index<Variant>)
    requires variants_shape(*base), variant_changes_shape(*changes), target.len() <= max_rows(),
        forall|key: KeyView| #[trigger] variant_count(*base, key) + variant_change_count(*changes, key) == supplier_count(target, supplier, key),
    ensures variants_shape(result) && result.wf(),
        forall|key: KeyView| #[trigger] variant_count(result, key) == variant_count(*base, key) + variant_change_count(*changes, key),
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
            changes.wf(), result.wf(), target.len() <= max_rows(),
            forall|key: KeyView| #[trigger] variant_count(*base, key) + variant_change_count(*changes, key) == supplier_count(target, supplier, key),
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
                return result;
            },
        };
        let ghost before = result;
        proof {
            changes.model();
            assert(changes.entries()[processed@] == (key.view(), *change));
            assert(changes.at(key.view()) == Some(*change));
        }
        let previous = match result.get(&key) { Some(value) => value.count, None => 0 };
        proof {
            assert(previous as int == variant_count(before, key.view()));
            let weight = variant_count_weight(key.view());
            assert forall|index: int| 0 <= index < changes.entries().take(processed@).len() implies weight(#[trigger] changes.entries().take(processed@)[index]) == 0 by {
                assert(changes.entries().take(processed@)[index] == changes.entries()[index]);
                verus_prooflib::ordered::lemma_key_order(changes.entries()[index].0, key.view());
                assert(verus_prooflib::ordered::key_lt(changes.entries()[index].0, changes.entries()[processed@].0));
            }
            lemma_sum_zero(changes.entries().take(processed@), weight);
            assert(variant_count(before, key.view()) == variant_count(*base, key.view()));
            assert(variant_change_count(*changes, key.view()) == change.count@);
            lemma_supplier_rows(target, supplier, key.view());
        }
        let wide = Wide::from_i128(previous as i128).add(&change.count);
        proof {
            assert(variant_count(*base, key.view()) + variant_change_count(*changes, key.view()) == supplier_count(target, supplier, key.view()));
            assert(wide@ == supplier_count(target, supplier, key.view()));
        }
        let count = wide.to_i128().unwrap() as u64;
        let ghost changed_key = key.view();
        if count == 0 { result.remove(&key); }
        else {
            result.put(key, Arc::new(Variant {
                name: copy_bytes(&change.name), address: copy_bytes(&change.address), phone: copy_bytes(&change.phone), count,
            }));
        }
        proof {
            assert forall|query: KeyView| #[trigger] variant_count(result, query) == variant_count(*base, query) +
                sum(changes.entries().take(processed@ + 1), |entry: (KeyView, VariantChange)| if entry.0 == query { entry.1.count@ } else { 0int }) by {
                let weight = |entry: (KeyView, VariantChange)| if entry.0 == query { entry.1.count@ } else { 0int };
                assert(variant_count(before, query) == variant_count(*base, query) + sum(changes.entries().take(processed@), weight));
                lemma_sum_take(changes.entries(), processed@, weight);
                if query == changed_key { assert(variant_count(result, query) == count as int); assert(count as int == wide@); }
                else { assert(result.at(query) == before.at(query)); }
            }
        }
        processed = Ghost(processed@ + 1);
    }
}
}
