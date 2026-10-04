use vstd::prelude::*;
use verus_common::sums::*;
use verus_prooflib::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::model::*;
use crate::storage::*;

verus! {

pub open spec fn selected_weight(part: PartRow, line: LineitemRow, class: u8, brand: Seq<u8>, parameter: int, revenue: bool) -> int {
    if class_matches(part, class) && part.p_brand == brand && part.p_partkey == line.l_partkey && eligible(line)
        && parameter <= line.l_quantity < parameter + 1001 { line_weight(line, revenue) } else { 0 }
}

pub open spec fn selected(db: Db, class: u8, brand: Seq<u8>, parameter: int, revenue: bool) -> int {
    sum(db.lineitem, |line: LineitemRow| sum(db.part, |part: PartRow| selected_weight(part, line, class, brand, parameter, revenue)))
}

pub proof fn lemma_window_database(db: Db, class: u8, brand: Seq<u8>, parameter: int, revenue: bool)
    ensures sum(rng(1001), |offset: int| hist_sum(db, class, brand, parameter + offset, revenue))
        == selected(db, class, brand, parameter, revenue),
{
    reveal(hist_sum);
    let by_offset = |offset: int| hist_sum(db, class, brand, parameter + offset, revenue);
    let matrix = |offset: int, part: PartRow| sum(db.lineitem, |line: LineitemRow|
        cell_weight(part, line, class, brand, parameter + offset, revenue));
    let by_part = |part: PartRow| sum(rng(1001), |offset: int| matrix(offset, part));
    assert forall|offset: int| #[trigger] by_offset(offset) == sum(db.part, |part: PartRow| matrix(offset, part)) by {
        lemma_sum_ext_all(db.part, |part: PartRow| sum(db.lineitem, |line: LineitemRow| cell_weight(part, line, class, brand, parameter + offset, revenue)),
            |part: PartRow| matrix(offset, part));
    }
    lemma_sum_swap(rng(1001), db.part, matrix, by_offset, by_part);
    let selected_part = |part: PartRow| sum(db.lineitem, |line: LineitemRow| selected_weight(part, line, class, brand, parameter, revenue));
    assert forall|part: PartRow| #[trigger] by_part(part) == selected_part(part) by {
        let inner = |offset: int, line: LineitemRow| cell_weight(part, line, class, brand, parameter + offset, revenue);
        let by_line = |line: LineitemRow| sum(rng(1001), |offset: int| inner(offset, line));
        assert forall|offset: int| #[trigger] matrix(offset, part) == sum(db.lineitem, |line: LineitemRow| inner(offset, line)) by {
            lemma_sum_ext_all(db.lineitem, |line: LineitemRow| cell_weight(part, line, class, brand, parameter + offset, revenue),
                |line: LineitemRow| inner(offset, line));
        }
        lemma_sum_swap(rng(1001), db.lineitem, inner, |offset: int| matrix(offset, part), by_line);
        assert forall|line: LineitemRow| #[trigger] by_line(line) == selected_weight(part, line, class, brand, parameter, revenue) by {
            let value = if class_matches(part, class) && part.p_brand == brand && part.p_partkey == line.l_partkey && eligible(line) {
                line_weight(line, revenue)
            } else { 0 };
            lemma_sum_ext_all(rng(1001), |offset: int| inner(offset, line), |offset: int| if parameter + offset == line.l_quantity { value } else { 0 });
            lemma_range_single_cell(1001, line.l_quantity, parameter, value);
        }
        lemma_sum_ext_all(db.lineitem, by_line, |line: LineitemRow| selected_weight(part, line, class, brand, parameter, revenue));
    }
    lemma_sum_ext_all(db.part, by_part, selected_part);
    let selected_matrix = |part: PartRow, line: LineitemRow| selected_weight(part, line, class, brand, parameter, revenue);
    assert forall|part: PartRow| #[trigger] selected_part(part) == sum(db.lineitem, |line: LineitemRow| selected_matrix(part, line)) by {
        lemma_sum_ext_all(db.lineitem, |line: LineitemRow| selected_weight(part, line, class, brand, parameter, revenue),
            |line: LineitemRow| selected_matrix(part, line));
    }
    let selected_line = |line: LineitemRow| sum(db.part, |part: PartRow| selected_weight(part, line, class, brand, parameter, revenue));
    assert forall|line: LineitemRow| #[trigger] selected_line(line) == sum(db.part, |part: PartRow| selected_matrix(part, line)) by {
        lemma_sum_ext_all(db.part, |part: PartRow| selected_weight(part, line, class, brand, parameter, revenue),
            |part: PartRow| selected_matrix(part, line));
    }
    lemma_sum_swap(db.part, db.lineitem, selected_matrix, selected_part,
        selected_line);
}

pub proof fn lemma_classes_disjoint(part: PartRow, first: u8, second: u8)
    requires class_matches(part, first), class_matches(part, second),
    ensures first == second,
{
    if first == 0 {
        if second == 1 {} else if second == 2 {}
    } else if first == 1 {
        if second == 0 {} else if second == 2 {}
    } else if first == 2 {
        if second == 0 {} else if second == 1 {}
    }
}

pub proof fn lemma_query_point(db: Db, parameters: Params, part: PartRow, line: LineitemRow, revenue: bool)
    ensures (if where_clause(db, parameters, line, part) { line_weight(line, revenue) } else { 0 })
        == selected_weight(part, line, 0, parameters.p1, parameters.p2, revenue)
            + selected_weight(part, line, 1, parameters.p3, parameters.p4, revenue)
            + selected_weight(part, line, 2, parameters.p5, parameters.p6, revenue),
{
    if class_matches(part, 0) {
        if class_matches(part, 1) { lemma_classes_disjoint(part, 0, 1); }
        if class_matches(part, 2) { lemma_classes_disjoint(part, 0, 2); }
    } else if class_matches(part, 1) {
        if class_matches(part, 2) { lemma_classes_disjoint(part, 1, 2); }
    }
}

pub proof fn lemma_query_sum(db: Db, parameters: Params, revenue: bool)
    ensures sum(tuples(db, parameters), |tuple: Tuple| line_weight(tuple.lineitem, revenue))
        == selected(db, 0, parameters.p1, parameters.p2, revenue)
            + selected(db, 1, parameters.p3, parameters.p4, revenue)
            + selected(db, 2, parameters.p5, parameters.p6, revenue),
{
    let tuple_weight = |tuple: Tuple| line_weight(tuple.lineitem, revenue);
    let first = |line: LineitemRow| sum(db.part, |part: PartRow| selected_weight(part, line, 0, parameters.p1, parameters.p2, revenue));
    let second = |line: LineitemRow| sum(db.part, |part: PartRow| selected_weight(part, line, 1, parameters.p3, parameters.p4, revenue));
    let third = |line: LineitemRow| sum(db.part, |part: PartRow| selected_weight(part, line, 2, parameters.p5, parameters.p6, revenue));
    let total = |line: LineitemRow| first(line) + second(line) + third(line);
    assert forall|line: LineitemRow| #[trigger] total(line) == sum(tuples_1(db, parameters, line), tuple_weight) by {
        let point = |part: PartRow| if where_clause(db, parameters, line, part) { line_weight(line, revenue) } else { 0 };
        assert forall|part: PartRow| #[trigger] point(part) == sum(tuples_2(db, parameters, line, part), tuple_weight) by {
            if where_clause(db, parameters, line, part) {
                lemma_sum_singleton(Tuple { lineitem: line, part }, tuple_weight);
            }
        }
        lemma_flat_map_weight(db.part, |part: PartRow| tuples_2(db, parameters, line, part), tuple_weight, point);
        assert forall|part: PartRow| #[trigger] point(part)
            == selected_weight(part, line, 0, parameters.p1, parameters.p2, revenue)
                + selected_weight(part, line, 1, parameters.p3, parameters.p4, revenue)
                + selected_weight(part, line, 2, parameters.p5, parameters.p6, revenue) by {
            lemma_query_point(db, parameters, part, line, revenue);
        }
        let one = |part: PartRow| selected_weight(part, line, 0, parameters.p1, parameters.p2, revenue);
        let two = |part: PartRow| selected_weight(part, line, 1, parameters.p3, parameters.p4, revenue);
        let three = |part: PartRow| selected_weight(part, line, 2, parameters.p5, parameters.p6, revenue);
        lemma_sum_add(db.part, one, two, |part: PartRow| one(part) + two(part));
        lemma_sum_add(db.part, |part: PartRow| one(part) + two(part), three, point);
    }
    lemma_flat_map_weight(db.lineitem, |line: LineitemRow| tuples_1(db, parameters, line), tuple_weight, total);
    lemma_sum_add(db.lineitem, first, second, |line: LineitemRow| first(line) + second(line));
    lemma_sum_add(db.lineitem, |line: LineitemRow| first(line) + second(line), third, total);
}

pub proof fn lemma_execute_result(db: Db, histogram: Store<HistCell>, parameters: Params, count: int, revenue: int)
    requires
        forall|class: u8, brand: Seq<u8>, quantity: int, is_revenue: bool|
            #![trigger hist_value(histogram, class, brand, quantity, is_revenue)]
            hist_value(histogram, class, brand, quantity, is_revenue) == hist_sum(db, class, brand, quantity, is_revenue),
        count == window_prefix(histogram, 0, parameters.p1, parameters.p2, 1001, false)
            + window_prefix(histogram, 1, parameters.p3, parameters.p4, 1001, false)
            + window_prefix(histogram, 2, parameters.p5, parameters.p6, 1001, false),
        revenue == window_prefix(histogram, 0, parameters.p1, parameters.p2, 1001, true)
            + window_prefix(histogram, 1, parameters.p3, parameters.p4, 1001, true)
            + window_prefix(histogram, 2, parameters.p5, parameters.p6, 1001, true),
    ensures query_result(db, parameters, seq![OutRow { revenue: if count == 0 { None } else { Some(revenue) } }]),
{
    assert forall|class: u8, brand: Seq<u8>, parameter: int, is_revenue: bool|
        #![trigger window_prefix(histogram, class, brand, parameter, 1001, is_revenue)]
        window_prefix(histogram, class, brand, parameter, 1001, is_revenue) == selected(db, class, brand, parameter, is_revenue) by {
        lemma_sum_ext_all(rng(1001), |offset: int| hist_value(histogram, class, brand, parameter + offset, is_revenue),
            |offset: int| hist_sum(db, class, brand, parameter + offset, is_revenue));
        lemma_window_database(db, class, brand, parameter, is_revenue);
    }
    lemma_query_sum(db, parameters, false);
    lemma_query_sum(db, parameters, true);
    lemma_sum_ext_all(tuples(db, parameters), |tuple: Tuple| line_weight(tuple.lineitem, false), |tuple: Tuple| 1);
    lemma_sum_ones(tuples(db, parameters));
    lemma_sum_ext_all(tuples(db, parameters), |tuple: Tuple| line_weight(tuple.lineitem, true),
        |tuple: Tuple| tuple.lineitem.l_extendedprice * (100 - tuple.lineitem.l_discount));
}

}
