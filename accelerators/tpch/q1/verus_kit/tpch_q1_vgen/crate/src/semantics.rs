use vstd::prelude::*;
use verus_common::bytes::*;
use verus_common::rat::*;
use verus_common::rel::*;
use verus_common::sums::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::values::*;
use crate::kernel::Output;

verus! {

pub open spec fn dates_ok(rows: Seq<LineitemRow>) -> bool {
    forall|index: int| 0 <= index < rows.len() ==>
        -2147483648 <= (#[trigger] rows[index]).l_shipdate <= 2147483647
}

pub open spec fn bad_date(row: LineitemRow) -> int {
    if -2147483648 <= row.l_shipdate <= 2147483647 { 0 } else { 1 }
}

pub proof fn lemma_dates_zero(rows: Seq<LineitemRow>)
    requires sum(rows, |row: LineitemRow| bad_date(row)) == 0,
    ensures dates_ok(rows),
{
    assert forall|index: int| 0 <= index < rows.len() implies
        -2147483648 <= (#[trigger] rows[index]).l_shipdate <= 2147483647 by {
        lemma_sum_ge_term(rows, |row: LineitemRow| bad_date(row), index);
    }
}

pub proof fn lemma_scan_dates(rows: Seq<LineitemInput>, db: Db)
    requires scan_lineitem_build_ok(db, rows),
    ensures dates_ok(db.lineitem),
{
    let projected = lineitem_input_seq_view(rows);
    assert forall|index: int| 0 <= index < projected.len() implies #[trigger] bad_date(projected[index]) == 0 by {
        assert(projected[index] == lineitem_input_view(rows[index]));
    }
    lemma_sum_zero(projected, |row: LineitemRow| bad_date(row));
    assert(sum(db.lineitem, |row: LineitemRow| bad_date(row)) == sum(projected, |row: LineitemRow| bad_date(row)));
    lemma_dates_zero(db.lineitem);
}

pub proof fn lemma_capture_dates(before: Db, after: Db, delta: Seq<LineitemDelta>)
    requires dates_ok(before.lineitem), capture_ok(before, after, lineitem_delta_seq_view(delta)),
    ensures dates_ok(after.lineitem),
{
    lemma_sum_zero(before.lineitem, |row: LineitemRow| bad_date(row));
    let projected = lineitem_delta_seq_view(delta);
    let change = |item: (LineitemRow, int)| item.1 * bad_date(item.0);
    assert forall|index: int| 0 <= index < projected.len() implies #[trigger] change(projected[index]) == 0 by {
        assert(projected[index] == (lineitem_input_view(delta[index].row), delta[index].sign as int));
    }
    lemma_sum_zero(projected, change);
    let f = |row: LineitemRow| bad_date(row);
    assert(sum(after.lineitem, f) == sum(before.lineitem, f) + sum(projected, |item: (LineitemRow, int)| item.1 * f(item.0)));
    lemma_sum_ext_all(projected, |item: (LineitemRow, int)| item.1 * f(item.0), change);
    lemma_dates_zero(after.lineitem);
}

pub open spec fn group_weight(flag: Seq<u8>, status: Seq<u8>, component: int) -> spec_fn(Group) -> int {
    |group: Group| if group.key() == (flag, status) { group.totals.at(component) } else { 0 }
}

pub open spec fn aggregate(groups: Seq<Group>, flag: Seq<u8>, status: Seq<u8>, component: int) -> int {
    sum(groups, group_weight(flag, status, component))
}

pub open spec fn prefix_weight(cutoff: int, flag: Seq<u8>, status: Seq<u8>, component: int) -> spec_fn(LineitemRow) -> int {
    |row: LineitemRow| if row.l_shipdate <= cutoff && row.l_returnflag == flag && row.l_linestatus == status {
        weight(row, component)
    } else { 0 }
}

pub open spec fn prefix_sum(db: Db, cutoff: int, flag: Seq<u8>, status: Seq<u8>, component: int) -> int {
    sum(db.lineitem, prefix_weight(cutoff, flag, status, component))
}

pub proof fn lemma_query_weight(db: Db, parameter: int, flag: Seq<u8>, status: Seq<u8>, f: spec_fn(Tuple) -> int)
    ensures group_sum(db, params(parameter), flag, status, f)
        == sum(db.lineitem, |row: LineitemRow| if row.l_shipdate <= 10561 - parameter
            && row.l_returnflag == flag && row.l_linestatus == status { f(Tuple { lineitem: row }) } else { 0 }),
{
    let p = params(parameter);
    let tuple_weight = |tuple: Tuple| if tuple.lineitem.l_returnflag == flag && tuple.lineitem.l_linestatus == status { f(tuple) } else { 0 };
    let expand = |row: LineitemRow| tuples_1(db, p, row);
    assert(db.lineitem.map_values(expand) =~= db.lineitem.map_values(|row: LineitemRow| tuples_1(db, p, row)));
    assert(tuples(db, p) == flat(db.lineitem.map_values(expand)));
    lemma_sum_flat_map(db.lineitem, expand, tuple_weight);
    let projected = |row: LineitemRow| if row.l_shipdate <= 10561 - parameter
        && row.l_returnflag == flag && row.l_linestatus == status { f(Tuple { lineitem: row }) } else { 0 };
    assert forall|row: LineitemRow| #[trigger] sum(expand(row), tuple_weight) == projected(row) by {
        lemma_sum_opt(where_clause(db, p, row), Tuple { lineitem: row }, tuple_weight);
    }
    lemma_sum_ext_all(db.lineitem, |row: LineitemRow| sum(expand(row), tuple_weight), projected);
}

pub proof fn lemma_query_sums(db: Db, parameter: int, flag: Seq<u8>, status: Seq<u8>, component: int)
    requires 0 <= component < 6,
    ensures group_sum(db, params(parameter), flag, status, |tuple: Tuple| weight(tuple.lineitem, component))
        == prefix_sum(db, 10561 - parameter, flag, status, component),
        component == 0 ==> group_count(db, params(parameter), flag, status)
            == prefix_sum(db, 10561 - parameter, flag, status, 0),
        component == 1 ==> group_sum(db, params(parameter), flag, status, |tuple: Tuple| tuple.lineitem.l_quantity)
            == prefix_sum(db, 10561 - parameter, flag, status, 1),
        component == 2 ==> group_sum(db, params(parameter), flag, status, |tuple: Tuple| tuple.lineitem.l_extendedprice)
            == prefix_sum(db, 10561 - parameter, flag, status, 2),
        component == 3 ==> group_sum(db, params(parameter), flag, status, |tuple: Tuple| tuple.lineitem.l_extendedprice * (100 - tuple.lineitem.l_discount))
            == prefix_sum(db, 10561 - parameter, flag, status, 3),
        component == 4 ==> group_sum(db, params(parameter), flag, status, |tuple: Tuple| tuple.lineitem.l_extendedprice * (100 - tuple.lineitem.l_discount) * (100 + tuple.lineitem.l_tax))
            == prefix_sum(db, 10561 - parameter, flag, status, 4),
        component == 5 ==> group_sum(db, params(parameter), flag, status, |tuple: Tuple| tuple.lineitem.l_discount)
            == prefix_sum(db, 10561 - parameter, flag, status, 5),
{
    let f = |tuple: Tuple| weight(tuple.lineitem, component);
    lemma_query_weight(db, parameter, flag, status, f);
    lemma_sum_ext_all(db.lineitem, |row: LineitemRow| if row.l_shipdate <= 10561 - parameter
        && row.l_returnflag == flag && row.l_linestatus == status { f(Tuple { lineitem: row }) } else { 0 },
        prefix_weight(10561 - parameter, flag, status, component));
    let actual = if component == 0 { |tuple: Tuple| 1int }
        else if component == 1 { |tuple: Tuple| tuple.lineitem.l_quantity }
        else if component == 2 { |tuple: Tuple| tuple.lineitem.l_extendedprice }
        else if component == 3 { |tuple: Tuple| tuple.lineitem.l_extendedprice * (100 - tuple.lineitem.l_discount) }
        else if component == 4 { |tuple: Tuple| tuple.lineitem.l_extendedprice * (100 - tuple.lineitem.l_discount) * (100 + tuple.lineitem.l_tax) }
        else { |tuple: Tuple| tuple.lineitem.l_discount };
    lemma_query_weight(db, parameter, flag, status, actual);
    lemma_sum_ext_all(db.lineitem, |row: LineitemRow| if row.l_shipdate <= 10561 - parameter
        && row.l_returnflag == flag && row.l_linestatus == status { actual(Tuple { lineitem: row }) } else { 0 },
        prefix_weight(10561 - parameter, flag, status, component));
    if component == 0 {
        lemma_sum_ext_all(tuples(db, params(parameter)),
            |tuple: Tuple| if tuple.lineitem.l_returnflag == flag && tuple.lineitem.l_linestatus == status { 1int } else { 0 },
            |tuple: Tuple| if tuple.lineitem.l_returnflag == flag && tuple.lineitem.l_linestatus == status { actual(tuple) } else { 0 });
    }
}

pub open spec fn group_lt(left: Group, right: Group) -> bool {
    bytes_lt(left.key().0, right.key().0)
        || (left.key().0 == right.key().0 && bytes_lt(left.key().1, right.key().1))
}

pub open spec fn group_directory(groups: Seq<Group>) -> bool {
    &&& forall|index: int| 0 <= index < groups.len() ==> (#[trigger] groups[index]).totals.count > 0
    &&& forall|first: int, second: int| 0 <= first < second < groups.len() ==>
        (#[trigger] groups[first]).key() != (#[trigger] groups[second]).key()
}

pub open spec fn group_row(group: Group) -> OutRow {
    OutRow { l_returnflag: group.key().0, l_linestatus: group.key().1,
        sum_qty: group.totals.quantity as int, sum_base_price: group.totals.price as int,
        sum_disc_price: group.totals.discounted as int, sum_charge: group.totals.charge as int,
        avg_qty: Rat { num: group.totals.quantity as int, den: 100 * group.totals.count },
        avg_price: Rat { num: group.totals.price as int, den: 100 * group.totals.count },
        avg_disc: Rat { num: group.totals.discount as int, den: 100 * group.totals.count },
        count_order: group.totals.count as int }
}

pub proof fn lemma_group_at(groups: Seq<Group>, index: int, component: int)
    requires group_directory(groups), 0 <= index < groups.len(),
    ensures aggregate(groups, groups[index].key().0, groups[index].key().1, component) == groups[index].totals.at(component),
{
    let f = group_weight(groups[index].key().0, groups[index].key().1, component);
    assert forall|other: int| 0 <= other < groups.len() && other != index implies #[trigger] f(groups[other]) == 0 by {
        if other < index { assert(groups[other].key() != groups[index].key()); }
        else { assert(groups[index].key() != groups[other].key()); }
    }
    lemma_sum_single_match(groups, f, index);
}

pub proof fn lemma_execute_result(db: Db, parameter: int, groups: Seq<Group>, output: Seq<Output>)
    requires group_directory(groups),
        forall|flag: Seq<u8>, status: Seq<u8>, component: int| 0 <= component < 6 ==>
            #[trigger] aggregate(groups, flag, status, component) == prefix_sum(db, 10561 - parameter, flag, status, component),
        forall|first: int, second: int| 0 <= first < second < groups.len() ==>
            !#[trigger] group_lt(groups[second], groups[first]),
        output.len() == groups.len(),
        forall|index: int| 0 <= index < output.len() ==> (#[trigger] out_view(output)[index]) == group_row(groups[index]),
    ensures query_result(db, params(parameter), out_view(output)),
{
    let rows = out_view(output);
    assert forall|index: int| 0 <= index < rows.len() implies row_ok(db, params(parameter), #[trigger] rows[index]) by {
        let group = groups[index];
        assert forall|component: int| 0 <= component < 6 implies
            #[trigger] prefix_sum(db, 10561 - parameter, group.key().0, group.key().1, component) == group.totals.at(component) by {
            lemma_group_at(groups, index, component);
        }
        lemma_query_sums(db, parameter, group.key().0, group.key().1, 0);
        lemma_query_sums(db, parameter, group.key().0, group.key().1, 1);
        lemma_query_sums(db, parameter, group.key().0, group.key().1, 2);
        lemma_query_sums(db, parameter, group.key().0, group.key().1, 3);
        lemma_query_sums(db, parameter, group.key().0, group.key().1, 4);
        lemma_query_sums(db, parameter, group.key().0, group.key().1, 5);
        assert(group_count(db, params(parameter), group.key().0, group.key().1) == group.totals.count);
        assert(group_sum(db, params(parameter), group.key().0, group.key().1, |tuple: Tuple| tuple.lineitem.l_quantity)
            == group.totals.quantity);
        assert(group_sum(db, params(parameter), group.key().0, group.key().1, |tuple: Tuple| tuple.lineitem.l_extendedprice)
            == group.totals.price);
        assert(group_sum(db, params(parameter), group.key().0, group.key().1,
            |tuple: Tuple| tuple.lineitem.l_extendedprice * (100 - tuple.lineitem.l_discount)) == group.totals.discounted);
        assert(group_sum(db, params(parameter), group.key().0, group.key().1,
            |tuple: Tuple| tuple.lineitem.l_extendedprice * (100 - tuple.lineitem.l_discount) * (100 + tuple.lineitem.l_tax)) == group.totals.charge);
        assert(group_sum(db, params(parameter), group.key().0, group.key().1, |tuple: Tuple| tuple.lineitem.l_discount)
            == group.totals.discount);
    }
    assert forall|first: int, second: int| 0 <= first < second < rows.len() implies
        !same_group(#[trigger] rows[first], #[trigger] rows[second]) && !row_lt(rows[second], rows[first]) by {
        assert(rows[first] == group_row(groups[first]));
        assert(rows[second] == group_row(groups[second]));
        assert(groups[first].key() != groups[second].key());
        assert(!group_lt(groups[second], groups[first]));
    }
    assert forall|flag: Seq<u8>, status: Seq<u8>| #[trigger] group_count(db, params(parameter), flag, status) > 0
        && having(db, params(parameter), flag, status) implies
        exists|index: int| 0 <= index < rows.len() && (#[trigger] rows[index]).l_returnflag == flag && rows[index].l_linestatus == status by {
        lemma_query_sums(db, parameter, flag, status, 0);
        assert(aggregate(groups, flag, status, 0) == prefix_sum(db, 10561 - parameter, flag, status, 0));
        let index = lemma_sum_nonzero_witness(groups, group_weight(flag, status, 0));
        assert(group_weight(flag, status, 0)(groups[index]) != 0);
        assert(rows[index].l_returnflag == flag && rows[index].l_linestatus == status);
    }
}

}
