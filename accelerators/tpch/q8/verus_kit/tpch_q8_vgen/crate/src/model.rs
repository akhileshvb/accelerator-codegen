use vstd::prelude::*;
use verus_common::sums::*;
use verus_common::sqlfns::*;
use crate::gen_spec::*;
use crate::dictionary::*;
use crate::table::*;


verus! {
pub proof fn lemma_shape_fields(state: Data)
    requires
        forall|key: Major, index: int| 0 <= index < state.orders.rows(key).len() ==> crate::dimensions::dimension_support(key, minor_of(#[trigger] state.orders.rows(key)[index]), state.dictionary, 3) && state.orders.rows(key)[index].volume@ == 0,
        forall|key: Major, index: int| 0 <= index < state.nations.rows(key).len() ==> crate::dimensions::dimension_support(key, minor_of(#[trigger] state.nations.rows(key)[index]), state.dictionary, 4) && state.nations.rows(key)[index].volume@ == 0,
        crate::aggregate_bridge::aggregate_ready(state),
        forall|key: Major, index: int| 0 <= index < state.totals.rows(key).len() ==> crate::aggregate_cells::output_support(key, minor_of(#[trigger] state.totals.rows(key)[index]), state.dictionary, false),
        forall|key: Major, index: int| 0 <= index < state.numerators.rows(key).len() ==> crate::aggregate_cells::output_support(key, minor_of(#[trigger] state.numerators.rows(key)[index]), state.dictionary, true),
    ensures runtime_orders(state), runtime_nations(state), runtime_base(state), runtime_totals(state), runtime_numerators(state),
{
    reveal(runtime_orders);
    reveal(runtime_nations);
    reveal(runtime_base);
    reveal(runtime_totals);
    reveal(runtime_numerators);
    assert forall|key: Major, index: int| 0 <= index < state.orders.rows(key).len() implies
        order_cell_shape(key, #[trigger] state.orders.rows(key)[index]) by {
        assert(crate::dimensions::dimension_support(key, minor_of(state.orders.rows(key)[index]), state.dictionary, 3));
        assert(state.orders.rows(key)[index].volume@ == 0);
    }
    assert forall|key: Major, index: int| 0 <= index < state.nations.rows(key).len() implies
        runtime_nation_cell_shape(key, #[trigger] state.nations.rows(key)[index]) by {
        assert(crate::dimensions::dimension_support(key, minor_of(state.nations.rows(key)[index]), state.dictionary, 4));
        assert(state.nations.rows(key)[index].volume@ == 0);
    }
    assert(runtime_orders(state));
    assert(runtime_nations(state));
    assert forall|key: Major, index: int| 0 <= index < state.base.rows(key).len() implies
        runtime_base_cell_shape(key, #[trigger] state.base.rows(key)[index]) by {
        assert(crate::base_proofs::base_support(key, minor_of(state.base.rows(key)[index]), state.dictionary));
    }
    assert forall|key: Major, index: int| 0 <= index < state.totals.rows(key).len() implies
        runtime_total_cell_shape(key, #[trigger] state.totals.rows(key)[index], state.dictionary) by {
        assert(crate::aggregate_cells::output_support(key, minor_of(state.totals.rows(key)[index]), state.dictionary, false));
    }
    assert forall|key: Major, index: int| 0 <= index < state.numerators.rows(key).len() implies
        runtime_numerator_cell_shape(key, #[trigger] state.numerators.rows(key)[index], state.dictionary) by {
        assert(crate::aggregate_cells::output_support(key, minor_of(state.numerators.rows(key)[index]), state.dictionary, true));
    }
}
pub proof fn lemma_runtime_from_fields(state: Data)
    requires runtime_storage(state), runtime_positive(state), runtime_scalar(state), runtime_text(state), runtime_orders(state), runtime_nations(state), runtime_base(state), runtime_totals(state), runtime_numerators(state),
    ensures runtime_valid(state),
{
    reveal(runtime_fields);
    lemma_runtime_complete(state);
}

pub struct Data {
    pub dictionary: Dictionary,
    pub parts: Table,
    pub suppliers: Table,
    pub customers: Table,
    pub orders: Table,
    pub nations: Table,
    pub regions: Table,
    pub orders_by_customer: Table,
    pub nations_by_region: Table,
    pub base: Table,
    pub base_by_customer: Table,
    pub base_by_supplier: Table,
    pub customer_routes: Table,
    pub supplier_routes: Table,
    pub totals: Table,
    pub numerators: Table,
}


pub open spec fn words_match(words: Words, id: i64, text: Seq<u8>) -> bool {
    words.known(id) && words.text(id) == text
}

pub open spec fn words_present(words: Words, text: Seq<u8>) -> bool {
    exists|id: i64| words.known(id) && #[trigger] words.text(id) == text
}

/// Id `id` of column `c` (types, names or regions) is the text.
pub open spec fn text_matches(dictionary: Dictionary, c: int, id: i64, text: Seq<u8>) -> bool {
    words_match(dictionary.col(c), id, text)
}

pub open spec fn text_present(dictionary: Dictionary, c: int, text: Seq<u8>) -> bool {
    words_present(dictionary.col(c), text)
}

pub open spec fn year_of(date: int) -> int {
    if 9131 <= date <= 9495 { 1995 } else if 9496 <= date <= 9861 { 1996 } else { 0 }
}

pub open spec fn order_represented(row: OrdersRow) -> bool {
    !(9131 <= row.o_orderdate <= 9861)
        || (i64::MIN <= row.o_orderkey <= i64::MAX && i64::MIN <= row.o_custkey <= i64::MAX)
}

pub open spec fn order_domain(db: Db) -> bool {
    forall|index: int| 0 <= index < db.orders.len() ==> order_represented(#[trigger] db.orders[index])
}

pub open spec fn nation_represented(row: NationRow) -> bool {
    i64::MIN <= row.n_nationkey <= i64::MAX && i64::MIN <= row.n_regionkey <= i64::MAX
}

pub open spec fn nation_domain(db: Db) -> bool {
    forall|index: int| 0 <= index < db.nation.len() ==> nation_represented(#[trigger] db.nation[index])
}

pub open spec fn line_represented(line: LineitemRow) -> bool {
    i64::MIN <= line.l_orderkey <= i64::MAX && i64::MIN <= line.l_partkey <= i64::MAX
        && i64::MIN <= line.l_suppkey <= i64::MAX && i64::MIN <= line.l_extendedprice <= i64::MAX
        && i64::MIN <= line.l_discount <= i64::MAX
}

pub open spec fn line_in_domain(orders_db: Db, line: LineitemRow) -> bool {
    !crate::gen_obligations::scan_lines_exists_1(orders_db, line) || line_represented(line)
}

pub open spec fn line_domain_at(lines_db: Db, orders_db: Db) -> bool {
    forall|index: int| 0 <= index < lines_db.lineitem.len() ==> line_in_domain(orders_db, #[trigger] lines_db.lineitem[index])
}

pub open spec fn line_domain(db: Db) -> bool {
    line_domain_at(db, db)
}

pub open spec fn positive(table: Table) -> bool {
    forall|key: Major, index: int| 0 <= index < table.rows(key).len() ==>
        (#[trigger] table.rows(key)[index]).count@ > 0
}

pub open spec fn scalar_cell(key: Major, cell: Cell) -> bool {
    key.1 == 0 && key.2 == 0 && cell.second == 0 && cell.third == 0 && cell.volume@ == 0
}

pub open spec fn scalar(table: Table) -> bool {
    forall|key: Major, index: int| 0 <= index < table.rows(key).len() ==>
        scalar_cell(key, #[trigger] table.rows(key)[index])
}

pub open spec fn text_domain(table: Table, dictionary: Dictionary, c: int) -> bool {
    forall|key: Major, index: int| 0 <= index < table.rows(key).len() ==>
        dictionary.col(c).known((#[trigger] table.rows(key)[index]).first)
}

#[verifier::opaque]
pub open spec fn line_weight(state: Data, db: Db, line: LineitemRow, customer_nation: i64, supplier_nation: i64, kind: i64, year: i64) -> int {
    state.parts.at((line.l_partkey as i64, 0, 0), (kind, 0, 0), false)
    * state.suppliers.at((line.l_suppkey as i64, 0, 0), (supplier_nation, 0, 0), false)
    * sum(db.orders, |order: OrdersRow|
        if order.o_orderkey == line.l_orderkey && year_of(order.o_orderdate) == year as int && year != 0 {
            state.customers.at((order.o_custkey as i64, 0, 0), (customer_nation, 0, 0), false)
        } else { 0 })
}

pub open spec fn measure(cell: Cell, volume: bool) -> int {
    if volume { cell.volume@ } else { cell.count@ }
}

pub open spec fn line_measure(line: LineitemRow, volume: bool) -> int {
    if volume { line.l_extendedprice * (100 - line.l_discount) } else { 1 }
}

#[verifier::opaque]
pub open spec fn projected_line_weight(parts: Table, suppliers: Table, customers: Table, orders: Table, line: LineitemRow, key: Major, minor: Major) -> int {
    if key.2 != 0 || minor.2 != 0 { 0 }
    else {
        parts.at((line.l_partkey as i64, 0, 0), (minor.0, 0, 0), false)
        * suppliers.at((line.l_suppkey as i64, 0, 0), (key.1, 0, 0), false)
        * sum(orders.rows((line.l_orderkey as i64, 0, 0)), |order: Cell|
            if order.second == minor.1 {
                order.count@ * customers.at((order.first, 0, 0), (key.0, 0, 0), false)
            } else { 0 })
    }
}

#[verifier::opaque]
pub open spec fn valid(state: Data, db: Db) -> bool {
    runtime_valid(state) && meaning(state, db) && crate::reading::read_valid(state, db)
}

pub proof fn lemma_valid(state: Data, db: Db)
    requires runtime_valid(state), meaning(state, db), crate::reading::read_valid(state, db),
    ensures valid(state, db),
{
    reveal(valid);
}

/// What a valid state says: its runtime shape, the meaning of each table, and its answers.
pub proof fn lemma_valid_parts(state: Data, db: Db)
    requires valid(state, db),
    ensures runtime_valid(state), meaning(state, db), crate::reading::read_valid(state, db),
{
    reveal(valid);
}

/// The meaning of a state is the meaning of each of its tables.
pub proof fn lemma_meaning_parts(state: Data, db: Db)
    requires meaning(state, db),
    ensures dimension_meaning(state, db), line_domain(db), base_meaning(state, db), index_meaning(state),
        route_meaning(state, db), aggregate_meaning(state, db), routed_meaning(state),
{
    reveal(meaning);
}

pub proof fn lemma_meaning_from_parts(state: Data, db: Db)
    requires dimension_meaning(state, db), line_domain(db), base_meaning(state, db), index_meaning(state),
        route_meaning(state, db), aggregate_meaning(state, db), routed_meaning(state),
    ensures meaning(state, db),
{
    reveal(meaning);
}

#[verifier::opaque]
pub open spec fn runtime_valid(state: Data) -> bool {
    runtime_fields(state)
}

pub proof fn lemma_runtime_access(state: Data)
    requires runtime_valid(state),
    ensures runtime_fields(state),
{
    reveal(runtime_valid);
}

pub proof fn lemma_runtime_complete(state: Data)
    requires runtime_fields(state),
    ensures runtime_valid(state),
{
    reveal(runtime_valid);
}

pub open spec fn runtime_storage(state: Data) -> bool {
    &&& state.dictionary.valid()
    &&& state.parts.valid() && state.suppliers.valid() && state.customers.valid() && state.orders.valid()
    &&& state.nations.valid() && state.regions.valid() && state.orders_by_customer.valid() && state.nations_by_region.valid()
    &&& state.base.valid() && state.base_by_customer.valid() && state.base_by_supplier.valid()
    &&& state.customer_routes.valid() && state.supplier_routes.valid() && state.totals.valid() && state.numerators.valid()
}

pub open spec fn runtime_positive(state: Data) -> bool {
    &&& positive(state.parts) && positive(state.suppliers) && positive(state.customers) && positive(state.orders)
    &&& positive(state.nations) && positive(state.regions) && positive(state.base)
    &&& positive(state.orders_by_customer) && positive(state.nations_by_region)
    &&& positive(state.base_by_customer) && positive(state.base_by_supplier)
    &&& positive(state.customer_routes) && positive(state.supplier_routes) && positive(state.totals) && positive(state.numerators)
}

pub open spec fn runtime_scalar(state: Data) -> bool {
    &&& scalar(state.parts) && scalar(state.suppliers) && scalar(state.customers) && scalar(state.regions)
    &&& scalar(state.orders_by_customer) && scalar(state.nations_by_region)
    &&& scalar(state.base_by_customer) && scalar(state.base_by_supplier)
    &&& scalar(state.customer_routes) && scalar(state.supplier_routes)
}

pub open spec fn runtime_text(state: Data) -> bool {
    &&& text_domain(state.parts, state.dictionary, 0) && text_domain(state.nations, state.dictionary, 1) && text_domain(state.regions, state.dictionary, 2)
    &&& text_domain(state.customer_routes, state.dictionary, 2) && text_domain(state.supplier_routes, state.dictionary, 1) && text_domain(state.base, state.dictionary, 0)
}

pub open spec fn order_cell_shape(key: Major, cell: Cell) -> bool {
    key.1 == 0 && key.2 == 0 && cell.third == 0 && cell.volume@ == 0
        && (cell.second == 1995 || cell.second == 1996)
}

pub open spec fn runtime_nation_cell_shape(key: Major, cell: Cell) -> bool {
    key.1 == 0 && key.2 == 0 && cell.third == 0 && cell.volume@ == 0
}

pub open spec fn runtime_orders(state: Data) -> bool {
    forall|key: Major, index: int| 0 <= index < state.orders.rows(key).len() ==>
        order_cell_shape(key, #[trigger] state.orders.rows(key)[index])
}

pub open spec fn runtime_nations(state: Data) -> bool {
    forall|key: Major, index: int| 0 <= index < state.nations.rows(key).len() ==>
        runtime_nation_cell_shape(key, #[trigger] state.nations.rows(key)[index])
}

pub open spec fn runtime_base(state: Data) -> bool {
    &&& forall|key: Major, index: int| 0 <= index < state.base.rows(key).len() ==>
        runtime_base_cell_shape(key, #[trigger] state.base.rows(key)[index])
}

pub open spec fn runtime_totals(state: Data) -> bool {
    &&& forall|key: Major, index: int| 0 <= index < state.totals.rows(key).len() ==>
        runtime_total_cell_shape(key, #[trigger] state.totals.rows(key)[index], state.dictionary)
}

pub open spec fn runtime_numerators(state: Data) -> bool {
    &&& forall|key: Major, index: int| 0 <= index < state.numerators.rows(key).len() ==>
        runtime_numerator_cell_shape(key, #[trigger] state.numerators.rows(key)[index], state.dictionary)
}

pub open spec fn runtime_base_cell_shape(key: Major, cell: Cell) -> bool {
    key.2 == 0 && cell.third == 0 && (cell.second == 1995 || cell.second == 1996)
}

pub open spec fn runtime_total_cell_shape(key: Major, cell: Cell, dictionary: Dictionary) -> bool {
    dictionary.regions.known(key.0) && dictionary.types.known(key.1) && key.2 == 0
        && (cell.first == 1995 || cell.first == 1996) && cell.second == 0 && cell.third == 0
}

pub open spec fn runtime_numerator_cell_shape(key: Major, cell: Cell, dictionary: Dictionary) -> bool {
    dictionary.regions.known(key.0) && dictionary.types.known(key.1) && dictionary.names.known(key.2)
        && (cell.first == 1995 || cell.first == 1996) && cell.second == 0 && cell.third == 0
}

pub open spec fn runtime_fields(state: Data) -> bool {
    runtime_storage(state) && runtime_positive(state) && runtime_scalar(state) && runtime_text(state) && runtime_orders(state) && runtime_nations(state) && runtime_base(state) && runtime_totals(state) && runtime_numerators(state)
}

#[verifier::opaque]
pub open spec fn dimension_meaning(state: Data, db: Db) -> bool {
    let dictionary = state.dictionary;
    &&& order_domain(db)
    &&& nation_domain(db)
    &&& forall|index: int| 0 <= index < db.part.len() ==>
        text_present(dictionary, 0, (#[trigger] db.part[index]).p_type)
    &&& forall|index: int| 0 <= index < db.nation.len() ==>
        text_present(dictionary, 1, (#[trigger] db.nation[index]).n_name)
    &&& forall|index: int| 0 <= index < db.region.len() ==>
        text_present(dictionary, 2, (#[trigger] db.region[index]).r_name)
    &&& forall|key: i64, kind: i64| #[trigger] state.parts.at((key, 0, 0), (kind, 0, 0), false) ==
        sum(db.part, |row: PartRow| if row.p_partkey == key as int && text_matches(dictionary, 0, kind, row.p_type) { 1 } else { 0 })
    &&& forall|key: i64, nation: i64| #[trigger] state.suppliers.at((key, 0, 0), (nation, 0, 0), false) ==
        sum(db.supplier, |row: SupplierRow| if row.s_suppkey == key as int && row.s_nationkey == nation as int { 1 } else { 0 })
    &&& forall|key: i64, nation: i64| #[trigger] state.customers.at((key, 0, 0), (nation, 0, 0), false) ==
        sum(db.customer, |row: CustomerRow| if row.c_custkey == key as int && row.c_nationkey == nation as int { 1 } else { 0 })
    &&& forall|key: i64, customer: i64, year: i64| #[trigger] state.orders.at((key, 0, 0), (customer, year, 0), false) ==
        sum(db.orders, |row: OrdersRow| if row.o_orderkey == key as int && row.o_custkey == customer as int
            && year != 0 && year_of(row.o_orderdate) == year as int { 1 } else { 0 })
    &&& forall|key: i64, name: i64, region: i64| #[trigger] state.nations.at((key, 0, 0), (name, region, 0), false) ==
        sum(db.nation, |row: NationRow| if row.n_nationkey == key as int && row.n_regionkey == region as int
            && text_matches(dictionary, 1, name, row.n_name) { 1 } else { 0 })
    &&& forall|key: i64, name: i64| #[trigger] state.regions.at((key, 0, 0), (name, 0, 0), false) ==
        sum(db.region, |row: RegionRow| if row.r_regionkey == key as int && text_matches(dictionary, 2, name, row.r_name) { 1 } else { 0 })
    &&& forall|customer: i64, order: i64| #[trigger] state.orders_by_customer.at((customer, 0, 0), (order, 0, 0), false) ==
        sum(db.orders, |row: OrdersRow| if row.o_orderkey == order as int && row.o_custkey == customer as int
            && year_of(row.o_orderdate) != 0 { 1 } else { 0 })
    &&& forall|region: i64, nation: i64| #[trigger] state.nations_by_region.at((region, 0, 0), (nation, 0, 0), false) ==
        sum(db.nation, |row: NationRow| if row.n_regionkey == region as int && row.n_nationkey == nation as int { 1 } else { 0 })
}

#[verifier::opaque]
pub open spec fn base_meaning(state: Data, db: Db) -> bool {
    forall|customer: i64, supplier: i64, kind: i64, year: i64, volume: bool|
        #[trigger] state.base.at((customer, supplier, 0), (kind, year, 0), volume) ==
        sum(db.lineitem, |line: LineitemRow| line_weight(state, db, line, customer, supplier, kind, year)
            * line_measure(line, volume))
}

#[verifier::opaque]
pub open spec fn meaning(state: Data, db: Db) -> bool {
    &&& dimension_meaning(state, db)
    &&& line_domain(db)
    &&& base_meaning(state, db)
    &&& index_meaning(state)
    &&& route_meaning(state, db)
    &&& aggregate_meaning(state, db)
    &&& routed_meaning(state)
}

#[verifier::opaque]
pub open spec fn routed_meaning(state: Data) -> bool {
    &&& forall|key: Major, minor: Major, volume: bool| #[trigger] state.totals.at(key, minor, volume) ==
        crate::routing::routed_effect(state.base.keys_view(), state.base, state.customer_routes, state.supplier_routes, 1, key, minor, false, volume)
    &&& forall|key: Major, minor: Major, volume: bool| #[trigger] state.numerators.at(key, minor, volume) ==
        crate::routing::routed_effect(state.base.keys_view(), state.base, state.customer_routes, state.supplier_routes, 1, key, minor, true, volume)
}

#[verifier::opaque]
pub open spec fn index_meaning(state: Data) -> bool {
    &&& forall|customer: i64, supplier: i64| #[trigger] state.base_by_customer.at((customer, 0, 0), (supplier, 0, 0), false) ==
        sum(state.base.rows((customer, supplier, 0)), |cell: Cell| cell.count@)
    &&& forall|supplier: i64, customer: i64| #[trigger] state.base_by_supplier.at((supplier, 0, 0), (customer, 0, 0), false) ==
        sum(state.base.rows((customer, supplier, 0)), |cell: Cell| cell.count@)
}

#[verifier::opaque]
pub open spec fn route_meaning(state: Data, db: Db) -> bool {
    let dictionary = state.dictionary;
    &&& forall|nation: i64, region: i64| #[trigger] state.customer_routes.at((nation, 0, 0), (region, 0, 0), false) ==
        sum(db.nation, |row: NationRow| if row.n_nationkey == nation as int {
            sum(db.region, |other: RegionRow| if row.n_regionkey == other.r_regionkey
                && text_matches(dictionary, 2, region, other.r_name) { 1 } else { 0 })
        } else { 0 })
    &&& forall|nation: i64, name: i64| #[trigger] state.supplier_routes.at((nation, 0, 0), (name, 0, 0), false) ==
        sum(db.nation, |row: NationRow| if row.n_nationkey == nation as int && text_matches(dictionary, 1, name, row.n_name) { 1 } else { 0 })
}

#[verifier::opaque]
pub open spec fn aggregate_meaning(state: Data, db: Db) -> bool {
    let dictionary = state.dictionary;
    &&& forall|region: i64, kind: i64, year: i64, volume: bool|
        #[trigger] state.totals.at((region, kind, 0), (year, 0, 0), volume) ==
        if dictionary.regions.known(region) && dictionary.types.known(kind) {
            let parameters = Params { p1: Seq::empty(), p2: dictionary.regions.text(region), p3: dictionary.types.text(kind) };
            if volume { group_sum(db, parameters, year as int, |tuple: Tuple| tuple.all_nations.volume) }
            else { group_count(db, parameters, year as int) }
        } else { 0 }
    &&& forall|region: i64, kind: i64, name: i64, year: i64, volume: bool|
        #[trigger] state.numerators.at((region, kind, name), (year, 0, 0), volume) ==
        if dictionary.regions.known(region) && dictionary.types.known(kind) && dictionary.names.known(name) {
            let parameters = Params { p1: dictionary.names.text(name), p2: dictionary.regions.text(region), p3: dictionary.types.text(kind) };
            group_sum(db, parameters, year as int, numerator_weight(parameters, volume))
        } else { 0 }
}

pub open spec fn numerator_weight(parameters: Params, volume: bool) -> spec_fn(Tuple) -> int {
    |tuple: Tuple| if tuple.all_nations.nation == parameters.p1 {
        if volume { tuple.all_nations.volume } else { 1int }
    } else { 0int }
}

}
