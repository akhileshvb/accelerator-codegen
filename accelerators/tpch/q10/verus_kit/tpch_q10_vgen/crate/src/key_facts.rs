use vstd::prelude::*;
use crate::encoding::*;
use crate::dimensions::customer_inner_view;
use crate::gen_spec::CustomerRow;
use crate::maintenance::order_key_view;
use crate::values::rank_view;

verus! {

pub proof fn lemma_machine_radices()
    ensures radix_power(8) == 18446744073709551616nat,
        radix_power(16) == 340282366920938463463374607431768211456nat,
{
    reveal_with_fuel(radix_power, 17);
}

pub proof fn lemma_customer_key_injective(left: CustomerRow, right: CustomerRow)
    requires left.c_custkey == right.c_custkey,
        i64::MIN <= left.c_acctbal <= i64::MAX, i64::MIN <= right.c_acctbal <= i64::MAX,
        customer_inner_view(left) == customer_inner_view(right),
    ensures left == right,
{
    let left_balance = number((left.c_acctbal - i64::MIN as int) as nat, 8);
    let right_balance = number((right.c_acctbal - i64::MIN as int) as nat, 8);
    let left_comment = text(left.c_comment);
    let right_comment = text(right.c_comment);
    let left_phone_tail = left_balance + left_comment;
    let right_phone_tail = right_balance + right_comment;
    let left_address_tail = text(left.c_phone) + left_phone_tail;
    let right_address_tail = text(right.c_phone) + right_phone_tail;
    let left_name_tail = text(left.c_address) + left_address_tail;
    let right_name_tail = text(right.c_address) + right_address_tail;
    assert(customer_inner_view(left).1 =~= text(left.c_name) + left_name_tail);
    assert(customer_inner_view(right).1 =~= text(right.c_name) + right_name_tail);
    lemma_text_frame(left.c_name, right.c_name, left_name_tail, right_name_tail);
    lemma_text_frame(left.c_address, right.c_address, left_address_tail, right_address_tail);
    lemma_text_frame(left.c_phone, right.c_phone, left_phone_tail, right_phone_tail);
    lemma_machine_radices();
    lemma_number_frame((left.c_acctbal - i64::MIN as int) as nat,
        (right.c_acctbal - i64::MIN as int) as nat, 8, left_comment, right_comment);
    assert(left_comment + Seq::<u8>::empty() =~= left_comment);
    assert(right_comment + Seq::<u8>::empty() =~= right_comment);
    lemma_text_frame(left.c_comment, right.c_comment, Seq::empty(), Seq::empty());
}

pub proof fn lemma_order_key_injective(left_customer: i64, left_day: i32, right_customer: i64, right_day: i32)
    requires order_key_view(left_customer as int, left_day as int) == order_key_view(right_customer as int, right_day as int),
    ensures left_customer == right_customer, left_day == right_day,
{
    lemma_machine_radices();
    let left = (left_day as int - i64::MIN as int) as nat;
    let right = (right_day as int - i64::MIN as int) as nat;
    assert(number(left, 8) + Seq::<u8>::empty() =~= number(left, 8));
    assert(number(right, 8) + Seq::<u8>::empty() =~= number(right, 8));
    lemma_number_frame(left, right, 8, Seq::empty(), Seq::empty());
}

pub proof fn lemma_rank_key_injective(left_upper: int, left_customer: i64, right_upper: int, right_customer: i64)
    requires 0 <= left_upper <= i128::MAX, 0 <= right_upper <= i128::MAX,
        rank_view(left_upper, left_customer as int) == rank_view(right_upper, right_customer as int),
    ensures left_upper == right_upper, left_customer == right_customer,
{
    verus_prooflib::lemma_i128_rank_order(left_upper as i128, right_upper as i128, left_customer, right_customer);
}

}
