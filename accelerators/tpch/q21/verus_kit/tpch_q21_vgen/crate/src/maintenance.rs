use vstd::prelude::*;
use std::sync::Arc;
use crate::index::*;
use crate::aggregates::*;
use crate::gen_obligations::*;
use verus_common::sums::*;
use verus_prooflib::wide::Wide;
use crate::gen_spec::*;
use crate::contribution_proofs::*;
use crate::neighborhood_proofs::*;
use crate::summaries::*;
use verus_common::bytes::*;

verus! {

pub struct LineCounts {
    pub total: Wide,
    pub late: Wide,
}

pub struct OrderSummary {
    pub lines: Index<LineCounts>,
    pub finished: Wide,
}

pub open spec fn line_total(summary: OrderSummary, supplier: KeyView) -> int {
    match summary.lines.at(supplier) { Some(counts) => counts.total@, None => 0 }
}

pub open spec fn line_late(summary: OrderSummary, supplier: KeyView) -> int {
    match summary.lines.at(supplier) { Some(counts) => counts.late@, None => 0 }
}

pub open spec fn order_total(index: Index<OrderSummary>, order: KeyView, supplier: KeyView) -> int {
    match index.at(order) { Some(summary) => line_total(summary, supplier), None => 0 }
}

pub open spec fn order_late(index: Index<OrderSummary>, order: KeyView, supplier: KeyView) -> int {
    match index.at(order) { Some(summary) => line_late(summary, supplier), None => 0 }
}

pub open spec fn order_finished(index: Index<OrderSummary>, order: KeyView) -> int {
    match index.at(order) { Some(summary) => summary.finished@, None => 0 }
}

impl OrderSummary {
    pub fn new() -> (result: Self)
        ensures result.lines.wf(), result.finished@ == 0,
            forall|key: KeyView| #![trigger line_total(result, key)] #![trigger line_late(result, key)] line_total(result, key) == 0 && line_late(result, key) == 0,
    {
        OrderSummary { lines: Index::new(), finished: Wide::zero() }
    }

    pub fn share(&self) -> (result: Self)
        ensures result.lines.wf(), result.lines.tree() == self.lines.tree(), result.finished@ == self.finished@,
            forall|key: KeyView| #![trigger line_total(result, key)] #![trigger line_late(result, key)] line_total(result, key) == line_total(*self, key) && line_late(result, key) == line_late(*self, key),
    {
        OrderSummary { lines: self.lines.share(), finished: self.finished.dup() }
    }
}

pub open spec fn line_touch(rows: Seq<LineitemDelta>, key: KeyView) -> bool {
    exists|position: int| #![trigger rows[position]] 0 <= position < rows.len() && key == (rows[position].row.l_orderkey as int, Seq::<u8>::empty())
}

pub open spec fn order_touch(rows: Seq<OrdersDelta>, key: KeyView) -> bool {
    exists|position: int| #![trigger rows[position]] 0 <= position < rows.len() && rows[position].is_finished != 0 && key == (rows[position].o_orderkey as int, Seq::<u8>::empty())
}

pub proof fn lemma_touch_take(lineitem: Seq<LineitemDelta>, orders: Seq<OrdersDelta>, position: int, key: KeyView)
    requires 0 <= position < lineitem.len() || 0 <= position < orders.len(),
    ensures
        0 <= position < lineitem.len() ==> (line_touch(lineitem.take(position + 1), key) == (line_touch(lineitem.take(position), key) || key == (lineitem[position].row.l_orderkey as int, Seq::<u8>::empty()))),
        0 <= position < orders.len() ==> (order_touch(orders.take(position + 1), key) == (order_touch(orders.take(position), key) || orders[position].is_finished != 0 && key == (orders[position].o_orderkey as int, Seq::<u8>::empty()))),
{
    if 0 <= position < lineitem.len() {
        if line_touch(lineitem.take(position + 1), key) {
            let source = choose|source: int| #![trigger lineitem.take(position + 1)[source]] 0 <= source < position + 1 && key == (lineitem.take(position + 1)[source].row.l_orderkey as int, Seq::<u8>::empty());
            if source < position { assert(line_touch(lineitem.take(position), key)); }
        }
        if line_touch(lineitem.take(position), key) {
            let source = choose|source: int| #![trigger lineitem.take(position)[source]] 0 <= source < position && key == (lineitem.take(position)[source].row.l_orderkey as int, Seq::<u8>::empty());
            assert(lineitem.take(position + 1)[source] == lineitem.take(position)[source]);
        }
        if key == (lineitem[position].row.l_orderkey as int, Seq::<u8>::empty()) {
            assert(lineitem.take(position + 1)[position] == lineitem[position]);
            assert(line_touch(lineitem.take(position + 1), key));
        }
    }
    if 0 <= position < orders.len() {
        if order_touch(orders.take(position + 1), key) {
            let source = choose|source: int| #![trigger orders.take(position + 1)[source]] 0 <= source < position + 1 && orders.take(position + 1)[source].is_finished != 0 && key == (orders.take(position + 1)[source].o_orderkey as int, Seq::<u8>::empty());
            if source < position { assert(order_touch(orders.take(position), key)); }
        }
        if order_touch(orders.take(position), key) {
            let source = choose|source: int| #![trigger orders.take(position)[source]] 0 <= source < position && orders.take(position)[source].is_finished != 0 && key == (orders.take(position)[source].o_orderkey as int, Seq::<u8>::empty());
            assert(orders.take(position + 1)[source] == orders.take(position)[source]);
        }
        if orders[position].is_finished != 0 && key == (orders[position].o_orderkey as int, Seq::<u8>::empty()) {
            assert(orders.take(position + 1)[position] == orders[position]);
            assert(order_touch(orders.take(position + 1), key));
        }
    }
}

pub fn touched(lineitem: &[LineitemDelta], orders: &[OrdersDelta]) -> (result: Counts)
    ensures positive(result),
        forall|key: KeyView| #![trigger count(result, key)] count(result, key) == if line_touch(lineitem@, key) || order_touch(orders@, key) { 1int } else { 0int },
{
    hide(Tree::at);
    hide(Tree::entries);
    hide(Tree::wf);
    let mut keys = Counts::new();
    let mut position = 0usize;
    while position < lineitem.len()
        invariant position <= lineitem.len(), positive(keys),
            forall|key: KeyView| #![trigger count(keys, key)] count(keys, key) == if line_touch(lineitem@.take(position as int), key) { 1int } else { 0int },
        decreases lineitem.len() - position,
    {
        let ghost previous = keys;
        let ghost inserted = (lineitem@[position as int].row.l_orderkey as int, Seq::<u8>::empty());
        keys.put(Key::number(lineitem[position].row.l_orderkey), Arc::new(Wide::from_i128(1)));
        proof {
            keys.model();
            assert forall|key: KeyView| #![trigger count(keys, key)] count(keys, key) == if line_touch(lineitem@.take(position as int + 1), key) { 1int } else { 0int } by {
                lemma_touch_take(lineitem@, Seq::empty(), position as int, key);
                assert(count(keys, key) == if key == inserted { 1int } else { count(previous, key) });
            }
            assert forall|source: int| #![trigger keys.entries()[source]] 0 <= source < keys.entries().len() implies keys.entries()[source].1@ > 0 by {
                assert(count(keys, keys.entries()[source].0) == keys.entries()[source].1@);
                if keys.entries()[source].0 != inserted {
                    lemma_positive_at(&previous, keys.entries()[source].0);
                    assert(previous.at(keys.entries()[source].0).is_some());
                }
            }
        }
        position += 1;
    }
    proof { assert(lineitem@.take(position as int) =~= lineitem@); }
    position = 0;
    while position < orders.len()
        invariant position <= orders.len(), positive(keys),
            forall|key: KeyView| #![trigger count(keys, key)] count(keys, key) == if line_touch(lineitem@, key) || order_touch(orders@.take(position as int), key) { 1int } else { 0int },
        decreases orders.len() - position,
    {
        let ghost previous = keys;
        let ghost inserted = (orders@[position as int].o_orderkey as int, Seq::<u8>::empty());
        if orders[position].is_finished != 0 {
            keys.put(Key::number(orders[position].o_orderkey), Arc::new(Wide::from_i128(1)));
        }
        proof {
            keys.model();
            assert forall|key: KeyView| #![trigger count(keys, key)] count(keys, key) == if line_touch(lineitem@, key) || order_touch(orders@.take(position as int + 1), key) { 1int } else { 0int } by {
                lemma_touch_take(Seq::empty(), orders@, position as int, key);
                assert(count(keys, key) == if orders@[position as int].is_finished != 0 && key == inserted { 1int } else { count(previous, key) });
            }
            assert forall|source: int| #![trigger keys.entries()[source]] 0 <= source < keys.entries().len() implies keys.entries()[source].1@ > 0 by {
                assert(count(keys, keys.entries()[source].0) == keys.entries()[source].1@);
                if keys.entries()[source].0 != inserted || orders@[position as int].is_finished == 0 {
                    lemma_positive_at(&previous, keys.entries()[source].0);
                    assert(previous.at(keys.entries()[source].0).is_some());
                }
            }
        }
        position += 1;
    }
    proof { assert(orders@.take(position as int) =~= orders@); }
    keys
}

pub open spec fn strictly_increasing(keys: Seq<i64>) -> bool {
    forall|first: int, last: int| 0 <= first < last < keys.len() ==> keys[first] < keys[last]
}

pub open spec fn touched_key(lineitem: Seq<LineitemDelta>, orders: Seq<OrdersDelta>, key: i64) -> bool {
    line_touch(lineitem, (key as int, Seq::<u8>::empty())) || order_touch(orders, (key as int, Seq::<u8>::empty()))
}

/// The order keys the deltas touch, in increasing order: what the host rescans.
pub open spec fn wanted_keys(lineitem: Seq<LineitemDelta>, orders: Seq<OrdersDelta>) -> Seq<i64> {
    choose|keys: Seq<i64>| strictly_increasing(keys) && forall|key: i64| keys.contains(key) <==> touched_key(lineitem, orders, key)
}

pub proof fn lemma_unique_keys(first: Seq<i64>, second: Seq<i64>)
    requires strictly_increasing(first), strictly_increasing(second),
        forall|key: i64| first.contains(key) <==> second.contains(key),
    ensures first == second,
    decreases first.len(),
{
    if first.len() == 0 {
        if second.len() > 0 { assert(second.contains(second[0])); assert(first.contains(second[0])); assert(false); }
        assert(second.len() == 0);
    } else {
        let top = first.last();
        assert(first.contains(top));
        assert(second.contains(top));
        assert(second.len() > 0);
        let bottom = second.last();
        assert(second.contains(bottom));
        assert(first.contains(bottom));
        let index = choose|index: int| 0 <= index < first.len() && first[index] == bottom;
        let other = choose|other: int| 0 <= other < second.len() && second[other] == top;
        if index < first.len() - 1 { assert(first[index] < first[first.len() - 1]); }
        if other < second.len() - 1 { assert(second[other] < second[second.len() - 1]); }
        assert(top == bottom);
        let rest_first = first.drop_last();
        let rest_second = second.drop_last();
        assert forall|first_index: int, last_index: int| 0 <= first_index < last_index < rest_first.len() implies rest_first[first_index] < rest_first[last_index] by {}
        assert forall|first_index: int, last_index: int| 0 <= first_index < last_index < rest_second.len() implies rest_second[first_index] < rest_second[last_index] by {}
        assert forall|key: i64| rest_first.contains(key) <==> rest_second.contains(key) by {
            if rest_first.contains(key) {
                let position = choose|position: int| 0 <= position < rest_first.len() && rest_first[position] == key;
                assert(first.contains(key));
                assert(first[position] < top);
                let spot = choose|spot: int| 0 <= spot < second.len() && second[spot] == key;
                if spot == second.len() - 1 { assert(key == bottom); }
                assert(rest_second[spot] == key);
            }
            if rest_second.contains(key) {
                let position = choose|position: int| 0 <= position < rest_second.len() && rest_second[position] == key;
                assert(second.contains(key));
                assert(second[position] < bottom);
                let spot = choose|spot: int| 0 <= spot < first.len() && first[spot] == key;
                if spot == first.len() - 1 { assert(key == top); }
                assert(rest_first[spot] == key);
            }
        }
        lemma_unique_keys(rest_first, rest_second);
        assert(first =~= rest_first.push(top));
        assert(second =~= rest_second.push(bottom));
    }
}

pub fn key_rows(keys: &Counts) -> (result: Vec<i64>)
    requires keys.wf(), forall|position: int| 0 <= position < keys.entries().len() ==> (#[trigger] keys.entries()[position]).0.1 == Seq::<u8>::empty(),
    ensures strictly_increasing(result@),
        forall|key: i64| result@.contains(key) <==> keys.at((key as int, Seq::<u8>::empty())).is_some(),
{
    let rows = keys.rows();
    proof { keys.model(); }
    let mut result = Vec::new();
    let mut position = 0usize;
    while position < rows.len()
        invariant position <= rows.len(), keys.wf(),
            rows@.map_values(|row: (Key, Arc<Wide>)| (row.0.view(), *row.1)) == keys.entries(),
            ordered_rows(keys.entries()),
            forall|index: int| 0 <= index < keys.entries().len() ==> (#[trigger] keys.entries()[index]).0.1 == Seq::<u8>::empty(),
            result@.len() == position,
            forall|index: int| 0 <= index < position ==> (#[trigger] result@[index]) == rows@[index].0.number,
        decreases rows.len() - position,
    {
        result.push(rows[position].0.number);
        position += 1;
    }
    proof {
        assert forall|first: int, last: int| 0 <= first < last < result@.len() implies result@[first] < result@[last] by {
            assert(key_lt(keys.entries()[first].0, keys.entries()[last].0));
            lemma_bytes_lt_irrefl(Seq::<u8>::empty());
            assert(rows@[first].0.view() == keys.entries()[first].0);
            assert(rows@[last].0.view() == keys.entries()[last].0);
        }
        assert forall|key: i64| result@.contains(key) <==> keys.at((key as int, Seq::<u8>::empty())).is_some() by {
            if result@.contains(key) {
                let index = choose|index: int| 0 <= index < result@.len() && result@[index] == key;
                assert(keys.entries()[index].0 == rows@[index].0.view());
                assert(keys.entries()[index].0 == (key as int, Seq::<u8>::empty()));
            }
            if keys.at((key as int, Seq::<u8>::empty())).is_some() {
                let index = choose|index: int| #![auto] 0 <= index < keys.entries().len() && keys.entries()[index].0 == (key as int, Seq::<u8>::empty());
                assert(rows@[index].0.view() == keys.entries()[index].0);
                assert(result@[index] == key);
                assert(result@.contains(key));
            }
        }
    }
    result
}

pub open spec fn key_list_contains(keys: Seq<i64>, key: KeyView) -> bool {
    exists|position: int| #![trigger keys[position]] 0 <= position < keys.len() && key == (keys[position] as int, Seq::<u8>::empty())
}

fn contains_key(keys: &[i64], key: i64) -> (result: bool)
    ensures result ==> key_list_contains(keys@, (key as int, Seq::<u8>::empty())),
{
    let mut position = 0usize;
    while position < keys.len()
        invariant position <= keys.len(),
        decreases keys.len() - position,
    {
        if keys[position] == key {
            proof {
                assert(key_list_contains(keys@, (key as int, Seq::<u8>::empty()))) by {
                    let source = position as int;
                    assert(keys@[source] == key);
                }
            }
            return true;
        }
        position += 1;
    }
    false
}

pub proof fn lemma_key_list_take(keys: Seq<i64>, position: int, key: KeyView)
    requires 0 <= position < keys.len(),
    ensures key_list_contains(keys.take(position + 1), key) == (key_list_contains(keys.take(position), key) || key == (keys[position] as int, Seq::<u8>::empty())),
{
    if key_list_contains(keys.take(position + 1), key) {
        let source = choose|source: int| #![trigger keys.take(position + 1)[source]] 0 <= source < position + 1 && key == (keys.take(position + 1)[source] as int, Seq::<u8>::empty());
        if source < position { assert(key_list_contains(keys.take(position), key)); }
    }
    if key_list_contains(keys.take(position), key) {
        let source = choose|source: int| #![trigger keys.take(position)[source]] 0 <= source < position && key == (keys.take(position)[source] as int, Seq::<u8>::empty());
        assert(keys.take(position + 1)[source] == keys.take(position)[source]);
    }
    if key == (keys[position] as int, Seq::<u8>::empty()) {
        assert(keys.take(position + 1)[position] == keys[position]);
        assert(key_list_contains(keys.take(position + 1), key));
    }
}

fn summary(index: &Index<OrderSummary>, key: &Key) -> (result: OrderSummary)
    ensures result.lines.wf(), result.finished@ == order_finished(*index, key.view()),
        forall|supplier: KeyView| #![trigger line_total(result, supplier)] #![trigger line_late(result, supplier)]
            line_total(result, supplier) == order_total(*index, key.view(), supplier) && line_late(result, supplier) == order_late(*index, key.view(), supplier),
{
    match index.get(key) {
        Some(value) => {
            let result = value.share();
            proof {
                assert forall|supplier: KeyView| #![trigger line_total(result, supplier)] #![trigger line_late(result, supplier)]
                    line_total(result, supplier) == order_total(*index, key.view(), supplier) && line_late(result, supplier) == order_late(*index, key.view(), supplier) by {
                    assert(index.at(key.view()) == Some(*value));
                    assert(result.lines.at(supplier) == value.lines.at(supplier));
                }
            }
            result
        },
        None => {
            let result = OrderSummary::new();
            proof {
                assert forall|supplier: KeyView| #![trigger line_total(result, supplier)] #![trigger line_late(result, supplier)]
                    line_total(result, supplier) == order_total(*index, key.view(), supplier) && line_late(result, supplier) == order_late(*index, key.view(), supplier) by {
                    assert(line_total(result, supplier) == 0);
                    assert(line_late(result, supplier) == 0);
                }
            }
            result
        },
    }
}

pub(crate) fn add_line(index: &mut Index<OrderSummary>, order: i64, supplier: i64, total: &Wide, late: &Wide)
    ensures
        final(index).wf(),
        forall|query_order: KeyView, query_supplier: KeyView| #![trigger order_total(*final(index), query_order, query_supplier)] #![trigger order_late(*final(index), query_order, query_supplier)] {
            let change = query_order == (order as int, Seq::<u8>::empty()) && query_supplier == (supplier as int, Seq::<u8>::empty());
            &&& order_total(*final(index), query_order, query_supplier) == order_total(*old(index), query_order, query_supplier) + if change { total@ } else { 0 }
            &&& order_late(*final(index), query_order, query_supplier) == order_late(*old(index), query_order, query_supplier) + if change { late@ } else { 0 }
        },
        forall|query_order: KeyView| #![trigger order_finished(*final(index), query_order)] order_finished(*final(index), query_order) == order_finished(*old(index), query_order),
{
    hide(Tree::at);
    hide(Tree::entries);
    hide(Tree::wf);
    let ghost previous = *index;
    let order_key = Key::number(order);
    let supplier_key = Key::number(supplier);
    let mut value = summary(index, &order_key);
    let ghost previous_value = value;
    let old = value.lines.get(&supplier_key);
    let (old_total, old_late) = match old { Some(counts) => (counts.total.dup(), counts.late.dup()), None => (Wide::zero(), Wide::zero()) };
    let new_total = old_total.add(total);
    let new_late = old_late.add(late);
    if new_total.is_zero() && new_late.is_zero() { value.lines.remove(&supplier_key); }
    else { value.lines.put(supplier_key, Arc::new(LineCounts { total: new_total, late: new_late })); }
    proof {
        assert forall|query: KeyView| #![trigger line_total(value, query)] #![trigger line_late(value, query)] {
            &&& line_total(value, query) == line_total(previous_value, query) + if query == supplier_key.view() { total@ } else { 0 }
            &&& line_late(value, query) == line_late(previous_value, query) + if query == supplier_key.view() { late@ } else { 0 }
        } by {}
    }
    let ghost updated_value = value;
    index.put(order_key, Arc::new(value));
    proof {
        assert forall|query_order: KeyView, query_supplier: KeyView| #![trigger order_total(*index, query_order, query_supplier)] #![trigger order_late(*index, query_order, query_supplier)] {
            let change = query_order == (order as int, Seq::<u8>::empty()) && query_supplier == (supplier as int, Seq::<u8>::empty());
            &&& order_total(*index, query_order, query_supplier) == order_total(previous, query_order, query_supplier) + if change { total@ } else { 0 }
            &&& order_late(*index, query_order, query_supplier) == order_late(previous, query_order, query_supplier) + if change { late@ } else { 0 }
        } by {
            if query_order == order_key.view() {
                assert(index.at(query_order) == Some(updated_value));
                assert(line_total(updated_value, query_supplier) == line_total(previous_value, query_supplier) + if query_supplier == supplier_key.view() { total@ } else { 0 });
                assert(line_late(updated_value, query_supplier) == line_late(previous_value, query_supplier) + if query_supplier == supplier_key.view() { late@ } else { 0 });
                assert(line_total(previous_value, query_supplier) == order_total(previous, query_order, query_supplier));
                assert(line_late(previous_value, query_supplier) == order_late(previous, query_order, query_supplier));
            }
        }
    }
}

pub(crate) fn add_finished(index: &mut Index<OrderSummary>, order: i64, delta: &Wide)
    ensures
        final(index).wf(),
        forall|query_order: KeyView| #![trigger order_finished(*final(index), query_order)] order_finished(*final(index), query_order) == order_finished(*old(index), query_order)
            + if query_order == (order as int, Seq::<u8>::empty()) { delta@ } else { 0 },
        forall|query_order: KeyView, query_supplier: KeyView| #![trigger order_total(*final(index), query_order, query_supplier)] #![trigger order_late(*final(index), query_order, query_supplier)] {
            &&& order_total(*final(index), query_order, query_supplier) == order_total(*old(index), query_order, query_supplier)
            &&& order_late(*final(index), query_order, query_supplier) == order_late(*old(index), query_order, query_supplier)
        },
{
    let ghost previous = *index;
    let key = Key::number(order);
    let mut value = summary(index, &key);
    let ghost previous_value = value;
    value.finished = value.finished.add(delta);
    index.put(key, Arc::new(value));
    proof {
        assert forall|query_order: KeyView, query_supplier: KeyView| #![trigger order_total(*index, query_order, query_supplier)] #![trigger order_late(*index, query_order, query_supplier)] {
            &&& order_total(*index, query_order, query_supplier) == order_total(previous, query_order, query_supplier)
            &&& order_late(*index, query_order, query_supplier) == order_late(previous, query_order, query_supplier)
        } by {
            if query_order == key.view() {
                assert(line_total(previous_value, query_supplier) == order_total(previous, query_order, query_supplier));
                assert(line_late(previous_value, query_supplier) == order_late(previous, query_order, query_supplier));
                assert(value.lines.tree() == previous_value.lines.tree());
            }
        }
    }
}

#[verifier::rlimit(20)]
fn contribution(summary: &OrderSummary, Ghost(db): Ghost<Db>, Ghost(order): Ghost<int>) -> (result: Option<(i64, Wide)>)
    requires summary_db(*summary, db, order),
    ensures forall|supplier: KeyView| #![trigger contribution_value(result, supplier)] contribution_value(result, supplier) ==
        if supplier.1 == Seq::<u8>::empty() { neighborhood_waiting(db, order, supplier.0) } else { 0 },
{
    hide(Tree::at);
    hide(Tree::entries);
    hide(Tree::wf);
    proof { lemma_line_counts(db, order, 0); }
    let rows = summary.lines.rows();
    let mut found = false;
    let mut multiple = false;
    let mut late_key = 0i64;
    let mut late_count = Wide::zero();
    let ghost mut selected_position = 0int;
    proof { summary.lines.model(); }
    let mut position = 0usize;
    while position < rows.len()
        invariant position <= rows.len(), summary_db(*summary, db, order),
            rows@.map_values(|row: (Key, Arc<LineCounts>)| (row.0.view(), *row.1)) == summary.lines.entries(),
            found ==> 0 <= selected_position < position && rows@[selected_position].1.late@ == late_count@ && late_count@ > 0 && rows@[selected_position].0.view() == (late_key as int, Seq::<u8>::empty()),
            found ==> line_late(*summary, (late_key as int, Seq::<u8>::empty())) == late_count@,
            !found ==> forall|source: int| 0 <= source < position ==> (#[trigger] rows@[source]).1.late@ <= 0,
            !multiple ==> forall|source: int| 0 <= source < position && (#[trigger] rows@[source]).1.late@ > 0 ==> rows@[source].0.number == late_key,
            multiple ==> exists|pair: (int, int)| #![trigger rows@[pair.0], rows@[pair.1]] 0 <= pair.0 < pair.1 < position && rows@[pair.0].1.late@ > 0 && rows@[pair.1].1.late@ > 0 && rows@[pair.0].0.number != rows@[pair.1].0.number,
        decreases rows.len() - position,
    {
        let (key, counts) = &rows[position];
        proof {
            lemma_summary_row(summary, db, order, position as int);
            assert(summary.lines.entries()[position as int] == (key.view(), **counts));
        }
        if counts.late.is_pos() {
            if found {
                proof {
                    summary.lines.model();
                    assert(key.view().1 == Seq::<u8>::empty());
                    assert(key.view() != rows@[selected_position].0.view()) by {
                        assert(summary.lines.entries()[selected_position].0 == rows@[selected_position].0.view());
                        assert(summary.lines.entries()[position as int].0 == key.view());
                        assert(key_lt(summary.lines.entries()[selected_position].0, summary.lines.entries()[position as int].0));
                        assert(key_lt(rows@[selected_position].0.view(), key.view()));
                        lemma_key_order(key.view(), key.view());
                    }
                    assert(key.number != late_key);
                }
                multiple = true;
                proof {
                    assert(exists|pair: (int, int)| #![trigger rows@[pair.0], rows@[pair.1]] 0 <= pair.0 < pair.1 < position + 1 && rows@[pair.0].1.late@ > 0 && rows@[pair.1].1.late@ > 0 && rows@[pair.0].0.number != rows@[pair.1].0.number) by {
                        let pair = (selected_position, position as int);
                        assert(rows@[pair.0].1.late@ > 0 && rows@[pair.1].1.late@ > 0 && rows@[pair.0].0.number != rows@[pair.1].0.number);
                    }
                }
            } else {
                found = true;
                late_key = key.number;
                late_count = counts.late.dup();
                proof {
                    summary.lines.model();
                    selected_position = position as int;
                    assert(key.view() == (late_key as int, Seq::<u8>::empty()));
                    assert(summary.lines.at(key.view()) == Some(**counts));
                    assert(line_late(*summary, key.view()) == counts.late@);
                }
            }
        }
        position += 1;
    }
    if !found || multiple || summary.finished.is_zero() {
        proof {
            summary.lines.model();
            assert forall|supplier: KeyView| #![trigger contribution_value(None, supplier)] contribution_value(None, supplier) == if supplier.1 == Seq::<u8>::empty() { neighborhood_waiting(db, order, supplier.0) } else { 0 } by {
                if supplier.1 == Seq::<u8>::empty() && summary.finished@ != 0 {
                    if !found {
                        if late_lines(db, order, supplier.0) > 0 {
                            assert(line_late(*summary, supplier) == late_lines(db, order, supplier.0));
                            assert(summary.lines.at(supplier).is_some());
                            let source = choose|source: int| #![auto] 0 <= source < summary.lines.entries().len() && summary.lines.entries()[source].0 == supplier;
                            assert(summary.lines.entries()[source].1.late@ == line_late(*summary, supplier));
                            assert(rows@[source].1.late == summary.lines.entries()[source].1.late);
                            assert(rows@[source].1.late@ > 0);
                        }
                        lemma_line_counts(db, order, supplier.0);
                        assert(late_lines(db, order, supplier.0) == 0);
                    } else {
                        lemma_summary_others(summary, db, order, supplier.0);
                        let (first, second) = choose|pair: (int, int)| #![trigger rows@[pair.0], rows@[pair.1]] 0 <= pair.0 < pair.1 < rows.len() && rows@[pair.0].1.late@ > 0 && rows@[pair.1].1.late@ > 0 && rows@[pair.0].0.number != rows@[pair.1].0.number;
                        if rows@[first].0.number as int != supplier.0 {
                            assert(summary.lines.entries()[first].1.late@ > 0);
                        } else { assert(summary.lines.entries()[second].1.late@ > 0); }
                        assert(other_late(db, order, supplier.0) > 0);
                    }
                }
            }
        }
        return None;
    }
    proof {
        lemma_summary_others(summary, db, order, late_key as int);
        assert(other_late(db, order, late_key as int) == 0);
        assert(late_lines(db, order, late_key as int) == late_count@);
    }
    let mut other = false;
    position = 0;
    while position < rows.len() && !other
        invariant position <= rows.len(), summary_db(*summary, db, order),
            rows@.map_values(|row: (Key, Arc<LineCounts>)| (row.0.view(), *row.1)) == summary.lines.entries(),
            0 <= selected_position < rows.len(), rows@[selected_position].1.late@ == late_count@, late_count@ > 0,
            rows@[selected_position].0.view() == (late_key as int, Seq::<u8>::empty()),
            other_late(db, order, late_key as int) == 0, late_lines(db, order, late_key as int) == late_count@,
            other == (exists|source: int| 0 <= source < position && (#[trigger] rows@[source]).1.total@ > 0 && rows@[source].0.number != late_key),
        decreases rows.len() - position,
    {
        let (key, counts) = &rows[position];
        if counts.total.is_pos() && key.number != late_key { other = true; }
        position += 1;
    }
    proof {
        lemma_summary_others(summary, db, order, late_key as int);
        if other {
            let source = choose|source: int| 0 <= source < position && (#[trigger] rows@[source]).1.total@ > 0 && rows@[source].0.number != late_key;
            assert(summary.lines.entries()[source] == (rows@[source].0.view(), *rows@[source].1));
            assert(summary.lines.entries()[source].0.0 != late_key as int && summary.lines.entries()[source].1.total@ > 0);
        } else if other_total(db, order, late_key as int) > 0 {
            assert(position == rows.len());
            let source = choose|source: int| #![trigger summary.lines.entries()[source]] 0 <= source < summary.lines.entries().len() && summary.lines.entries()[source].0.0 != late_key as int && summary.lines.entries()[source].1.total@ > 0;
            assert(rows@[source].0.number != late_key && rows@[source].1.total@ > 0);
        }
        assert(other == (other_total(db, order, late_key as int) > 0));
        assert forall|supplier: int| supplier != late_key as int implies neighborhood_waiting(db, order, supplier) == 0 by {
            lemma_summary_others(summary, db, order, supplier);
            assert(summary.lines.entries()[selected_position].1.late@ > 0);
            assert(summary.lines.entries()[selected_position].0.0 == late_key as int);
            assert(other_late(db, order, supplier) > 0);
        }
    }
    if !other { return None; }
    let weight = summary.finished.mul(&late_count);
    Some((late_key, weight))
}

pub open spec fn order_change_weight(db: Db, db2: Db, supplier: KeyView) -> spec_fn((Key, Arc<Wide>)) -> int {
    |row: (Key, Arc<Wide>)| if supplier.1 == Seq::<u8>::empty() {
        neighborhood_waiting(db2, row.0.view().0, supplier.0) - neighborhood_waiting(db, row.0.view().0, supplier.0)
    } else { 0 }
}

#[verifier::rlimit(20)]
pub fn waiting_changes(
    keys: &Counts,
    lineitem: &[LineitemDelta],
    orders: &[OrdersDelta],
    line_rows: &[OrderLine],
    finished_rows: &[OrderFinished],
    Ghost(line_keys): Ghost<Seq<i64>>,
    Ghost(finished_keys): Ghost<Seq<i64>>,
    Ghost(full): Ghost<Seq<(OrdersRow, int)>>,
    Ghost(db): Ghost<Db>,
    Ghost(db2): Ghost<Db>,
) -> (result: Counts)
    requires positive(*keys), delta_keys_covered(*keys, lineitem@, orders@),
        forall|position: int| 0 <= position < keys.entries().len() ==> (#[trigger] keys.entries()[position]).0.1 == Seq::<u8>::empty(),
        forall|key: KeyView| #![trigger keys.at(key)] keys.at(key).is_some() ==> key_list_contains(line_keys, key) && key_list_contains(finished_keys, key),
        bag_step(db.lineitem, db2.lineitem, lineitem_delta_seq_view(lineitem@)), bag_step(db.orders, db2.orders, full), orders_delta_ok(orders@, full),
        rescan_order_lines_ok(db2, line_keys, line_rows@), rescan_order_finished_ok(db2, finished_keys, finished_rows@),
    ensures forall|supplier: KeyView| #![trigger count(result, supplier)] count(result, supplier) ==
        if supplier.1 == Seq::<u8>::empty() { waiting(db2, supplier.0) - waiting(db, supplier.0) } else { 0 },
{
    hide(Tree::at);
    hide(Tree::entries);
    hide(Tree::wf);
    let after = crate::summaries::summarize(keys, line_rows, finished_rows);
    let before = crate::summaries::reconstruct(&after, lineitem, orders);
    proof {
        assert forall|order: int| #![trigger keys.at((order, Seq::<u8>::empty()))] keys.at((order, Seq::<u8>::empty())).is_some() implies
            summary_for_order(after, db2, order) && summary_for_order(before, db, order) by {
            lemma_scanned_summary(&after, keys, db2, line_rows@, finished_rows@, line_keys, finished_keys, order);
            lemma_reconstructed_summary(&before, &after, db, db2, lineitem@, orders@, full, order);
        }
    }
    let rows = keys.rows();
    let mut changes = Counts::new();
    let mut position = 0usize;
    while position < rows.len()
        invariant position <= rows.len(), positive(*keys),
            rows@.map_values(|row: (Key, Arc<Wide>)| (row.0.view(), *row.1)) == keys.entries(),
            forall|source: int| 0 <= source < keys.entries().len() ==> (#[trigger] keys.entries()[source]).0.1 == Seq::<u8>::empty(),
            summaries_keys_db(before, *keys, db), summaries_keys_db(after, *keys, db2),
            delta_keys_covered(*keys, lineitem@, orders@),
            bag_step(db.lineitem, db2.lineitem, lineitem_delta_seq_view(lineitem@)), bag_step(db.orders, db2.orders, full), orders_delta_ok(orders@, full),
            forall|supplier: KeyView| #![trigger count(changes, supplier)] count(changes, supplier) == sum(rows@.take(position as int), order_change_weight(db, db2, supplier)),
        decreases rows.len() - position,
    {
        let key = &rows[position].0;
        let old_summary = summary(&before, key);
        let new_summary = summary(&after, key);
        proof {
            keys.model();
            assert(keys.entries()[position as int].0 == key.view());
            assert(keys.at(key.view()).is_some());
            assert(key.view().1 == Seq::<u8>::empty());
            assert(summary_db(old_summary, db, key.view().0));
            assert(summary_db(new_summary, db2, key.view().0));
        }
        let old = contribution(&old_summary, Ghost(db), Ghost(key.view().0));
        let new = contribution(&new_summary, Ghost(db2), Ghost(key.view().0));
        let ghost previous = changes;
        if let Some((supplier, weight)) = &old {
            add_to(&mut changes, Key::number(*supplier), &weight.neg());
        }
        if let Some((supplier, weight)) = &new {
            add_to(&mut changes, Key::number(*supplier), weight);
        }
        proof {
            assert forall|supplier: KeyView| #![trigger count(changes, supplier)] count(changes, supplier) == sum(rows@.take(position as int + 1), order_change_weight(db, db2, supplier)) by {
                assert(count(changes, supplier) == count(previous, supplier) + contribution_value(new, supplier) - contribution_value(old, supplier));
                assert(contribution_value(new, supplier) == if supplier.1 == Seq::<u8>::empty() { neighborhood_waiting(db2, key.view().0, supplier.0) } else { 0 });
                assert(contribution_value(old, supplier) == if supplier.1 == Seq::<u8>::empty() { neighborhood_waiting(db, key.view().0, supplier.0) } else { 0 });
                lemma_sum_take(rows@, position as int, order_change_weight(db, db2, supplier));
            }
        }
        position += 1;
    }
    proof {
        assert(rows@.take(position as int) =~= rows@);
        assert forall|supplier: KeyView| #![trigger count(changes, supplier)] count(changes, supplier) == if supplier.1 == Seq::<u8>::empty() { waiting(db2, supplier.0) - waiting(db, supplier.0) } else { 0 } by {
            if supplier.1 == Seq::<u8>::empty() {
                lemma_waiting_delta(db, db2, keys, lineitem@, orders@, full, supplier.0);
                lemma_sum_map(rows@, |row: (Key, Arc<Wide>)| (row.0.view(), *row.1),
                    |row: (KeyView, Wide)| neighborhood_waiting(db2, row.0.0, supplier.0) - neighborhood_waiting(db, row.0.0, supplier.0), order_change_weight(db, db2, supplier));
            } else { lemma_sum_zero(rows@, order_change_weight(db, db2, supplier)); }
        }
    }
    changes
}

pub open spec fn supplier_delta_weight(outer: KeyView, inner: KeyView) -> spec_fn(SupplierDelta) -> int {
    |delta: SupplierDelta| if outer == (delta.row.s_suppkey as int, Seq::<u8>::empty()) && inner == (delta.row.s_nationkey as int, delta.row.s_name@) { delta.sign as int } else { 0 }
}

pub open spec fn nation_delta_weight(outer: KeyView, inner: KeyView) -> spec_fn(NationDelta) -> int {
    |delta: NationDelta| if outer == (0int, delta.row.n_name@) && inner == (delta.row.n_nationkey as int, Seq::<u8>::empty()) { delta.sign as int } else { 0 }
}

pub open spec fn supplier_name_delta_weight(key: KeyView) -> spec_fn(SupplierDelta) -> int {
    |delta: SupplierDelta| if key == (0int, delta.row.s_name@) { delta.sign as int } else { 0 }
}

pub open spec fn nation_name_delta_weight(key: KeyView) -> spec_fn(NationDelta) -> int {
    |delta: NationDelta| if key == (0int, delta.row.n_name@) { delta.sign as int } else { 0 }
}

pub fn supplier_changes(deltas: &Vec<SupplierDelta>, names: &NamePool, previous: &NamePool) -> (changes: Slices)
    requires names_valid(*names), names_valid(*previous),
    ensures changes.wf(), forall|outer: KeyView, inner: KeyView| #![trigger nested_count(changes, outer, inner)]
        nested_count(changes, outer, inner) == verus_common::sums::sum(deltas@, supplier_delta_weight(outer, inner)),
{
    hide(Tree::at);
    hide(Tree::entries);
    hide(Tree::wf);
    let mut changes = Slices::new();
    let mut position = 0usize;
    while position < deltas.len()
        invariant
            position <= deltas.len(), names_valid(*names), names_valid(*previous), changes.wf(),
            forall|outer: KeyView, inner: KeyView| #![trigger nested_count(changes, outer, inner)]
                nested_count(changes, outer, inner) == verus_common::sums::sum(deltas@.take(position as int), supplier_delta_weight(outer, inner)),
        decreases deltas.len() - position,
    {
        let delta = &deltas[position];
        let name = canonical(names, previous, &delta.row.s_name);
        let key = name.with_number(delta.row.s_nationkey);
        add_to_slice(&mut changes, Key::number(delta.row.s_suppkey), key, &Wide::from_i128(delta.sign as i128));
        proof {
            assert forall|outer: KeyView, inner: KeyView| #![trigger nested_count(changes, outer, inner)]
                nested_count(changes, outer, inner) == verus_common::sums::sum(deltas@.take(position as int + 1), supplier_delta_weight(outer, inner)) by {
                verus_common::sums::lemma_sum_take(deltas@, position as int, supplier_delta_weight(outer, inner));
            }
        }
        position += 1;
    }
    proof { assert(deltas@.take(position as int) =~= deltas@); }
    changes
}

pub fn nation_changes(deltas: &Vec<NationDelta>, names: &NamePool, previous: &NamePool) -> (changes: Slices)
    requires names_valid(*names), names_valid(*previous),
    ensures changes.wf(), forall|outer: KeyView, inner: KeyView| #![trigger nested_count(changes, outer, inner)]
        nested_count(changes, outer, inner) == verus_common::sums::sum(deltas@, nation_delta_weight(outer, inner)),
{
    hide(Tree::at);
    hide(Tree::entries);
    hide(Tree::wf);
    let mut changes = Slices::new();
    let mut position = 0usize;
    while position < deltas.len()
        invariant
            position <= deltas.len(), names_valid(*names), names_valid(*previous), changes.wf(),
            forall|outer: KeyView, inner: KeyView| #![trigger nested_count(changes, outer, inner)]
                nested_count(changes, outer, inner) == verus_common::sums::sum(deltas@.take(position as int), nation_delta_weight(outer, inner)),
        decreases deltas.len() - position,
    {
        let delta = &deltas[position];
        let name = canonical(names, previous, &delta.row.n_name);
        add_to_slice(&mut changes, name, Key::number(delta.row.n_nationkey), &Wide::from_i128(delta.sign as i128));
        proof {
            assert forall|outer: KeyView, inner: KeyView| #![trigger nested_count(changes, outer, inner)]
                nested_count(changes, outer, inner) == verus_common::sums::sum(deltas@.take(position as int + 1), nation_delta_weight(outer, inner)) by {
                verus_common::sums::lemma_sum_take(deltas@, position as int, nation_delta_weight(outer, inner));
            }
        }
        position += 1;
    }
    proof { assert(deltas@.take(position as int) =~= deltas@); }
    changes
}

pub fn name_changes(suppliers: &Vec<SupplierDelta>, nations: &Vec<NationDelta>) -> (changes: Counts)
    ensures changes.wf(), forall|key: KeyView| #![trigger count(changes, key)] count(changes, key)
        == verus_common::sums::sum(suppliers@, supplier_name_delta_weight(key)) + verus_common::sums::sum(nations@, nation_name_delta_weight(key)),
{
    hide(Tree::at);
    hide(Tree::entries);
    hide(Tree::wf);
    let mut changes = Counts::new();
    let mut position = 0usize;
    while position < suppliers.len()
        invariant position <= suppliers.len(), changes.wf(),
            forall|key: KeyView| #![trigger count(changes, key)] count(changes, key) == verus_common::sums::sum(suppliers@.take(position as int), supplier_name_delta_weight(key)),
        decreases suppliers.len() - position,
    {
        let delta = &suppliers[position];
        add_to(&mut changes, Key::text(delta.row.s_name.clone()), &Wide::from_i128(delta.sign as i128));
        proof {
            assert forall|key: KeyView| #![trigger count(changes, key)] count(changes, key) == verus_common::sums::sum(suppliers@.take(position as int + 1), supplier_name_delta_weight(key)) by {
                verus_common::sums::lemma_sum_take(suppliers@, position as int, supplier_name_delta_weight(key));
            }
        }
        position += 1;
    }
    proof { assert(suppliers@.take(position as int) =~= suppliers@); }
    position = 0;
    while position < nations.len()
        invariant position <= nations.len(), changes.wf(),
            forall|key: KeyView| #![trigger count(changes, key)] count(changes, key) == verus_common::sums::sum(suppliers@, supplier_name_delta_weight(key))
                + verus_common::sums::sum(nations@.take(position as int), nation_name_delta_weight(key)),
        decreases nations.len() - position,
    {
        let delta = &nations[position];
        add_to(&mut changes, Key::text(delta.row.n_name.clone()), &Wide::from_i128(delta.sign as i128));
        proof {
            assert forall|key: KeyView| #![trigger count(changes, key)] count(changes, key) == verus_common::sums::sum(suppliers@, supplier_name_delta_weight(key))
                + verus_common::sums::sum(nations@.take(position as int + 1), nation_name_delta_weight(key)) by {
                verus_common::sums::lemma_sum_take(nations@, position as int, nation_name_delta_weight(key));
            }
        }
        position += 1;
    }
    proof { assert(nations@.take(position as int) =~= nations@); }
    changes
}

}
