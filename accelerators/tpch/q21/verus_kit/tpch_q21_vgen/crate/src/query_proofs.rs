use vstd::prelude::*;
use verus_common::rel::*;
use verus_common::sums::*;
use crate::gen_spec::*;
use crate::aggregates::*;
use crate::index::*;

verus! {

pub open spec fn tuple_weight(name: Seq<u8>) -> spec_fn(Tuple) -> int {
    |row: Tuple| if row.supplier.s_name == name { 1 } else { 0 }
}

pub proof fn lemma_group_by_nation(db: Db, parameter: Params, name: Seq<u8>)
    ensures group_count(db, parameter, name) == sum(db.nation, |nation: NationRow|
        if nation.n_name == parameter.p1 { bucket(db, nation.n_nationkey, name) } else { 0 }),
{
    lemma_group_count(db, parameter, name);
    let pair = |supplier: SupplierRow, nation: NationRow| if supplier.s_name == name && nation.n_name == parameter.p1
        && supplier.s_nationkey == nation.n_nationkey { waiting(db, supplier.s_suppkey) } else { 0 };
    let supplier_term = |supplier: SupplierRow| if supplier.s_name == name { waiting(db, supplier.s_suppkey) * nation_multiplicity(db, supplier.s_nationkey, parameter.p1) } else { 0 };
    let nation_term = |nation: NationRow| if nation.n_name == parameter.p1 { bucket(db, nation.n_nationkey, name) } else { 0 };
    assert forall|supplier: SupplierRow| #[trigger] supplier_term(supplier) == sum(db.nation, |nation: NationRow| pair(supplier, nation)) by {
        let indicator = |nation: NationRow| if nation.n_nationkey == supplier.s_nationkey && nation.n_name == parameter.p1 { 1int } else { 0int };
        let weight = if supplier.s_name == name { waiting(db, supplier.s_suppkey) } else { 0int };
        lemma_sum_scale(db.nation, indicator, |nation: NationRow| pair(supplier, nation), weight);
        assert(weight * nation_multiplicity(db, supplier.s_nationkey, parameter.p1) == supplier_term(supplier));
    }
    assert forall|nation: NationRow| #[trigger] nation_term(nation) == sum(db.supplier, |supplier: SupplierRow| pair(supplier, nation)) by {
        if nation.n_name == parameter.p1 {
            lemma_sum_ext(db.supplier, |supplier: SupplierRow| pair(supplier, nation), |supplier: SupplierRow|
                if supplier.s_nationkey == nation.n_nationkey && supplier.s_name == name { waiting(db, supplier.s_suppkey) } else { 0 });
        } else { lemma_sum_zero(db.supplier, |supplier: SupplierRow| pair(supplier, nation)); }
    }
    lemma_sum_swap(db.supplier, db.nation, pair, supplier_term, nation_term);
}

pub open spec fn matches_for(matches: Counts, db: Db, parameter: Params) -> bool {
    &&& positive(matches)
    &&& forall|key: KeyView| #![trigger count(matches, key)] count(matches, key) ==
        if key.1 == Seq::<u8>::empty() { nation_multiplicity(db, key.0, parameter.p1) } else { 0 }
}

pub open spec fn indexed_group(matches: Counts, buckets: Slices, name: Seq<u8>) -> int {
    sum(matches.entries(), |row: (KeyView, i128)| row.1 as int * nested_count(buckets, row.0, (0int, name)))
}

pub proof fn lemma_matches_for(nations: &Slices, matches: &Counts, db: Db, parameter: Params)
    requires nations_for(*nations, db), nations.at((0int, parameter.p1)) == Some(*matches),
    ensures matches_for(*matches, db, parameter),
{
    nations.model();
    let position = choose|position: int| #![auto] 0 <= position < nations.entries().len() && nations.entries()[position].0 == (0int, parameter.p1);
    assert(nations.entries()[position].1 == *matches);
    assert forall|key: KeyView| #![trigger count(*matches, key)] count(*matches, key) ==
        if key.1 == Seq::<u8>::empty() { nation_multiplicity(db, key.0, parameter.p1) } else { 0 } by {
        assert(count(*matches, key) == nested_count(*nations, (0int, parameter.p1), key));
    }
}

#[verifier::rlimit(20)]
pub proof fn lemma_group_indexed(db: Db, parameter: Params, matches: &Counts, buckets: &Slices, name: Seq<u8>)
    requires matches_for(*matches, db, parameter), buckets_for(*buckets, db),
    ensures group_count(db, parameter, name) == indexed_group(*matches, *buckets, name),
{
    hide(Tree::at);
    hide(Tree::entries);
    hide(Tree::wf);
    matches.model();
    lemma_group_by_nation(db, parameter, name);
    let pair = |nation: NationRow, row: (KeyView, i128)| if nation.n_name == parameter.p1 && row.0 == (nation.n_nationkey, Seq::<u8>::empty()) {
        bucket(db, nation.n_nationkey, name)
    } else { 0 };
    let left = |nation: NationRow| sum(matches.entries(), |row: (KeyView, i128)| pair(nation, row));
    let right = |row: (KeyView, i128)| sum(db.nation, |nation: NationRow| pair(nation, row));
    lemma_sum_swap(db.nation, matches.entries(), pair, left, right);
    let full = |nation: NationRow| if nation.n_name == parameter.p1 { bucket(db, nation.n_nationkey, name) } else { 0 };
    assert forall|position: int| #![trigger db.nation[position]] 0 <= position < db.nation.len() implies left(db.nation[position]) == full(db.nation[position]) by {
        let nation = db.nation[position];
        let weight = |row: (KeyView, i128)| pair(nation, row);
        if nation.n_name == parameter.p1 {
            let key = (nation.n_nationkey, Seq::<u8>::empty());
            let indicator = |other: NationRow| if other.n_nationkey == nation.n_nationkey && other.n_name == parameter.p1 { 1int } else { 0int };
            lemma_sum_ge_term(db.nation, indicator, position);
            assert(count(*matches, key) > 0);
            assert(matches.at(key).is_some());
            let source = choose|source: int| #![auto] 0 <= source < matches.entries().len() && matches.entries()[source].0 == key;
            assert forall|other: int| #![trigger matches.entries()[other]] 0 <= other < matches.entries().len() && other != source implies weight(matches.entries()[other]) == 0 by {
                lemma_key_order(key, key);
                if other < source { assert(key_lt(matches.entries()[other].0, matches.entries()[source].0)); }
                else { assert(key_lt(matches.entries()[source].0, matches.entries()[other].0)); }
            }
            lemma_sum_single_match(matches.entries(), weight, source);
        } else { lemma_sum_zero(matches.entries(), weight); }
    }
    lemma_sum_ext(db.nation, left, full);
    let indexed = |row: (KeyView, i128)| row.1 as int * nested_count(*buckets, row.0, (0int, name));
    assert forall|position: int| #![trigger matches.entries()[position]] 0 <= position < matches.entries().len() implies right(matches.entries()[position]) == indexed(matches.entries()[position]) by {
        let row = matches.entries()[position];
        assert(count(*matches, row.0) == row.1 as int);
        assert(row.0.1 == Seq::<u8>::empty());
        let indicator = |nation: NationRow| if nation.n_nationkey == row.0.0 && nation.n_name == parameter.p1 { 1int } else { 0int };
        lemma_sum_scale(db.nation, indicator, |nation: NationRow| pair(nation, row), bucket(db, row.0.0, name));
        assert(bucket(db, row.0.0, name) * nation_multiplicity(db, row.0.0, parameter.p1) == nation_multiplicity(db, row.0.0, parameter.p1) * bucket(db, row.0.0, name)) by(nonlinear_arith);
    }
    lemma_sum_ext(matches.entries(), right, indexed);
}

pub proof fn lemma_bucket_names(buckets: &Slices, db: Db, outer: KeyView, groups: &Counts)
    requires buckets_for(*buckets, db), positive(*groups),
        forall|inner: KeyView| #![trigger count(*groups, inner)] count(*groups, inner) == nested_count(*buckets, outer, inner),
    ensures forall|position: int| 0 <= position < groups.entries().len() ==> (#[trigger] groups.entries()[position]).0.0 == 0,
{
    groups.model();
    assert forall|position: int| 0 <= position < groups.entries().len() implies (#[trigger] groups.entries()[position]).0.0 == 0 by {
        let key = groups.entries()[position].0;
        assert(count(*groups, key) > 0);
        assert(nested_count(*buckets, outer, key) > 0);
    }
}

pub open spec fn line_join(db: Db, supplier: SupplierRow, line: LineitemRow) -> bool {
    supplier.s_suppkey == line.l_suppkey && line.l_receiptdate > line.l_commitdate
        && exists_1(db, line) && !exists_2(db, line)
}

pub open spec fn order_join(line: LineitemRow, order: OrdersRow) -> bool {
    line.l_orderkey == order.o_orderkey && order.o_orderstatus == seq![b'F']
}

pub proof fn lemma_tuple_count(db: Db, parameter: Params, supplier: SupplierRow, line: LineitemRow, order: OrdersRow, nation: NationRow, name: Seq<u8>)
    ensures sum(tuples_4(db, parameter, supplier, line, order, nation), tuple_weight(name)) ==
        if where_clause(db, parameter, supplier, line, order, nation) && supplier.s_name == name { 1int } else { 0int },
{
    lemma_sum_opt(where_clause(db, parameter, supplier, line, order, nation),
        Tuple { supplier, l1: line, orders: order, nation }, tuple_weight(name));
}

pub proof fn lemma_nation_count(db: Db, parameter: Params, supplier: SupplierRow, line: LineitemRow, order: OrdersRow, name: Seq<u8>)
    ensures sum(tuples_3(db, parameter, supplier, line, order), tuple_weight(name)) ==
        if supplier.s_name == name && line_join(db, supplier, line) && order_join(line, order) {
            nation_multiplicity(db, supplier.s_nationkey, parameter.p1)
        } else { 0 },
{
    let extension = |nation: NationRow| tuples_4(db, parameter, supplier, line, order, nation);
    let actual = |nation: NationRow| sum(extension(nation), tuple_weight(name));
    let predicate = |nation: NationRow| if nation.n_nationkey == supplier.s_nationkey && nation.n_name == parameter.p1 { 1int } else { 0int };
    let enabled = supplier.s_name == name && line_join(db, supplier, line) && order_join(line, order);
    let weight = |nation: NationRow| if enabled && nation.n_nationkey == supplier.s_nationkey && nation.n_name == parameter.p1 { 1int } else { 0int };
    lemma_sum_flat_map(db.nation, extension, tuple_weight(name));
    assert forall|position: int| #![trigger db.nation[position]] 0 <= position < db.nation.len() implies actual(db.nation[position]) == weight(db.nation[position]) by {
        lemma_tuple_count(db, parameter, supplier, line, order, db.nation[position], name);
    }
    lemma_sum_ext(db.nation, actual, weight);
    lemma_sum_scale(db.nation, predicate, weight, if enabled { 1 } else { 0 });
}

pub proof fn lemma_order_count(db: Db, parameter: Params, supplier: SupplierRow, line: LineitemRow, name: Seq<u8>)
    ensures sum(tuples_2(db, parameter, supplier, line), tuple_weight(name)) ==
        if supplier.s_name == name && line_join(db, supplier, line) {
            finished(db, line.l_orderkey) * nation_multiplicity(db, supplier.s_nationkey, parameter.p1)
        } else { 0 },
{
    let extension = |order: OrdersRow| tuples_3(db, parameter, supplier, line, order);
    let actual = |order: OrdersRow| sum(extension(order), tuple_weight(name));
    let enabled = supplier.s_name == name && line_join(db, supplier, line);
    let nations = nation_multiplicity(db, supplier.s_nationkey, parameter.p1);
    let weight = |order: OrdersRow| if enabled && order_join(line, order) { nations } else { 0int };
    let predicate = |order: OrdersRow| if order_join(line, order) { 1int } else { 0int };
    lemma_sum_flat_map(db.orders, extension, tuple_weight(name));
    assert forall|position: int| #![trigger db.orders[position]] 0 <= position < db.orders.len() implies actual(db.orders[position]) == weight(db.orders[position]) by {
        lemma_nation_count(db, parameter, supplier, line, db.orders[position], name);
    }
    lemma_sum_ext(db.orders, actual, weight);
    lemma_sum_scale(db.orders, predicate, weight, if enabled { nations } else { 0 });
    lemma_sum_ext(db.orders, predicate, |row: OrdersRow| if row.o_orderkey == line.l_orderkey && row.o_orderstatus == seq![b'F'] { 1int } else { 0int });
    assert(nations * finished(db, line.l_orderkey) == finished(db, line.l_orderkey) * nations) by(nonlinear_arith);
}

pub proof fn lemma_line_count(db: Db, parameter: Params, supplier: SupplierRow, name: Seq<u8>)
    ensures sum(tuples_1(db, parameter, supplier), tuple_weight(name)) ==
        if supplier.s_name == name {
            waiting(db, supplier.s_suppkey) * nation_multiplicity(db, supplier.s_nationkey, parameter.p1)
        } else { 0 },
{
    let extension = |line: LineitemRow| tuples_2(db, parameter, supplier, line);
    let actual = |line: LineitemRow| sum(extension(line), tuple_weight(name));
    let nations = nation_multiplicity(db, supplier.s_nationkey, parameter.p1);
    let weight = |line: LineitemRow| if supplier.s_name == name && line_join(db, supplier, line) {
        finished(db, line.l_orderkey) * nations
    } else { 0int };
    let predicate = |line: LineitemRow| if line_join(db, supplier, line) { finished(db, line.l_orderkey) } else { 0int };
    lemma_sum_flat_map(db.lineitem, extension, tuple_weight(name));
    assert forall|position: int| #![trigger db.lineitem[position]] 0 <= position < db.lineitem.len() implies actual(db.lineitem[position]) == weight(db.lineitem[position]) by {
        lemma_order_count(db, parameter, supplier, db.lineitem[position], name);
    }
    lemma_sum_ext(db.lineitem, actual, weight);
    let coefficient = if supplier.s_name == name { nations } else { 0int };
    assert forall|row: LineitemRow| #[trigger] weight(row) == coefficient * predicate(row) by {
        if supplier.s_name == name && line_join(db, supplier, row) {
            assert(nations * finished(db, row.l_orderkey) == finished(db, row.l_orderkey) * nations) by(nonlinear_arith);
        }
    }
    lemma_sum_scale(db.lineitem, predicate, weight, if supplier.s_name == name { nations } else { 0 });
    lemma_sum_ext(db.lineitem, predicate, |row: LineitemRow| if row.l_suppkey == supplier.s_suppkey && row.l_receiptdate > row.l_commitdate && exists_1(db, row) && !exists_2(db, row) { finished(db, row.l_orderkey) } else { 0int });
    assert(nations * waiting(db, supplier.s_suppkey) == waiting(db, supplier.s_suppkey) * nations) by(nonlinear_arith);
}

pub proof fn lemma_group_count(db: Db, parameter: Params, name: Seq<u8>)
    ensures group_count(db, parameter, name) == sum(db.supplier, |supplier: SupplierRow|
        if supplier.s_name == name { waiting(db, supplier.s_suppkey) * nation_multiplicity(db, supplier.s_nationkey, parameter.p1) } else { 0 }),
{
    let extension = |supplier: SupplierRow| tuples_1(db, parameter, supplier);
    let actual = |supplier: SupplierRow| sum(extension(supplier), tuple_weight(name));
    let weight = |supplier: SupplierRow| if supplier.s_name == name {
        waiting(db, supplier.s_suppkey) * nation_multiplicity(db, supplier.s_nationkey, parameter.p1)
    } else { 0int };
    lemma_sum_flat_map(db.supplier, extension, tuple_weight(name));
    assert forall|position: int| #![trigger db.supplier[position]] 0 <= position < db.supplier.len() implies actual(db.supplier[position]) == weight(db.supplier[position]) by {
        lemma_line_count(db, parameter, db.supplier[position], name);
    }
    lemma_sum_ext(db.supplier, actual, weight);
}

pub proof fn lemma_no_nation_result(db: Db, parameter: Params)
    requires forall|position: int| #![trigger db.nation[position]] 0 <= position < db.nation.len() ==> db.nation[position].n_name != parameter.p1,
    ensures query_result(db, parameter, Seq::<OutRow>::empty()),
{
    assert forall|name: Seq<u8>| #[trigger] group_count(db, parameter, name) == 0 by {
        lemma_group_count(db, parameter, name);
        let weight = |supplier: SupplierRow| if supplier.s_name == name {
            waiting(db, supplier.s_suppkey) * nation_multiplicity(db, supplier.s_nationkey, parameter.p1)
        } else { 0int };
        assert forall|position: int| #![trigger db.supplier[position]] 0 <= position < db.supplier.len() implies weight(db.supplier[position]) == 0 by {
            let supplier = db.supplier[position];
            lemma_sum_zero(db.nation, |row: NationRow| if row.n_nationkey == supplier.s_nationkey && row.n_name == parameter.p1 { 1int } else { 0int });
        }
        lemma_sum_zero(db.supplier, weight);
    }
    let empty = Seq::<OutRow>::empty();
    assert(full_result(db, parameter, empty));
    assert(query_result(db, parameter, empty));
}

pub proof fn lemma_missing_nation(nations: &Slices, db: Db, parameter: Params)
    requires nations_for(*nations, db), nations.at((0int, parameter.p1)).is_none(),
    ensures query_result(db, parameter, Seq::<OutRow>::empty()),
{
    assert forall|position: int| #![trigger db.nation[position]] 0 <= position < db.nation.len() implies db.nation[position].n_name != parameter.p1 by {
        if db.nation[position].n_name == parameter.p1 {
            let nation = db.nation[position];
            let weight = |row: NationRow| if row.n_nationkey == nation.n_nationkey && row.n_name == parameter.p1 { 1int } else { 0int };
            lemma_sum_ge_term(db.nation, weight, position);
            assert(nested_count(*nations, (0int, parameter.p1), (nation.n_nationkey, Seq::<u8>::empty())) == 0);
            assert(nation_multiplicity(db, nation.n_nationkey, parameter.p1) == 0);
        }
    }
    lemma_no_nation_result(db, parameter);
}

}
