use vstd::prelude::*;
use verus_common::sums::*;
use verus_common::rel::*;
use verus_common::sqlfns::*;
use crate::gen_spec::*;
use crate::gen_obligations::params;
use crate::meaning::*;
use crate::construction::*;

verus! {

pub open spec fn tuple_weight(name: Seq<u8>, year: int, field: int) -> spec_fn(Tuple) -> int {
    |row: Tuple| if row.profit.nation == name && row.profit.o_year == year {
        if field == 0 { 1int } else { row.profit.amount }
    } else { 0int }
}

pub proof fn lemma_group_amount(db: Db, name: Seq<u8>, year: int, amount: spec_fn(Tuple) -> int)
    requires forall|row: Tuple| #[trigger] amount(row) == row.profit.amount,
    ensures group_sum(db, params(), name, year, amount) == sum(tuples(db, params()), tuple_weight(name, year, 1)),
{
    lemma_sum_ext_all(tuples(db, params()),
        |row: Tuple| if row.profit.nation == name && row.profit.o_year == year { amount(row) } else { 0int },
        tuple_weight(name, year, 1));
}

pub open spec fn profit_weight(name: Seq<u8>, year: int, field: int) -> spec_fn(ProfitTuple) -> int {
    |row: ProfitTuple| if row.nation.n_name == name && civil_year(row.orders.o_orderdate) == year {
        if field == 0 { 1int } else { line_weight(row.lineitem, 1) - row.partsupp.ps_supplycost * row.lineitem.l_quantity }
    } else { 0int }
}

pub open spec fn part_join(db: Db, key: int, supplier: int, year: int, field: int) -> int {
    if field == 0 { pair_count(db, key, supplier) * moments(db, key, supplier, year, 0) }
    else { pair_count(db, key, supplier) * moments(db, key, supplier, year, 1) - pair_cost(db, key, supplier) * moments(db, key, supplier, year, 2) }
}

pub open spec fn line_join(db: Db, key: int, supplier: int, row: LineitemRow, field: int) -> int {
    if row.l_partkey == key && row.l_suppkey == supplier {
        if field == 0 { pair_count(db, key, supplier) }
        else { pair_count(db, key, supplier) * line_weight(row, 1) - pair_cost(db, key, supplier) * row.l_quantity }
    } else { 0int }
}

pub proof fn lemma_pair_linear(db: Db, key: int, supplier: int, revenue: int, quantity: int)
    ensures sum(db.partsupp, |row: PartsuppRow| if row.ps_partkey == key && row.ps_suppkey == supplier {
        revenue - row.ps_supplycost * quantity
    } else { 0int }) == pair_count(db, key, supplier) * revenue - pair_cost(db, key, supplier) * quantity,
{
    let count = |row: PartsuppRow| if row.ps_partkey == key && row.ps_suppkey == supplier { 1int } else { 0int };
    let cost = |row: PartsuppRow| if row.ps_partkey == key && row.ps_suppkey == supplier { row.ps_supplycost } else { 0int };
    let revenues = |row: PartsuppRow| revenue * count(row);
    let costs = |row: PartsuppRow| quantity * cost(row);
    let combined = |row: PartsuppRow| if row.ps_partkey == key && row.ps_suppkey == supplier { revenue - row.ps_supplycost * quantity } else { 0int };
    assert forall|row: PartsuppRow| #[trigger] combined(row) == revenues(row) - costs(row) by {
        assert(row.ps_supplycost * quantity == quantity * row.ps_supplycost) by (nonlinear_arith);
    }
    lemma_sum_scale(db.partsupp, count, revenues, revenue);
    lemma_sum_scale(db.partsupp, cost, costs, quantity);
    lemma_sum_sub(db.partsupp, revenues, costs, combined);
    assert(revenue * pair_count(db, key, supplier) == pair_count(db, key, supplier) * revenue) by (nonlinear_arith);
    assert(quantity * pair_cost(db, key, supplier) == pair_cost(db, key, supplier) * quantity) by (nonlinear_arith);
}

pub proof fn lemma_line_join(db: Db, key: int, supplier: int, year: int, field: int)
    requires 0 <= field <= 1,
    ensures sum(db.lineitem, |row: LineitemRow| dates(db, row.l_orderkey, year) * line_join(db, key, supplier, row, field)) == part_join(db, key, supplier, year, field),
{
    let count = |row: LineitemRow| if row.l_partkey == key && row.l_suppkey == supplier { dates(db, row.l_orderkey, year) * line_weight(row, 0) } else { 0int };
    let revenue = |row: LineitemRow| if row.l_partkey == key && row.l_suppkey == supplier { dates(db, row.l_orderkey, year) * line_weight(row, 1) } else { 0int };
    let quantity = |row: LineitemRow| if row.l_partkey == key && row.l_suppkey == supplier { dates(db, row.l_orderkey, year) * line_weight(row, 2) } else { 0int };
    let copies = pair_count(db, key, supplier);
    let cost = pair_cost(db, key, supplier);
    let combined = |row: LineitemRow| dates(db, row.l_orderkey, year) * line_join(db, key, supplier, row, field);
    if field == 0 {
        assert forall|row: LineitemRow| #[trigger] combined(row) == copies * count(row) by {
            if row.l_partkey == key && row.l_suppkey == supplier {
                assert(dates(db, row.l_orderkey, year) * copies == copies * dates(db, row.l_orderkey, year)) by (nonlinear_arith);
            } else { assert(dates(db, row.l_orderkey, year) * 0int == 0) by (nonlinear_arith); }
        }
        lemma_sum_scale(db.lineitem, count, combined, copies);
    } else {
        let revenues = |row: LineitemRow| copies * revenue(row);
        let costs = |row: LineitemRow| cost * quantity(row);
        assert forall|row: LineitemRow| #[trigger] combined(row) == revenues(row) - costs(row) by {
            if row.l_partkey == key && row.l_suppkey == supplier {
                let dates = dates(db, row.l_orderkey, year);
                let price = line_weight(row, 1);
                let units = line_weight(row, 2);
                assert(dates * (copies * price - cost * units) == copies * (dates * price) - cost * (dates * units)) by (nonlinear_arith);
            } else { assert(dates(db, row.l_orderkey, year) * 0int == 0) by (nonlinear_arith); }
        }
        lemma_sum_scale(db.lineitem, revenue, revenues, copies);
        lemma_sum_scale(db.lineitem, quantity, costs, cost);
        lemma_sum_sub(db.lineitem, revenues, costs, combined);
    }
}

pub open spec fn base_match(part: PartRow, supplier: SupplierRow, line: LineitemRow) -> bool {
    part.p_partkey == line.l_partkey && supplier.s_suppkey == line.l_suppkey
        && like_match(part.p_name, seq![b'%', b'g', b'r', b'e', b'e', b'n', b'%'])
}

pub open spec fn order_value(db: Db, part: PartRow, supplier: SupplierRow, line: LineitemRow, pair: PartsuppRow, order: OrdersRow, name: Seq<u8>, year: int, field: int) -> int {
    if base_match(part, supplier, line) && pair.ps_partkey == line.l_partkey && pair.ps_suppkey == line.l_suppkey
        && order.o_orderkey == line.l_orderkey && civil_year(order.o_orderdate) == year {
        nation_count(db, supplier.s_nationkey, name) * if field == 0 { 1int } else { line_weight(line, 1) - pair.ps_supplycost * line.l_quantity }
    } else { 0int }
}

pub open spec fn pair_value(db: Db, part: PartRow, supplier: SupplierRow, line: LineitemRow, pair: PartsuppRow, name: Seq<u8>, year: int, field: int) -> int {
    if base_match(part, supplier, line) && pair.ps_partkey == line.l_partkey && pair.ps_suppkey == line.l_suppkey {
        (nation_count(db, supplier.s_nationkey, name) * dates(db, line.l_orderkey, year))
            * if field == 0 { 1int } else { line_weight(line, 1) - pair.ps_supplycost * line.l_quantity }
    } else { 0int }
}

pub open spec fn line_value(db: Db, part: PartRow, supplier: SupplierRow, line: LineitemRow, name: Seq<u8>, year: int, field: int) -> int {
    if base_match(part, supplier, line) {
        (nation_count(db, supplier.s_nationkey, name) * dates(db, line.l_orderkey, year)) * line_join(db, part.p_partkey, supplier.s_suppkey, line, field)
    } else { 0int }
}

pub open spec fn supplier_value(db: Db, part: PartRow, supplier: SupplierRow, name: Seq<u8>, year: int, field: int) -> int {
    if like_match(part.p_name, seq![b'%', b'g', b'r', b'e', b'e', b'n', b'%']) {
        nation_count(db, supplier.s_nationkey, name) * part_join(db, part.p_partkey, supplier.s_suppkey, year, field)
    } else { 0int }
}

pub proof fn lemma_profit5(db: Db, part: PartRow, supplier: SupplierRow, line: LineitemRow, pair: PartsuppRow, order: OrdersRow, name: Seq<u8>, year: int, field: int)
    requires 0 <= field <= 1,
    ensures sum(profit_tuples_5(db, params(), part, supplier, line, pair, order), profit_weight(name, year, field)) == order_value(db, part, supplier, line, pair, order, name, year, field),
{
    let branch = |nation: NationRow| profit_tuples_6(db, params(), part, supplier, line, pair, order, nation);
    let nested = |nation: NationRow| if profit_where_clause(db, params(), part, supplier, line, pair, order, nation) {
        profit_weight(name, year, field)(ProfitTuple { part, supplier, lineitem: line, partsupp: pair, orders: order, nation })
    } else { 0int };
    assert forall|nation: NationRow| #[trigger] nested(nation) == sum(branch(nation), profit_weight(name, year, field)) by {
        lemma_sum_opt(profit_where_clause(db, params(), part, supplier, line, pair, order, nation),
            ProfitTuple { part, supplier, lineitem: line, partsupp: pair, orders: order, nation }, profit_weight(name, year, field));
    }
    verus_prooflib::lemma_flat_map_weight(db.nation, branch, profit_weight(name, year, field), nested);
    if base_match(part, supplier, line) && pair.ps_partkey == line.l_partkey && pair.ps_suppkey == line.l_suppkey
        && order.o_orderkey == line.l_orderkey && civil_year(order.o_orderdate) == year {
        let count = |nation: NationRow| if nation.n_nationkey == supplier.s_nationkey && nation.n_name == name { 1int } else { 0int };
        let amount = if field == 0 { 1int } else { line_weight(line, 1) - pair.ps_supplycost * line.l_quantity };
        lemma_sum_scale(db.nation, count, nested, amount);
        assert(amount * nation_count(db, supplier.s_nationkey, name) == nation_count(db, supplier.s_nationkey, name) * amount) by (nonlinear_arith);
    } else { lemma_sum_zero(db.nation, nested); }
}

pub proof fn lemma_profit4(db: Db, part: PartRow, supplier: SupplierRow, line: LineitemRow, pair: PartsuppRow, name: Seq<u8>, year: int, field: int)
    requires 0 <= field <= 1,
    ensures sum(profit_tuples_4(db, params(), part, supplier, line, pair), profit_weight(name, year, field)) == pair_value(db, part, supplier, line, pair, name, year, field),
{
    let branch = |order: OrdersRow| profit_tuples_5(db, params(), part, supplier, line, pair, order);
    let nested = |order: OrdersRow| order_value(db, part, supplier, line, pair, order, name, year, field);
    assert forall|order: OrdersRow| #[trigger] nested(order) == sum(branch(order), profit_weight(name, year, field)) by {
        lemma_profit5(db, part, supplier, line, pair, order, name, year, field);
    }
    verus_prooflib::lemma_flat_map_weight(db.orders, branch, profit_weight(name, year, field), nested);
    if base_match(part, supplier, line) && pair.ps_partkey == line.l_partkey && pair.ps_suppkey == line.l_suppkey {
        let count = |order: OrdersRow| if order.o_orderkey == line.l_orderkey && civil_year(order.o_orderdate) == year { 1int } else { 0int };
        let names = nation_count(db, supplier.s_nationkey, name);
        let amount = if field == 0 { 1int } else { line_weight(line, 1) - pair.ps_supplycost * line.l_quantity };
        lemma_sum_scale(db.orders, count, nested, names * amount);
        let dates = dates(db, line.l_orderkey, year);
        assert((names * amount) * dates == (names * dates) * amount) by (nonlinear_arith);
    } else { lemma_sum_zero(db.orders, nested); }
}

#[verifier::spinoff_prover]
pub proof fn lemma_profit3(db: Db, part: PartRow, supplier: SupplierRow, line: LineitemRow, name: Seq<u8>, year: int, field: int)
    requires 0 <= field <= 1,
    ensures sum(profit_tuples_3(db, params(), part, supplier, line), profit_weight(name, year, field)) == line_value(db, part, supplier, line, name, year, field),
{
    let branch = |pair: PartsuppRow| profit_tuples_4(db, params(), part, supplier, line, pair);
    let nested = |pair: PartsuppRow| pair_value(db, part, supplier, line, pair, name, year, field);
    assert forall|pair: PartsuppRow| #[trigger] nested(pair) == sum(branch(pair), profit_weight(name, year, field)) by {
        lemma_profit4(db, part, supplier, line, pair, name, year, field);
    }
    verus_prooflib::lemma_flat_map_weight(db.partsupp, branch, profit_weight(name, year, field), nested);
    if base_match(part, supplier, line) {
        let raw = |pair: PartsuppRow| if pair.ps_partkey == line.l_partkey && pair.ps_suppkey == line.l_suppkey {
            if field == 0 { 1int } else { line_weight(line, 1) - pair.ps_supplycost * line.l_quantity }
        } else { 0int };
        let scale = nation_count(db, supplier.s_nationkey, name) * dates(db, line.l_orderkey, year);
        lemma_sum_scale(db.partsupp, raw, nested, scale);
        if field != 0 {
            lemma_pair_linear(db, line.l_partkey, line.l_suppkey, line_weight(line, 1), line.l_quantity);
            lemma_sum_ext_all(db.partsupp, raw, |pair: PartsuppRow| if pair.ps_partkey == line.l_partkey && pair.ps_suppkey == line.l_suppkey {
                line_weight(line, 1) - pair.ps_supplycost * line.l_quantity
            } else { 0int });
        } else {
            lemma_sum_ext_all(db.partsupp, raw, |pair: PartsuppRow| if pair.ps_partkey == line.l_partkey && pair.ps_suppkey == line.l_suppkey { 1int } else { 0int });
        }
        assert(sum(db.partsupp, raw) == line_join(db, part.p_partkey, supplier.s_suppkey, line, field));
    } else { lemma_sum_zero(db.partsupp, nested); }
}

pub proof fn lemma_profit2(db: Db, part: PartRow, supplier: SupplierRow, name: Seq<u8>, year: int, field: int)
    requires 0 <= field <= 1,
    ensures sum(profit_tuples_2(db, params(), part, supplier), profit_weight(name, year, field)) == supplier_value(db, part, supplier, name, year, field),
{
    let branch = |line: LineitemRow| profit_tuples_3(db, params(), part, supplier, line);
    let nested = |line: LineitemRow| line_value(db, part, supplier, line, name, year, field);
    assert forall|line: LineitemRow| #[trigger] nested(line) == sum(branch(line), profit_weight(name, year, field)) by {
        lemma_profit3(db, part, supplier, line, name, year, field);
    }
    verus_prooflib::lemma_flat_map_weight(db.lineitem, branch, profit_weight(name, year, field), nested);
    if like_match(part.p_name, seq![b'%', b'g', b'r', b'e', b'e', b'n', b'%']) {
        let raw = |line: LineitemRow| dates(db, line.l_orderkey, year) * line_join(db, part.p_partkey, supplier.s_suppkey, line, field);
        let names = nation_count(db, supplier.s_nationkey, name);
        assert forall|line: LineitemRow| #[trigger] nested(line) == names * raw(line) by {
            let dates = dates(db, line.l_orderkey, year);
            let amount = line_join(db, part.p_partkey, supplier.s_suppkey, line, field);
            assert((names * dates) * amount == names * (dates * amount)) by (nonlinear_arith);
            if !base_match(part, supplier, line) { assert(amount == 0); }
        }
        lemma_sum_scale(db.lineitem, raw, nested, names);
        lemma_line_join(db, part.p_partkey, supplier.s_suppkey, year, field);
    } else { lemma_sum_zero(db.lineitem, nested); }
}

pub proof fn lemma_profit1(db: Db, part: PartRow, name: Seq<u8>, year: int, field: int)
    requires 0 <= field <= 1,
    ensures sum(profit_tuples_1(db, params(), part), profit_weight(name, year, field))
        == sum(db.supplier, |supplier: SupplierRow| supplier_value(db, part, supplier, name, year, field)),
{
    let branch = |supplier: SupplierRow| profit_tuples_2(db, params(), part, supplier);
    let nested = |supplier: SupplierRow| supplier_value(db, part, supplier, name, year, field);
    assert forall|supplier: SupplierRow| #[trigger] nested(supplier) == sum(branch(supplier), profit_weight(name, year, field)) by {
        lemma_profit2(db, part, supplier, name, year, field);
    }
    verus_prooflib::lemma_flat_map_weight(db.supplier, branch, profit_weight(name, year, field), nested);
}

pub proof fn lemma_supplier_join(db: Db, supplier: int, year: int, field: int)
    requires 0 <= field <= 1,
    ensures sum(db.part, |part: PartRow| if like_match(part.p_name, seq![b'%', b'g', b'r', b'e', b'e', b'n', b'%']) {
        part_join(db, part.p_partkey, supplier, year, field)
    } else { 0int }) == supplier_total(db, supplier, year, field),
{
    let cross = |part: PartRow, pair: PartsuppRow| if like_match(part.p_name, seq![b'%', b'g', b'r', b'e', b'e', b'n', b'%']) && pair.ps_partkey == part.p_partkey && pair.ps_suppkey == supplier {
        if field == 0 { moments(db, pair.ps_partkey, supplier, year, 0) }
        else { moments(db, pair.ps_partkey, supplier, year, 1) - pair.ps_supplycost * moments(db, pair.ps_partkey, supplier, year, 2) }
    } else { 0int };
    let outer = |part: PartRow| if like_match(part.p_name, seq![b'%', b'g', b'r', b'e', b'e', b'n', b'%']) { part_join(db, part.p_partkey, supplier, year, field) } else { 0int };
    let inner = supplier_source(db, supplier, year, field);
    assert forall|part: PartRow| #[trigger] outer(part) == sum(db.partsupp, |pair: PartsuppRow| cross(part, pair)) by {
        let weight = |pair: PartsuppRow| cross(part, pair);
        if like_match(part.p_name, seq![b'%', b'g', b'r', b'e', b'e', b'n', b'%']) {
            if field == 0 {
                let count = |pair: PartsuppRow| if pair.ps_partkey == part.p_partkey && pair.ps_suppkey == supplier { 1int } else { 0int };
                let amount = moments(db, part.p_partkey, supplier, year, 0);
                lemma_sum_scale(db.partsupp, count, weight, amount);
                assert(amount * pair_count(db, part.p_partkey, supplier) == pair_count(db, part.p_partkey, supplier) * amount) by (nonlinear_arith);
            } else {
                lemma_pair_linear(db, part.p_partkey, supplier, moments(db, part.p_partkey, supplier, year, 1), moments(db, part.p_partkey, supplier, year, 2));
                lemma_sum_ext_all(db.partsupp, weight, |pair: PartsuppRow| if pair.ps_partkey == part.p_partkey && pair.ps_suppkey == supplier {
                    moments(db, part.p_partkey, supplier, year, 1) - pair.ps_supplycost * moments(db, part.p_partkey, supplier, year, 2)
                } else { 0int });
            }
        } else { lemma_sum_zero(db.partsupp, weight); }
    }
    assert forall|pair: PartsuppRow| #[trigger] inner(pair) == sum(db.part, |part: PartRow| cross(part, pair)) by {
        let weight = |part: PartRow| cross(part, pair);
        if pair.ps_suppkey == supplier {
            let amount = if field == 0 { moments(db, pair.ps_partkey, supplier, year, 0) }
                else { moments(db, pair.ps_partkey, supplier, year, 1) - pair.ps_supplycost * moments(db, pair.ps_partkey, supplier, year, 2) };
            lemma_sum_scale(db.part, green_weight(pair.ps_partkey), weight, amount);
            assert(amount * green(db, pair.ps_partkey) == green(db, pair.ps_partkey) * amount) by (nonlinear_arith);
        } else { lemma_sum_zero(db.part, weight); }
    }
    lemma_sum_swap(db.part, db.partsupp, cross, outer, inner);
    lemma_sum_ext_all(db.partsupp, inner, |pair: PartsuppRow| if pair.ps_suppkey == supplier {
        green(db, pair.ps_partkey) * if field == 0 { moments(db, pair.ps_partkey, supplier, year, 0) }
        else { moments(db, pair.ps_partkey, supplier, year, 1) - pair.ps_supplycost * moments(db, pair.ps_partkey, supplier, year, 2) }
    } else { 0int });
}

pub proof fn lemma_group_factorization(db: Db, name: Seq<u8>, year: int, field: int)
    requires 0 <= field <= 1,
    ensures sum(profit_tuples(db, params()), profit_weight(name, year, field)) == group_database(db, name, year, field),
{
    let branch = |part: PartRow| profit_tuples_1(db, params(), part);
    let cross = |part: PartRow, supplier: SupplierRow| supplier_value(db, part, supplier, name, year, field);
    let outer = |part: PartRow| sum(db.supplier, |supplier: SupplierRow| cross(part, supplier));
    let inner = |supplier: SupplierRow| nation_count(db, supplier.s_nationkey, name) * supplier_total(db, supplier.s_suppkey, year, field);
    assert forall|part: PartRow| #[trigger] outer(part) == sum(branch(part), profit_weight(name, year, field)) by {
        lemma_profit1(db, part, name, year, field);
        lemma_sum_ext_all(db.supplier, |supplier: SupplierRow| supplier_value(db, part, supplier, name, year, field), |supplier: SupplierRow| cross(part, supplier));
    }
    verus_prooflib::lemma_flat_map_weight(db.part, branch, profit_weight(name, year, field), outer);
    assert forall|supplier: SupplierRow| #[trigger] inner(supplier) == sum(db.part, |part: PartRow| cross(part, supplier)) by {
        let raw = |part: PartRow| if like_match(part.p_name, seq![b'%', b'g', b'r', b'e', b'e', b'n', b'%']) { part_join(db, part.p_partkey, supplier.s_suppkey, year, field) } else { 0int };
        let combined = |part: PartRow| cross(part, supplier);
        let names = nation_count(db, supplier.s_nationkey, name);
        assert forall|part: PartRow| #[trigger] combined(part) == names * raw(part) by {
            if !like_match(part.p_name, seq![b'%', b'g', b'r', b'e', b'e', b'n', b'%']) { assert(names * 0int == 0) by (nonlinear_arith); }
        }
        lemma_sum_scale(db.part, raw, combined, names);
        lemma_supplier_join(db, supplier.s_suppkey, year, field);
    }
    lemma_sum_swap(db.part, db.supplier, cross, outer, inner);
    let nation_cross = |nation: NationRow, supplier: SupplierRow| if nation.n_name == name && nation.n_nationkey == supplier.s_nationkey {
        supplier_total(db, supplier.s_suppkey, year, field)
    } else { 0int };
    let nation_outer = |nation: NationRow| if nation.n_name == name { nation_total(db, nation.n_nationkey, year, field) } else { 0int };
    assert forall|nation: NationRow| #[trigger] nation_outer(nation) == sum(db.supplier, |supplier: SupplierRow| nation_cross(nation, supplier)) by {
        if nation.n_name != name { lemma_sum_zero(db.supplier, |supplier: SupplierRow| nation_cross(nation, supplier)); }
        else {
            lemma_sum_ext_all(db.supplier, |supplier: SupplierRow| if supplier.s_nationkey == nation.n_nationkey { supplier_total(db, supplier.s_suppkey, year, field) } else { 0int }, |supplier: SupplierRow| nation_cross(nation, supplier));
        }
    }
    assert forall|supplier: SupplierRow| #[trigger] inner(supplier) == sum(db.nation, |nation: NationRow| nation_cross(nation, supplier)) by {
        let count = |nation: NationRow| if nation.n_nationkey == supplier.s_nationkey && nation.n_name == name { 1int } else { 0int };
        let combined = |nation: NationRow| nation_cross(nation, supplier);
        let amount = supplier_total(db, supplier.s_suppkey, year, field);
        lemma_sum_scale(db.nation, count, combined, amount);
        assert(amount * nation_count(db, supplier.s_nationkey, name) == nation_count(db, supplier.s_nationkey, name) * amount) by (nonlinear_arith);
    }
    lemma_sum_swap(db.nation, db.supplier, nation_cross, nation_outer, inner);
}

#[verifier::spinoff_prover]
pub proof fn lemma_query_groups(db: Db, name: Seq<u8>, year: int, field: int)
    requires 0 <= field <= 1,
    ensures group_database(db, name, year, field) == if field == 0 { group_count(db, params(), name, year) }
        else { group_sum(db, params(), name, year, |row: Tuple| row.profit.amount) },
{
    let profit_rows = |row: ProfitTuple| ProfitRow { nation: row.nation.n_name, o_year: civil_year(row.orders.o_orderdate), amount: line_weight(row.lineitem, 1) - row.partsupp.ps_supplycost * row.lineitem.l_quantity };
    let row_weight = |row: ProfitRow| tuple_weight(name, year, field)(Tuple { profit: row });
    let branch = |row: ProfitRow| tuples_1(db, params(), row);
    assert forall|row: ProfitRow| #[trigger] row_weight(row) == sum(branch(row), tuple_weight(name, year, field)) by {
        lemma_sum_singleton(Tuple { profit: row }, tuple_weight(name, year, field));
    }
    verus_prooflib::lemma_flat_map_weight(rel_profit(db, params()), branch, tuple_weight(name, year, field), row_weight);
    assert(sum(tuples(db, params()), tuple_weight(name, year, field)) == sum(rel_profit(db, params()), row_weight));
    lemma_sum_map(profit_tuples(db, params()), profit_rows, row_weight, profit_weight(name, year, field));
    assert(profit_tuples(db, params()).map_values(profit_rows) =~= rel_profit(db, params()));
    assert(sum(rel_profit(db, params()), row_weight) == sum(profit_tuples(db, params()), profit_weight(name, year, field)));
    lemma_group_factorization(db, name, year, field);
    assert(sum(tuples(db, params()), tuple_weight(name, year, field)) == group_database(db, name, year, field));
    if field == 0 {
        lemma_sum_ext_all(tuples(db, params()), tuple_weight(name, year, field), |row: Tuple| if row.profit.nation == name && row.profit.o_year == year { 1int } else { 0int });
        assert(sum(tuples(db, params()), tuple_weight(name, year, field)) == group_count(db, params(), name, year));
    } else {
        lemma_group_amount(db, name, year, |row: Tuple| row.profit.amount);
        lemma_sum_ext_all(tuples(db, params()), tuple_weight(name, year, field), |row: Tuple| if row.profit.nation == name && row.profit.o_year == year { row.profit.amount } else { 0int });
        assert(sum(tuples(db, params()), tuple_weight(name, year, field)) == group_sum(db, params(), name, year, |row: Tuple| row.profit.amount));
    }
}

}
