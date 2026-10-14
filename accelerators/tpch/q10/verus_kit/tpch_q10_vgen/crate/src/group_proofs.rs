use vstd::prelude::*;
use verus_common::sums::*;
use verus_prooflib::ordered::{Index, KeyView};
use crate::gen_spec::*;
use crate::gen_obligations::{customer_input_view, nation_input_view};
use crate::dimensions::*;
use crate::output::{Group, group_key_view};
use crate::values::*;
use crate::encoding::*;
use crate::paged::*;
use crate::customers::*;

verus! {

pub open spec fn output_group_key(group: OutRow) -> KeyView {
    (0, encode_text(group.c_name) + encode_nat((group.c_acctbal - i64::MIN as int) as nat, 8)
        + encode_text(group.c_phone) + encode_text(group.n_name)
        + encode_text(group.c_address) + encode_text(group.c_comment))
}

pub open spec fn group_columns(group: Group) -> OutRow {
    let customer = customer_input_view(*group.customer);
    let nation = nation_input_view(*group.nation);
    OutRow { c_custkey: customer.c_custkey, c_name: customer.c_name, revenue: 0int,
        c_acctbal: customer.c_acctbal, c_phone: customer.c_phone, n_name: nation.n_name,
        c_address: customer.c_address, c_comment: customer.c_comment }
}

pub open spec fn group_total(index: Index<Group>, key: KeyView) -> int {
    match index.at(key) { Some(group) => group.multiplicity as int, None => 0int }
}

pub open spec fn groups_valid(index: Index<Group>, customer: int) -> bool {
    &&& index.wf()
    &&& forall|key: KeyView| #[trigger] index.at(key).is_some() ==> {
        let group = index.at(key).unwrap();
        &&& group.multiplicity > 0
        &&& customer_input_view(*group.customer).c_custkey == customer
        &&& key == group_key_view(customer_input_view(*group.customer), nation_input_view(*group.nation))
    }
}

pub open spec fn nation_group_weight(customer: CustomerRow, key: KeyView) -> spec_fn((KeyView, Nation)) -> int {
    |entry: (KeyView, Nation)| if group_key_view(customer, nation_input_view(*entry.1.row)) == key { entry.1.count as int } else { 0int }
}

pub open spec fn nation_group_sum(dimensions: Dimensions, customer: CustomerRow, key: KeyView) -> int {
    match dimensions.nations.at((customer.c_nationkey, Seq::empty())) {
        Some(nations) => sum(nations.entries(), nation_group_weight(customer, key)), None => 0int,
    }
}

pub open spec fn customer_group_weight(dimensions: Dimensions, key: KeyView) -> spec_fn(Customer) -> int {
    |variant: Customer| variant.count as int * nation_group_sum(dimensions, customer_input_view(*variant.row), key)
}

pub open spec fn joined_group_sum(dimensions: Dimensions, customer: int, key: KeyView) -> int {
    sum(variants_at(dimensions.customers, customer), customer_group_weight(dimensions, key))
}

pub proof fn lemma_group_key_same(left: OutRow, right: OutRow)
    requires left.c_custkey == right.c_custkey,
        i64::MIN <= left.c_acctbal <= i64::MAX, i64::MIN <= right.c_acctbal <= i64::MAX,
        output_group_key(left) == output_group_key(right),
    ensures same_group(left, right),
{
    let left_comment = text(left.c_comment);
    let right_comment = text(right.c_comment);
    let left_address = text(left.c_address) + left_comment;
    let right_address = text(right.c_address) + right_comment;
    let left_nation = text(left.n_name) + left_address;
    let right_nation = text(right.n_name) + right_address;
    let left_phone = text(left.c_phone) + left_nation;
    let right_phone = text(right.c_phone) + right_nation;
    let left_balance = number((left.c_acctbal - i64::MIN as int) as nat, 8) + left_phone;
    let right_balance = number((right.c_acctbal - i64::MIN as int) as nat, 8) + right_phone;
    assert(output_group_key(left).1 =~= text(left.c_name) + left_balance);
    assert(output_group_key(right).1 =~= text(right.c_name) + right_balance);
    lemma_text_frame(left.c_name, right.c_name, left_balance, right_balance);
    crate::key_facts::lemma_machine_radices();
    lemma_number_frame((left.c_acctbal - i64::MIN as int) as nat, (right.c_acctbal - i64::MIN as int) as nat,
        8, left_phone, right_phone);
    lemma_text_frame(left.c_phone, right.c_phone, left_nation, right_nation);
    lemma_text_frame(left.n_name, right.n_name, left_address, right_address);
    lemma_text_frame(left.c_address, right.c_address, left_comment, right_comment);
    assert(left_comment + Seq::<u8>::empty() =~= left_comment);
    assert(right_comment + Seq::<u8>::empty() =~= right_comment);
    lemma_text_frame(left.c_comment, right.c_comment, Seq::empty(), Seq::empty());
}

pub proof fn lemma_joined_multiplicity(db: Db, dimensions: Dimensions, group: OutRow)
    requires crate::model::dimensions_valid(db, dimensions), dimensions.nations.wf(),
        i64::MIN <= group.c_acctbal <= i64::MAX,
    ensures joined_group_sum(dimensions, group.c_custkey, output_group_key(group))
        == crate::query_semantics::dimension_multiplicity(db, group),
{
    crate::dimension_query::lemma_indexed_dimension_multiplicity(db, dimensions, group);
    reveal(crate::model::dimensions_valid);
    let variants = variants_at(dimensions.customers, group.c_custkey);
    let folded = customer_group_weight(dimensions, output_group_key(group));
    let indexed = variant_weight(crate::dimension_query::indexed_dimension_weight(dimensions, group));
    assert forall|position: int| #![trigger variants[position]] 0 <= position < variants.len()
        implies folded(variants[position]) == indexed(variants[position]) by {
        let customer = customer_input_view(*variants[position].row);
        match dimensions.nations.at((customer.c_nationkey, Seq::empty())) {
            Some(nations) => {
                nations.model();
                let weights = nation_group_weight(customer, output_group_key(group));
                let names = crate::dimension_updates::nation_weight(crate::dimension_query::nation_name_weight(group.n_name));
                assert forall|nation_position: int| #![trigger nations.entries()[nation_position]]
                    0 <= nation_position < nations.entries().len() implies weights(nations.entries()[nation_position])
                        == if crate::query_semantics::customer_matches(customer, group) { names(nations.entries()[nation_position]) } else { 0int } by {
                    let nation = nation_input_view(*nations.entries()[nation_position].1.row);
                    let columns = OutRow { c_custkey: customer.c_custkey, c_name: customer.c_name, revenue: 0int,
                        c_acctbal: customer.c_acctbal, c_phone: customer.c_phone, n_name: nation.n_name,
                        c_address: customer.c_address, c_comment: customer.c_comment };
                    assert(output_group_key(columns) == group_key_view(customer, nation));
                    if output_group_key(columns) == output_group_key(group) {
                        lemma_group_key_same(columns, group);
                    }
                }
                if crate::query_semantics::customer_matches(customer, group) {
                    lemma_sum_ext(nations.entries(), weights, names);
                } else { lemma_sum_zero(nations.entries(), weights); }
            },
            None => {},
        }
    }
    lemma_sum_ext(variants, folded, indexed);
}

pub proof fn lemma_coalesced_candidate(db: Db, parameters: Params, dimensions: Dimensions,
    groups: Index<Group>, candidate: crate::output::Candidate, customer: i64, moments: Moments)
    requires crate::model::dimensions_valid(db, dimensions), dimensions.nations.wf(),
        groups_valid(groups, customer as int),
        forall|key: KeyView| #[trigger] group_total(groups, key) == joined_group_sum(dimensions, customer as int, key),
        groups.at(output_group_key(candidate.view())) == Some(*candidate.group),
        moments.count > 0, moments.count as int == crate::query_semantics::window_fact(db, parameters, customer as int, 0),
        moments.revenue as int == crate::query_semantics::window_fact(db, parameters, customer as int, 1),
        candidate.revenue as int == moments.revenue as int * candidate.group.multiplicity as int,
    ensures row_ok(db, parameters, candidate.view()),
{
    let row = candidate.view();
    let key = output_group_key(row);
    assert(groups.at(key).is_some());
    assert(row.c_custkey == customer as int);
    assert(group_total(groups, key) == joined_group_sum(dimensions, customer as int, key));
    lemma_joined_multiplicity(db, dimensions, row);
    crate::query_semantics::lemma_row_factor(db, parameters, row);
}

pub proof fn lemma_singleton_candidate(db: Db, parameters: Params, dimensions: Dimensions,
    nations: &Index<Nation>, cust: &Customer, nation: &Nation,
    candidate: crate::output::Candidate, moments: Moments)
    requires crate::model::dimensions_valid(db, dimensions), dimensions.nations.wf(),
        variants_at(dimensions.customers, customer_input_view(*cust.row).c_custkey).len() == 1,
        variants_at(dimensions.customers, customer_input_view(*cust.row).c_custkey)[0] == *cust,
        dimensions.nations.at((customer_input_view(*cust.row).c_nationkey, Seq::empty())) == Some(*nations),
        nations.entries().len() == 1, nations.entries()[0].1 == *nation,
        customer_input_view(*candidate.group.customer) == customer_input_view(*cust.row),
        nation_input_view(*candidate.group.nation) == nation_input_view(*nation.row),
        candidate.group.multiplicity as int == cust.count as int * nation.count as int,
        cust.count > 0, nation.count > 0, moments.count > 0,
        moments.count as int == crate::query_semantics::window_fact(db, parameters, customer_input_view(*cust.row).c_custkey, 0),
        moments.revenue as int == crate::query_semantics::window_fact(db, parameters, customer_input_view(*cust.row).c_custkey, 1),
        candidate.revenue as int == moments.revenue as int * candidate.group.multiplicity as int,
    ensures row_ok(db, parameters, candidate.view()),
{
    let row = candidate.view();
    crate::dimension_query::lemma_indexed_dimension_multiplicity(db, dimensions, row);
    let customer_weight = variant_weight(crate::dimension_query::indexed_dimension_weight(dimensions, row));
    let nation_weight = crate::dimension_updates::nation_weight(crate::dimension_query::nation_name_weight(row.n_name));
    let variants = variants_at(dimensions.customers, cust.row.c_custkey as int);
    assert(variants =~= seq![variants[0]]);
    assert(nations.entries() =~= seq![nations.entries()[0]]);
    lemma_sum_singleton(nations.entries()[0], nation_weight);
    lemma_sum_singleton(variants[0], customer_weight);
    assert(crate::query_semantics::dimension_multiplicity(db, row) == cust.count as int * nation.count as int);
    assert(cust.count as int * nation.count as int > 0) by (nonlinear_arith)
        requires cust.count > 0, nation.count > 0;
    crate::query_semantics::lemma_row_factor(db, parameters, row);
}

}
