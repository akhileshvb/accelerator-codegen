use vstd::prelude::*;
use vstd::arithmetic::div_mod::*;

verus! {

pub fn floor_div(value: i64, divisor: i64) -> (result: i64)
    requires divisor > 0,
    ensures result as int == value as int / divisor as int,
{
    if value >= 0 { return value / divisor; }
    let magnitude = -(value as i128);
    let rounded = (magnitude + divisor as i128 - 1) / divisor as i128;
    proof {
        let numerator = magnitude as int + divisor as int - 1;
        let denominator = divisor as int;
        let quotient = rounded as int;
        assert(quotient == numerator / denominator);
        lemma_fundamental_div_mod(numerator, denominator);
        assert(0 <= numerator % denominator < denominator);
        assert(quotient * denominator <= numerator < (quotient + 1) * denominator) by (nonlinear_arith)
            requires numerator == (numerator / denominator) * denominator + numerator % denominator,
                0 <= numerator % denominator < denominator, quotient == numerator / denominator, denominator > 0;
        assert((-quotient) * denominator <= (value as int) < (-quotient + 1) * denominator) by (nonlinear_arith)
            requires numerator == -(value as int) + denominator - 1,
                quotient * denominator <= numerator < (quotient + 1) * denominator, denominator > 0;
        lemma_fundamental_div_mod(value as int, denominator);
        assert(0 <= (value as int) % denominator < denominator);
        assert(value as int / denominator == -quotient) by (nonlinear_arith)
            requires denominator > 0,
                (-quotient) * denominator <= (value as int) < (-quotient + 1) * denominator,
                value as int == (value as int / denominator) * denominator + (value as int) % denominator,
                0 <= (value as int) % denominator < denominator;
        assert(1 <= quotient <= (magnitude as int)) by (nonlinear_arith)
            requires quotient * denominator <= numerator < (quotient + 1) * denominator,
                numerator == magnitude as int + denominator - 1, magnitude >= 1, denominator >= 1;
    }
    (-rounded) as i64
}

pub open spec fn month_of(day: int) -> int {
    let shifted = day + 719468;
    let era = shifted / 146097;
    let within = shifted - era * 146097;
    let year_within = (within - within / 1460 + within / 36524 - within / 146096) / 365;
    let year = year_within + era * 400;
    let year_day = within - (365 * year_within + year_within / 4 - year_within / 100);
    let march_month = (5 * year_day + 2) / 153;
    let month = if march_month < 10 { march_month + 2 } else { march_month - 10 };
    (year + if month < 2 { 1int } else { 0int }) * 12 + month
}

pub open spec fn month_start(month: int) -> int {
    let year = month / 12;
    let within = month - year * 12;
    let march_year = year - if within < 2 { 1int } else { 0int };
    let era = march_year / 400;
    let year_within = march_year - era * 400;
    let march_month = if within >= 2 { within - 2 } else { within + 10 };
    let year_day = (153 * march_month + 2) / 5;
    let era_day = year_within * 365 + year_within / 4 - year_within / 100 + year_day;
    era * 146097 + era_day - 719468
}

#[verifier::rlimit(10)]
pub fn month_key(day: i64) -> (result: i64)
    requires i32::MIN <= day <= i32::MAX,
    ensures result as int == month_of(day as int),
        i32::MIN <= result <= i32::MAX,
{
    let shifted = day + 719468;
    let era = floor_div(shifted, 146097);
    proof { assert(-15000 <= era <= 15000); }
    let within = shifted - era * 146097;
    proof {
        lemma_fundamental_div_mod(shifted as int, 146097);
        assert(0 <= within <= 146096);
    }
    let year_within = (within - within / 1460 + within / 36524 - within / 146096) / 365;
    proof { assert(0 <= year_within <= 401); }
    let year = year_within + era * 400;
    let year_day = within - (365 * year_within + year_within / 4 - year_within / 100);
    proof { assert(-147000 <= year_day <= 147000); }
    let march_month = (5 * year_day + 2) / 153;
    proof { assert(-5000 <= march_month <= 5000); }
    let month = if march_month < 10 { march_month + 2 } else { march_month - 10 };
    (year + if month < 2 { 1 } else { 0 }) * 12 + month
}

pub fn first_day(month: i64) -> (result: i64)
    requires i32::MIN <= month <= i32::MAX,
    ensures result as int == month_start(month as int),
{
    let year = floor_div(month, 12);
    let within = month - year * 12;
    let march_year = year - if within < 2 { 1 } else { 0 };
    let era = floor_div(march_year, 400);
    let year_within = march_year - era * 400;
    let march_month = if within >= 2 { within - 2 } else { within + 10 };
    let year_day = (153 * march_month + 2) / 5;
    let era_day = year_within * 365 + year_within / 4 - year_within / 100 + year_day;
    era * 146097 + era_day - 719468
}

pub open spec fn period_of(day: int, grain: int) -> int {
    if grain == 0 { day }
    else if grain == 1 { month_of(day) }
    else if grain == 2 { month_of(day) / 3 }
    else { month_of(day) / 12 }
}

pub fn period_key(day: i64, grain: u8) -> (result: i64)
    requires i32::MIN <= day <= i32::MAX, 0 <= grain <= 3,
    ensures result as int == period_of(day as int, grain as int),
        i32::MIN <= result <= i32::MAX,
{
    if grain == 0 { day }
    else {
        let month = month_key(day);
        if grain == 1 { month } else if grain == 2 { floor_div(month, 3) } else { floor_div(month, 12) }
    }
}

}
