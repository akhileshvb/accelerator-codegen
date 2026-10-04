use vstd::prelude::*;
use verus_common::rel::pow2;
use crate::moments::*;

verus! {

pub proof fn lemma_pow_positive(exponent: nat)
    ensures pow2(exponent) > 0,
    decreases exponent,
{
    if exponent > 0 { lemma_pow_positive((exponent - 1) as nat); }
}

pub proof fn lemma_pow_add(left: nat, right: nat)
    ensures pow2(left + right) == pow2(left) * pow2(right),
    decreases right,
{
    if right > 0 {
        lemma_pow_add(left, (right - 1) as nat);
        assert(pow2(left + right) == pow2(left) * pow2(right)) by (nonlinear_arith)
            requires pow2(left + right) == 2 * pow2((left + right - 1) as nat),
                pow2(right) == 2 * pow2((right - 1) as nat),
                pow2((left + right - 1) as nat) == pow2(left) * pow2((right - 1) as nat);
    }
}

pub proof fn lemma_pow_monotone(left: nat, right: nat)
    requires left <= right,
    ensures pow2(left) <= pow2(right),
    decreases right - left,
{
    lemma_pow_positive(left);
    if left < right { lemma_pow_monotone(left, (right - 1) as nat); }
}

pub proof fn lemma_pow_constants()
    ensures pow2(4) == 16, pow2(6) == 64, pow2(27) == 134217728,
        pow2(32) == 4294967296, pow2(33) == 8589934592, pow2(34) == 17179869184,
{ reveal_with_fuel(pow2, 35); }

pub fn power(exponent: u32) -> (out: u64)
    requires exponent <= 34,
    ensures out as int == pow2(exponent as nat), 0 < out <= 17179869184u64,
    decreases exponent,
{
    proof { lemma_pow_constants(); lemma_pow_monotone(exponent as nat, 34); }
    if exponent == 0 { 1 } else {
        let previous = power(exponent - 1);
        previous * 2
    }
}

pub proof fn lemma_division(value: int, divisor: int)
    requires divisor > 0,
    ensures 0 <= value % divisor < divisor, value == divisor * (value / divisor) + value % divisor,
{ vstd::arithmetic::div_mod::lemma_fundamental_div_mod(value, divisor); }

pub proof fn lemma_quotient(value: int, divisor: int, quotient: int, remainder: int)
    requires divisor > 0, 0 <= remainder < divisor, value == divisor * quotient + remainder,
    ensures value / divisor == quotient, value % divisor == remainder,
{
    lemma_division(value, divisor);
    assert(value / divisor == quotient) by (nonlinear_arith)
        requires divisor > 0, 0 <= remainder < divisor, 0 <= value % divisor < divisor,
            value == divisor * quotient + remainder,
            value == divisor * (value / divisor) + value % divisor;
    assert(value % divisor == remainder) by (nonlinear_arith)
        requires value / divisor == quotient, value == divisor * quotient + remainder,
            value == divisor * (value / divisor) + value % divisor;
}

pub proof fn lemma_nested_division(value: int, first: int, second: int)
    requires first > 0, second > 0,
    ensures value / (first * second) == (value / first) / second,
{
    lemma_division(value, first);
    lemma_division(value / first, second);
    let quotient = (value / first) / second;
    let remainder = first * ((value / first) % second) + value % first;
    assert(0 <= remainder < first * second) by (nonlinear_arith)
        requires first > 0, second > 0, 0 <= (value / first) % second < second, 0 <= value % first < first,
            remainder == first * ((value / first) % second) + value % first;
    assert(value == (first * second) * quotient + remainder) by (nonlinear_arith)
        requires value == first * (value / first) + value % first,
            value / first == second * quotient + (value / first) % second,
            remainder == first * ((value / first) % second) + value % first;
    assert(first * second > 0) by (nonlinear_arith) requires first > 0, second > 0;
    lemma_quotient(value, first * second, quotient, remainder);
}

pub proof fn lemma_radix(value: int, stride: int)
    requires stride > 0,
    ensures value % (16 * stride) == stride * ((value / stride) % 16) + value % stride,
        (value % (16 * stride)) / stride == (value / stride) % 16,
        (value % (16 * stride)) % stride == value % stride,
{
    lemma_division(value, stride);
    lemma_division(value / stride, 16);
    let quotient = (value / stride) / 16;
    let digit = (value / stride) % 16;
    let remainder = stride * digit + value % stride;
    assert(0 <= remainder < 16 * stride) by (nonlinear_arith)
        requires stride > 0, 0 <= digit < 16, 0 <= value % stride < stride,
            remainder == stride * digit + value % stride;
    assert(value == (16 * stride) * quotient + remainder) by (nonlinear_arith)
        requires value == stride * (value / stride) + value % stride, value / stride == 16 * quotient + digit,
            remainder == stride * digit + value % stride;
    lemma_quotient(value, 16 * stride, quotient, remainder);
    lemma_quotient(remainder, stride, digit, value % stride);
}

pub proof fn lemma_same_radix(left: int, right: int, stride: int)
    requires stride > 0,
    ensures (left % (16 * stride) == right % (16 * stride)) ==
        ((left / stride) % 16 == (right / stride) % 16 && left % stride == right % stride),
{
    lemma_radix(left, stride);
    lemma_radix(right, stride);
}

pub proof fn lemma_block_bounds(date: int, level: nat)
    requires -2147483648 <= date < 2147483648, level <= 32,
    ensures pow2((32 - level) as nat) <= block_id(date, level) < 2 * pow2((32 - level) as nat),
        0 < block_id(date, level) < 8589934592,
{
    lemma_pow_constants();
    lemma_pow_add((32 - level) as nat, level);
    lemma_pow_positive(level);
    lemma_pow_positive((32 - level) as nat);
    lemma_pow_monotone((32 - level) as nat, 32);
    let base = pow2((32 - level) as nat);
    let width = pow2(level);
    lemma_division(date + 2147483648, width);
    assert(0 <= (date + 2147483648) / width < base) by (nonlinear_arith)
        requires base > 0, width > 0, base * width == 4294967296,
            0 <= date + 2147483648 < 4294967296,
            date + 2147483648 == width * ((date + 2147483648) / width) + (date + 2147483648) % width,
            0 <= (date + 2147483648) % width < width;
}

pub proof fn lemma_block_level(first_date: int, first_level: nat, second_date: int, second_level: nat)
    requires -2147483648 <= first_date < 2147483648, -2147483648 <= second_date < 2147483648,
        first_level <= 32, second_level <= 32, block_id(first_date, first_level) == block_id(second_date, second_level),
    ensures first_level == second_level,
{
    lemma_block_bounds(first_date, first_level);
    lemma_block_bounds(second_date, second_level);
    if first_level < second_level {
        lemma_pow_monotone((33 - second_level) as nat, (32 - first_level) as nat);
        assert(pow2((33 - second_level) as nat) == 2 * pow2((32 - second_level) as nat));
    }
    if second_level < first_level {
        lemma_pow_monotone((33 - first_level) as nat, (32 - second_level) as nat);
        assert(pow2((33 - first_level) as nat) == 2 * pow2((32 - first_level) as nat));
    }
}

pub proof fn lemma_block_prefix(id: u64, date: int, level: nat)
    requires -2147483648 <= date < 2147483648, level <= 32,
    ensures in_block_prefix(id, date, level + 1) == (in_block_prefix(id, date, level) || id as int == block_id(date, level)),
        id as int == block_id(date, level) ==> !in_block_prefix(id, date, level),
{
    if in_block_prefix(id, date, level + 1) {
        let chosen = choose|chosen: nat| #[trigger] block_id(date, chosen) == id as int && chosen < level + 1 && chosen <= 32;
        if chosen < level { assert(in_block_prefix(id, date, level)); }
    }
    if in_block_prefix(id, date, level) {
        let chosen = choose|chosen: nat| #[trigger] block_id(date, chosen) == id as int && chosen < level && chosen <= 32;
        assert(in_block_prefix(id, date, level + 1));
        if id as int == block_id(date, level) { lemma_block_level(date, chosen, date, level); }
    }
    if id as int == block_id(date, level) { assert(in_block_prefix(id, date, level + 1)); }
}

pub proof fn lemma_block_interval(id: u64, date: int, start: int, level: nat)
    requires 0 <= start < 4294967296, level <= 32, start % pow2(level) == 0,
        start + pow2(level) <= 4294967296, id as int == pow2((32 - level) as nat) + start / pow2(level),
    ensures in_block(id, date) == (start <= date + 2147483648 < start + pow2(level)),
{
    lemma_pow_positive(level);
    lemma_division(start, pow2(level));
    if in_block(id, date) {
        let chosen = choose|chosen: nat| #[trigger] block_id(date, chosen) == id as int && chosen < 33 && chosen <= 32;
        lemma_block_level(start - 2147483648, level, date, chosen);
        lemma_division(date + 2147483648, pow2(level));
        assert(start <= date + 2147483648 < start + pow2(level)) by (nonlinear_arith)
            requires start == pow2(level) * (start / pow2(level)),
                (date + 2147483648) / pow2(level) == start / pow2(level),
                date + 2147483648 == pow2(level) * ((date + 2147483648) / pow2(level)) + (date + 2147483648) % pow2(level),
                0 <= (date + 2147483648) % pow2(level) < pow2(level);
    }
    if start <= date + 2147483648 < start + pow2(level) {
        lemma_quotient(date + 2147483648, pow2(level), start / pow2(level), date + 2147483648 - start);
        assert(block_id(date, level) == id as int);
        assert(in_block(id, date));
    }
}

}
