use vstd::prelude::*;
use verus_prooflib::wide::Wide;
use std::sync::Arc;
use crate::storage::*;
use crate::values::*;
use crate::model::*;
use verus_common::sums::*;
use verus_prooflib::*;

verus! {
broadcast use vstd::seq_lib::group_seq_properties;

pub fn line_contribution(suppliers: &Layers<Counts>, customers: &Layers<Counts>, orders: &Layers<Counts>,
    changes: &mut Sparse<Groups>, order: i64, supplier: i64, year: i64, volume: &Wide, sign: &Wide)
    requires old(changes).valid(),
    ensures final(changes).valid(),
        sparse_groups_shape(*old(changes)) && (year == 1995 || year == 1996)
            ==> sparse_groups_shape(*final(changes)),
        forall|first: i64, second: i64, target_year: i64, revenue: bool|
            #![trigger sparse_group(*final(changes), first, second, target_year, revenue)]
            sparse_group(*final(changes), first, second, target_year, revenue)
            == sparse_group(*old(changes), first, second, target_year, revenue)
                + sign@ * projected_contribution(*suppliers, *customers, *orders,
                    order, supplier, year, volume@, first, second, target_year, revenue),
{
    let ghost initial = *changes;
    proof { reveal(projected_contribution); }
    if let Some(supplier_bucket) = suppliers.get(supplier) {
        if let Some(order_bucket) = orders.get(order) {
            let mut supplier_index = 0;
            while supplier_index < supplier_bucket.rows.len()
                invariant supplier_index <= supplier_bucket.rows@.len(), changes.valid(), initial == *old(changes),
                    sparse_groups_shape(initial) && (year == 1995 || year == 1996) ==> sparse_groups_shape(*changes),
                    suppliers.at(supplier) == Some(*supplier_bucket), orders.at(order) == Some(*order_bucket),
                    forall|first: i64, second: i64, target_year: i64, revenue: bool|
                        #![trigger sparse_group(*changes, first, second, target_year, revenue)]
                        sparse_group(*changes, first, second, target_year, revenue)
                        == sparse_group(initial, first, second, target_year, revenue)
                            + prefix_product(supplier_bucket.rows@.take(supplier_index as int), order_bucket.rows@, *customers,
                                first, second, target_year, year, volume@, sign@, revenue),
                decreases supplier_bucket.rows@.len() - supplier_index,
            {
                let supplier_ref = &supplier_bucket.rows[supplier_index];
                let ghost supplier_row = *supplier_ref;
                let mut order_index = 0;
                proof {
                    assert(order_bucket.rows@.take(0).len() == 0);
                    assert forall|first: i64, second: i64, target_year: i64, measure: bool|
                        #![trigger prefix_product(seq![supplier_row], order_bucket.rows@.take(0), *customers,
                            first, second, target_year, year, volume@, sign@, measure)]
                        prefix_product(seq![supplier_row], order_bucket.rows@.take(0), *customers,
                            first, second, target_year, year, volume@, sign@, measure) == 0 by {
                        lemma_sum_empty(order_bucket.rows@.take(0), |row: (i64, Wide)| row.1@ * dimension_value(*customers, row.0, second));
                        assert(order_weight(order_bucket.rows@.take(0), *customers, second) == 0);
                        assert(prefix_product(seq![supplier_row], order_bucket.rows@.take(0), *customers,
                            first, second, target_year, year, volume@, sign@, measure)
                            == if target_year != year { 0int } else {
                                count_at(seq![supplier_row], first) * (0int * (sign@ * if measure { volume@ } else { 1int }))
                            });
                    }
                }
                while order_index < order_bucket.rows.len()
                    invariant order_index <= order_bucket.rows@.len(), supplier_index < supplier_bucket.rows@.len(),
                        *supplier_ref == supplier_row,
                        sparse_groups_shape(initial) && (year == 1995 || year == 1996) ==> sparse_groups_shape(*changes),
                        changes.valid(), initial == *old(changes),
                        supplier_row == supplier_bucket.rows@[supplier_index as int],
                        suppliers.at(supplier) == Some(*supplier_bucket), orders.at(order) == Some(*order_bucket),
                        forall|first: i64, second: i64, target_year: i64, revenue: bool|
                            #![trigger sparse_group(*changes, first, second, target_year, revenue)]
                            sparse_group(*changes, first, second, target_year, revenue)
                            == sparse_group(initial, first, second, target_year, revenue)
                                + prefix_product(supplier_bucket.rows@.take(supplier_index as int), order_bucket.rows@, *customers,
                                    first, second, target_year, year, volume@, sign@, revenue)
                                + prefix_product(seq![supplier_row], order_bucket.rows@.take(order_index as int), *customers,
                                    first, second, target_year, year, volume@, sign@, revenue),
                    decreases order_bucket.rows@.len() - order_index,
                {
                    let order_ref = &order_bucket.rows[order_index];
                    let ghost order_row = *order_ref;
                    if let Some(customer_bucket) = customers.get(order_ref.0) {
                        let mut customer_index = 0;
                        while customer_index < customer_bucket.rows.len()
                            invariant customer_index <= customer_bucket.rows@.len(), changes.valid(), initial == *old(changes),
                                *supplier_ref == supplier_row, *order_ref == order_row,
                                sparse_groups_shape(initial) && (year == 1995 || year == 1996) ==> sparse_groups_shape(*changes),
                                supplier_index < supplier_bucket.rows@.len(), order_index < order_bucket.rows@.len(),
                                supplier_row == supplier_bucket.rows@[supplier_index as int], order_row == order_bucket.rows@[order_index as int],
                                customers.at(order_row.0) == Some(*customer_bucket),
                                suppliers.at(supplier) == Some(*supplier_bucket), orders.at(order) == Some(*order_bucket),
                                forall|first: i64, second: i64, target_year: i64, revenue: bool|
                                    #![trigger sparse_group(*changes, first, second, target_year, revenue)]
                                    sparse_group(*changes, first, second, target_year, revenue)
                                    == sparse_group(initial, first, second, target_year, revenue)
                                        + prefix_product(supplier_bucket.rows@.take(supplier_index as int), order_bucket.rows@, *customers,
                                            first, second, target_year, year, volume@, sign@, revenue)
                                        + prefix_product(seq![supplier_row], order_bucket.rows@.take(order_index as int), *customers,
                                            first, second, target_year, year, volume@, sign@, revenue)
                                        + customer_piece(customer_bucket.rows@.take(customer_index as int), supplier_row, order_row,
                                            first, second, target_year, year, volume@, sign@, revenue),
                            decreases customer_bucket.rows@.len() - customer_index,
                        {
                            let customer_ref = &customer_bucket.rows[customer_index];
                            let ghost customer_row = *customer_ref;
                            let supplier_order_count = supplier_ref.1.mul(&order_ref.1);
                            let joined_count = supplier_order_count.mul(&customer_ref.1);
                            proof { assert(*supplier_ref == supplier_row); assert(*order_ref == order_row); assert(*customer_ref == customer_row); }
                            let count = joined_count.mul(sign);
                            let revenue = count.mul(volume);
                            proof {
                                assert(supplier_order_count@ == supplier_row.1@ * order_row.1@);
                                assert(joined_count@ == supplier_order_count@ * customer_row.1@);
                                assert(count@ == joined_count@ * sign@);
                            }
                            group_delta(changes, supplier_ref.0, customer_ref.0, year, &count, &revenue);
                            proof {
                                assert forall|first: i64, second: i64, target_year: i64, measure: bool|
                                    #![trigger sparse_group(*changes, first, second, target_year, measure)]
                                    sparse_group(*changes, first, second, target_year, measure)
                                    == sparse_group(initial, first, second, target_year, measure)
                                        + prefix_product(supplier_bucket.rows@.take(supplier_index as int), order_bucket.rows@, *customers,
                                            first, second, target_year, year, volume@, sign@, measure)
                                        + prefix_product(seq![supplier_row], order_bucket.rows@.take(order_index as int), *customers,
                                            first, second, target_year, year, volume@, sign@, measure)
                                        + customer_piece(customer_bucket.rows@.take(customer_index as int + 1), supplier_row, order_row,
                                            first, second, target_year, year, volume@, sign@, measure) by {
                                    lemma_sum_take(customer_bucket.rows@, customer_index as int,
                                        |row: (i64, Wide)| if row.0 == second { row.1@ } else { 0int });
                                    assert(count@ == supplier_row.1@ * order_row.1@ * customer_row.1@ * sign@);
                                    assert(revenue@ == count@ * volume@);
                                    assert(customer_piece(customer_bucket.rows@.take(customer_index as int + 1), supplier_row, order_row,
                                        first, second, target_year, year, volume@, sign@, measure)
                                        == customer_piece(customer_bucket.rows@.take(customer_index as int), supplier_row, order_row,
                                            first, second, target_year, year, volume@, sign@, measure)
                                        + if first == supplier_row.0 && second == customer_row.0 && target_year == year {
                                            if measure { revenue@ } else { count@ }
                                        } else { 0 }) by {
                                            lemma_scalar_distribute(count_at(customer_bucket.rows@.take(customer_index as int), second),
                                                if customer_row.0 == second { customer_row.1@ } else { 0int },
                                                supplier_row.1@ * (order_row.1@ * (sign@ * if measure { volume@ } else { 1int })));
                                            lemma_join_product(supplier_row.1@, order_row.1@, customer_row.1@,
                                                sign@, if measure { volume@ } else { 1int });
                                        }
                                }
                            }
                            customer_index += 1;
                        }
                        proof { assert(customer_bucket.rows@.take(customer_bucket.rows@.len() as int) == customer_bucket.rows@); }
                    }
                    proof {
                        assert forall|first: i64, second: i64, target_year: i64, measure: bool|
                            #![trigger sparse_group(*changes, first, second, target_year, measure)]
                            sparse_group(*changes, first, second, target_year, measure)
                            == sparse_group(initial, first, second, target_year, measure)
                                + prefix_product(supplier_bucket.rows@.take(supplier_index as int), order_bucket.rows@, *customers,
                                    first, second, target_year, year, volume@, sign@, measure)
                                + prefix_product(seq![supplier_row], order_bucket.rows@.take(order_index as int + 1), *customers,
                                    first, second, target_year, year, volume@, sign@, measure) by {
                            lemma_sum_take(order_bucket.rows@, order_index as int,
                                |row: (i64, Wide)| row.1@ * dimension_value(*customers, row.0, second));
                            assert(prefix_product(seq![supplier_row], order_bucket.rows@.take(order_index as int + 1), *customers,
                                first, second, target_year, year, volume@, sign@, measure)
                                == prefix_product(seq![supplier_row], order_bucket.rows@.take(order_index as int), *customers,
                                    first, second, target_year, year, volume@, sign@, measure)
                                + (if first == supplier_row.0 && target_year == year {
                                    supplier_row.1@ * order_row.1@ * dimension_value(*customers, order_row.0, second)
                                        * sign@ * if measure { volume@ } else { 1int }
                                } else { 0int })) by {
                                    lemma_sum_singleton(supplier_row, |row: (i64, Wide)| if row.0 == first { row.1@ } else { 0int });
                                    lemma_scalar_distribute(order_weight(order_bucket.rows@.take(order_index as int), *customers, second),
                                        order_row.1@ * dimension_value(*customers, order_row.0, second),
                                        sign@ * if measure { volume@ } else { 1int });
                                    lemma_left_scalar_distribute(order_weight(order_bucket.rows@.take(order_index as int), *customers, second)
                                            * (sign@ * if measure { volume@ } else { 1int }),
                                        (order_row.1@ * dimension_value(*customers, order_row.0, second))
                                            * (sign@ * if measure { volume@ } else { 1int }),
                                        count_at(seq![supplier_row], first));
                                    lemma_group_coefficient(supplier_row.1@, order_row.1@ * dimension_value(*customers, order_row.0, second),
                                        sign@, if measure { volume@ } else { 1int });
                                    lemma_group_coefficient(supplier_row.1@, order_row.1@, 1int, dimension_value(*customers, order_row.0, second));
                                }
                            if let Some(customer_bucket) = customers.at(order_row.0) {
                                lemma_sum_singleton(supplier_row, |row: (i64, Wide)| if row.0 == first { row.1@ } else { 0int });
                                lemma_join_product(supplier_row.1@, order_row.1@,
                                    count_at(customer_bucket.rows@, second), sign@,
                                    if measure { volume@ } else { 1int });
                            }
                        }
                    }
                    order_index += 1;
                }
                proof {
                    assert forall|first: i64, second: i64, target_year: i64, measure: bool|
                        #![trigger sparse_group(*changes, first, second, target_year, measure)]
                        sparse_group(*changes, first, second, target_year, measure)
                        == sparse_group(initial, first, second, target_year, measure)
                            + prefix_product(supplier_bucket.rows@.take(supplier_index as int + 1), order_bucket.rows@, *customers,
                                first, second, target_year, year, volume@, sign@, measure) by {
                        lemma_sum_take(supplier_bucket.rows@, supplier_index as int,
                            |row: (i64, Wide)| if row.0 == first { row.1@ } else { 0int });
                        assert(order_bucket.rows@.take(order_bucket.rows@.len() as int) == order_bucket.rows@);
                        assert(prefix_product(supplier_bucket.rows@.take(supplier_index as int + 1), order_bucket.rows@, *customers,
                            first, second, target_year, year, volume@, sign@, measure)
                            == prefix_product(supplier_bucket.rows@.take(supplier_index as int), order_bucket.rows@, *customers,
                                first, second, target_year, year, volume@, sign@, measure)
                                + prefix_product(seq![supplier_row], order_bucket.rows@, *customers,
                                    first, second, target_year, year, volume@, sign@, measure)) by {
                                lemma_sum_singleton(supplier_row, |row: (i64, Wide)| if row.0 == first { row.1@ } else { 0int });
                                lemma_scalar_distribute(count_at(supplier_bucket.rows@.take(supplier_index as int), first),
                                    if supplier_row.0 == first { supplier_row.1@ } else { 0int },
                                    order_weight(order_bucket.rows@, *customers, second) * (sign@ * if measure { volume@ } else { 1int }));
                            }
                    }
                }
                supplier_index += 1;
            }
            proof {
                assert(supplier_bucket.rows@.take(supplier_bucket.rows@.len() as int) == supplier_bucket.rows@);
                assert(initial == *old(changes));
                reveal(projected_contribution);
                assert forall|first: i64, second: i64, target_year: i64, measure: bool|
                    #![trigger sparse_group(*changes, first, second, target_year, measure)]
                    sparse_group(*changes, first, second, target_year, measure)
                    == sparse_group(initial, first, second, target_year, measure)
                        + sign@ * projected_contribution(*suppliers, *customers, *orders,
                            order, supplier, year, volume@, first, second, target_year, measure) by {
                    lemma_group_coefficient(count_at(supplier_bucket.rows@, first), order_weight(order_bucket.rows@, *customers, second),
                        sign@, if measure { volume@ } else { 1int });
                }
            }
            return;
        } else {
            proof {
                reveal(projected_contribution);
                assert(orders.at(order).is_none());
                assert(*changes == initial);
                assert forall|first: i64, second: i64, target_year: i64, measure: bool|
                    #![trigger projected_contribution(*suppliers, *customers, *orders,
                        order, supplier, year, volume@, first, second, target_year, measure)]
                    projected_contribution(*suppliers, *customers, *orders,
                        order, supplier, year, volume@, first, second, target_year, measure) == 0 by {
                    lemma_projected_absent(*suppliers, *customers, *orders, order, supplier, year, volume@,
                        first, second, target_year, measure);
                }
            }
            return;
        }
    } else {
        proof {
            reveal(projected_contribution);
            assert(suppliers.at(supplier).is_none());
            assert(*changes == initial);
            assert forall|first: i64, second: i64, target_year: i64, measure: bool|
                #![trigger projected_contribution(*suppliers, *customers, *orders,
                    order, supplier, year, volume@, first, second, target_year, measure)]
            projected_contribution(*suppliers, *customers, *orders,
                    order, supplier, year, volume@, first, second, target_year, measure) == 0 by {
                lemma_projected_absent(*suppliers, *customers, *orders, order, supplier, year, volume@,
                    first, second, target_year, measure);
            }
        }
        return;
    }
}

pub fn expand_cell(nations: &Layers<Names>, old_answers: &Layers<Answers>, changes: &mut Sparse<Answers>,
    first: i64, cell: &Cell, sign: &Wide)
    requires old(changes).valid(),
    ensures final(changes).valid(), forall|left: Seq<u8>, right: Seq<u8>, year: i64, measure: bool|
        #![trigger patched_answer(*old_answers, *final(changes), left, right, year, measure)]
        patched_answer(*old_answers, *final(changes), left, right, year, measure)
            == patched_answer(*old_answers, *old(changes), left, right, year, measure)
                + cell_expansion(*nations, first, *cell, sign@, left, right, year, measure),
        patched_answers_shape(*old_answers, *old(changes)) && (cell.year == 1995 || cell.year == 1996)
            ==> patched_answers_shape(*old_answers, *final(changes)),
{
    let ghost initial = *changes;
    if let Some(first_names) = nations.get(first) {
        if let Some(second_names) = nations.get(cell.other) {
            let mut first_index = 0;
            while first_index < first_names.rows.len()
                invariant first_index <= first_names.rows@.len(), changes.valid(), initial == *old(changes),
                    patched_answers_shape(*old_answers, initial) && (cell.year == 1995 || cell.year == 1996)
                        ==> patched_answers_shape(*old_answers, *changes),
                    nations.at(first) == Some(*first_names), nations.at(cell.other) == Some(*second_names),
                    forall|left: Seq<u8>, right: Seq<u8>, year: i64, measure: bool|
                        #![trigger patched_answer(*old_answers, *changes, left, right, year, measure)]
                        patched_answer(*old_answers, *changes, left, right, year, measure)
                            == patched_answer(*old_answers, initial, left, right, year, measure)
                                + expanded_prefix(first_names.rows@.take(first_index as int), second_names.rows@,
                                    *cell, sign@, left, right, year, measure),
                decreases first_names.rows@.len() - first_index,
            {
                let mut second_index = 0;
                proof {
                    assert(second_names.rows@.take(0).len() == 0);
                    assert forall|left: Seq<u8>, right: Seq<u8>, year: i64, measure: bool|
                        #![trigger expanded_prefix(seq![first_names.rows@[first_index as int]], second_names.rows@.take(0),
                            *cell, sign@, left, right, year, measure)]
                        expanded_prefix(seq![first_names.rows@[first_index as int]], second_names.rows@.take(0),
                            *cell, sign@, left, right, year, measure) == 0 by {
                        lemma_sum_empty(second_names.rows@.take(0), |row: (Vec<u8>, Wide)|
                            if row.0@ == right { row.1@ } else { 0int });
                        assert(name_at(second_names.rows@.take(0), right) == 0);
                        let first_count = name_at(seq![first_names.rows@[first_index as int]], left);
                        let value = sign@ * if measure { cell.revenue@ } else { cell.count@ };
                        assert(first_count * (0int * value) == 0) by (nonlinear_arith);
                        assert(expanded_prefix(seq![first_names.rows@[first_index as int]], second_names.rows@.take(0),
                            *cell, sign@, left, right, year, measure)
                            == if cell.year != year { 0int } else { first_count * (0int * value) });
                    }
                }
                while second_index < second_names.rows.len()
                    invariant second_index <= second_names.rows@.len(), first_index < first_names.rows@.len(), changes.valid(),
                        patched_answers_shape(*old_answers, initial) && (cell.year == 1995 || cell.year == 1996)
                            ==> patched_answers_shape(*old_answers, *changes),
                        initial == *old(changes), nations.at(first) == Some(*first_names), nations.at(cell.other) == Some(*second_names),
                        forall|left: Seq<u8>, right: Seq<u8>, year: i64, measure: bool|
                            #![trigger patched_answer(*old_answers, *changes, left, right, year, measure)]
                            patched_answer(*old_answers, *changes, left, right, year, measure)
                                == patched_answer(*old_answers, initial, left, right, year, measure)
                                    + expanded_prefix(first_names.rows@.take(first_index as int), second_names.rows@,
                                        *cell, sign@, left, right, year, measure)
                                    + expanded_prefix(seq![first_names.rows@[first_index as int]], second_names.rows@.take(second_index as int),
                                        *cell, sign@, left, right, year, measure),
                    decreases second_names.rows@.len() - second_index,
                {
                    let product = first_names.rows[first_index].1.mul(&second_names.rows[second_index].1);
                    let multiplier = product.mul(sign);
                    let count = cell.count.mul(&multiplier);
                    let revenue = cell.revenue.mul(&multiplier);
                    let ghost before_step = *changes;
                    answer_delta(old_answers, changes, &first_names.rows[first_index].0,
                        &second_names.rows[second_index].0, cell.year, &count, &revenue);
                    proof {
                        let first_row = first_names.rows@[first_index as int];
                        let second_row = second_names.rows@[second_index as int];
                        assert(product@ == first_row.1@ * second_row.1@);
                        assert(multiplier@ == product@ * sign@);
                        assert(count@ == cell.count@ * multiplier@);
                        assert(revenue@ == cell.revenue@ * multiplier@);
                        assert forall|left: Seq<u8>, right: Seq<u8>, year: i64, measure: bool|
                            #![trigger patched_answer(*old_answers, *changes, left, right, year, measure)]
                            patched_answer(*old_answers, *changes, left, right, year, measure)
                                == patched_answer(*old_answers, initial, left, right, year, measure)
                                    + expanded_prefix(first_names.rows@.take(first_index as int), second_names.rows@,
                                        *cell, sign@, left, right, year, measure)
                                    + expanded_prefix(seq![first_row], second_names.rows@.take(second_index as int + 1),
                                        *cell, sign@, left, right, year, measure) by {
                            let value = if measure { cell.revenue@ } else { cell.count@ };
                            lemma_sum_singleton(first_row, |row: (Vec<u8>, Wide)| if row.0@ == left { row.1@ } else { 0int });
                            lemma_sum_take(second_names.rows@, second_index as int,
                                |row: (Vec<u8>, Wide)| if row.0@ == right { row.1@ } else { 0int });
                            lemma_group_coefficient(1int, multiplier@, value, 1int);
                            lemma_group_coefficient(first_row.1@, second_row.1@, sign@, value);
                            lemma_scalar_distribute(name_at(second_names.rows@.take(second_index as int), right),
                                if second_row.0@ == right { second_row.1@ } else { 0int }, sign@ * value);
                            lemma_left_scalar_distribute(name_at(second_names.rows@.take(second_index as int), right) * (sign@ * value),
                                (if second_row.0@ == right { second_row.1@ } else { 0int }) * (sign@ * value),
                                name_at(seq![first_row], left));
                            assert(patched_answer(*old_answers, *changes, left, right, year, measure)
                                == patched_answer(*old_answers, before_step, left, right, year, measure)
                                    + if left == first_row.0@ && right == second_row.0@ && year == cell.year {
                                        if measure { revenue@ } else { count@ }
                                    } else { 0int });
                        }
                    }
                    second_index += 1;
                }
                proof {
                    assert(second_names.rows@.take(second_names.rows@.len() as int) == second_names.rows@);
                    assert forall|left: Seq<u8>, right: Seq<u8>, year: i64, measure: bool|
                        #![trigger patched_answer(*old_answers, *changes, left, right, year, measure)]
                        patched_answer(*old_answers, *changes, left, right, year, measure)
                            == patched_answer(*old_answers, initial, left, right, year, measure)
                                + expanded_prefix(first_names.rows@.take(first_index as int + 1), second_names.rows@,
                                    *cell, sign@, left, right, year, measure) by {
                        let row = first_names.rows@[first_index as int];
                        lemma_sum_singleton(row, |entry: (Vec<u8>, Wide)| if entry.0@ == left { entry.1@ } else { 0int });
                        lemma_sum_take(first_names.rows@, first_index as int,
                            |entry: (Vec<u8>, Wide)| if entry.0@ == left { entry.1@ } else { 0int });
                        lemma_scalar_distribute(name_at(first_names.rows@.take(first_index as int), left),
                            if row.0@ == left { row.1@ } else { 0int },
                            name_at(second_names.rows@, right) * (sign@ * if measure { cell.revenue@ } else { cell.count@ }));
                    }
                }
                first_index += 1;
            }
            proof { assert(first_names.rows@.take(first_names.rows@.len() as int) == first_names.rows@); }
            return;
        }
    }
    proof {
        assert(*changes == initial);
        assert forall|left: Seq<u8>, right: Seq<u8>, year: i64, measure: bool|
            #![trigger cell_expansion(*nations, first, *cell, sign@, left, right, year, measure)]
            cell_expansion(*nations, first, *cell, sign@, left, right, year, measure) == 0 by {
            if nations.at(first).is_none() {
                assert(nation_value(*nations, first, left) == 0);
            } else {
                assert(nations.at(cell.other).is_none());
                assert(nation_value(*nations, cell.other, right) == 0);
            }
            let first_count = nation_value(*nations, first, left);
            let second_count = nation_value(*nations, cell.other, right);
            let value = sign@ * if measure { cell.revenue@ } else { cell.count@ };
            assert(first_count * (second_count * value) == 0) by (nonlinear_arith)
                requires first_count == 0 || second_count == 0;
        }
    }
}

pub fn membership_patch(previous: &Layers<Layers<i128>>, changes: &mut Sparse<Layers<i128>>, key: i64, other: i64, present: bool)
    requires old(changes).valid(),
    ensures final(changes).valid(), forall|target: i64, variant: i64|
        #![trigger patched_member(*previous, *final(changes), target, variant)]
        patched_member(*previous, *final(changes), target, variant) ==
            if target == key && variant == other { present }
            else { patched_member(*previous, *old(changes), target, variant) },
        forall|target: i64| #![trigger final(changes).at(target)]
            final(changes).at(target) == old(changes).at(target)
                || (final(changes).at(target) matches Some(bucket) && bucket.valid()),
{
    let bucket = match changes.get(key) {
        Some(bucket) => bucket.share(),
        None => match previous.get(key) { Some(bucket) => bucket.share(), None => Layers::new(Sparse::new()) },
    };
    let mut patch = Sparse::new();
    patch.put(other, Arc::new(if present { 1i128 } else { 0i128 }));
    changes.put(key, Arc::new(bucket.patch(patch)));
}

pub fn add_key(keys: &mut Vec<i64>, key: i64)
    ensures final(keys)@ == if old(keys)@.contains(key) { old(keys)@ } else { old(keys)@.push(key) },
        final(keys)@.to_set() == old(keys)@.to_set().insert(key),
        old(keys)@.no_duplicates() ==> final(keys)@.no_duplicates(),
        forall|other: i64| #![trigger final(keys)@.contains(other)]
            final(keys)@.contains(other) == (old(keys)@.contains(other) || other == key),
{
    let mut offset = 0;
    while offset < keys.len()
        invariant offset <= keys@.len(), keys@ == old(keys)@,
            forall|index: int| #![trigger keys@[index]]
                0 <= index < offset ==> keys@[index] != key,
        decreases keys@.len() - offset,
    { if keys[offset] == key { proof { assert(keys@[offset as int] == key); } return; } offset += 1; }
    keys.push(key);
    proof { verus_prooflib::lemma_contains_push(old(keys)@, key); }
}

pub fn has_key(keys: &[i64], key: i64) -> (result: bool)
    ensures result == keys@.contains(key),
{
    let mut offset = 0;
    while offset < keys.len()
        invariant offset <= keys@.len(),
            forall|index: int| #![trigger keys@[index]] 0 <= index < offset ==> keys@[index] != key,
        decreases keys@.len() - offset,
    { if keys[offset] == key { return true; } offset += 1; }
    false
}

}
