use vstd::prelude::*;
use verus_common::order::*;
use crate::gen_spec::*;

verus! {

pub open spec fn valid_top(rows: Seq<OutRow>) -> bool {
    rows.len() <= 20 && sorted_by(rows, ext_order())
}

pub open spec fn covered(rows: Seq<OutRow>, row: OutRow) -> bool {
    rows.contains(row) || (rows.len() == 20 && !ext_lt(row, rows[19]))
}

pub proof fn lemma_output_total(left: OutRow, right: OutRow)
    ensures left != right ==> ext_order()(left, right) || ext_order()(right, left),
{
    if left.revenue == right.revenue && left != right { lemma_ext_total(left, right); }
}

pub proof fn lemma_fresh_at(rows: Seq<OutRow>, row: OutRow, position: int)
    requires valid_top(rows), 0 <= position <= rows.len(),
        forall|index: int| #![trigger rows[index]] 0 <= index < position ==> !ext_lt(row, rows[index]),
        position < rows.len() ==> ext_lt(row, rows[position]),
        position > 0 ==> ext_lt(rows[position - 1], row),
    ensures !rows.contains(row),
{
    lemma_ext_order();
    if rows.contains(row) {
        let index = choose|index: int| #![trigger rows[index]] 0 <= index < rows.len() && rows[index] == row;
        if index < position {
            if index < position - 1 { assert(ext_order()(rows[index], rows[position - 1])); }
        } else if index > position {
            assert(ext_order()(rows[position], rows[index]));
            assert(ext_order()(row, rows[position]));
            assert(ext_order()(row, row));
        }
    }
}

pub proof fn lemma_top_insert(rows: Seq<OutRow>, row: OutRow, position: int)
    requires valid_top(rows), !rows.contains(row), 0 <= position <= rows.len(),
        forall|index: int| #![trigger rows[index]] 0 <= index < position ==> !ext_lt(row, rows[index]),
        position < rows.len() ==> ext_lt(row, rows[position]),
    ensures valid_top(limit_rows(rows.insert(position, row), 20)),
        covered(limit_rows(rows.insert(position, row), 20), row),
        forall|value: OutRow| #[trigger] covered(rows, value) ==> covered(limit_rows(rows.insert(position, row), 20), value),
{
    hide(ext_lt);
    lemma_ext_order();
    assert forall|value: OutRow| rows.contains(value) implies #[trigger] ext_order()(row, value) || ext_order()(value, row) by {
        lemma_output_total(row, value);
    }
    lemma_insert_by(rows, row, ext_order());
    crate::query_proofs::lemma_candidate_insert_position(rows, row, position);
    let inserted = rows.insert(position, row);
    let after = limit_rows(inserted, 20);
    assert forall|first: int, last: int| #![trigger after[first], after[last]] 0 <= first < last < after.len()
        implies ext_order()(after[first], after[last]) by {
        assert(after[first] == inserted[first] && after[last] == inserted[last]);
    }
    if position < 20 {
        assert(after[position] == row);
        assert(after.contains(row));
    } else {
        assert(rows.len() == 20 && position == 20);
        assert(after =~= rows);
        assert(!ext_lt(row, rows[19]));
    }
    if rows.len() == 20 {
        assert(after.len() == 20);
        if position == 20 {
            assert(after[19] == rows[19]);
        } else if position == 19 {
            assert(after[19] == row);
            assert(ext_order()(after[19], rows[19]));
        } else {
            assert(after[19] == rows[18]);
            assert(ext_order()(after[19], rows[19]));
        }
        assert(after[19] == rows[19] || ext_order()(after[19], rows[19]));
    }
    assert forall|value: OutRow| #[trigger] covered(rows, value) implies covered(after, value) by {
        if rows.contains(value) {
            let index = choose|index: int| #![trigger rows[index]] 0 <= index < rows.len() && rows[index] == value;
            let shifted = if index < position { index } else { index + 1 };
            if shifted < after.len() {
                assert(after[shifted] == value);
                assert(after.contains(value));
            } else {
                assert(rows.len() == 20 && index == 19 && after.len() == 20);
                assert(value == rows[19]);
                if ext_order()(value, after[19]) {
                    if value != after[19] {
                        assert(ext_order()(after[19], value));
                        assert(ext_order()(value, value));
                    }
                }
            }
        } else {
            assert(rows.len() == 20 && after.len() == 20);
            if ext_order()(value, after[19]) {
                if after[19] != rows[19] {
                    assert(ext_order()(after[19], rows[19]));
                    assert(ext_order()(value, rows[19]));
                }
                assert(false);
            }
        }
    }
}

pub proof fn lemma_equal_group_rows(db: Db, parameters: Params, left: OutRow, right: OutRow)
    requires row_ok(db, parameters, left), row_ok(db, parameters, right), same_group(left, right),
    ensures left == right,
{}

pub proof fn lemma_rows_concat_contains(left: Seq<OutRow>, right: Seq<OutRow>, row: OutRow)
    ensures (left + right).contains(row) == (left.contains(row) || right.contains(row)),
{
    let joined = left + right;
    if joined.contains(row) {
        let position = choose|position: int| #![trigger joined[position]] 0 <= position < joined.len() && joined[position] == row;
        if position < left.len() { assert(left[position] == row); }
        else { assert(right[position - left.len()] == row); }
    }
    if left.contains(row) {
        let position = choose|position: int| #![trigger left[position]] 0 <= position < left.len() && left[position] == row;
        assert(joined[position] == row);
    }
    if right.contains(row) {
        let position = choose|position: int| #![trigger right[position]] 0 <= position < right.len() && right[position] == row;
        assert(joined[left.len() + position] == row);
    }
}

pub open spec fn result_shape(db: Db, parameters: Params, rows: Seq<OutRow>) -> bool {
    &&& forall|row: OutRow| #[trigger] rows.contains(row) ==> row_ok(db, parameters, row)
    &&& forall|first: int, last: int| #![trigger rows[first], rows[last]] 0 <= first < last < rows.len()
        ==> !same_group(rows[first], rows[last]) && !row_lt(rows[last], rows[first])
}

pub open spec fn missing_rows(all: Seq<OutRow>, top: Seq<OutRow>) -> Seq<OutRow>
    decreases all.len(),
{
    if all.len() == 0 { Seq::empty() }
    else if top.contains(all.last()) { missing_rows(all.drop_last(), top) }
    else { missing_rows(all.drop_last(), top).push(all.last()) }
}

pub proof fn lemma_extend_top(db: Db, parameters: Params, all: Seq<OutRow>, top: Seq<OutRow>)
    requires result_shape(db, parameters, all), result_shape(db, parameters, top), valid_top(top),
        forall|row: OutRow| #[trigger] row_ok(db, parameters, row) ==> covered(top, row),
    ensures result_shape(db, parameters, top + missing_rows(all, top)),
        forall|row: OutRow| #[trigger] (top + missing_rows(all, top)).contains(row)
            <==> top.contains(row) || all.contains(row),
        top.len() < 20 ==> missing_rows(all, top).len() == 0,
        forall|row: OutRow| #[trigger] missing_rows(all, top).contains(row) ==> all.contains(row) && !top.contains(row),
    decreases all.len(),
{
    hide(row_ok);
    hide(ext_lt);
    lemma_ext_order();
    if all.len() == 0 {
        assert(top + missing_rows(all, top) =~= top);
    } else {
        let prefix = all.drop_last();
        assert(prefix + seq![all.last()] =~= all);
        assert forall|row: OutRow| #[trigger] prefix.contains(row) implies row_ok(db, parameters, row) by {
            let position = choose|position: int| #![trigger prefix[position]] 0 <= position < prefix.len() && prefix[position] == row;
            assert(all[position] == row);
            assert(all.contains(row));
        }
        assert(result_shape(db, parameters, prefix));
        lemma_extend_top(db, parameters, prefix, top);
        let previous = top + missing_rows(prefix, top);
        let row = all.last();
        assert(all.contains(row));
        assert(row_ok(db, parameters, row));
        let full = top + missing_rows(all, top);
        if !top.contains(row) {
            assert(covered(top, row));
            assert(top.len() == 20);
            assert(full =~= previous.push(row));
            assert forall|first: int, last: int| #![trigger full[first], full[last]] 0 <= first < last < full.len()
                implies !same_group(full[first], full[last]) && !row_lt(full[last], full[first]) by {
                if last < previous.len() {
                    assert(full[first] == previous[first] && full[last] == previous[last]);
                } else {
                    let other = previous[first];
                    assert(previous.contains(other));
                    assert(top.contains(other) || prefix.contains(other));
                    if same_group(other, row) {
                        lemma_equal_group_rows(db, parameters, other, row);
                        assert(!top.contains(other));
                    }
                    if top.contains(other) {
                        let position = choose|position: int| #![trigger top[position]] 0 <= position < top.len() && top[position] == other;
                        if row_lt(row, other) {
                            lemma_row_lt_ext(row, other);
                            if position < 19 {
                                assert(ext_order()(top[position], top[19]));
                                assert(ext_order()(row, top[position]));
                                assert(ext_order()(row, top[19]));
                            }
                            assert(false);
                        }
                    } else {
                        let position = choose|position: int| #![trigger prefix[position]] 0 <= position < prefix.len() && prefix[position] == other;
                        assert(all[position] == other && all[all.len() as int - 1] == row);
                    }
                }
            }
            assert forall|value: OutRow| #[trigger] full.contains(value) implies row_ok(db, parameters, value) by {
                lemma_rows_concat_contains(previous, seq![row], value);
                if value != row { assert(previous.contains(value)); }
            }
        } else { assert(full == previous); }
        assert forall|value: OutRow| #[trigger] full.contains(value) <==> top.contains(value) || all.contains(value) by {
            lemma_rows_concat_contains(prefix, seq![row], value);
            if !top.contains(row) {
                assert(full =~= previous + seq![row]);
                lemma_rows_concat_contains(previous, seq![row], value);
            } else {
                assert(full == previous);
                if value == row { assert(top.contains(value)); }
            }
            assert(previous.contains(value) <==> top.contains(value) || prefix.contains(value));
        }
        assert forall|value: OutRow| #[trigger] missing_rows(all, top).contains(value)
            implies all.contains(value) && !top.contains(value) by {
            if !top.contains(row) {
                lemma_rows_concat_contains(missing_rows(prefix, top), seq![row], value);
            }
            if value != row || top.contains(row) {
                assert(missing_rows(prefix, top).contains(value));
                assert(prefix.contains(value));
                let position = choose|position: int| #![trigger prefix[position]] 0 <= position < prefix.len() && prefix[position] == value;
                assert(all[position] == value);
            }
        }
    }
}

pub proof fn lemma_query_from_coverage(db: Db, parameters: Params, top: Seq<OutRow>)
    requires valid_top(top),
        forall|row: OutRow| #[trigger] top.contains(row) ==> row_ok(db, parameters, row),
        forall|row: OutRow| #[trigger] row_ok(db, parameters, row) ==> covered(top, row),
    ensures query_result(db, parameters, top),
{
    hide(row_ok);
    hide(ext_lt);
    lemma_ext_order();
    lemma_answer_ok(db, parameters);
    let all = answer(db, parameters);
    assert forall|row: OutRow| #[trigger] all.contains(row) implies row_ok(db, parameters, row) by {
        let position = choose|position: int| #![trigger all[position]] 0 <= position < all.len() && all[position] == row;
    }
    assert(result_shape(db, parameters, all));
    assert forall|first: int, last: int| #![trigger top[first], top[last]] 0 <= first < last < top.len()
        implies !same_group(top[first], top[last]) && !row_lt(top[last], top[first]) by {
        assert(top.contains(top[first]) && top.contains(top[last]));
        if same_group(top[first], top[last]) { lemma_equal_group_rows(db, parameters, top[first], top[last]); }
        if row_lt(top[last], top[first]) {
            lemma_row_lt_ext(top[last], top[first]);
            assert(ext_order()(top[first], top[last]));
            assert(ext_order()(top[first], top[first]));
        }
    }
    lemma_extend_top(db, parameters, all, top);
    let full = top + missing_rows(all, top);
    assert forall|position: int| 0 <= position < full.len() implies row_ok(db, parameters, #[trigger] full[position]) by {
        assert(full.contains(full[position]));
    }
    assert forall|customer: int, name: Seq<u8>, balance: int, phone: Seq<u8>, nation: Seq<u8>, address: Seq<u8>, comment: Seq<u8>|
        #[trigger] group_count(db, parameters, customer, name, balance, phone, nation, address, comment) > 0
            && having(db, parameters, customer, name, balance, phone, nation, address, comment)
        implies exists|position: int| 0 <= position < full.len() && (#[trigger] full[position]).c_custkey == customer
            && full[position].c_name == name && full[position].c_acctbal == balance && full[position].c_phone == phone
            && full[position].n_name == nation && full[position].c_address == address && full[position].c_comment == comment by {
        let original = choose|position: int| 0 <= position < all.len() && (#[trigger] all[position]).c_custkey == customer
            && all[position].c_name == name && all[position].c_acctbal == balance && all[position].c_phone == phone
            && all[position].n_name == nation && all[position].c_address == address && all[position].c_comment == comment;
        let row = all[original];
        assert(all.contains(row));
        assert(full.contains(row));
        let position = choose|position: int| #![trigger full[position]] 0 <= position < full.len() && full[position] == row;
    }
    assert(full_result(db, parameters, full));
    if top.len() == 20 { assert(full.take(20) =~= top); }
    else { assert(full =~= top); }
    assert(limit_rows(full, 20) == top);
}

}
