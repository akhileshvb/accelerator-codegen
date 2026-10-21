use vstd::prelude::*;
use verus_common::sums::*;
use verus_common::rel::*;
use verus_common::sqlfns::*;
use crate::gen_spec::*;
use crate::model::*;

verus! {
pub open spec fn tuple_in_db(db: Db, tuple: AllNationsTuple) -> bool {
    db.part.contains(tuple.part) && db.supplier.contains(tuple.supplier) && db.lineitem.contains(tuple.lineitem)
        && db.orders.contains(tuple.orders) && db.customer.contains(tuple.customer) && db.nation.contains(tuple.n1)
        && db.nation.contains(tuple.n2) && db.region.contains(tuple.region)
}

pub open spec fn zero_weight(db: Db, parameters: Params, weight: spec_fn(AllNationsTuple) -> int) -> bool {
    forall|tuple: AllNationsTuple| tuple_in_db(db, tuple)
        && all_nations_where_clause(db, parameters, tuple.part, tuple.supplier, tuple.lineitem, tuple.orders,
            tuple.customer, tuple.n1, tuple.n2, tuple.region) ==> #[trigger] weight(tuple) == 0
}

pub proof fn lemma_sql_zero_8(db: Db, parameters: Params, part: PartRow, supplier: SupplierRow, lineitem: LineitemRow, orders: OrdersRow, customer: CustomerRow, n1: NationRow, n2: NationRow, region: RegionRow, weight: spec_fn(AllNationsTuple) -> int)
    requires zero_weight(db, parameters, weight), db.part.contains(part), db.supplier.contains(supplier), db.lineitem.contains(lineitem), db.orders.contains(orders), db.customer.contains(customer), db.nation.contains(n1), db.nation.contains(n2), db.region.contains(region),
    ensures sql_sum_8(db, parameters, part, supplier, lineitem, orders, customer, n1, n2, region, weight) == 0,
{
    hide(sum);
    reveal(sql_sum_8);
    let tuple = AllNationsTuple { part, supplier, lineitem, orders, customer, n1, n2, region };
    assert(tuple_in_db(db, tuple));
}

pub proof fn lemma_sql_zero_7(db: Db, parameters: Params, part: PartRow, supplier: SupplierRow, lineitem: LineitemRow, orders: OrdersRow, customer: CustomerRow, n1: NationRow, n2: NationRow, weight: spec_fn(AllNationsTuple) -> int)
    requires zero_weight(db, parameters, weight), db.part.contains(part), db.supplier.contains(supplier), db.lineitem.contains(lineitem), db.orders.contains(orders), db.customer.contains(customer), db.nation.contains(n1), db.nation.contains(n2),
    ensures sql_sum_7(db, parameters, part, supplier, lineitem, orders, customer, n1, n2, weight) == 0,
{
    hide(sum);
    reveal(sql_sum_7);
    let term = |region: RegionRow| sql_sum_8(db, parameters, part, supplier, lineitem, orders, customer, n1, n2, region, weight);
    assert forall|index: int| 0 <= index < db.region.len() implies term(#[trigger] db.region[index]) == 0 by {
        let region = db.region[index];
        assert(db.region.contains(region));
        lemma_sql_zero_8(db, parameters, part, supplier, lineitem, orders, customer, n1, n2, region, weight);
    }
    lemma_sum_zero(db.region, term);
}

pub proof fn lemma_sql_zero_6(db: Db, parameters: Params, part: PartRow, supplier: SupplierRow, lineitem: LineitemRow, orders: OrdersRow, customer: CustomerRow, n1: NationRow, weight: spec_fn(AllNationsTuple) -> int)
    requires zero_weight(db, parameters, weight), db.part.contains(part), db.supplier.contains(supplier), db.lineitem.contains(lineitem), db.orders.contains(orders), db.customer.contains(customer), db.nation.contains(n1),
    ensures sql_sum_6(db, parameters, part, supplier, lineitem, orders, customer, n1, weight) == 0,
{
    hide(sum);
    reveal(sql_sum_6);
    let term = |n2: NationRow| sql_sum_7(db, parameters, part, supplier, lineitem, orders, customer, n1, n2, weight);
    assert forall|index: int| 0 <= index < db.nation.len() implies term(#[trigger] db.nation[index]) == 0 by {
        let n2 = db.nation[index];
        assert(db.nation.contains(n2));
        lemma_sql_zero_7(db, parameters, part, supplier, lineitem, orders, customer, n1, n2, weight);
    }
    lemma_sum_zero(db.nation, term);
}

pub proof fn lemma_sql_zero_5(db: Db, parameters: Params, part: PartRow, supplier: SupplierRow, lineitem: LineitemRow, orders: OrdersRow, customer: CustomerRow, weight: spec_fn(AllNationsTuple) -> int)
    requires zero_weight(db, parameters, weight), db.part.contains(part), db.supplier.contains(supplier), db.lineitem.contains(lineitem), db.orders.contains(orders), db.customer.contains(customer),
    ensures sql_sum_5(db, parameters, part, supplier, lineitem, orders, customer, weight) == 0,
{
    hide(sum);
    reveal(sql_sum_5);
    let term = |n1: NationRow| sql_sum_6(db, parameters, part, supplier, lineitem, orders, customer, n1, weight);
    assert forall|index: int| 0 <= index < db.nation.len() implies term(#[trigger] db.nation[index]) == 0 by {
        let n1 = db.nation[index];
        assert(db.nation.contains(n1));
        lemma_sql_zero_6(db, parameters, part, supplier, lineitem, orders, customer, n1, weight);
    }
    lemma_sum_zero(db.nation, term);
}

pub proof fn lemma_sql_zero_4(db: Db, parameters: Params, part: PartRow, supplier: SupplierRow, lineitem: LineitemRow, orders: OrdersRow, weight: spec_fn(AllNationsTuple) -> int)
    requires zero_weight(db, parameters, weight), db.part.contains(part), db.supplier.contains(supplier), db.lineitem.contains(lineitem), db.orders.contains(orders),
    ensures sql_sum_4(db, parameters, part, supplier, lineitem, orders, weight) == 0,
{
    hide(sum);
    reveal(sql_sum_4);
    let term = |customer: CustomerRow| sql_sum_5(db, parameters, part, supplier, lineitem, orders, customer, weight);
    assert forall|index: int| 0 <= index < db.customer.len() implies term(#[trigger] db.customer[index]) == 0 by {
        let customer = db.customer[index];
        assert(db.customer.contains(customer));
        lemma_sql_zero_5(db, parameters, part, supplier, lineitem, orders, customer, weight);
    }
    lemma_sum_zero(db.customer, term);
}

pub proof fn lemma_sql_zero_3(db: Db, parameters: Params, part: PartRow, supplier: SupplierRow, lineitem: LineitemRow, weight: spec_fn(AllNationsTuple) -> int)
    requires zero_weight(db, parameters, weight), db.part.contains(part), db.supplier.contains(supplier), db.lineitem.contains(lineitem),
    ensures sql_sum_3(db, parameters, part, supplier, lineitem, weight) == 0,
{
    hide(sum);
    reveal(sql_sum_3);
    let term = |orders: OrdersRow| sql_sum_4(db, parameters, part, supplier, lineitem, orders, weight);
    assert forall|index: int| 0 <= index < db.orders.len() implies term(#[trigger] db.orders[index]) == 0 by {
        let orders = db.orders[index];
        assert(db.orders.contains(orders));
        lemma_sql_zero_4(db, parameters, part, supplier, lineitem, orders, weight);
    }
    lemma_sum_zero(db.orders, term);
}

pub proof fn lemma_sql_zero_2(db: Db, parameters: Params, part: PartRow, supplier: SupplierRow, weight: spec_fn(AllNationsTuple) -> int)
    requires zero_weight(db, parameters, weight), db.part.contains(part), db.supplier.contains(supplier),
    ensures sql_sum_2(db, parameters, part, supplier, weight) == 0,
{
    hide(sum);
    reveal(sql_sum_2);
    let term = |lineitem: LineitemRow| sql_sum_3(db, parameters, part, supplier, lineitem, weight);
    assert forall|index: int| 0 <= index < db.lineitem.len() implies term(#[trigger] db.lineitem[index]) == 0 by {
        let lineitem = db.lineitem[index];
        assert(db.lineitem.contains(lineitem));
        lemma_sql_zero_3(db, parameters, part, supplier, lineitem, weight);
    }
    lemma_sum_zero(db.lineitem, term);
}

pub proof fn lemma_sql_zero_1(db: Db, parameters: Params, part: PartRow, weight: spec_fn(AllNationsTuple) -> int)
    requires zero_weight(db, parameters, weight), db.part.contains(part),
    ensures sql_sum_1(db, parameters, part, weight) == 0,
{
    hide(sum);
    reveal(sql_sum_1);
    let term = |supplier: SupplierRow| sql_sum_2(db, parameters, part, supplier, weight);
    assert forall|index: int| 0 <= index < db.supplier.len() implies term(#[trigger] db.supplier[index]) == 0 by {
        let supplier = db.supplier[index];
        assert(db.supplier.contains(supplier));
        lemma_sql_zero_2(db, parameters, part, supplier, weight);
    }
    lemma_sum_zero(db.supplier, term);
}

pub proof fn lemma_sql_zero_0(db: Db, parameters: Params, weight: spec_fn(AllNationsTuple) -> int)
    requires zero_weight(db, parameters, weight),
    ensures sql_sum_0(db, parameters, weight) == 0,
{
    hide(sum);
    reveal(sql_sum_0);
    let term = |part: PartRow| sql_sum_1(db, parameters, part, weight);
    assert forall|index: int| 0 <= index < db.part.len() implies term(#[trigger] db.part[index]) == 0 by {
        let part = db.part[index];
        assert(db.part.contains(part));
        lemma_sql_zero_1(db, parameters, part, weight);
    }
    lemma_sum_zero(db.part, term);
}

pub proof fn lemma_sql_parameters_8(db: Db, parameters: Params, other: Params, part: PartRow, supplier: SupplierRow, lineitem: LineitemRow, orders: OrdersRow, customer: CustomerRow, n1: NationRow, n2: NationRow, region: RegionRow, weight: spec_fn(AllNationsTuple) -> int)
    requires parameters.p2 == other.p2, parameters.p3 == other.p3,
    ensures sql_sum_8(db, parameters, part, supplier, lineitem, orders, customer, n1, n2, region, weight) == sql_sum_8(db, other, part, supplier, lineitem, orders, customer, n1, n2, region, weight),
{
    hide(sum);
    reveal(sql_sum_8);
}

pub proof fn lemma_sql_parameters_7(db: Db, parameters: Params, other: Params, part: PartRow, supplier: SupplierRow, lineitem: LineitemRow, orders: OrdersRow, customer: CustomerRow, n1: NationRow, n2: NationRow, weight: spec_fn(AllNationsTuple) -> int)
    requires parameters.p2 == other.p2, parameters.p3 == other.p3,
    ensures sql_sum_7(db, parameters, part, supplier, lineitem, orders, customer, n1, n2, weight) == sql_sum_7(db, other, part, supplier, lineitem, orders, customer, n1, n2, weight),
{
    hide(sum);
    reveal(sql_sum_7);
    let left = |region: RegionRow| sql_sum_8(db, parameters, part, supplier, lineitem, orders, customer, n1, n2, region, weight);
    let right = |region: RegionRow| sql_sum_8(db, other, part, supplier, lineitem, orders, customer, n1, n2, region, weight);
    assert forall|region: RegionRow| #[trigger] left(region) == right(region) by {
        lemma_sql_parameters_8(db, parameters, other, part, supplier, lineitem, orders, customer, n1, n2, region, weight);
    }
    lemma_sum_ext_all(db.region, left, right);
}

pub proof fn lemma_sql_parameters_6(db: Db, parameters: Params, other: Params, part: PartRow, supplier: SupplierRow, lineitem: LineitemRow, orders: OrdersRow, customer: CustomerRow, n1: NationRow, weight: spec_fn(AllNationsTuple) -> int)
    requires parameters.p2 == other.p2, parameters.p3 == other.p3,
    ensures sql_sum_6(db, parameters, part, supplier, lineitem, orders, customer, n1, weight) == sql_sum_6(db, other, part, supplier, lineitem, orders, customer, n1, weight),
{
    hide(sum);
    reveal(sql_sum_6);
    let left = |n2: NationRow| sql_sum_7(db, parameters, part, supplier, lineitem, orders, customer, n1, n2, weight);
    let right = |n2: NationRow| sql_sum_7(db, other, part, supplier, lineitem, orders, customer, n1, n2, weight);
    assert forall|n2: NationRow| #[trigger] left(n2) == right(n2) by {
        lemma_sql_parameters_7(db, parameters, other, part, supplier, lineitem, orders, customer, n1, n2, weight);
    }
    lemma_sum_ext_all(db.nation, left, right);
}

pub proof fn lemma_sql_parameters_5(db: Db, parameters: Params, other: Params, part: PartRow, supplier: SupplierRow, lineitem: LineitemRow, orders: OrdersRow, customer: CustomerRow, weight: spec_fn(AllNationsTuple) -> int)
    requires parameters.p2 == other.p2, parameters.p3 == other.p3,
    ensures sql_sum_5(db, parameters, part, supplier, lineitem, orders, customer, weight) == sql_sum_5(db, other, part, supplier, lineitem, orders, customer, weight),
{
    hide(sum);
    reveal(sql_sum_5);
    let left = |n1: NationRow| sql_sum_6(db, parameters, part, supplier, lineitem, orders, customer, n1, weight);
    let right = |n1: NationRow| sql_sum_6(db, other, part, supplier, lineitem, orders, customer, n1, weight);
    assert forall|n1: NationRow| #[trigger] left(n1) == right(n1) by {
        lemma_sql_parameters_6(db, parameters, other, part, supplier, lineitem, orders, customer, n1, weight);
    }
    lemma_sum_ext_all(db.nation, left, right);
}

pub proof fn lemma_sql_parameters_4(db: Db, parameters: Params, other: Params, part: PartRow, supplier: SupplierRow, lineitem: LineitemRow, orders: OrdersRow, weight: spec_fn(AllNationsTuple) -> int)
    requires parameters.p2 == other.p2, parameters.p3 == other.p3,
    ensures sql_sum_4(db, parameters, part, supplier, lineitem, orders, weight) == sql_sum_4(db, other, part, supplier, lineitem, orders, weight),
{
    hide(sum);
    reveal(sql_sum_4);
    let left = |customer: CustomerRow| sql_sum_5(db, parameters, part, supplier, lineitem, orders, customer, weight);
    let right = |customer: CustomerRow| sql_sum_5(db, other, part, supplier, lineitem, orders, customer, weight);
    assert forall|customer: CustomerRow| #[trigger] left(customer) == right(customer) by {
        lemma_sql_parameters_5(db, parameters, other, part, supplier, lineitem, orders, customer, weight);
    }
    lemma_sum_ext_all(db.customer, left, right);
}

pub proof fn lemma_sql_parameters_3(db: Db, parameters: Params, other: Params, part: PartRow, supplier: SupplierRow, lineitem: LineitemRow, weight: spec_fn(AllNationsTuple) -> int)
    requires parameters.p2 == other.p2, parameters.p3 == other.p3,
    ensures sql_sum_3(db, parameters, part, supplier, lineitem, weight) == sql_sum_3(db, other, part, supplier, lineitem, weight),
{
    hide(sum);
    reveal(sql_sum_3);
    let left = |orders: OrdersRow| sql_sum_4(db, parameters, part, supplier, lineitem, orders, weight);
    let right = |orders: OrdersRow| sql_sum_4(db, other, part, supplier, lineitem, orders, weight);
    assert forall|orders: OrdersRow| #[trigger] left(orders) == right(orders) by {
        lemma_sql_parameters_4(db, parameters, other, part, supplier, lineitem, orders, weight);
    }
    lemma_sum_ext_all(db.orders, left, right);
}

pub proof fn lemma_sql_parameters_2(db: Db, parameters: Params, other: Params, part: PartRow, supplier: SupplierRow, weight: spec_fn(AllNationsTuple) -> int)
    requires parameters.p2 == other.p2, parameters.p3 == other.p3,
    ensures sql_sum_2(db, parameters, part, supplier, weight) == sql_sum_2(db, other, part, supplier, weight),
{
    hide(sum);
    reveal(sql_sum_2);
    let left = |lineitem: LineitemRow| sql_sum_3(db, parameters, part, supplier, lineitem, weight);
    let right = |lineitem: LineitemRow| sql_sum_3(db, other, part, supplier, lineitem, weight);
    assert forall|lineitem: LineitemRow| #[trigger] left(lineitem) == right(lineitem) by {
        lemma_sql_parameters_3(db, parameters, other, part, supplier, lineitem, weight);
    }
    lemma_sum_ext_all(db.lineitem, left, right);
}

pub proof fn lemma_sql_parameters_1(db: Db, parameters: Params, other: Params, part: PartRow, weight: spec_fn(AllNationsTuple) -> int)
    requires parameters.p2 == other.p2, parameters.p3 == other.p3,
    ensures sql_sum_1(db, parameters, part, weight) == sql_sum_1(db, other, part, weight),
{
    hide(sum);
    reveal(sql_sum_1);
    let left = |supplier: SupplierRow| sql_sum_2(db, parameters, part, supplier, weight);
    let right = |supplier: SupplierRow| sql_sum_2(db, other, part, supplier, weight);
    assert forall|supplier: SupplierRow| #[trigger] left(supplier) == right(supplier) by {
        lemma_sql_parameters_2(db, parameters, other, part, supplier, weight);
    }
    lemma_sum_ext_all(db.supplier, left, right);
}

pub proof fn lemma_sql_parameters_0(db: Db, parameters: Params, other: Params, weight: spec_fn(AllNationsTuple) -> int)
    requires parameters.p2 == other.p2, parameters.p3 == other.p3,
    ensures sql_sum_0(db, parameters, weight) == sql_sum_0(db, other, weight),
{
    hide(sum);
    reveal(sql_sum_0);
    let left = |part: PartRow| sql_sum_1(db, parameters, part, weight);
    let right = |part: PartRow| sql_sum_1(db, other, part, weight);
    assert forall|part: PartRow| #[trigger] left(part) == right(part) by {
        lemma_sql_parameters_1(db, parameters, other, part, weight);
    }
    lemma_sum_ext_all(db.part, left, right);
}

pub proof fn lemma_group_parameters(db: Db, parameters: Params, other: Params, year: int, weight: spec_fn(Tuple) -> int)
    requires parameters.p2 == other.p2, parameters.p3 == other.p3,
    ensures group_sum(db, parameters, year, weight) == group_sum(db, other, year, weight),
{
    hide(group_sum);
    lemma_group_sum(db, parameters, year, weight);
    lemma_group_sum(db, other, year, weight);
    lemma_sql_parameters_0(db, parameters, other, |tuple: AllNationsTuple| grouped_weight(tuple, year, weight));
}

pub proof fn lemma_query_year(date: int)
    requires 9131 <= date <= 9861,
    ensures civil_year(date) == year_of(date), civil_year(date) == 1995 || civil_year(date) == 1996,
{
    assert(civil_year(date) == year_of(date)) by (nonlinear_arith)
        requires 9131 <= date <= 9861;
}

pub proof fn lemma_missing_group(data: Data, db: Db, parameters: Params, year: int, weight: spec_fn(Tuple) -> int)
    requires dimension_meaning(data, db),
        data.dictionary.regions.resolve_spec(parameters.p2) is None || data.dictionary.types.resolve_spec(parameters.p3) is None
            || (year != 1995 && year != 1996),
    ensures group_sum(db, parameters, year, weight) == 0,
{
    hide(sum);
    hide(group_sum);
    reveal(dimension_meaning);
    let raw = |tuple: AllNationsTuple| grouped_weight(tuple, year, weight);
    assert forall|tuple: AllNationsTuple| tuple_in_db(db, tuple)
        && all_nations_where_clause(db, parameters, tuple.part, tuple.supplier, tuple.lineitem, tuple.orders,
            tuple.customer, tuple.n1, tuple.n2, tuple.region) implies #[trigger] raw(tuple) == 0 by {
        lemma_query_year(tuple.orders.o_orderdate);
        if data.dictionary.regions.resolve_spec(parameters.p2) is None {
            let index = choose|index: int| 0 <= index < db.region.len() && #[trigger] db.region[index] == tuple.region;
            assert(text_present(data.dictionary, 2, db.region[index].r_name));
        }
        if data.dictionary.types.resolve_spec(parameters.p3) is None {
            let index = choose|index: int| 0 <= index < db.part.len() && #[trigger] db.part[index] == tuple.part;
            assert(text_present(data.dictionary, 0, db.part[index].p_type));
        }
    }
    lemma_sql_zero_0(db, parameters, raw);
    lemma_group_sum(db, parameters, year, weight);
}

pub proof fn lemma_missing_numerator(data: Data, db: Db, parameters: Params, year: int)
    requires dimension_meaning(data, db), data.dictionary.names.resolve_spec(parameters.p1) is None,
    ensures group_sum(db, parameters, year, |tuple: Tuple| if tuple.all_nations.nation == parameters.p1 { tuple.all_nations.volume } else { 0 }) == 0,
{
    hide(sum);
    hide(group_sum);
    reveal(dimension_meaning);
    let weight = |tuple: Tuple| if tuple.all_nations.nation == parameters.p1 { tuple.all_nations.volume } else { 0int };
    let raw = |tuple: AllNationsTuple| grouped_weight(tuple, year, weight);
    assert forall|tuple: AllNationsTuple| tuple_in_db(db, tuple)
        && all_nations_where_clause(db, parameters, tuple.part, tuple.supplier, tuple.lineitem, tuple.orders,
            tuple.customer, tuple.n1, tuple.n2, tuple.region) implies #[trigger] raw(tuple) == 0 by {
        let index = choose|index: int| 0 <= index < db.nation.len() && #[trigger] db.nation[index] == tuple.n2;
        assert(text_present(data.dictionary, 1, db.nation[index].n_name));
    }
    lemma_sql_zero_0(db, parameters, raw);
    lemma_group_sum(db, parameters, year, weight);
}


#[verifier::opaque]
pub open spec fn sql_sum_8(db: Db, parameters: Params, part: PartRow, supplier: SupplierRow, lineitem: LineitemRow, orders: OrdersRow, customer: CustomerRow, n1: NationRow, n2: NationRow, region: RegionRow, weight: spec_fn(AllNationsTuple) -> int) -> int {
    if all_nations_where_clause(db, parameters, part, supplier, lineitem, orders, customer, n1, n2, region) { weight(AllNationsTuple { part, supplier, lineitem, orders, customer, n1, n2, region }) } else { 0 }
}

pub proof fn lemma_sql_sum_8(db: Db, parameters: Params, part: PartRow, supplier: SupplierRow, lineitem: LineitemRow, orders: OrdersRow, customer: CustomerRow, n1: NationRow, n2: NationRow, region: RegionRow, weight: spec_fn(AllNationsTuple) -> int)
    ensures sum(all_nations_tuples_8(db, parameters, part, supplier, lineitem, orders, customer, n1, n2, region), weight) == sql_sum_8(db, parameters, part, supplier, lineitem, orders, customer, n1, n2, region, weight),
{
    hide(sum);
    reveal(sql_sum_8);
    lemma_sum_opt(all_nations_where_clause(db, parameters, part, supplier, lineitem, orders, customer, n1, n2, region), AllNationsTuple { part, supplier, lineitem, orders, customer, n1, n2, region }, weight);
}

#[verifier::opaque]
pub open spec fn sql_sum_7(db: Db, parameters: Params, part: PartRow, supplier: SupplierRow, lineitem: LineitemRow, orders: OrdersRow, customer: CustomerRow, n1: NationRow, n2: NationRow, weight: spec_fn(AllNationsTuple) -> int) -> int {
    sum(db.region, |region: RegionRow| sql_sum_8(db, parameters, part, supplier, lineitem, orders, customer, n1, n2, region, weight))
}

pub proof fn lemma_sql_sum_7(db: Db, parameters: Params, part: PartRow, supplier: SupplierRow, lineitem: LineitemRow, orders: OrdersRow, customer: CustomerRow, n1: NationRow, n2: NationRow, weight: spec_fn(AllNationsTuple) -> int)
    ensures sum(all_nations_tuples_7(db, parameters, part, supplier, lineitem, orders, customer, n1, n2), weight) == sql_sum_7(db, parameters, part, supplier, lineitem, orders, customer, n1, n2, weight),
{
    hide(sum);
    reveal(sql_sum_7);
    let expand = |region: RegionRow| all_nations_tuples_8(db, parameters, part, supplier, lineitem, orders, customer, n1, n2, region);
    let raw = |region: RegionRow| sum(expand(region), weight);
    let nested = |region: RegionRow| sql_sum_8(db, parameters, part, supplier, lineitem, orders, customer, n1, n2, region, weight);
    assert forall|region: RegionRow| #[trigger] raw(region) == nested(region) by {
        lemma_sql_sum_8(db, parameters, part, supplier, lineitem, orders, customer, n1, n2, region, weight);
    }
    lemma_sum_flat_map(db.region, expand, weight);
    lemma_sum_ext_all(db.region, raw, nested);
}

#[verifier::opaque]
pub open spec fn sql_sum_6(db: Db, parameters: Params, part: PartRow, supplier: SupplierRow, lineitem: LineitemRow, orders: OrdersRow, customer: CustomerRow, n1: NationRow, weight: spec_fn(AllNationsTuple) -> int) -> int {
    sum(db.nation, |n2: NationRow| sql_sum_7(db, parameters, part, supplier, lineitem, orders, customer, n1, n2, weight))
}

pub proof fn lemma_sql_sum_6(db: Db, parameters: Params, part: PartRow, supplier: SupplierRow, lineitem: LineitemRow, orders: OrdersRow, customer: CustomerRow, n1: NationRow, weight: spec_fn(AllNationsTuple) -> int)
    ensures sum(all_nations_tuples_6(db, parameters, part, supplier, lineitem, orders, customer, n1), weight) == sql_sum_6(db, parameters, part, supplier, lineitem, orders, customer, n1, weight),
{
    hide(sum);
    reveal(sql_sum_6);
    let expand = |n2: NationRow| all_nations_tuples_7(db, parameters, part, supplier, lineitem, orders, customer, n1, n2);
    let raw = |n2: NationRow| sum(expand(n2), weight);
    let nested = |n2: NationRow| sql_sum_7(db, parameters, part, supplier, lineitem, orders, customer, n1, n2, weight);
    assert forall|n2: NationRow| #[trigger] raw(n2) == nested(n2) by {
        lemma_sql_sum_7(db, parameters, part, supplier, lineitem, orders, customer, n1, n2, weight);
    }
    lemma_sum_flat_map(db.nation, expand, weight);
    lemma_sum_ext_all(db.nation, raw, nested);
}

#[verifier::opaque]
pub open spec fn sql_sum_5(db: Db, parameters: Params, part: PartRow, supplier: SupplierRow, lineitem: LineitemRow, orders: OrdersRow, customer: CustomerRow, weight: spec_fn(AllNationsTuple) -> int) -> int {
    sum(db.nation, |n1: NationRow| sql_sum_6(db, parameters, part, supplier, lineitem, orders, customer, n1, weight))
}

pub proof fn lemma_sql_sum_5(db: Db, parameters: Params, part: PartRow, supplier: SupplierRow, lineitem: LineitemRow, orders: OrdersRow, customer: CustomerRow, weight: spec_fn(AllNationsTuple) -> int)
    ensures sum(all_nations_tuples_5(db, parameters, part, supplier, lineitem, orders, customer), weight) == sql_sum_5(db, parameters, part, supplier, lineitem, orders, customer, weight),
{
    hide(sum);
    reveal(sql_sum_5);
    let expand = |n1: NationRow| all_nations_tuples_6(db, parameters, part, supplier, lineitem, orders, customer, n1);
    let raw = |n1: NationRow| sum(expand(n1), weight);
    let nested = |n1: NationRow| sql_sum_6(db, parameters, part, supplier, lineitem, orders, customer, n1, weight);
    assert forall|n1: NationRow| #[trigger] raw(n1) == nested(n1) by {
        lemma_sql_sum_6(db, parameters, part, supplier, lineitem, orders, customer, n1, weight);
    }
    lemma_sum_flat_map(db.nation, expand, weight);
    lemma_sum_ext_all(db.nation, raw, nested);
}

#[verifier::opaque]
pub open spec fn sql_sum_4(db: Db, parameters: Params, part: PartRow, supplier: SupplierRow, lineitem: LineitemRow, orders: OrdersRow, weight: spec_fn(AllNationsTuple) -> int) -> int {
    sum(db.customer, |customer: CustomerRow| sql_sum_5(db, parameters, part, supplier, lineitem, orders, customer, weight))
}

pub proof fn lemma_sql_sum_4(db: Db, parameters: Params, part: PartRow, supplier: SupplierRow, lineitem: LineitemRow, orders: OrdersRow, weight: spec_fn(AllNationsTuple) -> int)
    ensures sum(all_nations_tuples_4(db, parameters, part, supplier, lineitem, orders), weight) == sql_sum_4(db, parameters, part, supplier, lineitem, orders, weight),
{
    hide(sum);
    reveal(sql_sum_4);
    let expand = |customer: CustomerRow| all_nations_tuples_5(db, parameters, part, supplier, lineitem, orders, customer);
    let raw = |customer: CustomerRow| sum(expand(customer), weight);
    let nested = |customer: CustomerRow| sql_sum_5(db, parameters, part, supplier, lineitem, orders, customer, weight);
    assert forall|customer: CustomerRow| #[trigger] raw(customer) == nested(customer) by {
        lemma_sql_sum_5(db, parameters, part, supplier, lineitem, orders, customer, weight);
    }
    lemma_sum_flat_map(db.customer, expand, weight);
    lemma_sum_ext_all(db.customer, raw, nested);
}

#[verifier::opaque]
pub open spec fn sql_sum_3(db: Db, parameters: Params, part: PartRow, supplier: SupplierRow, lineitem: LineitemRow, weight: spec_fn(AllNationsTuple) -> int) -> int {
    sum(db.orders, |orders: OrdersRow| sql_sum_4(db, parameters, part, supplier, lineitem, orders, weight))
}

pub proof fn lemma_sql_sum_3(db: Db, parameters: Params, part: PartRow, supplier: SupplierRow, lineitem: LineitemRow, weight: spec_fn(AllNationsTuple) -> int)
    ensures sum(all_nations_tuples_3(db, parameters, part, supplier, lineitem), weight) == sql_sum_3(db, parameters, part, supplier, lineitem, weight),
{
    hide(sum);
    reveal(sql_sum_3);
    let expand = |orders: OrdersRow| all_nations_tuples_4(db, parameters, part, supplier, lineitem, orders);
    let raw = |orders: OrdersRow| sum(expand(orders), weight);
    let nested = |orders: OrdersRow| sql_sum_4(db, parameters, part, supplier, lineitem, orders, weight);
    assert forall|orders: OrdersRow| #[trigger] raw(orders) == nested(orders) by {
        lemma_sql_sum_4(db, parameters, part, supplier, lineitem, orders, weight);
    }
    lemma_sum_flat_map(db.orders, expand, weight);
    lemma_sum_ext_all(db.orders, raw, nested);
}

#[verifier::opaque]
pub open spec fn sql_sum_2(db: Db, parameters: Params, part: PartRow, supplier: SupplierRow, weight: spec_fn(AllNationsTuple) -> int) -> int {
    sum(db.lineitem, |lineitem: LineitemRow| sql_sum_3(db, parameters, part, supplier, lineitem, weight))
}

pub proof fn lemma_sql_sum_2(db: Db, parameters: Params, part: PartRow, supplier: SupplierRow, weight: spec_fn(AllNationsTuple) -> int)
    ensures sum(all_nations_tuples_2(db, parameters, part, supplier), weight) == sql_sum_2(db, parameters, part, supplier, weight),
{
    hide(sum);
    reveal(sql_sum_2);
    let expand = |lineitem: LineitemRow| all_nations_tuples_3(db, parameters, part, supplier, lineitem);
    let raw = |lineitem: LineitemRow| sum(expand(lineitem), weight);
    let nested = |lineitem: LineitemRow| sql_sum_3(db, parameters, part, supplier, lineitem, weight);
    assert forall|lineitem: LineitemRow| #[trigger] raw(lineitem) == nested(lineitem) by {
        lemma_sql_sum_3(db, parameters, part, supplier, lineitem, weight);
    }
    lemma_sum_flat_map(db.lineitem, expand, weight);
    lemma_sum_ext_all(db.lineitem, raw, nested);
}

#[verifier::opaque]
pub open spec fn sql_sum_1(db: Db, parameters: Params, part: PartRow, weight: spec_fn(AllNationsTuple) -> int) -> int {
    sum(db.supplier, |supplier: SupplierRow| sql_sum_2(db, parameters, part, supplier, weight))
}

pub proof fn lemma_sql_sum_1(db: Db, parameters: Params, part: PartRow, weight: spec_fn(AllNationsTuple) -> int)
    ensures sum(all_nations_tuples_1(db, parameters, part), weight) == sql_sum_1(db, parameters, part, weight),
{
    hide(sum);
    reveal(sql_sum_1);
    let expand = |supplier: SupplierRow| all_nations_tuples_2(db, parameters, part, supplier);
    let raw = |supplier: SupplierRow| sum(expand(supplier), weight);
    let nested = |supplier: SupplierRow| sql_sum_2(db, parameters, part, supplier, weight);
    assert forall|supplier: SupplierRow| #[trigger] raw(supplier) == nested(supplier) by {
        lemma_sql_sum_2(db, parameters, part, supplier, weight);
    }
    lemma_sum_flat_map(db.supplier, expand, weight);
    lemma_sum_ext_all(db.supplier, raw, nested);
}

#[verifier::opaque]
pub open spec fn sql_sum_0(db: Db, parameters: Params, weight: spec_fn(AllNationsTuple) -> int) -> int {
    sum(db.part, |part: PartRow| sql_sum_1(db, parameters, part, weight))
}

pub proof fn lemma_sql_sum_0(db: Db, parameters: Params, weight: spec_fn(AllNationsTuple) -> int)
    ensures sum(all_nations_tuples(db, parameters), weight) == sql_sum_0(db, parameters, weight),
{
    hide(sum);
    reveal(sql_sum_0);
    let expand = |part: PartRow| all_nations_tuples_1(db, parameters, part);
    let raw = |part: PartRow| sum(expand(part), weight);
    let nested = |part: PartRow| sql_sum_1(db, parameters, part, weight);
    assert forall|part: PartRow| #[trigger] raw(part) == nested(part) by {
        lemma_sql_sum_1(db, parameters, part, weight);
    }
    lemma_sum_flat_map(db.part, expand, weight);
    lemma_sum_ext_all(db.part, raw, nested);
}

pub open spec fn tuple_row(tuple: AllNationsTuple) -> AllNationsRow {
    AllNationsRow { o_year: civil_year(tuple.orders.o_orderdate),
        volume: tuple.lineitem.l_extendedprice * (100 - tuple.lineitem.l_discount), nation: tuple.n2.n_name }
}

pub open spec fn grouped_weight(tuple: AllNationsTuple, year: int, weight: spec_fn(Tuple) -> int) -> int {
    if civil_year(tuple.orders.o_orderdate) == year { weight(Tuple { all_nations: tuple_row(tuple) }) } else { 0 }
}

pub proof fn lemma_group_sum(db: Db, parameters: Params, year: int, weight: spec_fn(Tuple) -> int)
    ensures group_sum(db, parameters, year, weight) ==
        sql_sum_0(db, parameters, |tuple: AllNationsTuple| grouped_weight(tuple, year, weight)),
{
    hide(sum);
    hide(all_nations_tuples);
    let grouped = |tuple: Tuple| if tuple.all_nations.o_year == year { weight(tuple) } else { 0int };
    let expand = |row: AllNationsRow| tuples_1(db, parameters, row);
    let row_weight = |row: AllNationsRow| grouped(Tuple { all_nations: row });
    assert forall|row: AllNationsRow| #[trigger] sum(expand(row), grouped) == row_weight(row) by {
        lemma_sum_opt(true, Tuple { all_nations: row }, grouped);
    }
    lemma_sum_flat_map(rel_all_nations(db, parameters), expand, grouped);
    lemma_sum_ext_all(rel_all_nations(db, parameters), |row: AllNationsRow| sum(expand(row), grouped), row_weight);
    let projection = |tuple: AllNationsTuple| tuple_row(tuple);
    let raw_weight = |tuple: AllNationsTuple| grouped_weight(tuple, year, weight);
    assert(rel_all_nations(db, parameters) =~= all_nations_tuples(db, parameters).map_values(projection));
    assert(sum(tuples(db, parameters), grouped) == sum(rel_all_nations(db, parameters), row_weight));
    lemma_sum_map(all_nations_tuples(db, parameters), projection, row_weight, raw_weight);
    assert(sum(rel_all_nations(db, parameters), row_weight) == sum(all_nations_tuples(db, parameters), raw_weight));
    lemma_sql_sum_0(db, parameters, raw_weight);
}

pub proof fn lemma_group_weights(db: Db, parameters: Params, year: int, weight: spec_fn(Tuple) -> int, other: spec_fn(Tuple) -> int)
    requires forall|tuple: Tuple| #[trigger] weight(tuple) == other(tuple),
    ensures group_sum(db, parameters, year, weight) == group_sum(db, parameters, year, other),
{
    hide(tuples);
    hide(sum);
    lemma_sum_ext_all(tuples(db, parameters),
        |tuple: Tuple| if tuple.all_nations.o_year == year { weight(tuple) } else { 0int },
        |tuple: Tuple| if tuple.all_nations.o_year == year { other(tuple) } else { 0int });
}

pub proof fn lemma_count_sum(db: Db, parameters: Params, year: int)
    ensures group_count(db, parameters, year) == group_sum(db, parameters, year, |tuple: Tuple| 1int),
{
    hide(tuples);
    hide(sum);
    let one = |tuple: Tuple| 1int;
    let counted = |tuple: Tuple| if tuple.all_nations.o_year == year { 1int } else { 0 };
    let grouped = |tuple: Tuple| if tuple.all_nations.o_year == year { one(tuple) } else { 0 };
    assert(group_count(db, parameters, year) == sum(tuples(db, parameters), counted));
    assert(group_sum(db, parameters, year, one) == sum(tuples(db, parameters), grouped));
    lemma_sum_ext_all(tuples(db, parameters), counted, grouped);
}

pub proof fn lemma_total_lookup(data: Data, db: Db, region: i64, kind: i64, year: i64, volume: bool)
    requires aggregate_meaning(data, db), data.dictionary.regions.known(region), data.dictionary.types.known(kind),
    ensures data.totals.at((region, kind, 0), (year, 0, 0), volume) ==
        if volume { group_sum(db, Params { p1: Seq::empty(), p2: data.dictionary.regions.text(region), p3: data.dictionary.types.text(kind) },
            year as int, |tuple: Tuple| tuple.all_nations.volume) }
        else { group_count(db, Params { p1: Seq::empty(), p2: data.dictionary.regions.text(region), p3: data.dictionary.types.text(kind) }, year as int) },
{
    hide(group_count);
    hide(group_sum);
    reveal(aggregate_meaning);
}

pub proof fn lemma_numerator_lookup(data: Data, db: Db, region: i64, kind: i64, name: i64, year: i64, volume: bool)
    requires aggregate_meaning(data, db), data.dictionary.regions.known(region), data.dictionary.types.known(kind),
        data.dictionary.names.known(name),
    ensures data.numerators.at((region, kind, name), (year, 0, 0), volume) ==
        group_sum(db, Params { p1: data.dictionary.names.text(name), p2: data.dictionary.regions.text(region), p3: data.dictionary.types.text(kind) },
            year as int, numerator_weight(Params { p1: data.dictionary.names.text(name), p2: data.dictionary.regions.text(region), p3: data.dictionary.types.text(kind) }, volume)),
{
    hide(group_sum);
    reveal(aggregate_meaning);
}

pub proof fn lemma_read_total(data: Data, db: Db, parameters: Params, year: int, volume: bool)
    requires data.dictionary.valid(), dimension_meaning(data, db), aggregate_meaning(data, db),
    ensures crate::reading::read_total(data, parameters, year, volume) ==
        if volume { group_sum(db, parameters, year, |tuple: Tuple| tuple.all_nations.volume) }
        else { group_count(db, parameters, year) },
{
    hide(group_sum);
    hide(group_count);
    hide(crate::dictionary::Words::resolve_spec);
    hide(crate::reading::read_total);
    if data.dictionary.regions.resolve_spec(parameters.p2) is None || data.dictionary.types.resolve_spec(parameters.p3) is None
        || !(i64::MIN <= year <= i64::MAX) {
        lemma_missing_group(data, db, parameters, year, |tuple: Tuple| tuple.all_nations.volume);
        lemma_missing_group(data, db, parameters, year, |tuple: Tuple| 1int);
        lemma_count_sum(db, parameters, year);
    } else {
        let region = data.dictionary.regions.resolve_spec(parameters.p2).unwrap();
        let kind = data.dictionary.types.resolve_spec(parameters.p3).unwrap();
        crate::dictionary::lemma_resolved_words(data.dictionary.regions, parameters.p2, region);
        crate::dictionary::lemma_resolved_words(data.dictionary.types, parameters.p3, kind);
        let canonical = Params { p1: Seq::empty(), p2: data.dictionary.regions.text(region), p3: data.dictionary.types.text(kind) };
        lemma_group_parameters(db, parameters, canonical, year, |tuple: Tuple| tuple.all_nations.volume);
        lemma_group_parameters(db, parameters, canonical, year, |tuple: Tuple| 1int);
        lemma_total_lookup(data, db, region, kind, year as i64, volume);
        lemma_count_sum(db, parameters, year);
        lemma_count_sum(db, canonical, year);
    }
    reveal(crate::reading::read_total);
}

pub proof fn lemma_read_numerator(data: Data, db: Db, parameters: Params, year: int)
    requires data.dictionary.valid(), dimension_meaning(data, db), aggregate_meaning(data, db),
    ensures crate::reading::read_numerator(data, parameters, year) ==
        group_sum(db, parameters, year, |tuple: Tuple| if tuple.all_nations.nation == parameters.p1 { tuple.all_nations.volume } else { 0 }),
{
    hide(group_sum);
    hide(crate::dictionary::Words::resolve_spec);
    hide(crate::reading::read_numerator);
    let weight = |tuple: Tuple| if tuple.all_nations.nation == parameters.p1 { tuple.all_nations.volume } else { 0int };
    if data.dictionary.regions.resolve_spec(parameters.p2) is None || data.dictionary.types.resolve_spec(parameters.p3) is None
        || !(i64::MIN <= year <= i64::MAX) {
        lemma_missing_group(data, db, parameters, year, weight);
        assert(group_sum(db, parameters, year, weight) == 0);
    } else if data.dictionary.names.resolve_spec(parameters.p1) is None {
        lemma_missing_numerator(data, db, parameters, year);
        lemma_group_weights(db, parameters, year,
            |tuple: Tuple| if tuple.all_nations.nation == parameters.p1 { tuple.all_nations.volume } else { 0 }, weight);
        assert(group_sum(db, parameters, year, weight) == 0);
    } else {
        let name = data.dictionary.names.resolve_spec(parameters.p1).unwrap();
        let region = data.dictionary.regions.resolve_spec(parameters.p2).unwrap();
        let kind = data.dictionary.types.resolve_spec(parameters.p3).unwrap();
        crate::dictionary::lemma_resolved_words(data.dictionary.names, parameters.p1, name);
        crate::dictionary::lemma_resolved_words(data.dictionary.regions, parameters.p2, region);
        crate::dictionary::lemma_resolved_words(data.dictionary.types, parameters.p3, kind);
        let canonical = Params { p1: data.dictionary.names.text(name), p2: data.dictionary.regions.text(region), p3: data.dictionary.types.text(kind) };
        assert(canonical == parameters);
        let canonical_weight = numerator_weight(canonical, true);
        lemma_numerator_lookup(data, db, region, kind, name, year as i64, true);
        assert(data.numerators.at((region, kind, name), (year as i64, 0, 0), true) == group_sum(db, canonical, year, canonical_weight));
        lemma_group_weights(db, canonical, year, canonical_weight, weight);
        assert(data.numerators.at((region, kind, name), (year as i64, 0, 0), true) == group_sum(db, parameters, year, weight));
    }
    reveal(crate::reading::read_numerator);
    assert(crate::reading::read_numerator(data, parameters, year) == group_sum(db, parameters, year, weight));
    lemma_group_weights(db, parameters, year, weight,
        |tuple: Tuple| if tuple.all_nations.nation == parameters.p1 { tuple.all_nations.volume } else { 0 });
    assert(crate::reading::read_numerator(data, parameters, year) ==
        group_sum(db, parameters, year, |tuple: Tuple| if tuple.all_nations.nation == parameters.p1 { tuple.all_nations.volume } else { 0 }));
}

pub proof fn lemma_read_valid(data: Data, db: Db)
    requires data.dictionary.valid(), dimension_meaning(data, db), aggregate_meaning(data, db),
    ensures crate::reading::read_valid(data, db),
{
    hide(group_count);
    hide(group_sum);
    hide(crate::reading::read_total);
    hide(crate::reading::read_numerator);
    reveal(crate::reading::read_valid);
    assert forall|parameters: Params, year: int| #[trigger] crate::reading::read_total(data, parameters, year, false)
        == group_count(db, parameters, year) by {
        lemma_read_total(data, db, parameters, year, false);
    }
    assert forall|parameters: Params, year: int| #[trigger] crate::reading::read_total(data, parameters, year, true)
        == group_sum(db, parameters, year, |tuple: Tuple| tuple.all_nations.volume) by {
        lemma_read_total(data, db, parameters, year, true);
    }
    assert forall|parameters: Params, year: int| #[trigger] crate::reading::read_numerator(data, parameters, year)
        == group_sum(db, parameters, year, |tuple: Tuple| if tuple.all_nations.nation == parameters.p1 { tuple.all_nations.volume } else { 0 }) by {
        lemma_read_numerator(data, db, parameters, year);
    }
    assert forall|parameters: Params, year: int| #[trigger] group_count(db, parameters, year) > 0 implies year == 1995 || year == 1996 by {
        if year != 1995 && year != 1996 {
            lemma_missing_group(data, db, parameters, year, |tuple: Tuple| 1int);
            lemma_count_sum(db, parameters, year);
        }
    }
}

}
