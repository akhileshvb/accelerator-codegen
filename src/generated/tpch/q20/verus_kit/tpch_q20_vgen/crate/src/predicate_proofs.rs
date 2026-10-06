use vstd::prelude::*;
use verus_common::sums::*;
use verus_common::rel::*;
use verus_common::order::*;
use verus_prooflib::*;
use crate::gen_spec::*;
use crate::model::*;
use crate::profiles::*;
use crate::profile_proofs::*;
use crate::storage::*;
use crate::dimensions::*;
use verus_common::sqlfns::like_match;

verus! {
pub proof fn lemma_in2(db: Db, parameters: Params, key: int)
    ensures in_2(db, parameters, key) == part_matches(db, parameters.p2, key),
{
    let extend = |row: PartRow| in_2_tuples_1(db, parameters, row);
    if in_2(db, parameters, key) {
        let index = choose|index: int| #![trigger in_2_rows(db, parameters)[index]] 0 <= index < in_2_rows(db, parameters).len() && in_2_rows(db, parameters)[index].p_partkey == key;
        let tuple = in_2_tuples(db, parameters)[index];
        assert(in_2_tuples(db, parameters).contains(tuple));
        lemma_flat_map_contains(db.part, extend, tuple);
        let source = choose|source: int| #![trigger extend(db.part[source]).contains(tuple)] 0 <= source < db.part.len() && extend(db.part[source]).contains(tuple);
        assert(tuple.part == db.part[source]);
        if !in_2_where_clause(db, parameters, db.part[source]) { assert(extend(db.part[source]) =~= Seq::<In2Tuple>::empty()); }
        assert(in_2_where_clause(db, parameters, db.part[source]));
        assert(db.part[source].p_partkey == key);
        assert(like_match(db.part[source].p_name, parameters.p2));
    }
    if part_matches(db, parameters.p2, key) {
        let source = choose|source: int| #![trigger db.part[source]] 0 <= source < db.part.len()
            && db.part[source].p_partkey == key && like_match(db.part[source].p_name, parameters.p2);
        let tuple = In2Tuple { part: db.part[source] };
        assert(in_2_where_clause(db, parameters, db.part[source]));
        assert(extend(db.part[source])[0] == tuple);
        assert(extend(db.part[source]).contains(tuple));
        lemma_flat_map_contains(db.part, extend, tuple);
        let index = choose|index: int| #![trigger in_2_tuples(db, parameters)[index]] 0 <= index < in_2_tuples(db, parameters).len() && in_2_tuples(db, parameters)[index] == tuple;
        assert(in_2_rows(db, parameters)[index].p_partkey == key);
    }
}
pub proof fn lemma_part_matches_eligible(db: Db, pattern: Seq<u8>, key: int)
    requires part_matches(db, pattern, key),
    ensures eligible(db, key) > 0,
{
    let source = choose|source: int| #![trigger db.part[source]] 0 <= source < db.part.len()
        && db.part[source].p_partkey == key && like_match(db.part[source].p_name, pattern);
    assert(crate::semantic_proofs::qualifying_weight(key)(db.part[source]) == 1);
    crate::semantic_proofs::lemma_eligible_presence(db, key);
}
pub proof fn lemma_names_match(db: Db, names: PartNames, pattern: Seq<u8>, key: i64)
    requires names_correct(db, names),
    ensures names_match(names, key, pattern) == part_matches(db, pattern, key as int),
{
    if names_match(names, key, pattern) {
        let name = choose|name: Seq<u8>| #![trigger name_count(names, key, name)] name_count(names, key, name) > 0 && like_match(name, pattern);
        let wp = |row: PartRow| if row.p_partkey == key && row.p_name == name { 1int } else { 0int };
        assert(name_count(names, key, name) == sum(db.part, wp));
        let j = lemma_sum_nonzero_witness(db.part, wp);
        assert(db.part[j].p_partkey == key as int && db.part[j].p_name == name);
        assert(part_matches(db, pattern, key as int));
    }
    if part_matches(db, pattern, key as int) {
        let j = choose|j: int| #![trigger db.part[j]] 0 <= j < db.part.len()
            && db.part[j].p_partkey == key as int && like_match(db.part[j].p_name, pattern);
        let name = db.part[j].p_name;
        let wp = |row: PartRow| if row.p_partkey == key && row.p_name == name { 1int } else { 0int };
        lemma_sum_pos(db.part, wp, j);
        assert(name_count(names, key, name) == sum(db.part, wp));
        assert(name_count(names, key, name) > 0 && like_match(name, pattern));
        assert(names_match(names, key, pattern));
    }
}
pub open spec fn aggregate_quantity(db: Db, parameters: Params, part: int, supplier: int) -> int {
    agg_lineitem_group_sum(db, parameters, part, supplier, |tuple: AggLineitemTuple| tuple.lineitem.l_quantity)
}
pub open spec fn window_count(db: Db, parameters: Params, part: int, supplier: int) -> int {
    sum(db.lineitem, |row: LineitemRow| if row.l_partkey == part && row.l_suppkey == supplier && parameters.p1 <= row.l_shipdate < parameters.p4 { 1 } else { 0 })
}
pub open spec fn window_quantity(db: Db, parameters: Params, part: int, supplier: int) -> int {
    sum(db.lineitem, |row: LineitemRow| if row.l_partkey == part && row.l_suppkey == supplier && parameters.p1 <= row.l_shipdate < parameters.p4 { row.l_quantity } else { 0 })
}
pub proof fn lemma_aggregate_values(db: Db, parameters: Params, part: int, supplier: int)
    ensures agg_lineitem_group_count(db, parameters, part, supplier) == window_count(db, parameters, part, supplier),
        aggregate_quantity(db, parameters, part, supplier) == window_quantity(db, parameters, part, supplier),
{
    let extend = |row: LineitemRow| agg_lineitem_tuples_1(db, parameters, row);
    let count = |tuple: AggLineitemTuple| if tuple.lineitem.l_partkey == part && tuple.lineitem.l_suppkey == supplier { 1int } else { 0int };
    let measure = |tuple: AggLineitemTuple| tuple.lineitem.l_quantity;
    let quantity = |tuple: AggLineitemTuple| if tuple.lineitem.l_partkey == part && tuple.lineitem.l_suppkey == supplier { measure(tuple) } else { 0int };
    lemma_sum_flat_map(db.lineitem, extend, count);
    lemma_sum_flat_map(db.lineitem, extend, quantity);
    assert forall|row: LineitemRow| #[trigger] sum(extend(row), count) ==
        if row.l_partkey == part && row.l_suppkey == supplier && parameters.p1 <= row.l_shipdate < parameters.p4 { 1int } else { 0int } by {
        if agg_lineitem_where_clause(db, parameters, row) { lemma_sum_singleton(AggLineitemTuple { lineitem: row }, count); }
    }
    assert forall|row: LineitemRow| #[trigger] sum(extend(row), quantity) ==
        if row.l_partkey == part && row.l_suppkey == supplier && parameters.p1 <= row.l_shipdate < parameters.p4 { row.l_quantity } else { 0int } by {
        if agg_lineitem_where_clause(db, parameters, row) { lemma_sum_singleton(AggLineitemTuple { lineitem: row }, quantity); }
    }
    lemma_sum_ext_all(db.lineitem, |row: LineitemRow| sum(extend(row), count),
        |row: LineitemRow| if row.l_partkey == part && row.l_suppkey == supplier && parameters.p1 <= row.l_shipdate < parameters.p4 { 1int } else { 0int });
    lemma_sum_ext_all(db.lineitem, |row: LineitemRow| sum(extend(row), quantity),
        |row: LineitemRow| if row.l_partkey == part && row.l_suppkey == supplier && parameters.p1 <= row.l_shipdate < parameters.p4 { row.l_quantity } else { 0int });
    lemma_sum_ext_all(agg_lineitem_tuples(db, parameters), quantity,
        |tuple: AggLineitemTuple| if tuple.lineitem.l_partkey == part && tuple.lineitem.l_suppkey == supplier { tuple.lineitem.l_quantity } else { 0int });
}
pub open spec fn profile_weight_sum(pair: Pair, quantity: bool, weight: spec_fn(int) -> int) -> int {
    sum(rng(pair.ship@.len() as int), |index: int| ((if quantity { pair.ship@[index].quantity as int } else { pair.ship@[index].count as int })
        - if index == 0 { 0 } else if quantity { pair.ship@[index - 1].quantity as int } else { pair.ship@[index - 1].count as int }) * weight(pair.ship@[index].date as int))
}
pub open spec fn database_weight_sum(db: Db, part: i64, supplier: i64, quantity: bool, weight: spec_fn(int) -> int) -> int {
    sum(db.lineitem, |row: LineitemRow| (if row.l_partkey == part && row.l_suppkey == supplier { if quantity { row.l_quantity } else { 1int } } else { 0int }) * weight(row.l_shipdate))
}
pub proof fn lemma_profile_weight_as_sum(pair: Pair, quantity: bool, weight: spec_fn(int) -> int, term: spec_fn(int) -> int)
    requires forall|index: int| #[trigger] term(index) == ((if quantity { pair.ship@[index].quantity as int } else { pair.ship@[index].count as int })
        - if index == 0 { 0 } else if quantity { pair.ship@[index - 1].quantity as int } else { pair.ship@[index - 1].count as int }) * weight(pair.ship@[index].date as int),
    ensures profile_weight_sum(pair, quantity, weight) == sum(rng(pair.ship@.len() as int), term),
{
    lemma_sum_ext_all(rng(pair.ship@.len() as int), |index: int| ((if quantity { pair.ship@[index].quantity as int } else { pair.ship@[index].count as int })
        - if index == 0 { 0 } else if quantity { pair.ship@[index - 1].quantity as int } else { pair.ship@[index - 1].count as int }) * weight(pair.ship@[index].date as int), term);
}
pub proof fn lemma_database_weight_as_sum(db: Db, part: i64, supplier: i64, quantity: bool, weight: spec_fn(int) -> int, term: spec_fn(LineitemRow) -> int)
    requires forall|row: LineitemRow| #[trigger] term(row) == (if row.l_partkey == part && row.l_suppkey == supplier { if quantity { row.l_quantity } else { 1int } } else { 0int }) * weight(row.l_shipdate),
    ensures database_weight_sum(db, part, supplier, quantity, weight) == sum(db.lineitem, term),
{
    lemma_sum_ext_all(db.lineitem, |row: LineitemRow| (if row.l_partkey == part && row.l_suppkey == supplier { if quantity { row.l_quantity } else { 1int } } else { 0int }) * weight(row.l_shipdate), term);
}
pub proof fn lemma_before_as_sum(pair: Pair, date: int, quantity: bool, term: spec_fn(int) -> int)
    requires forall|index: int| #[trigger] term(index) == if pair.ship@[index].date < date {
        (if quantity { pair.ship@[index].quantity as int } else { pair.ship@[index].count as int })
            - if index == 0 { 0 } else if quantity { pair.ship@[index - 1].quantity as int } else { pair.ship@[index - 1].count as int }
    } else { 0 },
    ensures (if quantity { stored_before_quantity(pair, date) } else { stored_before_count(pair, date) }) == sum(rng(pair.ship@.len() as int), term),
{
    reveal(stored_before_count);
    reveal(stored_before_quantity);
    if quantity {
        lemma_sum_ext_all(rng(pair.ship@.len() as int), |index: int| if pair.ship@[index].date < date { pair.ship@[index].quantity as int - if index == 0 { 0 } else { pair.ship@[index - 1].quantity as int } } else { 0 }, term);
    } else {
        lemma_sum_ext_all(rng(pair.ship@.len() as int), |index: int| if pair.ship@[index].date < date { pair.ship@[index].count as int - if index == 0 { 0 } else { pair.ship@[index - 1].count as int } } else { 0 }, term);
    }
}
pub proof fn lemma_profile_weight(db: Db, part: i64, supplier: i64, pair: Pair, quantity: bool, weight: spec_fn(int) -> int)
    requires forall|date: int| #![trigger stored_day_count(pair, date)] stored_day_count(pair, date) == daily_count(db, part as int, supplier as int, date)
        && stored_day_quantity(pair, date) == daily_quantity(db, part as int, supplier as int, date),
    ensures profile_weight_sum(pair, quantity, weight) == database_weight_sum(db, part, supplier, quantity, weight),
{
    let positions = rng(pair.ship@.len() as int);
    let project = |index: int| pair.ship@[index].date as int;
    let delta = |index: int| (if quantity { pair.ship@[index].quantity as int } else { pair.ship@[index].count as int })
        - if index == 0 { 0 } else if quantity { pair.ship@[index - 1].quantity as int } else { pair.ship@[index - 1].count as int };
    let date_of = |row: LineitemRow| row.l_shipdate;
    let selected = |row: LineitemRow| if row.l_partkey == part && row.l_suppkey == supplier { if quantity { row.l_quantity } else { 1int } } else { 0int };
    assert forall|date: int| #[trigger] class_total(positions, project, delta, date) == class_total(db.lineitem, date_of, selected, date) by {
        assert(stored_day_count(pair, date) == daily_count(db, part as int, supplier as int, date));
        assert(stored_day_quantity(pair, date) == daily_quantity(db, part as int, supplier as int, date));
        lemma_sum_ext_all(positions, |index: int| if project(index) == date { delta(index) } else { 0 },
            |index: int| if pair.ship@[index].date == date { (if quantity { pair.ship@[index].quantity as int } else { pair.ship@[index].count as int })
                - if index == 0 { 0 } else if quantity { pair.ship@[index - 1].quantity as int } else { pair.ship@[index - 1].count as int } } else { 0 });
        if quantity {
            lemma_sum_ext_all(db.lineitem, |row: LineitemRow| if date_of(row) == date { selected(row) } else { 0 },
                |row: LineitemRow| if row.l_partkey == part && row.l_suppkey == supplier && row.l_shipdate == date { row.l_quantity } else { 0 });
        } else {
            lemma_sum_ext_all(db.lineitem, |row: LineitemRow| if date_of(row) == date { selected(row) } else { 0 },
                |row: LineitemRow| if row.l_partkey == part && row.l_suppkey == supplier && row.l_shipdate == date { 1int } else { 0 });
        }
    }
    lemma_equal_classes(positions, db.lineitem, project, date_of, delta, selected, weight);
    lemma_sum_ext_all(positions, |index: int| delta(index) * weight(project(index)),
        |index: int| ((if quantity { pair.ship@[index].quantity as int } else { pair.ship@[index].count as int })
            - if index == 0 { 0 } else if quantity { pair.ship@[index - 1].quantity as int } else { pair.ship@[index - 1].count as int }) * weight(pair.ship@[index].date as int));
    lemma_sum_ext_all(db.lineitem, |row: LineitemRow| selected(row) * weight(date_of(row)),
        |row: LineitemRow| (if row.l_partkey == part && row.l_suppkey == supplier { if quantity { row.l_quantity } else { 1int } } else { 0int }) * weight(row.l_shipdate));
}
pub proof fn lemma_pair_window(db: Db, parameters: Params, part: i64, supplier: i64, pair: Pair)
    requires parameters.p1 < parameters.p4,
        forall|date: int| #![trigger stored_day_count(pair, date)] stored_day_count(pair, date) == daily_count(db, part as int, supplier as int, date)
            && stored_day_quantity(pair, date) == daily_quantity(db, part as int, supplier as int, date),
    ensures stored_before_count(pair, parameters.p4) - stored_before_count(pair, parameters.p1) == agg_lineitem_group_count(db, parameters, part as int, supplier as int),
        stored_before_quantity(pair, parameters.p4) - stored_before_quantity(pair, parameters.p1) == aggregate_quantity(db, parameters, part as int, supplier as int),
{
    reveal(stored_before_count);
    reveal(stored_before_quantity);
    let keep = |date: int| if parameters.p1 <= date < parameters.p4 { 1int } else { 0int };
    lemma_profile_weight(db, part, supplier, pair, false, keep);
    lemma_profile_weight(db, part, supplier, pair, true, keep);
    let positions = rng(pair.ship@.len() as int);
    let start_count = |index: int| if pair.ship@[index].date < parameters.p1 { pair.ship@[index].count as int - if index == 0 { 0 } else { pair.ship@[index - 1].count as int } } else { 0 };
    let end_count = |index: int| if pair.ship@[index].date < parameters.p4 { pair.ship@[index].count as int - if index == 0 { 0 } else { pair.ship@[index - 1].count as int } } else { 0 };
    let start_quantity = |index: int| if pair.ship@[index].date < parameters.p1 { pair.ship@[index].quantity as int - if index == 0 { 0 } else { pair.ship@[index - 1].quantity as int } } else { 0 };
    let end_quantity = |index: int| if pair.ship@[index].date < parameters.p4 { pair.ship@[index].quantity as int - if index == 0 { 0 } else { pair.ship@[index - 1].quantity as int } } else { 0 };
    let count = |index: int| end_count(index) - start_count(index);
    let quantity = |index: int| end_quantity(index) - start_quantity(index);
    lemma_before_as_sum(pair, parameters.p1, false, start_count);
    lemma_before_as_sum(pair, parameters.p4, false, end_count);
    lemma_before_as_sum(pair, parameters.p1, true, start_quantity);
    lemma_before_as_sum(pair, parameters.p4, true, end_quantity);
    lemma_profile_weight_as_sum(pair, false, keep, count);
    lemma_profile_weight_as_sum(pair, true, keep, quantity);
    lemma_database_weight_as_sum(db, part, supplier, false, keep,
        |row: LineitemRow| if row.l_partkey == part && row.l_suppkey == supplier && parameters.p1 <= row.l_shipdate < parameters.p4 { 1int } else { 0int });
    lemma_database_weight_as_sum(db, part, supplier, true, keep,
        |row: LineitemRow| if row.l_partkey == part && row.l_suppkey == supplier && parameters.p1 <= row.l_shipdate < parameters.p4 { row.l_quantity } else { 0int });
    lemma_sum_sub(positions, end_count, start_count, count);
    lemma_sum_sub(positions, end_quantity, start_quantity, quantity);
    lemma_sum_ext_all(positions, count, |index: int| ((if false { pair.ship@[index].quantity as int } else { pair.ship@[index].count as int })
        - if index == 0 { 0 } else if false { pair.ship@[index - 1].quantity as int } else { pair.ship@[index - 1].count as int }) * keep(pair.ship@[index].date as int));
    lemma_sum_ext_all(positions, quantity, |index: int| ((if true { pair.ship@[index].quantity as int } else { pair.ship@[index].count as int })
        - if index == 0 { 0 } else if true { pair.ship@[index - 1].quantity as int } else { pair.ship@[index - 1].count as int }) * keep(pair.ship@[index].date as int));
    lemma_sum_ext_all(db.lineitem,
        |row: LineitemRow| (if row.l_partkey == part && row.l_suppkey == supplier { if false { row.l_quantity } else { 1int } } else { 0int }) * keep(row.l_shipdate),
        |row: LineitemRow| if row.l_partkey == part && row.l_suppkey == supplier && parameters.p1 <= row.l_shipdate < parameters.p4 { 1int } else { 0int });
    lemma_sum_ext_all(db.lineitem,
        |row: LineitemRow| (if row.l_partkey == part && row.l_suppkey == supplier { if true { row.l_quantity } else { 1int } } else { 0int }) * keep(row.l_shipdate),
        |row: LineitemRow| if row.l_partkey == part && row.l_suppkey == supplier && parameters.p1 <= row.l_shipdate < parameters.p4 { row.l_quantity } else { 0int });
    lemma_aggregate_values(db, parameters, part as int, supplier as int);
}
pub proof fn lemma_aggregate_support(db: Db, parameters: Params, part: int, supplier: int)
    ensures (exists|index: int| #![trigger rel_agg_lineitem(db, parameters)[index]] 0 <= index < rel_agg_lineitem(db, parameters).len()
        && rel_agg_lineitem(db, parameters)[index].agg_partkey == part && rel_agg_lineitem(db, parameters)[index].agg_suppkey == supplier)
            == (agg_lineitem_group_count(db, parameters, part, supplier) > 0),
        forall|index: int| #![trigger rel_agg_lineitem(db, parameters)[index]] 0 <= index < rel_agg_lineitem(db, parameters).len()
            && rel_agg_lineitem(db, parameters)[index].agg_partkey == part && rel_agg_lineitem(db, parameters)[index].agg_suppkey == supplier ==>
                rel_agg_lineitem(db, parameters)[index].agg_quantity == 5 * aggregate_quantity(db, parameters, part, supplier),
{
    let tuples = agg_lineitem_tuples(db, parameters);
    let project = |tuple: AggLineitemTuple| (tuple.lineitem.l_partkey, tuple.lineitem.l_suppkey);
    let keep = |tuple: AggLineitemTuple| agg_lineitem_having(db, parameters, tuple.lineitem.l_partkey, tuple.lineitem.l_suppkey);
    lemma_map_where_all(tuples, keep, project);
    let keys = tuples.map_values(project);
    lemma_dedup(keys);
    let groups = dedup(keys);
    assert(rel_agg_lineitem(db, parameters) == groups.map_values(|key: (int, int)| agg_lineitem_row_of(db, parameters, key.0, key.1)));
    let weight = |tuple: AggLineitemTuple| if tuple.lineitem.l_partkey == part && tuple.lineitem.l_suppkey == supplier { 1int } else { 0int };
    if agg_lineitem_group_count(db, parameters, part, supplier) > 0 {
        let index = lemma_sum_nonzero_witness(tuples, weight);
        assert(keys[index] == (part, supplier));
        assert(groups.contains((part, supplier)));
        let group = choose|group: int| #![trigger groups[group]] 0 <= group < groups.len() && groups[group] == (part, supplier);
        assert(rel_agg_lineitem(db, parameters)[group] == agg_lineitem_row_of(db, parameters, part, supplier));
    }
    assert forall|index: int| #![trigger rel_agg_lineitem(db, parameters)[index]] 0 <= index < rel_agg_lineitem(db, parameters).len()
        && rel_agg_lineitem(db, parameters)[index].agg_partkey == part && rel_agg_lineitem(db, parameters)[index].agg_suppkey == supplier implies
            agg_lineitem_group_count(db, parameters, part, supplier) > 0 && rel_agg_lineitem(db, parameters)[index].agg_quantity == 5 * aggregate_quantity(db, parameters, part, supplier) by {
        assert(groups[index] == (part, supplier));
        assert(groups.contains((part, supplier)));
        assert(keys.contains((part, supplier)));
        let source = choose|source: int| #![trigger keys[source]] 0 <= source < keys.len() && keys[source] == (part, supplier);
        assert(weight(tuples[source]) == 1);
        lemma_sum_pos(tuples, weight, source);
    }
}
pub open spec fn stock_witness(db: Db, parameters: Params, supplier: int, index: int) -> bool {
    0 <= index < db.partsupp.len() && db.partsupp[index].ps_suppkey == supplier
        && part_matches(db, parameters.p2, db.partsupp[index].ps_partkey)
        && agg_lineitem_group_count(db, parameters, db.partsupp[index].ps_partkey, supplier) > 0
        && db.partsupp[index].ps_availqty * 1000 > 5 * aggregate_quantity(db, parameters, db.partsupp[index].ps_partkey, supplier)
}
pub proof fn lemma_in1(db: Db, parameters: Params, supplier: int)
    ensures in_1(db, parameters, supplier) == (exists|index: int| #[trigger] stock_witness(db, parameters, supplier, index)),
{
    let extend = |row: PartsuppRow| in_1_tuples_1(db, parameters, row);
    if in_1(db, parameters, supplier) {
        let index = choose|index: int| #![trigger in_1_rows(db, parameters)[index]] 0 <= index < in_1_rows(db, parameters).len() && in_1_rows(db, parameters)[index].ps_suppkey == supplier;
        let tuple = in_1_tuples(db, parameters)[index];
        assert(in_1_tuples(db, parameters).contains(tuple));
        lemma_flat_map_contains(db.partsupp, extend, tuple);
        let source = choose|source: int| #![trigger extend(db.partsupp[source]).contains(tuple)] 0 <= source < db.partsupp.len() && extend(db.partsupp[source]).contains(tuple);
        let stock = db.partsupp[source];
        let inner = |aggregate: AggLineitemRow| in_1_tuples_2(db, parameters, stock, aggregate);
        lemma_flat_map_contains(rel_agg_lineitem(db, parameters), inner, tuple);
        let aggregate = choose|aggregate: int| #![trigger inner(rel_agg_lineitem(db, parameters)[aggregate]).contains(tuple)] 0 <= aggregate < rel_agg_lineitem(db, parameters).len() && inner(rel_agg_lineitem(db, parameters)[aggregate]).contains(tuple);
        assert(tuple.partsupp == stock);
        assert(tuple.agg_lineitem == rel_agg_lineitem(db, parameters)[aggregate]);
        lemma_in2(db, parameters, stock.ps_partkey);
        lemma_aggregate_support(db, parameters, stock.ps_partkey, supplier);
        assert(stock_witness(db, parameters, supplier, source));
    }
    if exists|index: int| #[trigger] stock_witness(db, parameters, supplier, index) {
        let source = choose|source: int| #[trigger] stock_witness(db, parameters, supplier, source);
        let stock = db.partsupp[source];
        lemma_in2(db, parameters, stock.ps_partkey);
        lemma_aggregate_support(db, parameters, stock.ps_partkey, supplier);
        let aggregate = choose|aggregate: int| #![trigger rel_agg_lineitem(db, parameters)[aggregate]] 0 <= aggregate < rel_agg_lineitem(db, parameters).len()
            && rel_agg_lineitem(db, parameters)[aggregate].agg_partkey == stock.ps_partkey && rel_agg_lineitem(db, parameters)[aggregate].agg_suppkey == supplier;
        let tuple = In1Tuple { partsupp: stock, agg_lineitem: rel_agg_lineitem(db, parameters)[aggregate] };
        let inner = |row: AggLineitemRow| in_1_tuples_2(db, parameters, stock, row);
        assert(in_1_where_clause(db, parameters, stock, rel_agg_lineitem(db, parameters)[aggregate]));
        assert(inner(rel_agg_lineitem(db, parameters)[aggregate])[0] == tuple);
        assert(inner(rel_agg_lineitem(db, parameters)[aggregate]).contains(tuple));
        lemma_flat_map_contains(rel_agg_lineitem(db, parameters), inner, tuple);
        assert(extend(db.partsupp[source]).contains(tuple));
        lemma_flat_map_contains(db.partsupp, extend, tuple);
        let index = choose|index: int| #![trigger in_1_tuples(db, parameters)[index]] 0 <= index < in_1_tuples(db, parameters).len() && in_1_tuples(db, parameters)[index] == tuple;
        assert(in_1_rows(db, parameters)[index].ps_suppkey == supplier);
    }
}
#[verifier::rlimit(100)]
pub proof fn lemma_cached_supplier(db: Db, parameters: Params, parts: Table<Part>, directory: Table<Vec<i64>>, suppliers: Suppliers, nations: Nations, names: PartNames, supplier: i64)
    requires meaning(db, parts, directory, suppliers, nations, names), domain_valid(db), parameters.p1 < parameters.p4,
    ensures cached_supplier_passes(parts, directory, names, supplier, parameters.p1, parameters.p4, parameters.p2) == in_1(db, parameters, supplier as int),
{
    lemma_in1(db, parameters, supplier as int);
    if cached_supplier_passes(parts, directory, names, supplier, parameters.p1, parameters.p4, parameters.p2) {
        let part = choose|part: i64| #![trigger cached_part_selected(parts, names, part, supplier, parameters.p1, parameters.p4, parameters.p2)]
            cached_part_selected(parts, names, part, supplier, parameters.p1, parameters.p4, parameters.p2);
        lemma_names_match(db, names, parameters.p2, part);
        let pair = pair_of(parts, part, supplier).unwrap();
        assert(pair_of(parts, part, supplier) == Some(pair));
        assert(stored_stock(pair, 0) == stock_count(db, part as int, supplier as int, 0));
        assert forall|date: int| #![trigger stored_day_count(pair, date)] stored_day_count(pair, date) == daily_count(db, part as int, supplier as int, date)
            && stored_day_quantity(pair, date) == daily_quantity(db, part as int, supplier as int, date) by {
            assert(pair_of(parts, part, supplier) == Some(pair));
        }
        lemma_pair_window(db, parameters, part, supplier, pair);
        reveal(pair_valid);
        let last = pair.stocks@.len() as int - 1;
        let value = pair.stocks@[last].value as int;
        let stock_weight = |cell: StockCell| if cell.value == value { cell.count as int } else { 0int };
        lemma_sum_pos(pair.stocks@, stock_weight, last);
        assert(stored_stock(pair, value) > 0);
        assert(stock_count(db, part as int, supplier as int, value) > 0);
        let weight = |row: PartsuppRow| if row.ps_partkey == part && row.ps_suppkey == supplier && row.ps_availqty == value { 1int } else { 0int };
        let index = lemma_sum_nonzero_witness(db.partsupp, weight);
        assert(stock_witness(db, parameters, supplier as int, index));
    }
    if in_1(db, parameters, supplier as int) {
        let index = choose|index: int| #[trigger] stock_witness(db, parameters, supplier as int, index);
        let row = db.partsupp[index];
        lemma_part_matches_eligible(db, parameters.p2, row.ps_partkey);
        assert(machine_key(row.ps_partkey));
        let part = row.ps_partkey as i64;
        assert(part as int == row.ps_partkey);
        lemma_names_match(db, names, parameters.p2, part);
        let value = row.ps_availqty;
        let weight = |row: PartsuppRow| if row.ps_partkey == part && row.ps_suppkey == supplier && row.ps_availqty == value { 1int } else { 0int };
        lemma_sum_pos(db.partsupp, weight, index);
        assert(stock_count(db, part as int, supplier as int, value) > 0);
        if let Some(pair) = pair_of(parts, part, supplier) {
            assert(stored_stock(pair, value) > 0);
            let stock_weight = |cell: StockCell| if cell.value == value { cell.count as int } else { 0int };
            let selected = lemma_sum_nonzero_witness(pair.stocks@, stock_weight);
            assert(pair.stocks@[selected].value == value);
            crate::model::lemma_pair_order(&pair);
            let last = pair.stocks@.len() as int - 1;
            if selected < last { assert(pair.stocks@[selected].value < pair.stocks@[last].value); }
            assert(value <= pair.stocks@[last].value);
            lemma_pair_window(db, parameters, part, supplier, pair);
            assert(cached_pair_passes(pair, parameters.p1, parameters.p4));
            assert(cached_part_passes(parts, part, supplier, parameters.p1, parameters.p4));
            assert(cached_part_selected(parts, names, part, supplier, parameters.p1, parameters.p4, parameters.p2));
        } else {
            assert(stock_count(db, part as int, supplier as int, value) == 0);
            assert(false);
        }
    }
}
}
