use vstd::prelude::*;
use verus_common::sums::*;
use verus_common::sqlfns::*;
use crate::gen_spec::*;
use crate::pages::*;
use crate::algebra::*;
use crate::gen_obligations::max_rows;

verus! {

broadcast use vstd::seq_lib::group_seq_properties;

pub const LIMIT: i64 = 1125899906842624;

pub open spec fn country(code: u32) -> Seq<u8> {
    if code == 0 { Seq::empty() }
    else if code <= 256 { seq![(code - 1) as u8] }
    else { seq![((code - 257) / 256) as u8, ((code - 257) % 256) as u8] }
}

pub open spec fn prefix_spec(phone: Seq<u8>) -> u32 {
    if phone.len() == 0 { 0 }
    else if phone.len() == 1 { (1 + phone[0] as u32) as u32 }
    else { (257 + 256 * phone[0] as u32 + phone[1] as u32) as u32 }
}

pub proof fn lemma_prefix_model(phone: Seq<u8>)
    ensures prefix_spec(phone) <= 65792, country(prefix_spec(phone)) == sql_substr(phone, 1, 2),
{
    if phone.len() >= 2 {
        assert((prefix_spec(phone) - 257) / 256 == phone[0] as int);
        assert((prefix_spec(phone) - 257) % 256 == phone[1] as int);
    }
    assert(country(prefix_spec(phone)) =~= sql_substr(phone, 1, 2));
}

pub fn prefix(phone: &[u8]) -> (code: u32)
    ensures code <= 65792, code == prefix_spec(phone@), country(code) == sql_substr(phone@, 1, 2),
{
    proof { lemma_prefix_model(phone@); }
    if phone.len() == 0 { 0 }
    else if phone.len() == 1 { 1 + phone[0] as u32 }
    else { 257 + 256 * phone[0] as u32 + phone[1] as u32 }
}

pub fn decode(code: u32) -> (result: Vec<u8>)
    requires code <= 65792,
    ensures result@ == country(code),
{
    let mut result = Vec::new();
    if code > 0 && code <= 256 { result.push((code - 1) as u8); }
    else if code > 256 { result.push(((code - 257) / 256) as u8); result.push(((code - 257) % 256) as u8); }
    result
}

#[verifier::inline]
pub open spec fn histogram_key(code: u32, depth: u32, interval: u64) -> u128 {
    (((code as u128 * 64 + depth as u128) as u128) << 50) | interval as u128
}

pub fn hkey(code: u32, depth: u32, interval: u64) -> (key: u128)
    requires code <= 65792, depth <= 50, interval < (1u64 << depth),
    ensures key == histogram_key(code, depth, interval), key < KEY_LIMIT,
{
    proof {
        assert(((((code as u128 * 64 + depth as u128) as u128) << 50) | interval as u128) < KEY_LIMIT) by (bit_vector)
            requires code <= 65792, depth <= 50, interval < (1u64 << depth);
    }
    ((code as u128 * 64 + depth as u128) << 50) | interval as u128
}

pub proof fn lemma_histogram_key_bound(code: u32, depth: u32, interval: u64)
    requires code <= 65792, depth <= 50, interval < (1u64 << depth),
    ensures histogram_key(code, depth, interval) < KEY_LIMIT,
{
    assert(histogram_key(code, depth, interval) < KEY_LIMIT) by (bit_vector)
        requires code <= 65792, depth <= 50, interval < (1u64 << depth);
}

pub open spec fn orders_at(db: Db, key: i64) -> int {
    sum(db.orders, |row: OrdersRow| ind(row.o_custkey == key))
}

pub open spec fn positive_weight(row: CustomerRow, code: u32) -> bool {
    row.c_acctbal > 0 && sql_substr(row.c_phone, 1, 2) == country(code)
}

pub open spec fn interval_weight(row: CustomerRow, code: u32, depth: u32, interval: u64) -> bool {
    positive_weight(row, code) && ((row.c_acctbal as u64) >> ((50 - depth) as u32)) == interval
}

pub open spec fn histogram_weight(db: Db, row: CustomerRow, code: u32, depth: u32, interval: u64) -> bool {
    interval_weight(row, code, depth, interval) &&
    !(exists|index: int| 0 <= index < db.orders.len() && (#[trigger] db.orders[index]).o_custkey == row.c_custkey)
}

pub open spec fn moment_sum(rows: Seq<CustomerRow>, predicate: spec_fn(CustomerRow) -> bool) -> (int, int) {
    (sum(rows, |row: CustomerRow| ind(predicate(row))),
     sum(rows, |row: CustomerRow| if predicate(row) { row.c_acctbal } else { 0 }))
}

pub open spec fn signed_moment(rows: Seq<(CustomerRow, int)>, predicate: spec_fn(CustomerRow) -> bool) -> (int, int) {
    (sum(rows, |entry: (CustomerRow, int)| entry.1 * ind(predicate(entry.0))),
     sum(rows, |entry: (CustomerRow, int)| if predicate(entry.0) { entry.1 * entry.0.c_acctbal } else { 0 }))
}

pub proof fn lemma_moment_weights(rows: Seq<CustomerRow>, predicate: spec_fn(CustomerRow) -> bool,
    count: spec_fn(CustomerRow) -> int, total: spec_fn(CustomerRow) -> int)
    requires forall|row: CustomerRow| #[trigger] count(row) == ind(predicate(row)),
        forall|row: CustomerRow| #[trigger] total(row) == if predicate(row) { row.c_acctbal } else { 0int },
    ensures moment_sum(rows, predicate) == (sum(rows, count), sum(rows, total)),
{
    lemma_sum_ext_all(rows, |row: CustomerRow| ind(predicate(row)), count);
    lemma_sum_ext_all(rows, |row: CustomerRow| if predicate(row) { row.c_acctbal } else { 0int }, total);
}

pub proof fn lemma_signed_weights(rows: Seq<(CustomerRow, int)>, predicate: spec_fn(CustomerRow) -> bool,
    count: spec_fn((CustomerRow, int)) -> int, total: spec_fn((CustomerRow, int)) -> int)
    requires forall|entry: (CustomerRow, int)| #[trigger] count(entry) == entry.1 * ind(predicate(entry.0)),
        forall|entry: (CustomerRow, int)| #[trigger] total(entry) == if predicate(entry.0) { entry.1 * entry.0.c_acctbal } else { 0int },
    ensures signed_moment(rows, predicate) == (sum(rows, count), sum(rows, total)),
{
    lemma_sum_ext_all(rows, |entry: (CustomerRow, int)| entry.1 * ind(predicate(entry.0)), count);
    lemma_sum_ext_all(rows, |entry: (CustomerRow, int)| if predicate(entry.0) { entry.1 * entry.0.c_acctbal } else { 0int }, total);
}

pub proof fn lemma_signed_ext(rows: Seq<(CustomerRow, int)>, first: spec_fn(CustomerRow) -> bool, second: spec_fn(CustomerRow) -> bool)
    requires forall|index: int| 0 <= index < rows.len() ==> first((#[trigger] rows[index]).0) == second(rows[index].0),
    ensures signed_moment(rows, first) == signed_moment(rows, second),
{
    lemma_sum_ext(rows, |entry: (CustomerRow, int)| entry.1 * ind(first(entry.0)), |entry: (CustomerRow, int)| entry.1 * ind(second(entry.0)));
    lemma_sum_ext(rows, |entry: (CustomerRow, int)| if first(entry.0) { entry.1 * entry.0.c_acctbal } else { 0int },
        |entry: (CustomerRow, int)| if second(entry.0) { entry.1 * entry.0.c_acctbal } else { 0int });
}

pub proof fn lemma_capture_moment(before: Seq<CustomerRow>, after: Seq<CustomerRow>, delta: Seq<(CustomerRow, int)>,
    predicate: spec_fn(CustomerRow) -> bool)
    requires bag_step(before, after, delta),
    ensures moment_sum(after, predicate) == plus(moment_sum(before, predicate), signed_moment(delta, predicate)),
{
    let count = |row: CustomerRow| ind(predicate(row));
    let total = |row: CustomerRow| if predicate(row) { row.c_acctbal } else { 0int };
    assert(sum(after, count) == sum(before, count) + sum(delta, |entry: (CustomerRow, int)| entry.1 * count(entry.0)));
    assert(sum(after, total) == sum(before, total) + sum(delta, |entry: (CustomerRow, int)| entry.1 * total(entry.0)));
    lemma_signed_weights(delta, predicate,
        |entry: (CustomerRow, int)| entry.1 * count(entry.0), |entry: (CustomerRow, int)| entry.1 * total(entry.0));
}

pub proof fn lemma_capture_histogram(before: Seq<CustomerRow>, after: Seq<CustomerRow>, delta: Seq<(CustomerRow, int)>,
    old_orders: Pages, new_orders: Pages, code: u32, depth: u32, interval: u64)
    requires bag_step(before, after, delta),
    ensures moment_sum(after, |row: CustomerRow| interval_weight(row, code, depth, interval) && eligible(new_orders, row)) ==
        plus(moment_sum(before, |row: CustomerRow| interval_weight(row, code, depth, interval) && eligible(old_orders, row)),
            plus(signed_moment(delta, |row: CustomerRow| interval_weight(row, code, depth, interval) && eligible(old_orders, row)),
                crossing_moment(after, old_orders, new_orders, code, depth, interval))),
{
    let predicate = |row: CustomerRow| interval_weight(row, code, depth, interval);
    let old_predicate = |row: CustomerRow| predicate(row) && eligible(old_orders, row);
    let new_predicate = |row: CustomerRow| predicate(row) && eligible(new_orders, row);
    let count = |row: CustomerRow| ind(predicate(row));
    let total = |row: CustomerRow| if predicate(row) { row.c_acctbal } else { 0int };
    let old_factor = |row: CustomerRow| ind(eligible(old_orders, row));
    let new_factor = |row: CustomerRow| ind(eligible(new_orders, row));
    lemma_signed_product(before, after, delta, count, old_factor, new_factor);
    lemma_signed_product(before, after, delta, total, old_factor, new_factor);
    lemma_moment_weights(before, old_predicate, |row: CustomerRow| count(row) * old_factor(row), |row: CustomerRow| total(row) * old_factor(row));
    lemma_moment_weights(after, new_predicate, |row: CustomerRow| count(row) * new_factor(row), |row: CustomerRow| total(row) * new_factor(row));
    lemma_signed_weights(delta, old_predicate,
        |entry: (CustomerRow, int)| entry.1 * (count(entry.0) * old_factor(entry.0)),
        |entry: (CustomerRow, int)| entry.1 * (total(entry.0) * old_factor(entry.0)));
    lemma_sum_ext_all(after, |row: CustomerRow| count(row) * (new_factor(row) - old_factor(row)),
        |row: CustomerRow| if interval_weight(row, code, depth, interval) { ind(eligible(new_orders, row)) - ind(eligible(old_orders, row)) } else { 0int });
    let correction_total = |row: CustomerRow| total(row) * (new_factor(row) - old_factor(row));
    let normalized_total = |row: CustomerRow| if interval_weight(row, code, depth, interval) { (ind(eligible(new_orders, row)) - ind(eligible(old_orders, row))) * row.c_acctbal } else { 0int };
    assert forall|row: CustomerRow| #[trigger] correction_total(row) == normalized_total(row) by {
        if interval_weight(row, code, depth, interval) {
            let balance = row.c_acctbal;
            let difference = new_factor(row) - old_factor(row);
            assert(balance * difference == difference * balance) by (nonlinear_arith);
        }
    }
    lemma_sum_ext_all(after, |row: CustomerRow| total(row) * (new_factor(row) - old_factor(row)),
        |row: CustomerRow| if interval_weight(row, code, depth, interval) { (ind(eligible(new_orders, row)) - ind(eligible(old_orders, row))) * row.c_acctbal } else { 0int });
    lemma_moment_ext(before, old_predicate, |row: CustomerRow| interval_weight(row, code, depth, interval) && eligible(old_orders, row));
    lemma_moment_ext(after, new_predicate, |row: CustomerRow| interval_weight(row, code, depth, interval) && eligible(new_orders, row));
    lemma_signed_ext(delta, old_predicate, |row: CustomerRow| interval_weight(row, code, depth, interval) && eligible(old_orders, row));
}

pub open spec fn eligible(orders: Pages, row: CustomerRow) -> bool {
    orders.value(row.c_custkey as i64 as u64 as u128).0 == 0
}

pub open spec fn crossing_moment(rows: Seq<CustomerRow>, before: Pages, after: Pages,
    code: u32, depth: u32, interval: u64) -> (int, int)
{
    (sum(rows, |row: CustomerRow| if interval_weight(row, code, depth, interval) {
        ind(eligible(after, row)) - ind(eligible(before, row))
    } else { 0 }),
     sum(rows, |row: CustomerRow| if interval_weight(row, code, depth, interval) {
        (ind(eligible(after, row)) - ind(eligible(before, row))) * row.c_acctbal
    } else { 0 }))
}

pub open spec fn plus(left: (int, int), right: (int, int)) -> (int, int) {
    (left.0 + right.0, left.1 + right.1)
}

pub open spec fn ancestor_moment(code: u32, balance: i64, sign: i128, key: u128, depths: int) -> (int, int) {
    let count = sum(rng(depths), |depth: int|
        if histogram_key(code, depth as u32, (balance as u64) >> ((50 - depth) as u64)) == key { sign as int } else { 0 });
    (count, count * balance)
}

pub open spec fn suffix_moment(histogram: Pages, code: u32, threshold: i128, heights: int) -> (int, int) {
    if threshold >= LIMIT - 1 { (0, 0) }
    else {
        (sum(rng(heights), |height: int| if (((threshold as u64) >> (height as u64)) & 1) == 0 {
            histogram.value(histogram_key(code, (50 - height) as u32,
                ((((threshold as u64) >> (height as u64)) + 1) as u64))).0
        } else { 0 }),
         sum(rng(heights), |height: int| if (((threshold as u64) >> (height as u64)) & 1) == 0 {
            histogram.value(histogram_key(code, (50 - height) as u32,
                ((((threshold as u64) >> (height as u64)) + 1) as u64))).1
        } else { 0 }))
    }
}

pub proof fn lemma_interval_bound(balance: i64, depth: u32)
    requires 0 < balance < LIMIT, depth <= 50,
    ensures (balance as u64) >> ((50 - depth) as u32) < (1u64 << depth),
{
    assert((balance as u64) < 1125899906842624u64);
    assert((balance as u64) >> ((50 - depth) as u32) < (1u64 << depth)) by (bit_vector)
        requires 0 < balance < 1125899906842624i64, depth <= 50;
}

pub proof fn lemma_country_injective(left: u32, right: u32)
    requires left <= 65792, right <= 65792,
    ensures country(left) == country(right) <==> left == right,
{
    if country(left) == country(right) {
        assert(country(left).len() == country(right).len());
        if left == 0 || right == 0 { }
        else if left <= 256 || right <= 256 {
            assert(left <= 256 && right <= 256);
            assert(country(left)[0] as int == left - 1);
            assert(country(right)[0] as int == right - 1);
        } else {
            assert(country(left)[0] as int == (left - 257) / 256);
            assert(country(left)[1] as int == (left - 257) % 256);
            assert(country(right)[0] as int == (right - 257) / 256);
            assert(country(right)[1] as int == (right - 257) % 256);
        }
    }
}

pub proof fn lemma_signed_key_injective(left: i64, right: i64)
    ensures left as u64 == right as u64 <==> left == right,
{
    assert(left as u64 == right as u64 <==> left == right) by (bit_vector);
}

pub proof fn lemma_histogram_key_injective(code: u32, depth: u32, interval: u64,
    other_code: u32, other_depth: u32, other_interval: u64)
    requires code <= 65792, depth <= 50, interval < (1u64 << depth),
        other_code <= 65792, other_depth <= 50, other_interval < (1u64 << other_depth),
    ensures histogram_key(code, depth, interval) == histogram_key(other_code, other_depth, other_interval) <==>
        code == other_code && depth == other_depth && interval == other_interval,
{
    assert(histogram_key(code, depth, interval) == histogram_key(other_code, other_depth, other_interval) <==>
        code == other_code && depth == other_depth && interval == other_interval) by (bit_vector)
        requires code <= 65792, depth <= 50, interval < (1u64 << depth),
            other_code <= 65792, other_depth <= 50, other_interval < (1u64 << other_depth);
}

pub proof fn lemma_ancestor_at(code: u32, balance: i64, sign: i128, other_code: u32, depth: u32, interval: u64)
    requires code <= 65792, 0 < balance < LIMIT, other_code <= 65792, depth <= 50, interval < (1u64 << depth),
    ensures ancestor_moment(code, balance, sign, histogram_key(other_code, depth, interval), 51) ==
        if code == other_code && ((balance as u64) >> ((50 - depth) as u32)) == interval {
            (sign as int, sign * balance)
        } else { (0int, 0int) },
{
    let key = histogram_key(other_code, depth, interval);
    let weight = |level: int| if histogram_key(code, level as u32, (balance as u64) >> ((50 - level) as u64)) == key { sign as int } else { 0int };
    assert forall|index: int| 0 <= index < 51 && index != depth implies weight(#[trigger] rng(51)[index]) == 0 by {
        lemma_interval_bound(balance, index as u32);
        lemma_histogram_key_injective(code, index as u32, (balance as u64) >> ((50 - index) as u32), other_code, depth, interval);
    }
    lemma_sum_single_match(rng(51), weight, depth as int);
    lemma_interval_bound(balance, depth);
    lemma_histogram_key_injective(code, depth, (balance as u64) >> ((50 - depth) as u32), other_code, depth, interval);
}

pub proof fn lemma_ancestor_step(code: u32, balance: i64, sign: i128, key: u128, depth: u32)
    requires depth <= 50,
    ensures ancestor_moment(code, balance, sign, key, depth as int + 1) ==
        plus(ancestor_moment(code, balance, sign, key, depth as int),
            if key == histogram_key(code, depth, (balance as u64) >> ((50 - depth) as u32)) {
                (sign as int, sign * balance)
            } else { (0int, 0int) }),
{
    let weight = |level: int| if histogram_key(code, level as u32, (balance as u64) >> ((50 - level) as u64)) == key { sign as int } else { 0int };
    assert(rng(depth as int + 1) =~= rng(depth as int).push(depth as int));
    lemma_sum_push(rng(depth as int), depth as int, weight);
    assert((sum(rng(depth as int), weight) + weight(depth as int)) * balance ==
        sum(rng(depth as int), weight) * balance + weight(depth as int) * balance) by (nonlinear_arith);
}

pub proof fn lemma_suffix_step(histogram: Pages, code: u32, threshold: i128, height: u32)
    requires 0 <= threshold < LIMIT - 1, height < 50,
    ensures suffix_moment(histogram, code, threshold, height as int + 1) ==
        plus(suffix_moment(histogram, code, threshold, height as int),
            if (((threshold as u64) >> height) & 1) == 0 {
                histogram.value(histogram_key(code, (50 - height) as u32,
                    ((((threshold as u64) >> height) + 1) as u64)))
            } else { (0int, 0int) }),
{
    assert(rng(height as int + 1) =~= rng(height as int).push(height as int));
    lemma_sum_push(rng(height as int), height as int, |level: int| if (((threshold as u64) >> (level as u64)) & 1) == 0 {
        histogram.value(histogram_key(code, (50 - level) as u32, ((((threshold as u64) >> (level as u64)) + 1) as u64))).0
    } else { 0int });
    lemma_sum_push(rng(height as int), height as int, |level: int| if (((threshold as u64) >> (level as u64)) & 1) == 0 {
        histogram.value(histogram_key(code, (50 - level) as u32, ((((threshold as u64) >> (level as u64)) + 1) as u64))).1
    } else { 0int });
}

pub proof fn lemma_signed_balance(sign: i128, balance: i64)
    requires -1 <= sign <= 1,
    ensures i128::MIN <= sign * balance <= i128::MAX,
{
    if sign == -1 { } else if sign == 0 { } else { assert(sign == 1); }
}

pub open spec fn positive_moment(db: Db, code: u32) -> (int, int) {
    moment_sum(db.customer, |row: CustomerRow| positive_weight(row, code))
}

pub open spec fn histogram_moment(db: Db, code: u32, depth: u32, interval: u64) -> (int, int) {
    moment_sum(db.customer, |row: CustomerRow| histogram_weight(db, row, code, depth, interval))
}

pub proof fn lemma_moment_ext(rows: Seq<CustomerRow>, first: spec_fn(CustomerRow) -> bool, second: spec_fn(CustomerRow) -> bool)
    requires forall|index: int| 0 <= index < rows.len() ==> first(#[trigger] rows[index]) == second(rows[index]),
    ensures moment_sum(rows, first) == moment_sum(rows, second),
{
    lemma_sum_ext(rows, |row: CustomerRow| ind(first(row)), |row: CustomerRow| ind(second(row)));
    lemma_sum_ext(rows, |row: CustomerRow| if first(row) { row.c_acctbal } else { 0int },
        |row: CustomerRow| if second(row) { row.c_acctbal } else { 0int });
}

pub open spec fn histogram_small(histogram: Pages) -> bool {
    forall|code: u32, depth: u32, interval: u64| code <= 65792 && depth <= 50 && interval < (1u64 << depth) ==>
        small(#[trigger] histogram.value(histogram_key(code, depth, interval)))
}

pub open spec fn histogram_bounded(histogram: Pages) -> bool {
    forall|code: u32, depth: u32, interval: u64| code <= 65792 && depth <= 50 && interval < (1u64 << depth) ==>
        bounded(#[trigger] histogram.value(histogram_key(code, depth, interval)))
}

pub proof fn lemma_ancestor_small(code: u32, balance: i64, sign: i128, key: u128, depths: int)
    requires -1 <= sign <= 1, 0 < balance < LIMIT, 0 <= depths <= 51,
    ensures -51 <= ancestor_moment(code, balance, sign, key, depths).0 <= 51,
        -51 * LIMIT <= ancestor_moment(code, balance, sign, key, depths).1 <= 51 * LIMIT,
{
    let weight = |depth: int|
        if histogram_key(code, depth as u32, (balance as u64) >> ((50 - depth) as u64)) == key { sign as int } else { 0int };
    lemma_sum_between(rng(depths), weight, -1, 1);
    let count = ancestor_moment(code, balance, sign, key, depths).0;
    assert(-51 <= count <= 51);
    assert(-51 * LIMIT <= count * balance <= 51 * LIMIT) by (nonlinear_arith)
        requires -51 <= count <= 51, 0 < balance < LIMIT;
}

pub fn update_histogram(histogram: &mut Pages, code: u32, balance: i64, sign: i128) -> (result: bool)
    requires code <= 65792, 0 < balance < LIMIT, -1 <= sign <= 1,
    ensures result ==> forall|key: u128| key < KEY_LIMIT ==>
            #[trigger] final(histogram).value(key) == plus(old(histogram).value(key), ancestor_moment(code, balance, sign, key, 51)),
        histogram_small(*old(histogram)) ==> result,
{
    let initial = Ghost(*histogram);
    let mut depth = 0u32;
    while depth <= 50
        invariant depth <= 51, initial@ == *old(histogram), code <= 65792, 0 < balance < LIMIT, -1 <= sign <= 1,
            forall|key: u128| key < KEY_LIMIT ==>
                #[trigger] histogram.value(key) == plus(initial@.value(key), ancestor_moment(code, balance, sign, key, depth as int)),
        decreases 51 - depth,
    {
        proof { lemma_interval_bound(balance, depth); }
        proof { lemma_signed_balance(sign, balance); }
        let interval = (balance as u64) >> (50 - depth);
        let key = hkey(code, depth, interval);
        proof {
            if histogram_small(initial@) {
                assert(small(initial@.value(histogram_key(code, depth, interval))));
                lemma_ancestor_small(code, balance, sign, key, depth as int);
                assert(histogram.value(key) == plus(initial@.value(key), ancestor_moment(code, balance, sign, key, depth as int)));
                assert(-LIMIT <= sign * balance <= LIMIT) by (nonlinear_arith) requires -1 <= sign <= 1, 0 < balance < LIMIT;
                assert(fits(histogram.value(key).0 + sign) && fits(histogram.value(key).1 + sign * balance as i128));
            }
        }
        if !histogram.add(key, Moment { count: sign, total: sign * balance as i128 }) {
            proof { assert(!histogram_small(initial@)); }
            return false;
        }
        proof {
            assert forall|other: u128| other < KEY_LIMIT implies #[trigger] histogram.value(other) ==
                plus(initial@.value(other), ancestor_moment(code, balance, sign, other, depth as int + 1)) by {
                lemma_ancestor_step(code, balance, sign, other, depth);
            }
        }
        depth += 1;
    }
    true
}

pub fn suffix(histogram: &Pages, code: u32, threshold: i128) -> (result: Option<Moment>)
    requires code <= 65792, threshold >= 0,
    ensures result matches Some(value) ==> value.model() == suffix_moment(*histogram, code, threshold, 50),
        histogram_bounded(*histogram) ==> (result matches Some(value) &&
            0 <= value.count <= 50 * max_rows() && 0 <= value.total <= 50 * max_total()),
{
    let mut result = Moment::zero();
    if threshold >= (LIMIT - 1) as i128 { return Some(result); }
    let cutoff = threshold as u64;
    let mut height = 0u32;
    while height < 50
        invariant height <= 50, code <= 65792, 0 <= threshold < LIMIT - 1, cutoff == threshold,
            result.model() == suffix_moment(*histogram, code, threshold, height as int),
            histogram_bounded(*histogram) ==> (0 <= result.count <= height as int * max_rows() && 0 <= result.total <= height as int * max_total()),
        decreases 50 - height,
    {
        proof {
            lemma_suffix_step(*histogram, code, threshold, height);
            if histogram_bounded(*histogram) {
                let h = height as int;
                assert(h * max_rows() + max_rows() == (h + 1) * max_rows()) by (nonlinear_arith);
                assert(0 <= h * max_rows()) by (nonlinear_arith) requires h >= 0;
                assert(h * max_total() + max_total() == (h + 1) * max_total()) by (nonlinear_arith);
                assert(0 <= h * max_total()) by (nonlinear_arith) requires h >= 0;
                assert(h * max_rows() <= (h + 1) * max_rows()) by (nonlinear_arith) requires h >= 0;
                assert(h * max_total() <= (h + 1) * max_total()) by (nonlinear_arith) requires h >= 0;
                assert((h + 1) * max_rows() <= 50 * max_rows()) by (nonlinear_arith) requires h < 50;
                assert((h + 1) * max_total() <= 50 * max_total()) by (nonlinear_arith) requires h < 50;
            }
        }
        if ((cutoff >> height) & 1) == 0 {
            proof {
                assert((cutoff >> height) + 1 < (1u64 << ((50 - height) as u32))) by (bit_vector)
                    requires cutoff < 1125899906842623u64, height < 50, ((cutoff >> height) & 1) == 0;
            }
            let depth = 50 - height;
            let interval = (cutoff >> height) + 1;
            let page = histogram.get(hkey(code, depth, interval));
            proof {
                if histogram_bounded(*histogram) {
                    assert(bounded(histogram.value(histogram_key(code, depth, interval))));
                    assert(fits(result.count + page.count) && fits(result.total + page.total));
                }
            }
            result = match result.add(page) { Some(value) => value, None => {
                proof { assert(!histogram_bounded(*histogram)); }
                return None;
            } };
        }
        height += 1;
    }
    Some(result)
}

}
