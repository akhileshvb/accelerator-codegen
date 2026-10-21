use vstd::prelude::*;
use std::sync::Arc;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::index::*;
use crate::aggregates::*;
use verus_prooflib::wide::Wide;
use crate::bounds::*;
use crate::maintenance::*;
use crate::build_proofs::*;
use verus_common::sums::*;
use crate::maintenance_proofs::*;
use crate::neighborhood_proofs::*;
use crate::aggregation_proofs::*;

verus! {

pub struct State {
    pub(crate) waiting: Counts,
    pub(crate) suppliers: Slices,
    pub(crate) nations: Slices,
    pub(crate) buckets: Slices,
    pub(crate) names: NamePool,
    database: Ghost<Db>,
}

impl State {
    pub closed spec fn db(self) -> Db { self.database@ }

    #[verifier::type_invariant]
    spec fn wf(self) -> bool {
        &&& waiting_for(self.waiting, self.db())
        &&& suppliers_for(self.suppliers, self.db())
        &&& nations_for(self.nations, self.db())
        &&& buckets_for(self.buckets, self.db())
        &&& names_ok(self.names, self.db())
    }
}

pub closed spec fn rescan_order_lines_wanted(state: State, lineitem_delta: Seq<LineitemDelta>, nation_delta: Seq<NationDelta>,
    orders_delta: Seq<OrdersDelta>, supplier_delta: Seq<SupplierDelta>) -> Seq<i64>
{
    wanted_keys(lineitem_delta, orders_delta)
}

pub closed spec fn rescan_order_finished_wanted(state: State, lineitem_delta: Seq<LineitemDelta>, nation_delta: Seq<NationDelta>,
    orders_delta: Seq<OrdersDelta>, supplier_delta: Seq<SupplierDelta>) -> Seq<i64>
{
    wanted_keys(lineitem_delta, orders_delta)
}

#[verifier::rlimit(20)]
pub fn build(
    waiting_lines: &[WaitingLine],
    finished_orders: &[FinishedOrder],
    suppliers: Vec<SupplierScan>,
    nations: Vec<NationScan>,
    Ghost(db): Ghost<Db>,
) -> (result: Option<State>)
    requires
        scan_waiting_lines_ok(db, waiting_lines@),
        scan_finished_orders_ok(db, finished_orders@),
        scan_suppliers_ok(db, suppliers@),
        scan_nations_ok(db, nations@),
    ensures result matches Some(state) ==> state.db() == db,
        result.is_some(),
{
    hide(Tree::at);
    hide(Tree::entries);
    hide(Tree::wf);
    let mut finished = Counts::new();
    let mut position = 0usize;
    while position < finished_orders.len()
        invariant
            position <= finished_orders.len(), positive(finished),
            forall|key: KeyView| #![trigger count(finished, key)] count(finished, key) == sum(finished_orders@.take(position as int), finished_weight(key)),
        decreases finished_orders.len() - position,
    {
        add_nonneg(&mut finished, Key::number(finished_orders[position].o_orderkey), &Wide::from_i128(1));
        proof {
            assert forall|key: KeyView| #![trigger count(finished, key)] count(finished, key) == sum(finished_orders@.take(position as int + 1), finished_weight(key)) by {
                lemma_sum_take(finished_orders@, position as int, finished_weight(key));
            }
        }
        position += 1;
    }
    proof { assert(finished_orders@.take(position as int) =~= finished_orders@); }
    let mut waiting = Counts::new();
    position = 0;
    while position < waiting_lines.len()
        invariant
            position <= waiting_lines.len(), positive(waiting),
            forall|key: KeyView| #![trigger count(finished, key)] count(finished, key) == sum(finished_orders@, finished_weight(key)),
            forall|key: KeyView| #![trigger count(waiting, key)] count(waiting, key) == sum(waiting_lines@.take(position as int), waiting_weight(finished, key)),
        decreases waiting_lines.len() - position,
    {
        let row = &waiting_lines[position];
        let multiplicity = amount(&finished, &Key::number(row.l_orderkey));
        proof { lemma_waiting_row_finished(db, waiting_lines@, finished_orders@, position as int); }
        add_nonneg(&mut waiting, Key::number(row.l_suppkey), &multiplicity);
        proof {
            assert forall|key: KeyView| #![trigger count(waiting, key)] count(waiting, key) == sum(waiting_lines@.take(position as int + 1), waiting_weight(finished, key)) by {
                lemma_sum_take(waiting_lines@, position as int, waiting_weight(finished, key));
            }
        }
        position += 1;
    }
    proof {
        assert(waiting_lines@.take(position as int) =~= waiting_lines@);
        assert forall|key: KeyView| #![trigger count(waiting, key)] count(waiting, key) == if key.1 == Seq::<u8>::empty() { crate::aggregates::waiting(db, key.0) } else { 0 } by {
            lemma_waiting_scan(db, waiting_lines@, finished_orders@, &finished, key);
        }
    }
    let mut supplier_bag = Slices::new();
    let mut buckets = Slices::new();
    let mut names = NamePool::new();
    let ghost all_suppliers = suppliers@;
    let ghost all_nations = nations@;
    let mut supplier_rows = suppliers;
    while supplier_rows.len() > 0
        invariant
            names_valid(names), positive_slices(supplier_bag), positive_slices(buckets), positive(waiting),
            forall|key: KeyView| #![trigger count(waiting, key)] count(waiting, key) == if key.1 == Seq::<u8>::empty() { crate::aggregates::waiting(db, key.0) } else { 0 },
            forall|outer: KeyView, inner: KeyView| #![trigger nested_count(supplier_bag, outer, inner)]
                nested_count(supplier_bag, outer, inner) + sum(supplier_rows@, supplier_weight(outer, inner)) == sum(all_suppliers, supplier_weight(outer, inner)),
            forall|outer: KeyView, inner: KeyView| #![trigger nested_count(buckets, outer, inner)]
                nested_count(buckets, outer, inner) + sum(supplier_rows@, bucket_weight(waiting, outer, inner)) == sum(all_suppliers, bucket_weight(waiting, outer, inner)),
            forall|key: KeyView| #![trigger name_count(names, key)] name_count(names, key) + sum(supplier_rows@, supplier_name_weight(key)) == sum(all_suppliers, supplier_name_weight(key)),
        decreases supplier_rows.len(),
    {
        let ghost remaining = supplier_rows@;
        let row = some_of(supplier_rows.pop());
        let ghost item = supplier_scan_view(row);
        let supplier_key = Key::number(row.s_suppkey);
        let one = Wide::from_i128(1);
        proof { lemma_name_count_nonnegative(names, (0int, row.s_name@)); }
        let interned = some_of(intern(&mut names, Key::text(row.s_name), &one));
        let name = interned.with_number(row.s_nationkey);
        add_nonneg_slice(&mut supplier_bag, supplier_key.share(), name.share(), &one);
        let weight = amount(&waiting, &supplier_key);
        proof { lemma_positive_at(&waiting, supplier_key.view()); }
        if weight.is_pos() {
            add_nonneg_slice(&mut buckets, Key::number(row.s_nationkey), name.with_number(0), &weight);
        }
        proof {
            assert(supplier_rows@ == remaining.drop_last());
            assert forall|outer: KeyView, inner: KeyView| #![trigger nested_count(supplier_bag, outer, inner)]
                nested_count(supplier_bag, outer, inner) + sum(supplier_rows@, supplier_weight(outer, inner)) == sum(all_suppliers, supplier_weight(outer, inner)) by {
                assert(supplier_weight(outer, inner)(remaining.last()) == if outer == (item.s_suppkey, Seq::<u8>::empty()) && inner == (item.s_nationkey, item.s_name) { 1int } else { 0int });
            }
            assert forall|outer: KeyView, inner: KeyView| #![trigger nested_count(buckets, outer, inner)]
                nested_count(buckets, outer, inner) + sum(supplier_rows@, bucket_weight(waiting, outer, inner)) == sum(all_suppliers, bucket_weight(waiting, outer, inner)) by {
                lemma_positive_at(&waiting, (item.s_suppkey, Seq::<u8>::empty()));
                assert(bucket_weight(waiting, outer, inner)(remaining.last()) == if outer == (item.s_nationkey, Seq::<u8>::empty()) && inner == (0int, item.s_name) { weight@ } else { 0 });
            }
            assert forall|key: KeyView| #![trigger name_count(names, key)] name_count(names, key) + sum(supplier_rows@, supplier_name_weight(key)) == sum(all_suppliers, supplier_name_weight(key)) by {
                assert(supplier_name_weight(key)(remaining.last()) == if key == (0int, item.s_name) { 1int } else { 0int });
            }
        }
    }
    let mut nation_bag = Slices::new();
    let mut nation_rows = nations;
    while nation_rows.len() > 0
        invariant
            names_valid(names), positive_slices(nation_bag), positive_slices(supplier_bag), positive_slices(buckets), positive(waiting),
            forall|key: KeyView| #![trigger count(waiting, key)] count(waiting, key) == if key.1 == Seq::<u8>::empty() { crate::aggregates::waiting(db, key.0) } else { 0 },
            forall|outer: KeyView, inner: KeyView| #![trigger nested_count(supplier_bag, outer, inner)]
                nested_count(supplier_bag, outer, inner) == sum(all_suppliers, supplier_weight(outer, inner)),
            forall|outer: KeyView, inner: KeyView| #![trigger nested_count(buckets, outer, inner)]
                nested_count(buckets, outer, inner) == sum(all_suppliers, bucket_weight(waiting, outer, inner)),
            forall|outer: KeyView, inner: KeyView| #![trigger nested_count(nation_bag, outer, inner)]
                nested_count(nation_bag, outer, inner) + sum(nation_rows@, nation_weight(outer, inner)) == sum(all_nations, nation_weight(outer, inner)),
            forall|key: KeyView| #![trigger name_count(names, key)] name_count(names, key) + sum(nation_rows@, nation_name_weight(key)) == sum(all_suppliers, supplier_name_weight(key)) + sum(all_nations, nation_name_weight(key)),
        decreases nation_rows.len(),
    {
        let ghost remaining = nation_rows@;
        let row = some_of(nation_rows.pop());
        let ghost item = nation_scan_view(row);
        let one = Wide::from_i128(1);
        proof { lemma_name_count_nonnegative(names, (0int, row.n_name@)); }
        let name = some_of(intern(&mut names, Key::text(row.n_name), &one));
        add_nonneg_slice(&mut nation_bag, name, Key::number(row.n_nationkey), &one);
        proof {
            assert(nation_rows@ == remaining.drop_last());
            assert forall|outer: KeyView, inner: KeyView| #![trigger nested_count(nation_bag, outer, inner)]
                nested_count(nation_bag, outer, inner) + sum(nation_rows@, nation_weight(outer, inner)) == sum(all_nations, nation_weight(outer, inner)) by {
                assert(nation_weight(outer, inner)(remaining.last()) == if outer == (0int, item.n_name) && inner == (item.n_nationkey, Seq::<u8>::empty()) { 1int } else { 0int });
            }
            assert forall|key: KeyView| #![trigger name_count(names, key)] name_count(names, key) + sum(nation_rows@, nation_name_weight(key)) == sum(all_suppliers, supplier_name_weight(key)) + sum(all_nations, nation_name_weight(key)) by {
                assert(nation_name_weight(key)(remaining.last()) == if key == (0int, item.n_name) { 1int } else { 0int });
            }
        }
    }
    proof {
        assert forall|outer: KeyView, inner: KeyView| #![trigger nested_count(supplier_bag, outer, inner)]
            nested_count(supplier_bag, outer, inner) == if outer.1 == Seq::<u8>::empty() { supplier_multiplicity(db, outer.0, inner.0, inner.1) } else { 0 } by {
            lemma_supplier_scan(db, all_suppliers, &waiting, outer, inner);
        }
        assert forall|outer: KeyView, inner: KeyView| #![trigger nested_count(buckets, outer, inner)]
            nested_count(buckets, outer, inner) == if outer.1 == Seq::<u8>::empty() && inner.0 == 0 { bucket(db, outer.0, inner.1) } else { 0 } by {
            lemma_supplier_scan(db, all_suppliers, &waiting, outer, inner);
        }
        assert forall|outer: KeyView, inner: KeyView| #![trigger nested_count(nation_bag, outer, inner)]
            nested_count(nation_bag, outer, inner) == if outer.0 == 0 && inner.1 == Seq::<u8>::empty() { nation_multiplicity(db, inner.0, outer.1) } else { 0 } by {
            lemma_nation_scan(db, all_nations, outer, inner);
        }
        assert forall|key: KeyView| #![trigger name_count(names, key)] name_count(names, key) == if key.0 == 0 { name_references(db, key.1) } else { 0 } by {
            lemma_name_scan(db, all_suppliers, all_nations, key);
        }
    }
    Some(State { waiting, suppliers: supplier_bag, nations: nation_bag, buckets, names, database: Ghost(db) })
}

pub fn execute(state: &State, parameter: &[u8]) -> (result: Option<Vec<(Vec<u8>, i64)>>)
    ensures result matches Some(out) ==> query_result(state.db(), params(parameter@), out_view(out@)),
        result.is_none() ==> !answer_fits(state.db(), params(parameter@)),
{
    hide(Tree::at);
    hide(Tree::entries);
    hide(Tree::wf);
    proof { use_type_invariant(state); }
    crate::output::answer(&state.nations, &state.buckets, parameter, Ghost(state.db()))
}

pub fn rescan_order_lines_keys(
    state: &State,
    lineitem_delta: &[LineitemDelta],
    nation_delta: &Vec<NationDelta>,
    orders_delta: &[OrdersDelta],
    supplier_delta: &Vec<SupplierDelta>,
) -> (result: Vec<i64>)
    ensures result@ == rescan_order_lines_wanted(*state, lineitem_delta@, nation_delta@, orders_delta@, supplier_delta@),
{
    touched_rows(lineitem_delta, orders_delta)
}

pub fn rescan_order_finished_keys(
    state: &State,
    lineitem_delta: &[LineitemDelta],
    nation_delta: &Vec<NationDelta>,
    orders_delta: &[OrdersDelta],
    supplier_delta: &Vec<SupplierDelta>,
) -> (result: Vec<i64>)
    ensures result@ == rescan_order_finished_wanted(*state, lineitem_delta@, nation_delta@, orders_delta@, supplier_delta@),
{
    touched_rows(lineitem_delta, orders_delta)
}

fn touched_rows(lineitem_delta: &[LineitemDelta], orders_delta: &[OrdersDelta]) -> (result: Vec<i64>)
    ensures result@ == wanted_keys(lineitem_delta@, orders_delta@),
{
    let keys = touched(lineitem_delta, orders_delta);
    proof { keys.model(); lemma_touched_text(&keys, lineitem_delta@, orders_delta@); }
    let rows = key_rows(&keys);
    proof { lemma_wanted_is(rows@, &keys, lineitem_delta@, orders_delta@); }
    rows
}

proof fn lemma_touched_text(keys: &Counts, lineitem: Seq<LineitemDelta>, orders: Seq<OrdersDelta>)
    requires positive(*keys),
        forall|key: KeyView| #![trigger count(*keys, key)] count(*keys, key) == if line_touch(lineitem, key) || order_touch(orders, key) { 1int } else { 0int },
    ensures forall|position: int| 0 <= position < keys.entries().len() ==> (#[trigger] keys.entries()[position]).0.1 == Seq::<u8>::empty(),
{
    keys.model();
    assert forall|position: int| 0 <= position < keys.entries().len() implies (#[trigger] keys.entries()[position]).0.1 == Seq::<u8>::empty() by {
        let key = keys.entries()[position].0;
        assert(keys.at(key).is_some());
        assert(count(*keys, key) > 0);
        assert(line_touch(lineitem, key) || order_touch(orders, key));
    }
}

proof fn lemma_wanted_is(rows: Seq<i64>, keys: &Counts, lineitem: Seq<LineitemDelta>, orders: Seq<OrdersDelta>)
    requires positive(*keys), strictly_increasing(rows),
        forall|key: KeyView| #![trigger count(*keys, key)] count(*keys, key) == if line_touch(lineitem, key) || order_touch(orders, key) { 1int } else { 0int },
        forall|key: i64| rows.contains(key) <==> keys.at((key as int, Seq::<u8>::empty())).is_some(),
    ensures wanted_keys(lineitem, orders) == rows,
{
    assert forall|key: i64| rows.contains(key) <==> touched_key(lineitem, orders, key) by {
        lemma_positive_at(keys, (key as int, Seq::<u8>::empty()));
        assert(keys.at((key as int, Seq::<u8>::empty())).is_some() <==> count(*keys, (key as int, Seq::<u8>::empty())) != 0);
    }
    let chosen = wanted_keys(lineitem, orders);
    assert(strictly_increasing(chosen) && forall|key: i64| chosen.contains(key) <==> touched_key(lineitem, orders, key));
    lemma_unique_keys(chosen, rows);
}

proof fn lemma_covered(keys: &Counts, rows: Seq<i64>, lineitem: Seq<LineitemDelta>, orders: Seq<OrdersDelta>)
    requires positive(*keys),
        forall|key: KeyView| #![trigger count(*keys, key)] count(*keys, key) == if line_touch(lineitem, key) || order_touch(orders, key) { 1int } else { 0int },
        forall|key: i64| rows.contains(key) <==> touched_key(lineitem, orders, key),
    ensures forall|key: KeyView| #![trigger keys.at(key)] keys.at(key).is_some() ==> key_list_contains(rows, key),
{
    assert forall|key: KeyView| #![trigger keys.at(key)] keys.at(key).is_some() implies key_list_contains(rows, key) by {
        lemma_positive_at(keys, key);
        assert(line_touch(lineitem, key) || order_touch(orders, key));
        if line_touch(lineitem, key) {
            let position = choose|position: int| #![trigger lineitem[position]] 0 <= position < lineitem.len() && key == (lineitem[position].row.l_orderkey as int, Seq::<u8>::empty());
            let number = lineitem[position].row.l_orderkey;
            assert(touched_key(lineitem, orders, number));
            let source = choose|source: int| 0 <= source < rows.len() && rows[source] == number;
            assert(key == (rows[source] as int, Seq::<u8>::empty()));
        } else {
            let position = choose|position: int| #![trigger orders[position]] 0 <= position < orders.len() && orders[position].is_finished != 0 && key == (orders[position].o_orderkey as int, Seq::<u8>::empty());
            let number = orders[position].o_orderkey;
            assert(touched_key(lineitem, orders, number));
            let source = choose|source: int| 0 <= source < rows.len() && rows[source] == number;
            assert(key == (rows[source] as int, Seq::<u8>::empty()));
        }
    }
}

fn update(
    state: &State,
    lineitem_delta: &[LineitemDelta],
    nation_delta: &Vec<NationDelta>,
    orders_delta: &[OrdersDelta],
    supplier_delta: &Vec<SupplierDelta>,
    order_lines_keys: &[i64],
    order_lines_rows: &[OrderLine],
    order_finished_keys: &[i64],
    order_finished_rows: &[OrderFinished],
    Ghost(orders_full): Ghost<Seq<(OrdersRow, int)>>,
    Ghost(db2): Ghost<Db>,
) -> (next: State)
    requires
        orders_delta_ok(orders_delta@, orders_full),
        capture_ok(state.db(), db2, lineitem_delta_seq_view(lineitem_delta@), nation_delta_seq_view(nation_delta@), orders_full, supplier_delta_seq_view(supplier_delta@)),
        order_lines_keys@ == wanted_keys(lineitem_delta@, orders_delta@),
        order_finished_keys@ == wanted_keys(lineitem_delta@, orders_delta@),
        rescan_order_lines_ok(db2, order_lines_keys@, order_lines_rows@),
        rescan_order_finished_ok(db2, order_finished_keys@, order_finished_rows@),
    ensures next.db() == db2,
{
    hide(Tree::at);
    hide(Tree::entries);
    hide(Tree::wf);
    proof { use_type_invariant(state); }
    let keys = touched(lineitem_delta, orders_delta);
    proof {
        keys.model();
        lemma_touched_text(&keys, lineitem_delta@, orders_delta@);
    }
    let own = key_rows(&keys);
    proof {
        lemma_wanted_is(own@, &keys, lineitem_delta@, orders_delta@);
        lemma_covered(&keys, order_lines_keys@, lineitem_delta@, orders_delta@);
        lemma_covered(&keys, order_finished_keys@, lineitem_delta@, orders_delta@);
        assert forall|position: int| 0 <= position < lineitem_delta.len() implies keys.at(((#[trigger] lineitem_delta@[position]).row.l_orderkey as int, Seq::<u8>::empty())).is_some() by {
            let key = (lineitem_delta@[position].row.l_orderkey as int, Seq::<u8>::empty());
            assert(line_touch(lineitem_delta@, key));
            assert(count(keys, key) == 1);
        }
        assert forall|position: int| 0 <= position < orders_delta.len() && (#[trigger] orders_delta@[position]).is_finished != 0 implies keys.at((orders_delta@[position].o_orderkey as int, Seq::<u8>::empty())).is_some() by {
            let key = (orders_delta@[position].o_orderkey as int, Seq::<u8>::empty());
            assert(order_touch(orders_delta@, key));
            assert(count(keys, key) == 1);
        }
    }
    let waiting_delta = waiting_changes(&keys, lineitem_delta, orders_delta, order_lines_rows, order_finished_rows,
        Ghost(order_lines_keys@), Ghost(order_finished_keys@), Ghost(orders_full), Ghost(state.db()), Ghost(db2));
    let name_delta = name_changes(supplier_delta, nation_delta);
    let mut names = state.names.share();
    proof {
        assert forall|key: KeyView| #![trigger name_count(state.names, key)] name_count(state.names, key) + count(name_delta, key) >= 0 by {
            lemma_name_delta_step(state.db(), db2, supplier_delta@, nation_delta@, key);
            lemma_name_references_nonnegative(db2, key.1);
            assert(count(name_delta, key) == sum(supplier_delta@, supplier_name_delta_weight(key)) + sum(nation_delta@, nation_name_delta_weight(key)));
        }
    }
    update_names(&mut names, &name_delta);
    proof {
        assert forall|key: KeyView| #![trigger name_count(names, key)] name_count(names, key) == if key.0 == 0 { name_references(db2, key.1) } else { 0 } by {
            lemma_name_delta_step(state.db(), db2, supplier_delta@, nation_delta@, key);
            assert(count(name_delta, key) == sum(supplier_delta@, supplier_name_delta_weight(key)) + sum(nation_delta@, nation_name_delta_weight(key)));
            assert(name_count(state.names, key) == if key.0 == 0 { name_references(state.db(), key.1) } else { 0 });
            assert(name_count(names, key) == name_count(state.names, key) + count(name_delta, key));
        }
        assert(names_ok(names, db2));
    }
    let ghost supplier_updates = supplier_delta@;
    let ghost nation_updates = nation_delta@;
    let supplier_delta = supplier_changes(supplier_delta, &names, &state.names);
    let nation_delta = nation_changes(nation_delta, &names, &state.names);
    proof {
        assert forall|outer: KeyView, inner: KeyView| #![trigger nested_count(supplier_delta, outer, inner)] nested_count(supplier_delta, outer, inner) == if outer.1 == Seq::<u8>::empty() {
            supplier_multiplicity(db2, outer.0, inner.0, inner.1) - supplier_multiplicity(state.db(), outer.0, inner.0, inner.1)
        } else { 0 } by {
            lemma_supplier_delta_step(state.db(), db2, supplier_updates, outer, inner);
        }
        assert forall|outer: KeyView, inner: KeyView| #![trigger nested_count(nation_delta, outer, inner)] nested_count(nation_delta, outer, inner) == if outer.0 == 0 && inner.1 == Seq::<u8>::empty() {
            nation_multiplicity(db2, inner.0, outer.1) - nation_multiplicity(state.db(), inner.0, outer.1)
        } else { 0 } by {
            lemma_nation_delta_step(state.db(), db2, nation_updates, outer, inner);
        }
    }
    let mut waiting = state.waiting.share();
    let mut suppliers = state.suppliers.share();
    let mut nations = state.nations.share();
    let mut buckets = state.buckets.share();
    let mut bucket_delta = Slices::new();
    let changes = waiting_delta.rows();
    proof { assert(waiting_change_for(waiting_delta, state.db(), db2)); }
    let mut position = 0usize;
    while position < changes.len()
        invariant
            position <= changes.len(), state.wf(), positive(waiting), names_ok(names, db2),
            waiting_delta.wf(), waiting_change_for(waiting_delta, state.db(), db2),
            changes@.map_values(|row: (Key, Arc<i128>)| (row.0.view(), *row.1)) == waiting_delta.entries(),
            forall|query: KeyView| #![trigger count(waiting, query)] count(waiting, query) == count(state.waiting, query)
                + sum(changes@.take(position as int), count_row_weight(query)),
            forall|outer: KeyView, inner: KeyView| #![trigger nested_count(bucket_delta, outer, inner)] nested_count(bucket_delta, outer, inner)
                == sum(changes@.take(position as int), waiting_bucket_weight(state.suppliers, outer, inner)),
            supplier_delta_for(supplier_delta, state.db(), db2), nation_delta_for(nation_delta, state.db(), db2),
            suppliers.tree() == state.suppliers.tree(), nations.tree() == state.nations.tree(), buckets.tree() == state.buckets.tree(),
        decreases changes.len() - position,
    {
        let (key, change) = &changes[position];
        let variants = slice(&suppliers, key);
        add_weighted_variants(&mut bucket_delta, &variants, &**change);
        proof {
            lemma_prior_rows(changes@, waiting_delta, position as int);
            lemma_waiting_nonnegative(db2, key.view().0);
            assert(count(waiting, key.view()) + (**change)@ == waiting(db2, key.view().0));
        }
        let adjusted = adjust(&mut waiting, key.share(), &**change, true);
        proof { assert(adjusted.is_some()); }
        proof {
            assert forall|query: KeyView| #![trigger count(waiting, query)] count(waiting, query) == count(state.waiting, query)
                + sum(changes@.take(position as int + 1), count_row_weight(query)) by {
                lemma_sum_take(changes@, position as int, count_row_weight(query));
            }
            assert forall|outer: KeyView, inner: KeyView| #![trigger nested_count(bucket_delta, outer, inner)] nested_count(bucket_delta, outer, inner)
                == sum(changes@.take(position as int + 1), waiting_bucket_weight(state.suppliers, outer, inner)) by {
                lemma_sum_take(changes@, position as int, waiting_bucket_weight(state.suppliers, outer, inner));
            }
        }
        position += 1;
    }
    proof {
        assert(changes@.take(position as int) =~= changes@);
        assert forall|query: KeyView| #![trigger count(waiting, query)] count(waiting, query) == count(state.waiting, query) + count(waiting_delta, query) by {
            lemma_count_sum(&waiting_delta, query);
            lemma_sum_map(changes@, |row: (Key, Arc<i128>)| (row.0.view(), *row.1), entry_weight(query), count_row_weight(query));
        }
        assert(waiting_for(waiting, db2));
    }
    let supplier_changes = supplier_delta.rows();
    position = 0;
    while position < supplier_changes.len()
        invariant
            position <= supplier_changes.len(), state.wf(), positive(waiting), names_ok(names, db2),
            waiting_for(waiting, db2), waiting_delta.wf(), supplier_delta.wf(),
            waiting_change_for(waiting_delta, state.db(), db2),
            forall|query: KeyView| #![trigger count(waiting, query)] count(waiting, query) == count(state.waiting, query) + count(waiting_delta, query),
            changes@.map_values(|row: (Key, Arc<i128>)| (row.0.view(), *row.1)) == waiting_delta.entries(),
            supplier_changes@.map_values(|row: (Key, Arc<Counts>)| (row.0.view(), *row.1)) == supplier_delta.entries(),
            forall|outer: KeyView, inner: KeyView| #![trigger nested_count(bucket_delta, outer, inner)] nested_count(bucket_delta, outer, inner)
                == sum(changes@, waiting_bucket_weight(state.suppliers, outer, inner))
                    + sum(supplier_changes@.take(position as int), supplier_bucket_weight(waiting, outer, inner)),
            supplier_delta_for(supplier_delta, state.db(), db2), nation_delta_for(nation_delta, state.db(), db2),
            suppliers.tree() == state.suppliers.tree(), nations.tree() == state.nations.tree(), buckets.tree() == state.buckets.tree(),
        decreases supplier_changes.len() - position,
    {
        let (key, variants) = &supplier_changes[position];
        let weight = amount(&waiting, key);
        add_weighted_variants(&mut bucket_delta, variants, &weight);
        proof {
            assert forall|outer: KeyView, inner: KeyView| #![trigger nested_count(bucket_delta, outer, inner)] nested_count(bucket_delta, outer, inner)
                == sum(changes@, waiting_bucket_weight(state.suppliers, outer, inner))
                    + sum(supplier_changes@.take(position as int + 1), supplier_bucket_weight(waiting, outer, inner)) by {
                lemma_sum_take(supplier_changes@, position as int, supplier_bucket_weight(waiting, outer, inner));
            }
        }
        position += 1;
    }
    let ghost previous_suppliers = suppliers;
    proof { assert(supplier_changes@.take(position as int) =~= supplier_changes@); }
    let ghost previous_nations = nations;
    proof { assert(positive_slices(suppliers)); assert(positive_slices(nations)); }
    proof {
        assert forall|outer: KeyView, inner: KeyView| #![trigger nested_count(suppliers, outer, inner)] nested_count(suppliers, outer, inner) + nested_count(supplier_delta, outer, inner) >= 0 by {
            lemma_supplier_multiplicity_nonnegative(db2, outer.0, inner.0, inner.1);
        }
        assert forall|outer: KeyView, inner: KeyView| #![trigger nested_count(nations, outer, inner)] nested_count(nations, outer, inner) + nested_count(nation_delta, outer, inner) >= 0 by {
            lemma_nation_multiplicity_nonnegative(db2, inner.0, outer.1);
        }
    }
    apply_slice_changes(&mut suppliers, &supplier_delta);
    apply_slice_changes(&mut nations, &nation_delta);
    proof {
        assert forall|outer: KeyView, inner: KeyView| #![trigger nested_count(suppliers, outer, inner)] nested_count(suppliers, outer, inner) == if outer.1 == Seq::<u8>::empty() { supplier_multiplicity(db2, outer.0, inner.0, inner.1) } else { 0 } by {
            assert(nested_count(suppliers, outer, inner) == nested_count(previous_suppliers, outer, inner) + nested_count(supplier_delta, outer, inner));
            assert(nested_count(previous_suppliers, outer, inner) == nested_count(state.suppliers, outer, inner));
            assert(nested_count(state.suppliers, outer, inner) == if outer.1 == Seq::<u8>::empty() { supplier_multiplicity(state.db(), outer.0, inner.0, inner.1) } else { 0 });
        }
        assert forall|outer: KeyView, inner: KeyView| #![trigger nested_count(nations, outer, inner)] nested_count(nations, outer, inner) == if outer.0 == 0 && inner.1 == Seq::<u8>::empty() { nation_multiplicity(db2, inner.0, outer.1) } else { 0 } by {
            assert(nested_count(nations, outer, inner) == nested_count(previous_nations, outer, inner) + nested_count(nation_delta, outer, inner));
            assert(nested_count(previous_nations, outer, inner) == nested_count(state.nations, outer, inner));
            assert(nested_count(state.nations, outer, inner) == if outer.0 == 0 && inner.1 == Seq::<u8>::empty() { nation_multiplicity(state.db(), inner.0, outer.1) } else { 0 });
        }
        assert(suppliers_for(suppliers, db2));
        assert(nations_for(nations, db2));
    }
    let ghost previous_buckets = buckets;
    proof {
        assert(positive_slices(buckets));
        assert forall|outer: KeyView, inner: KeyView| #![trigger nested_count(bucket_delta, outer, inner)]
            nested_count(previous_buckets, outer, inner) + nested_count(bucket_delta, outer, inner)
                == if outer.1 == Seq::<u8>::empty() && inner.0 == 0 { bucket(db2, outer.0, inner.1) } else { 0 } by {
            lemma_bucket_delta_rows(&waiting_delta, &supplier_delta, state.suppliers, waiting, changes@, supplier_changes@, outer, inner);
            assert(nested_count(previous_buckets, outer, inner) == nested_count(state.buckets, outer, inner));
            if outer.1 == Seq::<u8>::empty() && inner.0 == 0 {
                assert forall|key: KeyView, variant: KeyView| #![trigger nested_count(suppliers, key, variant)]
                    nested_count(suppliers, key, variant) == nested_count(state.suppliers, key, variant) + nested_count(supplier_delta, key, variant) by {
                    assert(nested_count(previous_suppliers, key, variant) == nested_count(state.suppliers, key, variant));
                }
                lemma_bucket_update(&state.waiting, &state.suppliers, &waiting, &suppliers, &waiting_delta, &supplier_delta, outer.0, inner.1);
                lemma_bucket_factor(state.db(), &state.waiting, &state.suppliers, outer.0, inner.1);
                lemma_bucket_factor(db2, &waiting, &suppliers, outer.0, inner.1);
            }
        }
    }
    proof {
        assert forall|outer: KeyView, inner: KeyView| #![trigger nested_count(previous_buckets, outer, inner)]
            nested_count(previous_buckets, outer, inner) + nested_count(bucket_delta, outer, inner) >= 0 by {
            lemma_bucket_nonnegative(db2, outer.0, inner.1);
        }
    }
    apply_slice_changes(&mut buckets, &bucket_delta);
    proof { assert(buckets_for(buckets, db2)); }
    State { waiting, suppliers, nations, buckets, names, database: Ghost(db2) }
}

pub fn apply(
    state: &State,
    lineitem_delta: &[LineitemDelta],
    nation_delta: &Vec<NationDelta>,
    orders_delta: &[OrdersDelta],
    supplier_delta: &Vec<SupplierDelta>,
    order_lines_keys: &[i64],
    order_lines_rows: &[OrderLine],
    order_finished_keys: &[i64],
    order_finished_rows: &[OrderFinished],
    Ghost(orders_full): Ghost<Seq<(OrdersRow, int)>>,
    Ghost(db2): Ghost<Db>,
) -> (result: Result<State, u32>)
    requires
        orders_delta_ok(orders_delta@, orders_full),
        capture_ok(state.db(), db2, lineitem_delta_seq_view(lineitem_delta@), nation_delta_seq_view(nation_delta@), orders_full, supplier_delta_seq_view(supplier_delta@)),
        order_lines_keys@ == rescan_order_lines_wanted(*state, lineitem_delta@, nation_delta@, orders_delta@, supplier_delta@),
        rescan_order_lines_ok(db2, order_lines_keys@, order_lines_rows@),
        order_finished_keys@ == rescan_order_finished_wanted(*state, lineitem_delta@, nation_delta@, orders_delta@, supplier_delta@),
        rescan_order_finished_ok(db2, order_finished_keys@, order_finished_rows@),
    ensures result matches Ok(next) ==> next.db() == db2,
        result is Ok,
{
    Ok(update(state, lineitem_delta, nation_delta, orders_delta, supplier_delta,
        order_lines_keys, order_lines_rows, order_finished_keys, order_finished_rows, Ghost(orders_full), Ghost(db2)))
}

}
