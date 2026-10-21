use super::*;

verus! {

pub(super) type Cursor = (int, int, int, int);
pub(super) type Payload = (i128, i64, OrderVariant, NameVariant);

pub(super) open spec fn before(position: Cursor, limit: Cursor) -> bool {
    position.0 < limit.0 || position.0 == limit.0 &&
        (position.1 < limit.1 || position.1 == limit.1 &&
        (position.2 < limit.2 || position.2 == limit.2 && position.3 < limit.3))
}

pub(super) open spec fn payload(state: State, position: Cursor) -> Option<Payload> {
    if 0 <= position.0 < state.directory.len() {
        let quantity = state.directory@[position.0];
        match state.buckets.at(quantity) {
            Some(bucket) => if 0 <= position.1 < bucket.len {
                let key = bucket.view()[position.1];
                match state.order_at(key as int) {
                    Some(order) => if 0 <= position.2 < order.variants.len() {
                        let variant = order.variants@[position.2];
                        match state.customer_at(variant.customer as int) {
                            Some(customer) => if 0 <= position.3 < customer.names.len() {
                                Some((quantity, key, variant, customer.names@[position.3]))
                            } else { None },
                            None => None,
                        }
                    } else { None },
                    None => None,
                }
            } else { None },
            None => None,
        }
    } else { None }
}

pub(super) open spec fn slot(state: State, parameters: Params, position: Cursor) -> Option<OutRow> {
    match payload(state, position) {
        Some((quantity, key, variant, name)) => if quantity > parameters.p1 {
            Some(OutRow { c_name: name.name@, c_custkey: variant.customer as int, o_orderkey: key as int,
                o_orderdate: variant.date as int, o_totalprice: variant.price as int,
                sum_qty: quantity as int * variant.count@ * name.count@ })
        } else { None },
        None => None,
    }
}

pub(super) open spec fn slot_row(state: State, parameters: Params, position: Cursor) -> OutRow {
    match slot(state, parameters, position) {
        Some(row) => row,
        None => OutRow { c_name: Seq::empty(), c_custkey: 0, o_orderkey: 0, o_orderdate: 0, o_totalprice: 0, sum_qty: 0 },
    }
}

pub(super) open spec fn model(state: State, parameters: Params, rows: Seq<OutRow>, limit: Cursor) -> bool {
    &&& forall|index: int| 0 <= index < rows.len() ==> row_ok(state.db(), parameters, #[trigger] rows[index])
    &&& forall|left: int, right: int| 0 <= left < right < rows.len() ==> !same_group(#[trigger] rows[left], #[trigger] rows[right])
    &&& forall|row: OutRow| #[trigger] rows.contains(row) ==> exists|position: Cursor| #[trigger] slot(state, parameters, position) == Some(row) && before(position, limit)
    &&& forall|position: Cursor| #[trigger] slot(state, parameters, position) is Some && before(position, limit) ==> rows.contains(slot_row(state, parameters, position))
}

pub(super) proof fn slot_correct(state: State, parameters: Params, position: Cursor)
    requires state.wf(), slot(state, parameters, position) is Some,
    ensures row_ok(state.db(), parameters, slot_row(state, parameters, position)),
{
    let quantity = state.directory@[position.0];
    let bucket = choose|bucket: KeyBucket| #[trigger] state.buckets.at(quantity) == Some(bucket);
    let key = bucket.view()[position.1];
    let order = choose|order: OrderData| #[trigger] state.order_at(key as int) == Some(order);
    let variant = order.variants@[position.2];
    let customer = choose|customer: CustomerData| #[trigger] state.customer_at(variant.customer as int) == Some(customer);
    let name = customer.names@[position.3];
    assert(bucket.view().contains(key));
    assert(state.bucket_has(quantity, key));
    assert(order.wf());
    assert(customer.wf());
    order.variant_count(position.2);
    customer.name_count(position.3);
    state.query_counts(parameters, name.name@, variant.customer as int, key as int, variant.date as int, variant.price as int);
    assert(order.count@ > 0);
    assert(variant.count@ > 0);
    assert(name.count@ > 0);
    assert(order.count@ * variant.count@ * name.count@ > 0) by (nonlinear_arith)
        requires order.count@ > 0, variant.count@ > 0, name.count@ > 0;
}

proof fn slot_injective(state: State, parameters: Params, left: Cursor, right: Cursor)
    requires state.wf(), slot(state, parameters, left) is Some, slot(state, parameters, right) is Some,
        same_group(slot_row(state, parameters, left), slot_row(state, parameters, right)),
    ensures left == right,
{
    let left_quantity = state.directory@[left.0];
    let right_quantity = state.directory@[right.0];
    let left_bucket = choose|bucket: KeyBucket| #[trigger] state.buckets.at(left_quantity) == Some(bucket);
    let right_bucket = choose|bucket: KeyBucket| #[trigger] state.buckets.at(right_quantity) == Some(bucket);
    let left_key = left_bucket.view()[left.1];
    let right_key = right_bucket.view()[right.1];
    assert(left_bucket.view().contains(left_key));
    assert(right_bucket.view().contains(right_key));
    assert(state.bucket_has(left_quantity, left_key));
    assert(state.bucket_has(right_quantity, right_key));
    assert(left_key == right_key);
    assert(left_quantity == right_quantity);
    assert(left.0 == right.0);
    assert(left_bucket.view().no_duplicates());
    assert(left.1 == right.1);
    let order = choose|order: OrderData| #[trigger] state.order_at(left_key as int) == Some(order);
    assert(order.wf());
    assert(left.2 == right.2);
    let variant = order.variants@[left.2];
    let customer = choose|customer: CustomerData| #[trigger] state.customer_at(variant.customer as int) == Some(customer);
    assert(customer.wf());
    assert(left.3 == right.3);
}

pub(super) proof fn empty_cursor(state: State, parameters: Params, start: int)
    requires state.wf(), 0 <= start <= state.directory.len(),
        forall|index: int| 0 <= index < start ==> #[trigger] state.directory@[index] <= parameters.p1,
    ensures model(state, parameters, Seq::empty(), (start, 0, 0, 0)),
{
    assert forall|position: Cursor| #[trigger] slot(state, parameters, position) is Some implies !before(position, (start, 0, 0, 0)) by {}
}

pub(super) proof fn shift(state: State, parameters: Params, rows: Seq<OutRow>, from: Cursor, to: Cursor)
    requires model(state, parameters, rows, from),
        forall|position: Cursor| #[trigger] slot(state, parameters, position) is Some ==> before(position, from) == before(position, to),
    ensures model(state, parameters, rows, to),
{
    assert forall|row: OutRow| #[trigger] rows.contains(row) implies exists|position: Cursor| #[trigger] slot(state, parameters, position) == Some(row) && before(position, to) by {
        let found = choose|position: Cursor| #[trigger] slot(state, parameters, position) == Some(row) && before(position, from);
        assert(before(found, to));
    }
}

pub(super) proof fn append(state: State, parameters: Params, rows: Seq<OutRow>, position: Cursor)
    requires state.wf(), model(state, parameters, rows, position), slot(state, parameters, position) is Some,
    ensures model(state, parameters, rows.push(slot_row(state, parameters, position)), (position.0, position.1, position.2, position.3 + 1)),
{
    broadcast use vstd::seq_lib::group_seq_properties;
    let row = slot_row(state, parameters, position);
    let pushed = rows.push(row);
    let next = (position.0, position.1, position.2, position.3 + 1);
    slot_correct(state, parameters, position);
    assert forall|index: int| 0 <= index < rows.len() implies !same_group(#[trigger] rows[index], row) by {
        assert(rows.contains(rows[index]));
        let previous = choose|previous: Cursor| #[trigger] slot(state, parameters, previous) == Some(rows[index]) && before(previous, position);
        if same_group(rows[index], row) { slot_injective(state, parameters, previous, position); }
    }
    assert forall|result: OutRow| #[trigger] pushed.contains(result) implies exists|location: Cursor| #[trigger] slot(state, parameters, location) == Some(result) && before(location, next) by {
        if result == row { assert(before(position, next)); }
        else {
            assert(rows.contains(result));
            let previous = choose|previous: Cursor| #[trigger] slot(state, parameters, previous) == Some(result) && before(previous, position);
            assert(before(previous, next));
        }
    }
    assert forall|location: Cursor| #[trigger] slot(state, parameters, location) is Some && before(location, next) implies pushed.contains(slot_row(state, parameters, location)) by {
        if before(location, position) { assert(rows.contains(slot_row(state, parameters, location))); }
        else { assert(location == position); }
    }
}

proof fn group_slot(state: State, parameters: Params, name: Seq<u8>, customer: int, key: int, date: int, price: int) -> (position: Cursor)
    requires state.wf(), group_count(state.db(), parameters, name, customer, key, date, price) > 0,
    ensures slot(state, parameters, position) is Some,
        slot_row(state, parameters, position).c_name == name,
        slot_row(state, parameters, position).c_custkey == customer,
        slot_row(state, parameters, position).o_orderkey == key,
        slot_row(state, parameters, position).o_orderdate == date,
        slot_row(state, parameters, position).o_totalprice == price,
{
    state.query_counts(parameters, name, customer, key, date, price);
    assert(state.n(key) > 0 && state.quantity(key) > parameters.p1);
    let order = choose|order: OrderData| #[trigger] state.order_at(key) == Some(order);
    assert(order.wf());
    lemma_sum_nonneg(order.variants@, variant_weight(customer, date, price));
    if state.customer_at(customer) is None { assert(state.cm(customer, name) == 0); }
    let person = choose|person: CustomerData| #[trigger] state.customer_at(customer) == Some(person);
    assert(person.wf());
    lemma_sum_nonneg(person.names@, name_weight(name));
    assert(state.om(key, customer, date, price) >= 0);
    assert(state.cm(customer, name) >= 0);
    assert(state.n(key) * state.om(key, customer, date, price) * state.cm(customer, name) > 0);
    assert(state.om(key, customer, date, price) > 0 && state.cm(customer, name) > 0) by (nonlinear_arith)
        requires state.om(key, customer, date, price) >= 0, state.cm(customer, name) >= 0,
            state.n(key) * state.om(key, customer, date, price) * state.cm(customer, name) > 0;
    let order_index = lemma_sum_nonzero_witness(order.variants@, variant_weight(customer, date, price));
    let name_index = lemma_sum_nonzero_witness(person.names@, name_weight(name));
    assert(order.variants@[order_index].customer == customer && order.variants@[order_index].date == date && order.variants@[order_index].price == price);
    assert(order.quantity@ == order.bucket_quantity() as int);
    assert(state.bucket_has(order.bucket_quantity(), key as i64));
    let bucket = choose|bucket: KeyBucket| #[trigger] state.buckets.at(order.bucket_quantity()) == Some(bucket);
    assert(state.directory@.contains(order.bucket_quantity()));
    let quantity_index = choose|index: int| 0 <= index < state.directory.len() && #[trigger] state.directory@[index] == order.bucket_quantity();
    let key_index = choose|index: int| 0 <= index < bucket.len && #[trigger] bucket.view()[index] == key;
    (quantity_index, key_index, order_index, name_index)
}

pub(super) proof fn finish(state: State, parameters: Params, source: Seq<OutRow>, out: Seq<OutRow>)
    requires state.wf(), model(state, parameters, source, (state.directory.len() as int, 0, 0, 0)),
        forall|row: OutRow| #[trigger] out.contains(row) <==> source.contains(row),
        out.no_duplicates(),
        forall|left: int, right: int| 0 <= left < right < out.len() ==> !row_lt(#[trigger] out[right], #[trigger] out[left]),
    ensures query_result(state.db(), parameters, out),
{
    assert forall|index: int| 0 <= index < out.len() implies row_ok(state.db(), parameters, #[trigger] out[index]) by {
        assert(out.contains(out[index]));
        assert(source.contains(out[index]));
        let found = choose|found: int| 0 <= found < source.len() && #[trigger] source[found] == out[index];
    }
    assert forall|left: int, right: int| 0 <= left < right < out.len() implies !same_group(#[trigger] out[left], #[trigger] out[right]) by {
        if same_group(out[left], out[right]) { assert(out[left] == out[right]); }
    }
    assert forall|name: Seq<u8>, customer: int, key: int, date: int, price: int| #[trigger] group_count(state.db(), parameters, name, customer, key, date, price) > 0 && having(state.db(), parameters, name, customer, key, date, price) implies
        exists|index: int| 0 <= index < out.len() && (#[trigger] out[index]).c_name == name && out[index].c_custkey == customer && out[index].o_orderkey == key && out[index].o_orderdate == date && out[index].o_totalprice == price by {
        let location = group_slot(state, parameters, name, customer, key, date, price);
        let row = slot_row(state, parameters, location);
        assert(before(location, (state.directory.len() as int, 0, 0, 0)));
        assert(source.contains(row));
        assert(out.contains(row));
        let found = choose|found: int| 0 <= found < out.len() && #[trigger] out[found] == row;
    }
}

pub(super) proof fn unfit_slot_refutes(state: State, parameters: Params, position: Cursor)
    requires state.wf(), slot(state, parameters, position) is Some, !out_fits(slot_row(state, parameters, position)),
    ensures !answer_fits(state.db(), parameters),
{
    slot_correct(state, parameters, position);
    let row = slot_row(state, parameters, position);
    let db = state.db();
    lemma_answer_ok(db, parameters);
    let out = answer(db, parameters);
    let index = choose|index: int| 0 <= index < out.len() && (#[trigger] out[index]).c_name == row.c_name && out[index].c_custkey == row.c_custkey
        && out[index].o_orderkey == row.o_orderkey && out[index].o_orderdate == row.o_orderdate && out[index].o_totalprice == row.o_totalprice;
    assert(row_ok(db, parameters, out[index]));
    assert(!out_fits(out[index]));
}

}
