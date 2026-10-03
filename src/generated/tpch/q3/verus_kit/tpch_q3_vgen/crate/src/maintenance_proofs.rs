verus! {

pub open spec fn text_extension(before: Index<Vec<u8>>, after: Index<Vec<u8>>) -> bool {
    forall|segment: i64| (#[trigger] before.at(segment)).is_some() ==> after.at(segment) == before.at(segment)
}

pub open spec fn customer_changes_ok(work: Work<Counts>, texts: Index<Vec<u8>>, changes: Seq<(CustomerRow, int)>) -> bool {
    forall|key: i64, segment: i64| #[trigger] work_count(work, key, segment) == match texts.at(segment) {
        Some(text) => sum(changes, customer_delta_weight(key, text@)), None => 0,
    }
}

pub open spec fn customer_delta_weight(key: i64, text: Seq<u8>) -> spec_fn((CustomerRow, int)) -> int {
    |item: (CustomerRow, int)| if item.0.c_custkey == key && item.0.c_mktsegment == text { item.1 } else { 0 }
}

pub open spec fn join_db_counts(index: JoinIndex, texts: Index<Vec<u8>>, db: Db) -> bool {
    forall|segment: i64, date: i32, key: i64, priority: i64|
        #[trigger] h_at(index, segment, date, key, priority) == match texts.at(segment) {
            Some(text) => join_count(db, text@, date as int, key as int, priority as int), None => 0,
        }
}

proof fn lemma_extended_counts(state: &State, names: Index<Vec<Name>>, texts: Index<Vec<u8>>, next: i64)
    requires state.invariant(), dictionary_ok(names, texts, next), text_extension(state.texts, texts),
    ensures customer_db_counts(state.customers, state.db(), |segment: i64| texts.at(segment)),
        join_db_counts(state.joins, texts, state.db()),
        forall|position: int| 0 <= position < state.db().customer.len() ==> has_text(texts, (#[trigger] state.db().customer[position]).c_mktsegment),
{
    hide(h_at);
    hide(sum);
    reveal(State::invariant);
    reveal(State::c_at);
    let text_lookup = |segment: i64| texts.at(segment);
    assert(dictionary_model_ok(|hash: i64| names.at(hash), text_lookup, next));
    assert forall|first: i64, second: i64| first != second && (#[trigger] texts.at(first)).is_some()
        && (#[trigger] texts.at(second)).is_some() implies texts.at(first).unwrap()@ != texts.at(second).unwrap()@ by {
        assert(text_lookup(first).is_some());
        assert(text_lookup(second).is_some());
        assert(text_lookup(first).unwrap()@ != text_lookup(second).unwrap()@);
    }
    assert forall|position: int| 0 <= position < state.db().customer.len() implies
        has_text(texts, (#[trigger] state.db().customer[position]).c_mktsegment) by {
        let segment = choose|segment: i64| (#[trigger] state.texts.at(segment)) matches Some(text)
            && text@ == state.db().customer[position].c_mktsegment;
        assert(texts.at(segment) == state.texts.at(segment));
    }
    assert forall|key: i64, segment: i64| #[trigger] c_index(state.customers, key, segment) ==
        match texts.at(segment) { Some(text) => customer_count(state.db(), key as int, text@), None => 0 } by {
        assert(c_index(state.customers, key, segment) == state.c_at(key, segment));
        if state.texts.at(segment).is_none() {
            if let Some(text) = texts.at(segment) {
                assert forall|position: int| 0 <= position < state.db().customer.len() implies
                    (#[trigger] state.db().customer[position]).c_mktsegment != text@ by {
                    let prior = choose|prior: i64| (#[trigger] state.texts.at(prior)) matches Some(bytes)
                        && bytes@ == state.db().customer[position].c_mktsegment;
                    assert(texts.at(prior) == state.texts.at(prior));
                    assert(prior != segment);
                    assert(texts.at(prior).unwrap()@ != texts.at(segment).unwrap()@);
                }
                lemma_sum_zero(state.db().customer, |row: CustomerRow|
                    if row.c_custkey == key && row.c_mktsegment == text@ { 1int } else { 0 });
            }
        }
    }
    assert forall|segment: i64, date: i32, key: i64, priority: i64|
        #[trigger] h_at(state.joins, segment, date, key, priority) == match texts.at(segment) {
            Some(text) => join_count(state.db(), text@, date as int, key as int, priority as int), None => 0,
        } by {
        if state.texts.at(segment).is_none() {
            if let Some(text) = texts.at(segment) {
                assert forall|position: int| 0 <= position < state.db().orders.len() implies
                    customer_count(state.db(), (#[trigger] state.db().orders[position]).o_custkey, text@) == 0 by {
                    let customer = state.db().orders[position].o_custkey as i64;
                    assert(state.c_at(customer, segment) == 0);
                    assert(c_index(state.customers, customer, segment) == 0);
                }
                lemma_sum_zero(state.db().orders, |row: OrdersRow|
                    if row.o_orderdate == date && row.o_orderkey == key && row.o_shippriority == priority {
                        customer_count(state.db(), row.o_custkey, text@)
                    } else { 0 });
            }
        }
    }
}

proof fn lemma_customer_capture(state: &State, db2: Db, changes: Seq<(CustomerRow, int)>,
    work: Work<Counts>, index: Index<Counts>, names: Index<Vec<Name>>, texts: Index<Vec<u8>>, next: i64)
    requires state.invariant(), bag_step(state.db().customer, db2.customer, changes),
        dictionary_ok(names, texts, next), text_extension(state.texts, texts),
        customer_changes_ok(work, texts, changes),
        customer_texts_known(|segment: i64| texts.at(segment), changes, |item: (CustomerRow, int)| item.0.c_mktsegment),
        index.valid(), forall|key: i64| (#[trigger] index.at(key)) matches Some(rows) ==> counts_ok(rows@),
        forall|key: i64, segment: i64| #[trigger] c_index(index, key, segment) ==
            c_index(state.customers, key, segment) + work_count(work, key, segment),
    ensures customer_index_ok(index, texts, db2),
{
    hide(sum);
    lemma_extended_counts(state, names, texts, next);
    assert forall|key: i64, segment: i64| #[trigger] c_index(index, key, segment) ==
        match texts.at(segment) { Some(text) => customer_count(db2, key as int, text@), None => 0 } by {
        if let Some(text) = texts.at(segment) {
            let weight = |row: CustomerRow| if row.c_custkey == key && row.c_mktsegment == text@ { 1int } else { 0 };
            assert(sum(db2.customer, weight) == sum(state.db().customer, weight)
                + sum(changes, |item: (CustomerRow, int)| item.1 * weight(item.0)));
            lemma_sum_ext_all(changes, |item: (CustomerRow, int)| item.1 * weight(item.0),
                customer_delta_weight(key, text@));
            assert(work_count(work, key, segment) == sum(changes, customer_delta_weight(key, text@)));
        }
    }
    lemma_capture_predicate(state.db().customer, db2.customer, changes, |row: CustomerRow| has_text(texts, row.c_mktsegment));
}

pub open spec fn old_orders(db: Db, customer: i64, key: i64, date: i32, priority: i64) -> int {
    sum(db.orders, order_weight(customer, key, date, priority))
}

pub open spec fn order_weight(customer: i64, key: i64, date: i32, priority: i64) -> spec_fn(OrdersRow) -> int {
    |row: OrdersRow| if row.o_custkey == customer && row.o_orderkey == key
        && row.o_orderdate == date && row.o_shippriority == priority { 1int } else { 0 }
}

pub open spec fn customer_join_part(work: Work<Counts>, neighborhoods: Work<Vec<OrderCount>>,
    keys: Seq<i64>, segment: i64, date: i32, key: i64, priority: i64) -> int {
    sum(keys, |customer: i64| work_count(work, customer, segment)
        * order_work(neighborhoods, customer, key, date, priority))
}

pub open spec fn order_delta_part(index: Index<Counts>, changes: Seq<(OrdersRow, int)>,
    segment: i64, date: i32, key: i64, priority: i64) -> int {
    sum(changes, |item: (OrdersRow, int)| if item.0.o_orderdate == date && item.0.o_orderkey == key
        && item.0.o_shippriority == priority { item.1 * c_index(index, item.0.o_custkey as i64, segment) } else { 0 })
}

spec fn maintenance_context(state: &State, db2: Db, work: Work<Counts>, changes: Seq<(CustomerRow, int)>,
    keys: Seq<i64>, names: Index<Vec<Name>>, texts: Index<Vec<u8>>, next: i64) -> bool {
    &&& state.invariant()
    &&& dictionary_ok(names, texts, next)
    &&& text_extension(state.texts, texts)
    &&& counts_work_ok(work)
    &&& customer_changes_ok(work, texts, changes)
    &&& customer_texts_known(|segment: i64| texts.at(segment), changes, |item: (CustomerRow, int)| item.0.c_mktsegment)
    &&& forall|customer: i64| (#[trigger] work.data@.contains_key(customer)) ==> keys.contains(customer)
    &&& forall|position: int| 0 <= position < changes.len() ==> work.keys@.contains((#[trigger] changes[position]).0.c_custkey as i64)
}

pub proof fn lemma_old_neighborhood(db: Db, db2: Db, changes: Seq<(OrdersRow, int)>, keys: Seq<i64>,
    rows: Seq<CustomerOrdersRescan>, customer: i64, key: i64, date: i32, priority: i64)
    requires bag_step(db.orders, db2.orders, changes), rescan_customer_orders_ok(db2, keys, rows), keys.contains(customer),
    ensures old_orders(db, customer, key, date, priority) ==
        sum(customer_orders_rescan_seq_view(rows), order_weight(customer, key, date, priority))
        - sum(changes, |item: (OrdersRow, int)| item.1 * order_weight(customer, key, date, priority)(item.0)),
{
    hide(sum);
    let weight = order_weight(customer, key, date, priority);
    assert(sum(customer_orders_rescan_seq_view(rows), weight) == sum(db2.orders,
        |row: OrdersRow| if seq_view_i64(keys).contains(row.o_custkey) { weight(row) } else { 0 }));
    let member = choose|position: int| 0 <= position < keys.len() && (#[trigger] keys[position]) == customer;
    assert(seq_view_i64(keys)[member] == customer as int);
    assert(seq_view_i64(keys).contains(customer as int));
    lemma_sum_ext_all(db2.orders,
        |row: OrdersRow| if seq_view_i64(keys).contains(row.o_custkey) { weight(row) } else { 0 }, weight);
    assert(sum(db2.orders, weight) == sum(db.orders, weight) + sum(changes, |item: (OrdersRow, int)| item.1 * weight(item.0)));
}

pub proof fn lemma_customer_join_fold(db: Db, work: Work<Counts>, neighborhoods: Work<Vec<OrderCount>>,
    segment: i64, date: i32, key: i64, priority: i64)
    requires db_bounds(db), work_complete(work),
        forall|customer: i64, other_key: i64, other_date: i32, other_priority: i64|
            work.data@.contains_key(customer) ==> #[trigger] order_work(neighborhoods, customer, other_key, other_date, other_priority)
                == old_orders(db, customer, other_key, other_date, other_priority),
    ensures customer_join_part(work, neighborhoods, work.keys@, segment, date, key, priority) ==
        sum(db.orders, |row: OrdersRow| if row.o_orderdate == date && row.o_orderkey == key && row.o_shippriority == priority {
            work_count(work, row.o_custkey as i64, segment)
        } else { 0 }),
{
    hide(sum);
    let keys = work.keys@;
    let product = |customer: i64, row: OrdersRow| work_count(work, customer, segment) * order_weight(customer, key, date, priority)(row);
    let per_customer = |customer: i64| work_count(work, customer, segment) * order_work(neighborhoods, customer, key, date, priority);
    let per_order = |row: OrdersRow| sum(keys, |customer: i64| product(customer, row));
    assert forall|customer: i64| #[trigger] per_customer(customer) == sum(db.orders, |row: OrdersRow| product(customer, row)) by {
        lemma_sum_scale(db.orders, order_weight(customer, key, date, priority), |row: OrdersRow| product(customer, row), work_count(work, customer, segment));
        if !work.data@.contains_key(customer) { assert(work_count(work, customer, segment) == 0); }
    }
    lemma_sum_swap(keys, db.orders, product, per_customer, per_order);
    let contribution = |row: OrdersRow| if row.o_orderdate == date && row.o_orderkey == key && row.o_shippriority == priority {
        work_count(work, row.o_custkey as i64, segment)
    } else { 0 };
    assert(distinct(keys));
    assert forall|position: int| 0 <= position < db.orders.len() implies
        #[trigger] per_order(db.orders[position]) == contribution(db.orders[position]) by {
        let row = db.orders[position];
        let customer = row.o_custkey as i64;
        assert(customer as int == row.o_custkey);
        let weight = |other: i64| product(other, row);
        assert forall|other: i64| #[trigger] weight(other) == if other == customer { contribution(row) } else { 0 } by {
            if other != customer { assert(order_weight(other, key, date, priority)(row) == 0); }
        }
        if !keys.contains(customer) { assert(!work.data@.contains_key(customer)); }
        lemma_sum_indicator_distinct(keys, customer, contribution(row), weight);
    }
    lemma_sum_ext(db.orders, per_order, contribution);
}

proof fn lemma_join_capture(state: &State, db2: Db, orders: Seq<(OrdersRow, int)>, work: Work<Counts>,
    neighborhoods: Work<Vec<OrderCount>>, customers: Index<Counts>, names: Index<Vec<Name>>, texts: Index<Vec<u8>>, next: i64,
    joins: JoinIndex)
    requires state.invariant(), dictionary_ok(names, texts, next), text_extension(state.texts, texts),
        bag_step(state.db().orders, db2.orders, orders),
        forall|position: int| 0 <= position < orders.len() ==> order_bounds((#[trigger] orders[position]).0),
        counts_work_ok(work), customer_index_ok(customers, texts, db2),
        forall|customer: i64, segment: i64| #[trigger] c_index(customers, customer, segment) ==
            c_index(state.customers, customer, segment) + work_count(work, customer, segment),
        forall|customer: i64, key: i64, date: i32, priority: i64| work.data@.contains_key(customer) ==>
            #[trigger] order_work(neighborhoods, customer, key, date, priority) == old_orders(state.db(), customer, key, date, priority),
        join_index_ok(joins),
        forall|segment: i64, date: i32, key: i64, priority: i64| #[trigger] h_at(joins, segment, date, key, priority) ==
            h_at(state.joins, segment, date, key, priority)
            + customer_join_part(work, neighborhoods, work.keys@, segment, date, key, priority)
            + order_delta_part(customers, orders, segment, date, key, priority),
    ensures join_db_counts(joins, texts, db2),
        forall|segment: i64| (#[trigger] joins.at(segment)).is_some() ==> texts.at(segment).is_some(),
{
    hide(sum);
    hide(h_at);
    reveal(State::invariant);
    lemma_extended_counts(state, names, texts, next);
    assert(customer_db_counts(customers, db2, |segment: i64| texts.at(segment)));
    assert forall|segment: i64, date: i32, key: i64, priority: i64| #[trigger] h_at(joins, segment, date, key, priority) ==
        match texts.at(segment) { Some(text) => join_count(db2, text@, date as int, key as int, priority as int), None => 0 } by {
        lemma_customer_join_fold(state.db(), work, neighborhoods, segment, date, key, priority);
        let difference = |row: OrdersRow| if row.o_orderdate == date && row.o_orderkey == key && row.o_shippriority == priority {
            work_count(work, row.o_custkey as i64, segment)
        } else { 0 };
        let order_changes = |item: (OrdersRow, int)| if item.0.o_orderdate == date && item.0.o_orderkey == key && item.0.o_shippriority == priority {
            item.1 * c_index(customers, item.0.o_custkey as i64, segment)
        } else { 0 };
        if let Some(text) = texts.at(segment) {
            let before = |row: OrdersRow| if row.o_orderdate == date && row.o_orderkey == key && row.o_shippriority == priority {
                customer_count(state.db(), row.o_custkey, text@)
            } else { 0 };
            let after = |row: OrdersRow| if row.o_orderdate == date && row.o_orderkey == key && row.o_shippriority == priority {
                customer_count(db2, row.o_custkey, text@)
            } else { 0 };
            let subtraction = |row: OrdersRow| after(row) - before(row);
            assert forall|position: int| 0 <= position < state.db().orders.len() implies
                #[trigger] difference(state.db().orders[position]) == subtraction(state.db().orders[position]) by {
                let row = state.db().orders[position];
                let customer = row.o_custkey as i64;
                assert(customer as int == row.o_custkey);
                assert(c_index(customers, customer, segment) == customer_count(db2, row.o_custkey, text@));
                assert(c_index(state.customers, customer, segment) == customer_count(state.db(), row.o_custkey, text@));
            }
            lemma_sum_ext(state.db().orders, difference, subtraction);
            lemma_sum_sub(state.db().orders, after, before, subtraction);
            let captured = |item: (OrdersRow, int)| item.1 * after(item.0);
            assert forall|position: int| 0 <= position < orders.len() implies
                #[trigger] order_changes(orders[position]) == captured(orders[position]) by {
                let item = orders[position];
                assert(item.0.o_custkey as i64 as int == item.0.o_custkey);
                assert(c_index(customers, item.0.o_custkey as i64, segment) == customer_count(db2, item.0.o_custkey, text@));
            }
            lemma_sum_ext(orders, order_changes, captured);
            assert(sum(db2.orders, after) == sum(state.db().orders, after) + sum(orders, captured));
        } else {
            assert forall|customer: i64| #[trigger] work_count(work, customer, segment) == 0 by {
                assert(c_index(customers, customer, segment) == 0);
                assert(c_index(state.customers, customer, segment) == 0);
            }
            lemma_sum_zero(state.db().orders, difference);
            lemma_sum_zero(orders, order_changes);
        }
    }
    assert forall|segment: i64| (#[trigger] joins.at(segment)).is_some() implies texts.at(segment).is_some() by {
        let witness = lemma_join_live(joins, segment);
        assert(h_at(joins, segment, witness.0, witness.1, witness.2) > 0);
    }
}

pub fn add_order_segments(work: &mut JoinWork, counts: &Counts, date: i32, key: i64, priority: i64, amount: i128) -> (result: Option<()>)
    requires join_work_ok(*old(work)),
    ensures result.is_some() ==> join_work_ok(*final(work)),
        result.is_some() ==> forall|segment: i64, other_date: i32, other_key: i64, other_priority: i64|
            #[trigger] h_work(*final(work), segment, other_date, other_key, other_priority) ==
            h_work(*old(work), segment, other_date, other_key, other_priority)
            + if other_date == date && other_key == key && other_priority == priority {
                count_at(counts@, segment) * amount as int
            } else { 0 },
{
    let ghost original = *work;
    let mut position = 0;
    let mut processed = Ghost(0int);
    while position < counts.len()
        invariant position <= counts.len(), processed@ == position as int,
            original == *old(work), join_work_ok(*work),
            forall|segment: i64, other_date: i32, other_key: i64, other_priority: i64|
                #[trigger] h_work(*work, segment, other_date, other_key, other_priority) ==
                h_work(original, segment, other_date, other_key, other_priority)
                + if other_date == date && other_key == key && other_priority == priority {
                    count_at(counts@.take(processed@), segment) * amount as int
                } else { 0 },
        decreases counts.len() - position,
    {
        let item = counts[position];
        let product = match item.1.checked_mul(amount) { Some(value) => value, None => return None };
        if crate::joins::change(work, item.0, date, key, priority, product).is_none() { return None; }
        proof {
            assert forall|segment: i64, other_date: i32, other_key: i64, other_priority: i64|
                #[trigger] h_work(*work, segment, other_date, other_key, other_priority) ==
                h_work(original, segment, other_date, other_key, other_priority)
                + if other_date == date && other_key == key && other_priority == priority {
                    count_at(counts@.take(processed@ + 1), segment) * amount as int
                } else { 0 } by {
                lemma_sum_take(counts@, processed@, count_weight(segment));
                assert(count_at(counts@.take(processed@ + 1), segment) * amount as int ==
                    count_at(counts@.take(processed@), segment) * amount as int
                    + if segment == item.0 { product as int } else { 0 }) by (nonlinear_arith)
                    requires count_at(counts@.take(processed@ + 1), segment) == count_at(counts@.take(processed@), segment)
                        + if segment == item.0 { item.1 as int } else { 0 },
                        product as int == item.1 as int * amount as int;
            }
        }
        position += 1;
        processed = Ghost(position as int);
    }
    proof { assert(counts@.take(processed@) =~= counts@); }
    Some(())
}

pub fn add_customer_orders(work: &mut JoinWork, counts: &Counts, rows: &Vec<OrderCount>) -> (result: Option<()>)
    requires join_work_ok(*old(work)),
    ensures result.is_some() ==> join_work_ok(*final(work)),
        result.is_some() ==> forall|segment: i64, date: i32, key: i64, priority: i64|
            #[trigger] h_work(*final(work), segment, date, key, priority) ==
            h_work(*old(work), segment, date, key, priority)
            + count_at(counts@, segment) * order_at(rows@, key, date, priority),
{
    let ghost original = *work;
    let mut position = 0;
    let mut processed = Ghost(0int);
    while position < rows.len()
        invariant position <= rows.len(), processed@ == position as int,
            original == *old(work), join_work_ok(*work),
            forall|segment: i64, date: i32, key: i64, priority: i64|
                #[trigger] h_work(*work, segment, date, key, priority) ==
                h_work(original, segment, date, key, priority)
                + count_at(counts@, segment) * order_at(rows@.take(processed@), key, date, priority),
        decreases rows.len() - position,
    {
        let row = rows[position];
        if row.count < 0 { return None; }
        if add_order_segments(work, counts, row.date, row.key, row.priority, row.count).is_none() { return None; }
        proof {
            assert forall|segment: i64, date: i32, key: i64, priority: i64|
                #[trigger] h_work(*work, segment, date, key, priority) ==
                h_work(original, segment, date, key, priority)
                + count_at(counts@, segment) * order_at(rows@.take(processed@ + 1), key, date, priority) by {
                lemma_sum_take(rows@, processed@, |entry: OrderCount|
                    if entry.key == key && entry.date == date && entry.priority == priority { entry.count as int } else { 0 });
                assert(count_at(counts@, segment) * order_at(rows@.take(processed@ + 1), key, date, priority) ==
                    count_at(counts@, segment) * order_at(rows@.take(processed@), key, date, priority)
                    + if date == row.date && key == row.key && priority == row.priority {
                        count_at(counts@, segment) * row.count as int
                    } else { 0 }) by (nonlinear_arith)
                    requires order_at(rows@.take(processed@ + 1), key, date, priority) ==
                        order_at(rows@.take(processed@), key, date, priority)
                        + if date == row.date && key == row.key && priority == row.priority { row.count as int } else { 0 };
            }
        }
        position += 1;
        processed = Ghost(position as int);
    }
    proof { assert(rows@.take(processed@) =~= rows@); }
    Some(())
}

}
