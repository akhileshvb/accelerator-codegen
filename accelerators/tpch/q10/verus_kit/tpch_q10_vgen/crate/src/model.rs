use vstd::prelude::*;
use verus_common::sums::sum;
use verus_prooflib::ordered::Index;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::values::*;
use verus_prooflib::wide::Wide;
use crate::facts::*;
use crate::dimensions::*;
use crate::paged::*;
use crate::customers::*;
use crate::calendar::*;

verus! {

pub open spec fn line_weight(row: LineitemRow, component: int) -> int {
    if row.l_returnflag != seq![b'R'] { 0 }
    else if component == 0 { 1 }
    else {
        let revenue = row.l_extendedprice * (100 - row.l_discount);
        if component == 1 { revenue } else if revenue > 0 { revenue } else { 0 }
    }
}

#[verifier::opaque]
pub open spec fn fact_sum(db: Db, slice: int, customer: int, grain: int, component: int) -> int {
    sum(db.orders, |order: OrdersRow| {
        if order.o_custkey == customer && period_of(order.o_orderdate, grain) == slice {
            sum(db.lineitem, |line: LineitemRow|
                if line.l_orderkey == order.o_orderkey { line_weight(line, component) } else { 0 })
        } else { 0 }
    })
}

pub open spec fn fact_moments(db: Db, slice: int, customer: int, grain: int) -> Moments {
    Moments {
        count: fact_sum(db, slice, customer, grain, 0),
        revenue: fact_sum(db, slice, customer, grain, 1),
        upper: fact_sum(db, slice, customer, grain, 2),
    }
}

pub proof fn lemma_line_weight_ok(row: LineitemRow)
    ensures moment_ok(Moments { count: line_weight(row, 0), revenue: line_weight(row, 1), upper: line_weight(row, 2) }),
{}

/// Every group of facts holds consistent moments.
pub proof fn lemma_fact_moments_ok(db: Db, slice: int, customer: int, grain: int)
    ensures moment_ok(fact_moments(db, slice, customer, grain)),
{
    reveal(fact_sum);
    let k0 = 0int;
    let k1 = 1int;
    let k2 = 2int;
    let f0 = |order: OrdersRow| {
        if order.o_custkey == customer && period_of(order.o_orderdate, grain) == slice {
            sum(db.lineitem, |line: LineitemRow|
                if line.l_orderkey == order.o_orderkey { line_weight(line, k0) } else { 0 })
        } else { 0 }
    };
    let f1 = |order: OrdersRow| {
        if order.o_custkey == customer && period_of(order.o_orderdate, grain) == slice {
            sum(db.lineitem, |line: LineitemRow|
                if line.l_orderkey == order.o_orderkey { line_weight(line, k1) } else { 0 })
        } else { 0 }
    };
    let f2 = |order: OrdersRow| {
        if order.o_custkey == customer && period_of(order.o_orderdate, grain) == slice {
            sum(db.lineitem, |line: LineitemRow|
                if line.l_orderkey == order.o_orderkey { line_weight(line, k2) } else { 0 })
        } else { 0 }
    };
    assert forall|order: OrdersRow| moment_ok(Moments { count: #[trigger] f0(order), revenue: f1(order), upper: f2(order) }) by {
        if order.o_custkey == customer && period_of(order.o_orderdate, grain) == slice {
            let g0 = |line: LineitemRow| if line.l_orderkey == order.o_orderkey { line_weight(line, k0) } else { 0 };
            let g1 = |line: LineitemRow| if line.l_orderkey == order.o_orderkey { line_weight(line, k1) } else { 0 };
            let g2 = |line: LineitemRow| if line.l_orderkey == order.o_orderkey { line_weight(line, k2) } else { 0 };
            assert forall|line: LineitemRow| moment_ok(Moments { count: #[trigger] g0(line), revenue: g1(line), upper: g2(line) }) by {
                lemma_line_weight_ok(line);
            }
            lemma_sum_moment_ok(db.lineitem, g0, g1, g2);
        }
    }
    lemma_sum_moment_ok(db.orders, f0, f1, f2);
}

#[verifier::opaque]
pub open spec fn facts_valid(db: Db, slices: Index<Slice>, grain: int) -> bool {
    &&& forall|slice: int, customer: int, component: int|
        #[trigger] slice_at(slices, slice, customer).component(component)
        == fact_sum(db, slice, customer, grain, component)
    &&& forall|key: (int, Seq<u8>)| #[trigger] slices.at(key).is_some() ==> {
        let slice = slices.at(key).unwrap();
        &&& key.1 == Seq::<u8>::empty()
        &&& period_of(i32::MIN as int, grain) <= key.0 <= period_of(i32::MAX as int, grain)
        &&& slice.cells.nonempty()
        &&& slice.valid()
    }
}

pub open spec fn maxima_valid(totals: Index<Wide>, maxima: Index<Wide>) -> bool {
    &&& forall|key: (int, Seq<u8>)| #[trigger] totals.at(key).is_some() ==> {
        let value = totals.at(key).unwrap();
        &&& key.1 == Seq::<u8>::empty() && i64::MIN <= key.0 <= i64::MAX
        &&& value@ > 0
        &&& maxima.at(rank_view(sat(value@), key.0)).is_some()
        &&& maxima.at(rank_view(sat(value@), key.0)).unwrap()@ == value@
    }
    &&& forall|key: (int, Seq<u8>)| #[trigger] maxima.at(key).is_some() ==> {
        let value = maxima.at(key).unwrap();
        &&& value@ > 0
        &&& exists|join_key: int| #[trigger] totals.at((join_key, Seq::empty())).is_some()
            && totals.at((join_key, Seq::empty())).unwrap()@ == value@
            && key == rank_view(sat(value@), join_key)
    }
}

pub open spec fn ranked_values_valid(maxima: Index<Wide>) -> bool {
    forall|key: (int, Seq<u8>)| #[trigger] maxima.at(key).is_some() ==> {
        let value = maxima.at(key).unwrap();
        &&& value@ > 0
        &&& exists|tie: i64| #[trigger] rank_view(sat(value@), tie as int) == key
    }
}

pub proof fn lemma_maxima_ranked_values(totals: Index<Wide>, maxima: Index<Wide>)
    requires maxima_valid(totals, maxima),
    ensures ranked_values_valid(maxima),
{
    assert forall|key: (int, Seq<u8>)| #[trigger] maxima.at(key).is_some() implies {
        let value = maxima.at(key).unwrap();
        &&& value@ > 0
        &&& exists|tie: i64| #[trigger] rank_view(sat(value@), tie as int) == key
    } by {
        let value = maxima.at(key).unwrap();
        let join_key = choose|join_key: int| #[trigger] totals.at((join_key, Seq::empty())).is_some()
            && totals.at((join_key, Seq::empty())).unwrap()@ == value@
            && key == rank_view(sat(value@), join_key);
        assert(i64::MIN <= join_key <= i64::MAX);
        assert(rank_view(sat(value@), join_key as i64 as int) == key);
    }
}

#[verifier::opaque]
pub open spec fn dimensions_valid(db: Db, dimensions: Dimensions) -> bool {
    &&& customer_shape(dimensions)
    &&& forall|weight: spec_fn(CustomerRow) -> int| #[trigger] table_sum(dimensions.customers, weight) == sum(db.customer, weight)
    &&& forall|row: NationRow| #[trigger] nation_count(dimensions.nations, row)
        == sum(db.nation, |value: NationRow| if value == row { 1 } else { 0 })
    &&& forall|key: int| #[trigger] total_at(dimensions.nation_totals, key)
        == sum(db.nation, |row: NationRow| if row.n_nationkey == key { 1 } else { 0 })
    &&& maxima_valid(dimensions.nation_totals, dimensions.nation_maxima)
    &&& forall|outer: (int, Seq<u8>)| #[trigger] dimensions.nations.at(outer).is_some() ==> {
        let inner = dimensions.nations.at(outer).unwrap();
        &&& inner.wf()
        &&& outer.1 == Seq::<u8>::empty()
        &&& inner.entries().len() > 0
        &&& forall|key: (int, Seq<u8>)| #[trigger] inner.at(key).is_some() ==> {
            let value = inner.at(key).unwrap();
            &&& value.count@ > 0
            &&& nation_input_view(*value.row).n_nationkey == outer.0
            &&& nation_inner_view(nation_input_view(*value.row)) == key
        }
    }
}

pub proof fn lemma_facts_shape(db: Db, slices: Index<Slice>, grain: int)
    requires facts_valid(db, slices, grain),
    ensures slices_valid(slices),
{
    reveal(facts_valid);
}

pub proof fn lemma_facts_equal(db: Db, previous: Index<Slice>, next: Index<Slice>, grain: int)
    requires facts_valid(db, previous, grain),
        forall|key: (int, Seq<u8>)| #![trigger previous.at(key), next.at(key)] previous.at(key) == next.at(key),
    ensures facts_valid(db, next, grain),
{
    reveal(facts_valid);
    assert forall|day: int, customer: int, component: int|
        #[trigger] slice_at(next, day, customer).component(component) == fact_sum(db, day, customer, grain, component) by {
        assert(next.at((day, Seq::empty())) == previous.at((day, Seq::empty())));
        assert(slice_at(previous, day, customer).component(component) == fact_sum(db, day, customer, grain, component));
    }
    assert forall|key: (int, Seq<u8>)| #[trigger] next.at(key).is_some() implies {
        let slice = next.at(key).unwrap();
        &&& key.1 == Seq::<u8>::empty()
        &&& period_of(i32::MIN as int, grain) <= key.0 <= period_of(i32::MAX as int, grain)
        &&& slice.cells.nonempty()
        &&& slice.valid()
    } by {
        assert(next.at(key) == previous.at(key));
        assert(previous.at(key).is_some());
    }
}

pub proof fn lemma_dimensions_shape(db: Db, dimensions: Dimensions)
    requires dimensions_valid(db, dimensions),
    ensures customer_shape(dimensions), nation_index_valid(dimensions.nations),
        maxima_valid(dimensions.nation_totals, dimensions.nation_maxima),
{
    reveal(dimensions_valid);
}

pub proof fn lemma_nation_count_nonnegative(db: Db, dimensions: Dimensions, row: NationRow)
    requires forall|target: NationRow| #[trigger] nation_count(dimensions.nations, target)
        == sum(db.nation, crate::dimension_updates::nation_eq(target)),
    ensures nation_count(dimensions.nations, row) >= 0,
{
    hide(nation_count);
    assert(nation_count(dimensions.nations, row) == sum(db.nation, crate::dimension_updates::nation_eq(row)));
    verus_common::sums::lemma_sum_nonneg(db.nation, crate::dimension_updates::nation_eq(row));
}

pub proof fn lemma_dimensions_from_counts(db: Db, dimensions: Dimensions)
    requires crate::dimension_updates::shape(dimensions),
        forall|weight: spec_fn(CustomerRow) -> int| #[trigger] table_sum(dimensions.customers, weight) == sum(db.customer, weight),
        forall|row: NationRow| #[trigger] nation_count(dimensions.nations, row)
            == sum(db.nation, crate::dimension_updates::nation_eq(row)),
        forall|key: int| #[trigger] total_at(dimensions.nation_totals, key)
            == sum(db.nation, |row: NationRow| if row.n_nationkey == key { 1int } else { 0int }),
    ensures dimensions_valid(db, dimensions),
{
    reveal(dimensions_valid);
    assert forall|row: NationRow| #[trigger] nation_count(dimensions.nations, row)
        == sum(db.nation, |value: NationRow| if value == row { 1int } else { 0int }) by {
        verus_common::sums::lemma_sum_ext_all(db.nation, crate::dimension_updates::nation_eq(row),
            |value: NationRow| if value == row { 1int } else { 0int });
    }
    assert forall|outer: (int, Seq<u8>)| #[trigger] dimensions.nations.at(outer).is_some() implies {
        let inner = dimensions.nations.at(outer).unwrap();
        &&& inner.wf()
        &&& outer.1 == Seq::<u8>::empty()
        &&& inner.entries().len() > 0
        &&& forall|key: (int, Seq<u8>)| #[trigger] inner.at(key).is_some() ==> {
            let value = inner.at(key).unwrap();
            &&& value.count@ > 0
            &&& nation_input_view(*value.row).n_nationkey == outer.0
            &&& nation_inner_view(nation_input_view(*value.row)) == key
        }
    } by {
        let inner = dimensions.nations.at(outer).unwrap();
        assert forall|key: (int, Seq<u8>)| #[trigger] inner.at(key).is_some() implies {
            let value = inner.at(key).unwrap();
            &&& value.count@ > 0
            &&& nation_input_view(*value.row).n_nationkey == outer.0
            &&& nation_inner_view(nation_input_view(*value.row)) == key
        } by {
            let value = inner.at(key).unwrap();
            let row = nation_input_view(*value.row);
            assert(nation_count(dimensions.nations, row) == value.count@);
            lemma_nation_count_nonnegative(db, dimensions, row);
        }
    }
}

pub proof fn lemma_dimensions_equal(db: Db, previous: Dimensions, next: Dimensions)
    requires dimensions_valid(db, previous), crate::dimension_updates::shape(next),
        next.customers.entries() == previous.customers.entries(),
        next.nations.tree() == previous.nations.tree(),
        next.nation_totals.tree() == previous.nation_totals.tree(),
    ensures dimensions_valid(db, next),
{
    reveal(dimensions_valid);
    assert forall|key: (int, Seq<u8>)| #[trigger] next.nations.at(key) == previous.nations.at(key) by {}
    assert forall|key: (int, Seq<u8>)| #[trigger] next.nation_totals.at(key) == previous.nation_totals.at(key) by {}
    assert forall|weight: spec_fn(CustomerRow) -> int| #[trigger] table_sum(next.customers, weight) == sum(db.customer, weight) by {
        assert(table_sum(previous.customers, weight) == sum(db.customer, weight));
    }
    assert forall|row: NationRow| #[trigger] nation_count(next.nations, row)
        == sum(db.nation, |value: NationRow| if value == row { 1int } else { 0int }) by {
        assert(nation_count(previous.nations, row) == sum(db.nation, |value: NationRow| if value == row { 1int } else { 0int }));
        verus_common::sums::lemma_sum_ext_all(db.nation, |value: NationRow| if value == row { 1int } else { 0int },
            crate::dimension_updates::nation_eq(row));
    }
    assert forall|key: int| #[trigger] total_at(next.nation_totals, key)
        == sum(db.nation, |row: NationRow| if row.n_nationkey == key { 1int } else { 0int }) by {
        assert(total_at(previous.nation_totals, key) == sum(db.nation, |row: NationRow| if row.n_nationkey == key { 1int } else { 0int }));
    }
    lemma_dimensions_from_counts(db, next);
}

pub proof fn lemma_maxima_equal(totals: Index<Wide>, maxima: Index<Wide>, other_totals: Index<Wide>, other_maxima: Index<Wide>)
    requires maxima_valid(totals, maxima), totals.tree() == other_totals.tree(), maxima.tree() == other_maxima.tree(),
    ensures maxima_valid(other_totals, other_maxima),
{
    assert forall|key: (int, Seq<u8>)| #[trigger] other_totals.at(key) == totals.at(key) by {}
    assert forall|key: (int, Seq<u8>)| #[trigger] other_maxima.at(key) == maxima.at(key) by {}
    assert forall|key: (int, Seq<u8>)| #[trigger] other_maxima.at(key).is_some() implies {
        let value = other_maxima.at(key).unwrap();
        &&& value@ > 0
        &&& exists|join_key: int| #[trigger] other_totals.at((join_key, Seq::empty())).is_some()
            && other_totals.at((join_key, Seq::empty())).unwrap()@ == value@
            && key == rank_view(sat(value@), join_key)
    } by {
        let value = other_maxima.at(key).unwrap();
        let join_key = choose|join_key: int| #[trigger] totals.at((join_key, Seq::empty())).is_some()
            && totals.at((join_key, Seq::empty())).unwrap()@ == value@
            && key == rank_view(sat(value@), join_key);
        assert(other_totals.at((join_key, Seq::empty())) == totals.at((join_key, Seq::empty())));
    }
}

#[verifier::rlimit(20)]
pub proof fn lemma_dimensions_from_scans(db: Db, dimensions: Dimensions, customers: Seq<CustomerInput>, nations: Seq<NationInput>)
    requires scan_customers_ok(db, customers), scan_nations_ok(db, nations),
        customer_shape(dimensions), nation_index_valid(dimensions.nations),
        maxima_valid(dimensions.nation_totals, dimensions.nation_maxima),
        forall|weight: spec_fn(CustomerRow) -> int| #[trigger] table_sum(dimensions.customers, weight)
            == sum(customer_input_seq_view(customers), weight),
        forall|row: NationRow| #[trigger] nation_count(dimensions.nations, row)
            == sum(nations, |input: NationInput| if nation_input_view(input) == row { 1 } else { 0 }),
        forall|key: int| #[trigger] total_at(dimensions.nation_totals, key)
            == sum(nations, |input: NationInput| if input.n_nationkey == key { 1 } else { 0 }),
    ensures dimensions_valid(db, dimensions),
{
    assert forall|weight: spec_fn(CustomerRow) -> int| #[trigger] table_sum(dimensions.customers, weight) == sum(db.customer, weight) by {
        assert(sum(customer_input_seq_view(customers), weight) == sum(db.customer, weight));
    }
    assert forall|row: NationRow| #[trigger] nation_count(dimensions.nations, row)
        == sum(db.nation, crate::dimension_updates::nation_eq(row)) by {
        let weight = |value: NationRow| if value == row { 1int } else { 0int };
        verus_common::sums::lemma_sum_map(nations, |input: NationInput| nation_input_view(input), weight,
            |input: NationInput| if nation_input_view(input) == row { 1int } else { 0int });
        assert(sum(nation_input_seq_view(nations), weight) == sum(db.nation, weight));
        assert(crate::dimension_updates::nation_eq(row) =~= weight);
    }
    assert forall|key: int| #[trigger] total_at(dimensions.nation_totals, key)
        == sum(db.nation, |row: NationRow| if row.n_nationkey == key { 1int } else { 0int }) by {
        let weight = |value: NationRow| if value.n_nationkey == key { 1int } else { 0int };
        verus_common::sums::lemma_sum_map(nations, |input: NationInput| nation_input_view(input), weight,
            |input: NationInput| if input.n_nationkey == key { 1int } else { 0int });
        assert(sum(nation_input_seq_view(nations), weight) == sum(db.nation, weight));
    }
    lemma_dimensions_from_counts(db, dimensions);
}

}
