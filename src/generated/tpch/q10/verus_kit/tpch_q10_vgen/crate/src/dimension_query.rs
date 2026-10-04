use vstd::prelude::*;
use verus_common::sums::*;
use verus_prooflib::ordered::{Index, KeyView};
use crate::dimensions::*;
use crate::gen_spec::*;
use crate::gen_obligations::{customer_input_view, nation_input_view};
use crate::dimension_updates::*;
use crate::model::*;

verus! {

pub proof fn lemma_customer_database_sum(db: Db, index: &Index<Index<Customer>>, weight: spec_fn(CustomerRow) -> int)
    requires index.wf(), customer_index_valid(*index),
        forall|row: CustomerRow| #[trigger] customer_count(*index, row) == sum(db.customer, customer_eq(row)),
    ensures customer_index_sum(index.entries(), weight) == sum(db.customer, weight),
{
    let representatives = customer_representatives(index.entries());
    let source_view = |row: CustomerRow| row;
    let source_count = |row: CustomerRow| 1int;
    let representative_view = |value: Customer| customer_input_view(*value.row);
    let representative_count = |value: Customer| value.count as int;
    assert forall|row: CustomerRow| #[trigger] verus_prooflib::class_total(db.customer, source_view, source_count, row)
        == verus_prooflib::class_total(representatives, representative_view, representative_count, row) by {
        lemma_customer_point(index, row);
        lemma_customer_fold(index.entries(), customer_eq(row));
        lemma_sum_ext_all(representatives, customer_value_weight(customer_eq(row)),
            |value: Customer| if representative_view(value) == row { representative_count(value) } else { 0int });
        assert(customer_count(*index, row) == sum(db.customer, customer_eq(row)));
        lemma_sum_ext_all(db.customer,
            |value: CustomerRow| if source_view(value) == row { source_count(value) } else { 0int }, customer_eq(row));
    }
    verus_prooflib::lemma_equal_classes(db.customer, representatives, source_view, representative_view,
        source_count, representative_count, weight);
    lemma_customer_fold(index.entries(), weight);
    lemma_sum_ext_all(representatives,
        |value: Customer| representative_count(value) * weight(representative_view(value)), customer_value_weight(weight));
    lemma_sum_ext_all(db.customer, |row: CustomerRow| source_count(row) * weight(source_view(row)), weight);
}

pub proof fn lemma_nation_database_sum(db: Db, index: &Index<Index<Nation>>, weight: spec_fn(NationRow) -> int)
    requires index.wf(), nation_index_valid(*index),
        forall|row: NationRow| #[trigger] nation_count(*index, row) == sum(db.nation, nation_eq(row)),
    ensures nation_index_sum(index.entries(), weight) == sum(db.nation, weight),
{
    let representatives = nation_representatives(index.entries());
    let source_view = |row: NationRow| row;
    let source_count = |row: NationRow| 1int;
    let representative_view = |value: Nation| nation_input_view(*value.row);
    let representative_count = |value: Nation| value.count as int;
    assert forall|row: NationRow| #[trigger] verus_prooflib::class_total(db.nation, source_view, source_count, row)
        == verus_prooflib::class_total(representatives, representative_view, representative_count, row) by {
        lemma_nation_point(index, row);
        lemma_nation_fold(index.entries(), nation_eq(row));
        lemma_sum_ext_all(representatives, nation_value_weight(nation_eq(row)),
            |value: Nation| if representative_view(value) == row { representative_count(value) } else { 0int });
        assert(nation_count(*index, row) == sum(db.nation, nation_eq(row)));
        lemma_sum_ext_all(db.nation,
            |value: NationRow| if source_view(value) == row { source_count(value) } else { 0int }, nation_eq(row));
    }
    verus_prooflib::lemma_equal_classes(db.nation, representatives, source_view, representative_view,
        source_count, representative_count, weight);
    lemma_nation_fold(index.entries(), weight);
    lemma_sum_ext_all(representatives,
        |value: Nation| representative_count(value) * weight(representative_view(value)), nation_value_weight(weight));
    lemma_sum_ext_all(db.nation, |row: NationRow| source_count(row) * weight(source_view(row)), weight);
}

pub proof fn lemma_customer_join_key(index: &Index<Index<Customer>>, key: int, weight: spec_fn(CustomerRow) -> int)
    requires index.wf(), customer_index_valid(*index),
        forall|row: CustomerRow| row.c_custkey != key ==> #[trigger] weight(row) == 0,
    ensures customer_index_sum(index.entries(), weight) == match index.at((key, Seq::empty())) {
        Some(inner) => customer_leaf_sum(inner.entries(), weight), None => 0int,
    },
{
    index.model();
    let outer_weight = customer_outer_weight(weight);
    assert forall|position: int| #![trigger index.entries()[position]] 0 <= position < index.entries().len()
        implies outer_weight(index.entries()[position]) == if index.entries()[position].0 == (key, Seq::empty()) {
            outer_weight(index.entries()[position])
        } else { 0int } by {
        let entry = index.entries()[position];
        assert(index.at(entry.0).is_some());
        let inner = entry.1;
        inner.model();
        if entry.0.0 != key {
            assert forall|inner_position: int| #![trigger inner.entries()[inner_position]]
                0 <= inner_position < inner.entries().len()
                    implies customer_weight(weight)(inner.entries()[inner_position]) == 0 by {
                assert(inner.at(inner.entries()[inner_position].0).is_some());
                assert(customer_input_view(*inner.entries()[inner_position].1.row).c_custkey == entry.0.0);
                assert(weight(customer_input_view(*inner.entries()[inner_position].1.row)) == 0);
            }
            lemma_sum_zero(inner.entries(), customer_weight(weight));
        }
    }
    lemma_sum_ext(index.entries(), outer_weight,
        |entry: (KeyView, Index<Customer>)| if entry.0 == (key, Seq::<u8>::empty()) { outer_weight(entry) } else { 0int });
    verus_prooflib::ordered::lemma_key_contribution(index, (key, Seq::empty()), outer_weight);
}

pub proof fn lemma_nation_join_key(index: &Index<Index<Nation>>, key: int, weight: spec_fn(NationRow) -> int)
    requires index.wf(), nation_index_valid(*index),
        forall|row: NationRow| row.n_nationkey != key ==> #[trigger] weight(row) == 0,
    ensures nation_index_sum(index.entries(), weight) == match index.at((key, Seq::empty())) {
        Some(inner) => nation_leaf_sum(inner.entries(), weight), None => 0int,
    },
{
    index.model();
    let outer_weight = nation_outer_weight(weight);
    assert forall|position: int| #![trigger index.entries()[position]] 0 <= position < index.entries().len()
        implies outer_weight(index.entries()[position]) == if index.entries()[position].0 == (key, Seq::empty()) {
            outer_weight(index.entries()[position])
        } else { 0int } by {
        let entry = index.entries()[position];
        assert(index.at(entry.0).is_some());
        let inner = entry.1;
        inner.model();
        if entry.0.0 != key {
            assert forall|inner_position: int| #![trigger inner.entries()[inner_position]]
                0 <= inner_position < inner.entries().len()
                    implies nation_weight(weight)(inner.entries()[inner_position]) == 0 by {
                assert(inner.at(inner.entries()[inner_position].0).is_some());
                assert(nation_input_view(*inner.entries()[inner_position].1.row).n_nationkey == entry.0.0);
                assert(weight(nation_input_view(*inner.entries()[inner_position].1.row)) == 0);
            }
            lemma_sum_zero(inner.entries(), nation_weight(weight));
        }
    }
    lemma_sum_ext(index.entries(), outer_weight,
        |entry: (KeyView, Index<Nation>)| if entry.0 == (key, Seq::<u8>::empty()) { outer_weight(entry) } else { 0int });
    verus_prooflib::ordered::lemma_key_contribution(index, (key, Seq::empty()), outer_weight);
}

pub open spec fn nation_name_weight(name: Seq<u8>) -> spec_fn(NationRow) -> int {
    |nation: NationRow| if nation.n_name == name { 1int } else { 0int }
}

pub open spec fn indexed_names(dimensions: Dimensions, key: int, name: Seq<u8>) -> int {
    match dimensions.nations.at((key, Seq::empty())) {
        Some(inner) => nation_leaf_sum(inner.entries(), nation_name_weight(name)), None => 0int,
    }
}

pub open spec fn indexed_dimension_weight(dimensions: Dimensions, group: OutRow) -> spec_fn(CustomerRow) -> int {
    |customer: CustomerRow| if crate::query_semantics::customer_matches(customer, group) {
        indexed_names(dimensions, customer.c_nationkey, group.n_name)
    } else { 0int }
}

pub open spec fn indexed_dimension_multiplicity(dimensions: Dimensions, group: OutRow) -> int {
    match dimensions.customers.at((group.c_custkey, Seq::empty())) {
        Some(inner) => customer_leaf_sum(inner.entries(), indexed_dimension_weight(dimensions, group)), None => 0int,
    }
}

pub proof fn lemma_indexed_names(db: Db, dimensions: Dimensions, key: int, name: Seq<u8>)
    requires dimensions_valid(db, dimensions), dimensions.nations.wf(),
    ensures indexed_names(dimensions, key, name) == sum(db.nation, crate::query_semantics::nation_weight(key, name)),
{
    lemma_dimensions_shape(db, dimensions);
    reveal(dimensions_valid);
    let weight = crate::query_semantics::nation_weight(key, name);
    assert forall|row: NationRow| #[trigger] nation_count(dimensions.nations, row) == sum(db.nation, nation_eq(row)) by {
        assert(nation_eq(row) =~= (|value: NationRow| if value == row { 1int } else { 0int }));
    }
    lemma_nation_database_sum(db, &dimensions.nations, weight);
    lemma_nation_join_key(&dimensions.nations, key, weight);
    match dimensions.nations.at((key, Seq::empty())) {
        Some(inner) => {
            inner.model();
            assert forall|position: int| #![trigger inner.entries()[position]] 0 <= position < inner.entries().len()
                implies nation_weight(weight)(inner.entries()[position])
                    == nation_weight(nation_name_weight(name))(inner.entries()[position]) by {
                assert(inner.at(inner.entries()[position].0).is_some());
            }
            lemma_sum_ext(inner.entries(), nation_weight(weight), nation_weight(nation_name_weight(name)));
        },
        None => {},
    }
}

pub proof fn lemma_indexed_dimension_multiplicity(db: Db, dimensions: Dimensions, group: OutRow)
    requires dimensions_valid(db, dimensions), dimensions.customers.wf(), dimensions.nations.wf(),
    ensures indexed_dimension_multiplicity(dimensions, group) == crate::query_semantics::dimension_multiplicity(db, group),
{
    lemma_dimensions_shape(db, dimensions);
    reveal(dimensions_valid);
    let weight = indexed_dimension_weight(dimensions, group);
    assert forall|row: CustomerRow| #[trigger] customer_count(dimensions.customers, row) == sum(db.customer, customer_eq(row)) by {
        assert(customer_eq(row) =~= (|value: CustomerRow| if value == row { 1int } else { 0int }));
    }
    lemma_customer_database_sum(db, &dimensions.customers, weight);
    lemma_customer_join_key(&dimensions.customers, group.c_custkey, weight);
    let raw = |customer: CustomerRow| if crate::query_semantics::customer_matches(customer, group) {
        sum(db.nation, crate::query_semantics::nation_weight(customer.c_nationkey, group.n_name))
    } else { 0int };
    assert forall|customer: CustomerRow| #[trigger] weight(customer) == raw(customer) by {
        lemma_indexed_names(db, dimensions, customer.c_nationkey, group.n_name);
    }
    lemma_sum_ext_all(db.customer, weight, raw);
    reveal(crate::query_semantics::dimension_multiplicity);
}

}
