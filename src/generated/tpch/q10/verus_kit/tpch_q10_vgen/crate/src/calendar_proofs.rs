use vstd::prelude::*;
use vstd::arithmetic::div_mod::*;
use crate::calendar::*;

verus! {

pub open spec fn era_year(within: int) -> int {
    (within - within / 1460 + within / 36524 - within / 146096) / 365
}

#[verifier::rlimit(30)]
pub proof fn lemma_era_year_bounds(within: int)
    requires 0 <= within <= 146096,
    ensures 0 <= era_year(within) <= 399,
        0 <= within - (365 * era_year(within) + era_year(within) / 4 - era_year(within) / 100) <= 365,
{
    let year = era_year(within);
    assert(0 <= year <= 399);
    assert(0 <= within - (365 * year + year / 4 - year / 100) <= 365);
}

pub proof fn lemma_era_year_order(left: int, right: int)
    requires 0 <= left <= right <= 146096,
    ensures era_year(left) <= era_year(right),
{
    if left < right {
        let bound = if right == 146096 { right - 1 } else { right };
        assert(left <= bound);
        assert(bound / 1460 - left / 1460 <= bound - left);
        assert(left / 36524 <= bound / 36524);
        assert(left / 146096 == 0 && bound / 146096 == 0);
        assert(left - left / 1460 + left / 36524 <= bound - bound / 1460 + bound / 36524);
        if right == 146096 { assert(era_year(bound) == era_year(right)); }
    }
}

pub proof fn lemma_month_era(day: int)
    ensures {
        let era = (day + 719468) / 146097;
        let within = (day + 719468) % 146097;
        let year = era_year(within);
        let year_day = within - (365 * year + year / 4 - year / 100);
        &&& 0 <= within <= 146096
        &&& month_of(day) == era * 4800 + year * 12 + (5 * year_day + 2) / 153 + 2
        &&& era * 4800 + 2 <= month_of(day) <= era * 4800 + 4801
    },
{
    lemma_fundamental_div_mod(day + 719468, 146097);
    let within = (day + 719468) % 146097;
    lemma_era_year_bounds(within);
}

#[verifier::rlimit(20)]
pub proof fn lemma_month_order(left: int, right: int)
    requires left <= right,
    ensures month_of(left) <= month_of(right),
{
    lemma_month_era(left);
    lemma_month_era(right);
    let left_era = (left + 719468) / 146097;
    let right_era = (right + 719468) / 146097;
    assert(left_era <= right_era);
    if left_era == right_era {
        let left_within = (left + 719468) % 146097;
        let right_within = (right + 719468) % 146097;
        assert(left_within <= right_within);
        lemma_era_year_order(left_within, right_within);
        lemma_era_year_bounds(left_within);
        lemma_era_year_bounds(right_within);
        if era_year(left_within) == era_year(right_within) {
            assert(left_within - (365 * era_year(left_within) + era_year(left_within) / 4 - era_year(left_within) / 100)
                <= right_within - (365 * era_year(right_within) + era_year(right_within) / 4 - era_year(right_within) / 100));
        }
    }
}

pub proof fn lemma_period_bounds(day: int, grain: int)
    requires i32::MIN <= day <= i32::MAX, 0 <= grain <= 5,
    ensures period_of(i32::MIN as int, grain) <= period_of(day, grain) <= period_of(i32::MAX as int, grain),
{
    lemma_month_order(i32::MIN as int, day);
    lemma_month_order(day, i32::MAX as int);
}

pub proof fn lemma_calendar_div(value: int, divisor: int, quotient: int)
    requires divisor > 0, quotient * divisor <= value < (quotient + 1) * divisor,
    ensures value / divisor == quotient,
{
    lemma_fundamental_div_mod(value, divisor);
    assert(value == (value / divisor) * divisor + value % divisor) by (nonlinear_arith)
        requires value == divisor * (value / divisor) + value % divisor;
    assert(value / divisor == quotient) by (nonlinear_arith)
        requires value == (value / divisor) * divisor + value % divisor,
            0 <= value % divisor < divisor,
            quotient * divisor <= value < (quotient + 1) * divisor;
}

pub proof fn lemma_month_start_march(era: int, year: int, march_month: int)
    requires 0 <= year < 400, 0 <= march_month <= 12,
    ensures month_start(era * 4800 + year * 12 + march_month + 2)
        == era * 146097 + year * 365 + year / 4 - year / 100 + (153 * march_month + 2) / 5 - 719468
            || march_month == 12,
{
    hide(month_start);
    if march_month < 12 {
        let month = era * 4800 + year * 12 + march_month + 2;
        let civil_year = era * 400 + year + if march_month >= 10 { 1int } else { 0int };
        lemma_calendar_div(month, 12, civil_year);
        let civil_month = march_month + 2 - if march_month >= 10 { 12int } else { 0int };
        assert(month - civil_year * 12 == civil_month);
        assert(0 <= civil_month < 12);
        assert((civil_month < 2) == (march_month >= 10));
        assert(civil_year - if civil_month < 2 { 1int } else { 0int } == era * 400 + year);
        lemma_calendar_div(era * 400 + year, 400, era);
        reveal(month_start);
    }
}

#[verifier::rlimit(40)]
pub proof fn lemma_month_contains(day: int)
    ensures month_start(month_of(day)) <= day < month_start(month_of(day) + 1),
{
    hide(month_start);
    lemma_month_era(day);
    let era = (day + 719468) / 146097;
    let within = (day + 719468) % 146097;
    let year = era_year(within);
    let year_day = within - (365 * year + year / 4 - year / 100);
    let march_month = (5 * year_day + 2) / 153;
    lemma_era_year_bounds(within);
    assert(0 <= march_month <= 11);
    lemma_month_start_march(era, year, march_month);
    assert((153 * march_month + 2) / 5 <= year_day);
    if march_month < 11 {
        lemma_month_start_march(era, year, march_month + 1);
        assert(year_day < (153 * (march_month + 1) + 2) / 5);
    } else if year < 399 {
        lemma_month_start_march(era, year + 1, 0);
        assert(within < 365 * (year + 1) + (year + 1) / 4 - (year + 1) / 100);
    } else {
        lemma_month_start_march(era + 1, 0, 0);
        assert(within < 146097);
    }
}

pub proof fn lemma_month_coordinates(month: int)
    ensures {
        let shifted = month - 2;
        let era = shifted / 4800;
        let year = (shifted % 4800) / 12;
        let within = shifted % 12;
        &&& 0 <= year < 400
        &&& 0 <= within < 12
        &&& month == era * 4800 + year * 12 + within + 2
        &&& month_start(month) == era * 146097 + year * 365 + year / 4 - year / 100
            + (153 * within + 2) / 5 - 719468
    },
{
    hide(month_start);
    let shifted = month - 2;
    lemma_fundamental_div_mod(shifted, 4800);
    lemma_fundamental_div_mod(shifted % 4800, 12);
    lemma_fundamental_div_mod(shifted, 12);
    lemma_calendar_div(shifted, 12, (shifted / 4800) * 400 + (shifted % 4800) / 12);
    assert(shifted % 12 == (shifted % 4800) % 12);
    lemma_month_start_march(shifted / 4800, (shifted % 4800) / 12, shifted % 12);
}

#[verifier::rlimit(30)]
pub proof fn lemma_month_start_inverse(month: int)
    ensures month_of(month_start(month)) == month,
{
    hide(month_start);
    hide(month_of);
    lemma_month_coordinates(month);
    let shifted = month - 2;
    let era = shifted / 4800;
    let year = (shifted % 4800) / 12;
    let within = shifted % 12;
    let year_day = (153 * within + 2) / 5;
    let era_day = year * 365 + year / 4 - year / 100 + year_day;
    assert(0 <= era_day < 146097);
    assert(era_year(era_day) == year);
    assert((5 * year_day + 2) / 153 == within);
    lemma_calendar_div(month_start(month) + 719468, 146097, era);
    lemma_month_era(month_start(month));
}

pub proof fn lemma_month_window(day: int, lower: int, upper: int)
    requires lower <= upper,
    ensures (lower <= month_of(day) < upper) == (month_start(lower) <= day < month_start(upper)),
{
    hide(month_start);
    hide(month_of);
    lemma_month_start_inverse(lower);
    lemma_month_start_inverse(upper);
    lemma_month_contains(day);
    if day < month_start(lower) {
        lemma_month_order(day, month_start(lower));
        if month_of(day) == lower { assert(false); }
    } else {
        lemma_month_order(month_start(lower), day);
    }
    if day < month_start(upper) {
        lemma_month_order(day, month_start(upper));
        if month_of(day) == upper { assert(false); }
    } else {
        lemma_month_order(month_start(upper), day);
    }
}

pub proof fn lemma_month_start_order(lower: int, upper: int)
    requires lower <= upper,
    ensures month_start(lower) <= month_start(upper),
        lower < upper ==> month_start(lower) < month_start(upper),
{
    hide(month_start);
    hide(month_of);
    if lower < upper {
        lemma_month_start_inverse(lower);
        lemma_month_window(month_start(lower), lower, upper);
    }
}

pub proof fn lemma_day_cover_bounds(lower: int, upper: int)
    requires lower < upper,
        (if month_start(month_of(lower)) == lower { month_of(lower) } else { month_of(lower) + 1 }) < month_of(upper),
    ensures {
        let first = if month_start(month_of(lower)) == lower { month_of(lower) } else { month_of(lower) + 1 };
        lower <= month_start(first) <= month_start(month_of(upper)) <= upper
    },
{
    hide(month_start);
    hide(month_of);
    lemma_month_contains(lower);
    lemma_month_contains(upper);
    let first = if month_start(month_of(lower)) == lower { month_of(lower) } else { month_of(lower) + 1 };
    lemma_month_start_order(first, month_of(upper));
}

}
