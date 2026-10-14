use vstd::prelude::*;
use vstd::arithmetic::div_mod::lemma_fundamental_div_mod;
use verus_common::sums::*;
use verus_common::rel::*;
use verus_common::sqlfns::*;
use verus_common::bytes::*;
use crate::gen_spec::*;
use crate::gen_obligations::out_view;
use crate::moments::*;
use crate::pages::*;
use crate::algebra::*;

verus! {

broadcast use vstd::seq_lib::group_seq_properties;

pub open spec fn matches_country(parameters: Params, country: Seq<u8>) -> bool {
    country == parameters.p1 || country == parameters.p2 || country == parameters.p3 ||
    country == parameters.p4 || country == parameters.p5 || country == parameters.p6 || country == parameters.p7
}

pub open spec fn code_parameter(code: u32, parameter: Seq<u8>) -> bool {
    parameter.len() <= 2 && prefix_spec(parameter) == code
}

pub open spec fn codes_cover(codes: Seq<u32>, parameters: Params) -> bool {
    &&& codes.no_duplicates()
    &&& forall|index: int| 0 <= index < codes.len() ==> (#[trigger] codes[index]) <= 65792
    &&& forall|code: u32| #[trigger] codes.contains(code) <==>
        code_parameter(code, parameters.p1) || code_parameter(code, parameters.p2) || code_parameter(code, parameters.p3) ||
        code_parameter(code, parameters.p4) || code_parameter(code, parameters.p5) || code_parameter(code, parameters.p6) || code_parameter(code, parameters.p7)
}

pub open spec fn population_sums(positive: Pages, codes: Seq<u32>) -> (int, int) {
    (sum(codes, |code: u32| positive.value(code as u128).0), sum(codes, |code: u32| positive.value(code as u128).1))
}

pub open spec fn population_key(row: CustomerRow) -> u32 {
    prefix_spec(row.c_phone)
}

pub open spec fn population_count_per_code(db: Db, parameters: Params, code: u32) -> int {
    sum(db.customer, |row: CustomerRow|
        if population_key(row) == code { ind(scalar_1_where_clause(db, parameters, row)) } else { 0int })
}

pub open spec fn population_total_per_code(db: Db, parameters: Params, code: u32) -> int {
    sum(db.customer, |row: CustomerRow|
        if population_key(row) == code {
            if scalar_1_where_clause(db, parameters, row) { row.c_acctbal } else { 0int }
        } else { 0int })
}

proof fn lemma_population_group_pre(db: Db, parameters: Params)
    ensures forall|code: u32| #[trigger] population_count_per_code(db, parameters, code) ==
        sum(db.customer, |row: CustomerRow| if population_key(row) == code {
            ind(scalar_1_where_clause(db, parameters, row)) } else { 0int }),
        forall|code: u32| #[trigger] population_total_per_code(db, parameters, code) ==
            sum(db.customer, |row: CustomerRow| if population_key(row) == code {
                if scalar_1_where_clause(db, parameters, row) { row.c_acctbal } else { 0int }
            } else { 0int }),
{
}

pub proof fn lemma_population_partition(db: Db, parameters: Params, code: u32,
    key: spec_fn(CustomerRow) -> u32, count: spec_fn(CustomerRow) -> int,
    total: spec_fn(CustomerRow) -> int)
    requires forall|row: CustomerRow| #[trigger] key(row) == population_key(row),
        forall|row: CustomerRow| #[trigger] count(row) == ind(scalar_1_where_clause(db, parameters, row)),
        forall|row: CustomerRow| #[trigger] total(row) ==
            if scalar_1_where_clause(db, parameters, row) { row.c_acctbal } else { 0int },
    ensures population_count_per_code(db, parameters, code) ==
        sum(db.customer, |row: CustomerRow| if key(row) == code { count(row) } else { 0int }),
        population_total_per_code(db, parameters, code) ==
            sum(db.customer, |row: CustomerRow| if key(row) == code { total(row) } else { 0int }),
{
    let first = |row: CustomerRow| if population_key(row) == code {
        ind(scalar_1_where_clause(db, parameters, row))
    } else { 0int };
    let second = |row: CustomerRow| if key(row) == code { count(row) } else { 0int };
    assert forall|row: CustomerRow| #[trigger] first(row) == second(row) by {
        assert(key(row) == population_key(row));
        assert(count(row) == ind(scalar_1_where_clause(db, parameters, row)));
    }
    lemma_sum_ext_all(db.customer, first, second);
    let third = |row: CustomerRow| if population_key(row) == code {
        if scalar_1_where_clause(db, parameters, row) { row.c_acctbal } else { 0int }
    } else { 0int };
    let fourth = |row: CustomerRow| if key(row) == code { total(row) } else { 0int };
    assert forall|row: CustomerRow| #[trigger] third(row) == fourth(row) by {
        assert(key(row) == population_key(row));
        assert(total(row) == if scalar_1_where_clause(db, parameters, row) { row.c_acctbal } else { 0int });
    }
    lemma_sum_ext_all(db.customer, third, fourth);
}

pub proof fn lemma_code_parameter(code: u32, parameter: Seq<u8>)
    requires code <= 65792,
    ensures country(code) == parameter <==> code_parameter(code, parameter),
{
    lemma_prefix_model(parameter);
    if parameter.len() <= 2 {
        assert(sql_substr(parameter, 1, 2) =~= parameter);
        lemma_country_injective(code, prefix_spec(parameter));
    } else { assert(country(code).len() <= 2); }
}

pub proof fn lemma_codes(codes: Seq<u32>, parameters: Params, phone: Seq<u8>)
    requires codes_cover(codes, parameters),
    ensures codes.contains(prefix_spec(phone)) == matches_country(parameters, sql_substr(phone, 1, 2)),
{
    lemma_prefix_model(phone);
    let code = prefix_spec(phone);
    lemma_code_parameter(code, parameters.p1);
    lemma_code_parameter(code, parameters.p2);
    lemma_code_parameter(code, parameters.p3);
    lemma_code_parameter(code, parameters.p4);
    lemma_code_parameter(code, parameters.p5);
    lemma_code_parameter(code, parameters.p6);
    lemma_code_parameter(code, parameters.p7);
    assert(codes.contains(code) == matches_country(parameters, country(code)));
}

pub proof fn lemma_code_matches(code: u32, codes: Seq<u32>, parameters: Params)
    requires codes_cover(codes, parameters), code <= 65792, codes.contains(code),
    ensures matches_country(parameters, country(code)),
{
    lemma_code_parameter(code, parameters.p1);
    lemma_code_parameter(code, parameters.p2);
    lemma_code_parameter(code, parameters.p3);
    lemma_code_parameter(code, parameters.p4);
    lemma_code_parameter(code, parameters.p5);
    lemma_code_parameter(code, parameters.p6);
    lemma_code_parameter(code, parameters.p7);
}

pub proof fn lemma_prefix_prefix(phone: Seq<u8>)
    ensures prefix_spec(phone) == prefix_spec(sql_substr(phone, 1, 2)),
        sql_substr(sql_substr(phone, 1, 2), 1, 2) == sql_substr(phone, 1, 2),
{
    lemma_prefix_model(phone);
    lemma_prefix_model(sql_substr(phone, 1, 2));
}

pub proof fn lemma_custsale_sum(db: Db, parameters: Params, weight: spec_fn(CustsaleTuple) -> int)
    ensures sum(custsale_tuples(db, parameters), weight) == sum(db.customer, |row: CustomerRow|
        if custsale_where_clause(db, parameters, row) { weight(CustsaleTuple { customer: row }) } else { 0int }),
{
    let extend = |row: CustomerRow| custsale_tuples_1(db, parameters, row);
    let inner = |row: CustomerRow| sum(extend(row), weight);
    let direct = |row: CustomerRow| if custsale_where_clause(db, parameters, row) { weight(CustsaleTuple { customer: row }) } else { 0int };
    lemma_sum_flat_map(db.customer, extend, weight);
    assert forall|row: CustomerRow| #[trigger] inner(row) == direct(row) by {
        lemma_sum_opt(custsale_where_clause(db, parameters, row), CustsaleTuple { customer: row }, weight);
    }
    lemma_sum_ext_all(db.customer, inner, direct);
}

pub proof fn lemma_query_sum(db: Db, parameters: Params, weight: spec_fn(Tuple) -> int)
    ensures sum(tuples(db, parameters), weight) == sum(db.customer, |row: CustomerRow|
        if custsale_where_clause(db, parameters, row) {
            weight(Tuple { custsale: CustsaleRow { cntrycode: sql_substr(row.c_phone, 1, 2), c_acctbal: row.c_acctbal } })
        } else { 0int }),
{
    let extend = |row: CustsaleRow| tuples_1(db, parameters, row);
    let inner = |row: CustsaleRow| sum(extend(row), weight);
    let direct = |row: CustsaleRow| weight(Tuple { custsale: row });
    lemma_sum_flat_map(rel_custsale(db, parameters), extend, weight);
    assert forall|row: CustsaleRow| #[trigger] inner(row) == direct(row) by {
        lemma_sum_opt(true, Tuple { custsale: row }, weight);
    }
    lemma_sum_ext_all(rel_custsale(db, parameters), inner, direct);
    let project = |entry: CustsaleTuple| CustsaleRow { cntrycode: sql_substr(entry.customer.c_phone, 1, 2), c_acctbal: entry.customer.c_acctbal };
    let tuple_weight = |entry: CustsaleTuple| direct(project(entry));
    lemma_sum_map(custsale_tuples(db, parameters), project, direct, tuple_weight);
    lemma_custsale_sum(db, parameters, tuple_weight);
    lemma_sum_ext_all(db.customer, |row: CustomerRow| if custsale_where_clause(db, parameters, row) { tuple_weight(CustsaleTuple { customer: row }) } else { 0int },
        |row: CustomerRow| if custsale_where_clause(db, parameters, row) {
            weight(Tuple { custsale: CustsaleRow { cntrycode: sql_substr(row.c_phone, 1, 2), c_acctbal: row.c_acctbal } })
        } else { 0int });
}

pub proof fn lemma_group_moment(db: Db, parameters: Params, text: Seq<u8>)
    ensures (group_count(db, parameters, text), group_sum(db, parameters, text, |entry: Tuple| entry.custsale.c_acctbal)) ==
        moment_sum(db.customer, |row: CustomerRow| custsale_where_clause(db, parameters, row) && sql_substr(row.c_phone, 1, 2) == text),
{
    let count = |entry: Tuple| if entry.custsale.cntrycode == text { 1int } else { 0int };
    let total = |entry: Tuple| if entry.custsale.cntrycode == text { entry.custsale.c_acctbal } else { 0int };
    lemma_query_sum(db, parameters, count);
    lemma_query_sum(db, parameters, total);
    let balance = |entry: Tuple| entry.custsale.c_acctbal;
    lemma_sum_ext_all(tuples(db, parameters), |entry: Tuple| if entry.custsale.cntrycode == text { balance(entry) } else { 0int }, total);
    lemma_moment_weights(db.customer, |row: CustomerRow| custsale_where_clause(db, parameters, row) && sql_substr(row.c_phone, 1, 2) == text,
        |row: CustomerRow| if custsale_where_clause(db, parameters, row) { count(Tuple { custsale: CustsaleRow { cntrycode: sql_substr(row.c_phone, 1, 2), c_acctbal: row.c_acctbal } }) } else { 0int },
        |row: CustomerRow| if custsale_where_clause(db, parameters, row) { total(Tuple { custsale: CustsaleRow { cntrycode: sql_substr(row.c_phone, 1, 2), c_acctbal: row.c_acctbal } }) } else { 0int });
}

pub open spec fn group_customer(db: Db, parameters: Params, text: Seq<u8>) -> bool {
    exists|row: CustomerRow| #[trigger] custsale_where_clause(db, parameters, row) &&
        sql_substr(row.c_phone, 1, 2) == text
}

pub proof fn lemma_group_customer_witness(db: Db, parameters: Params, text: Seq<u8>) -> (row: CustomerRow)
    requires group_count(db, parameters, text) > 0,
    ensures custsale_where_clause(db, parameters, row),
        sql_substr(row.c_phone, 1, 2) == text,
{
    lemma_group_moment(db, parameters, text);
    let predicate = |row: CustomerRow| custsale_where_clause(db, parameters, row) &&
        sql_substr(row.c_phone, 1, 2) == text;
    let weight = |row: CustomerRow| ind(predicate(row));
    lemma_sum_support(db.customer, weight, predicate);
    choose|row: CustomerRow| #[trigger] predicate(row)
}

pub proof fn lemma_scalar_sum(db: Db, parameters: Params, weight: spec_fn(Scalar1Tuple) -> int)
    ensures sum(scalar_1_tuples(db, parameters), weight) == sum(db.customer, |row: CustomerRow|
        if scalar_1_where_clause(db, parameters, row) { weight(Scalar1Tuple { customer_2: row }) } else { 0int }),
{
    let extend = |row: CustomerRow| scalar_1_tuples_1(db, parameters, row);
    let inner = |row: CustomerRow| sum(extend(row), weight);
    let direct = |row: CustomerRow| if scalar_1_where_clause(db, parameters, row) { weight(Scalar1Tuple { customer_2: row }) } else { 0int };
    lemma_sum_flat_map(db.customer, extend, weight);
    assert forall|row: CustomerRow| #[trigger] inner(row) == direct(row) by {
        lemma_sum_opt(scalar_1_where_clause(db, parameters, row), Scalar1Tuple { customer_2: row }, weight);
    }
    lemma_sum_ext_all(db.customer, inner, direct);
}

pub proof fn lemma_sum_one<A>(rows: Seq<A>)
    ensures sum(rows, |row: A| 1int) == rows.len(),
    decreases rows.len(),
{
    if rows.len() > 0 { lemma_sum_one(rows.drop_last()); }
}

pub proof fn lemma_scalar_moment(db: Db, parameters: Params)
    ensures moment_sum(db.customer, |row: CustomerRow| scalar_1_where_clause(db, parameters, row)) ==
        (scalar_1_group_count(db, parameters), scalar_1_group_sum(db, parameters, |entry: Scalar1Tuple| entry.customer_2.c_acctbal)),
{
    lemma_sum_one(scalar_1_tuples(db, parameters));
    lemma_scalar_sum(db, parameters, |entry: Scalar1Tuple| 1int);
    lemma_scalar_sum(db, parameters, |entry: Scalar1Tuple| entry.customer_2.c_acctbal);
    let count = |entry: Scalar1Tuple| 1int;
    let total = |entry: Scalar1Tuple| entry.customer_2.c_acctbal;
    lemma_sum_ext_all(db.customer, |row: CustomerRow| if scalar_1_where_clause(db, parameters, row) { count(Scalar1Tuple { customer_2: row }) } else { 0int },
        |row: CustomerRow| if scalar_1_where_clause(db, parameters, row) { 1int } else { 0int });
    lemma_sum_ext_all(db.customer, |row: CustomerRow| if scalar_1_where_clause(db, parameters, row) { total(Scalar1Tuple { customer_2: row }) } else { 0int },
        |row: CustomerRow| if scalar_1_where_clause(db, parameters, row) { row.c_acctbal } else { 0int });
    lemma_moment_weights(db.customer, |row: CustomerRow| scalar_1_where_clause(db, parameters, row),
        |row: CustomerRow| if scalar_1_where_clause(db, parameters, row) { 1int } else { 0int },
        |row: CustomerRow| if scalar_1_where_clause(db, parameters, row) { row.c_acctbal } else { 0int });
}

pub proof fn lemma_population(db: Db, positive: Pages, codes: Seq<u32>, parameters: Params)
    requires codes_cover(codes, parameters),
        forall|code: u32| code <= 65792 ==> #[trigger] positive.value(code as u128) == positive_moment(db, code),
    ensures population_sums(positive, codes) ==
        (scalar_1_group_count(db, parameters), scalar_1_group_sum(db, parameters, |entry: Scalar1Tuple| entry.customer_2.c_acctbal)),
{
    let count = |row: CustomerRow| ind(scalar_1_where_clause(db, parameters, row));
    let total = |row: CustomerRow| if scalar_1_where_clause(db, parameters, row) { row.c_acctbal } else { 0int };
    assert forall|index: int| 0 <= index < db.customer.len() && count(db.customer[index]) != 0 implies codes.contains(population_key(#[trigger] db.customer[index])) by {
        lemma_codes(codes, parameters, db.customer[index].c_phone);
    }
    assert forall|index: int| 0 <= index < db.customer.len() && total(db.customer[index]) != 0 implies codes.contains(population_key(#[trigger] db.customer[index])) by {
        lemma_codes(codes, parameters, db.customer[index].c_phone);
    }
    assert(distinct(codes));
    let key = |row: CustomerRow| population_key(row);
    let per_count = |code: u32| sum(db.customer, |row: CustomerRow|
        if population_key(row) == code { ind(scalar_1_where_clause(db, parameters, row)) } else { 0int });
    let per_total = |code: u32| sum(db.customer, |row: CustomerRow|
        if population_key(row) == code {
            if scalar_1_where_clause(db, parameters, row) { row.c_acctbal } else { 0int }
        } else { 0int });
    let exact_count = per_count;
    let exact_total = per_total;
    lemma_population_group_pre(db, parameters);
    assert forall|row: CustomerRow| #[trigger] key(row) == population_key(row) by { }
    assert forall|row: CustomerRow| #[trigger] count(row) == ind(scalar_1_where_clause(db, parameters, row)) by { }
    assert forall|row: CustomerRow| #[trigger] total(row) ==
        if scalar_1_where_clause(db, parameters, row) { row.c_acctbal } else { 0int } by { }
    assert forall|code: u32| #[trigger] per_count(code) ==
        sum(db.customer, |row: CustomerRow| if key(row) == code { count(row) } else { 0int }) by {
        lemma_sum_ext_all(db.customer,
            |row: CustomerRow| if population_key(row) == code {
                ind(scalar_1_where_clause(db, parameters, row)) } else { 0int },
            |row: CustomerRow| if key(row) == code { count(row) } else { 0int });
    }
    assert forall|code: u32| #[trigger] per_total(code) ==
        sum(db.customer, |row: CustomerRow| if key(row) == code { total(row) } else { 0int }) by {
        lemma_sum_ext_all(db.customer,
            |row: CustomerRow| if population_key(row) == code {
                if scalar_1_where_clause(db, parameters, row) { row.c_acctbal } else { 0int }
            } else { 0int },
            |row: CustomerRow| if key(row) == code { total(row) } else { 0int });
    }
    lemma_sum_group(db.customer, codes, key, count, per_count);
    lemma_sum_group(db.customer, codes, key, total, per_total);
    assert forall|index: int| 0 <= index < codes.len() implies
        (#[trigger] positive.value(codes[index] as u128)).0 == population_count_per_code(db, parameters, codes[index]) &&
        positive.value(codes[index] as u128).1 == population_total_per_code(db, parameters, codes[index]) by {
        let code = codes[index];
        assert(codes.contains(code));
        assert(positive.value(code as u128) == positive_moment(db, code));
        assert forall|row: CustomerRow| #[trigger] positive_weight(row, code) == (population_key(row) == code && scalar_1_where_clause(db, parameters, row)) by {
            lemma_codes(codes, parameters, row.c_phone);
            lemma_prefix_model(row.c_phone);
            lemma_country_injective(population_key(row), code);
        }
        let selected_count = |row: CustomerRow| if population_key(row) == code {
            ind(scalar_1_where_clause(db, parameters, row))
        } else { 0int };
        let selected_total = |row: CustomerRow| if population_key(row) == code {
            if scalar_1_where_clause(db, parameters, row) { row.c_acctbal } else { 0int }
        } else { 0int };
        let weight_count = |row: CustomerRow| ind(positive_weight(row, code));
        let weight_total = |row: CustomerRow| if positive_weight(row, code) { row.c_acctbal } else { 0int };
        let quotient_count = |row: CustomerRow| if population_key(row) == code { ind(scalar_1_where_clause(db, parameters, row)) } else { 0int };
        let quotient_total = |row: CustomerRow| if population_key(row) == code { if scalar_1_where_clause(db, parameters, row) { row.c_acctbal } else { 0int } } else { 0int };
        lemma_sum_ext_all(db.customer, quotient_count, selected_count);
        lemma_sum_ext_all(db.customer, quotient_total, selected_total);
        lemma_sum_ext_all(db.customer, selected_count,
            |row: CustomerRow| if population_key(row) == code { count(row) } else { 0int });
        lemma_sum_ext_all(db.customer, selected_total,
            |row: CustomerRow| if population_key(row) == code { total(row) } else { 0int });
        lemma_sum_ext_all(db.customer, weight_count, selected_count);
        lemma_sum_ext_all(db.customer, weight_total, selected_total);
        lemma_moment_weights(db.customer, |row: CustomerRow| positive_weight(row, code), selected_count, selected_total);
        assert(positive_moment(db, code) == (sum(db.customer, selected_count), sum(db.customer, selected_total)));
        assert(per_count(code) == sum(db.customer, selected_count));
        assert(per_total(code) == sum(db.customer, selected_total));
        assert(positive.value(code as u128).0 == exact_count(code));
        assert(positive.value(code as u128).1 == exact_total(code));
    }
    lemma_sum_ext(codes, |code: u32| positive.value(code as u128).0, exact_count);
    lemma_sum_ext(codes, |code: u32| positive.value(code as u128).1, exact_total);
    lemma_sum_ext(codes, per_count, exact_count);
    lemma_sum_ext(codes, per_total, exact_total);
    lemma_scalar_moment(db, parameters);
    lemma_moment_weights(db.customer, |row: CustomerRow| scalar_1_where_clause(db, parameters, row), count, total);
}

pub proof fn lemma_empty_result(db: Db, parameters: Params)
    requires scalar_1_group_count(db, parameters) == 0,
    ensures query_result(db, parameters, Seq::empty()),
{
    let scalar = |row: CustomerRow| ind(scalar_1_where_clause(db, parameters, row));
    let one = |entry: Scalar1Tuple| 1int;
    lemma_sum_one(scalar_1_tuples(db, parameters));
    lemma_scalar_sum(db, parameters, one);
    lemma_sum_ext_all(db.customer,
        |row: CustomerRow| if scalar_1_where_clause(db, parameters, row) { one(Scalar1Tuple { customer_2: row }) } else { 0int },
        scalar);
    assert(sum(db.customer, scalar) == 0);
    assert forall|index: int| 0 <= index < db.customer.len() implies scalar(#[trigger] db.customer[index]) == 0 by {
        if scalar(db.customer[index]) > 0 { lemma_sum_pos(db.customer, scalar, index); }
    }
    assert(scalar_1_value(db, parameters) is None);
    assert forall|row: CustomerRow| !#[trigger] custsale_where_clause(db, parameters, row) by { }
    assert forall|text: Seq<u8>| #[trigger] group_count(db, parameters, text) == 0 by {
        let predicate = |row: CustomerRow| custsale_where_clause(db, parameters, row) && sql_substr(row.c_phone, 1, 2) == text;
        lemma_sum_zero(db.customer, |row: CustomerRow| ind(predicate(row)));
        lemma_sum_zero(db.customer, |row: CustomerRow| if predicate(row) { row.c_acctbal } else { 0int });
        assert(moment_sum(db.customer, predicate) == (0, 0));
        lemma_group_moment(db, parameters, text);
    }
}

pub proof fn lemma_dyadic_step(balance: u64, cutoff: u64, height: u32)
    requires balance < 1125899906842624u64, cutoff < 1125899906842624u64, height < 50,
    ensures (balance > cutoff && (balance >> ((height + 1) as u32)) == (cutoff >> ((height + 1) as u32))) <==>
        (balance > cutoff && (balance >> height) == (cutoff >> height)) ||
        (((cutoff >> height) & 1) == 0 && (balance >> height) == (((cutoff >> height) + 1) as u64)),
        !((balance > cutoff && (balance >> height) == (cutoff >> height)) &&
            (((cutoff >> height) & 1) == 0 && (balance >> height) == (((cutoff >> height) + 1) as u64))),
{
    assert((balance > cutoff && (balance >> ((height + 1) as u32)) == (cutoff >> ((height + 1) as u32))) <==>
        (balance > cutoff && (balance >> height) == (cutoff >> height)) ||
        (((cutoff >> height) & 1) == 0 && (balance >> height) == (((cutoff >> height) + 1) as u64))) by (bit_vector)
        requires balance < 1125899906842624u64, cutoff < 1125899906842624u64, height < 50;
    assert(!((balance > cutoff && (balance >> height) == (cutoff >> height)) &&
        (((cutoff >> height) & 1) == 0 && (balance >> height) == (((cutoff >> height) + 1) as u64)))) by (bit_vector)
        requires balance < 1125899906842624u64, cutoff < 1125899906842624u64, height < 50;
}

pub open spec fn dyadic_indicator(balance: u64, cutoff: u64, height: int) -> int {
    ind((((cutoff >> (height as u32)) & 1) == 0) && (balance >> (height as u32)) == (((cutoff >> (height as u32)) + 1) as u64))
}

pub proof fn lemma_dyadic_prefix(balance: u64, cutoff: u64, heights: u32)
    requires balance < 1125899906842624u64, cutoff < 1125899906842624u64, heights <= 50,
    ensures sum(rng(heights as int), |height: int| dyadic_indicator(balance, cutoff, height)) ==
        ind(balance > cutoff && (balance >> heights) == (cutoff >> heights)),
    decreases heights,
{
    if heights == 0 { assert((balance >> 0) == balance && (cutoff >> 0) == cutoff) by (bit_vector); }
    else {
        lemma_dyadic_prefix(balance, cutoff, (heights - 1) as u32);
        lemma_dyadic_step(balance, cutoff, (heights - 1) as u32);
        assert(rng(heights as int) =~= rng(heights as int - 1).push(heights as int - 1));
        lemma_sum_push(rng(heights as int - 1), heights as int - 1, |height: int| dyadic_indicator(balance, cutoff, height));
    }
}

pub proof fn lemma_dyadic_coverage(balance: u64, cutoff: u64)
    requires balance < 1125899906842624u64, cutoff < 1125899906842624u64,
    ensures sum(rng(50), |height: int| dyadic_indicator(balance, cutoff, height)) == ind(balance > cutoff),
{
    lemma_dyadic_prefix(balance, cutoff, 50);
    assert((balance >> 50) == 0 && (cutoff >> 50) == 0) by (bit_vector)
        requires balance < 1125899906842624u64, cutoff < 1125899906842624u64;
}

pub open spec fn suffix_predicate(db: Db, row: CustomerRow, code: u32, threshold: i128) -> bool {
    positive_weight(row, code) && !exists_1(db, row) && row.c_acctbal > threshold
}

pub proof fn lemma_suffix_probe(cutoff: u64, height: u32)
    requires cutoff < 1125899906842623u64, height < 50, ((cutoff >> height) & 1) == 0,
    ensures (((cutoff >> height) + 1) as u64) < (1u64 << ((50 - height) as u32)),
{
    assert((((cutoff >> height) + 1) as u64) < (1u64 << ((50 - height) as u32))) by (bit_vector)
        requires cutoff < 1125899906842623u64, height < 50, ((cutoff >> height) & 1) == 0;
}

pub proof fn lemma_suffix_correct(db: Db, histogram: Pages, code: u32, threshold: i128)
    requires code <= 65792, threshold >= 0,
        forall|index: int| 0 <= index < db.customer.len() ==> (#[trigger] db.customer[index]).c_acctbal < LIMIT,
        forall|country: u32, depth: u32, interval: u64| country <= 65792 && depth <= 50 && interval < (1u64 << depth) ==>
            #[trigger] histogram.value(histogram_key(country, depth, interval)) == histogram_moment(db, country, depth, interval),
    ensures suffix_moment(histogram, code, threshold, 50) ==
        moment_sum(db.customer, |row: CustomerRow| suffix_predicate(db, row, code, threshold)),
{
    let predicate = |row: CustomerRow| suffix_predicate(db, row, code, threshold);
    if threshold >= LIMIT - 1 {
        lemma_sum_zero(db.customer, |row: CustomerRow| ind(predicate(row)));
        lemma_sum_zero(db.customer, |row: CustomerRow| if predicate(row) { row.c_acctbal } else { 0int });
        return;
    }
    let cutoff = threshold as u64;
    let active = |height: int| 0 <= height < 50 && (((cutoff >> (height as u32)) & 1) == 0);
    let depth = |height: int| (50 - height) as u32;
    let interval = |height: int| (((cutoff >> (height as u32)) + 1) as u64);
    let cell_count = |height: int, row: CustomerRow| if active(height) {
        ind(histogram_weight(db, row, code, depth(height), interval(height)))
    } else { 0int };
    let cell_total = |height: int, row: CustomerRow| if active(height) && histogram_weight(db, row, code, depth(height), interval(height)) {
        row.c_acctbal
    } else { 0int };
    let per_count = |height: int| if active(height) { histogram.value(histogram_key(code, depth(height), interval(height))).0 } else { 0int };
    let per_total = |height: int| if active(height) { histogram.value(histogram_key(code, depth(height), interval(height))).1 } else { 0int };
    let row_count = |row: CustomerRow| sum(rng(50), |height: int| cell_count(height, row));
    let row_total = |row: CustomerRow| sum(rng(50), |height: int| cell_total(height, row));
    assert forall|height: int| #[trigger] per_count(height) == sum(db.customer, |row: CustomerRow| cell_count(height, row)) by {
        if active(height) {
            lemma_suffix_probe(cutoff, height as u32);
            assert(histogram.value(histogram_key(code, depth(height), interval(height))) == histogram_moment(db, code, depth(height), interval(height)));
            lemma_moment_weights(db.customer, |row: CustomerRow| histogram_weight(db, row, code, depth(height), interval(height)),
                |row: CustomerRow| cell_count(height, row), |row: CustomerRow| cell_total(height, row));
        } else { lemma_sum_zero(db.customer, |row: CustomerRow| cell_count(height, row)); }
    }
    assert forall|height: int| #[trigger] per_total(height) == sum(db.customer, |row: CustomerRow| cell_total(height, row)) by {
        if active(height) {
            lemma_suffix_probe(cutoff, height as u32);
            assert(histogram.value(histogram_key(code, depth(height), interval(height))) == histogram_moment(db, code, depth(height), interval(height)));
            lemma_moment_weights(db.customer, |row: CustomerRow| histogram_weight(db, row, code, depth(height), interval(height)),
                |row: CustomerRow| cell_count(height, row), |row: CustomerRow| cell_total(height, row));
        } else { lemma_sum_zero(db.customer, |row: CustomerRow| cell_total(height, row)); }
    }
    lemma_sum_swap(rng(50), db.customer, cell_count, per_count, row_count);
    lemma_sum_swap(rng(50), db.customer, cell_total, per_total, row_total);
    assert forall|index: int| 0 <= index < db.customer.len() implies
        row_count(#[trigger] db.customer[index]) == ind(predicate(db.customer[index])) by {
        let row = db.customer[index];
        if positive_weight(row, code) && !exists_1(db, row) {
            let balance = row.c_acctbal as u64;
            assert(0 < balance < 1125899906842624u64);
            lemma_dyadic_coverage(balance, cutoff);
            lemma_sum_ext(rng(50), |height: int| cell_count(height, row), |height: int| dyadic_indicator(balance, cutoff, height));
        } else { lemma_sum_zero(rng(50), |height: int| cell_count(height, row)); }
    }
    assert forall|index: int| 0 <= index < db.customer.len() implies
        row_total(#[trigger] db.customer[index]) == if predicate(db.customer[index]) { db.customer[index].c_acctbal } else { 0int } by {
        let row = db.customer[index];
        lemma_sum_scale(rng(50), |height: int| cell_count(height, row), |height: int| cell_total(height, row), row.c_acctbal);
        assert(row_count(row) == ind(predicate(row)));
    }
    lemma_sum_ext(db.customer, row_count, |row: CustomerRow| ind(predicate(row)));
    lemma_sum_ext(db.customer, row_total, |row: CustomerRow| if predicate(row) { row.c_acctbal } else { 0int });
    lemma_sum_ext(rng(50), |height: int| if (((threshold as u64) >> (height as u64)) & 1) == 0 {
        histogram.value(histogram_key(code, (50 - height) as u32, ((((threshold as u64) >> (height as u64)) + 1) as u64))).0
    } else { 0int }, per_count);
    lemma_sum_ext(rng(50), |height: int| if (((threshold as u64) >> (height as u64)) & 1) == 0 {
        histogram.value(histogram_key(code, (50 - height) as u32, ((((threshold as u64) >> (height as u64)) + 1) as u64))).1
    } else { 0int }, per_total);
}

pub proof fn lemma_positive_scale(left: int, right: int, scale: int)
    requires scale > 0,
    ensures left * scale < right * scale <==> left < right,
{
    assert(left * scale < right * scale <==> left < right) by (nonlinear_arith) requires scale > 0, left < right || left >= right;
}

pub proof fn lemma_average_cutoff(total: int, count: int, balance: int)
    requires total > 0, count > 0,
    ensures ratio_lt(total, 100 * count, balance, 100) <==> balance > total / count,
{
    let quotient = total / count;
    let remainder = total % count;
    assert(quotient == total / count);
    lemma_fundamental_div_mod(total, count);
    assert(total == quotient * count + remainder);
    assert(0 <= remainder && remainder < count);
    assert(quotient * count <= total && total < quotient * count + count) by (nonlinear_arith)
        requires count > 0, total == quotient * count + remainder, 0 <= remainder && remainder < count;
    assert(ratio_lt(total, 100 * count, balance, 100) <==> balance * count > total) by (nonlinear_arith)
        requires count > 0, total > 0;
    assert(balance * count > total <==> balance > quotient) by (nonlinear_arith)
        requires count > 0, quotient * count <= total, total < quotient * count + count;
}

pub proof fn lemma_suffix_group(db: Db, histogram: Pages, code: u32, threshold: i128,
    parameters: Params, total: int, count: int)
    requires code <= 65792, count > 0, total > 0, threshold as int == total / count,
        scalar_1_group_count(db, parameters) == count,
        scalar_1_group_sum(db, parameters, |entry: Scalar1Tuple| entry.customer_2.c_acctbal) == total,
        matches_country(parameters, country(code)),
        forall|index: int| 0 <= index < db.customer.len() ==> (#[trigger] db.customer[index]).c_acctbal < LIMIT,
        forall|country: u32, depth: u32, interval: u64| country <= 65792 && depth <= 50 && interval < (1u64 << depth) ==>
            #[trigger] histogram.value(histogram_key(country, depth, interval)) == histogram_moment(db, country, depth, interval),
    ensures suffix_moment(histogram, code, threshold, 50) ==
        (group_count(db, parameters, country(code)), group_sum(db, parameters, country(code), |entry: Tuple| entry.custsale.c_acctbal)),
{
    assert(threshold >= 0);
    lemma_suffix_correct(db, histogram, code, threshold);
    lemma_group_moment(db, parameters, country(code));
    let first = |row: CustomerRow| suffix_predicate(db, row, code, threshold);
    let second = |row: CustomerRow| custsale_where_clause(db, parameters, row) && sql_substr(row.c_phone, 1, 2) == country(code);
    assert forall|index: int| 0 <= index < db.customer.len() implies first(#[trigger] db.customer[index]) == second(db.customer[index]) by {
        let row = db.customer[index];
        lemma_average_cutoff(total, count, row.c_acctbal);
    }
    lemma_moment_ext(db.customer, first, second);
}

pub open spec fn output_part(db: Db, parameters: Params, histogram: Pages,
    rows: Seq<(Vec<u8>, i64, i128)>, codes: Seq<u32>, processed: int, threshold: i128) -> bool {
    let out = out_view(rows);
    &&& forall|i: int| 0 <= i < out.len() ==>
        row_ok(db, parameters, #[trigger] out[i])
    &&& forall|i: int, j: int| 0 <= i < j < out.len() ==>
        !same_group(#[trigger] out[i], #[trigger] out[j]) && !row_lt(out[j], out[i])
    &&& forall|i: int, position: int| 0 <= i < out.len() && processed <= position < codes.len() ==>
        (#[trigger] out[i]).cntrycode != #[trigger] country(codes[position])
    &&& forall|code: u32| #![trigger suffix_moment(histogram, code, threshold, 50).0]
        codes.take(processed).contains(code) && suffix_moment(histogram, code, threshold, 50).0 > 0 ==>
        exists|i: int| 0 <= i < out.len() && out[i].cntrycode == country(code)
}

pub proof fn lemma_output_country_distinct(db: Db, parameters: Params, histogram: Pages,
    rows: Seq<(Vec<u8>, i64, i128)>, codes: Seq<u32>, processed: int, threshold: i128, code: u32)
    requires
        output_part(db, parameters, histogram, rows, codes, processed, threshold),
        codes.no_duplicates(),
        0 <= processed < codes.len(),
        code == codes[processed],
        forall|index: int| 0 <= index < codes.len() ==> (#[trigger] codes[index]) <= 65792,
    ensures
        forall|i: int| 0 <= i < out_view(rows).len() ==>
            (#[trigger] out_view(rows)[i]).cntrycode != country(code),
{
    let out = out_view(rows);
    assert forall|i: int| 0 <= i < out.len() implies
        out[i].cntrycode != country(code) by {
        assert(codes[processed] == code);
    }
}

pub proof fn lemma_output_tail(db: Db, parameters: Params, histogram: Pages,
    rows: Seq<(Vec<u8>, i64, i128)>, codes: Seq<u32>, processed: int, threshold: i128,
    position: int, text: Seq<u8>)
    requires
        output_part(db, parameters, histogram, rows, codes, processed, threshold),
        0 <= position < rows.len(),
        bytes_lt(text, (#[trigger] out_view(rows)[position]).cntrycode),
    ensures
        forall|i: int| position <= i < rows.len() ==>
            bytes_lt(text, (#[trigger] out_view(rows)[i]).cntrycode),
{
    let out = out_view(rows);
    assert forall|i: int| position <= i < rows.len() implies
        bytes_lt(text, (#[trigger] out[i]).cntrycode) by {
        if i == position {
            assert(bytes_lt(text, out[i].cntrycode));
        } else {
            assert(!same_group(out[position], out[i]));
            assert(!row_lt(out[i], out[position]));
            lemma_bytes_lt_total(out[position].cntrycode, out[i].cntrycode);
            assert(bytes_lt(out[position].cntrycode, out[i].cntrycode));
            lemma_bytes_lt_trans(text, out[position].cntrycode, out[i].cntrycode);
        }
    }
}

pub proof fn lemma_sorted_tail(out: Seq<OutRow>, position: int, text: Seq<u8>)
    requires
        0 <= position < out.len(),
        bytes_lt(text, (#[trigger] out[position]).cntrycode),
        forall|i: int, j: int| 0 <= i < j < out.len() ==>
            !same_group(#[trigger] out[i], #[trigger] out[j]) && !row_lt(out[j], out[i]),
    ensures
        forall|i: int| position <= i < out.len() ==>
            bytes_lt(text, (#[trigger] out[i]).cntrycode),
{
    assert forall|i: int| position <= i < out.len() implies
        bytes_lt(text, (#[trigger] out[i]).cntrycode) by {
        if i == position {
            assert(bytes_lt(text, out[i].cntrycode));
        } else {
            assert(!same_group(out[position], out[i]));
            assert(!row_lt(out[i], out[position]));
            lemma_bytes_lt_total(out[position].cntrycode, out[i].cntrycode);
            assert(bytes_lt(out[position].cntrycode, out[i].cntrycode));
            lemma_bytes_lt_trans(text, out[position].cntrycode, out[i].cntrycode);
        }
    }
}

pub proof fn lemma_output_insert_view(db: Db, parameters: Params, histogram: Pages,
    old_rows: Seq<(Vec<u8>, i64, i128)>, old_out: Seq<OutRow>,
    codes: Seq<u32>, processed: int, threshold: i128,
    code: u32, source: (Vec<u8>, i64, i128), inserted: OutRow,
    position: int, new_rows: Seq<(Vec<u8>, i64, i128)>)
    requires
        out_view(old_rows) == old_out,
        source.0@ == inserted.cntrycode,
        source.1 as int == inserted.numcust,
        source.2 as int == inserted.totacctbal,
        new_rows == old_rows.insert(position, source),
        out_view(new_rows) == old_out.insert(position, inserted),
        0 <= processed < codes.len(), code == codes[processed],
        codes.no_duplicates(),
        forall|index: int| 0 <= index < codes.len() ==> (#[trigger] codes[index]) <= 65792,
        inserted.cntrycode == country(code),
        inserted.numcust > 0,
        inserted.numcust == suffix_moment(histogram, code, threshold, 50).0,
        inserted.totacctbal == suffix_moment(histogram, code, threshold, 50).1,
        row_ok(db, parameters, inserted),
        0 <= position <= old_out.len(),
        forall|i: int| 0 <= i < old_out.len() ==>
            row_ok(db, parameters, #[trigger] old_out[i]),
        forall|i: int, j: int| 0 <= i < j < old_out.len() ==>
            !same_group(#[trigger] old_out[i], #[trigger] old_out[j]) && !row_lt(old_out[j], old_out[i]),
        forall|i: int| 0 <= i < position ==>
            bytes_lt((#[trigger] old_out[i]).cntrycode, inserted.cntrycode),
        forall|i: int| position <= i < old_out.len() ==>
            bytes_lt(inserted.cntrycode, (#[trigger] old_out[i]).cntrycode),
        forall|i: int, code_index: int| 0 <= i < old_out.len() && processed <= code_index < codes.len() ==>
            (#[trigger] old_out[i]).cntrycode != #[trigger] country(codes[code_index]),
        forall|other: u32| #![trigger suffix_moment(histogram, other, threshold, 50).0]
            codes.take(processed).contains(other) && suffix_moment(histogram, other, threshold, 50).0 > 0 ==>
            exists|i: int| 0 <= i < old_out.len() && old_out[i].cntrycode == country(other),
    ensures
        output_part(db, parameters, histogram, new_rows, codes, processed + 1, threshold),
{
    let out = old_out.insert(position, inserted);
    lemma_output_rows_view(db, parameters, old_out, inserted, position);
    lemma_output_sorted_view(old_out, inserted, position);
    lemma_output_future_distinct_view(old_out, inserted, codes, processed, code, position);
    lemma_output_coverage_view(histogram, codes, processed, threshold,
        code, old_out, inserted, position);
    assert(output_part(db, parameters, histogram, new_rows, codes, processed + 1, threshold));
}

pub proof fn lemma_output_rows_view(db: Db, parameters: Params, old_out: Seq<OutRow>,
    inserted: OutRow, position: int)
    requires row_ok(db, parameters, inserted), 0 <= position <= old_out.len(),
        forall|i: int| 0 <= i < old_out.len() ==>
            row_ok(db, parameters, #[trigger] old_out[i]),
    ensures forall|i: int| 0 <= i < old_out.insert(position, inserted).len() ==>
        row_ok(db, parameters, #[trigger] old_out.insert(position, inserted)[i]),
{
    let out = old_out.insert(position, inserted);
    assert forall|i: int| 0 <= i < out.len() implies row_ok(db, parameters, #[trigger] out[i]) by {
        if i < position { assert(out[i] == old_out[i]); }
        else if i == position { assert(out[i] == inserted); }
        else { assert(out[i] == old_out[i - 1]); }
    }
}

pub proof fn lemma_output_sorted_view(old_out: Seq<OutRow>, inserted: OutRow, position: int)
    requires 0 <= position <= old_out.len(),
        forall|i: int, j: int| 0 <= i < j < old_out.len() ==>
            !same_group(#[trigger] old_out[i], #[trigger] old_out[j]) && !row_lt(old_out[j], old_out[i]),
        forall|i: int| 0 <= i < position ==>
            bytes_lt((#[trigger] old_out[i]).cntrycode, inserted.cntrycode),
        forall|i: int| position <= i < old_out.len() ==>
            bytes_lt(inserted.cntrycode, (#[trigger] old_out[i]).cntrycode),
    ensures forall|i: int, j: int| 0 <= i < j < old_out.insert(position, inserted).len() ==>
        !same_group(#[trigger] old_out.insert(position, inserted)[i],
            #[trigger] old_out.insert(position, inserted)[j]) &&
        !row_lt(old_out.insert(position, inserted)[j], old_out.insert(position, inserted)[i]),
{
    let out = old_out.insert(position, inserted);
    assert forall|i: int, j: int| 0 <= i < j < out.len() implies
        !same_group(#[trigger] out[i], #[trigger] out[j]) && !row_lt(out[j], out[i]) by {
        if i < position && j < position {
            assert(!same_group(old_out[i], old_out[j]) && !row_lt(old_out[j], old_out[i]));
        } else if i < position && j == position {
            lemma_bytes_lt_asym(inserted.cntrycode, old_out[i].cntrycode);
        } else if i < position && j > position {
            assert(!same_group(old_out[i], old_out[j - 1]) && !row_lt(old_out[j - 1], old_out[i]));
        } else if i == position && j > position {
            lemma_bytes_lt_asym(old_out[j - 1].cntrycode, inserted.cntrycode);
        } else {
            assert(!same_group(old_out[i - 1], old_out[j - 1]) && !row_lt(old_out[j - 1], old_out[i - 1]));
        }
    }
}

pub proof fn lemma_output_future_distinct_view(old_out: Seq<OutRow>, inserted: OutRow,
    codes: Seq<u32>, processed: int, code: u32, position: int)
    requires
        0 <= processed < codes.len(), code == codes[processed],
        inserted.cntrycode == country(code),
        0 <= position <= old_out.len(),
        codes.no_duplicates(),
        forall|index: int| 0 <= index < codes.len() ==> (#[trigger] codes[index]) <= 65792,
        forall|i: int, code_index: int| 0 <= i < old_out.len() && processed <= code_index < codes.len() ==>
            (#[trigger] old_out[i]).cntrycode != #[trigger] country(codes[code_index]),
    ensures
        forall|i: int, code_index: int|
            0 <= i < old_out.insert(position, inserted).len() &&
            processed + 1 <= code_index < codes.len() ==>
            (#[trigger] old_out.insert(position, inserted)[i]).cntrycode !=
                #[trigger] country(codes[code_index]),
{
    let out = old_out.insert(position, inserted);
    assert forall|i: int, code_index: int|
        0 <= i < out.len() && processed + 1 <= code_index < codes.len() implies
        (#[trigger] out[i]).cntrycode != #[trigger] country(codes[code_index]) by {
        if i == position {
            assert(codes[code_index] != code);
            lemma_country_injective(code, codes[code_index]);
        }
    }
}

pub proof fn lemma_output_coverage_view(histogram: Pages, codes: Seq<u32>,
    processed: int, threshold: i128, code: u32, old_out: Seq<OutRow>,
    inserted: OutRow, position: int)
    requires 0 <= processed < codes.len(), code == codes[processed],
        inserted.cntrycode == country(code), inserted.numcust > 0,
        inserted.numcust == suffix_moment(histogram, code, threshold, 50).0,
        inserted.totacctbal == suffix_moment(histogram, code, threshold, 50).1,
        0 <= position <= old_out.len(),
        forall|other: u32| #![trigger suffix_moment(histogram, other, threshold, 50).0]
            codes.take(processed).contains(other) && suffix_moment(histogram, other, threshold, 50).0 > 0 ==>
            exists|i: int| 0 <= i < old_out.len() && old_out[i].cntrycode == country(other),
    ensures
        forall|other: u32| #![trigger suffix_moment(histogram, other, threshold, 50).0]
            codes.take(processed + 1).contains(other) && suffix_moment(histogram, other, threshold, 50).0 > 0 ==>
            exists|i: int| 0 <= i < old_out.insert(position, inserted).len() &&
                old_out.insert(position, inserted)[i].cntrycode == country(other),
{
    let out = old_out.insert(position, inserted);
    assert(codes.take(processed + 1) =~= codes.take(processed).push(code));
    assert forall|other: u32| #![trigger suffix_moment(histogram, other, threshold, 50).0]
        codes.take(processed + 1).contains(other) && suffix_moment(histogram, other, threshold, 50).0 > 0 implies
        exists|i: int| 0 <= i < out.len() && out[i].cntrycode == country(other) by {
        if other == code {
            assert(out[position].cntrycode == country(other));
        } else {
            assert(codes.take(processed).contains(other));
            let old_index = choose|i: int| 0 <= i < old_out.len() && old_out[i].cntrycode == country(other);
            if old_index < position { assert(out[old_index].cntrycode == country(other)); }
            else { assert(out[old_index + 1].cntrycode == country(other)); }
        }
    }
}

pub proof fn lemma_execute_result(db: Db, parameters: Params, histogram: Pages,
    rows: Seq<(Vec<u8>, i64, i128)>, codes: Seq<u32>, threshold: i128)
    requires codes_cover(codes, parameters), threshold >= 0,
        scalar_1_group_count(db, parameters) > 0,
        scalar_1_group_sum(db, parameters, |entry: Scalar1Tuple| entry.customer_2.c_acctbal) > 0,
        threshold as int == scalar_1_group_sum(db, parameters,
            |entry: Scalar1Tuple| entry.customer_2.c_acctbal) /
            scalar_1_group_count(db, parameters),
        forall|index: int| 0 <= index < db.customer.len() ==>
            (#[trigger] db.customer[index]).c_acctbal < LIMIT,
        forall|country: u32, depth: u32, interval: u64|
            country <= 65792 && depth <= 50 && interval < (1u64 << depth) ==>
            #[trigger] histogram.value(histogram_key(country, depth, interval)) ==
                histogram_moment(db, country, depth, interval),
        output_part(db, parameters, histogram, rows, codes, codes.len() as int, threshold),
    ensures query_result(db, parameters, out_view(rows)),
{
    let out = out_view(rows);
    let total = scalar_1_group_sum(db, parameters, |entry: Scalar1Tuple| entry.customer_2.c_acctbal);
    let count = scalar_1_group_count(db, parameters);
    assert(count > 0);
    assert(threshold as int == total / count);
    assert forall|g: Seq<u8>| #[trigger] group_count(db, parameters, g) > 0 && having(db, parameters, g) implies
        exists|i: int| 0 <= i < out.len() && (#[trigger] out[i]).cntrycode == g by {
        let row = lemma_group_customer_witness(db, parameters, g);
        lemma_prefix_prefix(row.c_phone);
        let code = prefix_spec(row.c_phone);
        lemma_prefix_model(row.c_phone);
        lemma_prefix_prefix(row.c_phone);
        assert(country(code) == sql_substr(row.c_phone, 1, 2));
        assert(country(code) == g);
        lemma_codes(codes, parameters, row.c_phone);
        assert(codes.contains(code));
        lemma_suffix_group(db, histogram, code, threshold, parameters, total, count);
        assert(suffix_moment(histogram, code, threshold, 50).0 > 0);
        assert(output_part(db, parameters, histogram, rows, codes, codes.len() as int, threshold));
        assert(codes.take(codes.len() as int) =~= codes);
        assert(codes.contains(code));
        assert(exists|i: int| 0 <= i < out.len() && out[i].cntrycode == country(code));
        let i = choose|i: int| 0 <= i < out.len() && out[i].cntrycode == country(code);
        assert(out[i].cntrycode == g);
    }
    lemma_query_result_iff(db, parameters, out);
}

pub proof fn lemma_output_result(db: Db, parameters: Params, histogram: Pages,
    rows: Seq<(Vec<u8>, i64, i128)>, codes: Seq<u32>, threshold: i128)
    requires codes_cover(codes, parameters), threshold >= 0,
        scalar_1_group_count(db, parameters) > 0,
        scalar_1_group_sum(db, parameters, |entry: Scalar1Tuple| entry.customer_2.c_acctbal) > 0,
        threshold as int == scalar_1_group_sum(db, parameters,
            |entry: Scalar1Tuple| entry.customer_2.c_acctbal) /
            scalar_1_group_count(db, parameters),
        forall|index: int| 0 <= index < db.customer.len() ==>
            (#[trigger] db.customer[index]).c_acctbal < LIMIT,
        forall|country: u32, depth: u32, interval: u64|
            country <= 65792 && depth <= 50 && interval < (1u64 << depth) ==>
            #[trigger] histogram.value(histogram_key(country, depth, interval)) ==
                histogram_moment(db, country, depth, interval),
        output_part(db, parameters, histogram, rows, codes, codes.len() as int, threshold),
    ensures query_result(db, parameters, out_view(rows)),
{
    lemma_execute_result(db, parameters, histogram, rows, codes, threshold);
}

pub proof fn lemma_execute_final(db: Db, parameters: Params, histogram: Pages,
    rows: Seq<(Vec<u8>, i64, i128)>, codes: Seq<u32>, threshold: i128)
    requires codes_cover(codes, parameters), threshold >= 0,
        scalar_1_group_count(db, parameters) > 0,
        scalar_1_group_sum(db, parameters, |entry: Scalar1Tuple| entry.customer_2.c_acctbal) > 0,
        output_part(db, parameters, histogram, rows, codes, codes.len() as int, threshold),
        threshold as int == scalar_1_group_sum(db, parameters,
            |entry: Scalar1Tuple| entry.customer_2.c_acctbal) /
            scalar_1_group_count(db, parameters),
        forall|index: int| 0 <= index < db.customer.len() ==>
            (#[trigger] db.customer[index]).c_acctbal < LIMIT,
        forall|country: u32, depth: u32, interval: u64|
            country <= 65792 && depth <= 50 && interval < (1u64 << depth) ==>
            #[trigger] histogram.value(histogram_key(country, depth, interval)) ==
                histogram_moment(db, country, depth, interval),
    ensures query_result(db, parameters, out_view(rows)),
{
    lemma_output_result(db, parameters, histogram, rows, codes, threshold);
}

}
