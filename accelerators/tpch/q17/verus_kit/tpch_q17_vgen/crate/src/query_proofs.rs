use vstd::prelude::*;
use verus_common::sums::*;
use verus_common::rel::*;
use verus_common::order::*;
use verus_common::sqlfns::ratio_lt;
use verus_prooflib::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::model::*;
use crate::storage::*;
use crate::aggregation::*;
use verus_prooflib::wide::Wide;

verus! {

pub proof fn lemma_group_match(groups: Seq<Group>, hash: i64, index: int, brand: Seq<u8>, container: Seq<u8>)
    requires groups_wf(groups, hash), 0 <= index < groups.len(), same(*groups[index].identity, brand, container),
    ensures sum(groups, |group: Group| if same(*group.identity, brand, container) { group.moment.count@ } else { 0 }) == groups[index].moment.count@,
        sum(groups, |group: Group| if same(*group.identity, brand, container) { group.moment.price@ } else { 0 }) == groups[index].moment.price@,
{
    reveal(groups_wf);
    assert forall|other: int| 0 <= other < groups.len() && other != index implies !same(*#[trigger] groups[other].identity, brand, container) by {
        if other < index {
            assert(!same(*groups[other].identity, groups[index].identity.brand@, groups[index].identity.container@));
        } else {
            assert(!same(*groups[index].identity, groups[other].identity.brand@, groups[other].identity.container@));
        }
    }
    lemma_sum_single_match(groups, |group: Group| if same(*group.identity, brand, container) { group.moment.count@ } else { 0int }, index);
    lemma_sum_single_match(groups, |group: Group| if same(*group.identity, brand, container) { group.moment.price@ } else { 0int }, index);
}

pub fn read_group(groups: &Store<Vec<Group>>, brand: &[u8], container: &[u8]) -> (result: Joined)
    requires group_store_wf(*groups),
    ensures result.count@ == group_count_at(*groups, brand@, container@),
        result.price@ == group_price_at(*groups, brand@, container@),
{
    let hash = fingerprint(brand, container);
    if let Some(bucket) = groups.get(hash) {
        proof { assert(groups_wf(groups_at(*groups, hash), hash)); assert(groups_wf(bucket@, hash)); }
        let mut index = 0;
        while index < bucket.len()
            invariant index <= bucket.len(), groups_at(*groups, hash) == bucket@,
                groups_wf(bucket@, hash), hash as int == pair_hash(brand@, container@),
                forall|position: int| #![trigger bucket@[position]] 0 <= position < index ==> !same(*bucket@[position].identity, brand@, container@),
            decreases bucket.len() - index,
        {
            if identity_matches(&bucket[index].identity, brand, container) {
                proof {
                    lemma_group_match(bucket@, hash, index as int, brand@, container@);
                    reveal(group_count_at); reveal(group_price_at); reveal(group_total);
                    assert((|group: Group| if same(*group.identity, brand@, container@) { group_value(group, 1) } else { 0int })
                        =~= (|group: Group| if same(*group.identity, brand@, container@) { group.moment.count@ } else { 0int }));
                    assert((|group: Group| if same(*group.identity, brand@, container@) { group_value(group, 2) } else { 0int })
                        =~= (|group: Group| if same(*group.identity, brand@, container@) { group.moment.price@ } else { 0int }));
                }
                return bucket[index].moment.dup();
            }
            index += 1;
        }
        proof {
            lemma_sum_zero(bucket@, |group: Group| if same(*group.identity, brand@, container@) { group.moment.count@ } else { 0int });
            lemma_sum_zero(bucket@, |group: Group| if same(*group.identity, brand@, container@) { group.moment.price@ } else { 0int });
            reveal(group_count_at); reveal(group_price_at); reveal(group_total);
            lemma_sum_zero(bucket@, |group: Group| if same(*group.identity, brand@, container@) { group_value(group, 1) } else { 0int });
            lemma_sum_zero(bucket@, |group: Group| if same(*group.identity, brand@, container@) { group_value(group, 2) } else { 0int });
        }
    } else {
        proof { reveal(group_count_at); reveal(group_price_at); reveal(group_total); }
    }
    Joined { count: Wide::zero(), price: Wide::zero() }
}

#[verifier::opaque]
pub open spec fn derived_keys(db: Db) -> Seq<int> {
    dedup(db.lineitem.map_values(|row: LineitemRow| row.l_partkey))
}

pub proof fn lemma_aggregate_moments(db: Db, parameter: Params, key: int)
    ensures part_agg_group_count(db, parameter, key) == number(db.lineitem, key),
        part_agg_group_sum(db, parameter, key, |tuple: PartAggTuple| tuple.lineitem_2.l_quantity) == quantity(db.lineitem, key),
{
    hide(part_agg_rows);
    let mapper = |row: LineitemRow| part_agg_tuples_1(db, parameter, row);
    let count_weight = |tuple: PartAggTuple| if tuple.lineitem_2.l_partkey == key { 1int } else { 0int };
    let price_weight = |tuple: PartAggTuple| if tuple.lineitem_2.l_partkey == key { tuple.lineitem_2.l_quantity } else { 0int };
    let count = |row: LineitemRow| ind(row.l_partkey == key);
    let price = |row: LineitemRow| if row.l_partkey == key { row.l_quantity } else { 0int };
    assert forall|row: LineitemRow| #[trigger] count(row) == sum(mapper(row), count_weight) by {
        lemma_sum_opt(true, PartAggTuple { lineitem_2: row }, count_weight);
    }
    assert forall|row: LineitemRow| #[trigger] price(row) == sum(mapper(row), price_weight) by {
        lemma_sum_opt(true, PartAggTuple { lineitem_2: row }, price_weight);
    }
    lemma_flat_map_weight(db.lineitem, mapper, count_weight, count);
    lemma_flat_map_weight(db.lineitem, mapper, price_weight, price);
    assert(price_weight =~= (|tuple: PartAggTuple| if tuple.lineitem_2.l_partkey == key {
        (|entry: PartAggTuple| entry.lineitem_2.l_quantity)(tuple)
    } else { 0int }));
    reveal(number);
    reveal(quantity);
}

pub proof fn lemma_derived_keys(db: Db, parameter: Params)
    ensures part_agg_rows(db, parameter) == derived_keys(db).map_values(|key: int| part_agg_row_of(db, parameter, key)),
        distinct(derived_keys(db)),
        forall|index: int| #![trigger db.lineitem[index]] 0 <= index < db.lineitem.len() ==> derived_keys(db).contains(db.lineitem[index].l_partkey),
{
    let tuple_of = |row: LineitemRow| PartAggTuple { lineitem_2: row };
    lemma_flat_singleton_map(db.lineitem, tuple_of);
    assert((|row: LineitemRow| part_agg_tuples_1(db, parameter, row)) =~= (|row: LineitemRow| seq![tuple_of(row)]));
    assert(part_agg_tuples(db, parameter) == db.lineitem.map_values(tuple_of));
    let tuples = part_agg_tuples(db, parameter);
    lemma_map_where_all(tuples, |tuple: PartAggTuple| part_agg_having(db, parameter, tuple.lineitem_2.l_partkey),
        |tuple: PartAggTuple| tuple.lineitem_2.l_partkey);
    assert(tuples.map_values(|tuple: PartAggTuple| tuple.lineitem_2.l_partkey)
        =~= db.lineitem.map_values(|row: LineitemRow| row.l_partkey));
    let keys = db.lineitem.map_values(|row: LineitemRow| row.l_partkey);
    lemma_dedup(keys);
    reveal(derived_keys);
    assert forall|index: int| #![trigger db.lineitem[index]] 0 <= index < db.lineitem.len()
        implies derived_keys(db).contains(db.lineitem[index].l_partkey) by {
        assert(keys[index] == db.lineitem[index].l_partkey);
        assert(keys.contains(keys[index]));
    }
}

pub proof fn lemma_line_number_positive(rows: Seq<LineitemRow>, row: LineitemRow)
    requires rows.contains(row),
    ensures number(rows, row.l_partkey) > 0,
{
    let position = choose|position: int| 0 <= position < rows.len() && #[trigger] rows[position] == row;
    let weight = |other: LineitemRow| ind(other.l_partkey == row.l_partkey);
    lemma_sum_pos(rows, weight, position);
    reveal(number);
}

pub proof fn lemma_threshold(db: Db, parameter: Params, row: LineitemRow)
    requires db.lineitem.contains(row),
    ensures ratio_lt(row.l_quantity, 100, part_agg_row_of(db, parameter, row.l_partkey).avg_quantity.num,
        part_agg_row_of(db, parameter, row.l_partkey).avg_quantity.den) == qualifies(db.lineitem, row),
{
    lemma_aggregate_moments(db, parameter, row.l_partkey);
    lemma_line_number_positive(db.lineitem, row);
    let count = number(db.lineitem, row.l_partkey);
    let total = quantity(db.lineitem, row.l_partkey);
    lemma_ratio_lt_positive(row.l_quantity, 100, 2 * total, 1000 * count);
    reveal(qualifies);
    assert((row.l_quantity * (1000 * count) < (2 * total) * 100)
        == (5 * count * row.l_quantity < total)) by (nonlinear_arith);
}

pub proof fn lemma_pair_sum(db: Db, parameter: Params, row: LineitemRow, part: PartRow, weight: spec_fn(LineitemRow) -> int)
    requires db.lineitem.contains(row),
    ensures sum(tuples_2(db, parameter, row, part), |tuple: Tuple| weight(tuple.lineitem))
        == if row.l_partkey == part.p_partkey && part.p_brand == parameter.p1 && part.p_container == parameter.p2
            && qualifies(db.lineitem, row) { weight(row) } else { 0 },
{
    hide(tuples);
    hide(tuples_1);
    lemma_derived_keys(db, parameter);
    lemma_threshold(db, parameter, row);
    let keys = derived_keys(db);
    let tuple_weight = |tuple: Tuple| weight(tuple.lineitem);
    let mapper = |aggregate: PartAggRow| tuples_3(db, parameter, row, part, aggregate);
    let per_aggregate = |aggregate: PartAggRow| if where_clause(db, parameter, row, part, aggregate) { weight(row) } else { 0int };
    assert forall|aggregate: PartAggRow| #[trigger] per_aggregate(aggregate) == sum(mapper(aggregate), tuple_weight) by {
        lemma_sum_opt(where_clause(db, parameter, row, part, aggregate), Tuple { lineitem: row, part, part_agg: aggregate }, tuple_weight);
    }
    lemma_flat_map_weight(rel_part_agg(db, parameter), mapper, tuple_weight, per_aggregate);
    let value = if row.l_partkey == part.p_partkey && part.p_brand == parameter.p1 && part.p_container == parameter.p2
            && qualifies(db.lineitem, row) { weight(row) } else { 0int };
    let by_key = |key: int| if key == row.l_partkey { value } else { 0int };
    assert forall|key: int| #[trigger] by_key(key) == per_aggregate(part_agg_row_of(db, parameter, key)) by {
        if key == row.l_partkey {
            assert(ratio_lt(row.l_quantity, 100, part_agg_row_of(db, parameter, key).avg_quantity.num,
                part_agg_row_of(db, parameter, key).avg_quantity.den) == qualifies(db.lineitem, row));
        }
    }
    lemma_sum_map(keys, |key: int| part_agg_row_of(db, parameter, key), per_aggregate, by_key);
    let position = choose|position: int| 0 <= position < db.lineitem.len() && #[trigger] db.lineitem[position] == row;
    assert(keys.contains(db.lineitem[position].l_partkey));
    lemma_sum_indicator_distinct(keys, row.l_partkey, value, by_key);
}

pub proof fn lemma_query_weight(db: Db, parameter: Params, weight: spec_fn(LineitemRow) -> int)
    ensures sum(tuples(db, parameter), |tuple: Tuple| weight(tuple.lineitem))
        == sum(db.part, |part: PartRow| if part.p_brand == parameter.p1 && part.p_container == parameter.p2 {
            sum(db.lineitem, |row: LineitemRow| if row.l_partkey == part.p_partkey && qualifies(db.lineitem, row) { weight(row) } else { 0 })
        } else { 0 }),
{
    let pair_weight = |row: LineitemRow, part: PartRow| if row.l_partkey == part.p_partkey
        && part.p_brand == parameter.p1 && part.p_container == parameter.p2 && qualifies(db.lineitem, row) { weight(row) } else { 0int };
    let tuple_weight = |tuple: Tuple| weight(tuple.lineitem);
    let by_line = |row: LineitemRow| sum(tuples_1(db, parameter, row), tuple_weight);
    lemma_sum_flat_map(db.lineitem, |row: LineitemRow| tuples_1(db, parameter, row), tuple_weight);
    assert forall|index: int| 0 <= index < db.lineitem.len() implies #[trigger] by_line(db.lineitem[index])
        == sum(db.part, |part: PartRow| pair_weight(db.lineitem[index], part)) by {
        let row = db.lineitem[index];
        assert(db.lineitem.contains(row));
        lemma_sum_flat_map(db.part, |part: PartRow| tuples_2(db, parameter, row, part), tuple_weight);
        assert forall|part: PartRow| #[trigger] pair_weight(row, part) == sum(tuples_2(db, parameter, row, part), tuple_weight) by {
            lemma_pair_sum(db, parameter, row, part, weight);
        }
        lemma_sum_ext_all(db.part, |part: PartRow| sum(tuples_2(db, parameter, row, part), tuple_weight), |part: PartRow| pair_weight(row, part));
        assert((|part: PartRow| sum(tuples_2(db, parameter, row, part), tuple_weight))
            =~= (|part: PartRow| sum((|entry: PartRow| tuples_2(db, parameter, row, entry))(part), tuple_weight)));
        assert(tuples_1(db, parameter, row) == flat(db.part.map_values(|part: PartRow| tuples_2(db, parameter, row, part))));
    }
    let outer_line = |row: LineitemRow| sum(db.part, |part: PartRow| pair_weight(row, part));
    lemma_sum_ext(db.lineitem, by_line, outer_line);
    let outer_part = |part: PartRow| sum(db.lineitem, |row: LineitemRow| pair_weight(row, part));
    lemma_sum_swap(db.lineitem, db.part, pair_weight, outer_line, outer_part);
    let answer = |part: PartRow| if part.p_brand == parameter.p1 && part.p_container == parameter.p2 {
        sum(db.lineitem, |row: LineitemRow| if row.l_partkey == part.p_partkey && qualifies(db.lineitem, row) { weight(row) } else { 0int })
    } else { 0int };
    assert forall|part: PartRow| #[trigger] outer_part(part) == answer(part) by {
        if part.p_brand == parameter.p1 && part.p_container == parameter.p2 {
            lemma_sum_ext_all(db.lineitem, |row: LineitemRow| pair_weight(row, part),
                |row: LineitemRow| if row.l_partkey == part.p_partkey && qualifies(db.lineitem, row) { weight(row) } else { 0int });
        } else {
            lemma_sum_zero(db.lineitem, |row: LineitemRow| pair_weight(row, part));
        }
    }
    lemma_sum_ext_all(db.part, outer_part, answer);
    assert((|row: LineitemRow| sum((|entry: LineitemRow| tuples_1(db, parameter, entry))(row), tuple_weight)) =~= by_line);
}

pub proof fn lemma_query_moments(db: Db, parameter: Params)
    ensures group_count(db, parameter) == joined_count(db, parameter.p1, parameter.p2),
        group_sum(db, parameter, |tuple: Tuple| tuple.lineitem.l_extendedprice) == joined_price(db, parameter.p1, parameter.p2),
{
    let count_weight = |row: LineitemRow| 1int;
    let price_weight = |row: LineitemRow| row.l_extendedprice;
    lemma_query_weight(db, parameter, count_weight);
    lemma_query_weight(db, parameter, price_weight);
    lemma_sum_ones(tuples(db, parameter));
    reveal(joined_count);
    reveal(joined_price);
    reveal(line_count);
    reveal(line_price);
    let actual_count = |part: PartRow| if part.p_brand == parameter.p1 && part.p_container == parameter.p2 {
        sum(db.lineitem, |row: LineitemRow| if row.l_partkey == part.p_partkey && qualifies(db.lineitem, row) { count_weight(row) } else { 0int })
    } else { 0int };
    let actual_price = |part: PartRow| if part.p_brand == parameter.p1 && part.p_container == parameter.p2 {
        sum(db.lineitem, |row: LineitemRow| if row.l_partkey == part.p_partkey && qualifies(db.lineitem, row) { price_weight(row) } else { 0int })
    } else { 0int };
    let desired_count = |part: PartRow| if part.p_brand == parameter.p1 && part.p_container == parameter.p2 { line_count(db.lineitem, part.p_partkey) } else { 0int };
    let desired_price = |part: PartRow| if part.p_brand == parameter.p1 && part.p_container == parameter.p2 { line_price(db.lineitem, part.p_partkey) } else { 0int };
    assert forall|part: PartRow| #[trigger] actual_count(part) == desired_count(part) by {
        lemma_sum_ext_all(db.lineitem, |row: LineitemRow| if row.l_partkey == part.p_partkey && qualifies(db.lineitem, row) { count_weight(row) } else { 0int },
            |row: LineitemRow| ind(row.l_partkey == part.p_partkey && qualifies(db.lineitem, row)));
    }
    assert forall|part: PartRow| #[trigger] actual_price(part) == desired_price(part) by {
        lemma_sum_ext_all(db.lineitem, |row: LineitemRow| if row.l_partkey == part.p_partkey && qualifies(db.lineitem, row) { price_weight(row) } else { 0int },
            |row: LineitemRow| if row.l_partkey == part.p_partkey && qualifies(db.lineitem, row) { row.l_extendedprice } else { 0int });
    }
    lemma_sum_ext_all(db.part, actual_count, desired_count);
    lemma_sum_ext_all(db.part, actual_price, desired_price);
    assert((|tuple: Tuple| count_weight(tuple.lineitem)) =~= (|tuple: Tuple| 1int));
    assert((|tuple: Tuple| price_weight(tuple.lineitem)) =~= (|tuple: Tuple| tuple.lineitem.l_extendedprice));
    lemma_sum_ext_all(tuples(db, parameter), |tuple: Tuple| 1int, |tuple: Tuple| (|row: LineitemRow| 1int)(tuple.lineitem));
    assert((|tuple: Tuple| (|row: LineitemRow| row.l_extendedprice)(tuple.lineitem))
        =~= (|tuple: Tuple| tuple.lineitem.l_extendedprice));
    assert forall|part: PartRow| #[trigger] line_count(db.lineitem, part.p_partkey)
        == sum(db.lineitem, |row: LineitemRow| if row.l_partkey == part.p_partkey && qualifies(db.lineitem, row) { 1int } else { 0int }) by {
        lemma_sum_ext_all(db.lineitem, |row: LineitemRow| ind(row.l_partkey == part.p_partkey && qualifies(db.lineitem, row)),
            |row: LineitemRow| if row.l_partkey == part.p_partkey && qualifies(db.lineitem, row) { 1int } else { 0int });
    }
    lemma_sum_ext_all(db.part, |part: PartRow| if part.p_brand == parameter.p1 && part.p_container == parameter.p2 {
        sum(db.lineitem, |row: LineitemRow| if row.l_partkey == part.p_partkey && qualifies(db.lineitem, row) { 1int } else { 0int })
    } else { 0int }, |part: PartRow| if part.p_brand == parameter.p1 && part.p_container == parameter.p2 {
        line_count(db.lineitem, part.p_partkey)
    } else { 0int });
    lemma_sum_ext_all(db.part, |part: PartRow| if part.p_brand == parameter.p1 && part.p_container == parameter.p2 {
        sum(db.lineitem, |row: LineitemRow| if row.l_partkey == part.p_partkey && qualifies(db.lineitem, row) { row.l_extendedprice } else { 0int })
    } else { 0int }, |part: PartRow| if part.p_brand == parameter.p1 && part.p_container == parameter.p2 {
        line_price(db.lineitem, part.p_partkey)
    } else { 0int });
}

pub proof fn lemma_execute_result(db: Db, brand: Seq<u8>, container: Seq<u8>, answer: Option<(i128, i128)>, count: int, price: int)
    requires count == joined_count(db, brand, container), price == joined_price(db, brand, container),
        answer == if count > 0 { Some(((price * 10) as i128, 7000i128)) } else { None },
        count > 0 ==> i128::MIN <= price * 10 <= i128::MAX,
    ensures query_result(db, params(brand, container), out_view(seq![answer])),
{
    let parameter = params(brand, container);
    lemma_query_moments(db, parameter);
    assert(out_view(seq![answer]) =~= rows(db, parameter));
    lemma_query_result_iff(db, parameter, out_view(seq![answer]));
}

pub proof fn lemma_answer_overflow(db: Db, brand: Seq<u8>, container: Seq<u8>, count: int, price: int)
    requires count == joined_count(db, brand, container), price == joined_price(db, brand, container),
        count > 0, !(i128::MIN <= price * 10 <= i128::MAX),
    ensures !answer_fits(db, params(brand, container)),
{
    let parameter = params(brand, container);
    lemma_query_moments(db, parameter);
    lemma_query_result_iff(db, parameter, rows(db, parameter));
    assert(rows(db, parameter)[0] == row_of(db, parameter));
}

}
