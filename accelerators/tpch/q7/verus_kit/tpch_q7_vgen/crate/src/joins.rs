use vstd::prelude::*;
use std::sync::Arc;
use crate::storage::*;
use crate::values::*;
use crate::values::value;
use crate::model::*;
use verus_common::sums::*;
use verus_prooflib::*;

verus! {
broadcast use vstd::seq_lib::group_seq_properties;

pub fn line_contribution(suppliers: &Layers<Counts>, customers: &Layers<Counts>, orders: &Layers<Counts>,
    changes: &mut Sparse<Groups>, order: i64, supplier: i64, year: i64, volume: i128, sign: i128) -> (result: Option<()>)
    requires old(changes).valid(),
    ensures result.is_some() ==> final(changes).valid(),
        result.is_some() && sparse_groups_shape(*old(changes)) && (year == 1995 || year == 1996)
            ==> sparse_groups_shape(*final(changes)),
        result.is_some() ==> forall|first: i64, second: i64, target_year: i64, revenue: bool|
            #![trigger sparse_group(*final(changes), first, second, target_year, revenue)]
            sparse_group(*final(changes), first, second, target_year, revenue)
            == sparse_group(*old(changes), first, second, target_year, revenue)
                + sign as int * projected_contribution(*suppliers, *customers, *orders,
                    order, supplier, year, volume as int, first, second, target_year, revenue),
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
                                first, second, target_year, year, volume as int, sign as int, revenue),
                decreases supplier_bucket.rows@.len() - supplier_index,
            {
                let supplier_row = supplier_bucket.rows[supplier_index];
                let mut order_index = 0;
                proof {
                    assert(order_bucket.rows@.take(0).len() == 0);
                    assert forall|first: i64, second: i64, target_year: i64, measure: bool|
                        #![trigger prefix_product(seq![supplier_row], order_bucket.rows@.take(0), *customers,
                            first, second, target_year, year, volume as int, sign as int, measure)]
                        prefix_product(seq![supplier_row], order_bucket.rows@.take(0), *customers,
                            first, second, target_year, year, volume as int, sign as int, measure) == 0 by {
                        lemma_sum_empty(order_bucket.rows@.take(0), |row: (i64, i128)| row.1 as int * dimension_value(*customers, row.0, second));
                        assert(order_weight(order_bucket.rows@.take(0), *customers, second) == 0);
                        assert(prefix_product(seq![supplier_row], order_bucket.rows@.take(0), *customers,
                            first, second, target_year, year, volume as int, sign as int, measure)
                            == if target_year != year { 0int } else {
                                count_at(seq![supplier_row], first) * (0int * (sign as int * if measure { volume as int } else { 1int }))
                            });
                    }
                }
                while order_index < order_bucket.rows.len()
                    invariant order_index <= order_bucket.rows@.len(), supplier_index < supplier_bucket.rows@.len(),
                        sparse_groups_shape(initial) && (year == 1995 || year == 1996) ==> sparse_groups_shape(*changes),
                        changes.valid(), initial == *old(changes),
                        supplier_row == supplier_bucket.rows@[supplier_index as int],
                        suppliers.at(supplier) == Some(*supplier_bucket), orders.at(order) == Some(*order_bucket),
                        forall|first: i64, second: i64, target_year: i64, revenue: bool|
                            #![trigger sparse_group(*changes, first, second, target_year, revenue)]
                            sparse_group(*changes, first, second, target_year, revenue)
                            == sparse_group(initial, first, second, target_year, revenue)
                                + prefix_product(supplier_bucket.rows@.take(supplier_index as int), order_bucket.rows@, *customers,
                                    first, second, target_year, year, volume as int, sign as int, revenue)
                                + prefix_product(seq![supplier_row], order_bucket.rows@.take(order_index as int), *customers,
                                    first, second, target_year, year, volume as int, sign as int, revenue),
                    decreases order_bucket.rows@.len() - order_index,
                {
                    let order_row = order_bucket.rows[order_index];
                    if let Some(customer_bucket) = customers.get(order_row.0) {
                        let mut customer_index = 0;
                        while customer_index < customer_bucket.rows.len()
                            invariant customer_index <= customer_bucket.rows@.len(), changes.valid(), initial == *old(changes),
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
                                            first, second, target_year, year, volume as int, sign as int, revenue)
                                        + prefix_product(seq![supplier_row], order_bucket.rows@.take(order_index as int), *customers,
                                            first, second, target_year, year, volume as int, sign as int, revenue)
                                        + customer_piece(customer_bucket.rows@.take(customer_index as int), supplier_row, order_row,
                                            first, second, target_year, year, volume as int, sign as int, revenue),
                            decreases customer_bucket.rows@.len() - customer_index,
                        {
                            let customer_row = customer_bucket.rows[customer_index];
                            let supplier_order_count = value!(supplier_row.1.checked_mul(order_row.1));
                            let joined_count = value!(supplier_order_count.checked_mul(customer_row.1));
                            let count = value!(joined_count.checked_mul(sign));
                            let revenue = value!(count.checked_mul(volume));
                            proof {
                                assert(supplier_order_count as int == supplier_row.1 as int * order_row.1 as int);
                                assert(joined_count as int == supplier_order_count as int * customer_row.1 as int);
                                assert(count as int == joined_count as int * sign as int);
                            }
                            value!(group_delta(changes, supplier_row.0, customer_row.0, year, count, revenue));
                            proof {
                                assert forall|first: i64, second: i64, target_year: i64, measure: bool|
                                    #![trigger sparse_group(*changes, first, second, target_year, measure)]
                                    sparse_group(*changes, first, second, target_year, measure)
                                    == sparse_group(initial, first, second, target_year, measure)
                                        + prefix_product(supplier_bucket.rows@.take(supplier_index as int), order_bucket.rows@, *customers,
                                            first, second, target_year, year, volume as int, sign as int, measure)
                                        + prefix_product(seq![supplier_row], order_bucket.rows@.take(order_index as int), *customers,
                                            first, second, target_year, year, volume as int, sign as int, measure)
                                        + customer_piece(customer_bucket.rows@.take(customer_index as int + 1), supplier_row, order_row,
                                            first, second, target_year, year, volume as int, sign as int, measure) by {
                                    lemma_sum_take(customer_bucket.rows@, customer_index as int,
                                        |row: (i64, i128)| if row.0 == second { row.1 as int } else { 0int });
                                    assert(count as int == supplier_row.1 as int * order_row.1 as int * customer_row.1 as int * sign as int);
                                    assert(revenue as int == count as int * volume as int);
                                    assert(customer_piece(customer_bucket.rows@.take(customer_index as int + 1), supplier_row, order_row,
                                        first, second, target_year, year, volume as int, sign as int, measure)
                                        == customer_piece(customer_bucket.rows@.take(customer_index as int), supplier_row, order_row,
                                            first, second, target_year, year, volume as int, sign as int, measure)
                                        + if first == supplier_row.0 && second == customer_row.0 && target_year == year {
                                            if measure { revenue as int } else { count as int }
                                        } else { 0 }) by {
                                            lemma_scalar_distribute(count_at(customer_bucket.rows@.take(customer_index as int), second),
                                                if customer_row.0 == second { customer_row.1 as int } else { 0int },
                                                supplier_row.1 as int * (order_row.1 as int * (sign as int * if measure { volume as int } else { 1int })));
                                            lemma_join_product(supplier_row.1 as int, order_row.1 as int, customer_row.1 as int,
                                                sign as int, if measure { volume as int } else { 1int });
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
                                    first, second, target_year, year, volume as int, sign as int, measure)
                                + prefix_product(seq![supplier_row], order_bucket.rows@.take(order_index as int + 1), *customers,
                                    first, second, target_year, year, volume as int, sign as int, measure) by {
                            lemma_sum_take(order_bucket.rows@, order_index as int,
                                |row: (i64, i128)| row.1 as int * dimension_value(*customers, row.0, second));
                            assert(prefix_product(seq![supplier_row], order_bucket.rows@.take(order_index as int + 1), *customers,
                                first, second, target_year, year, volume as int, sign as int, measure)
                                == prefix_product(seq![supplier_row], order_bucket.rows@.take(order_index as int), *customers,
                                    first, second, target_year, year, volume as int, sign as int, measure)
                                + (if first == supplier_row.0 && target_year == year {
                                    supplier_row.1 as int * order_row.1 as int * dimension_value(*customers, order_row.0, second)
                                        * sign as int * if measure { volume as int } else { 1int }
                                } else { 0int })) by {
                                    lemma_sum_singleton(supplier_row, |row: (i64, i128)| if row.0 == first { row.1 as int } else { 0int });
                                    lemma_scalar_distribute(order_weight(order_bucket.rows@.take(order_index as int), *customers, second),
                                        order_row.1 as int * dimension_value(*customers, order_row.0, second),
                                        sign as int * if measure { volume as int } else { 1int });
                                    lemma_left_scalar_distribute(order_weight(order_bucket.rows@.take(order_index as int), *customers, second)
                                            * (sign as int * if measure { volume as int } else { 1int }),
                                        (order_row.1 as int * dimension_value(*customers, order_row.0, second))
                                            * (sign as int * if measure { volume as int } else { 1int }),
                                        count_at(seq![supplier_row], first));
                                    lemma_group_coefficient(supplier_row.1 as int, order_row.1 as int * dimension_value(*customers, order_row.0, second),
                                        sign as int, if measure { volume as int } else { 1int });
                                    lemma_group_coefficient(supplier_row.1 as int, order_row.1 as int, 1int, dimension_value(*customers, order_row.0, second));
                                }
                            if let Some(customer_bucket) = customers.at(order_row.0) {
                                lemma_sum_singleton(supplier_row, |row: (i64, i128)| if row.0 == first { row.1 as int } else { 0int });
                                lemma_join_product(supplier_row.1 as int, order_row.1 as int,
                                    count_at(customer_bucket.rows@, second), sign as int,
                                    if measure { volume as int } else { 1int });
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
                                first, second, target_year, year, volume as int, sign as int, measure) by {
                        lemma_sum_take(supplier_bucket.rows@, supplier_index as int,
                            |row: (i64, i128)| if row.0 == first { row.1 as int } else { 0int });
                        assert(order_bucket.rows@.take(order_bucket.rows@.len() as int) == order_bucket.rows@);
                        assert(prefix_product(supplier_bucket.rows@.take(supplier_index as int + 1), order_bucket.rows@, *customers,
                            first, second, target_year, year, volume as int, sign as int, measure)
                            == prefix_product(supplier_bucket.rows@.take(supplier_index as int), order_bucket.rows@, *customers,
                                first, second, target_year, year, volume as int, sign as int, measure)
                                + prefix_product(seq![supplier_row], order_bucket.rows@, *customers,
                                    first, second, target_year, year, volume as int, sign as int, measure)) by {
                                lemma_sum_singleton(supplier_row, |row: (i64, i128)| if row.0 == first { row.1 as int } else { 0int });
                                lemma_scalar_distribute(count_at(supplier_bucket.rows@.take(supplier_index as int), first),
                                    if supplier_row.0 == first { supplier_row.1 as int } else { 0int },
                                    order_weight(order_bucket.rows@, *customers, second) * (sign as int * if measure { volume as int } else { 1int }));
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
                        + sign as int * projected_contribution(*suppliers, *customers, *orders,
                            order, supplier, year, volume as int, first, second, target_year, measure) by {
                    lemma_group_coefficient(count_at(supplier_bucket.rows@, first), order_weight(order_bucket.rows@, *customers, second),
                        sign as int, if measure { volume as int } else { 1int });
                }
            }
            return Some(());
        } else {
            proof {
                reveal(projected_contribution);
                assert(orders.at(order).is_none());
                assert(*changes == initial);
                assert forall|first: i64, second: i64, target_year: i64, measure: bool|
                    #![trigger projected_contribution(*suppliers, *customers, *orders,
                        order, supplier, year, volume as int, first, second, target_year, measure)]
                    projected_contribution(*suppliers, *customers, *orders,
                        order, supplier, year, volume as int, first, second, target_year, measure) == 0 by {
                    lemma_projected_absent(*suppliers, *customers, *orders, order, supplier, year, volume as int,
                        first, second, target_year, measure);
                }
            }
            return Some(());
        }
    } else {
        proof {
            reveal(projected_contribution);
            assert(suppliers.at(supplier).is_none());
            assert(*changes == initial);
            assert forall|first: i64, second: i64, target_year: i64, measure: bool|
                #![trigger projected_contribution(*suppliers, *customers, *orders,
                    order, supplier, year, volume as int, first, second, target_year, measure)]
            projected_contribution(*suppliers, *customers, *orders,
                    order, supplier, year, volume as int, first, second, target_year, measure) == 0 by {
                lemma_projected_absent(*suppliers, *customers, *orders, order, supplier, year, volume as int,
                    first, second, target_year, measure);
            }
        }
        return Some(());
    }
}

pub fn expand_cell(nations: &Layers<Names>, old_answers: &Layers<Answers>, changes: &mut Sparse<Answers>,
    first: i64, cell: Cell, sign: i128) -> (result: Option<()>)
    requires old(changes).valid(),
    ensures final(changes).valid(), result.is_some() ==> forall|left: Seq<u8>, right: Seq<u8>, year: i64, measure: bool|
        #![trigger patched_answer(*old_answers, *final(changes), left, right, year, measure)]
        patched_answer(*old_answers, *final(changes), left, right, year, measure)
            == patched_answer(*old_answers, *old(changes), left, right, year, measure)
                + cell_expansion(*nations, first, cell, sign as int, left, right, year, measure),
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
                                    cell, sign as int, left, right, year, measure),
                decreases first_names.rows@.len() - first_index,
            {
                let mut second_index = 0;
                proof {
                    assert(second_names.rows@.take(0).len() == 0);
                    assert forall|left: Seq<u8>, right: Seq<u8>, year: i64, measure: bool|
                        #![trigger expanded_prefix(seq![first_names.rows@[first_index as int]], second_names.rows@.take(0),
                            cell, sign as int, left, right, year, measure)]
                        expanded_prefix(seq![first_names.rows@[first_index as int]], second_names.rows@.take(0),
                            cell, sign as int, left, right, year, measure) == 0 by {
                        lemma_sum_empty(second_names.rows@.take(0), |row: (Vec<u8>, i128)|
                            if row.0@ == right { row.1 as int } else { 0int });
                        assert(name_at(second_names.rows@.take(0), right) == 0);
                        let first_count = name_at(seq![first_names.rows@[first_index as int]], left);
                        let value = sign as int * if measure { cell.revenue as int } else { cell.count as int };
                        assert(first_count * (0int * value) == 0) by (nonlinear_arith);
                        assert(expanded_prefix(seq![first_names.rows@[first_index as int]], second_names.rows@.take(0),
                            cell, sign as int, left, right, year, measure)
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
                                        cell, sign as int, left, right, year, measure)
                                    + expanded_prefix(seq![first_names.rows@[first_index as int]], second_names.rows@.take(second_index as int),
                                        cell, sign as int, left, right, year, measure),
                    decreases second_names.rows@.len() - second_index,
                {
                    let product = value!(first_names.rows[first_index].1.checked_mul(second_names.rows[second_index].1));
                    let multiplier = value!(product.checked_mul(sign));
                    let count = value!(cell.count.checked_mul(multiplier));
                    let revenue = value!(cell.revenue.checked_mul(multiplier));
                    let ghost before_step = *changes;
                    value!(answer_delta(old_answers, changes, &first_names.rows[first_index].0,
                        &second_names.rows[second_index].0, cell.year, count, revenue));
                    proof {
                        let first_row = first_names.rows@[first_index as int];
                        let second_row = second_names.rows@[second_index as int];
                        assert(product as int == first_row.1 as int * second_row.1 as int);
                        assert(multiplier as int == product as int * sign as int);
                        assert(count as int == cell.count as int * multiplier as int);
                        assert(revenue as int == cell.revenue as int * multiplier as int);
                        assert forall|left: Seq<u8>, right: Seq<u8>, year: i64, measure: bool|
                            #![trigger patched_answer(*old_answers, *changes, left, right, year, measure)]
                            patched_answer(*old_answers, *changes, left, right, year, measure)
                                == patched_answer(*old_answers, initial, left, right, year, measure)
                                    + expanded_prefix(first_names.rows@.take(first_index as int), second_names.rows@,
                                        cell, sign as int, left, right, year, measure)
                                    + expanded_prefix(seq![first_row], second_names.rows@.take(second_index as int + 1),
                                        cell, sign as int, left, right, year, measure) by {
                            let value = if measure { cell.revenue as int } else { cell.count as int };
                            lemma_sum_singleton(first_row, |row: (Vec<u8>, i128)| if row.0@ == left { row.1 as int } else { 0int });
                            lemma_sum_take(second_names.rows@, second_index as int,
                                |row: (Vec<u8>, i128)| if row.0@ == right { row.1 as int } else { 0int });
                            lemma_group_coefficient(1int, multiplier as int, value, 1int);
                            lemma_group_coefficient(first_row.1 as int, second_row.1 as int, sign as int, value);
                            lemma_scalar_distribute(name_at(second_names.rows@.take(second_index as int), right),
                                if second_row.0@ == right { second_row.1 as int } else { 0int }, sign as int * value);
                            lemma_left_scalar_distribute(name_at(second_names.rows@.take(second_index as int), right) * (sign as int * value),
                                (if second_row.0@ == right { second_row.1 as int } else { 0int }) * (sign as int * value),
                                name_at(seq![first_row], left));
                            assert(patched_answer(*old_answers, *changes, left, right, year, measure)
                                == patched_answer(*old_answers, before_step, left, right, year, measure)
                                    + if left == first_row.0@ && right == second_row.0@ && year == cell.year {
                                        if measure { revenue as int } else { count as int }
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
                                    cell, sign as int, left, right, year, measure) by {
                        let row = first_names.rows@[first_index as int];
                        lemma_sum_singleton(row, |entry: (Vec<u8>, i128)| if entry.0@ == left { entry.1 as int } else { 0int });
                        lemma_sum_take(first_names.rows@, first_index as int,
                            |entry: (Vec<u8>, i128)| if entry.0@ == left { entry.1 as int } else { 0int });
                        lemma_scalar_distribute(name_at(first_names.rows@.take(first_index as int), left),
                            if row.0@ == left { row.1 as int } else { 0int },
                            name_at(second_names.rows@, right) * (sign as int * if measure { cell.revenue as int } else { cell.count as int }));
                    }
                }
                first_index += 1;
            }
            proof { assert(first_names.rows@.take(first_names.rows@.len() as int) == first_names.rows@); }
            return Some(());
        }
    }
    proof {
        assert(*changes == initial);
        assert forall|left: Seq<u8>, right: Seq<u8>, year: i64, measure: bool|
            #![trigger cell_expansion(*nations, first, cell, sign as int, left, right, year, measure)]
            cell_expansion(*nations, first, cell, sign as int, left, right, year, measure) == 0 by {
            if nations.at(first).is_none() {
                assert(nation_value(*nations, first, left) == 0);
            } else {
                assert(nations.at(cell.other).is_none());
                assert(nation_value(*nations, cell.other, right) == 0);
            }
            let first_count = nation_value(*nations, first, left);
            let second_count = nation_value(*nations, cell.other, right);
            let value = sign as int * if measure { cell.revenue as int } else { cell.count as int };
            assert(first_count * (second_count * value) == 0) by (nonlinear_arith)
                requires first_count == 0 || second_count == 0;
        }
    }
    Some(())
}

pub fn membership_patch(previous: &Layers<Layers<i128>>, changes: &mut Sparse<Layers<i128>>, key: i64, other: i64, present: bool) -> (result: Option<()>)
    requires old(changes).valid(),
    ensures final(changes).valid(), result.is_some() ==> forall|target: i64, variant: i64|
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
    changes.put(key, Arc::new(value!(bucket.patch(patch))));
    Some(())
}

pub fn add_key(keys: &mut Vec<i64>, key: i64)
    ensures final(keys)@.to_set() == old(keys)@.to_set().insert(key),
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
    { if keys[offset] == key { return; } offset += 1; }
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

pub fn covered(needed: &[i64], actual: &[i64]) -> (result: bool)
    ensures result ==> forall|index: int| #![trigger needed@[index]]
        0 <= index < needed@.len() ==> actual@.contains(needed@[index]),
        result ==> forall|key: i64| #![trigger needed@.contains(key)]
            needed@.contains(key) ==> actual@.contains(key),
{
    let mut offset = 0;
    while offset < needed.len()
        invariant offset <= needed@.len(),
            forall|index: int| #![trigger needed@[index]]
                0 <= index < offset ==> actual@.contains(needed@[index]),
        decreases needed@.len() - offset,
    { if !has_key(actual, needed[offset]) { return false; } offset += 1; }
    proof {
        assert forall|key: i64| #![trigger needed@.contains(key)]
            needed@.contains(key) implies actual@.contains(key) by {
            let position = choose|position: int| #![trigger needed@[position]]
                0 <= position < needed@.len() && needed@[position] == key;
            assert(actual@.contains(needed@[position]));
        }
    }
    true
}

pub fn positive_counts(patch: &Sparse<Counts>) -> (result: bool)
    requires patch.valid(),
    ensures result ==> forall|key: i64| #![trigger patch.at(key)]
        patch.at(key) matches Some(bucket) ==> counts_positive(bucket),
{
    let mut bucket_index = 0;
    while bucket_index < patch.rows().len()
        invariant bucket_index <= patch.view().len(), patch.valid(),
            forall|position: int| #![trigger patch.view()[position]]
                0 <= position < bucket_index ==> counts_positive(*patch.view()[position].1),
        decreases patch.view().len() - bucket_index,
    {
        let bucket = &*patch.rows()[bucket_index].1;
        let mut offset = 0;
        while offset < bucket.rows.len()
            invariant offset <= bucket.rows@.len(),
                forall|position: int| #![trigger bucket.rows@[position]]
                    0 <= position < offset ==> bucket.rows@[position].1 > 0,
            decreases bucket.rows@.len() - offset,
        { if bucket.rows[offset].1 <= 0 { return false; } offset += 1; }
        bucket_index += 1;
    }
    proof {
        patch.row_facts();
        assert forall|key: i64| #![trigger patch.at(key)]
            patch.at(key) matches Some(bucket) ==> counts_positive(bucket) by {
            if let Some(bucket) = patch.at(key) {
                let position = choose|position: int| #![trigger patch.view()[position]]
                    0 <= position < patch.view().len() && patch.view()[position].0 == key
                        && patch.at(key) == Some(*patch.view()[position].1);
                assert(counts_positive(*patch.view()[position].1));
            }
        }
    }
    true
}

pub fn positive_names(patch: &Sparse<Names>) -> (result: bool)
    requires patch.valid(),
    ensures result ==> forall|key: i64| #![trigger patch.at(key)]
        patch.at(key) matches Some(bucket) ==> names_positive(bucket),
{
    let mut bucket_index = 0;
    while bucket_index < patch.rows().len()
        invariant bucket_index <= patch.view().len(), patch.valid(),
            forall|position: int| #![trigger patch.view()[position]]
                0 <= position < bucket_index ==> names_positive(*patch.view()[position].1),
        decreases patch.view().len() - bucket_index,
    {
        let bucket = &*patch.rows()[bucket_index].1;
        let mut offset = 0;
        while offset < bucket.rows.len()
            invariant offset <= bucket.rows@.len(),
                forall|position: int| #![trigger bucket.rows@[position]]
                    0 <= position < offset ==> bucket.rows@[position].1 > 0,
            decreases bucket.rows@.len() - offset,
        { if bucket.rows[offset].1 <= 0 { return false; } offset += 1; }
        bucket_index += 1;
    }
    proof {
        patch.row_facts();
        assert forall|key: i64| #![trigger patch.at(key)]
            patch.at(key) matches Some(bucket) ==> names_positive(bucket) by {
            if let Some(bucket) = patch.at(key) {
                let position = choose|position: int| #![trigger patch.view()[position]]
                    0 <= position < patch.view().len() && patch.view()[position].0 == key
                        && patch.at(key) == Some(*patch.view()[position].1);
                assert(names_positive(*patch.view()[position].1));
            }
        }
    }
    true
}

pub fn positive_groups(patch: &Sparse<Groups>) -> (result: bool)
    requires patch.valid(),
    ensures result ==> forall|key: i64| #![trigger patch.at(key)]
        patch.at(key) matches Some(bucket) ==> groups_positive(bucket),
{
    let mut bucket_index = 0;
    while bucket_index < patch.rows().len()
        invariant bucket_index <= patch.view().len(), patch.valid(),
            forall|position: int| #![trigger patch.view()[position]]
                0 <= position < bucket_index ==> groups_positive(*patch.view()[position].1),
        decreases patch.view().len() - bucket_index,
    {
        let bucket = &*patch.rows()[bucket_index].1;
        let mut offset = 0;
        while offset < bucket.rows.len()
            invariant offset <= bucket.rows@.len(),
                forall|position: int| #![trigger bucket.rows@[position]]
                    0 <= position < offset ==> bucket.rows@[position].count > 0,
            decreases bucket.rows@.len() - offset,
        { if bucket.rows[offset].count <= 0 { return false; } offset += 1; }
        bucket_index += 1;
    }
    proof {
        patch.row_facts();
        assert forall|key: i64| #![trigger patch.at(key)]
            patch.at(key) matches Some(bucket) ==> groups_positive(bucket) by {
            if let Some(bucket) = patch.at(key) {
                let position = choose|position: int| #![trigger patch.view()[position]]
                    0 <= position < patch.view().len() && patch.view()[position].0 == key
                        && patch.at(key) == Some(*patch.view()[position].1);
                assert(groups_positive(*patch.view()[position].1));
            }
        }
    }
    true
}

pub fn positive_answers(patch: &Sparse<Answers>) -> (result: bool)
    requires patch.valid(),
    ensures result ==> forall|key: i64| #![trigger patch.at(key)]
        patch.at(key) matches Some(bucket) ==> answers_positive(bucket),
{
    let mut bucket_index = 0;
    while bucket_index < patch.rows().len()
        invariant bucket_index <= patch.view().len(), patch.valid(),
            forall|position: int| #![trigger patch.view()[position]]
                0 <= position < bucket_index ==> answers_positive(*patch.view()[position].1),
        decreases patch.view().len() - bucket_index,
    {
        let bucket = &*patch.rows()[bucket_index].1;
        let mut offset = 0;
        while offset < bucket.rows.len()
            invariant offset <= bucket.rows@.len(),
                forall|position: int| #![trigger bucket.rows@[position]]
                    0 <= position < offset ==> bucket.rows@[position].count > 0,
            decreases bucket.rows@.len() - offset,
        { if bucket.rows[offset].count <= 0 { return false; } offset += 1; }
        bucket_index += 1;
    }
    proof {
        patch.row_facts();
        assert forall|key: i64| #![trigger patch.at(key)]
            patch.at(key) matches Some(bucket) ==> answers_positive(bucket) by {
            if let Some(bucket) = patch.at(key) {
                let position = choose|position: int| #![trigger patch.view()[position]]
                    0 <= position < patch.view().len() && patch.view()[position].0 == key
                        && patch.at(key) == Some(*patch.view()[position].1);
                assert(answers_positive(*patch.view()[position].1));
            }
        }
    }
    true
}

}
