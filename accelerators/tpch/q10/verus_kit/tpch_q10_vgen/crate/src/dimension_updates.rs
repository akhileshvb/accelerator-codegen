use vstd::prelude::*;
use verus_common::sums::*;
use verus_common::rel::{flat, lemma_sum_flat_map};
use verus_prooflib::ordered::Index;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::dimensions::*;
use crate::model::maxima_valid;
use crate::paged::*;
use crate::customers::*;

verus! {

pub open spec fn shape(dimensions: Dimensions) -> bool {
    &&& customer_shape(dimensions)
    &&& nation_index_valid(dimensions.nations)
    &&& maxima_valid(dimensions.nation_totals, dimensions.nation_maxima)
}

pub open spec fn customer_eq(row: CustomerRow) -> spec_fn(CustomerRow) -> int {
    |target: CustomerRow| if target == row { 1int } else { 0int }
}

pub open spec fn nation_eq(row: NationRow) -> spec_fn(NationRow) -> int {
    |target: NationRow| if target == row { 1int } else { 0int }
}

pub open spec fn customer_key_weight(key: int) -> spec_fn(CustomerRow) -> int {
    |row: CustomerRow| if row.c_custkey == key { 1int } else { 0int }
}

pub open spec fn nation_key_weight(key: int) -> spec_fn(NationRow) -> int {
    |row: NationRow| if row.n_nationkey == key { 1int } else { 0int }
}

pub open spec fn customer_weight(weight: spec_fn(CustomerRow) -> int) -> spec_fn(((int, Seq<u8>), Customer)) -> int {
    |entry: ((int, Seq<u8>), Customer)| entry.1.count as int * weight(customer_input_view(*entry.1.row))
}

pub open spec fn nation_weight(weight: spec_fn(NationRow) -> int) -> spec_fn(((int, Seq<u8>), Nation)) -> int {
    |entry: ((int, Seq<u8>), Nation)| entry.1.count as int * weight(nation_input_view(*entry.1.row))
}

pub open spec fn customer_leaf_sum(rows: Seq<((int, Seq<u8>), Customer)>, weight: spec_fn(CustomerRow) -> int) -> int {
    sum(rows, customer_weight(weight))
}

pub open spec fn nation_leaf_sum(rows: Seq<((int, Seq<u8>), Nation)>, weight: spec_fn(NationRow) -> int) -> int {
    sum(rows, nation_weight(weight))
}

pub open spec fn customer_outer_weight(weight: spec_fn(CustomerRow) -> int) -> spec_fn(((int, Seq<u8>), Index<Customer>)) -> int {
    |entry: ((int, Seq<u8>), Index<Customer>)| customer_leaf_sum(entry.1.entries(), weight)
}

pub open spec fn nation_outer_weight(weight: spec_fn(NationRow) -> int) -> spec_fn(((int, Seq<u8>), Index<Nation>)) -> int {
    |entry: ((int, Seq<u8>), Index<Nation>)| nation_leaf_sum(entry.1.entries(), weight)
}

pub open spec fn customer_index_sum(rows: Seq<((int, Seq<u8>), Index<Customer>)>, weight: spec_fn(CustomerRow) -> int) -> int {
    sum(rows, customer_outer_weight(weight))
}

pub open spec fn nation_index_sum(rows: Seq<((int, Seq<u8>), Index<Nation>)>, weight: spec_fn(NationRow) -> int) -> int {
    sum(rows, nation_outer_weight(weight))
}

pub proof fn lemma_customer_point(index: &Index<Index<Customer>>, row: CustomerRow)
    requires index.wf(), customer_index_valid(*index),
    ensures customer_index_sum(index.entries(), customer_eq(row)) == customer_count(*index, row),
{
    index.model();
    let weight = customer_outer_weight(customer_eq(row));
    assert forall|position: int| #![trigger index.entries()[position]] 0 <= position < index.entries().len()
        implies weight(index.entries()[position]) == if index.entries()[position].0 == (row.c_custkey, Seq::empty()) {
            weight(index.entries()[position]) } else { 0 } by {
        let entry = index.entries()[position];
        assert(index.at(entry.0).is_some());
        let inner = entry.1;
        inner.model();
        if entry.0.0 != row.c_custkey {
            assert forall|inner_position: int| #![trigger inner.entries()[inner_position]] 0 <= inner_position < inner.entries().len()
                implies customer_weight(customer_eq(row))(inner.entries()[inner_position]) == 0 by {
                assert(inner.at(inner.entries()[inner_position].0).is_some());
            }
            lemma_sum_zero(inner.entries(), customer_weight(customer_eq(row)));
        }
    }
    lemma_sum_ext(index.entries(), weight,
        |entry: ((int, Seq<u8>), Index<Customer>)| if entry.0 == (row.c_custkey, Seq::empty()) { weight(entry) } else { 0 });
    verus_prooflib::ordered::lemma_key_contribution(index, (row.c_custkey, Seq::empty()), weight);
    match index.at((row.c_custkey, Seq::empty())) {
        Some(inner) => {
            assert(index.at((row.c_custkey, Seq::empty())).is_some());
            inner.model();
            let leaf_weight = customer_weight(customer_eq(row));
            assert forall|position: int| #![trigger inner.entries()[position]] 0 <= position < inner.entries().len()
                implies leaf_weight(inner.entries()[position]) == if inner.entries()[position].0 == customer_inner_view(row) {
                    leaf_weight(inner.entries()[position]) } else { 0 } by {
                assert(inner.at(inner.entries()[position].0).is_some());
            }
            lemma_sum_ext(inner.entries(), leaf_weight,
                |entry: ((int, Seq<u8>), Customer)| if entry.0 == customer_inner_view(row) { leaf_weight(entry) } else { 0 });
            verus_prooflib::ordered::lemma_key_contribution(&inner, customer_inner_view(row), leaf_weight);
        },
        None => {},
    }
}

pub proof fn lemma_nation_point(index: &Index<Index<Nation>>, row: NationRow)
    requires index.wf(), nation_index_valid(*index),
    ensures nation_index_sum(index.entries(), nation_eq(row)) == nation_count(*index, row),
{
    index.model();
    let weight = nation_outer_weight(nation_eq(row));
    assert forall|position: int| #![trigger index.entries()[position]] 0 <= position < index.entries().len()
        implies weight(index.entries()[position]) == if index.entries()[position].0 == (row.n_nationkey, Seq::empty()) {
            weight(index.entries()[position]) } else { 0 } by {
        let entry = index.entries()[position];
        assert(index.at(entry.0).is_some());
        let inner = entry.1;
        inner.model();
        if entry.0.0 != row.n_nationkey {
            assert forall|inner_position: int| #![trigger inner.entries()[inner_position]] 0 <= inner_position < inner.entries().len()
                implies nation_weight(nation_eq(row))(inner.entries()[inner_position]) == 0 by {
                assert(inner.at(inner.entries()[inner_position].0).is_some());
            }
            lemma_sum_zero(inner.entries(), nation_weight(nation_eq(row)));
        }
    }
    lemma_sum_ext(index.entries(), weight,
        |entry: ((int, Seq<u8>), Index<Nation>)| if entry.0 == (row.n_nationkey, Seq::empty()) { weight(entry) } else { 0 });
    verus_prooflib::ordered::lemma_key_contribution(index, (row.n_nationkey, Seq::empty()), weight);
    match index.at((row.n_nationkey, Seq::empty())) {
        Some(inner) => {
            assert(index.at((row.n_nationkey, Seq::empty())).is_some());
            inner.model();
            let leaf_weight = nation_weight(nation_eq(row));
            assert forall|position: int| #![trigger inner.entries()[position]] 0 <= position < inner.entries().len()
                implies leaf_weight(inner.entries()[position]) == if inner.entries()[position].0 == nation_inner_view(row) {
                    leaf_weight(inner.entries()[position]) } else { 0 } by {
                assert(inner.at(inner.entries()[position].0).is_some());
            }
            lemma_sum_ext(inner.entries(), leaf_weight,
                |entry: ((int, Seq<u8>), Nation)| if entry.0 == nation_inner_view(row) { leaf_weight(entry) } else { 0 });
            verus_prooflib::ordered::lemma_key_contribution(&inner, nation_inner_view(row), leaf_weight);
        },
        None => {},
    }
}

pub open spec fn customer_inner_transform() -> spec_fn(((int, Seq<u8>), Customer)) -> Customer {
    |entry: ((int, Seq<u8>), Customer)| entry.1
}

pub open spec fn nation_inner_transform() -> spec_fn(((int, Seq<u8>), Nation)) -> Nation {
    |entry: ((int, Seq<u8>), Nation)| entry.1
}

pub open spec fn customer_outer_transform() -> spec_fn(((int, Seq<u8>), Index<Customer>)) -> Seq<Customer> {
    |entry: ((int, Seq<u8>), Index<Customer>)| entry.1.entries().map_values(customer_inner_transform())
}

pub open spec fn nation_outer_transform() -> spec_fn(((int, Seq<u8>), Index<Nation>)) -> Seq<Nation> {
    |entry: ((int, Seq<u8>), Index<Nation>)| entry.1.entries().map_values(nation_inner_transform())
}

pub open spec fn customer_value_weight(weight: spec_fn(CustomerRow) -> int) -> spec_fn(Customer) -> int {
    |value: Customer| value.count as int * weight(customer_input_view(*value.row))
}

pub open spec fn nation_value_weight(weight: spec_fn(NationRow) -> int) -> spec_fn(Nation) -> int {
    |value: Nation| value.count as int * weight(nation_input_view(*value.row))
}

pub open spec fn customer_representatives(rows: Seq<((int, Seq<u8>), Index<Customer>)>) -> Seq<Customer> {
    flat(rows.map_values(customer_outer_transform()))
}

pub open spec fn nation_representatives(rows: Seq<((int, Seq<u8>), Index<Nation>)>) -> Seq<Nation> {
    flat(rows.map_values(nation_outer_transform()))
}

pub proof fn lemma_customer_fold(rows: Seq<((int, Seq<u8>), Index<Customer>)>, weight: spec_fn(CustomerRow) -> int)
    ensures customer_index_sum(rows, weight) == sum(customer_representatives(rows), customer_value_weight(weight)),
{
    let transform = customer_outer_transform();
    let value_weight = customer_value_weight(weight);
    lemma_sum_flat_map(rows, transform, value_weight);
    assert forall|entry: ((int, Seq<u8>), Index<Customer>)| #[trigger] customer_outer_weight(weight)(entry)
        == sum(transform(entry), value_weight) by {
        lemma_sum_map(entry.1.entries(), customer_inner_transform(), value_weight, customer_weight(weight));
    }
    lemma_sum_ext_all(rows, customer_outer_weight(weight),
        |entry: ((int, Seq<u8>), Index<Customer>)| sum(transform(entry), value_weight));
}

pub proof fn lemma_nation_fold(rows: Seq<((int, Seq<u8>), Index<Nation>)>, weight: spec_fn(NationRow) -> int)
    ensures nation_index_sum(rows, weight) == sum(nation_representatives(rows), nation_value_weight(weight)),
{
    let transform = nation_outer_transform();
    let value_weight = nation_value_weight(weight);
    lemma_sum_flat_map(rows, transform, value_weight);
    assert forall|entry: ((int, Seq<u8>), Index<Nation>)| #[trigger] nation_outer_weight(weight)(entry)
        == sum(transform(entry), value_weight) by {
        lemma_sum_map(entry.1.entries(), nation_inner_transform(), value_weight, nation_weight(weight));
    }
    lemma_sum_ext_all(rows, nation_outer_weight(weight),
        |entry: ((int, Seq<u8>), Index<Nation>)| sum(transform(entry), value_weight));
}

pub proof fn lemma_customer_capture_sum(index: &Index<Index<Customer>>, rows: Seq<CustomerDelta>, weight: spec_fn(CustomerRow) -> int)
    requires index.wf(), customer_index_valid(*index),
        forall|row: CustomerRow| #[trigger] customer_count(*index, row)
            == sum(rows, |delta: CustomerDelta| if customer_input_view(delta.row) == row { delta.sign as int } else { 0 }),
    ensures customer_index_sum(index.entries(), weight) == sum(rows, |delta: CustomerDelta| delta.sign as int * weight(customer_input_view(delta.row))),
{
    let representatives = customer_representatives(index.entries());
    let input_view = |delta: CustomerDelta| customer_input_view(delta.row);
    let input_count = |delta: CustomerDelta| delta.sign as int;
    let representative_view = |value: Customer| customer_input_view(*value.row);
    let representative_count = |value: Customer| value.count as int;
    assert forall|row: CustomerRow| #[trigger] verus_prooflib::class_total(rows, input_view, input_count, row)
        == verus_prooflib::class_total(representatives, representative_view, representative_count, row) by {
        lemma_customer_point(index, row);
        lemma_customer_fold(index.entries(), customer_eq(row));
        lemma_sum_ext_all(representatives, customer_value_weight(customer_eq(row)),
            |value: Customer| if representative_view(value) == row { representative_count(value) } else { 0 });
        assert(customer_count(*index, row)
            == sum(rows, |delta: CustomerDelta| if customer_input_view(delta.row) == row { delta.sign as int } else { 0 }));
        lemma_sum_ext_all(rows,
            |delta: CustomerDelta| if input_view(delta) == row { input_count(delta) } else { 0 },
            |delta: CustomerDelta| if customer_input_view(delta.row) == row { delta.sign as int } else { 0 });
    }
    verus_prooflib::lemma_equal_classes(rows, representatives, input_view, representative_view, input_count, representative_count, weight);
    lemma_customer_fold(index.entries(), weight);
    lemma_sum_ext_all(representatives, |value: Customer| representative_count(value) * weight(representative_view(value)), customer_value_weight(weight));
    lemma_sum_ext_all(rows, |delta: CustomerDelta| input_count(delta) * weight(input_view(delta)),
        |delta: CustomerDelta| delta.sign as int * weight(customer_input_view(delta.row)));
}

pub proof fn lemma_nation_capture_sum(index: &Index<Index<Nation>>, rows: Seq<NationDelta>, weight: spec_fn(NationRow) -> int)
    requires index.wf(), nation_index_valid(*index),
        forall|row: NationRow| #[trigger] nation_count(*index, row)
            == sum(rows, |delta: NationDelta| if nation_input_view(delta.row) == row { delta.sign as int } else { 0 }),
    ensures nation_index_sum(index.entries(), weight) == sum(rows, |delta: NationDelta| delta.sign as int * weight(nation_input_view(delta.row))),
{
    let representatives = nation_representatives(index.entries());
    let input_view = |delta: NationDelta| nation_input_view(delta.row);
    let input_count = |delta: NationDelta| delta.sign as int;
    let representative_view = |value: Nation| nation_input_view(*value.row);
    let representative_count = |value: Nation| value.count as int;
    assert forall|row: NationRow| #[trigger] verus_prooflib::class_total(rows, input_view, input_count, row)
        == verus_prooflib::class_total(representatives, representative_view, representative_count, row) by {
        lemma_nation_point(index, row);
        lemma_nation_fold(index.entries(), nation_eq(row));
        lemma_sum_ext_all(representatives, nation_value_weight(nation_eq(row)),
            |value: Nation| if representative_view(value) == row { representative_count(value) } else { 0 });
        assert(nation_count(*index, row)
            == sum(rows, |delta: NationDelta| if nation_input_view(delta.row) == row { delta.sign as int } else { 0 }));
        lemma_sum_ext_all(rows,
            |delta: NationDelta| if input_view(delta) == row { input_count(delta) } else { 0 },
            |delta: NationDelta| if nation_input_view(delta.row) == row { delta.sign as int } else { 0 });
    }
    verus_prooflib::lemma_equal_classes(rows, representatives, input_view, representative_view, input_count, representative_count, weight);
    lemma_nation_fold(index.entries(), weight);
    lemma_sum_ext_all(representatives, |value: Nation| representative_count(value) * weight(representative_view(value)), nation_value_weight(weight));
    lemma_sum_ext_all(rows, |delta: NationDelta| input_count(delta) * weight(input_view(delta)),
        |delta: NationDelta| delta.sign as int * weight(nation_input_view(delta.row)));
}

pub fn install_customer_leaf(dimensions: &mut Dimensions, values: &Index<Customer>) -> (result: bool)
    requires shape(*old(dimensions)),
    ensures result ==> shape(*final(dimensions)),
        result ==> forall|weight: spec_fn(CustomerRow) -> int| #[trigger] table_sum(final(dimensions).customers, weight)
            == table_sum(old(dimensions).customers, weight) + customer_leaf_sum(values.entries(), weight),
        final(dimensions).nations == old(dimensions).nations,
        final(dimensions).nation_totals == old(dimensions).nation_totals,
        final(dimensions).nation_maxima == old(dimensions).nation_maxima,
{
    let ghost before = *dimensions;
    let ghost entries = values.entries();
    let ghost mut processed = 0int;
    let mut cursor = values.cursor();
    loop
        invariant shape(*dimensions), before == *old(dimensions), entries == values.entries(),
            0 <= processed <= entries.len(), cursor.remaining() == entries.skip(processed),
            forall|weight: spec_fn(CustomerRow) -> int| #[trigger] table_sum(dimensions.customers, weight)
                == table_sum(before.customers, weight) + customer_leaf_sum(entries.take(processed), weight),
            dimensions.nations == before.nations, dimensions.nation_totals == before.nation_totals,
            dimensions.nation_maxima == before.nation_maxima,
        decreases cursor.remaining().len(),
    {
        let (_, value) = match cursor.next() {
            Some(row) => row,
            None => {
                proof { assert(processed == entries.len()); assert(entries.take(processed) =~= entries); }
                return true;
            },
        };
        proof { assert(*value == entries[processed].1); }
        if !dimensions.edit_customer(&value.row, value.count) { return false; }
        proof {
            assert forall|weight: spec_fn(CustomerRow) -> int| #[trigger] table_sum(dimensions.customers, weight)
                == table_sum(before.customers, weight) + customer_leaf_sum(entries.take(processed + 1), weight) by {
                lemma_sum_take(entries, processed, customer_weight(weight));
            }
            processed = processed + 1;
        }
    }
}

pub fn install_nation_leaf(dimensions: &mut Dimensions, values: &Index<Nation>) -> (result: bool)
    requires shape(*old(dimensions)),
    ensures result ==> shape(*final(dimensions)),
        result ==> forall|row: NationRow| #[trigger] nation_count(final(dimensions).nations, row)
            == nation_count(old(dimensions).nations, row) + nation_leaf_sum(values.entries(), nation_eq(row)),
        result ==> forall|key: int| #[trigger] total_at(final(dimensions).nation_totals, key)
            == total_at(old(dimensions).nation_totals, key) + nation_leaf_sum(values.entries(), nation_key_weight(key)),
        final(dimensions).customers == old(dimensions).customers, final(dimensions).customer_max == old(dimensions).customer_max,
{
    let ghost before = *dimensions;
    let ghost entries = values.entries();
    let ghost mut processed = 0int;
    let mut cursor = values.cursor();
    loop
        invariant shape(*dimensions), before == *old(dimensions), entries == values.entries(),
            0 <= processed <= entries.len(), cursor.remaining() == entries.skip(processed),
            forall|row: NationRow| #[trigger] nation_count(dimensions.nations, row)
                == nation_count(before.nations, row) + nation_leaf_sum(entries.take(processed), nation_eq(row)),
            forall|key: int| #[trigger] total_at(dimensions.nation_totals, key)
                == total_at(before.nation_totals, key) + nation_leaf_sum(entries.take(processed), nation_key_weight(key)),
            dimensions.customers == before.customers, dimensions.customer_max == before.customer_max,
        decreases cursor.remaining().len(),
    {
        let (_, value) = match cursor.next() {
            Some(row) => row,
            None => {
                proof { assert(processed == entries.len()); assert(entries.take(processed) =~= entries); }
                return true;
            },
        };
        proof { assert(*value == entries[processed].1); }
        if !dimensions.edit_nation(&*value.row, value.count) { return false; }
        proof {
            assert forall|row: NationRow| #[trigger] nation_count(dimensions.nations, row)
                == nation_count(before.nations, row) + nation_leaf_sum(entries.take(processed + 1), nation_eq(row)) by {
                lemma_sum_take(entries, processed, nation_weight(nation_eq(row)));
            }
            assert forall|key: int| #[trigger] total_at(dimensions.nation_totals, key)
                == total_at(before.nation_totals, key) + nation_leaf_sum(entries.take(processed + 1), nation_key_weight(key)) by {
                lemma_sum_take(entries, processed, nation_weight(nation_key_weight(key)));
            }
            processed = processed + 1;
        }
    }
}

pub fn install_customers(dimensions: &mut Dimensions, changes: &Index<Index<Customer>>) -> (result: bool)
    requires shape(*old(dimensions)),
    ensures result ==> shape(*final(dimensions)),
        result ==> forall|weight: spec_fn(CustomerRow) -> int| #[trigger] table_sum(final(dimensions).customers, weight)
            == table_sum(old(dimensions).customers, weight) + customer_index_sum(changes.entries(), weight),
        final(dimensions).nations == old(dimensions).nations,
        final(dimensions).nation_totals == old(dimensions).nation_totals,
        final(dimensions).nation_maxima == old(dimensions).nation_maxima,
{
    let ghost before = *dimensions;
    let ghost entries = changes.entries();
    let ghost mut processed = 0int;
    let mut cursor = changes.cursor();
    loop
        invariant shape(*dimensions), before == *old(dimensions), entries == changes.entries(),
            0 <= processed <= entries.len(), cursor.remaining() == entries.skip(processed),
            forall|weight: spec_fn(CustomerRow) -> int| #[trigger] table_sum(dimensions.customers, weight)
                == table_sum(before.customers, weight) + customer_index_sum(entries.take(processed), weight),
            dimensions.nations == before.nations, dimensions.nation_totals == before.nation_totals,
            dimensions.nation_maxima == before.nation_maxima,
        decreases cursor.remaining().len(),
    {
        let (_, values) = match cursor.next() {
            Some(row) => row,
            None => {
                proof { assert(processed == entries.len()); assert(entries.take(processed) =~= entries); }
                return true;
            },
        };
        proof { assert(*values == entries[processed].1); }
        if !install_customer_leaf(dimensions, &*values) { return false; }
        proof {
            assert forall|weight: spec_fn(CustomerRow) -> int| #[trigger] table_sum(dimensions.customers, weight)
                == table_sum(before.customers, weight) + customer_index_sum(entries.take(processed + 1), weight) by {
                lemma_sum_take(entries, processed, customer_outer_weight(weight));
            }
            processed = processed + 1;
        }
    }
}

pub fn install_nations(dimensions: &mut Dimensions, changes: &Index<Index<Nation>>) -> (result: bool)
    requires shape(*old(dimensions)),
    ensures result ==> shape(*final(dimensions)),
        result ==> forall|row: NationRow| #[trigger] nation_count(final(dimensions).nations, row)
            == nation_count(old(dimensions).nations, row) + nation_index_sum(changes.entries(), nation_eq(row)),
        result ==> forall|key: int| #[trigger] total_at(final(dimensions).nation_totals, key)
            == total_at(old(dimensions).nation_totals, key) + nation_index_sum(changes.entries(), nation_key_weight(key)),
        final(dimensions).customers == old(dimensions).customers, final(dimensions).customer_max == old(dimensions).customer_max,
{
    let ghost before = *dimensions;
    let ghost entries = changes.entries();
    let ghost mut processed = 0int;
    let mut cursor = changes.cursor();
    loop
        invariant shape(*dimensions), before == *old(dimensions), entries == changes.entries(),
            0 <= processed <= entries.len(), cursor.remaining() == entries.skip(processed),
            forall|row: NationRow| #[trigger] nation_count(dimensions.nations, row)
                == nation_count(before.nations, row) + nation_index_sum(entries.take(processed), nation_eq(row)),
            forall|key: int| #[trigger] total_at(dimensions.nation_totals, key)
                == total_at(before.nation_totals, key) + nation_index_sum(entries.take(processed), nation_key_weight(key)),
            dimensions.customers == before.customers, dimensions.customer_max == before.customer_max,
        decreases cursor.remaining().len(),
    {
        let (_, values) = match cursor.next() {
            Some(row) => row,
            None => {
                proof { assert(processed == entries.len()); assert(entries.take(processed) =~= entries); }
                return true;
            },
        };
        proof { assert(*values == entries[processed].1); }
        if !install_nation_leaf(dimensions, &*values) { return false; }
        proof {
            assert forall|row: NationRow| #[trigger] nation_count(dimensions.nations, row)
                == nation_count(before.nations, row) + nation_index_sum(entries.take(processed + 1), nation_eq(row)) by {
                lemma_sum_take(entries, processed, nation_outer_weight(nation_eq(row)));
            }
            assert forall|key: int| #[trigger] total_at(dimensions.nation_totals, key)
                == total_at(before.nation_totals, key) + nation_index_sum(entries.take(processed + 1), nation_key_weight(key)) by {
                lemma_sum_take(entries, processed, nation_outer_weight(nation_key_weight(key)));
            }
            processed = processed + 1;
        }
    }
}

pub proof fn lemma_dimensions_capture(before: Db, after: Db, previous: Dimensions, next: Dimensions, customers: Seq<CustomerDelta>, nations: Seq<NationDelta>)
    requires crate::model::dimensions_valid(before, previous), shape(next),
        bag_step(before.customer, after.customer, customer_delta_seq_view(customers)),
        bag_step(before.nation, after.nation, nation_delta_seq_view(nations)),
        forall|weight: spec_fn(CustomerRow) -> int| #[trigger] table_sum(next.customers, weight) == table_sum(previous.customers, weight)
            + sum(customers, |delta: CustomerDelta| delta.sign as int * weight(customer_input_view(delta.row))),
        forall|row: NationRow| #[trigger] nation_count(next.nations, row) == nation_count(previous.nations, row)
            + sum(nations, |delta: NationDelta| if nation_input_view(delta.row) == row { delta.sign as int } else { 0 }),
        forall|key: int| #[trigger] total_at(next.nation_totals, key) == total_at(previous.nation_totals, key)
            + sum(nations, |delta: NationDelta| if delta.row.n_nationkey == key { delta.sign as int } else { 0 }),
    ensures crate::model::dimensions_valid(after, next),
{
    reveal(crate::model::dimensions_valid);
    assert forall|weight: spec_fn(CustomerRow) -> int| #[trigger] table_sum(next.customers, weight) == sum(after.customer, weight) by {
        assert(sum(after.customer, weight) == sum(before.customer, weight)
            + sum(customer_delta_seq_view(customers), |entry: (CustomerRow, int)| entry.1 * weight(entry.0)));
        lemma_sum_map(customers, |delta: CustomerDelta| (customer_input_view(delta.row), delta.sign as int),
            |entry: (CustomerRow, int)| entry.1 * weight(entry.0),
            |delta: CustomerDelta| delta.sign as int * weight(customer_input_view(delta.row)));
        assert(table_sum(previous.customers, weight) == sum(before.customer, weight));
    }
    assert forall|row: NationRow| #[trigger] nation_count(next.nations, row)
        == sum(after.nation, nation_eq(row)) by {
        let weight = nation_eq(row);
        assert(sum(after.nation, weight) == sum(before.nation, weight)
            + sum(nation_delta_seq_view(nations), |entry: (NationRow, int)| entry.1 * weight(entry.0)));
        lemma_sum_map(nations, |delta: NationDelta| (nation_input_view(delta.row), delta.sign as int),
            |entry: (NationRow, int)| entry.1 * weight(entry.0),
            |delta: NationDelta| if nation_input_view(delta.row) == row { delta.sign as int } else { 0 });
        lemma_sum_ext_all(before.nation, weight, |value: NationRow| if value == row { 1int } else { 0int });
        lemma_sum_ext_all(after.nation, weight, |value: NationRow| if value == row { 1int } else { 0int });
        assert(nation_count(previous.nations, row) == sum(before.nation, |value: NationRow| if value == row { 1int } else { 0int }));
    }
    assert forall|key: int| #[trigger] total_at(next.nation_totals, key)
        == sum(after.nation, |row: NationRow| if row.n_nationkey == key { 1int } else { 0int }) by {
        let weight = nation_key_weight(key);
        assert(sum(after.nation, weight) == sum(before.nation, weight)
            + sum(nation_delta_seq_view(nations), |entry: (NationRow, int)| entry.1 * weight(entry.0)));
        lemma_sum_map(nations, |delta: NationDelta| (nation_input_view(delta.row), delta.sign as int),
            |entry: (NationRow, int)| entry.1 * weight(entry.0),
            |delta: NationDelta| if delta.row.n_nationkey == key { delta.sign as int } else { 0 });
        lemma_sum_ext_all(before.nation, weight, |row: NationRow| if row.n_nationkey == key { 1int } else { 0int });
        lemma_sum_ext_all(after.nation, weight, |row: NationRow| if row.n_nationkey == key { 1int } else { 0int });
        assert(total_at(previous.nation_totals, key) == sum(before.nation, |row: NationRow| if row.n_nationkey == key { 1int } else { 0int }));
    }
    crate::model::lemma_dimensions_from_counts(after, next);
}

}
