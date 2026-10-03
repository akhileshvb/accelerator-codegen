verus! {

#[derive(Clone, Copy)]
pub struct OrderCount {
    pub key: i64,
    pub date: i32,
    pub priority: i64,
    pub count: i128,
}

pub open spec fn order_work(work: Work<Vec<OrderCount>>, customer: i64, key: i64, date: i32, priority: i64) -> int {
    if work.data@.contains_key(customer) {
        order_at(work.data@[customer]@, key, date, priority)
    } else { 0 }
}

pub open spec fn order_at(rows: Seq<OrderCount>, key: i64, date: i32, priority: i64) -> int {
    sum(rows, |row: OrderCount|
        if row.key == key && row.date == date && row.priority == priority { row.count as int } else { 0 })
}

pub fn order_change(rows: &mut Vec<OrderCount>, key: i64, date: i32, priority: i64, amount: i128) -> (result: Option<()>)
    ensures result.is_some() ==> forall|other_key: i64, other_date: i32, other_priority: i64|
        #[trigger] order_at(final(rows)@, other_key, other_date, other_priority) ==
            order_at(old(rows)@, other_key, other_date, other_priority)
            + if other_key == key && other_date == date && other_priority == priority { amount as int } else { 0 },
{
    let ghost original = rows@;
    let mut position = 0;
    while position < rows.len()
        invariant position <= rows.len(), rows@ == original, original == old(rows)@,
        decreases rows.len() - position,
    {
        let row = rows[position];
        if row.key == key && row.date == date && row.priority == priority {
            let count = match row.count.checked_add(amount) { Some(value) => value, None => return None };
            rows.set(position, OrderCount { key, date, priority, count });
            proof {
                assert forall|other_key: i64, other_date: i32, other_priority: i64|
                    #[trigger] order_at(rows@, other_key, other_date, other_priority) ==
                        order_at(original, other_key, other_date, other_priority)
                        + if other_key == key && other_date == date && other_priority == priority { amount as int } else { 0 } by {
                    lemma_sum_update(original, position as int, rows@[position as int], |entry: OrderCount|
                        if entry.key == other_key && entry.date == other_date && entry.priority == other_priority { entry.count as int } else { 0 });
                }
            }
            return Some(());
        }
        position += 1;
    }
    rows.push(OrderCount { key, date, priority, count: amount });
    proof {
        assert forall|other_key: i64, other_date: i32, other_priority: i64|
            #[trigger] order_at(rows@, other_key, other_date, other_priority) ==
                order_at(original, other_key, other_date, other_priority)
                + if other_key == key && other_date == date && other_priority == priority { amount as int } else { 0 } by {
            lemma_sum_push(original, rows@.last(), |entry: OrderCount|
                if entry.key == other_key && entry.date == other_date && entry.priority == other_priority { entry.count as int } else { 0 });
        }
    }
    Some(())
}

pub fn order_change_work(work: &mut Work<Vec<OrderCount>>, customer: i64, key: i64, date: i32, priority: i64, amount: i128) -> (result: Option<()>)
    ensures result.is_some() ==> forall|other_customer: i64, other_key: i64, other_date: i32, other_priority: i64|
        #[trigger] order_work(*final(work), other_customer, other_key, other_date, other_priority) ==
            order_work(*old(work), other_customer, other_key, other_date, other_priority)
            + if other_customer == customer && other_key == key && other_date == date && other_priority == priority { amount as int } else { 0 },
{
    let mut rows = match work.take(customer) { Some(value) => value, None => Vec::new() };
    if order_change(&mut rows, key, date, priority, amount).is_none() { return None; }
    work.put(customer, rows);
    Some(())
}

pub fn apply_inner(
    state: &State, customer_delta: &Vec<CustomerDelta>, lineitem_delta: &[ShipmentDelta], orders_delta: &[OrdersDelta],
    customer_orders_keys: &[i64], customer_orders_rows: &[CustomerOrdersRescan], Ghost(db2): Ghost<Db>,
) -> (result: Option<State>)
    requires
        capture_ok(state.db(), db2, customer_delta_seq_view(customer_delta@), shipment_delta_seq_view(lineitem_delta@), orders_delta_seq_view(orders_delta@)),
        rescan_customer_orders_ok(db2, customer_orders_keys@, customer_orders_rows@),
    ensures result matches Some(next) ==> next.db() == db2 && next.invariant(),
{
    proof { use_type_invariant(state); }
    let mut coverage: Work<bool> = Work::new();
    let mut position = 0;
    let mut processed = Ghost(0int);
    while position < customer_orders_keys.len()
        invariant position <= customer_orders_keys.len(), processed@ == position as int,
            forall|key: i64| #[trigger] coverage.data@.contains_key(key) <==> customer_orders_keys@.take(processed@).contains(key),
        decreases customer_orders_keys.len() - position,
    {
        coverage.put(customer_orders_keys[position], true);
        proof { assert(customer_orders_keys@.take(processed@ + 1) =~= customer_orders_keys@.take(processed@).push(customer_orders_keys@[processed@])); }
        position += 1;
        processed = Ghost(position as int);
    }
    proof { use_type_invariant(state); }
    proof { assert(customer_orders_keys@.take(processed@) =~= customer_orders_keys@); }
    let mut name_delta = Run::new();
    let mut text_delta: Run<Vec<u8>> = Run::new();
    let mut next_segment = state.next_segment;
    proof {
        reveal(State::invariant);
        lemma_dictionary_overlay_empty(state.names, name_delta, state.texts, text_delta, next_segment);
    }
    let mut customer_changes: Work<Counts> = Work::new();
    position = 0;
    processed = Ghost(0int);
    while position < customer_delta.len()
        invariant
            position <= customer_delta.len(), processed@ == position as int,
            run_ok(name_delta), run_ok(text_delta),
            counts_work_ok(customer_changes),
            state.invariant(),
            forall|key: i64| #[trigger] coverage.data@.contains_key(key) <==> customer_orders_keys@.contains(key),
            forall|key: i64| (#[trigger] customer_changes.data@.contains_key(key)) ==> coverage.data@.contains_key(key),
            forall|prior: int| 0 <= prior < processed@ ==> customer_changes.keys@.contains((#[trigger] customer_delta@[prior]).row.c_custkey),
            forall|id: i64| (#[trigger] state.texts.at(id)).is_some() ==> overlay_at(state.texts, text_delta, id) == state.texts.at(id),
            dictionary_overlay_ok(state.names, name_delta, state.texts, text_delta, next_segment),
            customer_prefix(customer_changes, |id: i64| overlay_at(state.texts, text_delta, id),
                customer_delta_seq_view(customer_delta@).take(processed@),
                |item: (CustomerRow, int)| item.0.c_custkey, |item: (CustomerRow, int)| item.0.c_mktsegment,
                |item: (CustomerRow, int)| item.1),
            customer_texts_known(|id: i64| overlay_at(state.texts, text_delta, id),
                customer_delta_seq_view(customer_delta@).take(processed@), |item: (CustomerRow, int)| item.0.c_mktsegment),
        decreases customer_delta.len() - position,
    {
        let item = &customer_delta[position];
        let key = item.row.c_custkey;
        if !coverage.data.contains_key(&key) { return None; }
        let ghost previous_work = customer_changes;
        let ghost previous_texts = text_delta;
        let segment = match intern(&state.names, &state.texts, &mut name_delta, &mut text_delta, &mut next_segment, item.row.c_mktsegment.as_slice()) {
            Some(value) => value, None => return None,
        };
        if add_work(&mut customer_changes, key, segment, item.sign as i128).is_none() { return None; }
        proof {
            assert(work_complete(customer_changes));
            assert(work_complete(previous_work));
            assert(customer_changes.keys@.to_set() =~= previous_work.keys@.to_set().insert(key));
            assert forall|prior: int| 0 <= prior < processed@ + 1 implies
                customer_changes.keys@.contains((#[trigger] customer_delta@[prior]).row.c_custkey) by {
                if prior < processed@ {
                    assert(previous_work.keys@.contains(customer_delta@[prior].row.c_custkey));
                } else { assert(customer_delta@[prior].row.c_custkey == key); }
                assert(customer_changes.keys@.to_set().contains(customer_delta@[prior].row.c_custkey));
            }
            lemma_dictionary_unique(state.names, name_delta, state.texts, text_delta, next_segment);
            lemma_customer_prefix_step(previous_work, customer_changes,
                |id: i64| overlay_at(state.texts, previous_texts, id),
                |id: i64| overlay_at(state.texts, text_delta, id),
                customer_delta_seq_view(customer_delta@).take(processed@), customer_delta_seq_view(customer_delta@)[processed@],
                key, segment, item.sign as int,
                |entry: (CustomerRow, int)| entry.0.c_custkey, |entry: (CustomerRow, int)| entry.0.c_mktsegment,
                |entry: (CustomerRow, int)| entry.1);
            assert(customer_delta_seq_view(customer_delta@).take(processed@ + 1) =~=
                customer_delta_seq_view(customer_delta@).take(processed@).push(customer_delta_seq_view(customer_delta@)[processed@]));
        }
        position += 1;
        processed = Ghost(position as int);
    }
    let names = match state.names.with(name_delta) { Some(value) => value, None => return None };
    let texts = match state.texts.with(text_delta) { Some(value) => value, None => return None };
    proof {
        lemma_dictionary_finished(state.names, name_delta, state.texts, text_delta, next_segment, names, texts);
        assert(customer_delta_seq_view(customer_delta@).take(processed@) =~= customer_delta_seq_view(customer_delta@));
        assert((|id: i64| overlay_at(state.texts, text_delta, id)) =~= (|id: i64| texts.at(id)));
        assert forall|key: i64, segment: i64| #[trigger] work_count(customer_changes, key, segment) == match texts.at(segment) {
            Some(text) => sum(customer_delta_seq_view(customer_delta@), customer_delta_weight(key, text@)), None => 0,
        } by {
            if let Some(text) = texts.at(segment) {
                let key_of = |item: (CustomerRow, int)| item.0.c_custkey;
                let text_of = |item: (CustomerRow, int)| item.0.c_mktsegment;
                let amount_of = |item: (CustomerRow, int)| item.1;
                let weight = |item: (CustomerRow, int)| if key_of(item) == key && text_of(item) == text@ { amount_of(item) } else { 0 };
                assert(work_count(customer_changes, key, segment) == sum(customer_delta_seq_view(customer_delta@), weight));
                lemma_sum_ext_all(customer_delta_seq_view(customer_delta@), weight, customer_delta_weight(key, text@));
            }
        }
        assert(maintenance_context(state, db2, customer_changes, customer_delta_seq_view(customer_delta@), customer_orders_keys@, names, texts, next_segment));
    }
    let mut neighborhoods: Work<Vec<OrderCount>> = Work::new();
    position = 0;
    processed = Ghost(0int);
    while position < customer_orders_rows.len()
        invariant
            position <= customer_orders_rows.len(), processed@ == position as int,
            maintenance_context(state, db2, customer_changes, customer_delta_seq_view(customer_delta@), customer_orders_keys@, names, texts, next_segment),
            forall|customer: i64, key: i64, date: i32, priority: i64|
                #[trigger] order_work(neighborhoods, customer, key, date, priority) ==
                if customer_changes.data@.contains_key(customer) {
                    sum(customer_orders_rescan_seq_view(customer_orders_rows@).take(processed@), |row: OrdersRow|
                        if row.o_custkey == customer && row.o_orderkey == key && row.o_orderdate == date && row.o_shippriority == priority { 1int } else { 0 })
                } else { 0 },
        decreases customer_orders_rows.len() - position,
    {
        let row = customer_orders_rows[position];
        if customer_changes.data.contains_key(&row.o_custkey) {
            if order_change_work(&mut neighborhoods, row.o_custkey, row.o_orderkey, row.o_orderdate, row.o_shippriority, 1).is_none() { return None; }
        }
        proof {
            assert forall|customer: i64, key: i64, date: i32, priority: i64|
                #[trigger] order_work(neighborhoods, customer, key, date, priority) ==
                if customer_changes.data@.contains_key(customer) {
                    sum(customer_orders_rescan_seq_view(customer_orders_rows@).take(processed@ + 1), |item: OrdersRow|
                        if item.o_custkey == customer && item.o_orderkey == key && item.o_orderdate == date && item.o_shippriority == priority { 1int } else { 0 })
                } else { 0 } by {
                lemma_sum_take(customer_orders_rescan_seq_view(customer_orders_rows@), processed@, |item: OrdersRow|
                    if item.o_custkey == customer && item.o_orderkey == key && item.o_orderdate == date && item.o_shippriority == priority { 1int } else { 0 });
            }
        }
        position += 1;
        processed = Ghost(position as int);
    }
    proof { assert(customer_orders_rescan_seq_view(customer_orders_rows@).take(processed@) =~= customer_orders_rescan_seq_view(customer_orders_rows@)); }
    position = 0;
    processed = Ghost(0int);
    while position < orders_delta.len()
        invariant
            position <= orders_delta.len(), processed@ == position as int,
            maintenance_context(state, db2, customer_changes, customer_delta_seq_view(customer_delta@), customer_orders_keys@, names, texts, next_segment),
            forall|customer: i64, key: i64, date: i32, priority: i64|
                #[trigger] order_work(neighborhoods, customer, key, date, priority) ==
                if customer_changes.data@.contains_key(customer) {
                    sum(customer_orders_rescan_seq_view(customer_orders_rows@), |row: OrdersRow|
                        if row.o_custkey == customer && row.o_orderkey == key && row.o_orderdate == date && row.o_shippriority == priority { 1int } else { 0 })
                    - sum(orders_delta_seq_view(orders_delta@).take(processed@), |item: (OrdersRow, int)|
                        if item.0.o_custkey == customer && item.0.o_orderkey == key && item.0.o_orderdate == date && item.0.o_shippriority == priority { item.1 } else { 0 })
                } else { 0 },
        decreases orders_delta.len() - position,
    {
        let item = orders_delta[position];
        if customer_changes.data.contains_key(&item.row.o_custkey) {
            if order_change_work(&mut neighborhoods, item.row.o_custkey, item.row.o_orderkey, item.row.o_orderdate, item.row.o_shippriority, -(item.sign as i128)).is_none() { return None; }
        }
        proof {
            assert forall|customer: i64, key: i64, date: i32, priority: i64|
                #[trigger] order_work(neighborhoods, customer, key, date, priority) ==
                if customer_changes.data@.contains_key(customer) {
                    sum(customer_orders_rescan_seq_view(customer_orders_rows@), |row: OrdersRow|
                        if row.o_custkey == customer && row.o_orderkey == key && row.o_orderdate == date && row.o_shippriority == priority { 1int } else { 0 })
                    - sum(orders_delta_seq_view(orders_delta@).take(processed@ + 1), |entry: (OrdersRow, int)|
                        if entry.0.o_custkey == customer && entry.0.o_orderkey == key && entry.0.o_orderdate == date && entry.0.o_shippriority == priority { entry.1 } else { 0 })
                } else { 0 } by {
                lemma_sum_take(orders_delta_seq_view(orders_delta@), processed@, |entry: (OrdersRow, int)|
                    if entry.0.o_custkey == customer && entry.0.o_orderkey == key && entry.0.o_orderdate == date && entry.0.o_shippriority == priority { entry.1 } else { 0 });
            }
        }
        position += 1;
        processed = Ghost(position as int);
    }
    proof {
        assert(orders_delta_seq_view(orders_delta@).take(processed@) =~= orders_delta_seq_view(orders_delta@));
        assert forall|customer: i64, key: i64, date: i32, priority: i64|
            customer_changes.data@.contains_key(customer) implies
            #[trigger] order_work(neighborhoods, customer, key, date, priority) == old_orders(state.db(), customer, key, date, priority) by {
            lemma_old_neighborhood(state.db(), db2, orders_delta_seq_view(orders_delta@), customer_orders_keys@,
                customer_orders_rows@, customer, key, date, priority);
            lemma_sum_ext_all(customer_orders_rescan_seq_view(customer_orders_rows@),
                |row: OrdersRow| if row.o_custkey == customer && row.o_orderkey == key && row.o_orderdate == date && row.o_shippriority == priority { 1int } else { 0 },
                order_weight(customer, key, date, priority));
            lemma_sum_ext_all(orders_delta_seq_view(orders_delta@),
                |item: (OrdersRow, int)| if item.0.o_custkey == customer && item.0.o_orderkey == key && item.0.o_orderdate == date && item.0.o_shippriority == priority { item.1 } else { 0 },
                |item: (OrdersRow, int)| item.1 * order_weight(customer, key, date, priority)(item.0));
        }
    }
    let ghost signed_customers = customer_changes;
    let ghost old_neighborhoods = neighborhoods;
    let mut join_changes = Work::new();
    position = 0;
    processed = Ghost(0int);
    while position < customer_changes.keys.len()
        invariant position <= customer_changes.keys.len(), processed@ == position as int,
            customer_changes == signed_customers, neighborhoods == old_neighborhoods,
            maintenance_context(state, db2, customer_changes, customer_delta_seq_view(customer_delta@), customer_orders_keys@, names, texts, next_segment),
            forall|customer: i64, key: i64, date: i32, priority: i64|
                customer_changes.data@.contains_key(customer) ==> #[trigger] order_work(neighborhoods, customer, key, date, priority) == old_orders(state.db(), customer, key, date, priority),
            join_work_ok(join_changes),
            forall|segment: i64, date: i32, key: i64, priority: i64|
                #[trigger] h_work(join_changes, segment, date, key, priority) ==
                customer_join_part(customer_changes, neighborhoods, customer_changes.keys@.take(processed@), segment, date, key, priority),
        decreases customer_changes.keys.len() - position,
    {
        let customer = customer_changes.keys[position];
        let counts = match customer_changes.data.get(&customer) { Some(value) => value, None => return None };
        if let Some(rows) = neighborhoods.data.get(&customer) {
            if add_customer_orders(&mut join_changes, counts, rows).is_none() { return None; }
        }
        proof {
            assert forall|segment: i64, date: i32, key: i64, priority: i64|
                #[trigger] h_work(join_changes, segment, date, key, priority) ==
                customer_join_part(customer_changes, neighborhoods, customer_changes.keys@.take(processed@ + 1), segment, date, key, priority) by {
                lemma_sum_take(customer_changes.keys@, processed@, |other: i64|
                    work_count(customer_changes, other, segment) * order_work(neighborhoods, other, key, date, priority));
            }
        }
        position += 1;
        processed = Ghost(position as int);
    }
    proof { assert(customer_changes.keys@.take(processed@) =~= customer_changes.keys@); }
    let ghost first_join_changes = join_changes;
    let customer_index = match finish_customers(&state.customers, customer_changes) { Some(value) => value, None => return None };
    proof {
        lemma_customer_capture(state, db2, customer_delta_seq_view(customer_delta@), signed_customers, customer_index, names, texts, next_segment);
    }
    position = 0;
    processed = Ghost(0int);
    while position < orders_delta.len()
        invariant position <= orders_delta.len(), processed@ == position as int,
            maintenance_context(state, db2, signed_customers, customer_delta_seq_view(customer_delta@), customer_orders_keys@, names, texts, next_segment),
            customer_index_ok(customer_index, texts, db2),
            forall|customer: i64, segment: i64| #[trigger] c_index(customer_index, customer, segment) ==
                c_index(state.customers, customer, segment) + work_count(signed_customers, customer, segment),
            forall|customer: i64, key: i64, date: i32, priority: i64|
                signed_customers.data@.contains_key(customer) ==> #[trigger] order_work(old_neighborhoods, customer, key, date, priority) == old_orders(state.db(), customer, key, date, priority),
            forall|segment: i64, date: i32, key: i64, priority: i64|
                #[trigger] h_work(first_join_changes, segment, date, key, priority) ==
                customer_join_part(signed_customers, old_neighborhoods, signed_customers.keys@, segment, date, key, priority),
            join_work_ok(join_changes),
            forall|segment: i64, date: i32, key: i64, priority: i64|
                #[trigger] h_work(join_changes, segment, date, key, priority) == h_work(first_join_changes, segment, date, key, priority)
                + order_delta_part(customer_index, orders_delta_seq_view(orders_delta@).take(processed@), segment, date, key, priority),
        decreases orders_delta.len() - position,
    {
        let item = orders_delta[position];
        if let Some(counts) = customer_index.get(item.row.o_custkey) {
            if add_order_segments(&mut join_changes, &counts, item.row.o_orderdate, item.row.o_orderkey, item.row.o_shippriority, item.sign as i128).is_none() { return None; }
        }
        proof {
            assert forall|segment: i64, date: i32, key: i64, priority: i64|
                #[trigger] h_work(join_changes, segment, date, key, priority) == h_work(first_join_changes, segment, date, key, priority)
                + order_delta_part(customer_index, orders_delta_seq_view(orders_delta@).take(processed@ + 1), segment, date, key, priority) by {
                lemma_sum_take(orders_delta_seq_view(orders_delta@), processed@, |entry: (OrdersRow, int)|
                    if entry.0.o_orderdate == date && entry.0.o_orderkey == key && entry.0.o_shippriority == priority {
                        entry.1 * c_index(customer_index, entry.0.o_custkey as i64, segment)
                    } else { 0 });
                assert(item.sign as int * c_index(customer_index, item.row.o_custkey, segment) ==
                    c_index(customer_index, item.row.o_custkey, segment) * item.sign as int) by (nonlinear_arith);
            }
        }
        position += 1;
        processed = Ghost(position as int);
    }
    proof { assert(orders_delta_seq_view(orders_delta@).take(processed@) =~= orders_delta_seq_view(orders_delta@)); }
    let join_index = match finish(&state.joins, join_changes) { Some(value) => value, None => return None };
    proof {
        lemma_join_capture(state, db2, orders_delta_seq_view(orders_delta@), signed_customers, old_neighborhoods,
            customer_index, names, texts, next_segment, join_index);
    }
    let mut shipment_work: Work<Vec<Bucket>> = Work::new();
    position = 0;
    processed = Ghost(0int);
    while position < lineitem_delta.len()
        invariant
            position <= lineitem_delta.len(), processed@ == position as int,
            dictionary_ok(names, texts, next_segment),
            customer_index_ok(customer_index, texts, db2),
            join_index_ok(join_index), join_db_counts(join_index, texts, db2),
            forall|segment: i64| (#[trigger] join_index.at(segment)).is_some() ==> texts.at(segment).is_some(),
            state.invariant(),
            bucket_work_ok(shipment_work),
            forall|prior: int| 0 <= prior < processed@ ==>
                shipment_work.data@.contains_key((#[trigger] lineitem_delta@[prior]).row.l_orderkey),
            forall|key: i64, date: i32| shipment_work.data@.contains_key(key) ==>
                #[trigger] bucket_count(shipment_work, key, date) ==
                line_count(state.db(), key as int, date as int) - line_count(state.db(), key as int, date as int + 1)
                + sum(shipment_delta_seq_view(lineitem_delta@.take(processed@)), |item: (LineitemRow, int)|
                    if item.0.l_orderkey == key && item.0.l_shipdate == date { item.1 } else { 0 }),
            forall|key: i64, date: i32| shipment_work.data@.contains_key(key) ==>
                #[trigger] bucket_revenue(shipment_work, key, date) ==
                line_revenue(state.db(), key as int, date as int) - line_revenue(state.db(), key as int, date as int + 1)
                + sum(shipment_delta_seq_view(lineitem_delta@.take(processed@)), |item: (LineitemRow, int)|
                    if item.0.l_orderkey == key && item.0.l_shipdate == date {
                        item.1 * item.0.l_extendedprice * (100 - item.0.l_discount)
                    } else { 0 }),
        decreases lineitem_delta.len() - position,
    {
        let item = lineitem_delta[position];
        let row = item.row;
        let ghost previous_work = shipment_work;
        let revenue = match numerator(row.l_extendedprice, row.l_discount) { Some(value) => value, None => return None };
        let signed_revenue = match revenue.checked_mul(item.sign as i128) { Some(value) => value, None => return None };
        proof {
            assert(signed_revenue as int == item.sign as int * row.l_extendedprice as int * (100 - row.l_discount as int)) by (nonlinear_arith)
                requires revenue as int == row.l_extendedprice as int * (100 - row.l_discount as int),
                    signed_revenue as int == revenue as int * item.sign as int;
        }
        let mut rows = match shipment_work.take(row.l_orderkey) {
            Some(value) => value,
            None => match state.shipments.get(row.l_orderkey) {
                Some(profile) => {
                    let value = match buckets(&profile) { Some(value) => value, None => return None };
                    proof {
                        assert forall|date: i32| #![trigger date_count(value@, date)] #![trigger date_revenue(value@, date)]
                            date_count(value@, date) == line_count(state.db(), row.l_orderkey as int, date as int)
                                - line_count(state.db(), row.l_orderkey as int, date as int + 1)
                            && date_revenue(value@, date) == line_revenue(state.db(), row.l_orderkey as int, date as int)
                                - line_revenue(state.db(), row.l_orderkey as int, date as int + 1) by {
                            lemma_buckets_point(state.db(), row.l_orderkey, profile@, value@, date);
                        }
                    }
                    value
                },
                None => {
                    proof {
                        assert(line_count(state.db(), row.l_orderkey as int, i32::MIN as int) == 0) by {
                            lemma_sum_nonneg(state.db().lineitem, |line: LineitemRow|
                                if line.l_orderkey == row.l_orderkey && line.l_shipdate >= i32::MIN { 1int } else { 0 });
                        }
                        assert forall|date: int| #![trigger line_count(state.db(), row.l_orderkey as int, date)]
                            #![trigger line_revenue(state.db(), row.l_orderkey as int, date)]
                            line_count(state.db(), row.l_orderkey as int, date) == 0 && line_revenue(state.db(), row.l_orderkey as int, date) == 0 by {
                            lemma_line_empty(state.db(), row.l_orderkey, date);
                        }
                    }
                    Vec::new()
                },
            },
        };
        proof { assert(dates_sorted(rows@)); }
        proof {
            assert forall|date: i32| #![trigger date_count(rows@, date)] #![trigger date_revenue(rows@, date)]
                date_count(rows@, date) == line_count(state.db(), row.l_orderkey as int, date as int)
                    - line_count(state.db(), row.l_orderkey as int, date as int + 1)
                    + sum(shipment_delta_seq_view(lineitem_delta@.take(processed@)), |entry: (LineitemRow, int)|
                        if entry.0.l_orderkey == row.l_orderkey && entry.0.l_shipdate == date { entry.1 } else { 0 })
                && date_revenue(rows@, date) == line_revenue(state.db(), row.l_orderkey as int, date as int)
                    - line_revenue(state.db(), row.l_orderkey as int, date as int + 1)
                    + sum(shipment_delta_seq_view(lineitem_delta@.take(processed@)), |entry: (LineitemRow, int)|
                        if entry.0.l_orderkey == row.l_orderkey && entry.0.l_shipdate == date {
                            entry.1 * entry.0.l_extendedprice * (100 - entry.0.l_discount)
                        } else { 0 }) by {
                if !previous_work.data@.contains_key(row.l_orderkey) {
                    assert forall|prior: int| 0 <= prior < processed@ implies
                        (#[trigger] shipment_delta_seq_view(lineitem_delta@.take(processed@))[prior]).0.l_orderkey != row.l_orderkey by {
                        assert(lineitem_delta@[prior].row.l_orderkey != row.l_orderkey);
                    }
                    lemma_sum_zero(shipment_delta_seq_view(lineitem_delta@.take(processed@)), |entry: (LineitemRow, int)|
                        if entry.0.l_orderkey == row.l_orderkey && entry.0.l_shipdate == date { entry.1 } else { 0 });
                    lemma_sum_zero(shipment_delta_seq_view(lineitem_delta@.take(processed@)), |entry: (LineitemRow, int)|
                        if entry.0.l_orderkey == row.l_orderkey && entry.0.l_shipdate == date {
                            entry.1 * entry.0.l_extendedprice * (100 - entry.0.l_discount)
                        } else { 0 });
                } else {
                    assert(rows@ == previous_work.data@[row.l_orderkey]@);
                    assert(bucket_count(previous_work, row.l_orderkey, date) ==
                        line_count(state.db(), row.l_orderkey as int, date as int) - line_count(state.db(), row.l_orderkey as int, date as int + 1)
                        + sum(shipment_delta_seq_view(lineitem_delta@.take(processed@)), |entry: (LineitemRow, int)|
                            if entry.0.l_orderkey == row.l_orderkey && entry.0.l_shipdate == date { entry.1 } else { 0 }));
                    assert(bucket_revenue(previous_work, row.l_orderkey, date) ==
                        line_revenue(state.db(), row.l_orderkey as int, date as int) - line_revenue(state.db(), row.l_orderkey as int, date as int + 1)
                        + sum(shipment_delta_seq_view(lineitem_delta@.take(processed@)), |entry: (LineitemRow, int)|
                            if entry.0.l_orderkey == row.l_orderkey && entry.0.l_shipdate == date {
                                entry.1 * entry.0.l_extendedprice * (100 - entry.0.l_discount)
                            } else { 0 }));
                }
            }
        }
        let ghost previous_rows = rows@;
        if crate::profiles::change(&mut rows, row.l_shipdate, item.sign as i128, signed_revenue).is_none() { return None; }
        let ghost changed_rows = rows@;
        shipment_work.put(row.l_orderkey, rows);
        proof {
            assert(shipment_work.data@.dom() =~= previous_work.data@.dom().insert(row.l_orderkey));
            assert(shipment_work.keys@.to_set() =~= previous_work.keys@.to_set().insert(row.l_orderkey));
            assert(bucket_work_ok(shipment_work));
            assert(shipment_delta_seq_view(lineitem_delta@.take(processed@ + 1)) =~=
                shipment_delta_seq_view(lineitem_delta@.take(processed@)).push((shipment_input_view(item.row), item.sign as int)));
            assert forall|key: i64, date: i32| #![trigger bucket_count(shipment_work, key, date)] #![trigger bucket_revenue(shipment_work, key, date)]
                shipment_work.data@.contains_key(key) implies
                bucket_count(shipment_work, key, date) == line_count(state.db(), key as int, date as int)
                    - line_count(state.db(), key as int, date as int + 1)
                    + sum(shipment_delta_seq_view(lineitem_delta@.take(processed@ + 1)), |entry: (LineitemRow, int)|
                        if entry.0.l_orderkey == key && entry.0.l_shipdate == date { entry.1 } else { 0 })
                && bucket_revenue(shipment_work, key, date) == line_revenue(state.db(), key as int, date as int)
                    - line_revenue(state.db(), key as int, date as int + 1)
                    + sum(shipment_delta_seq_view(lineitem_delta@.take(processed@ + 1)), |entry: (LineitemRow, int)|
                        if entry.0.l_orderkey == key && entry.0.l_shipdate == date {
                            entry.1 * entry.0.l_extendedprice * (100 - entry.0.l_discount)
                        } else { 0 }) by {
                lemma_sum_push(shipment_delta_seq_view(lineitem_delta@.take(processed@)), (shipment_input_view(item.row), item.sign as int),
                    |entry: (LineitemRow, int)| if entry.0.l_orderkey == key && entry.0.l_shipdate == date { entry.1 } else { 0 });
                lemma_sum_push(shipment_delta_seq_view(lineitem_delta@.take(processed@)), (shipment_input_view(item.row), item.sign as int),
                    |entry: (LineitemRow, int)| if entry.0.l_orderkey == key && entry.0.l_shipdate == date {
                        entry.1 * entry.0.l_extendedprice * (100 - entry.0.l_discount)
                    } else { 0 });
                if key != row.l_orderkey {
                    assert(shipment_work.data@[key] == previous_work.data@[key]);
                    assert(bucket_count(previous_work, key, date) ==
                        line_count(state.db(), key as int, date as int) - line_count(state.db(), key as int, date as int + 1)
                        + sum(shipment_delta_seq_view(lineitem_delta@.take(processed@)), |entry: (LineitemRow, int)|
                            if entry.0.l_orderkey == key && entry.0.l_shipdate == date { entry.1 } else { 0 }));
                    assert(bucket_revenue(previous_work, key, date) ==
                        line_revenue(state.db(), key as int, date as int) - line_revenue(state.db(), key as int, date as int + 1)
                        + sum(shipment_delta_seq_view(lineitem_delta@.take(processed@)), |entry: (LineitemRow, int)|
                            if entry.0.l_orderkey == key && entry.0.l_shipdate == date {
                                entry.1 * entry.0.l_extendedprice * (100 - entry.0.l_discount)
                            } else { 0 }));
                } else {
                    assert(date_count(changed_rows, date) == date_count(previous_rows, date)
                        + if date == row.l_shipdate { item.sign as int } else { 0 });
                    assert(date_revenue(changed_rows, date) == date_revenue(previous_rows, date)
                        + if date == row.l_shipdate { signed_revenue as int } else { 0 });
                    assert(date_count(previous_rows, date) == line_count(state.db(), key as int, date as int)
                        - line_count(state.db(), key as int, date as int + 1)
                        + sum(shipment_delta_seq_view(lineitem_delta@.take(processed@)), |entry: (LineitemRow, int)|
                            if entry.0.l_orderkey == key && entry.0.l_shipdate == date { entry.1 } else { 0 }));
                    assert(date_revenue(previous_rows, date) == line_revenue(state.db(), key as int, date as int)
                        - line_revenue(state.db(), key as int, date as int + 1)
                        + sum(shipment_delta_seq_view(lineitem_delta@.take(processed@)), |entry: (LineitemRow, int)|
                            if entry.0.l_orderkey == key && entry.0.l_shipdate == date {
                                entry.1 * entry.0.l_extendedprice * (100 - entry.0.l_discount)
                            } else { 0 }));
                }
            }
        }
        position += 1;
        processed = Ghost(position as int);
    }
    proof {
        assert(lineitem_delta@.take(processed@) =~= lineitem_delta@);
        lemma_capture_bounds(state.db(), db2, customer_delta@, orders_delta@, lineitem_delta@);
        assert forall|key: i64| (#[trigger] shipment_work.data@.contains_key(key)) implies buckets_db(db2, key, shipment_work.data@[key]@) by {
            assert forall|date: i32| #![trigger date_count(shipment_work.data@[key]@, date)] #![trigger date_revenue(shipment_work.data@[key]@, date)]
                date_count(shipment_work.data@[key]@, date) == line_count(db2, key as int, date as int) - line_count(db2, key as int, date as int + 1)
                && date_revenue(shipment_work.data@[key]@, date) == line_revenue(db2, key as int, date as int) - line_revenue(db2, key as int, date as int + 1) by {
                assert(bucket_count(shipment_work, key, date) == date_count(shipment_work.data@[key]@, date));
                assert(bucket_revenue(shipment_work, key, date) == date_revenue(shipment_work.data@[key]@, date));
                lemma_capture_line_point(state.db(), db2, shipment_delta_seq_view(lineitem_delta@), key, date);
            }
        }
        assert forall|key: i64| !(#[trigger] shipment_work.data@.contains_key(key)) implies
            ((state.shipments.at(key) matches Some(rows) ==> rows.len() > 0 && profile_ok(db2, key, rows@))
            && (line_count(db2, key as int, i32::MIN as int) > 0 <==> state.shipments.at(key).is_some())) by {
            assert forall|prior: int| 0 <= prior < lineitem_delta.len() implies
                (#[trigger] shipment_delta_seq_view(lineitem_delta@)[prior]).0.l_orderkey != key by {
                assert(lineitem_delta@[prior].row.l_orderkey != key);
            }
            lemma_capture_line_unchanged(state.db(), db2, shipment_delta_seq_view(lineitem_delta@), key);
            if let Some(rows) = state.shipments.at(key) {
                assert(profile_ok(state.db(), key, rows@));
                assert forall|date: i32| #[trigger] line_count(db2, key as int, date as int) - line_count(db2, key as int, date as int + 1) > 0 <==>
                    exists|entry: int| 0 <= entry < rows.len() && (#[trigger] rows@[entry]).date == date by {
                    assert(line_count(db2, key as int, date as int) == line_count(state.db(), key as int, date as int));
                    assert(line_count(db2, key as int, date as int + 1) == line_count(state.db(), key as int, date as int + 1));
                }
                assert forall|entry: int| 0 <= entry < rows.len() implies
                    (#[trigger] rows@[entry]).count == line_count(db2, key as int, rows@[entry].date as int)
                    && rows@[entry].revenue == line_revenue(db2, key as int, rows@[entry].date as int) && rows@[entry].count > 0 by {
                    assert(line_count(db2, key as int, rows@[entry].date as int) == line_count(state.db(), key as int, rows@[entry].date as int));
                    assert(line_revenue(db2, key as int, rows@[entry].date as int) == line_revenue(state.db(), key as int, rows@[entry].date as int));
                }
            }
        }
        assert(shipment_finish_ok(state.shipments, shipment_work, db2));
    }
    let shipment_index = match finish_shipments(&state.shipments, shipment_work, Ghost(db2)) { Some(value) => value, None => return None };
    Some(State {
        database: Ghost(db2), customers: customer_index, joins: join_index, shipments: shipment_index,
        names, texts, next_segment,
    })
}

}
