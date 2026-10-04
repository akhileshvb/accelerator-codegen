use vstd::prelude::*;
use verus_common::order::*;
use verus_common::sums::*;
use verus_prooflib::ordered::{Index, KeyView};
use crate::gen_spec::*;
use crate::gen_obligations::{params, out_view};
use crate::output::*;
use crate::model::*;
use crate::values::*;
use crate::semantics::*;

verus! {

pub proof fn lemma_row_unique(db: Db, parameters: Params, left: OutRow, right: OutRow)
    requires row_ok(db, parameters, left), row_ok(db, parameters, right), same_group(left, right),
    ensures left == right,
{}

pub proof fn lemma_out_membership(rows: Seq<(i64, i128, i32, i64)>)
    ensures forall|row: OutRow| #![trigger out_view(rows).contains(row)]
        out_view(rows).contains(row) == (exists|position: int| #![trigger rows[position]]
            0 <= position < rows.len() && output_row(rows[position]) == row),
{
    let out = out_view(rows);
    assert forall|row: OutRow| #![trigger out.contains(row)] out.contains(row) ==
        (exists|position: int| #![trigger rows[position]] 0 <= position < rows.len() && output_row(rows[position]) == row) by {
        if out.contains(row) {
            let position = choose|position: int| #![trigger out[position]] 0 <= position < out.len() && out[position] == row;
            assert(output_row(rows[position]) == row);
        }
        if exists|position: int| #![trigger rows[position]] 0 <= position < rows.len() && output_row(rows[position]) == row {
            let position = choose|position: int| #![trigger rows[position]] 0 <= position < rows.len() && output_row(rows[position]) == row;
            assert(out[position] == row);
        }
    }
}

pub proof fn lemma_top_result(db: Db, parameters: Params, rows: Seq<(i64, i128, i32, i64)>)
    requires rows.len() <= 10, output_sorted(rows), output_distinct(rows),
        forall|position: int| #![trigger rows[position]] 0 <= position < rows.len() ==> row_ok(db, parameters, output_row(rows[position])),
        forall|row: OutRow| #![trigger row_ok(db, parameters, row)] row_ok(db, parameters, row) ==> top_covers(rows, row),
    ensures query_result(db, parameters, out_view(rows)),
{
    lemma_answer_ok(db, parameters);
    lemma_out_membership(rows);
    let out = out_view(rows);
    let answer_rows = answer(db, parameters);
    let keep = |row: OutRow| !out.contains(row);
    let relation = |left: OutRow, right: OutRow| !same_group(left, right) && !row_lt(right, left);
    assert(sorted_by(answer_rows, relation));
    verus_prooflib::lemma_filter_ordered(answer_rows, keep, relation);
    let rest = answer_rows.filter(keep);
    let full = out + rest;
    assert forall|position: int| #![trigger full[position]] 0 <= position < full.len() implies row_ok(db, parameters, full[position]) by {
        if position < out.len() { assert(out[position] == output_row(rows[position])); }
        else {
            assert(rest.contains(full[position]));
            assert(answer_rows.contains(full[position]));
            let source = choose|source: int| #![trigger answer_rows[source]] 0 <= source < answer_rows.len() && answer_rows[source] == full[position];
        }
    }
    assert forall|left: int, right: int| #![trigger full[left], full[right]] 0 <= left < right < full.len() implies
        !same_group(full[left], full[right]) && !row_lt(full[right], full[left]) by {
        if right < out.len() {
            assert(out[left] == output_row(rows[left]) && out[right] == output_row(rows[right]));
        } else if left >= out.len() {
            assert(relation(rest[left - out.len()], rest[right - out.len()]));
        } else {
            let later = full[right];
            assert(rest.contains(later));
            assert(!out.contains(later));
            assert(out.contains(full[left]));
            if same_group(full[left], later) { lemma_row_unique(db, parameters, full[left], later); }
            assert(top_covers(rows, later));
            assert(rows.len() == 10 && !ext_lt(later, output_row(rows[9])));
            assert(!ext_lt(output_row(rows[9]), output_row(rows[left])));
        }
    }
    assert forall|key: int, date: int, priority: int| #![trigger group_count(db, parameters, key, date, priority)]
        group_count(db, parameters, key, date, priority) > 0 && having(db, parameters, key, date, priority) implies
            exists|position: int| #![trigger full[position]] 0 <= position < full.len()
                && full[position].l_orderkey == key && full[position].o_orderdate == date && full[position].o_shippriority == priority by {
        let source = choose|source: int| #![trigger answer_rows[source]] 0 <= source < answer_rows.len()
            && answer_rows[source].l_orderkey == key && answer_rows[source].o_orderdate == date && answer_rows[source].o_shippriority == priority;
        let row = answer_rows[source];
        if out.contains(row) {
            let position = choose|position: int| #![trigger out[position]] 0 <= position < out.len() && out[position] == row;
            assert(full[position] == row);
        } else {
            assert(rest.contains(row));
            let position = choose|position: int| #![trigger rest[position]] 0 <= position < rest.len() && rest[position] == row;
            assert(full[out.len() + position] == row);
        }
    }
    if rows.len() < 10 && rest.len() > 0 {
        let row = rest[0];
        assert(rest.contains(row));
        assert(row_ok(db, parameters, row));
        assert(top_covers(rows, row));
        assert(!out.contains(row));
    }
    assert(full_result(db, parameters, full));
    assert(full.take(out.len() as int) =~= out);
    if rows.len() < 10 { assert(rest.len() == 0); assert(full =~= out); }
    assert(out == limit_rows(full, 10));
}

pub open spec fn ancestor_node(date: int, level: int) -> int {
    level * 67108864 + ((date - i32::MIN as int) / 64) / pow2(level)
}

pub open spec fn candidate_present(pages: Index<Page>, segment: Seq<u8>, row: OutRow, group: GroupCell) -> bool {
    let page_key = (row.l_orderkey / 256, Seq::<u8>::empty());
    pages.at(page_key).is_some() && (exists|position: int| #![trigger (*pages.at(page_key).unwrap().groups)@[position]]
        0 <= position < (*pages.at(page_key).unwrap().groups)@.len() && (*pages.at(page_key).unwrap().groups)@[position] == group)
        && (*group.segment)@ == segment && group.id.key == row.l_orderkey
        && group.id.date == row.o_orderdate && group.id.priority == row.o_shippriority
}

pub closed spec fn selected_group(pages: Index<Page>, segment: Seq<u8>, row: OutRow) -> GroupCell {
    choose|group: GroupCell| #[trigger] candidate_present(pages, segment, row, group)
}

pub proof fn lemma_candidate_info(customers: Index<CustomerCell>, pages: Index<Page>, ranks: Index<Index<()>>,
    db: Db, segment: Seq<u8>, date: i32, row: OutRow)
    requires state_ok(customers, pages, ranks, db), row_ok(db, params(segment, date as int), row),
    ensures {
        let group = selected_group(pages, segment, row);
        &&& candidate_present(pages, segment, row, group)
        &&& group.weight > 0 && group.bound >= 0 && row.revenue <= group.bound
        &&& group.last_ship.is_some() && group.id.date < date < group.last_ship.unwrap()
        &&& exists|level: int| #![trigger listed(ranks, ancestor_node(date as int, level), segment, group)]
            0 <= level <= 26 && listed(ranks, ancestor_node(date as int, level), segment, group)
    },
{
    let parameters = params(segment, date as int);
    let key = row.l_orderkey;
    let orderdate = row.o_orderdate;
    let priority = row.o_shippriority;
    lemma_sql_group(db, parameters, key, orderdate, priority);
    let weight = weight_sum(db, segment, key, orderdate, priority);
    let count = line_suffix(db, key, date as int, 0);
    lemma_sum_nonneg(db.lineitem, |entry: LineitemRow| if entry.l_orderkey == key && entry.l_shipdate > date { line_measure(entry, 0) } else { 0int });
    assert(weight > 0 && count > 0 && orderdate < date) by (nonlinear_arith)
        requires group_count(db, parameters, key, orderdate, priority) > 0,
            group_count(db, parameters, key, orderdate, priority) == if orderdate < date { weight * count } else { 0int }, count >= 0;
    let page_key = (key / 256, Seq::<u8>::empty());
    assert(group_at(pages, segment, key, orderdate, priority) == weight);
    assert(pages.at(page_key).is_some());
    let page = pages.at(page_key).unwrap();
    let groups = (*page.groups)@;
    let measure = column_measure(segment, key, orderdate, priority);
    assert(group_at(pages, segment, key, orderdate, priority) == sum(groups, measure));
    assert(sum(groups, measure) > 0);
    if !(exists|position: int| #![trigger groups[position]] 0 <= position < groups.len()
        && (*groups[position].segment)@ == segment && groups[position].id.key == key
        && groups[position].id.date == orderdate && groups[position].id.priority == priority) {
        lemma_sum_zero(groups, measure);
    }
    let position = choose|position: int| #![trigger groups[position]] 0 <= position < groups.len()
        && (*groups[position].segment)@ == segment && groups[position].id.key == key
        && groups[position].id.date == orderdate && groups[position].id.priority == priority;
    assert(candidate_present(pages, segment, row, groups[position]));
    reveal(selected_group);
    let group = selected_group(pages, segment, row);
    assert(candidate_present(pages, segment, row, group));
    let source = choose|source: int| #![trigger groups[source]] 0 <= source < groups.len() && groups[source] == group;
    lemma_group_weight(customers, pages, ranks, db, &page, key / 256, source);
    lemma_page_bounds(&page, db, key / 256);
    if group.last_ship.is_none() || group.last_ship.unwrap() <= date {
        lemma_sum_zero(db.lineitem, |entry: LineitemRow| if entry.l_orderkey == key && entry.l_shipdate > date { line_measure(entry, 0) } else { 0int });
    }
    lemma_suffix_bound(db, key, date as int);
    assert(row.revenue <= group.bound) by (nonlinear_arith)
        requires row.revenue == weight * line_suffix(db, key, date as int, 1),
            group.bound == weight * positive_sum(db, key), weight > 0,
            line_suffix(db, key, date as int, 1) <= positive_sum(db, key);
    lemma_group_ancestor(group, date);
    let level = choose|level: int| #![trigger covers(group, ancestor_node(date as int, level))]
        0 <= level <= 26 && covers(group, ancestor_node(date as int, level));
    assert(listed(ranks, ancestor_node(date as int, level), segment, group));
}

pub proof fn lemma_group_ancestor(group: GroupCell, date: i32)
    requires group.last_ship.is_some(), group.id.date < date < group.last_ship.unwrap(),
    ensures exists|level: int| #![trigger covers(group, ancestor_node(date as int, level))]
        0 <= level <= 26 && covers(group, ancestor_node(date as int, level)),
{
    let first = (group.id.date as int + 1 - i32::MIN as int) / 64;
    let end = (group.last_ship.unwrap() as int - 1 - i32::MIN as int) / 64 + 1;
    let point = (date as int - i32::MIN as int) / 64;
    verus_prooflib::lemma_dyadic_width_bounds(0);
    verus_prooflib::lemma_dyadic_width_bounds(26);
    verus_prooflib::lemma_dyadic_ancestor(first, end, point, 0, 26);
    let level = choose|level: int| #![trigger pow2(level)] 0 <= level <= 26 && {
        let width = pow2(level);
        let offset = point / width;
        first <= offset * width && (offset + 1) * width <= end
            && (level == 26 || !(first <= (offset / 2) * (2 * width)
                && (offset / 2 + 1) * (2 * width) <= end))
    };
    verus_prooflib::lemma_dyadic_width_bounds(level);
    let offset = point / pow2(level);
    vstd::arithmetic::div_mod::lemma_div_is_ordered(0, point, pow2(level));
    vstd::arithmetic::div_mod::lemma_fundamental_div_mod(point, pow2(level));
    vstd::arithmetic::div_mod::lemma_mod_bound(point, pow2(level));
    assert(offset * pow2(level) <= point) by (nonlinear_arith)
        requires point == pow2(level) * offset + point % pow2(level), point % pow2(level) >= 0;
    assert(0 <= offset < 67108864) by (nonlinear_arith)
        requires 0 <= point < 67108864, pow2(level) >= 1, offset >= 0, offset * pow2(level) <= point;
    assert(ancestor_node(date as int, level) / 67108864 == level);
    assert(ancestor_node(date as int, level) % 67108864 == offset);
    assert(covers(group, ancestor_node(date as int, level)));
}

}
