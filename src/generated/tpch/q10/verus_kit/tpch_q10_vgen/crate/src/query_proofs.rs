use vstd::prelude::*;
use crate::gen_spec::*;
use crate::dimensions::*;
use crate::model::*;
use verus_common::rel::flat;

verus! {

pub proof fn lemma_output_prepend_contains(head: OutRow, tail: Seq<OutRow>, value: OutRow)
    ensures (seq![head] + tail).contains(value) == (head == value || tail.contains(value)),
{
    let rows = seq![head] + tail;
    if rows.contains(value) {
        let position = choose|position: int| #![trigger rows[position]] 0 <= position < rows.len() && rows[position] == value;
        if position > 0 {
            assert(tail[position - 1] == value);
            assert(tail.contains(value));
        }
    }
    if value == head { assert(rows[0] == value); }
    if tail.contains(value) {
        let position = choose|position: int| #![trigger tail[position]] 0 <= position < tail.len() && tail[position] == value;
        assert(rows[position + 1] == value);
    }
}

pub proof fn lemma_candidate_insert_support(rows: Seq<OutRow>, row: OutRow)
    ensures forall|value: OutRow| #[trigger] verus_common::order::insert_by(rows, row, ext_order()).contains(value)
        <==> rows.contains(value) || value == row,
    decreases rows.len(),
{
    let inserted = verus_common::order::insert_by(rows, row, ext_order());
    if rows.len() == 0 {
        assert(inserted =~= seq![row]);
    } else if ext_lt(row, rows[0]) {
        assert(inserted =~= seq![row] + rows);
    } else {
        lemma_candidate_insert_support(rows.drop_first(), row);
        assert(rows =~= seq![rows[0]] + rows.drop_first());
        assert(inserted =~= seq![rows[0]] + verus_common::order::insert_by(rows.drop_first(), row, ext_order()));
    }
    assert forall|value: OutRow| #[trigger] inserted.contains(value) <==> rows.contains(value) || value == row by {
        if rows.len() == 0 {
            lemma_output_prepend_contains(row, Seq::empty(), value);
        } else if ext_lt(row, rows[0]) {
            lemma_output_prepend_contains(row, rows, value);
        } else {
            let tail = verus_common::order::insert_by(rows.drop_first(), row, ext_order());
            lemma_output_prepend_contains(rows[0], rows.drop_first(), value);
            lemma_output_prepend_contains(rows[0], tail, value);
            assert(tail.contains(value) <==> rows.drop_first().contains(value) || value == row);
        }
    }
}

pub proof fn lemma_limit_support(rows: Seq<OutRow>, value: OutRow)
    requires limit_rows(rows, 20).contains(value),
    ensures rows.contains(value),
{
    if rows.len() > 20 {
        let position = choose|position: int| #![trigger rows.take(20)[position]]
            0 <= position < 20 && rows.take(20)[position] == value;
        assert(rows[position] == value);
    }
}

pub proof fn lemma_candidate_bounded_order(rows: Seq<OutRow>, row: OutRow)
    requires verus_common::order::nondesc(rows, ext_order()),
    ensures verus_common::order::nondesc(limit_rows(verus_common::order::insert_by(rows, row, ext_order()), 20), ext_order()),
{
    lemma_ext_order();
    assert forall|other: OutRow| rows.contains(other) && other != row
        implies #[trigger] ext_order()(row, other) || ext_order()(other, row) by {
        if other.revenue == row.revenue { lemma_ext_total(row, other); }
    }
    verus_common::order::lemma_insert_by_dups(rows, row, ext_order());
    let inserted = verus_common::order::insert_by(rows, row, ext_order());
    let prefix = limit_rows(inserted, 20);
    assert forall|first: int, last: int| #![trigger prefix[first], prefix[last]]
        0 <= first < last < prefix.len() implies !ext_lt(prefix[last], prefix[first]) by {
        assert(prefix[first] == inserted[first]);
        assert(prefix[last] == inserted[last]);
    }
}

pub proof fn lemma_candidate_insert_position(rows: Seq<OutRow>, row: OutRow, position: int)
    requires 0 <= position <= rows.len(),
        forall|index: int| #![trigger rows[index]] 0 <= index < position ==> !ext_lt(row, rows[index]),
        position < rows.len() ==> ext_lt(row, rows[position]),
    ensures verus_common::order::insert_by(rows, row, ext_order()) == rows.insert(position, row),
    decreases rows.len(),
{
    if rows.len() == 0 {
        assert(rows.insert(position, row) =~= seq![row]);
    } else if position == 0 {
        assert(rows.insert(0, row) =~= seq![row] + rows);
    } else {
        assert forall|index: int| #![trigger rows.drop_first()[index]] 0 <= index < position - 1
            implies !ext_lt(row, rows.drop_first()[index]) by {
            assert(rows.drop_first()[index] == rows[index + 1]);
        }
        assert(position - 1 < rows.drop_first().len() ==> ext_lt(row, rows.drop_first()[position - 1]));
        lemma_candidate_insert_position(rows.drop_first(), row, position - 1);
        assert(rows.insert(position, row) =~= seq![rows[0]] + rows.drop_first().insert(position - 1, row));
    }
}

pub proof fn lemma_empty_result(db: Db, parameters: Params)
    requires tuples(db, parameters).len() == 0,
    ensures query_result(db, parameters, Seq::empty()),
{
    assert forall|customer: int, name: Seq<u8>, balance: int, phone: Seq<u8>, nation: Seq<u8>, address: Seq<u8>, comment: Seq<u8>|
        #[trigger] group_count(db, parameters, customer, name, balance, phone, nation, address, comment) == 0 by {}
    assert(full_result(db, parameters, Seq::empty()));
}

pub proof fn lemma_empty_lines(db: Db, parameters: Params, customer: CustomerRow, order: OrdersRow)
    requires parameters.p1 >= parameters.p2 || db.nation.len() == 0,
    ensures tuples_2(db, parameters, customer, order).len() == 0,
{
    assert forall|line: LineitemRow| #[trigger] tuples_3(db, parameters, customer, order, line).len() == 0 by {
        assert forall|nation: NationRow| #[trigger] tuples_4(db, parameters, customer, order, line, nation).len() == 0
            || db.nation.len() == 0 by {}
        if db.nation.len() == 0 {
            assert(db.nation.map_values(|nation: NationRow| tuples_4(db, parameters, customer, order, line, nation)).len() == 0);
        } else {
            verus_prooflib::lemma_flat_map_empty(db.nation,
                |nation: NationRow| tuples_4(db, parameters, customer, order, line, nation));
        }
    }
    verus_prooflib::lemma_flat_map_empty(db.lineitem, |line: LineitemRow| tuples_3(db, parameters, customer, order, line));
}

pub proof fn lemma_empty_query(db: Db, parameters: Params)
    requires parameters.p1 >= parameters.p2 || db.customer.len() == 0 || db.nation.len() == 0,
    ensures query_result(db, parameters, Seq::empty()),
{
    if db.customer.len() == 0 {
        assert(db.customer.map_values(|customer: CustomerRow| tuples_1(db, parameters, customer)).len() == 0);
    } else {
        assert forall|customer: CustomerRow| #[trigger] tuples_1(db, parameters, customer).len() == 0 by {
            assert forall|order: OrdersRow| #[trigger] tuples_2(db, parameters, customer, order).len() == 0 by {
                lemma_empty_lines(db, parameters, customer, order);
            }
            verus_prooflib::lemma_flat_map_empty(db.orders, |order: OrdersRow| tuples_2(db, parameters, customer, order));
        }
        verus_prooflib::lemma_flat_map_empty(db.customer, |customer: CustomerRow| tuples_1(db, parameters, customer));
    }
    lemma_empty_result(db, parameters);
}

pub proof fn lemma_empty_dimensions(db: Db, dimensions: Dimensions)
    requires dimensions_valid(db, dimensions),
        (forall|key: (int, Seq<u8>)| #[trigger] dimensions.customer_maxima.at(key) == None)
            || (forall|key: (int, Seq<u8>)| #[trigger] dimensions.nation_maxima.at(key) == None),
    ensures db.customer.len() == 0 || db.nation.len() == 0,
{
    reveal(dimensions_valid);
    if db.customer.len() > 0 && db.nation.len() > 0 {
        let customer = db.customer[0];
        let nation = db.nation[0];
        verus_common::sums::lemma_sum_pos(db.customer,
            |row: CustomerRow| if row.c_custkey == customer.c_custkey { 1int } else { 0int }, 0);
        verus_common::sums::lemma_sum_pos(db.nation,
            |row: NationRow| if row.n_nationkey == nation.n_nationkey { 1int } else { 0int }, 0);
        assert(total_at(dimensions.customer_totals, customer.c_custkey) > 0);
        assert(total_at(dimensions.nation_totals, nation.n_nationkey) > 0);
        let customer_total = dimensions.customer_totals.at((customer.c_custkey, Seq::empty())).unwrap();
        let nation_total = dimensions.nation_totals.at((nation.n_nationkey, Seq::empty())).unwrap();
        assert(dimensions.customer_maxima.at(crate::values::rank_view(customer_total as int, customer.c_custkey)).is_some());
        assert(dimensions.nation_maxima.at(crate::values::rank_view(nation_total as int, nation.n_nationkey)).is_some());
    }
}

}
