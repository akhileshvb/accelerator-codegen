use vstd::prelude::*;
use verus_common::order::*;
use verus_common::sums::*;
use crate::gen_spec::*;
use crate::gen_obligations::out_view;
use crate::profiles::{compare, copy_bytes};

verus! {
pub open spec fn projected(row: (Vec<u8>, Vec<u8>)) -> OutRow {
    OutRow { s_name: row.0@, s_address: row.1@ }
}
pub open spec fn le(left: OutRow, right: OutRow) -> bool { !ext_lt(right, left) }

pub proof fn lemma_lt_le(left: OutRow, right: OutRow)
    requires ext_lt(left, right),
    ensures le(left, right),
{
    lemma_ext_order();
    assert(ext_order()(left, right));
    if ext_lt(right, left) {
        assert(ext_order()(right, left));
        assert(ext_order()(left, left));
        assert(false);
    }
}

pub proof fn lemma_le_trans(left: OutRow, middle: OutRow, right: OutRow)
    requires le(left, middle), le(middle, right),
    ensures le(left, right),
{
    lemma_ext_order();
    if ext_lt(right, left) {
        if left != middle {
            lemma_ext_total(left, middle);
            assert(ext_lt(left, middle));
        }
        if middle != right {
            lemma_ext_total(middle, right);
            assert(ext_lt(middle, right));
        }
        if left != middle && middle != right {
            assert(ext_order()(right, left));
            assert(ext_order()(left, middle));
            assert(ext_order()(middle, right));
            assert(ext_order()(right, middle));
            assert(ext_order()(right, right));
        }
        assert(false);
    }
}

pub proof fn lemma_view_push(source: Seq<(Vec<u8>, Vec<u8>)>, row: (Vec<u8>, Vec<u8>))
    ensures out_view(source.push(row)) == out_view(source).push(projected(row)),
{
    assert(out_view(source.push(row)) =~= out_view(source).push(projected(row)));
}

pub proof fn lemma_bag_push(source: Seq<(Vec<u8>, Vec<u8>)>, row: (Vec<u8>, Vec<u8>))
    ensures out_view(source.push(row)).to_multiset() == out_view(source).to_multiset().insert(projected(row)),
{
    broadcast use vstd::seq_lib::group_to_multiset_ensures, vstd::multiset::group_multiset_axioms;
    lemma_view_push(source, row);
}

pub fn output_less(left: &(Vec<u8>, Vec<u8>), right: &(Vec<u8>, Vec<u8>)) -> (result: bool)
    ensures result == ext_lt(projected(*left), projected(*right)),
{
    let name = compare(&left.0, &right.0);
    name < 0 || (name == 0 && compare(&left.1, &right.1) < 0)
}

pub fn merge_sort(rows: Vec<(Vec<u8>, Vec<u8>)>) -> (result: Vec<(Vec<u8>, Vec<u8>)>)
    ensures out_view(result@).to_multiset() == out_view(rows@).to_multiset(),
        nondesc(out_view(result@), ext_order()),
    decreases rows@.len(),
{
    if rows.len() <= 1 { return rows; }
    let ghost input = out_view(rows@).to_multiset();
    let length = rows.len(); let middle = length / 2;
    let mut source = rows; let mut left = Vec::with_capacity(middle); let mut right = Vec::with_capacity(length - middle);
    proof {
        broadcast use vstd::seq_lib::group_to_multiset_ensures, vstd::multiset::group_multiset_axioms;
        assert(out_view(left@).to_multiset() =~= vstd::multiset::Multiset::<OutRow>::empty());
        assert(out_view(right@).to_multiset() =~= vstd::multiset::Multiset::<OutRow>::empty());
    }
    while source.len() > 0
        invariant length > 1, middle == length / 2,
            source.len() <= length, left.len() <= middle, right.len() <= length - middle,
            source.len() + left.len() + right.len() == length,
            out_view(source@).to_multiset().add(out_view(left@).to_multiset()).add(out_view(right@).to_multiset()) == input,
            source.len() > middle ==> left.len() == 0,
            source.len() <= middle ==> right.len() == length - middle,
        decreases source.len(),
    {
        let ghost previous = source@;
        let ghost previous_left = left@;
        let ghost previous_right = right@;
        let position = source.len() - 1;
        let row = match source.pop() { Some(value) => value, None => return Vec::new() };
        proof {
            broadcast use vstd::seq_lib::group_to_multiset_ensures, vstd::multiset::group_multiset_axioms;
            assert(previous =~= source@.push(row));
            lemma_bag_push(source@, row);
        }
        if position < middle {
            left.push(row);
            proof {
                broadcast use vstd::seq_lib::group_to_multiset_ensures, vstd::multiset::group_multiset_axioms;
                lemma_bag_push(previous_left, row);
                assert(out_view(source@).to_multiset().add(out_view(left@).to_multiset()).add(out_view(right@).to_multiset()) =~= input);
            }
        } else {
            right.push(row);
            proof {
                broadcast use vstd::seq_lib::group_to_multiset_ensures, vstd::multiset::group_multiset_axioms;
                lemma_bag_push(previous_right, row);
                assert(out_view(source@).to_multiset().add(out_view(left@).to_multiset()).add(out_view(right@).to_multiset()) =~= input);
            }
        }
    }
    proof {
        broadcast use vstd::seq_lib::group_to_multiset_ensures, vstd::multiset::group_multiset_axioms;
        assert(out_view(source@).to_multiset() =~= vstd::multiset::Multiset::<OutRow>::empty());
    }
    let left = merge_sort(left); let right = merge_sort(right);
    let mut result = Vec::with_capacity(length); let mut left_index = 0; let mut right_index = 0;
    while left_index < left.len() || right_index < right.len()
        invariant left_index <= left.len(), right_index <= right.len(),
            nondesc(out_view(left@), ext_order()), nondesc(out_view(right@), ext_order()),
            nondesc(out_view(result@), ext_order()),
            out_view(left@).to_multiset().add(out_view(right@).to_multiset()) == input,
            out_view(result@).to_multiset() == out_view(left@.take(left_index as int)).to_multiset().add(out_view(right@.take(right_index as int)).to_multiset()),
            forall|index: int| #![trigger result@[index]] 0 <= index < result.len() && left_index < left.len() ==>
                le(projected(result@[index]), projected(left@[left_index as int])),
            forall|index: int| #![trigger result@[index]] 0 <= index < result.len() && right_index < right.len() ==>
                le(projected(result@[index]), projected(right@[right_index as int])),
        decreases left.len() - left_index + right.len() - right_index,
    {
        let ghost previous = result@;
        if right_index == right.len() || (left_index < left.len() && output_less(&left[left_index], &right[right_index])) {
            let ghost row = projected(left@[left_index as int]);
            proof { lemma_ext_order(); }
            result.push((copy_bytes(&left[left_index].0), copy_bytes(&left[left_index].1)));
            proof {
                broadcast use vstd::seq_lib::group_to_multiset_ensures, vstd::multiset::group_multiset_axioms;
                assert(out_view(result@) =~= out_view(previous).push(row));
                assert(out_view(left@.take(left_index as int + 1)) =~= out_view(left@.take(left_index as int)).push(row));
                assert(out_view(result@).to_multiset() =~= out_view(left@.take(left_index as int + 1)).to_multiset().add(out_view(right@.take(right_index as int)).to_multiset()));
                assert(nondesc(out_view(result@), ext_order())) by {
                    assert forall|first: int, second: int| #![trigger out_view(result@)[first], out_view(result@)[second]]
                        0 <= first < second < result.len() implies !ext_lt(out_view(result@)[second], out_view(result@)[first]) by {
                        if second < previous.len() { assert(out_view(result@)[second] == out_view(previous)[second]); }
                        else { assert(le(projected(previous[first]), row)); }
                    }
                }
                if left_index + 1 < left.len() {
                    assert(!ext_order()(out_view(left@)[left_index as int + 1], out_view(left@)[left_index as int]));
                    assert(le(row, projected(left@[left_index as int + 1])));
                    assert forall|index: int| #![trigger result@[index]] 0 <= index < result.len()
                        implies le(projected(result@[index]), projected(left@[left_index as int + 1])) by {
                        if index < previous.len() { lemma_le_trans(projected(previous[index]), row, projected(left@[left_index as int + 1])); }
                    }
                }
                if right_index < right.len() {
                    lemma_lt_le(row, projected(right@[right_index as int]));
                    assert(le(row, projected(right@[right_index as int])));
                    assert forall|index: int| #![trigger result@[index]] 0 <= index < result.len()
                        implies le(projected(result@[index]), projected(right@[right_index as int])) by {
                        if index < previous.len() { assert(le(projected(previous[index]), projected(right@[right_index as int]))); }
                    }
                }
            }
            left_index += 1;
        } else {
            let ghost row = projected(right@[right_index as int]);
            result.push((copy_bytes(&right[right_index].0), copy_bytes(&right[right_index].1)));
            proof {
                broadcast use vstd::seq_lib::group_to_multiset_ensures, vstd::multiset::group_multiset_axioms;
                assert(out_view(result@) =~= out_view(previous).push(row));
                assert(out_view(right@.take(right_index as int + 1)) =~= out_view(right@.take(right_index as int)).push(row));
                assert(out_view(result@).to_multiset() =~= out_view(left@.take(left_index as int)).to_multiset().add(out_view(right@.take(right_index as int + 1)).to_multiset()));
                assert(nondesc(out_view(result@), ext_order())) by {
                    assert forall|first: int, second: int| #![trigger out_view(result@)[first], out_view(result@)[second]]
                        0 <= first < second < result.len() implies !ext_lt(out_view(result@)[second], out_view(result@)[first]) by {
                        if second < previous.len() { assert(out_view(result@)[second] == out_view(previous)[second]); }
                        else { assert(le(projected(previous[first]), row)); }
                    }
                }
                if right_index + 1 < right.len() {
                    assert(!ext_order()(out_view(right@)[right_index as int + 1], out_view(right@)[right_index as int]));
                    assert(le(row, projected(right@[right_index as int + 1])));
                    assert forall|index: int| #![trigger result@[index]] 0 <= index < result.len()
                        implies le(projected(result@[index]), projected(right@[right_index as int + 1])) by {
                        if index < previous.len() { lemma_le_trans(projected(previous[index]), row, projected(right@[right_index as int + 1])); }
                    }
                }
                if left_index < left.len() {
                    assert(le(row, projected(left@[left_index as int])));
                    assert forall|index: int| #![trigger result@[index]] 0 <= index < result.len()
                        implies le(projected(result@[index]), projected(left@[left_index as int])) by {
                        if index < previous.len() { assert(le(projected(previous[index]), projected(left@[left_index as int]))); }
                    }
                }
            }
            right_index += 1;
        }
    }
    proof {
        assert(out_view(left@.take(left_index as int)) =~= out_view(left@));
        assert(out_view(right@.take(right_index as int)) =~= out_view(right@));
    }
    result
}
}
