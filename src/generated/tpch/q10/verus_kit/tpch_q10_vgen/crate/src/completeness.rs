use vstd::prelude::*;
use verus_common::sums::*;
use verus_prooflib::ordered::{Index, KeyView};
use crate::gen_spec::*;
use crate::gen_obligations::{customer_input_view, nation_input_view};
use crate::dimensions::*;
use crate::dimension_updates::*;
use crate::dimension_query::*;
use crate::group_proofs::*;
use crate::model::*;

verus! {

pub proof fn lemma_representatives(db: Db, parameters: Params, dimensions: Dimensions, row: OutRow)
    -> (representatives: (Customer, Nation))
    requires dimensions_valid(db, dimensions), dimensions.customers.wf(), dimensions.nations.wf(),
        row_ok(db, parameters, row),
    ensures i64::MIN <= row.c_custkey <= i64::MAX, i64::MIN <= row.c_acctbal <= i64::MAX,
        crate::query_semantics::window_fact(db, parameters, row.c_custkey, 0) > 0,
        dimensions.customers.at((row.c_custkey, Seq::empty())).is_some(),
        dimensions.customers.at((row.c_custkey, Seq::empty())).unwrap().entries().contains(
            (customer_inner_view(customer_input_view(*representatives.0.row)), representatives.0)),
        crate::query_semantics::customer_matches(customer_input_view(*representatives.0.row), row),
        dimensions.nations.at((customer_input_view(*representatives.0.row).c_nationkey, Seq::empty())).is_some(),
        dimensions.nations.at((customer_input_view(*representatives.0.row).c_nationkey, Seq::empty())).unwrap().entries().contains(
            (nation_inner_view(nation_input_view(*representatives.1.row)), representatives.1)),
        nation_input_view(*representatives.1.row).n_name == row.n_name,
{
    crate::query_semantics::lemma_row_factor(db, parameters, row);
    lemma_indexed_dimension_multiplicity(db, dimensions, row);
    reveal(dimensions_valid);
    let customers = dimensions.customers.at((row.c_custkey, Seq::empty())).unwrap();
    assert(dimensions.customers.at((row.c_custkey, Seq::empty())).is_some());
    customers.model();
    let weight = customer_weight(indexed_dimension_weight(dimensions, row));
    let position = lemma_sum_nonzero_witness(customers.entries(), weight);
    let customer = customers.entries()[position].1;
    assert(customers.at(customers.entries()[position].0).is_some());
    let view = crate::dimensions::customer_ref_view(&customer.row);
    assert(crate::query_semantics::customer_matches(view, row));
    assert(indexed_names(dimensions, view.c_nationkey, row.n_name) != 0);
    let nations = dimensions.nations.at((view.c_nationkey, Seq::empty())).unwrap();
    assert(dimensions.nations.at((view.c_nationkey, Seq::empty())).is_some());
    nations.model();
    let names = nation_weight(nation_name_weight(row.n_name));
    let nation_position = lemma_sum_nonzero_witness(nations.entries(), names);
    let nation = nations.entries()[nation_position].1;
    assert(nations.at(nations.entries()[nation_position].0).is_some());
    assert(customers.entries().contains(customers.entries()[position]));
    assert(nations.entries().contains(nations.entries()[nation_position]));
    (customer, nation)
}

pub proof fn lemma_group_present(db: Db, parameters: Params, dimensions: Dimensions,
    groups: Index<crate::output::Group>, customer: i64, row: OutRow)
    requires dimensions_valid(db, dimensions), dimensions.customers.wf(), dimensions.nations.wf(),
        groups_valid(groups, customer as int), row_ok(db, parameters, row), row.c_custkey == customer as int,
        forall|key: KeyView| #[trigger] group_total(groups, key) == joined_group_sum(dimensions, customer as int, key),
    ensures groups.at(output_group_key(row)).is_some(),
        same_group(group_columns(groups.at(output_group_key(row)).unwrap()), row),
{
    lemma_representatives(db, parameters, dimensions, row);
    lemma_joined_multiplicity(db, dimensions, row);
    crate::query_semantics::lemma_row_factor(db, parameters, row);
    let key = output_group_key(row);
    assert(group_total(groups, key) > 0);
    assert(groups.at(key).is_some());
    let columns = group_columns(groups.at(key).unwrap());
    assert(output_group_key(columns) == key);
    lemma_group_key_same(columns, row);
}

pub open spec fn seen_prefix(slices: Seq<crate::output::ReadSlice>, seen: Set<i64>) -> bool {
    forall|slice_position: int, rank_position: int| #![trigger slices[slice_position].slice.ranks.entries()[rank_position]]
        0 <= slice_position < slices.len() && 0 <= rank_position < slices[slice_position].consumed@
            ==> seen.contains(slices[slice_position].slice.ranks.entries()[rank_position].1.customer)
}

pub open spec fn seen_covered(db: Db, parameters: Params, top: Seq<OutRow>, seen: Set<i64>) -> bool {
    forall|row: OutRow| #[trigger] row_ok(db, parameters, row)
        && seen.contains(row.c_custkey as i64) ==> crate::selection::covered(top, row)
}

pub proof fn lemma_maximum_bounds(db: Db, dimensions: Dimensions, customer_max: int, nation_max: int)
    requires dimensions_valid(db, dimensions), customer_max >= 0, nation_max >= 0,
        forall|key: KeyView| #[trigger] dimensions.customer_maxima.at(key).is_some()
            ==> dimensions.customer_maxima.at(key).unwrap() <= customer_max,
        forall|key: KeyView| #[trigger] dimensions.nation_maxima.at(key).is_some()
            ==> dimensions.nation_maxima.at(key).unwrap() <= nation_max,
    ensures forall|key: int| #[trigger] sum(db.customer, customer_key_weight(key)) <= customer_max,
        forall|key: int| #[trigger] crate::query_semantics::nation_total(db, key) <= nation_max,
{
    reveal(dimensions_valid);
    assert forall|key: int| #[trigger] sum(db.customer, customer_key_weight(key)) <= customer_max by {
        assert(customer_key_weight(key) =~= (|row: CustomerRow| if row.c_custkey == key { 1int } else { 0int }));
        assert(total_at(dimensions.customer_totals, key) == sum(db.customer, customer_key_weight(key)));
        if dimensions.customer_totals.at((key, Seq::empty())).is_some() {
            let total = dimensions.customer_totals.at((key, Seq::empty())).unwrap();
            assert(dimensions.customer_maxima.at(crate::values::rank_view(total as int, key)).is_some());
            assert(dimensions.customer_maxima.at(crate::values::rank_view(total as int, key)).unwrap() <= customer_max);
        }
    }
    assert forall|key: int| #[trigger] crate::query_semantics::nation_total(db, key) <= nation_max by {
        assert(nation_key_weight(key) =~= (|row: NationRow| if row.n_nationkey == key { 1int } else { 0int }));
        assert(total_at(dimensions.nation_totals, key) == crate::query_semantics::nation_total(db, key));
        if dimensions.nation_totals.at((key, Seq::empty())).is_some() {
            let total = dimensions.nation_totals.at((key, Seq::empty())).unwrap();
            assert(dimensions.nation_maxima.at(crate::values::rank_view(total as int, key)).is_some());
            assert(dimensions.nation_maxima.at(crate::values::rank_view(total as int, key)).unwrap() <= nation_max);
        }
    }
}

pub proof fn lemma_exhausted(db: Db, parameters: Params, dimensions: Dimensions,
    slices: Seq<crate::output::ReadSlice>, top: Seq<OutRow>, seen: Set<i64>)
    requires dimensions_valid(db, dimensions), dimensions.customers.wf(), dimensions.nations.wf(),
        crate::output::read_slices_valid(slices), seen_prefix(slices, seen), seen_covered(db, parameters, top, seen),
        crate::selection::valid_top(top),
        forall|row: OutRow| #[trigger] top.contains(row) ==> row_ok(db, parameters, row),
        forall|customer: int, component: int| #[trigger] crate::output::read_moment_sum(slices, customer, component)
            == crate::query_semantics::window_fact(db, parameters, customer, component),
        forall|position: int| #![trigger slices[position]] 0 <= position < slices.len() ==> slices[position].head.is_none(),
    ensures query_result(db, parameters, top),
{
    assert forall|row: OutRow| #[trigger] row_ok(db, parameters, row) implies crate::selection::covered(top, row) by {
        lemma_representatives(db, parameters, dimensions, row);
        let customer = row.c_custkey as i64;
        if !seen.contains(customer) {
            let weight = crate::output::read_moment_weight(customer as int, 0);
            assert forall|position: int| #![trigger slices[position]] 0 <= position < slices.len()
                implies weight(slices[position]) == 0 by {
                let slice = slices[position];
                assert forall|rank_position: int| #![trigger slice.slice.ranks.entries()[rank_position]]
                    0 <= rank_position < slice.consumed@
                    implies seen.contains(slice.slice.ranks.entries()[rank_position].1.customer) by {}
                crate::read_proofs::lemma_slice_unseen_bound(slice, customer, seen);
            }
            lemma_sum_zero(slices, weight);
            assert(crate::output::read_moment_sum(slices, customer as int, 0) == 0);
            assert(false);
        }
    }
    crate::selection::lemma_query_from_coverage(db, parameters, top);
}

pub proof fn lemma_threshold(db: Db, parameters: Params, dimensions: Dimensions,
    slices: Seq<crate::output::ReadSlice>, top: Seq<OutRow>, seen: Set<i64>, customer_max: int, nation_max: int)
    requires dimensions_valid(db, dimensions), dimensions.customers.wf(), dimensions.nations.wf(),
        crate::output::read_slices_valid(slices), seen_prefix(slices, seen), seen_covered(db, parameters, top, seen),
        crate::selection::valid_top(top), top.len() == 20,
        forall|row: OutRow| #[trigger] top.contains(row) ==> row_ok(db, parameters, row),
        forall|customer: int, component: int| #[trigger] crate::output::read_moment_sum(slices, customer, component)
            == crate::query_semantics::window_fact(db, parameters, customer, component),
        customer_max >= 0, nation_max >= 0,
        forall|key: int| #[trigger] sum(db.customer, customer_key_weight(key)) <= customer_max,
        forall|key: int| #[trigger] crate::query_semantics::nation_total(db, key) <= nation_max,
        top[19].revenue > sum(slices, crate::output::head_upper_weight()) * (customer_max * nation_max),
    ensures query_result(db, parameters, top),
{
    assert forall|row: OutRow| #[trigger] row_ok(db, parameters, row) implies crate::selection::covered(top, row) by {
        lemma_representatives(db, parameters, dimensions, row);
        let customer = row.c_custkey as i64;
        if !seen.contains(customer) {
            crate::read_proofs::lemma_window_unseen_bound(slices, db, parameters, customer, seen);
            assert(customer_key_weight(row.c_custkey) =~=
                (|customer: CustomerRow| if customer.c_custkey == row.c_custkey { 1int } else { 0int }));
            crate::query_semantics::lemma_dimension_bound(db, row, customer_max, nation_max);
            assert(customer_max * nation_max >= 0) by (nonlinear_arith)
                requires customer_max >= 0, nation_max >= 0;
            crate::query_semantics::lemma_unseen_bound(db, parameters, row,
                sum(slices, crate::output::head_upper_weight()), customer_max * nation_max);
        }
    }
    crate::selection::lemma_query_from_coverage(db, parameters, top);
}

}
