use vstd::prelude::*;
use crate::gen_spec::*;
use verus_common::sums::*;
use crate::maps::*;
use crate::proof_support::lemma_sum_insert;

verus! {

#[derive(Clone, Copy)]
pub struct Bucket {
    pub date: i32,
    pub count: i128,
    pub revenue: i128,
}

pub open spec fn line_count(db: Db, key: int, date: int) -> int {
    sum(db.lineitem, |row: LineitemRow|
        if row.l_orderkey == key && row.l_shipdate >= date { 1int } else { 0 })
}

pub open spec fn line_revenue(db: Db, key: int, date: int) -> int {
    sum(db.lineitem, |row: LineitemRow|
        if row.l_orderkey == key && row.l_shipdate >= date {
            row.l_extendedprice * (100 - row.l_discount)
        } else { 0 })
}

pub open spec fn profile_ok(db: Db, key: i64, rows: Seq<Bucket>) -> bool {
    &&& forall|position: int| 0 <= position < rows.len() ==>
        (#[trigger] rows[position]).count == line_count(db, key as int, rows[position].date as int)
        && rows[position].revenue == line_revenue(db, key as int, rows[position].date as int)
        && rows[position].count > 0
    &&& forall|first: int, second: int| 0 <= first < second < rows.len() ==>
        (#[trigger] rows[first]).date < (#[trigger] rows[second]).date
    &&& forall|date: i32| #[trigger] line_count(db, key as int, date as int)
        - line_count(db, key as int, date as int + 1) > 0 <==>
        exists|position: int| 0 <= position < rows.len() && (#[trigger] rows[position]).date == date
}

pub open spec fn dates_sorted(rows: Seq<Bucket>) -> bool {
    forall|first: int, second: int| 0 <= first < second < rows.len() ==>
        (#[trigger] rows[first]).date < (#[trigger] rows[second]).date
}

pub open spec fn suffix_count(rows: Seq<Bucket>, date: int) -> int {
    sum(rows, |row: Bucket| if row.date >= date { row.count as int } else { 0 })
}

pub open spec fn suffix_revenue(rows: Seq<Bucket>, date: int) -> int {
    sum(rows, |row: Bucket| if row.date >= date { row.revenue as int } else { 0 })
}

pub open spec fn bucket_work_ok(work: Work<Vec<Bucket>>) -> bool {
    work_complete(work) && forall|key: i64| (#[trigger] work.data@.contains_key(key)) ==> dates_sorted(work.data@[key]@)
}

pub open spec fn suffix_conversion(rows: Seq<Bucket>, profile: Seq<Bucket>) -> bool {
    &&& dates_sorted(profile)
    &&& forall|position: int| 0 <= position < profile.len() ==>
        (#[trigger] profile[position]).count == suffix_count(rows, profile[position].date as int)
        && profile[position].revenue == suffix_revenue(rows, profile[position].date as int)
        && profile[position].count > 0
    &&& forall|date: i32| #[trigger] date_count(rows, date) > 0 <==>
        exists|position: int| 0 <= position < profile.len() && (#[trigger] profile[position]).date == date
}

pub proof fn lemma_bucket_support(rows: Seq<Bucket>, date: i32)
    requires forall|position: int| 0 <= position < rows.len() ==> (#[trigger] rows[position]).count > 0,
    ensures date_count(rows, date) > 0 <==>
        exists|position: int| 0 <= position < rows.len() && (#[trigger] rows[position]).date == date,
{
    let weight = |row: Bucket| if row.date == date { row.count as int } else { 0 };
    if date_count(rows, date) > 0 {
        let position = lemma_sum_nonzero_witness(rows, weight);
        assert(rows[position].date == date);
    }
    if exists|position: int| 0 <= position < rows.len() && (#[trigger] rows[position]).date == date {
        let position = choose|position: int| 0 <= position < rows.len() && (#[trigger] rows[position]).date == date;
        lemma_sum_ge_term(rows, weight, position);
    }
}

pub open spec fn after_position_ok(rows: Seq<Bucket>, cutoff: i32, result: Option<Bucket>) -> bool {
    match result {
        None => forall|position: int| 0 <= position < rows.len() ==> (#[trigger] rows[position]).date <= cutoff,
        Some(row) => exists|position: int| 0 <= position < rows.len() && (#[trigger] rows[position]) == row && row.date > cutoff &&
            forall|prior: int| 0 <= prior < position ==> (#[trigger] rows[prior]).date <= cutoff,
    }
}

pub proof fn lemma_after_position(rows: Seq<Bucket>, cutoff: i32, position: int)
    requires 0 <= position < rows.len(), rows[position].date > cutoff,
        forall|prior: int| 0 <= prior < position ==> (#[trigger] rows[prior]).date <= cutoff,
    ensures after_position_ok(rows, cutoff, Some(rows[position])),
{
    assert(exists|selected: int| 0 <= selected < rows.len() && (#[trigger] rows[selected]) == rows[position] && rows[selected].date > cutoff &&
        forall|prior: int| 0 <= prior < selected ==> (#[trigger] rows[prior]).date <= cutoff);
}

pub proof fn lemma_line_point_count(db: Db, key: int, date: int)
    ensures line_count(db, key, date) - line_count(db, key, date + 1) ==
        sum(db.lineitem, |row: LineitemRow| if row.l_orderkey == key && row.l_shipdate == date { 1int } else { 0 }),
{
    lemma_sum_sub(db.lineitem,
        |row: LineitemRow| if row.l_orderkey == key && row.l_shipdate >= date { 1int } else { 0 },
        |row: LineitemRow| if row.l_orderkey == key && row.l_shipdate >= date + 1 { 1int } else { 0 },
        |row: LineitemRow| if row.l_orderkey == key && row.l_shipdate == date { 1int } else { 0 });
}

pub proof fn lemma_line_point_revenue(db: Db, key: int, date: int)
    ensures line_revenue(db, key, date) - line_revenue(db, key, date + 1) ==
        sum(db.lineitem, |row: LineitemRow| if row.l_orderkey == key && row.l_shipdate == date { row.l_extendedprice * (100 - row.l_discount) } else { 0 }),
{
    lemma_sum_sub(db.lineitem,
        |row: LineitemRow| if row.l_orderkey == key && row.l_shipdate >= date { row.l_extendedprice * (100 - row.l_discount) } else { 0 },
        |row: LineitemRow| if row.l_orderkey == key && row.l_shipdate >= date + 1 { row.l_extendedprice * (100 - row.l_discount) } else { 0 },
        |row: LineitemRow| if row.l_orderkey == key && row.l_shipdate == date { row.l_extendedprice * (100 - row.l_discount) } else { 0 });
}

pub proof fn lemma_line_empty(db: Db, key: i64, date: int)
    requires line_count(db, key as int, i32::MIN as int) == 0,
        forall|position: int| 0 <= position < db.lineitem.len() ==>
            i32::MIN <= (#[trigger] db.lineitem[position]).l_shipdate <= i32::MAX,
    ensures line_count(db, key as int, date) == 0, line_revenue(db, key as int, date) == 0,
{
    hide(sum);
    assert forall|position: int| 0 <= position < db.lineitem.len() implies
        (#[trigger] db.lineitem[position]).l_orderkey != key by {
        if db.lineitem[position].l_orderkey == key {
            lemma_sum_ge_term(db.lineitem,
                |row: LineitemRow| if row.l_orderkey == key && row.l_shipdate >= i32::MIN { 1int } else { 0 }, position);
        }
    }
    lemma_sum_zero(db.lineitem, |row: LineitemRow| if row.l_orderkey == key && row.l_shipdate >= date { 1int } else { 0 });
    lemma_sum_zero(db.lineitem, |row: LineitemRow| if row.l_orderkey == key && row.l_shipdate >= date { row.l_extendedprice * (100 - row.l_discount) } else { 0 });
}

pub proof fn lemma_profile_date_present(db: Db, key: i64, rows: Seq<Bucket>, line_position: int)
    requires profile_ok(db, key, rows), 0 <= line_position < db.lineitem.len(),
        db.lineitem[line_position].l_orderkey == key,
        i32::MIN <= db.lineitem[line_position].l_shipdate <= i32::MAX,
    ensures exists|position: int| 0 <= position < rows.len() &&
        (#[trigger] rows[position]).date == db.lineitem[line_position].l_shipdate,
{
    let date = db.lineitem[line_position].l_shipdate as i32;
    let indicator = |row: LineitemRow| if row.l_orderkey == key && row.l_shipdate == date { 1int } else { 0 };
    lemma_line_point_count(db, key as int, date as int);
    lemma_sum_ge_term(db.lineitem, indicator, line_position);
    assert(line_count(db, key as int, date as int) - line_count(db, key as int, date as int + 1) > 0);
}

pub proof fn lemma_profile_cutoff(db: Db, key: i64, rows: Seq<Bucket>, cutoff: i32, selected: int)
    requires profile_ok(db, key, rows), dates_sorted(rows), 0 <= selected < rows.len(), rows[selected].date > cutoff,
        forall|prior: int| 0 <= prior < selected ==> (#[trigger] rows[prior]).date <= cutoff,
        forall|position: int| 0 <= position < db.lineitem.len() ==>
            i32::MIN <= (#[trigger] db.lineitem[position]).l_shipdate <= i32::MAX,
    ensures forall|position: int| 0 <= position < db.lineitem.len() ==>
        (((#[trigger] db.lineitem[position]).l_orderkey == key && db.lineitem[position].l_shipdate >= cutoff as int + 1) <==>
            (db.lineitem[position].l_orderkey == key && db.lineitem[position].l_shipdate >= rows[selected].date)),
{
    hide(profile_ok);
    hide(sum);
    hide(line_count);
    hide(line_revenue);
    assert forall|position: int| 0 <= position < db.lineitem.len() implies
        (#[trigger] db.lineitem[position]).l_orderkey == key && db.lineitem[position].l_shipdate >= cutoff as int + 1 <==>
            db.lineitem[position].l_orderkey == key && db.lineitem[position].l_shipdate >= rows[selected].date by {
        let line = db.lineitem[position];
        if line.l_orderkey == key && line.l_shipdate > cutoff {
            lemma_profile_date_present(db, key, rows, position);
            let profile_position = choose|profile_position: int| 0 <= profile_position < rows.len() &&
                (#[trigger] rows[profile_position]).date == line.l_shipdate;
            if profile_position < selected { assert(rows[profile_position].date <= cutoff); }
            else if profile_position > selected { assert(rows[selected].date < rows[profile_position].date); }
        }
    }
}

pub proof fn lemma_profile_no_after(db: Db, key: i64, rows: Seq<Bucket>, cutoff: i32)
    requires profile_ok(db, key, rows), after_position_ok(rows, cutoff, None),
        forall|position: int| 0 <= position < db.lineitem.len() ==>
            i32::MIN <= (#[trigger] db.lineitem[position]).l_shipdate <= i32::MAX,
    ensures forall|position: int| 0 <= position < db.lineitem.len() ==>
        (#[trigger] db.lineitem[position]).l_orderkey != key as int || db.lineitem[position].l_shipdate <= cutoff,
{
    hide(profile_ok);
    hide(sum);
    hide(line_count);
    hide(line_revenue);
    assert forall|position: int| 0 <= position < db.lineitem.len() implies
        (#[trigger] db.lineitem[position]).l_orderkey != key as int || db.lineitem[position].l_shipdate <= cutoff by {
        if db.lineitem[position].l_orderkey == key && db.lineitem[position].l_shipdate > cutoff {
            lemma_profile_date_present(db, key, rows, position);
            let profile_position = choose|profile_position: int| 0 <= profile_position < rows.len() &&
                (#[trigger] rows[profile_position]).date == db.lineitem[position].l_shipdate;
            assert(rows[profile_position].date <= cutoff);
        }
    }
}

pub proof fn lemma_profile_after(db: Db, key: i64, rows: Seq<Bucket>, cutoff: i32, result: Option<Bucket>)
    requires profile_ok(db, key, rows), after_position_ok(rows, cutoff, result),
        forall|position: int| 0 <= position < db.lineitem.len() ==>
            i32::MIN <= (#[trigger] db.lineitem[position]).l_shipdate <= i32::MAX,
    ensures
        result matches Some(row) ==> row.count == line_count(db, key as int, cutoff as int + 1)
            && row.revenue == line_revenue(db, key as int, cutoff as int + 1) && row.count > 0,
        result.is_none() ==> line_count(db, key as int, cutoff as int + 1) == 0 && line_revenue(db, key as int, cutoff as int + 1) == 0,
{
    hide(sum);
    match result {
        Some(row) => {
            let selected = choose|position: int| 0 <= position < rows.len() && (#[trigger] rows[position]) == row && row.date > cutoff &&
                forall|prior: int| 0 <= prior < position ==> (#[trigger] rows[prior]).date <= cutoff;
            assert(row.count == line_count(db, key as int, row.date as int));
            assert(row.revenue == line_revenue(db, key as int, row.date as int));
            lemma_profile_cutoff(db, key, rows, cutoff, selected);
            lemma_sum_ext(db.lineitem,
                |line: LineitemRow| if line.l_orderkey == key && line.l_shipdate >= cutoff as int + 1 { 1int } else { 0 },
                |line: LineitemRow| if line.l_orderkey == key && line.l_shipdate >= row.date { 1int } else { 0 });
            lemma_sum_ext(db.lineitem,
                |line: LineitemRow| if line.l_orderkey == key && line.l_shipdate >= cutoff as int + 1 { line.l_extendedprice * (100 - line.l_discount) } else { 0 },
                |line: LineitemRow| if line.l_orderkey == key && line.l_shipdate >= row.date { line.l_extendedprice * (100 - line.l_discount) } else { 0 });
        },
        None => {
            lemma_profile_no_after(db, key, rows, cutoff);
            lemma_sum_zero(db.lineitem, |line: LineitemRow| if line.l_orderkey == key && line.l_shipdate >= cutoff as int + 1 { 1int } else { 0 });
            lemma_sum_zero(db.lineitem, |line: LineitemRow| if line.l_orderkey == key && line.l_shipdate >= cutoff as int + 1 { line.l_extendedprice * (100 - line.l_discount) } else { 0 });
        },
    }
}

pub open spec fn bucket_count(work: Work<Vec<Bucket>>, key: i64, date: i32) -> int {
    if work.data@.contains_key(key) {
        date_count(work.data@[key]@, date)
    } else { 0 }
}

pub open spec fn date_count(rows: Seq<Bucket>, date: i32) -> int {
    sum(rows, |row: Bucket| if row.date == date { row.count as int } else { 0 })
}

pub open spec fn date_revenue(rows: Seq<Bucket>, date: i32) -> int {
    sum(rows, |row: Bucket| if row.date == date { row.revenue as int } else { 0 })
}

pub open spec fn bucket_revenue(work: Work<Vec<Bucket>>, key: i64, date: i32) -> int {
    if work.data@.contains_key(key) {
        date_revenue(work.data@[key]@, date)
    } else { 0 }
}

pub fn numerator(price: i64, discount: i64) -> (result: Option<i128>)
    ensures result matches Some(value) ==> value as int == price as int * (100 - discount as int),
{
    let factor = 100i128 - discount as i128;
    (price as i128).checked_mul(factor)
}

pub fn change(rows: &mut Vec<Bucket>, date: i32, count: i128, revenue: i128) -> (result: Option<()>)
    ensures
        result.is_some() && dates_sorted(old(rows)@) ==> dates_sorted(final(rows)@),
        result.is_some() ==> forall|other: i32| #[trigger] date_count(final(rows)@, other) ==
            date_count(old(rows)@, other) + if other == date { count as int } else { 0 },
        result.is_some() ==> forall|other: i32| #[trigger] date_revenue(final(rows)@, other) ==
            date_revenue(old(rows)@, other) + if other == date { revenue as int } else { 0 },
{
    let ghost original = rows@;
    let mut lower = 0;
    let mut upper = rows.len();
    while lower < upper
        invariant lower <= upper <= rows.len(), rows@ == original, original == old(rows)@,
            dates_sorted(original) ==> forall|position: int| 0 <= position < lower ==> (#[trigger] original[position]).date < date,
            dates_sorted(original) ==> forall|position: int| upper <= position < original.len() ==> (#[trigger] original[position]).date >= date,
        decreases upper - lower,
    {
        let middle = lower + (upper - lower) / 2;
        if rows[middle].date < date {
            proof {
                if dates_sorted(original) {
                    assert forall|position: int| 0 <= position <= middle implies (#[trigger] original[position]).date < date by {}
                }
            }
            lower = middle + 1;
        } else {
            proof {
                if dates_sorted(original) {
                    assert forall|position: int| middle <= position < original.len() implies (#[trigger] original[position]).date >= date by {}
                }
            }
            upper = middle;
        }
    }
    if lower < rows.len() && rows[lower].date == date {
        let old = rows[lower];
        let new_count = match old.count.checked_add(count) { Some(value) => value, None => return None };
        let new_revenue = match old.revenue.checked_add(revenue) { Some(value) => value, None => return None };
        rows.set(lower, Bucket { date, count: new_count, revenue: new_revenue });
        proof {
            let item = rows@[lower as int];
            if dates_sorted(original) {
                assert forall|first: int, second: int| 0 <= first < second < rows.len() implies
                    (#[trigger] rows@[first]).date < (#[trigger] rows@[second]).date by {
                    assert(rows@[first].date == original[first].date);
                    assert(rows@[second].date == original[second].date);
                }
            }
            assert forall|other: i32| #![trigger date_count(rows@, other)] #![trigger date_revenue(rows@, other)]
                #[trigger] date_count(rows@, other) ==
                    sum(original, |row: Bucket| if row.date == other { row.count as int } else { 0 })
                    + if other == date { count as int } else { 0 }
                && #[trigger] date_revenue(rows@, other) ==
                    sum(original, |row: Bucket| if row.date == other { row.revenue as int } else { 0 })
                    + if other == date { revenue as int } else { 0 } by {
                lemma_sum_update(original, lower as int, item,
                    |row: Bucket| if row.date == other { row.count as int } else { 0 });
                lemma_sum_update(original, lower as int, item,
                    |row: Bucket| if row.date == other { row.revenue as int } else { 0 });
            }
        }
    } else {
        rows.insert(lower, Bucket { date, count, revenue });
        proof {
            let item = rows@[lower as int];
            if dates_sorted(original) {
                assert forall|first: int, second: int| 0 <= first < second < rows.len() implies
                    (#[trigger] rows@[first]).date < (#[trigger] rows@[second]).date by {
                    if first < lower && second < lower {
                        assert(rows@[first] == original[first]);
                        assert(rows@[second] == original[second]);
                    } else if first > lower {
                        assert(rows@[first] == original[first - 1]);
                        assert(rows@[second] == original[second - 1]);
                    } else if first == lower {
                        assert(rows@[second] == original[second - 1]);
                        if lower < original.len() { assert(original[lower as int].date > date); }
                    } else {
                        assert(rows@[first] == original[first]);
                        if second > lower {
                            assert(rows@[second] == original[second - 1]);
                            assert(original[first].date < original[second - 1].date);
                        }
                    }
                }
            }
            assert forall|other: i32| #![trigger date_count(rows@, other)] #![trigger date_revenue(rows@, other)]
                #[trigger] date_count(rows@, other) ==
                    sum(original, |row: Bucket| if row.date == other { row.count as int } else { 0 })
                    + if other == date { count as int } else { 0 }
                && #[trigger] date_revenue(rows@, other) ==
                    sum(original, |row: Bucket| if row.date == other { row.revenue as int } else { 0 })
                    + if other == date { revenue as int } else { 0 } by {
                lemma_sum_insert(original, lower as int, item,
                    |row: Bucket| if row.date == other { row.count as int } else { 0 });
                lemma_sum_insert(original, lower as int, item,
                    |row: Bucket| if row.date == other { row.revenue as int } else { 0 });
            }
        }
    }
    Some(())
}

pub fn change_work(work: &mut Work<Vec<Bucket>>, key: i64, date: i32, count: i128, revenue: i128) -> (result: Option<()>)
    requires bucket_work_ok(*old(work)),
    ensures
        result.is_some() ==> bucket_work_ok(*final(work)),
        result.is_some() ==> forall|other_key: i64, other_date: i32|
            #[trigger] bucket_count(*final(work), other_key, other_date) ==
                bucket_count(*old(work), other_key, other_date)
                + if other_key == key && other_date == date { count as int } else { 0 },
        result.is_some() ==> forall|other_key: i64, other_date: i32|
            #[trigger] bucket_revenue(*final(work), other_key, other_date) ==
                bucket_revenue(*old(work), other_key, other_date)
                + if other_key == key && other_date == date { revenue as int } else { 0 },
{
    let ghost original = *work;
    let mut rows = match work.take(key) { Some(value) => value, None => Vec::new() };
    if change(&mut rows, date, count, revenue).is_none() { return None; }
    work.put(key, rows);
    proof {
        assert(work.data@.dom() =~= original.data@.dom().insert(key));
        assert(work.keys@.to_set() =~= original.keys@.to_set().insert(key));
        assert(bucket_work_ok(*work));
    }
    Some(())
}

pub open spec fn bucket_conversion(profile: Seq<Bucket>, rows: Seq<Bucket>) -> bool {
    &&& rows.len() == profile.len()
    &&& forall|position: int| 0 <= position < rows.len() ==>
        (#[trigger] rows[position]).date == profile[position].date
        && rows[position].count == profile[position].count -
            if position + 1 < profile.len() { profile[position + 1].count as int } else { 0 }
        && rows[position].revenue == profile[position].revenue -
            if position + 1 < profile.len() { profile[position + 1].revenue as int } else { 0 }
}

pub proof fn lemma_profile_next(db: Db, key: i64, profile: Seq<Bucket>, selected: int)
    requires profile_ok(db, key, profile), 0 <= selected < profile.len(),
        forall|position: int| 0 <= position < db.lineitem.len() ==>
            i32::MIN <= (#[trigger] db.lineitem[position]).l_shipdate <= i32::MAX,
    ensures line_count(db, key as int, profile[selected].date as int + 1) ==
        if selected + 1 < profile.len() { profile[selected + 1].count as int } else { 0 },
        line_revenue(db, key as int, profile[selected].date as int + 1) ==
        if selected + 1 < profile.len() { profile[selected + 1].revenue as int } else { 0 },
{
    hide(sum);
    hide(profile_ok);
    hide(line_count);
    hide(line_revenue);
    lemma_profile_sorted(db, key, profile);
    let date = profile[selected].date;
    let next = if selected + 1 < profile.len() { Some(profile[selected + 1]) } else { None };
    if selected + 1 < profile.len() {
        assert forall|prior: int| 0 <= prior < selected + 1 implies (#[trigger] profile[prior]).date <= date by {
            if prior < selected { assert(profile[prior].date < profile[selected].date); }
        }
        lemma_after_position(profile, date, selected + 1);
    } else {
        assert forall|prior: int| 0 <= prior < profile.len() implies (#[trigger] profile[prior]).date <= date by {
            if prior < selected { assert(profile[prior].date < profile[selected].date); }
        }
    }
    lemma_profile_after(db, key, profile, date, next);
}

pub proof fn lemma_profile_sorted(db: Db, key: i64, profile: Seq<Bucket>)
    requires profile_ok(db, key, profile),
    ensures dates_sorted(profile),
{
}

pub proof fn lemma_buckets_point(db: Db, key: i64, profile: Seq<Bucket>, rows: Seq<Bucket>, date: i32)
    requires profile_ok(db, key, profile), bucket_conversion(profile, rows),
        forall|position: int| 0 <= position < db.lineitem.len() ==>
            i32::MIN <= (#[trigger] db.lineitem[position]).l_shipdate <= i32::MAX,
    ensures date_count(rows, date) == line_count(db, key as int, date as int) - line_count(db, key as int, date as int + 1),
        date_revenue(rows, date) == line_revenue(db, key as int, date as int) - line_revenue(db, key as int, date as int + 1),
{
    hide(sum);
    if exists|position: int| 0 <= position < profile.len() && (#[trigger] profile[position]).date == date {
        let selected = choose|position: int| 0 <= position < profile.len() && (#[trigger] profile[position]).date == date;
        lemma_profile_next(db, key, profile, selected);
        assert forall|position: int| 0 <= position < rows.len() && position != selected implies
            (#[trigger] rows[position]).date != date by {
            if position < selected { assert(profile[position].date < profile[selected].date); }
            else { assert(profile[selected].date < profile[position].date); }
        }
        lemma_sum_single_match(rows, |row: Bucket| if row.date == date { row.count as int } else { 0 }, selected);
        lemma_sum_single_match(rows, |row: Bucket| if row.date == date { row.revenue as int } else { 0 }, selected);
    } else {
        assert forall|position: int| 0 <= position < db.lineitem.len() implies
            (#[trigger] db.lineitem[position]).l_orderkey != key || db.lineitem[position].l_shipdate != date by {
            if db.lineitem[position].l_orderkey == key && db.lineitem[position].l_shipdate == date {
                lemma_profile_date_present(db, key, profile, position);
            }
        }
        lemma_line_point_count(db, key as int, date as int);
        lemma_line_point_revenue(db, key as int, date as int);
        lemma_sum_zero(db.lineitem, |row: LineitemRow| if row.l_orderkey == key && row.l_shipdate == date { 1int } else { 0 });
        lemma_sum_zero(db.lineitem, |row: LineitemRow| if row.l_orderkey == key && row.l_shipdate == date { row.l_extendedprice * (100 - row.l_discount) } else { 0 });
        assert forall|position: int| 0 <= position < rows.len() implies (#[trigger] rows[position]).date != date by {
            assert(profile[position].date != date);
        }
        lemma_sum_zero(rows, |row: Bucket| if row.date == date { row.count as int } else { 0 });
        lemma_sum_zero(rows, |row: Bucket| if row.date == date { row.revenue as int } else { 0 });
    }
}

pub fn buckets(profile: &Vec<Bucket>) -> (result: Option<Vec<Bucket>>)
    ensures result matches Some(rows) ==> bucket_conversion(profile@, rows@),
        result matches Some(rows) ==> dates_sorted(profile@) ==> dates_sorted(rows@),
{
    let mut rows: Vec<Bucket> = Vec::new();
    let mut position = 0;
    while position < profile.len()
        invariant position <= profile.len(), rows.len() == position,
            forall|prior: int| 0 <= prior < rows.len() ==>
                (#[trigger] rows@[prior]).date == profile@[prior].date
                && rows@[prior].count == profile@[prior].count -
                    if prior + 1 < profile.len() { profile@[prior + 1].count as int } else { 0 }
                && rows@[prior].revenue == profile@[prior].revenue -
                    if prior + 1 < profile.len() { profile@[prior + 1].revenue as int } else { 0 },
        decreases profile.len() - position,
    {
        let old = profile[position];
        let (count, revenue) = if position + 1 < profile.len() {
            let next = profile[position + 1];
            let count = match old.count.checked_sub(next.count) { Some(value) => value, None => return None };
            let revenue = match old.revenue.checked_sub(next.revenue) { Some(value) => value, None => return None };
            (count, revenue)
        } else { (old.count, old.revenue) };
        rows.push(Bucket { date: old.date, count, revenue });
        position += 1;
    }
    proof {
        if dates_sorted(profile@) {
            assert forall|first: int, second: int| 0 <= first < second < rows.len() implies
                (#[trigger] rows@[first]).date < (#[trigger] rows@[second]).date by {}
        }
    }
    Some(rows)
}

pub fn suffix(rows: Vec<Bucket>) -> (result: Option<Vec<Bucket>>)
    requires dates_sorted(rows@),
    ensures result matches Some(profile) ==> suffix_conversion(rows@, profile@),
{
    hide(sum);
    let mut live: Vec<Bucket> = Vec::new();
    let mut position = 0;
    proof { assert(rows@.take(0) =~= live@); }
    while position < rows.len()
        invariant position <= rows.len(), dates_sorted(rows@), dates_sorted(live@),
            forall|entry: int| 0 <= entry < live.len() ==> (#[trigger] live@[entry]).count > 0
                && rows@.take(position as int).contains(live@[entry]),
            forall|cutoff: int| #[trigger] suffix_count(live@, cutoff) == suffix_count(rows@.take(position as int), cutoff),
            forall|cutoff: int| #[trigger] suffix_revenue(live@, cutoff) == suffix_revenue(rows@.take(position as int), cutoff),
            forall|date: i32| #[trigger] date_count(live@, date) == date_count(rows@.take(position as int), date),
        decreases rows.len() - position,
    {
        let row = rows[position];
        let ghost previous = live@;
        if row.count < 0 || (row.count == 0 && row.revenue != 0) { return None; }
        if row.count > 0 {
            proof {
                assert forall|entry: int| 0 <= entry < live.len() implies (#[trigger] live@[entry]).date < row.date by {
                    let prior = choose|prior: int| 0 <= prior < position && (#[trigger] rows@[prior]) == live@[entry];
                    assert(rows@[prior].date < rows@[position as int].date);
                }
            }
            live.push(row);
        }
        proof {
            assert(rows@.take(position as int + 1) =~= rows@.take(position as int).push(row));
            assert forall|cutoff: int| #![trigger suffix_count(live@, cutoff)] #![trigger suffix_revenue(live@, cutoff)]
                suffix_count(live@, cutoff) == suffix_count(rows@.take(position as int + 1), cutoff)
                && suffix_revenue(live@, cutoff) == suffix_revenue(rows@.take(position as int + 1), cutoff) by {
                assert(suffix_count(previous, cutoff) == suffix_count(rows@.take(position as int), cutoff));
                assert(suffix_revenue(previous, cutoff) == suffix_revenue(rows@.take(position as int), cutoff));
                lemma_sum_take(rows@, position as int, |entry: Bucket| if entry.date >= cutoff { entry.count as int } else { 0 });
                lemma_sum_take(rows@, position as int, |entry: Bucket| if entry.date >= cutoff { entry.revenue as int } else { 0 });
                if row.count > 0 {
                    lemma_sum_push(previous, row, |entry: Bucket| if entry.date >= cutoff { entry.count as int } else { 0 });
                    lemma_sum_push(previous, row, |entry: Bucket| if entry.date >= cutoff { entry.revenue as int } else { 0 });
                }
            }
            assert forall|date: i32| #[trigger] date_count(live@, date) == date_count(rows@.take(position as int + 1), date) by {
                assert(date_count(previous, date) == date_count(rows@.take(position as int), date));
                lemma_sum_take(rows@, position as int, |entry: Bucket| if entry.date == date { entry.count as int } else { 0 });
                if row.count > 0 { lemma_sum_push(previous, row, |entry: Bucket| if entry.date == date { entry.count as int } else { 0 }); }
            }
            assert forall|entry: int| 0 <= entry < live.len() implies
                (#[trigger] live@[entry]).count > 0 && rows@.take(position as int + 1).contains(live@[entry]) by {
                if entry < previous.len() {
                    let prior = choose|prior: int| 0 <= prior < position && (#[trigger] rows@[prior]) == previous[entry];
                    assert(rows@.take(position as int + 1)[prior] == live@[entry]);
                } else { assert(rows@.take(position as int + 1)[position as int] == live@[entry]); }
            }
        }
        position += 1;
    }
    proof { assert(rows@.take(position as int) =~= rows@); }
    let ghost filtered = live@;
    let mut remaining = live.len();
    let mut count = 0i128;
    let mut revenue = 0i128;
    proof {
        lemma_sum_empty(filtered.subrange(remaining as int, filtered.len() as int), |entry: Bucket| entry.count as int);
        lemma_sum_empty(filtered.subrange(remaining as int, filtered.len() as int), |entry: Bucket| entry.revenue as int);
    }
    while remaining > 0
        invariant remaining <= live.len(), live.len() == filtered.len(), dates_sorted(filtered),
            forall|entry: int| 0 <= entry < filtered.len() ==> (#[trigger] filtered[entry]).count > 0,
            forall|entry: int| 0 <= entry < live.len() ==> (#[trigger] live@[entry]).date == filtered[entry].date,
            forall|entry: int| 0 <= entry < remaining ==> (#[trigger] live@[entry]) == filtered[entry],
            forall|entry: int| remaining <= entry < live.len() ==>
                (#[trigger] live@[entry]).count == suffix_count(rows@, live@[entry].date as int)
                && live@[entry].revenue == suffix_revenue(rows@, live@[entry].date as int) && live@[entry].count > 0,
            count >= 0,
            count as int == sum(filtered.subrange(remaining as int, filtered.len() as int), |entry: Bucket| entry.count as int),
            revenue as int == sum(filtered.subrange(remaining as int, filtered.len() as int), |entry: Bucket| entry.revenue as int),
            forall|cutoff: int| #[trigger] suffix_count(filtered, cutoff) == suffix_count(rows@, cutoff),
            forall|cutoff: int| #[trigger] suffix_revenue(filtered, cutoff) == suffix_revenue(rows@, cutoff),
            forall|date: i32| #[trigger] date_count(filtered, date) == date_count(rows@, date),
        decreases remaining,
    {
        remaining -= 1;
        let row = live[remaining];
        proof {
            let tail = filtered.subrange(remaining as int, filtered.len() as int);
            assert(tail[0] == row);
            assert(tail.drop_first() =~= filtered.subrange(remaining as int + 1, filtered.len() as int));
            lemma_sum_drop_first(tail, |entry: Bucket| entry.count as int);
            lemma_sum_drop_first(tail, |entry: Bucket| entry.revenue as int);
            lemma_suffix_position(filtered, remaining as int);
        }
        count = match count.checked_add(row.count) { Some(value) => value, None => return None };
        revenue = match revenue.checked_add(row.revenue) { Some(value) => value, None => return None };
        proof {
            assert(count as int == suffix_count(filtered, row.date as int));
            assert(revenue as int == suffix_revenue(filtered, row.date as int));
            assert(suffix_count(filtered, row.date as int) == suffix_count(rows@, row.date as int));
            assert(suffix_revenue(filtered, row.date as int) == suffix_revenue(rows@, row.date as int));
        }
        live.set(remaining, Bucket { date: row.date, count, revenue });
    }
    proof {
        assert(dates_sorted(live@));
        assert forall|date: i32| #[trigger] date_count(rows@, date) > 0 <==>
            exists|entry: int| 0 <= entry < live.len() && (#[trigger] live@[entry]).date == date by {
            lemma_bucket_support(filtered, date);
            if date_count(rows@, date) > 0 {
                assert(date_count(filtered, date) == date_count(rows@, date));
                let entry = choose|entry: int| 0 <= entry < filtered.len() && (#[trigger] filtered[entry]).date == date;
                assert(live@[entry].date == date);
            }
            if exists|entry: int| 0 <= entry < live.len() && (#[trigger] live@[entry]).date == date {
                let entry = choose|entry: int| 0 <= entry < live.len() && (#[trigger] live@[entry]).date == date;
                assert(filtered[entry].date == date);
                assert(date_count(filtered, date) == date_count(rows@, date));
            }
        }
    }
    Some(live)
}

pub proof fn lemma_suffix_position(rows: Seq<Bucket>, position: int)
    requires dates_sorted(rows), 0 <= position < rows.len(),
    ensures suffix_count(rows, rows[position].date as int) ==
        sum(rows.subrange(position, rows.len() as int), |entry: Bucket| entry.count as int),
        suffix_revenue(rows, rows[position].date as int) ==
        sum(rows.subrange(position, rows.len() as int), |entry: Bucket| entry.revenue as int),
{
    let count_weight = |entry: Bucket| if entry.date >= rows[position].date { entry.count as int } else { 0 };
    let revenue_weight = |entry: Bucket| if entry.date >= rows[position].date { entry.revenue as int } else { 0 };
    let prefix = rows.take(position);
    let tail = rows.subrange(position, rows.len() as int);
    assert(rows =~= prefix + tail);
    assert forall|entry: int| 0 <= entry < prefix.len() implies (#[trigger] prefix[entry]).date < rows[position].date by {
        assert(prefix[entry] == rows[entry]);
    }
    assert forall|entry: int| 0 <= entry < tail.len() implies (#[trigger] tail[entry]).date >= rows[position].date by {
        assert(tail[entry] == rows[position + entry]);
    }
    lemma_sum_zero(prefix, count_weight);
    lemma_sum_zero(prefix, revenue_weight);
    lemma_sum_concat(prefix, tail, count_weight);
    lemma_sum_concat(prefix, tail, revenue_weight);
    lemma_sum_ext(tail, count_weight, |entry: Bucket| entry.count as int);
    lemma_sum_ext(tail, revenue_weight, |entry: Bucket| entry.revenue as int);
}

pub open spec fn buckets_db(db: Db, key: i64, rows: Seq<Bucket>) -> bool {
    &&& dates_sorted(rows)
    &&& forall|date: i32| #[trigger] date_count(rows, date) ==
        line_count(db, key as int, date as int) - line_count(db, key as int, date as int + 1)
    &&& forall|date: i32| #[trigger] date_revenue(rows, date) ==
        line_revenue(db, key as int, date as int) - line_revenue(db, key as int, date as int + 1)
}

pub proof fn lemma_bucket_single(rows: Seq<Bucket>, position: int)
    requires dates_sorted(rows), 0 <= position < rows.len(),
    ensures date_count(rows, rows[position].date) == rows[position].count,
        date_revenue(rows, rows[position].date) == rows[position].revenue,
{
    assert forall|other: int| 0 <= other < rows.len() && other != position implies
        (#[trigger] rows[other]).date != rows[position].date by {
        if other < position { assert(rows[other].date < rows[position].date); }
        else { assert(rows[position].date < rows[other].date); }
    }
    lemma_sum_single_match(rows, |entry: Bucket| if entry.date == rows[position].date { entry.count as int } else { 0 }, position);
    lemma_sum_single_match(rows, |entry: Bucket| if entry.date == rows[position].date { entry.revenue as int } else { 0 }, position);
}

pub proof fn lemma_bucket_db_sum(db: Db, key: i64, rows: Seq<Bucket>, cutoff: int, value: spec_fn(LineitemRow) -> int, bucket_value: spec_fn(Bucket) -> int)
    requires dates_sorted(rows),
        forall|position: int| 0 <= position < db.lineitem.len() ==>
            i32::MIN <= (#[trigger] db.lineitem[position]).l_shipdate <= i32::MAX,
        forall|date: i32| #[trigger] date_count(rows, date) ==
            sum(db.lineitem, |line: LineitemRow| if line.l_orderkey == key && line.l_shipdate == date { 1int } else { 0 }),
        forall|position: int| 0 <= position < rows.len() ==> bucket_value(#[trigger] rows[position]) ==
            sum(db.lineitem, |line: LineitemRow| if line.l_orderkey == key && line.l_shipdate == rows[position].date { value(line) } else { 0 }),
    ensures sum(rows, |entry: Bucket| if entry.date >= cutoff { bucket_value(entry) } else { 0 }) ==
        sum(db.lineitem, |line: LineitemRow| if line.l_orderkey == key && line.l_shipdate >= cutoff { value(line) } else { 0 }),
{
    hide(sum);
    let joined = |entry: Bucket, line: LineitemRow| if entry.date >= cutoff && line.l_orderkey == key && line.l_shipdate == entry.date { value(line) } else { 0 };
    let row_total = |entry: Bucket| sum(db.lineitem, |line: LineitemRow| joined(entry, line));
    let line_total = |line: LineitemRow| sum(rows, |entry: Bucket| joined(entry, line));
    let expected_rows = |entry: Bucket| if entry.date >= cutoff { bucket_value(entry) } else { 0 };
    let expected_lines = |line: LineitemRow| if line.l_orderkey == key && line.l_shipdate >= cutoff { value(line) } else { 0 };
    assert forall|position: int| 0 <= position < rows.len() implies
        row_total(#[trigger] rows[position]) == expected_rows(rows[position]) by {
        let entry = rows[position];
        if entry.date >= cutoff {
            lemma_sum_ext_all(db.lineitem, |line: LineitemRow| joined(entry, line),
                |line: LineitemRow| if line.l_orderkey == key && line.l_shipdate == entry.date { value(line) } else { 0 });
        } else { lemma_sum_zero(db.lineitem, |line: LineitemRow| joined(entry, line)); }
    }
    assert forall|position: int| 0 <= position < db.lineitem.len() implies
        line_total(#[trigger] db.lineitem[position]) == expected_lines(db.lineitem[position]) by {
        let line = db.lineitem[position];
        if line.l_orderkey == key && line.l_shipdate >= cutoff {
            let date = line.l_shipdate as i32;
            let indicator = |candidate: LineitemRow| if candidate.l_orderkey == key && candidate.l_shipdate == date { 1int } else { 0 };
            lemma_sum_ge_term(db.lineitem, indicator, position);
            assert(date_count(rows, date) > 0);
            let selected = lemma_sum_nonzero_witness(rows, |entry: Bucket| if entry.date == date { entry.count as int } else { 0 });
            assert(rows[selected].date == date);
            assert forall|other: int| 0 <= other < rows.len() && other != selected implies joined(#[trigger] rows[other], line) == 0 by {
                if other < selected { assert(rows[other].date < rows[selected].date); }
                else { assert(rows[selected].date < rows[other].date); }
            }
            lemma_sum_single_match(rows, |entry: Bucket| joined(entry, line), selected);
        } else { lemma_sum_zero(rows, |entry: Bucket| joined(entry, line)); }
    }
    lemma_sum_swap(rows, db.lineitem, joined, row_total, line_total);
    lemma_sum_ext(rows, row_total, expected_rows);
    lemma_sum_ext(db.lineitem, line_total, expected_lines);
}

pub proof fn lemma_buckets_db_suffix(db: Db, key: i64, rows: Seq<Bucket>, cutoff: int)
    requires buckets_db(db, key, rows),
        forall|position: int| 0 <= position < db.lineitem.len() ==>
            i32::MIN <= (#[trigger] db.lineitem[position]).l_shipdate <= i32::MAX,
    ensures suffix_count(rows, cutoff) == line_count(db, key as int, cutoff),
        suffix_revenue(rows, cutoff) == line_revenue(db, key as int, cutoff),
{
    hide(sum);
    assert forall|date: i32| #[trigger] date_count(rows, date) ==
        sum(db.lineitem, |line: LineitemRow| if line.l_orderkey == key && line.l_shipdate == date { 1int } else { 0 }) by {
        lemma_line_point_count(db, key as int, date as int);
    }
    assert forall|position: int| 0 <= position < rows.len() implies
        (#[trigger] rows[position]).count == sum(db.lineitem, |line: LineitemRow|
            if line.l_orderkey == key && line.l_shipdate == rows[position].date { 1int } else { 0 })
        && rows[position].revenue == sum(db.lineitem, |line: LineitemRow|
            if line.l_orderkey == key && line.l_shipdate == rows[position].date { line.l_extendedprice * (100 - line.l_discount) } else { 0 }) by {
        lemma_bucket_single(rows, position);
        lemma_line_point_count(db, key as int, rows[position].date as int);
        lemma_line_point_revenue(db, key as int, rows[position].date as int);
    }
    let count_value = |line: LineitemRow| 1int;
    let revenue_value = |line: LineitemRow| line.l_extendedprice * (100 - line.l_discount);
    let bucket_count_value = |entry: Bucket| entry.count as int;
    let bucket_revenue_value = |entry: Bucket| entry.revenue as int;
    assert forall|position: int| 0 <= position < rows.len() implies bucket_count_value(#[trigger] rows[position]) ==
        sum(db.lineitem, |line: LineitemRow| if line.l_orderkey == key && line.l_shipdate == rows[position].date { count_value(line) } else { 0 }) by {
        lemma_sum_ext_all(db.lineitem,
            |line: LineitemRow| if line.l_orderkey == key && line.l_shipdate == rows[position].date { count_value(line) } else { 0 },
            |line: LineitemRow| if line.l_orderkey == key && line.l_shipdate == rows[position].date { 1int } else { 0 });
    }
    assert forall|position: int| 0 <= position < rows.len() implies bucket_revenue_value(#[trigger] rows[position]) ==
        sum(db.lineitem, |line: LineitemRow| if line.l_orderkey == key && line.l_shipdate == rows[position].date { revenue_value(line) } else { 0 }) by {
        lemma_sum_ext_all(db.lineitem,
            |line: LineitemRow| if line.l_orderkey == key && line.l_shipdate == rows[position].date { revenue_value(line) } else { 0 },
            |line: LineitemRow| if line.l_orderkey == key && line.l_shipdate == rows[position].date { line.l_extendedprice * (100 - line.l_discount) } else { 0 });
    }
    lemma_bucket_db_sum(db, key, rows, cutoff, count_value, bucket_count_value);
    lemma_bucket_db_sum(db, key, rows, cutoff, revenue_value, bucket_revenue_value);
    lemma_sum_ext_all(rows, |entry: Bucket| if entry.date >= cutoff { bucket_count_value(entry) } else { 0 },
        |entry: Bucket| if entry.date >= cutoff { entry.count as int } else { 0 });
    lemma_sum_ext_all(rows, |entry: Bucket| if entry.date >= cutoff { bucket_revenue_value(entry) } else { 0 },
        |entry: Bucket| if entry.date >= cutoff { entry.revenue as int } else { 0 });
    lemma_sum_ext_all(db.lineitem, |line: LineitemRow| if line.l_orderkey == key && line.l_shipdate >= cutoff { count_value(line) } else { 0 },
        |line: LineitemRow| if line.l_orderkey == key && line.l_shipdate >= cutoff { 1int } else { 0 });
    lemma_sum_ext_all(db.lineitem, |line: LineitemRow| if line.l_orderkey == key && line.l_shipdate >= cutoff { revenue_value(line) } else { 0 },
        |line: LineitemRow| if line.l_orderkey == key && line.l_shipdate >= cutoff { line.l_extendedprice * (100 - line.l_discount) } else { 0 });
}

pub proof fn lemma_suffix_db(db: Db, key: i64, rows: Seq<Bucket>, profile: Seq<Bucket>)
    requires buckets_db(db, key, rows), suffix_conversion(rows, profile),
        forall|position: int| 0 <= position < db.lineitem.len() ==>
            i32::MIN <= (#[trigger] db.lineitem[position]).l_shipdate <= i32::MAX,
    ensures profile_ok(db, key, profile),
        profile.len() > 0 <==> line_count(db, key as int, i32::MIN as int) > 0,
{
    hide(sum);
    assert forall|date: i32| #[trigger] line_count(db, key as int, date as int) - line_count(db, key as int, date as int + 1) > 0 <==>
        exists|position: int| 0 <= position < profile.len() && (#[trigger] profile[position]).date == date by {
        assert(date_count(rows, date) == line_count(db, key as int, date as int) - line_count(db, key as int, date as int + 1));
    }
    assert forall|position: int| 0 <= position < profile.len() implies
        (#[trigger] profile[position]).count == line_count(db, key as int, profile[position].date as int)
        && profile[position].revenue == line_revenue(db, key as int, profile[position].date as int) && profile[position].count > 0 by {
        lemma_buckets_db_suffix(db, key, rows, profile[position].date as int);
    }
    if profile.len() > 0 {
        let date = profile[0].date as int;
        let indicator = |line: LineitemRow| if line.l_orderkey == key && line.l_shipdate >= date { 1int } else { 0 };
        let selected = lemma_sum_nonzero_witness(db.lineitem, indicator);
        lemma_sum_ge_term(db.lineitem, |line: LineitemRow| if line.l_orderkey == key && line.l_shipdate >= i32::MIN { 1int } else { 0 }, selected);
    }
    if line_count(db, key as int, i32::MIN as int) > 0 {
        let selected = lemma_sum_nonzero_witness(db.lineitem,
            |line: LineitemRow| if line.l_orderkey == key && line.l_shipdate >= i32::MIN { 1int } else { 0 });
        let date = db.lineitem[selected].l_shipdate as i32;
        lemma_line_point_count(db, key as int, date as int);
        lemma_sum_ge_term(db.lineitem, |line: LineitemRow| if line.l_orderkey == key && line.l_shipdate == date { 1int } else { 0 }, selected);
        assert(date_count(rows, date) > 0);
        let position = choose|position: int| 0 <= position < profile.len() && (#[trigger] profile[position]).date == date;
    }
}

pub fn after(profile: &Vec<Bucket>, cutoff: i32) -> (result: Option<Bucket>)
    ensures dates_sorted(profile@) ==> after_position_ok(profile@, cutoff, result),
{
    let mut lower = 0;
    let mut upper = profile.len();
    while lower < upper
        invariant lower <= upper <= profile.len(),
            dates_sorted(profile@) ==> forall|position: int| 0 <= position < lower ==> (#[trigger] profile@[position]).date <= cutoff,
            dates_sorted(profile@) ==> forall|position: int| upper <= position < profile.len() ==> (#[trigger] profile@[position]).date > cutoff,
        decreases upper - lower,
    {
        let middle = lower + (upper - lower) / 2;
        if profile[middle].date <= cutoff {
            proof {
                if dates_sorted(profile@) {
                    assert forall|position: int| 0 <= position <= middle implies (#[trigger] profile@[position]).date <= cutoff by {}
                }
            }
            lower = middle + 1;
        } else {
            proof {
                if dates_sorted(profile@) {
                    assert forall|position: int| middle <= position < profile.len() implies (#[trigger] profile@[position]).date > cutoff by {}
                }
            }
            upper = middle;
        }
    }
    if lower < profile.len() {
        proof {
            if dates_sorted(profile@) {
                assert(profile@[lower as int].date > cutoff);
                lemma_after_position(profile@, cutoff, lower as int);
            }
        }
        Some(profile[lower])
    } else { None }
}

}
