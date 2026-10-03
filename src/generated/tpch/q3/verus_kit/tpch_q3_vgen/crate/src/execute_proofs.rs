verus! {

pub open spec fn top_ten(rows: Seq<Row>) -> Seq<Row> {
    rows.take(if rows.len() < 10 { rows.len() as int } else { 10 })
}

pub open spec fn candidates(db: Db, parameters: Params, rows: Seq<Row>) -> bool {
    &&& verus_common::order::sorted_by(rows, selection_order())
    &&& forall|position: int| 0 <= position < rows.len() ==> row_ok(db, parameters, row_view(#[trigger] rows[position]))
}

pub open spec fn processed_group(completed_dates: Seq<i64>, date: Option<i64>, completed_keys: Seq<i64>, key: Option<i64>, completed_priorities: Seq<(i64, i128)>, query_date: i32, query_key: i64, query_priority: i64) -> bool {
    completed_dates.contains(query_date as i64) || (date == Some(query_date as i64) &&
        (completed_keys.contains(query_key) || (key == Some(query_key) &&
            exists|position: int| 0 <= position < completed_priorities.len() && (#[trigger] completed_priorities[position]).0 == query_priority)))
}

pub open spec fn candidate_frontier(rows: Seq<Row>, completed_dates: Seq<i64>, date: Option<i64>, completed_keys: Seq<i64>, key: Option<i64>, completed_priorities: Seq<(i64, i128)>) -> bool {
    forall|position: int| 0 <= position < rows.len() ==>
        processed_group(completed_dates, date, completed_keys, key, completed_priorities,
            (#[trigger] rows[position]).2, rows[position].0, rows[position].3)
}

closed spec fn ready(state: State, segment: i64, parameters: Params, date: i32, key: i64, priority: i64) -> bool {
    date < parameters.p2 && h_at(state.joins, segment, date, key, priority) > 0
        && line_count(state.db(), key as int, parameters.p2 + 1) > 0
}

pub open spec fn contains_group(rows: Seq<Row>, db: Db, parameters: Params, date: i32, key: i64, priority: i64) -> bool {
    out_view(rows).contains(row_of(db, parameters, key as int, date as int, priority as int))
}

closed spec fn candidate_coverage(state: State, segment: i64, parameters: Params, rows: Seq<Row>, completed_dates: Seq<i64>, date: Option<i64>, completed_keys: Seq<i64>, key: Option<i64>, completed_priorities: Seq<(i64, i128)>) -> bool {
    forall|query_date: i32, query_key: i64, query_priority: i64|
        #[trigger] ready(state, segment, parameters, query_date, query_key, query_priority)
        && processed_group(completed_dates, date, completed_keys, key, completed_priorities, query_date, query_key, query_priority)
        ==> contains_group(rows, state.db(), parameters, query_date, query_key, query_priority)
}

proof fn lemma_selection_order()
    ensures verus_common::order::strict_order(selection_order()),
        forall|left: Row, right: Row| left != right ==> #[trigger] selection_order()(left, right) || selection_order()(right, left),
{
}

proof fn lemma_candidate_insert(db: Db, parameters: Params, rows: Seq<Row>, row: Row)
    requires candidates(db, parameters, rows), row_ok(db, parameters, row_view(row)), !rows.contains(row),
    ensures candidates(db, parameters, insert_row(rows, row)),
        forall|value: Row| #[trigger] insert_row(rows, row).contains(value) <==> rows.contains(value) || value == row,
        forall|value: OutRow| #[trigger] out_view(insert_row(rows, row)).contains(value) <==>
            out_view(rows).contains(value) || value == row_view(row),
{
    lemma_selection_order();
    verus_common::order::lemma_insert_by(rows, row, selection_order());
    let inserted = insert_row(rows, row);
    assert forall|position: int| 0 <= position < inserted.len() implies row_ok(db, parameters, row_view(#[trigger] inserted[position])) by {
        assert(inserted.contains(inserted[position]));
        if inserted[position] != row {
            let previous = choose|previous: int| 0 <= previous < rows.len() && (#[trigger] rows[previous]) == inserted[position];
        }
    }
    assert forall|value: OutRow| #[trigger] out_view(inserted).contains(value) <==>
        out_view(rows).contains(value) || value == row_view(row) by {
        if out_view(inserted).contains(value) {
            let position = choose|position: int| 0 <= position < inserted.len() && (#[trigger] out_view(inserted)[position]) == value;
            assert(inserted.contains(inserted[position]));
            if inserted[position] != row {
                assert(rows.contains(inserted[position]));
                let previous = choose|previous: int| 0 <= previous < rows.len() && (#[trigger] rows[previous]) == inserted[position];
                assert(out_view(rows)[previous] == value);
            }
        }
        if out_view(rows).contains(value) {
            let previous = choose|previous: int| 0 <= previous < rows.len() && (#[trigger] out_view(rows)[previous]) == value;
            assert(inserted.contains(rows[previous]));
            let position = choose|position: int| 0 <= position < inserted.len() && (#[trigger] inserted[position]) == rows[previous];
            assert(out_view(inserted)[position] == value);
        }
        if value == row_view(row) {
            assert(inserted.contains(row));
            let position = choose|position: int| 0 <= position < inserted.len() && (#[trigger] inserted[position]) == row;
            assert(out_view(inserted)[position] == value);
        }
    }
}

proof fn lemma_selection_prefix(rows: Seq<Row>, row: Row, output: Seq<Row>, next: Seq<Row>)
    requires output == top_ten(rows),
        next == insert_row(output, row).take(if output.len() < 10 { output.len() as int + 1 } else { 10 }),
    ensures next == top_ten(insert_row(rows, row)),
{
    crate::proof_support::lemma_insert_by_length(rows, row, selection_order());
    if rows.len() < 10 {
        assert(top_ten(rows) =~= rows);
    } else {
        crate::proof_support::lemma_insert_by_prefix(rows, row, selection_order(), 10);
    }
}

proof fn lemma_count_item(rows: Seq<(i64, i128)>, position: int)
    requires counts_unique(rows), 0 <= position < rows.len(),
    ensures count_at(rows, rows[position].0) == rows[position].1,
{
    let weight = count_weight(rows[position].0);
    lemma_sum_single_match(rows, weight, position);
}

proof fn lemma_candidate_row(state: State, segment: i64, parameters: Params, date: i32, key: i64, priority: i64, amount: i128, lines: Bucket, revenue: i128)
    requires state.invariant(), state.texts.at(segment).is_some(), state.texts.at(segment).unwrap()@ == parameters.p1,
        date < parameters.p2, h_at(state.joins, segment, date, key, priority) == amount, amount > 0,
        lines.count == line_count(state.db(), key as int, parameters.p2 + 1), lines.count > 0,
        lines.revenue == line_revenue(state.db(), key as int, parameters.p2 + 1), revenue as int == amount as int * lines.revenue as int,
    ensures row_ok(state.db(), parameters, row_view((key, revenue, date, priority))),
        row_view((key, revenue, date, priority)) == row_of(state.db(), parameters, key as int, date as int, priority as int),
{
    lemma_group_count_factor(state.db(), parameters, key as int, date as int, priority as int);
    lemma_group_revenue_factor(state.db(), parameters, key as int, date as int, priority as int);
    assert(group_count(state.db(), parameters, key as int, date as int, priority as int) > 0) by (nonlinear_arith)
        requires group_count(state.db(), parameters, key as int, date as int, priority as int) == amount as int * lines.count as int,
            amount > 0, lines.count > 0;
}

proof fn lemma_candidate_step(state: State, segment: i64, parameters: Params, rows: Seq<Row>, row: Row, completed_dates: Seq<i64>, date: i64, completed_keys: Seq<i64>, key: i64, completed_priorities: Seq<(i64, i128)>, item: (i64, i128))
    requires candidates(state.db(), parameters, rows), row_ok(state.db(), parameters, row_view(row)),
        candidate_frontier(rows, completed_dates, Some(date), completed_keys, Some(key), completed_priorities),
        candidate_coverage(state, segment, parameters, rows, completed_dates, Some(date), completed_keys, Some(key), completed_priorities),
        row.0 == key, row.2 as i64 == date, row.3 == item.0,
        !processed_group(completed_dates, Some(date), completed_keys, Some(key), completed_priorities, row.2, key, item.0),
    ensures candidates(state.db(), parameters, insert_row(rows, row)),
        candidate_frontier(insert_row(rows, row), completed_dates, Some(date), completed_keys, Some(key), completed_priorities.push(item)),
        candidate_coverage(state, segment, parameters, insert_row(rows, row), completed_dates, Some(date), completed_keys, Some(key), completed_priorities.push(item)),
{
    assert(!rows.contains(row)) by {
        if rows.contains(row) {
            let position = choose|position: int| 0 <= position < rows.len() && (#[trigger] rows[position]) == row;
        }
    }
    lemma_candidate_insert(state.db(), parameters, rows, row);
    let next = insert_row(rows, row);
    assert forall|position: int| 0 <= position < next.len() implies
        processed_group(completed_dates, Some(date), completed_keys, Some(key), completed_priorities.push(item),
            (#[trigger] next[position]).2, next[position].0, next[position].3) by {
        assert(next.contains(next[position]));
        if next[position] == row {
            assert(completed_priorities.push(item)[completed_priorities.len() as int].0 == item.0);
        } else {
            let previous = choose|previous: int| 0 <= previous < rows.len() && (#[trigger] rows[previous]) == next[position];
            assert(processed_group(completed_dates, Some(date), completed_keys, Some(key), completed_priorities,
                rows[previous].2, rows[previous].0, rows[previous].3));
            if !completed_dates.contains(rows[previous].2 as i64) && !completed_keys.contains(rows[previous].0) {
                let prior = choose|prior: int| 0 <= prior < completed_priorities.len() && (#[trigger] completed_priorities[prior]).0 == rows[previous].3;
                assert(completed_priorities.push(item)[prior].0 == rows[previous].3);
            }
        }
    }
    assert forall|query_date: i32, query_key: i64, query_priority: i64|
        #[trigger] ready(state, segment, parameters, query_date, query_key, query_priority)
        && processed_group(completed_dates, Some(date), completed_keys, Some(key), completed_priorities.push(item), query_date, query_key, query_priority)
        implies contains_group(next, state.db(), parameters, query_date, query_key, query_priority) by {
        if processed_group(completed_dates, Some(date), completed_keys, Some(key), completed_priorities, query_date, query_key, query_priority) {
            assert(contains_group(rows, state.db(), parameters, query_date, query_key, query_priority));
            assert(out_view(next).contains(row_of(state.db(), parameters, query_key as int, query_date as int, query_priority as int)));
        } else {
            assert(query_date as i64 == date && query_key == key && query_priority == item.0);
            assert(row_view(row) == row_of(state.db(), parameters, query_key as int, query_date as int, query_priority as int));
        }
    }
}

proof fn lemma_group_supported(state: State, segment: i64, parameters: Params, key: int, date: int, priority: int)
    requires state.invariant(), state.texts.at(segment).is_some(), state.texts.at(segment).unwrap()@ == parameters.p1,
        group_count(state.db(), parameters, key, date, priority) > 0,
    ensures i64::MIN <= key <= i64::MAX, i32::MIN <= date <= i32::MAX, i64::MIN <= priority <= i64::MAX,
        ready(state, segment, parameters, date as i32, key as i64, priority as i64),
{
    hide(sum);
    let db = state.db();
    lemma_group_count_factor(db, parameters, key, date, priority);
    assert(date < parameters.p2);
    let joins = join_count(db, parameters.p1, date, key, priority);
    let lines = line_count(db, key, parameters.p2 + 1);
    lemma_sum_nonneg(db.lineitem, |line: LineitemRow| if line.l_orderkey == key && line.l_shipdate >= parameters.p2 + 1 { 1int } else { 0 });
    assert(joins > 0 && lines > 0) by (nonlinear_arith)
        requires lines >= 0, group_count(db, parameters, key, date, priority) == joins * lines,
            group_count(db, parameters, key, date, priority) > 0;
    let weight = |order: OrdersRow| if order.o_orderdate == date && order.o_orderkey == key && order.o_shippriority == priority {
        customer_count(db, order.o_custkey, parameters.p1)
    } else { 0 };
    let position = lemma_sum_nonzero_witness(db.orders, weight);
    assert(db.orders[position].o_orderdate == date && db.orders[position].o_orderkey == key && db.orders[position].o_shippriority == priority);
}

proof fn lemma_execute_complete(state: State, segment: i64, parameters: Params, full: Seq<Row>, output: Seq<Row>)
    requires state.invariant(), state.texts.at(segment).is_some(), state.texts.at(segment).unwrap()@ == parameters.p1,
        candidates(state.db(), parameters, full), output == top_ten(full),
        forall|date: i32, key: i64, priority: i64| #[trigger] ready(state, segment, parameters, date, key, priority) ==>
            contains_group(full, state.db(), parameters, date, key, priority),
    ensures query_result(state.db(), parameters, out_view(output)),
{
    let viewed = out_view(full);
    assert forall|first: int, second: int| 0 <= first < second < viewed.len() implies
        !same_group(#[trigger] viewed[first], #[trigger] viewed[second]) && !row_lt(viewed[second], viewed[first]) by {
        assert(selection_order()(full[first], full[second]));
        assert(row_ok(state.db(), parameters, viewed[first]) && row_ok(state.db(), parameters, viewed[second]));
        if same_group(viewed[first], viewed[second]) {
            assert(viewed[first].revenue == viewed[second].revenue);
            assert(full[first] == full[second]);
        }
    }
    assert forall|key: int, date: int, priority: int| #[trigger] group_count(state.db(), parameters, key, date, priority) > 0
        && having(state.db(), parameters, key, date, priority) implies
        exists|position: int| 0 <= position < viewed.len() && (#[trigger] viewed[position]).l_orderkey == key
            && viewed[position].o_orderdate == date && viewed[position].o_shippriority == priority by {
        lemma_group_supported(state, segment, parameters, key, date, priority);
        assert(contains_group(full, state.db(), parameters, date as i32, key as i64, priority as i64));
        let position = choose|position: int| 0 <= position < viewed.len() && (#[trigger] viewed[position]) ==
            row_of(state.db(), parameters, key, date, priority);
    }
    assert(full_result(state.db(), parameters, viewed));
    assert(out_view(output) =~= limit_rows(viewed, 10));
    assert(exists|witness: Seq<OutRow>| #[trigger] full_result(state.db(), parameters, witness) && out_view(output) == limit_rows(witness, 10));
}

}
